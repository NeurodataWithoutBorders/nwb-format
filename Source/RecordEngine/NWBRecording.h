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

#ifndef NWBRECORDING_H
#define NWBRECORDING_H

#include <RecordingLib.h>

#include <map>

#include "Channel.hpp"
#include "io/BaseIO.hpp"
#include "nwb/NWBFile.hpp"
#include "nwb/epoch/TimeIntervals.hpp"
#include "nwb/event/EventsTable.hpp"

typedef Array<const ContinuousChannel*> ContinuousGroup;

namespace NWBRecording
{

/**

        Record Engine that writes data into NWB 2.0 format

     */
class NWBRecordEngine : public RecordEngine
{
public:
    /** Constructor */
    NWBRecordEngine();

    /** Destructor */
    ~NWBRecordEngine();

    /** Launches the manager for this engine */
    static RecordEngineManager* getEngineManager();

    /** Returns a (hopefully unique) string identifier for this engine */
    String getEngineId() const override { return "NWB2"; }

    /** Called when recording starts to open all needed files */
    void openFiles (File rootFolder, int experimentNumber, int recordingNumber) override;

    /** Called when recording stops to close all files and do all the necessary cleanup */
    void closeFiles() override;

    /** Write continuous data for a channel, including synchronized float timestamps for each sample */
    void writeContinuousData (int writeChannel,
                              int realChannel,
                              const float* dataBuffer,
                              const double* timestampBuffer,
                              int size) override;

    /** Write a single event to disk (TTL or TEXT) */
    void writeEvent (int eventIndex, const MidiMessage& event) override;

    /** Write a spike to disk */
    void writeSpike (int electrodeIndex, const Spike* spike) override;

    /** Write the timestamp sync text messages to disk*/
    void writeTimestampSyncText (uint64 streamId, int64 timestamp, float sourceSampleRate, String text) override;

    /** Allows the file identifier to be set externally*/
    void setParameter (EngineParameter& parameter) override;

private:
    /** Collect the channel information and create the AqNWB channel mappings */
    void createRecordingArrays();

    /** Finalize and close the current file (if any) and release all recording objects */
    void closeFile();

    /** Create the <series>/sync/sample_numbers dataset for the TimeSeries at seriesPath */
    std::shared_ptr<AQNWB::IO::BaseRecordingData> createSampleNumbersDataSet (const std::string& seriesPath,
                                                                              AQNWB::Types::SizeType chunkSize,
                                                                              const std::string& description);

    /** Start tracking the time range of a new recording (epoch) */
    void startEpoch (int recordingNumber);

    /** Create the epochs table with float64 start and stop times */
    bool createEpochsTable();

    /** Create one EventsTable per recorded event channel and the sync messages table */
    bool createEventsTables();

    /** Create an EventsTable with a float64 timestamp column, the optional annotation column, and custom columns */
    std::shared_ptr<AQNWB::NWB::EventsTable> createEventsTable (const std::string& name,
                                                               const std::string& description,
                                                               const std::string& sourceDescription,
                                                               float timestampResolution,
                                                               bool annotationColumn,
                                                               const std::vector<AQNWB::NWB::DynamicTable::DataSpecPtr>& customColumns);

    /** Write the pending sync messages whose stream start time is known. If force is true, the remaining
        messages are written with the start time of the recording as timestamp. */
    void writeSyncMessages (bool force);

    /** Log a failed AqNWB operation. Returns true if the status indicates success. */
    static bool checkStatus (AQNWB::Types::Status status, const String& operation);

    /** NWB I/O object */
    std::shared_ptr<AQNWB::IO::BaseIO> io;

    /** NWB file */
    std::shared_ptr<AQNWB::NWB::NWBFile> nwbfile;

    /** AqNWB channels for all continuous channels entering the Record Node, grouped by stream (electrodes table) */
    std::vector<AQNWB::Types::ChannelVector> electrodeArrays;

    /** AqNWB channels for the recorded continuous channels, one ChannelVector per stream (ElectricalSeries) */
    std::vector<AQNWB::Types::ChannelVector> recordingArrays;

