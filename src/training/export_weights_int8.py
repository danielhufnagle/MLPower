#!/usr/bin/env python3
"""
Export FreqMLP int8 (QAT) weights to mlp_weights_int8.h for kernel deployment.

Quantization scheme (qnnpack, per_tensor_affine):
  - Activations: uint8, asymmetric  (scale + zero_point)
  - Weights:     int8,  symmetric   (scale, zero_point=0)
  - Biases:      float32

Forward pass in kernel C (per layer with relu):
  acc[i]  = sum_j( (uint8)q_in[j] * (int8)w[i][j] )   // int32 accumulate
  f[i]    = (float)acc[i] * MUL + adj_bias[i]           // MUL = in_scale * w_scale
  q_out[i]= clamp( round(f[i] / out_scale) + out_zp, out_zp, 255 )  // relu = clamp floor to out_zp

For the last layer (no relu, no requantize): take argmax of f[i] directly.

StandardScaler + QuantStub are combined into per-feature arrays so the kernel
only does:  q_in[i] = clamp( round((x[i] - mean[i]) * inv_combined[i]) + IN_ZP, 0, 255 )

Usage:
    cd src/training
    python3 export_weights_int8.py

Output: mlp_weights_int8.h  (copy next to your kernel module .c file)
"""

import sys
import copy
import numpy as np
import torch
import torch.quantization
import joblib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from model import FreqMLP

TRAINING_DIR = Path(__file__).parent
DATA_DIR     = TRAINING_DIR.parent / 'data_collection'

torch.backends.quantized.engine = 'qnnpack'

INPUT_DIM      = 174
N_CLASSES      = 22
LINEAR_INDICES = [0, 3, 6, 8]        # indices inside model.net Sequential
LAYER_DIMS     = [256, 128, 64, 22]  # output dim of each Linear

DVFS_STEPS = [
    115200,  192000,  268800,  345600,  422400,  499200,
    576000,  652800,  729600,  806400,  883200,  960000,
    1036800, 1113600, 1190400, 1267200, 1344000, 1420800,
    1497600, 1574400, 1651200, 1728000,
]


# ── model reconstruction ──────────────────────────────────────────────────────

def load_int8_model():
    base = FreqMLP(input_dim=INPUT_DIM, num_classes=N_CLASSES)
    qat  = copy.deepcopy(base)
    qat.qconfig = torch.quantization.get_default_qat_qconfig('qnnpack')
    torch.quantization.prepare_qat(qat, inplace=True)
    qat.eval()
    m = torch.quantization.convert(qat)
    m.load_state_dict(
        torch.load(TRAINING_DIR / 'model_int8.pt', map_location='cpu', weights_only=False)
    )
    m.eval()
    return m


# ── parameter extraction ──────────────────────────────────────────────────────

def extract_layers(model, scaler):
    """Return a list of dicts, one per Linear layer."""

    quant_scale = float(model.quant.scale)
    quant_zp    = int(model.quant.zero_point)

    # Per-feature arrays for the combined scaler + QuantStub step.
    # inv_combined[i] = 1.0 / (scaler_std[i] * quant_scale)
    inv_combined = (1.0 / (scaler.scale_ * quant_scale)).astype(np.float32)
    scaler_mean  = scaler.mean_.astype(np.float32)

    layers    = []
    in_scale  = quant_scale
    in_zp     = quant_zp

    for i, net_idx in enumerate(LINEAR_INDICES):
        layer    = model.net[net_idx]
        w_qtensor = layer.weight()                       # quantized tensor
        w_int8    = w_qtensor.int_repr().numpy()         # int8 ndarray  (out, in)
        w_scale   = float(w_qtensor.q_scale())
        b_float   = layer.bias().detach().numpy()        # float32 (out,)
        out_scale = float(layer.scale)
        out_zp    = int(layer.zero_point)
        has_relu  = (i < 3)                              # layers 0,3,6 are followed by ReLU

        # Adjusted bias absorbs the zero_point correction so the kernel never
        # needs to separately compute in_zp * sum(w[i]).
        # Derivation:
        #   y = in_scale * w_scale * (acc - in_zp * w_sum) + bias
        #   y = in_scale * w_scale * acc  +  (bias - in_scale * w_scale * in_zp * w_sum)
        mul      = np.float32(in_scale * w_scale)
        w_sum    = w_int8.sum(axis=1).astype(np.float32)   # (out,)
        adj_bias = (b_float - mul * in_zp * w_sum).astype(np.float32)

        layers.append(dict(
            idx       = i + 1,
            w_int8    = w_int8,
            adj_bias  = adj_bias,
            mul       = mul,
            out_scale = np.float32(out_scale),
            out_zp    = out_zp,
            has_relu  = has_relu,
        ))

        in_scale = out_scale
        in_zp    = out_zp

    return quant_scale, quant_zp, scaler_mean, inv_combined, layers


