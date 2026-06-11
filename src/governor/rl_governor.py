#!/usr/bin/env python3
"""
rl_governor.py — Userspace RL frequency governor for Jetson Orin Nano.

"""

import argparse
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

import joblib
import numpy as np
import torch
import torch.nn as nn
from torch.distributions import Categorical


REPO         = Path(__file__).resolve().parent.parent
TRAINING_DIR = REPO / 'training'

sys.path.insert(0, str(TRAINING_DIR))
from model import FreqMLP


DVFS_STEPS = np.array([
    115200,  192000,  268800,  345600,  422400,  499200,
    576000,  652800,  729600,  806400,  883200,  960000,
    1036800, 1113600, 1190400, 1267200, 1344000, 1420800,
    1497600, 1574400, 1651200, 1728000,
], dtype=np.int64)

N_CLASSES   = 22
FEATURE_DIM = 87
INPUT_DIM   = 174   # current_87 + delta_87

SNAP_FMT  = '<qII42q'
SNAP_SIZE = struct.calcsize(SNAP_FMT)   # 352 bytes

RATIO_CLIP = {
    'stall_ratio':    (0.0, 1.0),
    'fe_stall_ratio': (0.0, 1.0),
    'ipc':            (0.0, 5.0),
    'll_miss_rate':   (0.0, 0.5),
    'br_misrate':     (0.0, 0.5),
    'dtlb_rate':      (0.0, 0.5),
}

# Value network -- estimates future reward of being in a state. Used to determine advantage. (How much better an action is than baseline.) 
class ValueNet(nn.Module):
    def __init__(self, input_dim=INPUT_DIM):
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, 256), nn.ReLU(),
            nn.Linear(256, 128),       nn.ReLU(),
            nn.Linear(128, 64),        nn.ReLU(),
            nn.Linear(64, 1),
        )

    def forward(self, x):
        return self.net(x).squeeze(-1)

# Reads from file set up by kernel module. Decodes bytes into PMU data. 
def read_snapshot():
    fd = os.open('/dev/pmu_dc', os.O_RDONLY)
    try:
        data = os.read(fd, SNAP_SIZE)
    finally:
        os.close(fd)
    if len(data) != SNAP_SIZE:
        raise IOError(f'short read: {len(data)} bytes')
    unpacked = struct.unpack(SNAP_FMT, data)
    ts       = unpacked[0]
    freq_p0  = unpacked[1]
    freq_p4  = unpacked[2]
    deltas   = np.array(unpacked[3:], dtype=np.float64).reshape(6, 7)
    return ts, freq_p0, freq_p4, deltas

# Reads tegrastats (conveniently user space. 
def parse_tegrastats(line):
    d = {}
    m = re.search(r'CPU \[([^\]]+)\]', line)
    if m:
        cores = m.group(1).split(',')
        utils = [int(c.split('%')[0]) for c in cores]
        d['cpu_util_avg_pct'] = sum(utils) / len(utils)
        d['cpu_util_max_pct'] = float(max(utils))  # schedutil uses per-CPU max
    m = re.search(r'RAM (\d+)/\d+MB', line)
    if m:
        d['ram_used_mb'] = float(m.group(1))
    m = re.search(r'EMC_FREQ (\d+)%', line)
    if m:
        d['emc_util_pct'] = float(m.group(1))
    m = re.search(r'cpu@([\d.]+)C', line)
    if m:
        d['cpu_temp_c'] = float(m.group(1))
    m = re.search(r'tj@([\d.]+)C', line)
    if m:
        d['tj_temp_c'] = float(m.group(1))
    m = re.search(r'VDD_CPU_GPU_CV (\d+)mW', line)
    if m:
        d['cpu_gpu_cv_power_mw'] = float(m.group(1))
    m = re.search(r'VDD_IN (\d+)mW', line)
    if m:
        d['vdd_in_power_mw'] = float(m.group(1))
    return d


