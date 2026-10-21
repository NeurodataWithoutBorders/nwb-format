"""Break down where the Record Thread spends its time in a `sample` profile (from profile_record_thread.py).

Usage: python3 perf/breakdown.py perf/results/profiles/NWB2_3072.txt.gz [more profiles...]
"""
import gzip
import re
import sys
from pathlib import Path

LINE = re.compile(r"^(?P<indent>[\s+!:|]*)(?P<count>\d+)\s+(?P<name>.+)$")
PARTS = {
    "engine writes (writeContinuousData[Batch])": r"writeContinuousData(Batch)?\b",
    "  ElectricalSeries::writeChannel (HDF5 data writes)": r"ElectricalSeries::writeChannel",
    "    H5Dwrite": r"^H5Dwrite\b",
    "    H5VM_memcpyvv (copy into chunks)": r"H5VM_memcpyvv",
    "    hyperslab selection": r"H5S_select_hyperslab|H5Sselect_hyperslab|H5S__hyper",
    "    chunk lookup/cache": r"H5D__chunk_lookup|H5D__chunk_lock|H5D__chunk_unlock",
    "  float -> int16 conversion": r"transformToInt16|convertFloatToInt16|FloatVectorOperations",
    "polling the data queues (DataQueue::startRead)": r"DataQueue::startRead",
}


def read(path):
    p = Path(path)
    return gzip.open(p, "rt").read() if p.suffix == ".gz" else p.read_text()


def thread_block(text, thread_pattern):
    m = re.search(r"\n\s*(\d+) Thread_\d+: " + thread_pattern + r"[^\n]*\n(.*?)(?=\n\s*\d+ Thread_\d+|\nTotal number in stack|\Z)",
                  text, re.S)
    return (int(m.group(1)), m.group(2).splitlines()) if m else (None, [])


def outermost_count(lines, pattern):
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


for path in sys.argv[1:]:
    total, lines = thread_block(read(path), "Record Thread")
    print(f"{Path(path).name}: {total} Record Thread samples")
    for label, pattern in PARTS.items():
        c = outermost_count(lines, pattern)
        if c:
            print(f"  {label}: {100 * c / total:.1f}%")