    /** Names of the ElectricalSeries, one per entry in recordingArrays */
    std::vector<std::string> recordingArraysNames;

    /** AqNWB channels for each recorded spike channel (SpikeEventSeries) */
    std::vector<AQNWB::Types::ChannelVector> spikeRecordingArrays;

    /** Names of the SpikeEventSeries, one per entry in spikeRecordingArrays */
    std::vector<std::string> spikeRecordingArraysNames;

    /** Recording object indexes of the ElectricalSeries, one per entry in recordingArrays */
    std::vector<AQNWB::Types::SizeType> esContainerIndexes;

    /** Recording object indexes of the SpikeEventSeries, one per entry in spikeRecordingArrays */
    std::vector<AQNWB::Types::SizeType> spikeContainerIndexes;

    /** For each write channel (index among all recorded channels): index into recordingArrays */
    std::vector<size_t> writeChannelSeriesIndexes;

    /** For each write channel: position of the channel within its ElectricalSeries */
    std::vector<size_t> writeChannelLocalIndexes;

    /** Sample number datasets (<series>/sync/sample_numbers), one per ElectricalSeries / SpikeEventSeries */
    std::vector<std::shared_ptr<AQNWB::IO::BaseRecordingData>> esSampleNumbers;
    std::vector<std::shared_ptr<AQNWB::IO::BaseRecordingData>> spikeSampleNumbers;

    /** Number of sample numbers written to each dataset in esSampleNumbers / spikeSampleNumbers */
    std::vector<AQNWB::Types::SizeType> esSampleNumbersWritten;
    std::vector<AQNWB::Types::SizeType> spikeSampleNumbersWritten;

    /** Epochs table with one row per recording */
    std::shared_ptr<AQNWB::NWB::TimeIntervals> epochs;

    /** Type of data stored in the EventsTable of an event channel */
    enum class EventTableType
    {
        TTL,
        Text,
        Unsupported
    };

    /** EventsTable of a recorded event channel */
    struct EventTable
    {
        EventTableType type = EventTableType::Unsupported;
        std::string name;
        std::shared_ptr<AQNWB::NWB::EventsTable> table;
    };

    /** One EventTable per recorded event channel (indexed by the event channel index) */
    std::vector<EventTable> eventTables;

    /** EventsTable with the sync messages (start sample number of each stream for each recording) */
    std::shared_ptr<AQNWB::NWB::EventsTable> syncMessagesTable;

    /** Sync message received for the current recording, written when the recording stops */
    struct SyncMessage
    {
        uint16 streamId;
        int64 sampleNumber;
        std::string text;
    };
    std::vector<SyncMessage> pendingSyncMessages;

    /** Stream id of each ElectricalSeries, one per entry in recordingArrays */
    std::vector<uint16> recordingStreamIds;

    /** First recorded timestamp of each stream in the current recording */
    std::map<uint16, double> streamStartTimes;

    /** Set after the first failed or unsupported event write so that errors are only logged once per file */
    bool eventErrorLogged = false;

    /** Recording number and time range (in seconds) of the current recording */
    int currentRecordingNumber = 0;
    double recordingStartTime = 0.0;
    double recordingStopTime = 0.0;
    bool recordingHasData = false;

    /** Reusable buffers for converting float samples to int16 and for sample numbers */
    std::vector<float> scaledBuffer;
    std::vector<int16_t> intBuffer;
    std::vector<int64> sampleNumberBuffer;

    /** Set after the first failed data write so that errors are only logged once per file */
    bool writeErrorLogged = false;

    /** Interval for flushing the file during a recording, so that data is not lost if the GUI crashes */
    static constexpr uint32 flushIntervalMs = 10000;

    /** Time of the last flush (Time::getMillisecondCounter()) */
    uint32 lastFlushTime = 0;

    /** The identifier for the current file (can be set externally) */
    String identifierText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NWBRecordEngine);
};
} // namespace NWBRecording

#endif
