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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <set>
#include <string>

#include "NWBRecording.h"
#include "Utils.hpp"
#include "io/hdf5/HDF5IO.hpp"
#include "io/nwbio_utils.hpp"

#include "../../Source/Processors/RecordNode/RecordNode.h"

using namespace NWBRecording;
using AQNWB::Types::SizeType;
using AQNWB::Types::Status;

namespace
{
/** Name of the optional file with metadata for the NWB files (see README) */
const String metadataFileName = "nwb_metadata.json";

/** Session and subject metadata for the NWB file */
struct SessionMetadata
{
    std::string sessionDescription = "Recording with the Open Ephys GUI";
    std::optional<AQNWB::NWB::Subject::SubjectSpec> subject;
    AQNWB::NWB::NWBFile::GeneralMetadataSpec general;
};

/** A string or an array of strings of a JSON value */
std::vector<std::string> toStrings (const var& value)
{
    std::vector<std::string> strings;
    if (value.isArray())
    {
        for (const auto& element : *value.getArray())
            strings.push_back (element.toString().toStdString());
    }
    else
        strings.push_back (value.toString().toStdString());
    return strings;
}

/** Read the metadata file from the recording directory or one of its two parent directories (e.g., the
    folder selected in the GUI). Uses the same structure as NeuroConv metadata ("NWBFile" and "Subject"). */
SessionMetadata loadSessionMetadata (const File& recordingDirectory)
{
    SessionMetadata metadata;

    File file;
    File directory = recordingDirectory;
    for (int level = 0; level < 3 && directory.isDirectory(); level++, directory = directory.getParentDirectory())
    {
        if (directory.getChildFile (metadataFileName).existsAsFile())
        {
            file = directory.getChildFile (metadataFileName);
            break;
        }
    }
    if (file == File())
        return metadata;

    var json;
    const Result result = JSON::parse (file.loadFileAsString(), json);
    if (result.failed() || ! json.isObject())
    {
        LOGE ("NWB2: Unable to read ", file.getFullPathName(), ": ", result.failed() ? result.getErrorMessage() : "not a JSON object");
        return metadata;
    }
    LOGC ("NWB2: Using metadata from ", file.getFullPathName());

    if (auto* nwbFile = json["NWBFile"].getDynamicObject())
    {
        for (const auto& property : nwbFile->getProperties())
        {
            const std::string key = property.name.toString().toStdString();
            const std::string value = property.value.toString().toStdString();
            auto& general = metadata.general;
            if (key == "session_description")
                metadata.sessionDescription = value;
            else if (key == "experiment_description")
                general.experimentDescription = value;
            else if (key == "experimenter")
                general.experimenter = toStrings (property.value);
            else if (key == "institution")
                general.institution = value;
            else if (key == "keywords")
                general.keywords = toStrings (property.value);
            else if (key == "lab")
                general.lab = value;
            else if (key == "notes")
                general.notes = value;
            else if (key == "pharmacology")
                general.pharmacology = value;
            else if (key == "protocol")
                general.protocol = value;
            else if (key == "related_publications")
                general.relatedPublications = toStrings (property.value);
            else if (key == "session_id")
                general.sessionId = value;
            else if (key == "slices")
                general.slices = value;
            else if (key == "source_script")
                general.sourceScript = value;
            else if (key == "source_script_file_name")
                general.sourceScriptFileName = value;
            else if (key == "stimulus_notes")
                general.stimulusNotes = value;
            else if (key == "surgery")
                general.surgery = value;
            else if (key == "virus")
                general.virus = value;
            else
                LOGE ("NWB2: Unsupported NWBFile metadata '", key, "' in ", file.getFullPathName(), " is ignored");
        }
        // The schema requires the file name of the source script
        if (metadata.general.sourceScript.has_value() != metadata.general.sourceScriptFileName.has_value())
        {
            LOGE ("NWB2: 'source_script' and 'source_script_file_name' in ", file.getFullPathName(), " must be set together; both are ignored");
            metadata.general.sourceScript.reset();
            metadata.general.sourceScriptFileName.reset();
        }
    }

    if (auto* subject = json["Subject"].getDynamicObject())
    {
        AQNWB::NWB::Subject::SubjectSpec spec;
        for (const auto& property : subject->getProperties())
        {
            const std::string key = property.name.toString().toStdString();
            const std::string value = property.value.toString().toStdString();
            if (key == "subject_id")
                spec.subjectId = value;
            else if (key == "species")
                spec.species = value;
            else if (key == "sex")
                spec.sex = value;
            else if (key == "age")
                spec.age = value;
            else if (key == "age__reference" || key == "age_reference")
                spec.ageReference = value;
            else if (key == "date_of_birth")
                spec.dateOfBirth = value;
            else if (key == "description")
                spec.description = value;
            else if (key == "genotype")
                spec.genotype = value;
            else if (key == "strain")
                spec.strain = value;
            else if (key == "weight")
                spec.weight = value;
            else
                LOGE ("NWB2: Unsupported Subject metadata '", key, "' in ", file.getFullPathName(), " is ignored");
        }
        metadata.subject = spec;
    }

    return metadata;
}

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
    // New file for each experiment, e.g. experiment1.nwb, experiment2.nwb, etc.
    const File file = rootFolder.getChildFile ("experiment" + String (experimentNumber) + ".nwb");

