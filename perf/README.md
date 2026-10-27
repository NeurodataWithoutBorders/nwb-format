# Recording performance tests (NWB2 vs. Binary)

Scripts to measure how well the NWB2 record engine of this plugin keeps up with high channel counts, compared with
the Open Ephys Binary format, using Release (optimized) builds of the GUI and the plugin. They run on macOS and don't
change a Debug development setup.

## Setup

1. Build the GUI in Release mode, next to the Debug build:
   ```
   xcodebuild -project ../../plugin-GUI/Build/open-ephys-GUI.xcodeproj -scheme ALL_BUILD -configuration Release build
   ```
2. Build the Release plugin and install it into the Release GUI app (`<app>/Contents/PlugIns`, with its HDF5 libraries
   in `<app>/Contents/shared-api10`): `perf/setup_release.sh`. Rerun it after changing the plugin.

The GUI on macOS ignores `$HOME`, so a Release plugin can't be isolated in a separate home directory; installing it
into the Release app keeps the Debug plugin in `~/Library/Application Support/open-ephys/plugins-api10` untouched.
The GUI still saves its state in `~/Library/Application Support/open-ephys/configs-api10`; the scripts back up this
directory and restore it afterwards.

Environment variables (see `paths.py` and `setup_release.sh`): `GUI_BASE_DIR` (default `../../plugin-GUI`),
`NWB_PLUGIN_SRC` (the plugin source to build; default: this repository), `OE_HDF5LIB_DIR` (default
`../OpenEphysHDF5Lib`), `PERF_WORK` (datasets, recordings, builds, logs; default `perf/work`, not tracked).

## Tests

- `uv run --isolated perf/run_perf.py --channels 384 1536 3072 --repeats 3`: plays back a synthetic dataset
  (`make_stress_dataset.py`: int16, 30 kHz, noise plus a 7 Hz sine, 5 s, looped) with the File Reader into a Record
  Node (`perf_template.xml`, no other processors), acquires for 15 s without recording, records for 30 s, and counts the
  samples written to the file. The GUI stops a recording when its record buffer is 90% full (the buffer holds 300
  blocks of 1024 samples, i.e. about 10 s at 30 kHz), so a recording that is still running after 30 s and has all
  samples kept up with the data rate. `--record-channels N` records only the first N channels of the stream.
  Results: `results/results.jsonl`; `python3 perf/summarize.py` prints a table.
- `uv run --isolated perf/profile_record_thread.py --channels 384 1536 3072 4608`: records for 13 s and runs the macOS
  `sample` profiler for 10 s. Reports the share of the GUI's Record Thread spent in the engine's continuous writes
  (`writeContinuousData`/`writeContinuousDataBatch` and everything they call) and how busy the processing (audio)
  thread is. Results: `results/profile.jsonl`, call trees in `$PERF_WORK/profiles` (compressed copies in
  `results/profiles`); `python3 perf/breakdown.py <profile>` breaks down the Record Thread time. Samples in which
  `sample` couldn't unwind the stack through the engine function (reported as `???` directly inside
  `RecordThread::writeData`, e.g. for the Binary engine's SIMD conversion) are counted as engine writes as well;
  `--reanalyze <results.jsonl>` recomputes stored results from their profiles.

The process CPU time is not useful to compare the engines: the Record Thread polls its data queues without waiting
(`RecordThread::run` calls `writeData` in a loop), so it uses a full core during any recording.

## Results (2026-10-21)

**Correction (2026-10-27):** the Binary numbers first published here (0.7/3.3/7.2/10.3% at 384/1536/3072/4608
channels) were too low. For the samples in the Binary engine's float to int16 conversion (`SIMDConverter`), `sample`
reports the caller as `???` directly inside `RecordThread::writeData`, so they weren't counted as engine writes. The
analysis now counts these samples too (about 40% of Binary's write time); the NWB2 numbers are unchanged (no such
samples).

Apple M4 (10 cores, 32 GB RAM), internal SSD, macOS 15.7.2, Open Ephys GUI v1.1.0 (main) and the plugin (branch
`add-aqnwb-v0.4` at 21e67fd, AqNWB 71e34af1) in Release mode, HDF5 1.14.3. 3 runs of 30 s per data point.