# ── C array emitters ──────────────────────────────────────────────────────────

def emit_float_1d(name, arr, vals_per_line=8):
    flat  = np.asarray(arr, dtype=np.float32).ravel()
    n     = flat.size
    lines = [f'static const float {name}[{n}] = {{']
    for i in range(0, n, vals_per_line):
        chunk = flat[i : i + vals_per_line]
        lines.append('    ' + ', '.join(f'{v:.8e}f' for v in chunk) + ',')
    lines.append('};')
    return '\n'.join(lines)


def emit_int8_1d(name, arr, vals_per_line=16):
    flat  = np.asarray(arr, dtype=np.int8).ravel()
    n     = flat.size
    lines = [f'static const int8_t {name}[{n}] = {{']
    for i in range(0, n, vals_per_line):
        chunk = flat[i : i + vals_per_line]
        lines.append('    ' + ', '.join(str(int(v)) for v in chunk) + ',')
    lines.append('};')
    return '\n'.join(lines)


# ── header generation ─────────────────────────────────────────────────────────

def build_header(quant_scale, quant_zp, scaler_mean, inv_combined, layers, feature_cols):
    h = []

    h.append('/* Auto-generated by export_weights_int8.py — do not edit. */')
    h.append('/* FreqMLP QAT int8: 174 → 256 → 128 → 64 → 22            */')
    h.append('/* Quantization: uint8 activations, int8 symmetric weights  */')
    h.append('')
    h.append('#ifndef MLP_WEIGHTS_INT8_H')
    h.append('#define MLP_WEIGHTS_INT8_H')
    h.append('')
    h.append('#include <stdint.h>')
    h.append('')

    # Dimension macros
    h.append(f'#define MLP_INPUT_DIM   {INPUT_DIM}')
    h.append(f'#define MLP_L1_OUT      {LAYER_DIMS[0]}')
    h.append(f'#define MLP_L2_OUT      {LAYER_DIMS[1]}')
    h.append(f'#define MLP_L3_OUT      {LAYER_DIMS[2]}')
    h.append(f'#define MLP_N_CLASSES   {N_CLASSES}')
    h.append('')

    # DVFS step table
    h.append('/* DVFS frequency steps (kHz) indexed by model output class */')
    h.append('static const unsigned int mlp_dvfs_steps[MLP_N_CLASSES] = {')
    h.append('    ' + ', '.join(str(s) for s in DVFS_STEPS))
    h.append('};')
    h.append('')

    # Input quantization parameters
    h.append('/* --- Input quantization -------------------------------------------- */')
    h.append('/*')
    h.append(' * To quantize a raw input vector x[174]:')
    h.append(' *   q_in[i] = clamp( round((x[i] - mlp_in_mean[i]) * mlp_in_inv[i]) + MLP_IN_ZP, 0, 255 )')
    h.append(' *')
    h.append(' * mlp_in_mean[i]  = StandardScaler mean')
    h.append(' * mlp_in_inv[i]   = 1.0 / (StandardScaler std[i] * quant_scale)')
    h.append(' */')
    h.append(f'#define MLP_IN_SCALE  {quant_scale:.8e}f')
    h.append(f'#define MLP_IN_ZP     {quant_zp}')
    h.append('')
    h.append(emit_float_1d('mlp_in_mean', scaler_mean))
    h.append('')
    h.append(emit_float_1d('mlp_in_inv', inv_combined))
    h.append('')

    # Feature index reference
    h.append('/*')
    h.append(' * Input layout: [0..86] current snapshot, [87..173] per-step deltas.')
    h.append(' * Feature order within each half:')
    for i, col in enumerate(feature_cols):
        h.append(f' *   [{i:2d} / {i + INPUT_DIM // 2:3d}]  {col}')
    h.append(' */')
    h.append('')

    # Per-layer weights, adjusted biases, and quantization parameters
    for L in layers:
        n   = L['idx']
        rows, cols = L['w_int8'].shape
        h.append(f'/* --- Layer {n} ({cols} → {rows}) {"+ ReLU " if L["has_relu"] else "       "}--- */')
        h.append('/*')
        h.append(f' * Forward pass:')
        h.append(f' *   acc[i]   = sum_j( q_in[j] * mlp_w{n}[i*{cols}+j] )  // int32')
        h.append(f' *   f[i]     = (float)acc[i] * MLP_L{n}_MUL + mlp_b{n}_adj[i]')
        if L['has_relu']:
            h.append(f' *   q_out[i] = clamp( round(f[i] / MLP_L{n}_OUT_SCALE) + MLP_L{n}_OUT_ZP,')
            h.append(f' *                      MLP_L{n}_OUT_ZP, 255 )  // clamp floor = relu')
        else:
            h.append(f' *   argmax over f[i] — no requantize needed for last layer')
        h.append(' */')
        h.append(f'#define MLP_L{n}_MUL        {L["mul"]:.8e}f')
        if L['has_relu']:
            h.append(f'#define MLP_L{n}_OUT_SCALE  {L["out_scale"]:.8e}f')
            h.append(f'#define MLP_L{n}_OUT_ZP     {L["out_zp"]}')
        h.append('')
        h.append(emit_int8_1d(f'mlp_w{n}', L['w_int8']))
        h.append('')
        h.append(emit_float_1d(f'mlp_b{n}_adj', L['adj_bias']))
        h.append('')

    h.append('#endif /* MLP_WEIGHTS_INT8_H */')
    h.append('')
    return '\n'.join(h)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    model  = load_int8_model()
    scaler = joblib.load(TRAINING_DIR / 'scaler.pkl')

    feature_cols = (DATA_DIR / 'feature_cols.txt').read_text().strip().split('\n')
    assert len(feature_cols) == INPUT_DIM // 2

    quant_scale, quant_zp, scaler_mean, inv_combined, layers = extract_layers(model, scaler)

    header = build_header(quant_scale, quant_zp, scaler_mean, inv_combined, layers, feature_cols)

    out_path = TRAINING_DIR / 'mlp_weights_int8.h'
    out_path.write_text(header)

    total_int8   = sum(L['w_int8'].size for L in layers)
    total_float  = sum(L['adj_bias'].size for L in layers)
    total_float += scaler_mean.size + inv_combined.size

    print(f'wrote {out_path}  ({out_path.stat().st_size / 1024:.0f} KB source)')
    print(f'int8 weight params:  {total_int8:,}  ({total_int8 / 1024:.1f} KB binary)')
    print(f'float32 aux params:  {total_float:,}  ({total_float * 4 / 1024:.1f} KB binary)')
    print(f'total binary:        {(total_int8 + total_float * 4) / 1024:.1f} KB')
    print()
    for L in layers:
        rows, cols = L['w_int8'].shape
        relu_str   = '+relu' if L['has_relu'] else '     '
        print(f'  L{L["idx"]} {relu_str}  w={L["w_int8"].shape}  '
              f'mul={L["mul"]:.4e}'
              + (f'  out_scale={L["out_scale"]:.4e}  out_zp={L["out_zp"]}' if L["has_relu"] else ''))


if __name__ == '__main__':
    main()
