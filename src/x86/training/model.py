import torch
import torch.nn as nn
import torch.quantization

class FreqMLP(nn.Module):
    def __init__(self, input_dim=330, num_classes=12, dropout=0.2):
        super().__init__()
        self.quant = torch.quantization.QuantStub()
        self.net = nn.Sequential(
            nn.Linear(input_dim, 512),
            nn.ReLU(),
            nn.Dropout(dropout),

            nn.Linear(512, 256),
            nn.ReLU(),
            nn.Dropout(dropout),

            nn.Linear(256, 128),
            nn.ReLU(),
            nn.Dropout(dropout),

            nn.Linear(128, 64),
            nn.ReLU(),

            nn.Linear(64, num_classes),
        )
        self.dequant = torch.quantization.DeQuantStub()

    def forward(self, x):
        return self.dequant(self.net(self.quant(x)))