| Channels | Data rate | Binary: data written | NWB2: data written | Binary: Record Thread busy writing | NWB2: Record Thread busy writing |
|---:|---:|---|---|---:|---:|
| 384 | 23 MB/s | 100% | 100% | 1.1% | 8.9% |
| 1536 | 92 MB/s | 100% | 100% | 5.8% | 34.5% |
| 3072 | 184 MB/s | 98.6-99.4% | 99.3-99.8% | 13.2% | 70.5% |
| 4608 | 276 MB/s | 45-48% | 44-45% | 18.0% | 80.6% |
| 6144 | 369 MB/s | 39-40% | 37-38% | | |

- Both engines keep up with up to 3072 channels at 30 kHz (184 MB/s): the recordings were never stopped and contain 100% (384 and 1536 channels) and 98.6-99.8% (3072 channels) of the samples, with the same file sizes. The small shortfall at 3072 channels is most likely the test source: the data is contiguous (no gaps apart from the loop points), Binary loses the most although it uses only 13% of the Record Thread, and recording 4 of the 3072 channels yields 100% (`results/check_3072.jsonl`), i.e. presumably the File Reader slowing down slightly when the writes evict the dataset from the page cache (as at 4608 channels). Part of it may be the samples discarded at the end of a recording (the Record Thread writes at most 4096 samples after stopping, https://github.com/open-ephys/plugin-GUI/issues/713).
- At 4608 and 6144 channels, the File Reader can't keep up while recording (the processing thread is ~70% busy, mostly
  in `FileReader::process`; the data is contiguous but the signal chain runs at ~45% of real time, so the record buffer
  never fills). Recording only 4 of the 4608 channels keeps up, so this is presumably the File Reader re-reading the
  dataset from the SSD because the writes evict it from the page cache: a limit of the test setup, not of the engines.
- NWB2 needs ~5-8x more Record Thread time than Binary per MB. At 3072 channels: 55% in the per-channel HDF5 writes
  (`ElectricalSeries::writeChannel`: `H5Dwrite`, copying the strided channel column into the chunks, hyperslab
  selection) and 14% in the per-channel float to int16 conversion (Binary: 6% in `SIMDConverter`, 2.8% interleaving
  and 4.2% file writes, with batch writes). Extrapolated, NWB2 would reach 100% at ~260 MB/s (~4300 channels at 30 kHz) per Record Node.
