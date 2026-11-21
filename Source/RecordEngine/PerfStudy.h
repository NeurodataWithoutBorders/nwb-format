/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2024 Open Ephys

    ------------------------------------------------------------------

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef NWBPERFSTUDY_H
#define NWBPERFSTUDY_H

/* Performance study (branch perf-study, not for release): environment variables that switch the record engine to
   earlier variants of its design, and timers of the write path. Used by the benchmarks in
   NeurodataWithoutBorders/nwb_acquisition_paper (benchmarks/openephys).

   NWB2_PERF_WRITE=channel        write one channel per call, also when the GUI passes whole blocks (batch writes)
   NWB2_PERF_CACHE=default        use the HDF5 default chunk cache instead of one sized to the channel count
   NWB2_PERF_CONVERSION=juce      convert to int16 with JUCE (copyWithMultiply + convertFloatToInt16LE)
   NWB2_PERF_INTERLEAVE=strided   interleave a block channel by channel (strided writes) instead of in tiles
   NWB2_PERF_EVENTS=row           write each event when it arrives (one row per call) instead of in batches
   NWB2_PERF_FLUSH_MS=<ms>        flush interval during a recording (default 10000; 0: only when recording stops)
   NWB2_PERF_SWMR=0               don't use HDF5 SWMR mode
   NWB2_PERF_TIMERS=<file>        append the timers of each recording to <file> (one JSON line per recording)
*/

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace NWBRecording::PerfStudy
{

/** Variant of the record engine selected with the environment variables above */
struct Options
{
    bool perChannelWrites = false;
    bool defaultChunkCache = false;
    bool juceConversion = false;
    bool stridedInterleave = false;
    bool rowEvents = false;
    bool swmr = true;
    uint32_t flushIntervalMs = 10000;
    std::string timersPath;

    static Options fromEnvironment()
    {
        const auto env = [] (const char* name) -> std::string
        {
            const char* value = std::getenv (name);
            return value != nullptr ? value : "";
        };
        Options options;
        options.perChannelWrites = env ("NWB2_PERF_WRITE") == "channel";
        options.defaultChunkCache = env ("NWB2_PERF_CACHE") == "default";
        options.juceConversion = env ("NWB2_PERF_CONVERSION") == "juce";
        options.stridedInterleave = env ("NWB2_PERF_INTERLEAVE") == "strided";
        options.rowEvents = env ("NWB2_PERF_EVENTS") == "row";
        options.swmr = env ("NWB2_PERF_SWMR") != "0";
        if (! env ("NWB2_PERF_FLUSH_MS").empty())
            options.flushIntervalMs = static_cast<uint32_t> (std::stoul (env ("NWB2_PERF_FLUSH_MS")));
        options.timersPath = env ("NWB2_PERF_TIMERS");
        return options;
    }

    std::string json() const
    {
        return std::string ("\"write\": \"") + (perChannelWrites ? "channel" : "batch") + "\", \"cache\": \""
               + (defaultChunkCache ? "default" : "sized") + "\", \"conversion\": \"" + (juceConversion ? "juce" : "fast")
               + "\", \"interleave\": \"" + (stridedInterleave ? "strided" : "tiled") + "\", \"events\": \""
               + (rowEvents ? "row" : "batch") + "\", \"swmr\": "
               + (swmr ? "true" : "false") + ", \"flush_ms\": " + std::to_string (flushIntervalMs);
    }
};

/** Parts of the write path that are timed */
enum class Section
{
    Continuous, // a whole writeContinuousData or writeContinuousDataBatch call
    Convert, // float -> int16
    Interleave, // channel-major -> [sample][channel]
    DataWrite, // AqNWB write of data and timestamps
    SampleNumbers, // AqNWB write of the sample numbers
    Flush, // periodic flush
    Event, // writeEvent
    Spike, // writeSpike
};

/** Accumulated time per section, and the latencies of the continuous write calls */
class Timers
{
public:
    using Clock = std::chrono::steady_clock;

    bool enabled = false;

    void add (Section section, Clock::time_point start)
    {
        const double seconds = std::chrono::duration<double> (Clock::now() - start).count();
        auto& total = totals[section];
        total.seconds += seconds;
        total.count++;
        if (section == Section::Continuous)
            latencies.push_back (seconds);
    }

    void reset()
    {
        totals.clear();
        latencies.clear();
    }

    /** Append one JSON line with the totals and latency percentiles to path */
    void write (const std::string& path, const Options& options, int numChannels, double recordingSeconds)
    {
        if (path.empty())
            return;
        std::sort (latencies.begin(), latencies.end());
        const auto percentile = [this] (double p)
        { return latencies.empty() ? 0.0 : latencies[std::min (latencies.size() - 1, (size_t) (p / 100.0 * latencies.size()))]; };

        std::string line = "{" + options.json() + ", \"channels\": " + std::to_string (numChannels)
                           + ", \"recording_s\": " + std::to_string (recordingSeconds);
        const std::pair<Section, const char*> names[] = { { Section::Continuous, "continuous" },
                                                          { Section::Convert, "convert" },
                                                          { Section::Interleave, "interleave" },
                                                          { Section::DataWrite, "data_write" },
                                                          { Section::SampleNumbers, "sample_numbers" },
                                                          { Section::Flush, "flush" },
                                                          { Section::Event, "event" },
                                                          { Section::Spike, "spike" } };
        for (const auto& [section, name] : names)
        {
            const auto& total = totals[section];
            line += std::string (", \"") + name + "_s\": " + std::to_string (total.seconds) + ", \"" + name
                    + "_calls\": " + std::to_string (total.count);
        }
        line += ", \"continuous_p50_ms\": " + std::to_string (percentile (50) * 1e3) + ", \"continuous_p99_ms\": "
                + std::to_string (percentile (99) * 1e3) + ", \"continuous_max_ms\": "
                + std::to_string ((latencies.empty() ? 0.0 : latencies.back()) * 1e3) + "}";
        std::ofstream (path, std::ios::app) << line << "\n";
    }

private:
    struct Total
    {
        double seconds = 0.0;
        long long count = 0;
    };
    std::map<Section, Total> totals;
    std::vector<double> latencies;
};

/** Times a scope if the timers are enabled */
class ScopedTimer
{
public:
    ScopedTimer (Timers& timers, Section section) : timers (timers), section (section)
    {
        if (timers.enabled)
            start = Timers::Clock::now();
    }

    ~ScopedTimer()
    {
        if (timers.enabled)
            timers.add (section, start);
    }

private:
    Timers& timers;
    Section section;
    Timers::Clock::time_point start;
};

} // namespace NWBRecording::PerfStudy

#endif
