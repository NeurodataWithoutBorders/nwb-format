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

#include "NWBFileSource.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Utils.hpp"

using AQNWB::IO::BaseDataType;
using AQNWB::IO::DataBlockGeneric;
using AQNWB::Types::SizeArray;
using AQNWB::Types::SizeType;

namespace
{
/** Convert the values of a numeric data block to the given type */
template <typename T>
std::vector<T> convertValues (const DataBlockGeneric& block)
{
    auto convert = [&block] (auto typeTag)
    {
        using V = decltype (typeTag);
        const auto& values = std::any_cast<const std::vector<V>&> (block.data);
        return std::vector<T> (values.begin(), values.end());
    };

    switch (block.baseDataType.type)
    {
        case BaseDataType::T_U8:
            return convert (uint8_t());
        case BaseDataType::T_U16:
            return convert (uint16_t());
        case BaseDataType::T_U32:
            return convert (uint32_t());
        case BaseDataType::T_U64:
            return convert (uint64_t());
        case BaseDataType::T_I8:
            return convert (int8_t());
        case BaseDataType::T_I16:
            return convert (int16_t());
        case BaseDataType::T_I32:
            return convert (int32_t());
        case BaseDataType::T_I64:
            return convert (int64_t());
        case BaseDataType::T_F32:
            return convert (float());
        case BaseDataType::T_F64:
            return convert (double());
        default:
            throw std::runtime_error ("unsupported (non-numeric) data type");
    }
}

/** Last component of a path */
std::string getName (const std::string& path)
{
    return path.substr (path.find_last_of ('/') + 1);
}

/** Time series that store TTL events in files written by earlier versions of the plugin end with ".TTL" */
const std::string legacyTTLSuffix = ".TTL";
} // namespace

bool NWBFileSource::isDataset (const std::string& path) const
{
    // check existence first: getStorageObjectType reports an HDF5 error for paths that do not exist
    return io->objectExists (path) && io->getStorageObjectType (path) == AQNWB::Types::StorageObjectType::Dataset;
}

NWBFileSource::NWBFileSource() : samplePos (0)
{
}

NWBFileSource::~NWBFileSource()
{
    if (io != nullptr && io->isOpen())
        io->close();
}

bool NWBFileSource::open (File file)
{
    try
    {
        io = AQNWB::createIO ("HDF5", file.getFullPathName().toStdString());
        if (io->open (AQNWB::IO::FileMode::ReadOnly) != AQNWB::Types::Status::Success)
        {
            LOGE ("NWBFileSource: unable to open ", file.getFullPathName());
            io.reset();
            return false;
        }

        if (! io->objectExists ("/acquisition"))
        {
            LOGE ("NWBFileSource: ", file.getFullPathName(), " has no /acquisition group");
            io->close();
            io.reset();
            return false;
        }
        return true;
    }
    catch (const std::exception& e)
    {
        LOGE ("NWBFileSource: unable to open ", file.getFullPathName(), ": ", e.what());
        io.reset();
        return false;
    }
}

std::vector<double> NWBFileSource::readDoubles (const std::string& path, int64 start, int64 count) const
{
    if (count < 0)
        return convertValues<double> (io->readDataset (path));
    return convertValues<double> (io->readDataset (path, { static_cast<SizeType> (start) }, { static_cast<SizeType> (count) }));
}

std::vector<int64> NWBFileSource::readInt64s (const std::string& path, int64 start, int64 count) const
{
    if (count < 0)
        return convertValues<int64> (io->readDataset (path));
    return convertValues<int64> (io->readDataset (path, { static_cast<SizeType> (start) }, { static_cast<SizeType> (count) }));
}

std::vector<std::string> NWBFileSource::readStrings (const std::string& path) const
{
    return std::any_cast<std::vector<std::string>> (io->readDataset (path).data);
}