- The GUI only uses batch writes (`writeContinuousDataBatch`) for the Binary engine
  (https://github.com/open-ephys/plugin-GUI/issues/711), so the NWB2 engine receives each block one channel at a time.

## Prototype: writing all channels of a block with one HDF5 write (this branch)

The GUI calls `writeContinuousData` once per channel for each block of a stream (in channel order, with the same
number of samples). On this branch, the plugin collects the channels of a block (`SeriesBlock` in `NWBRecording.h`)
and writes the block with a single `ElectricalSeries::writeAllChannels` call when the last channel of the series
arrives, instead of one `H5Dwrite` per channel. This is what the plugin could do with `writeContinuousDataBatch` once
the GUI calls it for plugin engines (https://github.com/open-ephys/plugin-GUI/issues/711), but works with GUI v1.1.0.
If the calls arrive in an unexpected order, the plugin logs an error and falls back to per-channel writes (the
channels collected for that block are lost in this case; this doesn't happen with GUI v1.1.0).

Three versions (Record Thread busy writing, same setup as above; `results/profile_block_writes_v*.jsonl`):

| Channels | Data rate | Per-channel writes | v1: one write per block | v2: + vectorized conversion, tiled interleave | v3: + faster int16 conversion | Binary |
|---:|---:|---:|---:|---:|---:|---:|
| 384 | 23 MB/s | 8.9% | 2.2% | 3.1% | 2.3% | 1.1% |
| 1536 | 92 MB/s | 34.5% | 12.7% | 12.6% | 7.9% | 5.8% |
| 3072 | 184 MB/s | 70.5% | 27.6% | 23.1% | 16.2% | 13.2% |
| 4608 | 276 MB/s | 80.6% | 29.9% | 28.2% | 19.7% | 18.0% |

- v1: convert each channel straight into its column of the interleaved block (scalar, strided writes).
- v2: convert each channel with the existing JUCE conversion into a channel-major buffer, then interleave the block in
  64 x 64 tiles.
- v3: replace the JUCE conversion (`AudioDataConverters::convertFloatToInt16LE`, a scalar loop in double precision)
  by a loop the compiler vectorizes, with the same arithmetic (float scaling, clipping in double, rounding to nearest
  with ties to even as `juce::roundToInt`); identical results for 1.7e8 random values including halves and
  out-of-range values.
- The recorded data is bit-identical to the source dataset (all 115 million values of a 10 s, 384-channel recording).
  With v3, the recordings contain the same share of samples as the other variants (see the comparison below).
- With v3, the HDF5 write itself takes ~3% of the Record Thread at 3072 channels; NWB2 needs ~1.2x the Record Thread
  time of Binary (was ~5x). Extrapolated, a Record Node could write ~1 GB/s of NWB2 data on this machine (if the source
  keeps up). At 4608 channels the data rate is still limited by the File Reader (see above).

## Comparison: NWB2 (PR), NWB2 with block writes (v3 prototype), Binary

NWB2 (PR): branch `add-aqnwb-v0.4` at 21e67fd; NWB2 (v3): this branch. Release builds, same setup as above.

Record Thread time spent in the engine's continuous data writes (share of the 1 ms samples of the `sample` profiler in
which the Record Thread is in `writeContinuousData`/`writeContinuousDataBatch`, including conversion, HDF5/file writes
and flushes, and the unresolved engine frames described above; the thread never sleeps, so this is the share of real time spent writing, and 100% means the engine can't
keep up):

| Channels | Data rate | NWB2 (PR) | NWB2 (v3) | Binary |
|---:|---:|---:|---:|---:|
| 384 | 23 MB/s | 8.9% | 2.3% | 1.1% |
| 1536 | 92 MB/s | 34.5% | 7.9% | 5.8% |
| 3072 | 184 MB/s | 70.5% | 16.2% | 13.2% |
| 4608 | 276 MB/s* | 80.6% | 19.7% | 18.0% |

Samples written in 30 s recordings (range over 3 runs; the recording was never stopped by the GUI):

| Channels | Data rate | NWB2 (PR) | NWB2 (v3) | Binary |
|---:|---:|---|---|---|
| 384 | 23 MB/s | 100% | 100% | 100% |
| 1536 | 92 MB/s | 99.8-100% | 100% | 99.9-100% |
| 3072 | 184 MB/s | 99.3-99.8% | 99.6-100% | 98.6-99.4% |
| 4608 | 276 MB/s* | 44.1-44.6% | 44.9-46.1% | 45.1-47.7% |
| 6144 | 369 MB/s* | 37.4-38.1% | 39.0-39.9% | 38.6-40.0% |

\* Above 3072 channels, the File Reader can't keep up while recording (see above), so these rows show the limit of
the test source, not of the engines.

## writeContinuousDataBatch (branch `batch-writes`, 2026-10-26)

The branch `batch-writes` (on top of `add-aqnwb-v0.4`) implements `RecordEngine::writeContinuousDataBatch`, which the GUI calls with all recorded channels of a stream for each block: the plugin converts the channels with the existing JUCE conversion, interleaves them in 64 x 64 tiles and writes the block with one `writeAllChannels` call (as the v2 prototype above, but without collecting the per-channel calls). The GUI uses batch writes only for the Binary engine (https://github.com/open-ephys/plugin-GUI/issues/711), so the test used a local GUI change that also enables them for NWB2 if the environment variable `OE_NWB2_BATCH_WRITES` is set (`RecordThread::setEngine`, `RecordThread.cpp`).

Record Thread busy writing (Release, same setup as above; `results/profile_batch_api.jsonl`, `results/profile_batch_branch_perchannel.jsonl`):

| Channels | Data rate | NWB2, one write per channel (branch, unmodified GUI) | NWB2, writeContinuousDataBatch | Binary |
|---:|---:|---:|---:|---:|
| 384 | 23 MB/s | 8.6% | 3.1% | 1.1% |
| 1536 | 92 MB/s | 35.0% | 12.9% | 5.8% |
| 3072 | 184 MB/s | 71.8% | 23.8% | 13.2% |

The per-channel path of the branch matches `add-aqnwb-v0.4` (8.9/34.5/70.5%). At 3072 channels, the batch path spends 15.8% in the JUCE float to int16 conversion (`copyWithMultiply` + `convertFloatToInt16LE`), 3.6% in the HDF5 write of data and timestamps, and the rest in interleaving and sample numbers. The conversion is independent of #711: the vectorizable loop of the v3 prototype lowered the batch path to 16.2%; the GUI's `SIMDConverter`, which takes 6.0% of the Record Thread in the Binary profile for the same data, would presumably lower it to ~14%, i.e. about Binary's 13.2% (an estimate, not measured: `SIMDConverter` is not available to plugins).
