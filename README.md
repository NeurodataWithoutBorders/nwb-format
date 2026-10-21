# NWB Format

![header-image](Resources/header-image.png)

A Record Engine plugin for the Open Ephys GUI based on the [Neurodata Without Borders 2.X specification](https://nwb-schema.readthedocs.io/en/latest/format_release_notes.html).

## Installation

This plugin can be added via the Open Ephys GUI's built-in Plugin Installer. First, make sure there are no Record Nodes in your signal chain. Then, press **ctrl-P** or **⌘P** to open the Plugin Installer, browse to the "NWB Format" plugin, and click the "Install" button. The next time you add a Record Node to the signal chain, `NWB2` will be available in the data format drop-down menu.

## Usage

The specifications of NWB files written by the Open Ephys GUI are available [here](https://open-ephys.github.io/gui-docs/User-Manual/Recording-data/NWB-format.html).

The plugin uses [AqNWB](https://github.com/NeurodataWithoutBorders/aqnwb) to write NWB 2.11 files. Each experiment (start/stop of acquisition) is written to a separate file `experiment<N>.nwb`; recordings within an experiment are appended to the same datasets:

| Data | Location in the NWB file |
|---|---|
| Continuous data (one per stream) | `/acquisition/<source>-<id>.<stream>` (`ElectricalSeries`), sample numbers in `sync/sample_numbers` |
| Spikes (one per electrode) | `/acquisition/<source>-<id>.<stream>.<electrode>` (`SpikeEventSeries`), sample numbers in `sync/sample_numbers` |
| TTL events (one per event channel) | `/events/<processor>-<id>.<stream>.TTL` (`TTL_1`, `TTL_2`, ... for further channels of the same processor and stream; named as the event folders of the Binary format). `EventsTable` with `line`, `state`, `full_word`, `sample_number` |
| Text messages (Message Center) | `/events/messages` (`EventsTable` with `annotation`, `sample_number`) |
| Recording start sample numbers | `/events/sync_messages` |
| Start/stop of each recording | `/intervals/epochs` |
| Electrodes | `/general/extracellular_ephys/electrodes` |

All objects are created when the file is created (the file is written in HDF5 SWMR mode, which doesn't allow adding objects during a recording), so an event channel that never produces an event leaves an empty `EventsTable`. NWB Inspector reports these with `check_empty_table`; use `nwbinspector --ignore check_empty_table` to skip this check. (NWB Inspector 0.7.2 also reports errors in `check_column_binary_capability`, `check_table_values_for_dict` and `check_col_not_nan` for empty tables; this is fixed in NWB Inspector after 0.7.2, see NeurodataWithoutBorders/nwbinspector#729.)

The file is flushed when a recording stops and every 10 s during a recording, so that data is not lost if the GUI crashes; it is closed when acquisition starts again or the GUI is closed. The `NWB File` file source (File Reader) can play back NWB files written by this plugin, by earlier versions of this plugin, and by other tools (e.g., PyNWB).

### Session and subject metadata

NWB files should describe the session and the subject (DANDI requires a subject with `subject_id`, `species`, `sex` and `age` or `date_of_birth`). There are two ways to add this metadata:

**1. A metadata file (before recording).** If a file named `nwb_metadata.json` exists in the Record Node directory, the recording directory, or the directory selected for recordings in the GUI (searched in this order), the plugin writes its contents to each new NWB file. The file uses the same structure as [NeuroConv](https://neuroconv.readthedocs.io) metadata:

```json
{
  "NWBFile": {
    "session_description": "Recording in V1 during visual stimulation",
    "experiment_description": "...",
    "experimenter": ["Last, First"],
    "institution": "...",
    "lab": "...",
    "keywords": ["V1", "Neuropixels"]
  },
  "Subject": {
    "subject_id": "mouse01",
    "species": "Mus musculus",
    "sex": "M",
    "age": "P90D"
  }
}
```

Supported `NWBFile` fields: `session_description`, `experiment_description`, `institution`, `lab`, `protocol`, `notes`, `surgery`, `virus`, `pharmacology`, `slices`, `experimenter`, `keywords` and `related_publications` (the last three are lists of strings). Supported `Subject` fields: `subject_id`, `species`, `sex`, `age` (ISO 8601 duration, e.g. `P90D`), `age__reference`, `date_of_birth` (ISO 8601), `description`, `genotype`, `strain` and `weight`. The GUI log reports which metadata file was used, and any unsupported fields. The file is read when an NWB file is created (at the first recording of each experiment), so changes apply to the next experiment.

**2. After recording.** NWB files can be edited once they are closed (when the next experiment starts or the GUI quits), e.g. with [PyNWB](https://pynwb.readthedocs.io):

```python
from pynwb import NWBHDF5IO
from pynwb.file import Subject

with NWBHDF5IO("experiment1.nwb", "a") as io:
    nwbfile = io.read()
    nwbfile.subject = Subject(subject_id="mouse01", species="Mus musculus", sex="M", age="P90D")
    io.write(nwbfile)
```

or with [NWB GUIDE](https://nwb-guide.readthedocs.io).

## Building from source

First, follow the instructions on [this page](https://open-ephys.github.io/gui-docs/Developer-Guide/Compiling-the-GUI.html) to build the Open Ephys GUI.

This plugin depends on the [OpenEphysHDF5Lib](https://github.com/open-ephys-plugins/OpenEphysHDF5Lib) common library. Make sure you build and install that first before you proceed with building NWBFormat plugin. The plugin requires a GUI with plugin API version 10 (GUI v1.x).

[AqNWB](https://github.com/NeurodataWithoutBorders/aqnwb) is included as a git submodule in `Source/aqnwb` and is compiled together with the plugin (no separate installation is needed). Clone this repository with its submodules into a directory at the same level as the `plugin-GUI`:

```bash
git clone --recurse-submodules https://github.com/open-ephys-plugins/nwb-format.git
```

(or run `git submodule update --init` in an existing clone), e.g.:
 
```
Code
├── plugin-GUI
│   ├── Build
│   ├── Source
│   └── ...
├── OEPlugins
│   └── nwb-format
│       ├── Build
│       ├── Source
│       └── ...
```

### Windows

**Requirements:** [Visual Studio](https://visualstudio.microsoft.com/) and [CMake](https://cmake.org/install/)

From the `Build` directory, enter:

```bash
cmake -G "Visual Studio 17 2022" -A x64 ..
```

Next, launch Visual Studio and open the `OE_PLUGIN_nwb-format.sln` file that was just created. Select the appropriate configuration (Debug/Release) and build the solution.

Selecting the `INSTALL` project and manually building it will copy the `.dll` and any other required files into the GUI's `plugins` directory. The next time you launch the GUI from Visual Studio, `NWB2` should appear as an data format option in the Record Node.


### Linux

**Requirements:** [CMake](https://cmake.org/install/)

From the `Build` directory, enter:

```bash
cmake -G "Unix Makefiles" ..
cd Debug
make -j
make install
```

This will build the plugin and copy the `.so` file into the GUI's `plugins` directory. The next time you launch the compiled version of the GUI, `NWB2` should appear as an data format option in the Record Node.


### macOS

**Requirements:** [Xcode](https://developer.apple.com/xcode/) and [CMake](https://cmake.org/install/)

From the `Build` directory, enter:

```bash
cmake -G "Xcode" ..
```

Next, launch Xcode and open the `nwb-format.xcodeproj` file that now lives in the “Build” directory.

Running the `ALL_BUILD` scheme will compile the plugin; running the `INSTALL` scheme will install the `.bundle` file to `/Users/<username>/Library/Application Support/open-ephys/plugins-api10`. `NWB2` should now appear as an data format option in the Record Node.

**Note:** installing modifies the bundle and invalidates its code signature, and macOS refuses to load unsigned bundles (the GUI then crashes with "Code Signature Invalid" while loading plugins). Sign the installed bundle (ad hoc) after installing:

```bash
codesign --force --deep --sign - ~/Library/Application\ Support/open-ephys/plugins-api10/nwb-format.bundle
```


### Attribution

This plugin, along with the OpenEphysHDF5Lib, were developed collaboratively by Aaron Cuevas Lopez, Pavel Kulik, and Josh Siegle. It is based on the [NWB Format Specification](https://nwb-schema.readthedocs.io/en/latest/format_release_notes.html).
 