std::vector<NWBFileSource::Segment> NWBFileSource::readSegments (const std::string& sampleNumbersPath, int64 numRows)
{
    // Read in chunks to avoid loading all sample numbers of long recordings into memory
    std::vector<Segment> segments;
    const int64 chunkSize = 1 << 20;
    for (int64 start = 0; start < numRows; start += chunkSize)
    {
        const auto sampleNumbers = readInt64s (sampleNumbersPath, start, std::min (chunkSize, numRows - start));
        for (size_t i = 0; i < sampleNumbers.size(); i++)
        {
            const int64 row = start + static_cast<int64> (i);
            if (! segments.empty())
            {
                Segment& last = segments.back();
                if (sampleNumbers[i] == last.firstSampleNumber + last.numRows)
                {
                    last.numRows++;
                    continue;
                }
            }
            segments.push_back ({ row, sampleNumbers[i], 1 });
        }
    }
    return segments;
}

int64 NWBFileSource::getRow (const StreamInfo& stream, int64 sampleNumber)
{
    if (stream.segments.empty())
        return (sampleNumber >= 0 && sampleNumber < stream.numSamples) ? sampleNumber : -1;

    for (const auto& segment : stream.segments)
    {
        if (sampleNumber >= segment.firstSampleNumber && sampleNumber < segment.firstSampleNumber + segment.numRows)
            return segment.firstRow + (sampleNumber - segment.firstSampleNumber);
    }
    return -1;
}