    // Recordings within the same experiment are appended to the datasets of the open file. If the data
    // directory was changed between recordings (which doesn't reset the recording number), start a new
    // file in the new directory, unless a file of that name exists there already
    if (recordingNumber > 0 && io != nullptr && io->isOpen())
    {
        if (rootFolder == openFileFolder || file.exists())
        {
            if (rootFolder != openFileFolder)
                LOGE ("NWB2: ", file.getFullPathName(), " already exists and is not overwritten; recording ", recordingNumber + 1, " is appended to the file in ", openFileFolder.getFullPathName());
            startEpoch (recordingNumber);
            return;
        }
    }

    closeFile();

    // Never overwrite an existing file (e.g., of an earlier recording in a data directory that is used again)
    if (file.exists())
    {
        LOGE ("NWB2: ", file.getFullPathName(), " already exists and is not overwritten; the recording is not saved in NWB format");
        return;
    }

    openFileFolder = rootFolder;
    createRecordingArrays();

    const String filename = file.getFullPathName();

    io = AQNWB::createIO ("HDF5", filename.toStdString());

    // The data is written one channel at a time, so the chunk cache of each dataset must hold the
    // chunks of all channels (the default, e.g. 1 MiB in HDF5 1.14, is too small for high channel counts: a chunk of
    // 2048 samples x 384 channels x int16 is 1.5 MiB, causing a read-modify-write of the chunk per channel)
    size_t maxChannels = 1;
    for (const auto& group : recordingArrays)
        maxChannels = std::max (maxChannels, group.size());
    const SizeType chunkBytes = 2048 * maxChannels * sizeof (int16_t);
    const SizeType cacheBytes = std::max<SizeType> (AQNWB::IO::HDF5::HDF5IO::getDefaultChunkCacheNumBytes(), 4 * chunkBytes);
    auto hdf5io = std::dynamic_pointer_cast<AQNWB::IO::HDF5::HDF5IO> (io);
    const Status openStatus = hdf5io != nullptr
                                  ? hdf5io->open (AQNWB::IO::FileMode::Overwrite, AQNWB::IO::HDF5::ChunkCacheConfig { cacheBytes })
                                  : io->open (AQNWB::IO::FileMode::Overwrite);

    if (! checkStatus (openStatus, "Opening " + filename))
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
    SessionMetadata metadata = loadSessionMetadata (rootFolder);
    metadata.general.wasGeneratedBy = std::vector<std::pair<std::string, std::string>> {
        { "Open Ephys GUI", CoreServices::getGUIVersion().toStdString() },
        { "Open Ephys NWB Format plugin", pluginVersion }
    };

    // Initialize the NWB file with the session metadata and subject information
    // The session_start_time and timestamps_reference_time are set to the
    // current time by default and are overwritten when the recording starts.
    bool success = checkStatus (nwbfile->initialize (identifier.toStdString(),
                                                     metadata.sessionDescription,
                                                     dataCollection,
                                                     "", // session_start_time
                                                     "", // timestamps_reference_time
                                                     metadata.subject,
                                                     metadata.general),
                                "NWBFile initialization");

    // Create the ElectrodesTable
    if (success)
    {
        auto electrodesTable = nwbfile->createElectrodesTable (electrodeArrays);
        if (electrodesTable == nullptr)
        {
            LOGE ("NWB2: Electrodes table creation failed");
            success = false;
        }
        else
        {
            success = addChannelTypeColumn (electrodesTable);
        }
    }

    // Create the ElectricalSeries for continuous data. This call may create
    // multiple ElectricalSeries creating one series per recorded stream.
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

    // Create the epochs table and the events tables before starting the recording
    success = success && createEpochsTable();
    success = success && createEventsTables();

