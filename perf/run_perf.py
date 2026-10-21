# /// script
# requires-python = ">=3.10"
# dependencies = ["h5py", "numpy"]
# ///
"""Record performance of the NWB2 and Binary record engines with the Release (optimized) GUI and plugin.

Runs the GUI from plugin-GUI/Build/Release with the Release plugin inside the app (see setup_release.sh), so the Debug
development setup is not used. The GUI saves its state in ~/Library/Application Support/open-ephys/configs-api10 (as
the Debug GUI does); this directory is backed up before and restored after the runs. Each run plays back a synthetic dataset (perf/make_stress_dataset.py) with the File
Reader into a Record Node (no other processors), then:

1. acquires for --baseline seconds without recording (CPU time of the GUI process without writing),
2. records for --seconds seconds (CPU time with writing; status at the end shows whether the GUI stopped the
   recording because the record buffer filled up),
3. counts the samples written to the file and compares them with the recording time.

Usage: uv run --isolated perf/run_perf.py --channels 384 1536 --engines BINARY NWB2 --repeats 3 [--seconds 30]
Results are appended as JSON lines to perf/results/results.jsonl (see paths.py for the directories used).
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

import h5py

sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import APP, CONFIGS, PERF, RESULTS, WORK  # noqa: E402
API = "http://localhost:37497/api"
RATE = 30000


def api(path, body=None):
    req = urllib.request.Request(f"{API}/{path}", method="PUT" if body is not None else "GET",
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read() or b"{}")


def cpu_seconds(pid):
    """Total CPU time (user + system, all threads) of a process, from ps (format [[dd-]hh:]mm:ss.ss)"""
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    days, _, rest = out.rpartition("-")
    parts = [float(p) for p in rest.split(":")]
    secs = 0.0
    for p in parts:
        secs = secs * 60 + p
    return secs + (float(days) * 86400 if days else 0.0)


def make_config(dataset, engine, record_dir, path, channels=None, record_channels=None):
    s = (PERF / "perf_template.xml").read_text()  # File Reader -> Record Node only
    s = re.sub(r'selected_file="[^"]*"', f'selected_file="{dataset}"', s)
    s = re.sub(r'start_time="[^"]*"', 'start_time="00:00:00"', s)
    s = re.sub(r'end_time="[^"]*"', 'end_time="00:00:00"', s)
    s = re.sub(r'engine="[0-9]"', 'engine="0"' if engine == "BINARY" else 'engine="1"', s)
    s = re.sub(r'recordEngine="[A-Z0-9]*"', f'recordEngine="{engine}"', s)
    s = re.sub(r'directory="[^"]*"', f'directory="{record_dir}"', s)
    s = re.sub(r'recordPath="[^"]*"', f'recordPath="{record_dir}"', s)
    # stream settings of the dataset (otherwise the GUI resets the stream parameters, e.g. the recorded channels)
    if channels:
        s = re.sub(r'channel_count="[0-9]+"', f'channel_count="{channels}"', s)
        s = re.sub(r'(<STREAM name=")[^"]*(" description="[^"]*"\s*sample_rate=")[^"]*"', rf'\g<1>stress_data\g<2>{RATE}.0"', s)
    if record_channels:  # record only the first record_channels channels (the Record Node parameter is a mask)
        mask = ",".join(str(c) for c in range(record_channels + 1, channels + 1))
        s = re.sub(r'<PARAMETERS channels="[^"]*"', f'<PARAMETERS channels="{mask}"', s)
    assert "LFP Viewer" not in s
    path.write_text(s)


def written_samples(rec_dir, engine, channels):
    d = sorted(glob.glob(f"{rec_dir}/20*/"), key=os.path.getmtime)[-1]
    size = sum(f.stat().st_size for f in Path(d).rglob("*") if f.is_file())
    if engine == "NWB2":
        with h5py.File(glob.glob(f"{d}/*/experiment*.nwb")[0], "r") as f:
            es = [g for g in f["acquisition"].values() if g.attrs.get("neurodata_type") == "ElectricalSeries"][0]
            n = es["data"].shape[0]
    else:
        dat = glob.glob(f"{d}/*/experiment*/recording*/continuous/*/continuous.dat")[0]
        n = os.path.getsize(dat) // (2 * channels)
    return d, n, size


def run(dataset, engine, channels, seconds, baseline, record_channels=None, keep=False):
    record_dir = WORK / "recordings"
    record_dir.mkdir(exist_ok=True)
    config = WORK / "perf_config.xml"
    make_config(dataset, engine, record_dir, config, channels, record_channels)
    log = open(WORK / f"gui_{engine}_{channels}.log", "w")
    gui = subprocess.Popen([str(APP), str(config)], stdout=log, stderr=subprocess.STDOUT)
    try:
        for _ in range(60):
            try:
                api("status")
                break
            except Exception:
                time.sleep(1)
        api("status", {"mode": "ACQUIRE"})
        time.sleep(3)  # warm up (the dataset is read from disk once, then from the page cache)
        c0, t0 = cpu_seconds(gui.pid), time.time()
        time.sleep(baseline)
        c1, t1 = cpu_seconds(gui.pid), time.time()
        api("status", {"mode": "RECORD"})
        c2, t2 = cpu_seconds(gui.pid), time.time()
        time.sleep(seconds)
        c3, t3 = cpu_seconds(gui.pid), time.time()
        status = api("status").get("mode")
        api("status", {"mode": "ACQUIRE"})
        t4 = time.time()
        time.sleep(2)
        api("status", {"mode": "IDLE"})
        time.sleep(2)
        try:
            api("quit", {})
        except Exception:
            pass
        gui.wait(timeout=60)
    finally:
        if gui.poll() is None:
            gui.kill()
        log.close()
    d, n, size = written_samples(record_dir, engine, record_channels or channels)
    if not keep:
        shutil.rmtree(d)
    record_s = t4 - t2
    return {
        "engine": engine, "channels": channels, "recorded_channels": record_channels or channels, "rate_MBps": round(channels * RATE * 2 / 1e6, 1),
        "status_at_end": status, "kept_up": status == "RECORD" and n >= 0.99 * record_s * RATE,
        "record_s": round(record_s, 2), "written_s": round(n / RATE, 2),
        "written_fraction": round(n / (record_s * RATE), 3), "size_GB": round(size / 1e9, 3),
        "cpu_acquire": round((c1 - c0) / (t1 - t0), 3),  # CPU cores used without recording
        "cpu_record": round((c3 - c2) / (t3 - t2), 3),  # CPU cores used while recording
    }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--channels", type=int, nargs="+", default=[384, 1536, 3072])
    p.add_argument("--engines", nargs="+", default=["BINARY", "NWB2"])
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--seconds", type=float, default=30)
    p.add_argument("--baseline", type=float, default=15)
    p.add_argument("--record-channels", type=int, default=None, help="record only the first N channels")
    p.add_argument("--output", default="results.jsonl")
    p.add_argument("--keep", action="store_true", help="keep the recordings (in perf/recordings)")
    a = p.parse_args()
    WORK.mkdir(parents=True, exist_ok=True)
    RESULTS.mkdir(exist_ok=True)
    backup = WORK / "configs-api10.backup"
    if CONFIGS.is_dir() and not backup.exists():
        shutil.copytree(CONFIGS, backup)
    try:
        measure(a)
    finally:
        if backup.exists():
            shutil.rmtree(CONFIGS, ignore_errors=True)
            shutil.copytree(backup, CONFIGS)
            shutil.rmtree(backup)


def measure(a):
    for ch in a.channels:
        dataset = WORK / f"stress_data_{ch}"
        if not (dataset / "structure.oebin").exists():
            subprocess.run(["uv", "run", "-q", "--isolated", str(PERF / "make_stress_dataset.py"), str(dataset),
                            str(ch), str(RATE), "5"], check=True)
        for rep in range(a.repeats):
            for engine in a.engines:  # alternate engines within a repeat
                r = run(dataset / "structure.oebin", engine, ch, a.seconds, a.baseline, a.record_channels, a.keep)
                r["repeat"] = rep
                print(json.dumps(r), flush=True)
                with open(RESULTS / a.output, "a") as f:
                    f.write(json.dumps(r) + "\n")


if __name__ == "__main__":
    sys.exit(main())
