"""Paths used by the performance scripts (override with environment variables).

- GUI_BASE_DIR: the plugin-GUI checkout (default: ../../plugin-GUI relative to this repository, as in CMakeLists.txt)
- NWB_PLUGIN_SRC: the nwb-format plugin source to benchmark (default: this repository)
- PERF_WORK: directory for datasets, recordings, builds and logs (default: perf/work, not tracked)
Results are written to perf/results.
"""
import os
from pathlib import Path

PERF = Path(__file__).resolve().parent
REPO = PERF.parent
GUI = Path(os.environ.get("GUI_BASE_DIR", REPO.parent.parent / "plugin-GUI")).resolve()
PLUGIN_SRC = Path(os.environ.get("NWB_PLUGIN_SRC", REPO)).resolve()
WORK = Path(os.environ.get("PERF_WORK", PERF / "work")).resolve()
RESULTS = PERF / "results"
APP = GUI / "Build/Release/Open Ephys GUI.app/Contents/MacOS/open-ephys"
CONFIGS = Path.home() / "Library/Application Support/open-ephys/configs-api10"