bool NWBFileSource::readStreamInfo (const std::string& path, StreamInfo& stream, RecordInfo& info)
{
    const std::string dataPath = path + "/data";
    const SizeArray shape = io->getStorageObjectShape (dataPath);
    if (shape.empty() || shape.size() > 2)
    {
        LOGE ("NWBFileSource: ", path, " has unsupported data dimensions and is skipped");
        return false;
    }

    stream.path = path;
    stream.numSamples = static_cast<int64> (shape[0]);
    stream.numChannels = shape.size() > 1 ? static_cast<int> (shape[1]) : 1;

    // Scaling to microvolts: volts = data * conversion * channel_conversion + offset
    float conversion = 1.0f;
    if (io->attributeExists (dataPath + "/conversion"))
        conversion = convertValues<float> (io->readAttribute (dataPath + "/conversion"))[0];
    float offset = 0.0f;
    if (io->attributeExists (dataPath + "/offset"))
        offset = convertValues<float> (io->readAttribute (dataPath + "/offset"))[0];
    std::vector<double> channelConversion (stream.numChannels, 1.0);
    if (io->objectExists (path + "/channel_conversion"))
        channelConversion = readDoubles (path + "/channel_conversion");
    if (static_cast<int> (channelConversion.size()) != stream.numChannels)
    {
        LOGE ("NWBFileSource: ", path, " has an invalid channel_conversion and is skipped");
        return false;
    }
    // Files written by earlier versions of the plugin (identified by their "sync" dataset) store the
    // conversion factor both in data.conversion and in channel_conversion; as in the original reader,
    // only channel_conversion is applied for these files
    const bool legacyLayout = isDataset (path + "/sync");
    if (legacyLayout)
        conversion = 1.0f;
    stream.scale.resize (stream.numChannels);
    for (int ch = 0; ch < stream.numChannels; ch++)
        stream.scale[ch] = static_cast<float> (conversion * channelConversion[ch] * 1e6);
    stream.offset = offset * 1e6f;

    // Sample rate: from starting_time.rate, or estimated from the timestamps
    info.sampleRate = -1.0f;
    if (io->objectExists (path + "/starting_time") && io->attributeExists (path + "/starting_time/rate"))
    {
        info.sampleRate = convertValues<float> (io->readAttribute (path + "/starting_time/rate"))[0];
    }
    else if (io->objectExists (path + "/timestamps"))
    {
        const int64 n = std::min<int64> (stream.numSamples, 1001);
        const auto timestamps = readDoubles (path + "/timestamps", 0, n);
        std::vector<double> diffs;
        for (size_t i = 1; i < timestamps.size(); i++)
            diffs.push_back (timestamps[i] - timestamps[i - 1]);
        if (! diffs.empty())
        {
            std::nth_element (diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
            const double medianInterval = diffs[diffs.size() / 2];
            if (medianInterval > 0)
                info.sampleRate = static_cast<float> (1.0 / medianInterval);
        }
    }
    if (info.sampleRate <= 0)
    {
        LOGE ("NWBFileSource: unable to determine the sample rate of ", path, "; it is skipped");
        return false;
    }

    // Sample numbers: sync/sample_numbers, or the "sync" dataset written by earlier versions of the plugin
    if (io->objectExists (path + "/sync") && io->objectExists (path + "/sync/sample_numbers"))
        stream.segments = readSegments (path + "/sync/sample_numbers", stream.numSamples);
    else if (isDataset (path + "/sync"))
        stream.segments = readSegments (path + "/sync", stream.numSamples);
    info.startSampleNumber = stream.segments.empty() ? 0 : stream.segments[0].firstSampleNumber;

    // Channel types were stored by earlier versions of the plugin
    std::vector<int64> channelTypes (stream.numChannels, 0);
    if (io->objectExists (path + "/channel_type"))
    {
        auto types = readInt64s (path + "/channel_type");
        if (static_cast<int> (types.size()) == stream.numChannels)
            channelTypes = types;
    }

    info.name = getName (path);
    info.numSamples = stream.numSamples;
    for (int ch = 0; ch < stream.numChannels; ch++)
    {
        RecordedChannelInfo c;
        c.name = "CH" + String (ch);
        c.bitVolts = stream.scale[ch];
        c.type = static_cast<uint8> (channelTypes[ch]);
        info.channels.add (c);
    }
    return true;
}

void NWBFileSource::fillRecordInfo()
{
    try
    {
        // ElectricalSeries in /acquisition (sorted by path for a deterministic order)
        auto types = io->findTypes ("/acquisition", { "core::ElectricalSeries" }, AQNWB::IO::SearchMode::STOP_ON_TYPE);
        std::vector<std::string> paths;
        for (const auto& [path, type] : types)
            paths.push_back (path);
        std::sort (paths.begin(), paths.end());

        for (const auto& path : paths)
        {
            try
            {
                StreamInfo stream;
                RecordInfo info;
                if (readStreamInfo (path, stream, info))
                {
                    streams.push_back (stream);
                    infoArray.add (info);
                    numRecords++;
                }
            }
            catch (const std::exception& e)
            {
                LOGE ("NWBFileSource: unable to read ", path, ": ", e.what());
            }
        }

        readEventsTables();

        // Text messages written by earlier versions of the plugin (AnnotationSeries with a "sync" dataset)
        if (! streams.empty() && isDataset ("/acquisition/messages/data") && isDataset ("/acquisition/messages/sync")
            && eventInfoMap.find ("MessageCenter") == eventInfoMap.end())
        {
            try
            {
                const auto text = readStrings ("/acquisition/messages/data");
                const auto sampleNumbers = readInt64s ("/acquisition/messages/sync");
                for (size_t i = 0; i < text.size() && i < sampleNumbers.size(); i++)
                {
                    const int64 row = getRow (streams[0], sampleNumbers[i]);
                    if (row >= 0)
                        addEvent ("MessageCenter", 0, 0, row, String (text[i]));
                }
            }
            catch (const std::exception& e)
            {
                LOGE ("NWBFileSource: unable to read /acquisition/messages: ", e.what());
            }
        }

        // TTL events written by earlier versions of the plugin
        auto legacy = io->findTypes ("/acquisition", { "core::TimeSeries" }, AQNWB::IO::SearchMode::STOP_ON_TYPE);
        for (const auto& [path, type] : legacy)
        {
            const std::string name = getName (path);
            if (name.size() > legacyTTLSuffix.size()
                && name.compare (name.size() - legacyTTLSuffix.size(), legacyTTLSuffix.size(), legacyTTLSuffix) == 0)
                readLegacyEvents (path);
        }
    }
    catch (const std::exception& e)
    {
        LOGE ("NWBFileSource: unable to read the file structure: ", e.what());
    }
}

void NWBFileSource::addEvent (const String& streamName, int16 channel, int16 state, int64 row, const String& text)
{
    EventInfo& info = eventInfoMap[streamName];
    info.channels.push_back (channel);
    info.channelStates.push_back (state);
    info.sampleNumbers.push_back (row);
    info.text.push_back (text);
}

void NWBFileSource::readEventsTables()
{
    if (! io->objectExists ("/events") || streams.empty())
        return;

    auto tables = io->findTypes ("/events", { "core::EventsTable" }, AQNWB::IO::SearchMode::STOP_ON_TYPE);
    for (const auto& [path, type] : tables)
    {
        const std::string name = getName (path);
        try
        {
            if (! io->objectExists (path + "/sample_number"))
            {
                LOGD ("NWBFileSource: ", path, " has no sample_number column; its events are not loaded");
                continue;
            }

            // Text events (Message Center); the sample numbers refer to the main (first) stream
            if (name == "messages" && io->objectExists (path + "/annotation"))
            {
                const auto sampleNumbers = readInt64s (path + "/sample_number");
                const auto text = readStrings (path + "/annotation");
                for (size_t i = 0; i < sampleNumbers.size() && i < text.size(); i++)
                {
                    const int64 row = getRow (streams[0], sampleNumbers[i]);
                    if (row >= 0)
                        addEvent ("MessageCenter", 0, 0, row, String (text[i]));
                }
                continue;
            }

            // TTL events: the table name starts with the name of the ElectricalSeries of its stream
            if (! io->objectExists (path + "/line") || ! io->objectExists (path + "/state"))
                continue;

            int streamIndex = -1;
            for (int s = 0; s < numRecords; s++)
            {
                const std::string prefix = infoArray[s].name.toStdString() + ".";
                if (name.compare (0, prefix.size(), prefix) == 0)
                    streamIndex = s;
            }
            if (streamIndex < 0)
            {
                LOGD ("NWBFileSource: no data stream found for ", path, "; its events are not loaded");
                continue;
            }

            const auto sampleNumbers = readInt64s (path + "/sample_number");
            const auto lines = readInt64s (path + "/line");
            const auto states = readInt64s (path + "/state");
            for (size_t i = 0; i < sampleNumbers.size() && i < lines.size() && i < states.size(); i++)
            {
                const int64 row = getRow (streams[streamIndex], sampleNumbers[i]);
                if (row >= 0)
                    addEvent (infoArray[streamIndex].name, static_cast<int16> (lines[i] + 1), states[i] > 0 ? 1 : 0, row, String());
            }
        }
        catch (const std::exception& e)
        {
            LOGE ("NWBFileSource: unable to read events from ", path, ": ", e.what());
        }
    }
}

void NWBFileSource::readLegacyEvents (const std::string& path)
{
    // Earlier versions of the plugin stored TTL events as a TimeSeries "<stream>.TTL" with
    // data = +/-(line + 1) and the sample numbers in a "sync" dataset
    const std::string name = getName (path);
    const String streamName = name.substr (0, name.size() - legacyTTLSuffix.size());
    // The series is named after the event source (e.g. "Network Events-108.example_data.TTL"); if there is
    // no ElectricalSeries with this name, associate it by the stream name (part after the source) if unique
    int index = -1;
    for (int s = 0; s < numRecords; s++)
    {
        if (infoArray[s].name == streamName)
            index = s;
    }
    if (index < 0)
    {
        auto getStreamPart = [] (const String& name) { return name.fromFirstOccurrenceOf (".", false, false); };
        int matches = 0;
        for (int s = 0; s < numRecords; s++)
        {
            if (getStreamPart (infoArray[s].name) == getStreamPart (streamName))
            {
                index = s;
                matches++;
            }
        }
        if (matches != 1)
            index = -1;
    }
    if (index < 0 || ! isDataset (path + "/sync"))
        return;

    try
    {
        const auto states = readInt64s (path + "/data");
        const auto sampleNumbers = readInt64s (path + "/sync");
        for (size_t i = 0; i < states.size() && i < sampleNumbers.size(); i++)
        {
            const int64 row = getRow (streams[index], sampleNumbers[i]);
            if (row >= 0)
                addEvent (infoArray[index].name, static_cast<int16> (std::abs (states[i])), states[i] > 0 ? 1 : 0, row, String());
        }
    }
    catch (const std::exception& e)
    {
        LOGE ("NWBFileSource: unable to read events from ", path, ": ", e.what());
    }
}

void NWBFileSource::updateActiveRecord (int index)
{
    samplePos = 0;
    currentStream = infoArray[index].name;
}

void NWBFileSource::seekTo (int64 sample)
{
    samplePos = sample % getActiveNumSamples();
}

int NWBFileSource::readData (float* buffer, int nSamples)
{
    const int record = activeRecord.get();
    if (io == nullptr || record < 0 || record >= static_cast<int> (streams.size()))
        return 0;

    const StreamInfo& stream = streams[record];
    const int64 samplesToRead = std::min<int64> (nSamples, stream.numSamples - samplePos);
    if (samplesToRead <= 0)
        return 0;

    try
    {
        const std::string dataPath = stream.path + "/data";
        const DataBlockGeneric block = stream.numChannels > 1 || io->getStorageObjectShape (dataPath).size() > 1
                                           ? io->readDataset (dataPath,
                                                              { static_cast<SizeType> (samplePos), 0 },
                                                              { static_cast<SizeType> (samplesToRead), static_cast<SizeType> (stream.numChannels) })
                                           : io->readDataset (dataPath, { static_cast<SizeType> (samplePos) }, { static_cast<SizeType> (samplesToRead) });
        const auto values = convertValues<float> (block);

        for (int64 i = 0; i < samplesToRead * stream.numChannels; i++)
            buffer[i] = values[i] * stream.scale[i % stream.numChannels] + stream.offset;

        samplePos += samplesToRead;
        return static_cast<int> (samplesToRead);
    }
    catch (const std::exception& e)
    {
        LOGE ("NWBFileSource: reading data from ", stream.path, " failed: ", e.what());
        return 0;
    }
}

void NWBFileSource::processEventData (EventInfo& eventInfo, int64 start, int64 stop)
{
    // The File Reader loops over the data; events are reported for all loops overlapping [start, stop)
    const int64 numSamples = getActiveNumSamples();
    if (numSamples <= 0 || stop <= start)
        return;

    const int64 loopStart = start - (start % numSamples);

    for (const String& key : { currentStream, String ("MessageCenter") })
    {
        auto it = eventInfoMap.find (key);
        if (it == eventInfoMap.end())
            continue;

        const EventInfo& info = it->second;
        for (int64 loop = loopStart; loop < stop; loop += numSamples)
        {
            for (size_t i = 0; i < info.sampleNumbers.size(); i++)
            {
                const int64 sampleNumber = loop + info.sampleNumbers[i];
                if (sampleNumber >= start && sampleNumber < stop)
                {
                    eventInfo.channels.push_back (info.channels[i] - 1);
                    eventInfo.channelStates.push_back (info.channelStates[i]);
                    eventInfo.sampleNumbers.push_back (sampleNumber);
                    eventInfo.text.push_back (info.text[i]);
                }
            }
        }
    }
}

bool NWBFileSource::isReady()
{
    return true;
}
