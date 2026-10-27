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
    "  interleave": r"interleaveInt16",
    "  Binary file writes": r"SequentialBlockFile::writeChannel",
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


def unresolved_count(lines, parent_pattern):
    """Samples in unresolved frames (`???`) directly inside a frame matching parent_pattern: `sample` couldn't unwind
    through the engine function (e.g. the Binary engine's SIMDConverter), so its code appears below `???` in
    RecordThread::writeData instead of below writeContinuousData[Batch] (see profile_record_thread.py)"""
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


for path in sys.argv[1:]:
    total, lines = thread_block(read(path), "Record Thread")
    unresolved = unresolved_count(lines, r"RecordThread::writeData\b")
    print(f"{Path(path).name}: {total} Record Thread samples")
    for label, pattern in PARTS.items():
        c = outermost_count(lines, pattern)
        if label.startswith("engine writes"):
            c += unresolved
            if unresolved:
                print(f"  {label}, incl. unresolved frames: {100 * c / total:.1f}%")
                print(f"    of which unresolved frames in RecordThread::writeData: {100 * unresolved / total:.1f}%")
                continue
        if c:
            print(f"  {label}: {100 * c / total:.1f}%")
