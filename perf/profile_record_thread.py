# /// script
# requires-python = ">=3.10"
# dependencies = ["h5py", "numpy"]
# ///
"""Profile the GUI's Record Thread while recording, to compare how busy the record engines are.

The Record Thread polls its data queues without waiting (RecordThread::run calls writeData in a loop), so it always
uses a full CPU core while recording, and the process CPU time doesn't show the cost of an engine. Instead, this script
runs the macOS `sample` profiler on the GUI during a recording and reports which fraction of the Record Thread's samples
are spent in the engine's continuous data writes (RecordEngine::writeContinuousData / writeContinuousDataBatch and
everything they call, e.g. HDF5 or file I/O). This is the engine's utilization of the Record Thread: at 100%, the
engine can't keep up.

For some samples, `sample` can't unwind the stack through the engine function: the caller frame is reported as `???`
directly inside RecordThread::writeData, with the engine code below it. This happens for the Binary engine's
SIMDConverter (SIMD code without a stack frame) and accounts for about 40% of Binary's write time. These samples are
counted as engine writes too (`unresolved_samples`; RecordThread::writeData only calls the engine and the data queues,
whose frames are resolved).

Uses the Release GUI and plugin set up by setup_release.sh (see run_perf.py).
Usage: uv run --isolated perf/profile_record_thread.py --channels 384 1536 3072 [--engines BINARY NWB2]
       uv run --isolated perf/profile_record_thread.py --reanalyze perf/results/profile.jsonl [--profiles DIR] [--label L]
       (recompute the results of a JSON lines file from its profiles, e.g. after changing the analysis)
Results are appended as JSON lines to perf/results/profile.jsonl; `sample` call trees are written to $PERF_WORK/profiles
(compressed copies of the ones used for the summary are in perf/results/profiles).
"""
import argparse
import gzip
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import run_perf as rp  # noqa: E402

PROFILES = rp.WORK / "profiles"
WRITE_FUNCS = re.compile(r"writeContinuousData(Batch)?\b")
LINE = re.compile(r"^(?P<indent>[\s+!:|]*)(?P<count>\d+)\s+(?P<name>.+)$")


def thread_block(text, thread_pattern):
    """Total number of samples and the call tree lines of the first thread whose name matches thread_pattern"""
    m = re.search(r"\n\s*(\d+) Thread_\d+: " + thread_pattern + r"[^\n]*\n(.*?)(?=\n\s*\d+ Thread_\d+|\nTotal number in stack|\Z)",
                  text, re.S)
    return (int(m.group(1)), m.group(2).splitlines()) if m else (None, [])


def outermost_count(lines, pattern):
    """Number of samples in frames matching pattern (outermost matching frames only, i.e. inclusive time)"""
    count, match_depth = 0, None
    for line in lines:
        lm = LINE.match(line)
        if not lm:
            continue
        depth = len(lm.group("indent"))
        if match_depth is not None and depth <= match_depth:
            match_depth = None
        if match_depth is None and re.search(pattern, lm.group("name")):
            count += int(lm.group("count"))
            match_depth = depth
    return count


def unresolved_count(lines, parent_pattern):
    """Number of samples in unresolved frames (`???`) directly inside a frame matching parent_pattern"""
    count, stack = 0, []
    for line in lines:
        lm = LINE.match(line)
        if not lm:
            continue
        depth, name = len(lm.group("indent")), lm.group("name")
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if name.startswith("???") and stack and re.search(parent_pattern, stack[-1][1]):
            count += int(lm.group("count"))
        stack.append((depth, name))
    return count


def read_profile(path):
    path = Path(path)
    return gzip.open(path, "rt").read() if path.suffix == ".gz" else path.read_text()


def analyze(path):
    """Shares of the Record Thread spent in engine writes, and of the processing (audio) thread that is busy"""
    text = read_profile(path)
    total, lines = thread_block(text, "Record Thread")
    ptotal, plines = thread_block(text, r"com\.apple\.audio\.IOThread\.client")
    r = {"record_thread_samples": total}
    if total:
        r["unresolved_samples"] = unresolved_count(lines, r"RecordThread::writeData\b")
        r["write_samples"] = outermost_count(lines, WRITE_FUNCS.pattern) + r["unresolved_samples"]
    r["write_utilization"] = round(r["write_samples"] / total, 3) if total else None
    if ptotal:
        r["processing_busy"] = round(outermost_count(plines, r"audioIOProc") / ptotal, 3)
        r["file_reader"] = round(outermost_count(plines, r"FileReader::process\(") / ptotal, 3)
        r["record_node"] = round(outermost_count(plines, r"RecordNode::process\(") / ptotal, 3)
    return r


