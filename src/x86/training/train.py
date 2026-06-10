#!/usr/bin/env python3
import copy
import numpy as np
import pandas as pd
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader
from sklearn.preprocessing import StandardScaler
from pathlib import Path
import joblib

from model import FreqMLP

# ── config ────────────────────────────────────────────────────────────────────

DATA_DIR  = Path(__file__).parent.parent
OUT_DIR   = Path(__file__).parent

EPOCHS      = 100
QAT_EPOCHS  = 15
BATCH       = 512
LR          = 1e-3
PATIENCE    = 10

FEATURE_DIM = 165
INPUT_DIM   = 330   # 165 current + 165 delta
N_CLASSES   = 12

DVFS_STEPS = np.array([
    400000, 800000, 1200000, 1600000, 2000000, 2400000, 
    2800000, 3200000, 3600000, 4000000, 4400000, 4800000
])

# ── data ──────────────────────────────────────────────────────────────────────

class FreqDataset(Dataset):
    def __init__(self, X, y):
        self.X = torch.tensor(X, dtype=torch.float32)
        self.y = torch.tensor(y, dtype=torch.long)

    def __len__(self):
        return len(self.y)

    def __getitem__(self, idx):
        return self.X[idx], self.y[idx]


def load_data():
    df           = pd.read_csv(DATA_DIR / 'training_data.csv')
    # feature_cols from preprocess.py:
    # ['freq_khz_p0', 'freq_khz_p6']
    # + [f'{ev}_c{c}' for ev in PMU_EVENTS + ['cycles'] for c in range(12)]
    # + [f'{r}_c{c}'  for r  in RATIO_COLS              for c in range(12)]
    # + PMIC_COLS
    
    # We'll re-derive them to be safe
    pmu_events = ['inst_retired', 'stall_backend', 'stall_frontend', 'l2_cache_miss', 'br_mis_pred', 'dtlb_miss']
    ratio_cols = ['stall_ratio', 'fe_stall_ratio', 'ipc', 'l2_miss_rate', 'br_misrate', 'dtlb_rate']
    pmic_cols  = ['cpu_util_avg_pct', 'emc_util_pct', 'ram_used_mb', 'cpu_temp_c', 'tj_temp_c', 'core_power_mw', 'pkg_power_mw']
    
    feature_cols = (
        ['freq_khz_p0', 'freq_khz_p6']
        + [f'{ev}_c{c}' for ev in pmu_events + ['cycles'] for c in range(12)]
        + [f'{r}_c{c}'  for r  in ratio_cols              for c in range(12)]
        + pmic_cols
    )
    
    assert len(feature_cols) == FEATURE_DIM

    df    = df.sort_values('segment_id').reset_index(drop=True)
    delta = df.groupby('segment_id')[feature_cols].diff().fillna(0).values.astype(np.float32)

    X       = np.concatenate([df[feature_cols].values.astype(np.float32), delta], axis=1)
    y       = df['label_class'].values.astype(np.int64)
    seg_ids = df['segment_id'].values
    return X, y, seg_ids


def split_segments(seg_ids, train_frac=0.70, val_frac=0.15, seed=42):
    ids = np.random.default_rng(seed).permutation(np.unique(seg_ids))
    n   = len(ids)
    i1  = int(n * train_frac)
    i2  = int(n * (train_frac + val_frac))
    return set(ids[:i1]), set(ids[i1:i2]), set(ids[i2:])


def make_loaders(X_train, y_train, X_val, y_val, X_test, y_test):
    kw = dict(batch_size=BATCH, num_workers=2, pin_memory=True)
    return (
        DataLoader(FreqDataset(X_train, y_train), shuffle=True,  **kw),
        DataLoader(FreqDataset(X_val,   y_val),   shuffle=False, **kw),
        DataLoader(FreqDataset(X_test,  y_test),  shuffle=False, **kw),
    )


def class_weights(y):
    counts = np.bincount(y, minlength=N_CLASSES).astype(float)
    w      = len(y) / (N_CLASSES * np.maximum(counts, 1))
    return torch.tensor(w, dtype=torch.float32)

# ── train / eval ──────────────────────────────────────────────────────────────

def train_epoch(model, loader, optimizer, criterion, device):
    model.train()
    total = 0.0
    for X, y in loader:
        X, y = X.to(device), y.to(device)
        optimizer.zero_grad()
        loss = criterion(model(X), y)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        total += loss.item() * len(y)
    return total / len(loader.dataset)