# Starts daemon that reads Tegrastats every 10ms 
class TegrastatsReader:
    REQUIRED = {'cpu_util_avg_pct', 'emc_util_pct', 'ram_used_mb',
                'cpu_temp_c', 'tj_temp_c', 'cpu_gpu_cv_power_mw', 'vdd_in_power_mw'}

    def __init__(self):
        self._latest = {k: 0.0 for k in self.REQUIRED}
        self._latest['cpu_util_max_pct'] = 0.0
        self._proc   = subprocess.Popen(
            ['tegrastats', '--interval', '10'],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True
        )
        import threading
        self._thread = threading.Thread(target=self._read_loop, daemon=True)
        self._thread.start()

    def _read_loop(self):
        for line in self._proc.stdout:
            d = parse_tegrastats(line)
            if self.REQUIRED.issubset(d):
                self._latest = d

    def get(self):
        return dict(self._latest)

    def stop(self):
        self._proc.kill()
        try:
            self._proc.wait(timeout=2)
        except Exception:
            pass

# Use pmu / pmic data to compute ratios, storing info into 1D vector
def build_features(freq_p0, freq_p4, deltas, pmic):
    """87-element feature vector — order matches feature_cols.txt exactly."""
    feat = np.zeros(87, dtype=np.float32)
    feat[0] = freq_p0
    feat[1] = freq_p4

    # Raw PMU events: [inst, stall_be, stall_fe, ll_miss, br_mis, dtlb] × 6 cores
    for ev in range(6):
        for core in range(6):
            feat[2 + ev * 6 + core] = deltas[core][ev]
    # Cycles (slot 6 in deltas)
    for core in range(6):
        feat[38 + core] = deltas[core][6]

    # Derived ratios per core
    for core in range(6):
        cyc  = max(deltas[core][6], 1.0)
        inst = max(deltas[core][0], 1.0)
        feat[44 + core] = float(np.clip(deltas[core][1] / cyc,  *RATIO_CLIP['stall_ratio']))
        feat[50 + core] = float(np.clip(deltas[core][2] / cyc,  *RATIO_CLIP['fe_stall_ratio']))
        feat[56 + core] = float(np.clip(deltas[core][0] / cyc,  *RATIO_CLIP['ipc']))
        feat[62 + core] = float(np.clip(deltas[core][3] / inst, *RATIO_CLIP['ll_miss_rate']))
        feat[68 + core] = float(np.clip(deltas[core][4] / inst, *RATIO_CLIP['br_misrate']))
        feat[74 + core] = float(np.clip(deltas[core][5] / inst, *RATIO_CLIP['dtlb_rate']))

    # PMIC features
    feat[80] = pmic['cpu_util_avg_pct']
    feat[81] = pmic['emc_util_pct']
    feat[82] = pmic['ram_used_mb']
    feat[83] = pmic['cpu_temp_c']
    feat[84] = pmic['tj_temp_c']
    feat[85] = pmic['cpu_gpu_cv_power_mw']
    feat[86] = pmic['vdd_in_power_mw']

    return feat

def set_frequency(freq_khz):
    # Policy 0 (cores 0-3): the benchmark workload — full ML decision.
    # Policy 4 (cores 4-5): the governor itself — cap at 576 MHz.
    # Limits interference of PyTorch itself
    GOV_FREQ_CAP = 576000
    for policy, khz in ((0, freq_khz), (4, min(freq_khz, GOV_FREQ_CAP))):
        path = f'/sys/devices/system/cpu/cpufreq/policy{policy}/scaling_setspeed'
        try:
            with open(path, 'w') as f:
                f.write(str(khz))
        except OSError as e:
            print(f'[warn] policy{policy}: {e}')

def switch_governor_userspace():
    for cpu in range(6):
        path = f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor'
        try:
            with open(path, 'w') as f:
                f.write('userspace')
        except OSError as e:
            print(f'[warn] cpu{cpu}: {e}')

