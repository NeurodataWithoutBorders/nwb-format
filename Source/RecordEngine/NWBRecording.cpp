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

#include <set>
#include <string>

#include "NWBRecording.h"
#include "Utils.hpp"
#include "io/nwbio_utils.hpp"

#include "../../Source/Processors/RecordNode/RecordNode.h"

using namespace NWBRecording;
using AQNWB::Types::SizeType;
using AQNWB::Types::Status;

namespace
{
/** Name of the ElectricalSeries / ElectrodeGroup for the stream of a continuous channel */
std::string getStreamName (const ContinuousChannel* channel)
{
    return channel->getSourceNodeName().toStdString() + "-"
           + std::to_string (channel->getSourceNodeId())
           + "." + channel->getStreamName().toStdString();
}

/** Scale float samples (in units of bitVolts) and convert them to int16 */
void convertToInt16 (const float* data, int numSamples, float bitVolts, std::vector<float>& scaled, std::vector<int16_t>& out)
{
    if (scaled.size() < (size_t) numSamples)
    {
        scaled.resize (numSamples);
        out.resize (numSamples);
    }

    FloatVectorOperations::copyWithMultiply (scaled.data(), data, 1.0f / (float (0x7fff) * bitVolts), numSamples);
    AudioDataConverters::convertFloatToInt16LE (scaled.data(), out.data(), numSamples);
}
} // namespace

NWBRecordEngine::NWBRecordEngine()
{
}

NWBRecordEngine::~NWBRecordEngine()
{
    closeFile();
}

RecordEngineManager* NWBRecordEngine::getEngineManager()
{
    //static factory that instantiates the engine manager, which allows to configure recording options among other things. See OriginalRecording to see how to create options for a record engine
    RecordEngineManager* man = new RecordEngineManager ("NWB2", "NWB2", &(engineFactory<NWBRecordEngine>) );
    EngineParameter* param;
    param = new EngineParameter (EngineParameter::STR, 0, "Identifier Text", String());
    man->addParameter (param);
    return man;
}

bool NWBRecordEngine::checkStatus (Status status, const String& operation)
{
    if (status != Status::Success)
    {
        LOGE ("NWB2: ", operation, " failed");
        return false;
    }
    return true;
}