def evaluate(model, loader, criterion, device):
    model.eval()
    total, correct, top2 = 0.0, 0, 0
    with torch.no_grad():
        for X, y in loader:
            X, y   = X.to(device), y.to(device)
            logits = model(X)
            total   += criterion(logits, y).item() * len(y)
            correct += (logits.argmax(dim=1) == y).sum().item()
            top2    += (logits.topk(2, dim=1).indices == y.unsqueeze(1)).any(dim=1).sum().item()
    n = len(loader.dataset)
    return total / n, correct / n, top2 / n


def run_training(model, train_loader, val_loader, criterion, device, epochs, patience, tag):
    optimizer = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=1e-4)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)

    best_loss, best_weights, no_improve = float('inf'), None, 0

    print(f"\n── {tag} ──")
    print(f"{'epoch':>5}  {'train_loss':>10}  {'val_loss':>10}  {'val_acc':>8}  {'val_top2':>9}")
    for epoch in range(1, epochs + 1):
        train_loss                  = train_epoch(model, train_loader, optimizer, criterion, device)
        val_loss, val_acc, val_top2 = evaluate(model, val_loader, criterion, device)
        scheduler.step()
        print(f"{epoch:5d}  {train_loss:10.4f}  {val_loss:10.4f}  {val_acc:8.4f}  {val_top2:9.4f}")

        if val_loss < best_loss:
            best_loss    = val_loss
            best_weights = copy.deepcopy(model.state_dict())
            no_improve   = 0
        else:
            no_improve += 1
            if no_improve >= patience:
                print(f"  early stop at epoch {epoch}")
                break

    model.load_state_dict(best_weights)
    return model

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    device = torch.device('cuda' if torch.cuda.is_available() else 'cpu')
    print(f"device: {device}")

    # For x86 we typically want fbgemm
    torch.backends.quantized.engine = 'fbgemm'

    if not (DATA_DIR / 'training_data.csv').exists():
        print(f"Error: {DATA_DIR / 'training_data.csv'} not found. Run preprocess.py first.")
        exit(1)

    print("loading data...")
    X, y, seg_ids = load_data()

    train_segs, val_segs, test_segs = split_segments(seg_ids)
    train_mask = np.isin(seg_ids, list(train_segs))
    val_mask   = np.isin(seg_ids, list(val_segs))
    test_mask  = np.isin(seg_ids, list(test_segs))
    print(f"  train={train_mask.sum():,}  val={val_mask.sum():,}  test={test_mask.sum():,}")

    scaler  = StandardScaler()
    X_train = scaler.fit_transform(X[train_mask])
    X_val   = scaler.transform(X[val_mask])
    X_test  = scaler.transform(X[test_mask])
    joblib.dump(scaler, OUT_DIR / 'scaler.pkl')

    train_loader, val_loader, test_loader = make_loaders(
        X_train, y[train_mask], X_val, y[val_mask], X_test, y[test_mask]
    )

    criterion = nn.CrossEntropyLoss(weight=class_weights(y[train_mask]).to(device))
    model     = FreqMLP(input_dim=INPUT_DIM, num_classes=N_CLASSES).to(device)

    # phase 1 — float32 training
    model = run_training(model, train_loader, val_loader, criterion, device, EPOCHS, PATIENCE, "float32 training")
    torch.save(model.state_dict(), OUT_DIR / 'model_fp32.pt')

    # phase 2 — QAT fine-tuning
    model_qat = copy.deepcopy(model)
    model_qat.train()
    model_qat.qconfig = torch.quantization.get_default_qat_qconfig('fbgemm')
    torch.quantization.prepare_qat(model_qat, inplace=True)
    model_qat = model_qat.to(device)

    model_qat = run_training(model_qat, train_loader, val_loader, criterion, device, QAT_EPOCHS, QAT_EPOCHS, "QAT fine-tuning")

    model_qat.eval()
    model_qat.cpu()
    model_int8 = torch.quantization.convert(model_qat)
    torch.save(model_int8.state_dict(), OUT_DIR / 'model_int8.pt')

    # ── final evaluation ──────────────────────────────────────────────────────────

    criterion_cpu = nn.CrossEntropyLoss(weight=class_weights(y[train_mask]))

    print("\n── test set results ──")
    for tag, m, dev, crit in [
        ("float32", model,      device,             criterion),
        ("int8",    model_int8, torch.device('cpu'), criterion_cpu),
    ]:
        loss, acc, top2 = evaluate(m, test_loader, crit, dev)
        print(f"  {tag:8s}  loss={loss:.4f}  acc={acc:.4f}  top2={top2:.4f}")
