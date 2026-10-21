# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""Create a synthetic Open Ephys Binary-format dataset with many channels for record-performance tests.

Usage: uv run --isolated perf/make_stress_dataset.py <output_dir> [channels=384] [rate=30000] [seconds=10]
Writes <output_dir>/structure.oebin and <output_dir>/continuous/stress_data/{continuous.dat,timestamps.npy}.
"""
import json
import sys
from pathlib import Path

import numpy as np

out = Path(sys.argv[1])
channels = int(sys.argv[2]) if len(sys.argv) > 2 else 384
rate = int(sys.argv[3]) if len(sys.argv) > 3 else 30000
seconds = int(sys.argv[4]) if len(sys.argv) > 4 else 10
data_dir = out / "continuous" / "stress_data"
data_dir.mkdir(parents=True, exist_ok=True)

n = rate * seconds
rng = np.random.default_rng(0)
with open(data_dir / "continuous.dat", "wb") as f:
    block = rate  # write one second at a time
    t = np.arange(block)
    for start in range(0, n, block):
        x = rng.normal(0, 400, (block, channels)) + 2000 * np.sin(2 * np.pi * 7 * (start + t) / rate)[:, None]
        f.write(x.astype("<i2").tobytes())
np.save(data_dir / "timestamps.npy", np.arange(n, dtype=np.int64))

structure = {
    "GUI version": "1.1.0",
    "continuous": [{
        "folder_name": "stress_data/",
        "sample_rate": rate,
        "source_processor_name": "Stress source",
        "source_processor_id": 100,
        "source_processor_sub_idx": 0,
        "recorded_processor": "Stress source",
        "recorded_processor_id": 100,
        "num_channels": channels,
        "channels": [{
            "channel_name": f"CH{i + 1}", "description": "Synthetic channel", "identifier": "genericdata.continuous",
            "history": "Stress source", "bit_volts": 0.195, "units": "uV",
            "source_processor_index": i, "recorded_processor_index": i,
        } for i in range(channels)],
    }],
    "events": [],
    "spikes": [],
}
(out / "structure.oebin").write_text(json.dumps(structure, indent=2))
print(f"{channels} channels, {rate} Hz, {seconds} s: {(data_dir / 'continuous.dat').stat().st_size / 1e6:.0f} MB")