void NWBRecordEngine::openFiles (File rootFolder, int experimentNumber, int recordingNumber)
{
    // Recordings within the same experiment are appended to the datasets of the open file
    if (recordingNumber > 0 && io != nullptr && io->isOpen())
    {
        startEpoch (recordingNumber);
        return;
    }

    closeFile();
    createRecordingArrays();

    // New file for each experiment, e.g. experiment1.nwb, experiment2.nwb, etc.
    String filename = rootFolder.getFullPathName() + rootFolder.getSeparatorString()
                      + "experiment" + String (experimentNumber) + ".nwb";

    io = AQNWB::createIO ("HDF5", filename.toStdString());
    if (! checkStatus (io->open (AQNWB::IO::FileMode::Overwrite), "Opening " + filename))
    {
        io.reset();
        return;
    }

    nwbfile = AQNWB::NWB::NWBFile::create (io);
    if (nwbfile == nullptr)
    {
        LOGE ("NWB2: Creating the NWBFile object failed");
        closeFile();
        return;
    }

    String identifier = identifierText.isNotEmpty() ? identifierText : String (AQNWB::generateUuid());
    std::string dataCollection = "Open Ephys GUI Version " + CoreServices::getGUIVersion().toStdString();

    bool success = checkStatus (nwbfile->initialize (identifier.toStdString(), "Recording with the Open Ephys GUI", dataCollection),
                                "NWBFile initialization");

    if (success && nwbfile->createElectrodesTable (electrodeArrays) == nullptr)
    {
        LOGE ("NWB2: Electrodes table creation failed");
        success = false;
    }

    success = success
              && checkStatus (nwbfile->createElectricalSeries (recordingArrays, recordingArraysNames, AQNWB::IO::BaseDataType::I16, esContainerIndexes),
                              "ElectricalSeries creation");

    // One SpikeEventSeries per spike channel, since the number of samples per spike may differ. All channels
    // of a spike are scaled with the same factor (see writeSpike), which is stored in data.conversion
    for (size_t i = 0; success && i < spikeRecordingArrays.size(); i++)
    {
        success = checkStatus (nwbfile->createSpikeEventSeries ({ spikeRecordingArrays[i] },
                                                                { spikeRecordingArraysNames[i] },
                                                                AQNWB::IO::BaseDataType::I16,
                                                                spikeContainerIndexes,
                                                                static_cast<SizeType> (getSpikeChannel (static_cast<int> (i))->getTotalSamples()),
                                                                false),
                               "SpikeEventSeries creation for " + String (spikeRecordingArraysNames[i]));
    }

    // Sample numbers and the epochs table must be created before recording starts (SWMR mode)
    auto recordingObjects = io->getRecordingObjects();
    for (size_t i = 0; success && i < esContainerIndexes.size(); i++)
    {
        auto dataset = createSampleNumbersDataSet (recordingObjects->getRecordingObject (esContainerIndexes[i])->getPath(),
                                                   8192,
                                                   "Sample number of each sample since the start of acquisition, counted on the clock of the data stream");
        success = dataset != nullptr;
        esSampleNumbers.push_back (dataset);
        esSampleNumbersWritten.push_back (0);
    }

    for (size_t i = 0; success && i < spikeContainerIndexes.size(); i++)
    {
        auto dataset = createSampleNumbersDataSet (recordingObjects->getRecordingObject (spikeContainerIndexes[i])->getPath(),
                                                   256,
                                                   "Sample number of the peak of each spike since the start of acquisition, counted on the clock of the data stream");
        success = dataset != nullptr;
        spikeSampleNumbers.push_back (dataset);
        spikeSampleNumbersWritten.push_back (0);
    }

    success = success && createEpochsTable() && createEventsTables();

    success = success && checkStatus (io->startRecording(), "Start recording");
    lastFlushTime = Time::getMillisecondCounter();

    if (success)
        startEpoch (recordingNumber);
    else
        closeFile();
}

std::shared_ptr<AQNWB::IO::BaseRecordingData> NWBRecordEngine::createSampleNumbersDataSet (const std::string& seriesPath,
                                                                                           SizeType chunkSize,
                                                                                           const std::string& description)
{
    // Sample numbers are stored in the (lab-specific) TimeSeries.sync group defined by the NWB schema
    const std::string syncPath = seriesPath + "/sync";
    const std::string path = syncPath + "/sample_numbers";

    try
    {
        if (! checkStatus (io->createGroup (syncPath), "Creating " + String (syncPath)))
            return nullptr;

        std::shared_ptr<AQNWB::IO::BaseRecordingData> dataset =
            io->createArrayDataSet (AQNWB::IO::ArrayDataSetConfig (AQNWB::IO::BaseDataType::I64, { 0 }, { chunkSize }), path);

        if (dataset == nullptr || ! checkStatus (io->createAttribute (description, path, "description"), "Creating " + String (path) + " description"))
            return nullptr;

        return dataset;
    }
    catch (const std::exception& e)
    {
        LOGE ("NWB2: Creating ", path, " failed: ", e.what());
        return nullptr;
    }
}

void NWBRecordEngine::startEpoch (int recordingNumber)
{
    currentRecordingNumber = recordingNumber;
    recordingHasData = false;
    recordingStartTime = 0.0;
    recordingStopTime = 0.0;
    streamStartTimes.clear();
}

