#!/usr/bin/env bash
# Build a Release (optimized) NWB plugin for performance tests without touching a Debug development setup in
# ~/Library/Application Support/open-ephys (macOS).
#
# Prerequisite: a Release build of the GUI, next to the Debug build in plugin-GUI/Build:
#   xcodebuild -project "$GUI_BASE_DIR/Build/open-ephys-GUI.xcodeproj" -scheme ALL_BUILD -configuration Release build
#
# The Release plugin and the Release HDF5 wrapper library (OpenEphysHDF5Lib) are installed into a staging directory
# ($PERF_WORK/home, used as $HOME when configuring, since the install destination is derived from $HOME) and then copied
# into the Release app: the GUI also loads plugins from <app>/Contents/PlugIns, and the plugin finds its shared libraries
# via @loader_path/../../../../shared-api10, i.e. <app>/Contents/shared-api10. (On macOS, the GUI ignores $HOME at
# runtime, so it still also tries to load a Debug plugin from ~/Library, which fails harmlessly in the Release GUI.)
#
# Environment: GUI_BASE_DIR (default ../../plugin-GUI), NWB_PLUGIN_SRC (default: this repository),
# OE_HDF5LIB_DIR (default ../OpenEphysHDF5Lib), PERF_WORK (default perf/work).
set -euo pipefail
PERF="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$PERF/.." && pwd)"
GUI="${GUI_BASE_DIR:-$REPO/../../plugin-GUI}"
PLUGIN_SRC="${NWB_PLUGIN_SRC:-$REPO}"
HDF5LIB="${OE_HDF5LIB_DIR:-$REPO/../OpenEphysHDF5Lib}"
WORK="${PERF_WORK:-$PERF/work}"
FAKEHOME="$WORK/home"
APPDIR="$FAKEHOME/Library/Application Support/open-ephys"
APP="$GUI/Build/Release/Open Ephys GUI.app"
[ -d "$APP" ] || { echo "Release GUI not found: $APP (build it first, see above)"; exit 1; }
mkdir -p "$APPDIR/plugins-api10" "$APPDIR/shared-api10"
# Show the end of the log of a failed step
run() { local log="$1"; shift; "$@" > "$log" 2>&1 || { tail -20 "$log"; echo "FAILED: $* (log: $log)"; exit 1; }; }
# One build directory per plugin source (CMake doesn't allow changing the source of a build directory)
BUILD="$WORK/nwb-format-build-$(basename "$PLUGIN_SRC")"

# The HDF5 libraries are prebuilt (same for Debug and Release); copy them from the Debug setup
cp -p "$HOME/Library/Application Support/open-ephys/shared-api10/"libhdf5*.dylib "$APPDIR/shared-api10/"
# Release build of the GUI's HDF5 wrapper library (installed into plugin-GUI/installed_libs/Release, next to Debug,
# and into the staging shared-api10 folder), needed to link the Release plugin
run "$WORK/hdf5lib-cmake.log" env HOME="$FAKEHOME" cmake -G Xcode -S "$HDF5LIB" -B "$WORK/hdf5lib-build"
run "$WORK/hdf5lib-build.log" env HOME="$FAKEHOME" xcodebuild -project "$(ls -d "$WORK/hdf5lib-build/"*.xcodeproj)" -target install -configuration Release
# Release build of the plugin in a separate build directory
run "$WORK/nwb-cmake.log" env HOME="$FAKEHOME" cmake -G Xcode -S "$PLUGIN_SRC" -B "$BUILD" -DGUI_BASE_DIR="$GUI"
run "$WORK/nwb-build.log" env HOME="$FAKEHOME" xcodebuild -project "$(ls -d "$BUILD/"*.xcodeproj)" -target install -configuration Release

# Copy the Release plugin and libraries into the Release app and ad-hoc sign them
# The plugin is named after its source directory (e.g. nwb-format-perf.bundle for a worktree); remove any other
# copy so that the GUI has a single NWB2 engine
BUNDLE="$(basename "$PLUGIN_SRC").bundle"
rm -rf "$APP/Contents/PlugIns/"nwb-format*.bundle
cp -R "$APPDIR/plugins-api10/$BUNDLE" "$APP/Contents/PlugIns/"
mkdir -p "$APP/Contents/shared-api10"
cp -p "$APPDIR/shared-api10/"*.dylib "$APP/Contents/shared-api10/"
find "$APP" -name "*.bundle" -exec codesign --force --deep --sign - {} \; 2>/dev/null
for lib in "$APP/Contents/shared-api10/"*.dylib; do codesign --force --sign - "$lib"; done
echo "Release plugin $BUNDLE from $PLUGIN_SRC ($(git -C "$PLUGIN_SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)) installed into $APP"
