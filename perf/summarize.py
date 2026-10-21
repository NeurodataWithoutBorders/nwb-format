"""Summarize perf/results/results.jsonl (from run_perf.py) per channel count and engine as a markdown table.

Usage: python3 perf/summarize.py [perf/results/results.jsonl]
"""
import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

path = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).parent / "results" / "results.jsonl")
runs = defaultdict(list)
for line in path.read_text().splitlines():
    r = json.loads(line)
    runs[(r["channels"], r["engine"])].append(r)


def span(values, fmt):
    lo, hi = fmt.format(min(values)), fmt.format(max(values))
    return lo if lo == hi else f"{lo}-{hi}"


print("| Channels | Data rate | Engine | Kept up | Data written | CPU without recording | CPU while recording | Added CPU for recording | File size |")
print("|---:|---:|---|---|---|---|---|---|---|")
for (ch, engine), rs in sorted(runs.items()):
    kept = sum(r["kept_up"] for r in rs)
    written = [100 * r["written_fraction"] for r in rs]
    acq = [r["cpu_acquire"] for r in rs]
    rec = [r["cpu_record"] for r in rs]
    added = [r["cpu_record"] - r["cpu_acquire"] for r in rs]
    size_per_s = [r["size_GB"] / r["record_s"] for r in rs]
    print(f"| {ch} | {rs[0]['rate_MBps']:.0f} MB/s | {engine} | {kept}/{len(rs)} | {span(written, '{:.0f}')}% "
          f"| {statistics.mean(acq):.2f} | {statistics.mean(rec):.2f} | {statistics.mean(added):.2f} "
          f"| {1000 * statistics.mean(size_per_s):.0f} MB/s |")