bool NWBRecordEngine::createEpochsTable()
{
    // Use float64 start and stop times, since float32 is not precise enough for long recordings
    const std::string path = AQNWB::NWB::NWBFile::INTERVALS_PATH + "/epochs";
    auto specs = AQNWB::NWB::TimeIntervals::createDefaultDataSpecs (path, 100, true);
    for (auto& spec : specs)
    {
        if (spec->name == "start_time" || spec->name == "stop_time")
        {
            spec = AQNWB::NWB::VectorData::createDataSpec (
                spec->name,
                AQNWB::IO::ArrayDataSetConfig (AQNWB::IO::BaseDataType::F64, { 0 }, { 100 }),
                spec->name == "start_time" ? "Start time of the recording, in seconds."
                                           : "Stop time of the recording, in seconds.");
        }
    }

    try
    {
        epochs = nwbfile->createTimeIntervals ("epochs",
                                               "Recordings (start and stop of recording within an acquisition). "
                                               "Data of all recordings of an experiment are appended to the same datasets.",
                                               specs);
    }
    catch (const std::exception& e)
    {
        LOGE ("NWB2: Epochs table creation failed: ", e.what());
        epochs = nullptr;
    }

    if (epochs == nullptr)
        LOGE ("NWB2: Epochs table creation failed");
    return epochs != nullptr;
}

std::shared_ptr<AQNWB::NWB::EventsTable> NWBRecordEngine::createEventsTable (const std::string& name,
                                                                            const std::string& description,
                                                                            const std::string& sourceDescription,
                                                                            float timestampResolution,
                                                                            bool annotationColumn,
                                                                            const std::vector<AQNWB::NWB::DynamicTable::DataSpecPtr>& customColumns)
{
    // Use a float64 timestamp column, since float32 is not precise enough for long recordings
    auto specs = AQNWB::NWB::EventsTable::createDefaultDataSpecs (timestampResolution, false, std::nullopt, annotationColumn, 100);
    for (auto& spec : specs)
    {
        if (spec->name == "timestamp")
        {
            spec = std::make_shared<AQNWB::NWB::TimestampVectorData::DataSpec> (
                "timestamp",
                AQNWB::IO::ArrayDataSetConfig (AQNWB::IO::BaseDataType::F64, { 0 }, { 100 }),
                "Time of each event, in seconds, from the session start time.",
                timestampResolution);
        }
    }
    specs.insert (specs.end(), customColumns.begin(), customColumns.end());

    try
    {
        auto table = nwbfile->createEventsTable (name, description, sourceDescription, specs);
        if (table == nullptr)
            LOGE ("NWB2: Creating EventsTable ", name, " failed");
        return table;
    }
    catch (const std::exception& e)
    {
        LOGE ("NWB2: Creating EventsTable ", name, " failed: ", e.what());
        return nullptr;
    }
}