def profile(dataset, engine, channels, seconds, label=""):
    record_dir = rp.WORK / "recordings"
    record_dir.mkdir(exist_ok=True)
    PROFILES.mkdir(exist_ok=True)
    config = rp.WORK / "perf_config.xml"
    rp.make_config(dataset, engine, record_dir, config)
    log = open(rp.WORK / f"gui_profile_{engine}_{channels}.log", "w")
    gui = subprocess.Popen([str(rp.APP), str(config)], stdout=log, stderr=subprocess.STDOUT)
    out = PROFILES / f"{engine}_{channels}{label}.txt"
    try:
        for _ in range(60):
            try:
                rp.api("status")
                break
            except Exception:
                time.sleep(1)
        rp.api("status", {"mode": "ACQUIRE"})
        time.sleep(3)
        rp.api("status", {"mode": "RECORD"})
        time.sleep(3)  # let the recording settle (file creation, first flush)
        subprocess.run(["sample", str(gui.pid), str(int(seconds)), "-mayDie", "-file", str(out)],
                       capture_output=True, check=True)
        status = rp.api("status").get("mode")
        rp.api("status", {"mode": "ACQUIRE"})
        time.sleep(2)
        rp.api("status", {"mode": "IDLE"})
        time.sleep(2)
        try:
            rp.api("quit", {})
        except Exception:
            pass
        gui.wait(timeout=60)
    finally:
        if gui.poll() is None:
            gui.kill()
        log.close()
    d, n, size = rp.written_samples(record_dir, engine, channels)
    shutil.rmtree(d)
    return {"engine": engine, "channels": channels, "rate_MBps": round(channels * rp.RATE * 2 / 1e6, 1),
            "status_at_end": status, "written_s": round(n / rp.RATE, 2), **analyze(out)}


def reanalyze(results, profiles, label):
    """Recompute the profile analysis of each result in a JSON lines file (in place)"""
    rows = [json.loads(line) for line in results.read_text().splitlines() if line.strip()]
    for row in rows:
        name = f"{row['engine']}_{row['channels']}{label}.txt"
        path = next((p for p in (profiles / name, profiles / (name + ".gz")) if p.exists()), None)
        if path is None:
            print(f"{results.name}: no profile {name} in {profiles}, kept as is", file=sys.stderr)
            continue
        old = row.get("write_utilization")
        row.update(analyze(path))
        print(f"{results.name} {row['engine']} {row['channels']}: write_utilization {old} -> {row['write_utilization']}")
    results.write_text("".join(json.dumps(row) + "\n" for row in rows))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--channels", type=int, nargs="+", default=[384, 1536, 3072])
    p.add_argument("--engines", nargs="+", default=["BINARY", "NWB2"])
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--output", default="profile.jsonl")
    p.add_argument("--label", default="", help="suffix for the profile file names")
    p.add_argument("--reanalyze", metavar="JSONL", help="recompute the results in this file from the profiles")
    p.add_argument("--profiles", default=str(rp.RESULTS / "profiles"),
                   help="directory with the profiles for --reanalyze (<engine>_<channels><label>.txt[.gz])")
    a = p.parse_args()
    if a.reanalyze:
        return reanalyze(Path(a.reanalyze), Path(a.profiles), a.label)
    rp.WORK.mkdir(parents=True, exist_ok=True)
    rp.RESULTS.mkdir(exist_ok=True)
    backup = rp.WORK / "configs-api10.backup"
    if rp.CONFIGS.is_dir() and not backup.exists():
        shutil.copytree(rp.CONFIGS, backup)
    try:
        for ch in a.channels:
            dataset = rp.WORK / f"stress_data_{ch}"
            if not (dataset / "structure.oebin").exists():
                subprocess.run(["uv", "run", "-q", "--isolated", str(rp.PERF / "make_stress_dataset.py"),
                                str(dataset), str(ch), str(rp.RATE), "5"], check=True)
            for engine in a.engines:
                r = profile(dataset / "structure.oebin", engine, ch, a.seconds, a.label)
                print(json.dumps(r), flush=True)
                with open(rp.RESULTS / a.output, "a") as f:
                    f.write(json.dumps(r) + "\n")
    finally:
        if backup.exists():
            shutil.rmtree(rp.CONFIGS, ignore_errors=True)
            shutil.copytree(backup, rp.CONFIGS)
            shutil.rmtree(backup)


if __name__ == "__main__":
    sys.exit(main())
