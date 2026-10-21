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
| TTL events (one per event channel) | `/events/<stream series>.<channel>` (`EventsTable` with `line`, `state`, `full_word`, `sample_number`) |
| Text messages (Message Center) | `/events/messages` (`EventsTable` with `annotation`, `sample_number`) |
| Recording start sample numbers | `/events/sync_messages` |
| Start/stop of each recording | `/intervals/epochs` |
| Electrodes | `/general/extracellular_ephys/electrodes` |

The file is flushed when a recording stops and every 10 s during a recording, so that data is not lost if the GUI crashes; it is closed when acquisition starts again or the GUI is closed. The `NWB File` file source (File Reader) can play back NWB files written by this plugin, by earlier versions of this plugin, and by other tools (e.g., PyNWB).

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
 