bool NWBRecordEngine::createEventsTables()
{
    using AQNWB::IO::ArrayDataSetConfig;
    using AQNWB::IO::BaseDataType;
    using AQNWB::NWB::VectorData;

    auto column = [] (const std::string& name, const BaseDataType& type, const std::string& description)
    { return VectorData::createDataSpec (name, ArrayDataSetConfig (type, { 0 }, { 100 }), description); };

    const std::string sampleNumberDescription =
        "Sample number of each event since the start of acquisition, counted on the clock of the data stream";

    std::set<std::string> usedNames;
    eventTables.clear();

    for (int i = 0; i < getNumRecordedEventChannels(); i++)
    {
        const EventChannel* channel = getEventChannel (i);
        EventTable eventTable;

        // Event tables are named after the ElectricalSeries of their data stream (e.g.,
        // "File Reader-100.example_data.TTL"), so that readers can associate them with the stream.
        // Events created by a downstream processor (e.g., a Phase Detector) include the processor:
        // "File Reader-100.example_data.Phase Detector-104.<channel name>"
        const bool isMessageCenter = channel->getSourceNodeName() == "Message Center";
        const String eventSource = channel->getSourceNodeName() + "-" + String (channel->getSourceNodeId());
        String streamName = eventSource + "." + channel->getStreamName();
        for (size_t s = 0; s < recordingStreamIds.size(); s++)
        {
            if (recordingStreamIds[s] == channel->getStreamId())
                streamName = recordingArraysNames[s];
        }
        const String channelName = streamName.startsWith (eventSource + ".") ? channel->getName()
                                                                              : eventSource + "." + channel->getName();
        std::string name = isMessageCenter ? std::string ("messages")
                                           : (streamName + "." + channelName).replaceCharacter ('/', '_').toStdString();
        // make sure the table names are unique
        const std::string baseName = name;
        for (int n = 2; usedNames.count (name) > 0; n++)
            name = baseName + "_" + std::to_string (n);
        usedNames.insert (name);
        eventTable.name = name;

        const float resolution = channel->getSampleRate() > 0 ? 1.0f / channel->getSampleRate() : -1.0f;
        const std::string description = channel->getDescription().toStdString();

        if (channel->getType() == EventChannel::TTL)
        {
            eventTable.type = EventTableType::TTL;
            eventTable.table = createEventsTable (
                name,
                description.empty() ? "TTL events" : description,
                "Acquisition system: TTL events from " + channel->getSourceNodeName().toStdString(),
                resolution,
                false,
                { column ("line", BaseDataType::U8, "TTL line (0-based) that changed state"),
                  column ("state", BaseDataType::U8, "New state of the TTL line (1 = on, 0 = off)"),
                  column ("full_word", BaseDataType::U64, "State of the first 64 TTL lines when the event occurred"),
                  column ("sample_number", BaseDataType::I64, sampleNumberDescription) });
        }
        else if (channel->getType() == EventChannel::TEXT)
        {
            eventTable.type = EventTableType::Text;
            eventTable.table = createEventsTable (
                name,
                isMessageCenter ? "Text messages generated during the experiment (e.g., via the Message Center)"
                                : (description.empty() ? "Text events" : description),
                isMessageCenter ? "Message Center" : channel->getSourceNodeName().toStdString(),
                resolution,
                true,
                { column ("sample_number", BaseDataType::I64, sampleNumberDescription) });
        }
        else
        {
            LOGD ("NWB2: Event channel ", channel->getName(), " has an unsupported event type and will not be recorded");
            eventTables.push_back (eventTable);
            continue;
        }

        if (eventTable.table == nullptr)
            return false;
        eventTables.push_back (eventTable);
    }

    syncMessagesTable = createEventsTable (
        "sync_messages",
        "Start of each recording for each data stream. The timestamp is the first timestamp recorded for the "
        "stream in the recording. For the 'Software Time' message, the sample_number is the system time in "
        "milliseconds since midnight Jan 1st 1970 UTC and the timestamp is the start time of the recording.",
        "Open Ephys Record Node",
        -1.0f,
        true,
        { column ("stream_id", BaseDataType::U16, "Open Ephys id of the data stream (0 for the 'Software Time' message)"),
          column ("sample_number", BaseDataType::I64, "First sample number of the stream in the recording") });

    return syncMessagesTable != nullptr;
}

void NWBRecordEngine::writeSyncMessages (bool force)
{
    if (syncMessagesTable == nullptr)
    {
        pendingSyncMessages.clear();
        return;
    }

    std::vector<SyncMessage> remaining;
    for (const auto& message : pendingSyncMessages)
    {
        // The 'Software Time' message (stream 0) refers to the start of the recording
        double timestamp = recordingStartTime;
        bool known = message.streamId == 0 && recordingHasData;
        auto startTime = streamStartTimes.find (message.streamId);
        if (startTime != streamStartTimes.end())
        {
            timestamp = startTime->second;
            known = true;
        }

        if (! known && ! force)
        {
            remaining.push_back (message);
            continue;
        }

        AQNWB::Types::RowData row = { { "timestamp", timestamp },
                                      { "annotation", message.text },
                                      { "stream_id", message.streamId },
                                      { "sample_number", message.sampleNumber } };
        checkStatus (syncMessagesTable->addRow (row), "Writing sync message '" + String (message.text) + "'");
    }
    pendingSyncMessages = remaining;
}

void NWBRecordEngine::closeFiles()
{
    // Called each time recording stops. Flush so that all data written so far is complete on disk.
    // The file stays open so that further recordings of the same experiment are appended to it;
    // it is finalized and closed when the next experiment starts or the engine is destroyed.
    if (io == nullptr || ! io->isOpen())
        return;

    // Mark the time range of this recording in the epochs table
    if (epochs != nullptr && recordingHasData)
    {
        AQNWB::Types::RowData row = {
            { "start_time", recordingStartTime },
            { "stop_time", recordingStopTime },
            { "tags", std::vector<std::string> { "recording" + std::to_string (currentRecordingNumber + 1) } }
        };
        checkStatus (epochs->addRow (row), "Adding the epoch for recording " + String (currentRecordingNumber + 1));
    }

    writeSyncMessages (true);
    recordingHasData = false;

    checkStatus (io->flush(), "Flushing the NWB file");
}