# Proximal policy optimization. Also uses Generalized Advantage Estimation, which balances accuracy with variance. 
def ppo_update(actor, critic, actor_opt, critic_opt, buffer, ref_actor=None,
               gamma=0.99, lam=0.95, epochs=2,
               clip_eps=0.2, vf_coef=0.1, ent_coef=0.01, kl_coef=0.05):
    actor.train()
    critic.train()

    states      = torch.stack([b[0] for b in buffer])
    actions     = torch.tensor([b[1] for b in buffer], dtype=torch.long)
    rewards     = torch.tensor([b[2] for b in buffer], dtype=torch.float32)
    old_logp    = torch.tensor([b[3] for b in buffer], dtype=torch.float32)
    next_states = torch.stack([b[4] for b in buffer])

    with torch.no_grad():
        values      = critic(states)
        next_values = critic(next_states)

        deltas     = rewards + gamma * next_values - values
        advantages = torch.zeros_like(rewards)
        gae        = 0.0
        for t in reversed(range(len(buffer))):
            gae           = float(deltas[t]) + gamma * lam * gae
            advantages[t] = gae

        td_targets = advantages + values
        advantages = (advantages - advantages.mean()) / (advantages.std() + 1e-8)
        
        # Ref model is original pretrained model. Used in KL divergence so that the RL does not forget everything learning in pretraining. 
        ref_logits = ref_actor(states) if ref_actor is not None else None

    total_loss = 0.0
    finite_updates = 0
    for _ in range(epochs):
        logits     = actor(states)
        dist       = Categorical(logits=logits)
        logp       = dist.log_prob(actions)
        entropy    = dist.entropy().mean()

        log_ratio  = (logp - old_logp).clamp(-10.0, 10.0)
        ratio      = log_ratio.exp()
        surr1      = ratio * advantages
        surr2      = ratio.clamp(1 - clip_eps, 1 + clip_eps) * advantages
        actor_loss = -torch.min(surr1, surr2).mean()

        value_pred = critic(states)
        # Huber loss is less sensitive to large initial critic errors than MSE.
        value_loss = nn.functional.huber_loss(value_pred, td_targets, delta=1.0)

        kl = torch.zeros(1)
        if ref_logits is not None:
            kl = torch.distributions.kl_divergence(
                dist, Categorical(logits=ref_logits)
            ).mean().clamp(max=10.0)

        loss = actor_loss + vf_coef * value_loss - ent_coef * entropy + kl_coef * kl
        if not torch.isfinite(loss):
            continue
        actor_opt.zero_grad()
        critic_opt.zero_grad()
        loss.backward()
        nn.utils.clip_grad_norm_(actor.parameters(),  1.0)
        nn.utils.clip_grad_norm_(critic.parameters(), 1.0)
        actor_opt.step()
        critic_opt.step()
        total_loss += loss.item()
        finite_updates += 1

    actor.eval()
    critic.eval()
    return total_loss / max(finite_updates, 1)