    // Start the recording in the NWB file
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

bool NWBRecordEngine::addChannelTypeColumn (const std::shared_ptr<AQNWB::NWB::ElectrodesTable>& electrodesTable)
{
    // Custom column with the Open Ephys channel type, e.g., to distinguish headstage and ADC channels
    const std::string path = electrodesTable->getPath() + "/channel_type";
    try
    {
        auto spec = AQNWB::NWB::VectorData::createDataSpec (
            "channel_type",
            AQNWB::IO::ArrayDataSetConfig (AQNWB::IO::BaseDataType::U8, { 0 }, { 100 }),
            "Open Ephys channel type: 0 = headstage (electrode), 1 = auxiliary (AUX), 2 = ADC");
        if (! checkStatus (electrodesTable->addColumn (spec), "Adding the channel_type column"))
            return false;

        auto dataset = io->getDataSet (path);
        return dataset != nullptr
               && checkStatus (dataset->writeDataBlock ({ electrodeChannelTypes.size() }, { 0 }, AQNWB::IO::BaseDataType::U8, electrodeChannelTypes.data()),
                               "Writing the channel_type column");
    }
    catch (const std::exception& e)
    {
        LOGE ("NWB2: Adding the channel_type column failed: ", e.what());
        return false;
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
                "Time of each event, in seconds, relative to the timestamps reference time.",
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
    std::map<std::string, int> channelCounts;
    eventTables.clear();

    for (int i = 0; i < getNumRecordedEventChannels(); i++)
    {
        const EventChannel* channel = getEventChannel (i);
        EventTable eventTable;

        // Event tables are named as the event folders of the Binary format: "<processor>-<id>.<stream>.TTL"
        // (e.g., "Phase Detector-104.example_data.TTL"), where <processor> is the processor that created the
        // event channel. Further TTL channels of the same processor and stream are named "TTL_1", "TTL_2", ...
        // Text events of the Message Center are stored in "messages".
        const bool isMessageCenter = channel->getSourceNodeName() == "Message Center";
        const String prefix = channel->getSourceNodeName() + "-" + String (channel->getSourceNodeId()) + "."
                              + channel->getStreamName();
        const String suffix = channel->getType() == EventChannel::TTL ? "TTL" : "TEXT";
        const int count = channelCounts[(prefix + "." + suffix).toStdString()]++;
        std::string name = isMessageCenter ? std::string ("messages")
                                           : (prefix + "." + suffix + (count > 0 ? "_" + String (count) : String()))
                                                 .replaceCharacter ('/', '_')
                                                 .toStdString();
        // make sure the table names are unique
        const std::string baseName = name;
        for (int n = 2; usedNames.count (name) > 0; n++)
            name = baseName + "_" + std::to_string (n);
        usedNames.insert (name);
        eventTable.name = name;

        const float resolution = channel->getSampleRate() > 0 ? 1.0f / channel->getSampleRate() : -1.0f;
        // The name of the channel is not part of the table name, so it is included in the description
        const String channelDescription = channel->getDescription().isNotEmpty() ? ": " + channel->getDescription() : String();
        const std::string description = ("'" + channel->getName() + "' of " + channel->getSourceNodeName() + " ("
                                         + String (channel->getSourceNodeId()) + ") on data stream '"
                                         + channel->getStreamName() + "'" + channelDescription)
                                            .toStdString();

        if (channel->getType() == EventChannel::TTL)
        {
            eventTable.type = EventTableType::TTL;
            eventTable.table = createEventsTable (
                name,
                "TTL events of " + description,
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
                                : "Text events of " + description,
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
        "milliseconds since midnight Jan 1st 1970 UTC and the timestamp is the start time of the recording. "
        "The timestamps reference time of the file is the system time of the first 'Software Time' message "
        "minus the start time of the first recording.",
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

        // The first 'Software Time' message of the file defines the timestamps reference time
        if (message.streamId == 0 && recordingHasData && ! referenceTimeSet)
            setReferenceTime (message.sampleNumber, recordingStartTime);

        AQNWB::Types::RowData row = { { "timestamp", timestamp },
                                      { "annotation", message.text },
                                      { "stream_id", message.streamId },
                                      { "sample_number", message.sampleNumber } };
        checkStatus (syncMessagesTable->addRow (row), "Writing sync message '" + String (message.text) + "'");
    }
    pendingSyncMessages = remaining;
}

void NWBRecordEngine::setReferenceTime (int64 softwareTimeMs, double firstTimestamp)
{
    // The timestamps are seconds since timestamp 0 of the GUI (sample 0 of the main stream, i.e., normally the
    // start of acquisition), while the file is created when recording starts. The system time is taken when
    // the Record Node processes the first block of the recording, whose first timestamp is firstTimestamp.
    referenceTimeSet = true;
    const int64 referenceMicros = softwareTimeMs * 1000 - static_cast<int64> (std::llround (firstTimestamp * 1e6));
    const std::string referenceTime = AQNWB::formatTime (
        std::chrono::system_clock::time_point (std::chrono::duration_cast<std::chrono::system_clock::duration> (
            std::chrono::microseconds (referenceMicros))));

    checkStatus (nwbfile->setTimestampsReferenceTime (referenceTime), "Setting the timestamps reference time");
    checkStatus (nwbfile->setSessionStartTime (referenceTime), "Setting the session start time");
    LOGD ("NWB2: Timestamps reference time ", referenceTime);
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
    referenceTimeSet = false;
    openFileFolder = File();

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

    convertToInt16 (spike->getDataPointer(), static_cast<int> (numSamplesPerChannel * numChannels), spikeChannel->getChannelBitVolts (0), scaledBuffer, intBuffer);

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
    electrodeChannelTypes.clear();
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

        electrodeChannelTypes.push_back (static_cast<uint8_t> (channelInfo->getChannelType()));
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