void NWBRecordEngine::closeFile()
{
    // Release our dataset handles first so that closing the file is not deferred by open HDF5 objects
    esSampleNumbers.clear();
    spikeSampleNumbers.clear();
    esSampleNumbersWritten.clear();
    spikeSampleNumbersWritten.clear();
    epochs.reset();
    eventTables.clear();
    syncMessagesTable.reset();
    pendingSyncMessages.clear();
    streamStartTimes.clear();

    if (io != nullptr && io->isOpen())
    {
        // Finalizes all recording objects. In SWMR mode this also closes the file.
        checkStatus (io->stopRecording(), "Stop recording");

        if (io->isOpen())
            checkStatus (io->close(), "Closing the NWB file");
    }

    nwbfile.reset();
    io.reset();
    writeErrorLogged = false;
    eventErrorLogged = false;
}

void NWBRecordEngine::writeContinuousData (int writeChannel,
                                           int realChannel,
                                           const float* dataBuffer,
                                           const double* timestampBuffer,
                                           int size)
{
    if (io == nullptr || writeChannel < 0 || (size_t) writeChannel >= writeChannelSeriesIndexes.size())
        return;

    const size_t seriesIndex = writeChannelSeriesIndexes[writeChannel];
    const AQNWB::Channel& channel = recordingArrays[seriesIndex][writeChannelLocalIndexes[writeChannel]];

    convertToInt16 (dataBuffer, size, channel.getBitVolts(), scaledBuffer, intBuffer);

    // Timestamps are only written together with the first channel of each ElectricalSeries
    Status status = AQNWB::IO::writeElectricalSeriesData (io->getRecordingObjects(),
                                                          esContainerIndexes[seriesIndex],
                                                          channel,
                                                          static_cast<SizeType> (size),
                                                          intBuffer.data(),
                                                          timestampBuffer);

    /* All channels of a stream share the timestamps and sample numbers, so these are written
       together with the first channel of each ElectricalSeries */
    if (writeChannelLocalIndexes[writeChannel] == 0 && size > 0)
    {
        if (sampleNumberBuffer.size() < (size_t) size)
            sampleNumberBuffer.resize (size);

        const int64 firstSampleNumber = getLatestSampleNumber (writeChannel);
        for (int i = 0; i < size; i++)
            sampleNumberBuffer[i] = firstSampleNumber + i;

        auto& written = esSampleNumbersWritten[seriesIndex];
        status = status && esSampleNumbers[seriesIndex]->writeDataBlock ({ static_cast<SizeType> (size) }, { written }, AQNWB::IO::BaseDataType::I64, sampleNumberBuffer.data());
        written += static_cast<SizeType> (size);

        // The epoch of a recording spans from its first to its last recorded timestamp
        if (! recordingHasData)
        {
            recordingStartTime = timestampBuffer[0];
            recordingHasData = true;
        }
        recordingStopTime = timestampBuffer[size - 1];

        // first timestamp of each stream in the recording (for the sync messages)
        if (streamStartTimes.emplace (recordingStreamIds[seriesIndex], timestampBuffer[0]).second && ! pendingSyncMessages.empty())
            writeSyncMessages (false);
    }

    if (status != Status::Success && ! writeErrorLogged)
    {
        LOGE ("NWB2: Writing continuous data for ", recordingArraysNames[seriesIndex], " failed");
        writeErrorLogged = true;
    }
    // Flush periodically so that the data written so far is not lost if the GUI crashes
    const uint32 now = Time::getMillisecondCounter();
    if (now - lastFlushTime >= flushIntervalMs)
    {
        checkStatus (io->flush(), "Flushing the NWB file");
        lastFlushTime = now;
    }
}

