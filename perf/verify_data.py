# /// script
# dependencies = ["h5py", "numpy"]
# ///
"""Compare the int16 data of an NWB recording of a synthetic dataset with the source file (looped playback).

Usage: uv run --isolated perf/verify_data.py <recording directory> <dataset directory>
(e.g. after run_perf.py --keep: $PERF_WORK/recordings/<date> and $PERF_WORK/stress_data_<channels>)
"""
import glob, sys
import h5py, numpy as np
rec, src = sys.argv[1], sys.argv[2]
f = h5py.File(glob.glob(rec + "/*/experiment*.nwb")[0], "r")
es = [g for g in f["acquisition"].values() if g.attrs.get("neurodata_type") == "ElectricalSeries"][0]
data = es["data"][:]
first = int(es["sync/sample_numbers"][0])
ch = data.shape[1]
source = np.fromfile(glob.glob(src + "/continuous/*/continuous.dat")[0], dtype="<i2").reshape(-1, ch)
rows = (first + np.arange(data.shape[0])) % source.shape[0]
diff = data != source[rows]
print(f"{data.shape} recorded; mismatching values: {int(diff.sum())}; first row in the file: {first % source.shape[0]}")