# ── main loop ──────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inference-only', action='store_true')
    ap.add_argument('--update-every',   type=int,   default=1000,
                    help='steps between PPO updates (default: 1000)')
    ap.add_argument('--lr',             type=float, default=1e-5)
    ap.add_argument('--save-every',     type=int,   default=1000,
                    help='save checkpoint every N steps')
    ap.add_argument('--model',          type=str,   default='model_fp32.pt',
                    help='model filename in training dir (default: supervised model, not RL checkpoint)')
    ap.add_argument('--log-gates',      action='store_true',
                    help='print gate decisions with PMU values every step (for debugging)')
    ap.add_argument('--kl-coef',        type=float, default=0.3,
                    help='KL penalty coefficient vs reference model (default: 0.3)')
    ap.add_argument('--vf-coef',        type=float, default=0.1,
                    help='value function loss coefficient (default: 0.1)')
    ap.add_argument('--ref-model',      type=str,   default='model_fp32.pt',
                    help='frozen reference model for KL anchor (default: model_fp32.pt)')
    ap.add_argument('--ent-coef',       type=float, default=0.01,
                    help='entropy bonus coefficient (default: 0.01; increase to 0.05 to break frequency fixation)')
    args = ap.parse_args()

    if os.geteuid() != 0:
        print('error: must run as root')
        sys.exit(1)

    try:
        os.sched_setaffinity(0, {4, 5})
        print('governor pinned to cores 4-5')
    except OSError as e:
        print(f'warning: could not pin to cores 4-5: {e}')

    # Load model + scaler (normalization) 
    model_path = TRAINING_DIR / args.model
    if not model_path.exists():
        print(f'error: model not found: {model_path}')
        sys.exit(1)

    actor = FreqMLP(input_dim=INPUT_DIM, num_classes=N_CLASSES)
    actor.load_state_dict(torch.load(model_path, map_location='cpu', weights_only=True))
    actor.eval()

    # Derive scaler filename from model: model_fp32_eff.pt → scaler_eff.pkl, else scaler.pkl
    model_stem  = Path(args.model).stem            # e.g. "model_fp32_eff"
    scaler_stem = model_stem.replace('model_fp32', 'scaler', 1)  # → "scaler_eff"
    scaler_path = TRAINING_DIR / f'{scaler_stem}.pkl'
    if not scaler_path.exists():
        scaler_path = TRAINING_DIR / 'scaler.pkl'
    scaler = joblib.load(scaler_path)
    print(f'scaler: {scaler_path.name}')
    def normalize(x): return scaler.transform(x.reshape(1, -1))[0].astype(np.float32)

    ref_actor = None
    if not args.inference_only:
        ref_path = TRAINING_DIR / args.ref_model
        if not ref_path.exists():
            ref_path = TRAINING_DIR / 'model_fp32.pt'
        if ref_path.exists():
            ref_actor = FreqMLP(input_dim=INPUT_DIM, num_classes=N_CLASSES)
            ref_actor.load_state_dict(torch.load(ref_path, map_location='cpu', weights_only=True))
            ref_actor.eval()
            for p in ref_actor.parameters():
                p.requires_grad_(False)
            print(f'ref_actor (KL anchor): {ref_path.name}')

    critic     = ValueNet()
    actor_opt  = torch.optim.Adam(actor.parameters(),  lr=args.lr)
    critic_opt = torch.optim.Adam(critic.parameters(), lr=args.lr)

    # Allows us to change freq
    switch_governor_userspace()
    tegra = TegrastatsReader()
    time.sleep(0.5)

    print(f'governor started  model={args.model}  update_every={args.update_every}  '
          f'lr={args.lr}  inference_only={args.inference_only}')

    prev_feat      = None
    buffer         = []
    
    step = 0 
    m = re.search(r'_rl_(\d+)\.pt', args.model)
    if m: 
        step = int(m.group(1))

    total_updates = step // args.update_every
    last_ts        = None
    pending        = None   # (x_t, action, logp, ts) — reward deferred to next snapshot
    last_reward    = 0.0
    last_executed_action = 21 if args.inference_only else 11  # inference: start at max; RL: start mid-range for exploration

    ACTION_FLOOR = 11  # index into DVFS_STEPS (960000 kHz)

    # Running reward normalizer: keeps reward variance stable so gradient
    reward_mean = 0.0
    reward_var  = 1.0
    reward_n    = 0
   
    # Power smoothing 
    ema_power_mw = None
    POWER_ALPHA = 0.2 

    # EMA of raw PMU signals — smooths window noise for logging.
    EMA_ALPHA   = 0.10
    ema_ll_miss = 0.0
    ema_stall   = 0.0

    # EMA of model logits — prevents single-step oscillation between classes
    EMA_LOGIT_ALPHA = 0.30
    ema_logits      = None

    try:
        while True:
            # Snapshots arrive every ~16ms. We sleep 13ms at the end of each
            # control cycle so this thread is idle for most of each window,
            # leaving CPU and cache bandwidth for the workload.
            while True:
                try:
                    ts, freq_p0, freq_p4, deltas = read_snapshot()
                    if ts != last_ts:
                        last_ts = ts
                        break
                except IOError as e:
                    print(f'[warn] snapshot read failed: {e}')
                time.sleep(0.002)

            pmic = tegra.get()

            # deltas contain instructions accumulated while the frequency we
            # chose last step was active — the true consequence of that action.
            # Buffer append is deferred until x_t (next state) is built below.
            prev_transition = None
            if pending is not None:
                x_prev, a_prev, lp_prev, ts_prev, step_diff = pending
                window_s   = (ts - ts_prev) / 1e9
                # Cores 0-3 only — the governor/PyTorch runs on cores 4-5
                total_inst = float(deltas[:4, 0].sum())
                inst_per_sec = total_inst / max(window_s, 0.001) 

                current_power = max(pmic['vdd_in_power_mw'], 1.0) 
                if ema_power_mw is None: 
                    ema_power_mw = current_power 
                else: 
                    ema_power_mw = POWER_ALPHA * current_power + (1.0 - POWER_ALPHA) * ema_power_mw 
                
                pmic['vdd_in_power_mw'] = ema_power_mw 
                TRANSITION_PENALTY = 0.05
                raw_reward = (float(inst_per_sec / ema_power_mw) / 1e5) - (TRANSITION_PENALTY * float(step_diff))
                # Prevents reward scale from dominating gradient magnitude and
                # stabilizes training across workloads with very different
                # instruction rates (e.g. stream vs cpu_all).
                reward_n   += 1
                delta_r     = raw_reward - reward_mean
                reward_mean += delta_r / reward_n
                reward_var  += (delta_r * (raw_reward - reward_mean) - reward_var) / reward_n
                last_reward  = float(np.clip(
                    (raw_reward - reward_mean) / (reward_var ** 0.5 + 1e-8), -5.0, 5.0
                ))
                prev_transition = (x_prev, a_prev, last_reward, lp_prev)
                pending = None

            # Build feature vector
            feat = build_features(freq_p0, freq_p4, deltas, pmic)
            if prev_feat is None:
                prev_feat = feat.copy()
            delta_feat = feat - prev_feat
            x_raw = np.concatenate([feat, delta_feat]).astype(np.float32)
            x_t = torch.tensor(normalize(x_raw), dtype=torch.float32)

            # x_t is the next state — complete and store the transition
            if prev_transition is not None and not args.inference_only:
                buffer.append((*prev_transition, x_t))

            active = np.array([deltas[c, 0] > 1000 for c in range(6)])
            if active.any():
                cur_ll    = float(feat[62:68][active].mean())
                cur_stall = float(feat[44:50][active].mean())
            else:
                cur_ll, cur_stall = 0.0, 0.0
            ema_ll_miss = EMA_ALPHA * cur_ll    + (1 - EMA_ALPHA) * ema_ll_miss
            ema_stall   = EMA_ALPHA * cur_stall + (1 - EMA_ALPHA) * ema_stall

            # ── inference ─────────────────────────────────────────────────────
            with torch.no_grad():
                logits = actor(x_t)
                dist   = Categorical(logits=logits)

                if ema_logits is None:
                    ema_logits = logits.clone()
                else:
                    ema_logits = EMA_LOGIT_ALPHA * logits + (1 - EMA_LOGIT_ALPHA) * ema_logits

                if args.inference_only:
                    action = ema_logits.argmax(dim=0).item()
                    gate = 'model' 
                    cpu_util = pmic['cpu_util_avg_pct']
                    emc_util = pmic['emc_util_pct']

                    if args.log_gates and step % 10 == 0:
                        print(f'[{step:6d}] util={cpu_util:5.1f}% emc={emc_util:4.1f}%'
                              f' ll={ema_ll_miss:.3f} stall={ema_stall:.3f}'
                              f' gate={gate} → {DVFS_STEPS[action]:>8} kHz', flush=True)
                else:
                    if ref_actor is not None and np.random.random() < 0.15:
                        action = ref_actor(x_t).argmax().item()
                    else:
                        action = dist.sample().item()
                    # Hard floor: never go below 576 MHz during training.
                    # Prevents the policy collapsing into the low-frequency trap.
                    action = max(action, ACTION_FLOOR)
                
                step_diff = abs(action - last_executed_action) 
                last_executed_action = action 
                logp = dist.log_prob(torch.tensor(action)).item()

            freq_khz = int(DVFS_STEPS[action])
            set_frequency(freq_khz)

            pending = (x_t, action, logp, ts, step_diff)

            # ── PPO + GAE update ──────────────────────────────────────────────
            if not args.inference_only and len(buffer) >= args.update_every:
                current_kl = max(0.005, args.kl_coef * (1.0 - total_updates / 2000.0)) 

                loss = ppo_update(actor, critic, actor_opt, critic_opt, buffer,
                                 ref_actor=ref_actor,
                                 kl_coef=current_kl, vf_coef=args.vf_coef,
                                 ent_coef=args.ent_coef)
                total_updates += 1
                print(f'[step {step}] ppo #{total_updates}  loss={loss:.4f}  '
                      f'freq={freq_khz}  eff={last_reward:.3f}')
                buffer.clear()

            # Checkpoint
            if step > 0 and step % args.save_every == 0:
                ckpt = TRAINING_DIR / f'model_fp32_rl_{step}.pt'
                torch.save(actor.state_dict(), ckpt)
                print(f'[step {step}] saved {ckpt.name}')

            prev_feat = feat.copy()
            step += 1
            time.sleep(0.013)   # yield CPU for ~13ms between 16ms snapshots

    except KeyboardInterrupt:
        print(f'\nstopped at step {step}  total ppo updates={total_updates}')
        if not args.inference_only and total_updates > 0:
            out = TRAINING_DIR / 'model_fp32_rl_final.pt'
            torch.save(actor.state_dict(), out)
            print(f'saved → {out}')
        tegra.stop()
        os._exit(0)   # force-exit: don't wait for daemon threads or tegrastats pipe
    finally:
        tegra.stop()


if __name__ == '__main__':
    main()