void NWBRecordEngine::writeEvent (int eventIndex, const MidiMessage& event)
{
    if (io == nullptr || eventIndex < 0 || (size_t) eventIndex >= eventTables.size())
        return;

    const EventTable& eventTable = eventTables[eventIndex];
    if (eventTable.table == nullptr)
    {
        if (! eventErrorLogged)
        {
            LOGE ("NWB2: Events of type ", (int) getEventChannel (eventIndex)->getType(), " are not supported and are not recorded");
            eventErrorLogged = true;
        }
        return;
    }

    const EventChannel* channel = getEventChannel (eventIndex);
    EventPtr eventStruct = Event::deserialize (event, channel);
    if (eventStruct == nullptr)
        return;

    AQNWB::Types::RowData row = { { "timestamp", eventStruct->getTimestampInSeconds() },
                                  { "sample_number", static_cast<int64_t> (eventStruct->getSampleNumber()) } };

    if (eventTable.type == EventTableType::TTL)
    {
        const TTLEvent* ttl = static_cast<const TTLEvent*> (eventStruct.get());
        row["line"] = static_cast<uint8_t> (ttl->getLine());
        row["state"] = static_cast<uint8_t> (ttl->getState() ? 1 : 0);
        row["full_word"] = static_cast<uint64_t> (ttl->getWord());
    }
    else
    {
        row["annotation"] = static_cast<const TextEvent*> (eventStruct.get())->getText().toStdString();
    }

    if (eventTable.table->addRow (row) != Status::Success && ! eventErrorLogged)
    {
        LOGE ("NWB2: Writing event to ", eventTable.name, " failed");
        eventErrorLogged = true;
    }
}

void NWBRecordEngine::writeTimestampSyncText (uint64 streamId, int64 sampleNumber, float sourceSampleRate, String text)
{
    // Written once the first timestamp of the stream in the recording is known
    if (io != nullptr)
    {
        pendingSyncMessages.push_back ({ static_cast<uint16> (streamId), sampleNumber, text.toStdString() });
        writeSyncMessages (false);
    }
}

void NWBRecordEngine::writeSpike (int electrodeIndex, const Spike* spike)
{
    if (io == nullptr || electrodeIndex < 0 || (size_t) electrodeIndex >= spikeContainerIndexes.size())
        return;

    const SpikeChannel* spikeChannel = getSpikeChannel (electrodeIndex);
    const SizeType numSamplesPerChannel = static_cast<SizeType> (spikeChannel->getTotalSamples());
    const SizeType numChannels = static_cast<SizeType> (spikeChannel->getNumChannels());

    convertToInt16 (spike->getDataPointer(), static_cast<int> (numSamplesPerChannel * numChannels),
                    spikeChannel->getChannelBitVolts (0), scaledBuffer, intBuffer);

    double timestamp = spike->getTimestampInSeconds();

    Status status = AQNWB::IO::writeSpikeEventData (io->getRecordingObjects(),
                                                    spikeContainerIndexes[electrodeIndex],
                                                    numSamplesPerChannel,
                                                    numChannels,
                                                    intBuffer.data(),
                                                    &timestamp);

    const int64 sampleNumber = spike->getSampleNumber();
    auto& written = spikeSampleNumbersWritten[electrodeIndex];
    status = status && spikeSampleNumbers[electrodeIndex]->writeDataBlock ({ 1 }, { written }, AQNWB::IO::BaseDataType::I64, &sampleNumber);
    written += 1;

    if (status != Status::Success && ! writeErrorLogged)
    {
        LOGE ("NWB2: Writing spike data for ", spikeRecordingArraysNames[electrodeIndex], " failed");
        writeErrorLogged = true;
    }

    // TODO - save spike metadata
}

void NWBRecordEngine::setParameter (EngineParameter& parameter)
{
    strParameter (0, identifierText);
}

void NWBRecordEngine::createRecordingArrays()
{
    electrodeArrays.clear();
    recordingArrays.clear();
    recordingArraysNames.clear();
    spikeRecordingArrays.clear();
    spikeRecordingArraysNames.clear();
    esContainerIndexes.clear();
    spikeContainerIndexes.clear();
    writeChannelSeriesIndexes.clear();
    writeChannelLocalIndexes.clear();
    recordingStreamIds.clear();

    // Electrodes table: all continuous channels entering the Record Node (not only the recorded ones),
    // grouped by stream. Rows are ordered by global channel index, so the row index of each electrode
    // equals the global index referenced by the ElectricalSeries/SpikeEventSeries electrodes regions.
    std::vector<uint16> electrodeStreamIds;
    for (int i = 0; i < recordNode->getNumOutputs(); i++)
    {
        const ContinuousChannel* channelInfo = getContinuousChannel (i);

        if (electrodeStreamIds.empty() || channelInfo->getStreamId() != electrodeStreamIds.back())
        {
            electrodeStreamIds.push_back (channelInfo->getStreamId());
            electrodeArrays.emplace_back();
        }

        auto& group = electrodeArrays.back();
        group.push_back (AQNWB::Channel (channelInfo->getName().toStdString(),
                                         getStreamName (channelInfo),
                                         electrodeArrays.size() - 1,
                                         group.size(),
                                         static_cast<SizeType> (i),
                                         1e6f,
                                         channelInfo->getSampleRate(),
                                         channelInfo->getBitVolts()));
    }

    // ElectricalSeries: one per stream with the recorded channels of that stream. The local index of
    // each channel is its position (column) within the ElectricalSeries.
    uint16 lastStreamId = 0;
    for (int ch = 0; ch < getNumRecordedContinuousChannels(); ch++)
    {
        const int globalIndex = getGlobalIndex (ch); // index across all channels entering the Record Node
        const ContinuousChannel* channelInfo = getContinuousChannel (globalIndex);

        if (recordingArrays.empty() || channelInfo->getStreamId() != lastStreamId)
        {
            recordingArrays.emplace_back();
            recordingArraysNames.push_back (getStreamName (channelInfo));
            recordingStreamIds.push_back (channelInfo->getStreamId());
            lastStreamId = channelInfo->getStreamId();
        }

        auto& group = recordingArrays.back();
        writeChannelSeriesIndexes.push_back (recordingArrays.size() - 1);
        writeChannelLocalIndexes.push_back (group.size());

        group.push_back (AQNWB::Channel (channelInfo->getName().toStdString(),
                                         recordingArraysNames.back(),
                                         recordingArrays.size() - 1,
                                         group.size(),
                                         static_cast<SizeType> (globalIndex),
                                         1e6f,
                                         channelInfo->getSampleRate(),
                                         channelInfo->getBitVolts()));
    }

    // SpikeEventSeries: one per spike channel (electrode)
    for (int i = 0; i < getNumRecordedSpikeChannels(); i++)
    {
        const SpikeChannel* spikeChannel = getSpikeChannel (i);
        std::string sourceName = spikeChannel->getSourceNodeName().toStdString() + "-"
                                 + std::to_string (spikeChannel->getSourceNodeId())
                                 + "." + spikeChannel->getStreamName().toStdString()
                                 + "." + spikeChannel->getName().toStdString();

        // As in the Binary format, all channels of a spike waveform are converted to int16 with the
        // bitVolts of the first channel (see writeSpike), so use the same factor for all channels
        AQNWB::Types::ChannelVector channelVector;
        const float spikeBitVolts = spikeChannel->getChannelBitVolts (0);
        for (int ch = 0; ch < spikeChannel->getNumChannels(); ch++)
        {
            const ContinuousChannel* sourceChannel = spikeChannel->getSourceChannels()[ch];
            channelVector.push_back (AQNWB::Channel (sourceChannel->getName().toStdString(),
                                                     getStreamName (sourceChannel),
                                                     static_cast<SizeType> (i),
                                                     static_cast<SizeType> (ch),
                                                     static_cast<SizeType> (sourceChannel->getGlobalIndex()),
                                                     1e6f,
                                                     sourceChannel->getSampleRate(),
                                                     spikeBitVolts));
        }
        spikeRecordingArrays.push_back (channelVector);
        spikeRecordingArraysNames.push_back (sourceName);
    }
}
