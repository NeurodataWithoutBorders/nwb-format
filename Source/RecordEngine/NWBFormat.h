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

#ifndef NWBFORMAT_H
#define NWBFORMAT_H

#include <HDF5FileFormat.h>
#include <RecordingLib.h>
#include <ProcessorHeaders.h>

using namespace OpenEphysHDF5;

namespace NWBRecording
{

typedef Array<const ContinuousChannel*> ContinuousGroup;

/**
        Represents a generic NWB TimeSeries dataset
     */
class TimeSeries
{
public:
    /** Constructor */
    TimeSeries (String rootPath, String name, String description);

    /** Holds the sample data */
    ScopedPointer<HDF5RecordingData> baseDataSet;

    /** Holds the timestamps (in seconds) for each sample */
    ScopedPointer<HDF5RecordingData> timestampDataSet;

    /** Holds the sample number for each sample (relative to the start of acquisition) */
    ScopedPointer<HDF5RecordingData> sampleNumberDataSet;

    /** Holds metadata for this time series */
    OwnedArray<HDF5RecordingData> metaDataSet;

    /** The path to this dataset within the NWB file */
    String basePath;

    /** The description of this dataset*/
    String description;

    /** Total number of samples written */
    uint64 numSamples = 0;

    /** Get neurodata_type */
    virtual String getNeurodataType() { return "TimeSeries"; }
};

namespace ecephys
{
    /**
        Represents an NWB ElectricalSeries dataset
     */
    class ElectricalSeries : public TimeSeries
    {
    public:
        /** Constructor */
        ElectricalSeries (String rootPath, String name, String description, int channel_count, Array<float> channel_conversion);

        /** Holds the sample number for each sample (relative to the start of acquisition) */
        ScopedPointer<HDF5RecordingData> channelConversionDataSet;

        /** Holds the DynamicTableRegion index of each electrode  */
        ScopedPointer<HDF5RecordingData> electrodeDataSet;

        /** Channel conversion values */
        Array<float> channel_conversion;

        /** Number of channels to write */
        int channel_count;

        /** Get neurodata_type */
        virtual String getNeurodataType() override { return "ElectricalSeries"; }
    };

    /**
        Represents a sequence of spike events
     */
    class SpikeEventSeries : public ElectricalSeries
    {
    public:
        /** Constructor */
        SpikeEventSeries (String rootPath, String name, String description, int channel_count, Array<float> channel_conversion);

        /** Get neurodata_type */
        virtual String getNeurodataType() override { return "SpikeEventSeries"; }
    };
} // namespace ecephys

/**
        Represents a TTL event series (not a core NWB data type)
     */
class TTLEventSeries : public TimeSeries
{
public:
    /** Constructor */
    TTLEventSeries (String rootPath, String name, String description);

    /** Holds the TTL word for each sample */
    ScopedPointer<HDF5RecordingData> ttlWordDataSet;

    /** Get neurodata_type */
    virtual String getNeurodataType() override { return "TimeSeries"; }
};

/**
        Represents a sequence of string annotations
     */
class AnnotationSeries : public TimeSeries
{
public:
    /** Constructor */
    AnnotationSeries (String rootPath, String name, String description);

    /** Get neurodata_type */
    virtual String getNeurodataType() override { return "AnnotationSeries"; }
};

/**
        
        Represents an NWB 2.0 File (a specific type of HDF5 file)
            
     */
class NWBFile : public HDF5FileBase
{
public:
    /** Constructor */
    NWBFile (String fName, String ver, String identifier);

    /** Destructor */
    ~NWBFile();

    /** Creates the groups required for a new recording, given an array of continuous channels, event channels, and spike channels*/
    bool startNewRecording (int recordingNumber,
                            const Array<ContinuousGroup>& continuousArray,
                            const Array<const ContinuousChannel*>& continuousChannels,
                            const Array<const EventChannel*>& eventArray,
                            const Array<const SpikeChannel*>& electrodeArray);

    /** Writes the num_samples value and closes the relevent datasets */
    void stopRecording();

    /** Writes continuous data for a particular channel */
    void writeData (int datasetID, int channel, int nSamples, const float* data, float bitVolts);

    /** Writes synchronized timestamps for a particular continuous dataset */
    void writeTimestamps (int datasetID, int nSamples, const double* data);

    /** Writes sample numbers for a particular continuous dataset */
    void writeSampleNumbers (int datasetID, int nSamples, const int64* data);

    /** Writes electrode numbers for a continuous dataset */
    void writeElectrodes (ecephys::ElectricalSeries* electricalSeries, Array<int> electrodeInds);

    /** Writes channel conversion values */
    void writeChannelConversions (ecephys::ElectricalSeries* series);

    /** Writes a spike event*/
    void writeSpike (int electrodeId, const SpikeChannel* channel, const Spike* event);

    /** Writes an event (TEXT or TTL) */
    void writeEvent (int eventID, const EventChannel* channel, const Event* event);

    /** Writes a timestamp sync text event */
    void writeTimestampSyncText (uint16 sourceID,
                                 int64 timestamp,
                                 float sourceSampleRate,
                                 String text);

    /** Returns the name of this NWB file */
    String getFileName() override;

    /** Generate a new uuid string*/
    String generateUuid();

	protected:
	protected:
        
protected:
        
    /** Initializes the default groups */
    int createFileStructure() override;

	private:
	private:

private:

    /** Creates a new dataset to hold text data (messages) */
    void createTextDataSet (String path, String name, String text);

    /** Creates a new dataset to hold binary events */
    void createBinaryDataSet (String path, String name, HDF5FileBase::BaseDataType type, int length, void* data);

    /** Returns the HDF5 data type for a given event channel type */
    static HDF5FileBase::BaseDataType getEventH5Type (EventChannel::Type type, int length = 1);

    /** Returns the HDF5 data type for a given metadata type*/
    static HDF5FileBase::BaseDataType getMetadataH5Type (MetadataDescriptor::MetadataType type, int length = 1);

    /** Creates a time series dataset*/
    bool createTimeSeriesBase (TimeSeries* timeSeries);

    /** Creates dataset attributes */
    bool createExtraInfo (String basePath, String name, String desc, String id, uint16 index, uint16 typeIndex);

    /** Creates a dataset of synchronized timestamps (interval = 1/sample_rate) */
    HDF5RecordingData* createTimestampDataSet (String basePath, int chunk_size, float interval);

    /** Creates a dataset of sample numbers */
    HDF5RecordingData* createSampleNumberDataSet (String basePath, int chunk_size);

    /** Creates a dataset for electrode indices */
    HDF5RecordingData* createElectrodeDataSet (String basePath, String description, int chunk_size);

    /** Creates a dataset for electrode indices */
    HDF5RecordingData* createChannelConversionDataSet (String basePath, String description, int chunk_size);

    /** Adds attributes (e.g. conversion, resolution) to a continuous dataset */
    void createDataAttributes (String basePath, float conversion, float resolution, String unit);

    /** Creates a dataset for channel metdata */
    bool createChannelMetadataSets (String basePath, const MetadataObject* info);

    /** Creates a dataset for event metdata */
    bool createEventMetadataSets (String basePath, TimeSeries* timeSeries, const MetadataEventObject* info);

    /** Writes metadata associated with an event*/
    void writeEventMetadata (TimeSeries* timeSeries, const MetadataEventObject* info, const MetadataEvent* event);

    const String filename;
    const String GUIVersion;

    OwnedArray<ecephys::ElectricalSeries> continuousDataSets;
    OwnedArray<ecephys::SpikeEventSeries> spikeDataSets;
    OwnedArray<TTLEventSeries> eventDataSets;
    std::unique_ptr<AnnotationSeries> messagesDataSet;
    std::unique_ptr<AnnotationSeries> syncMsgDataSet;

    const String identifierText;

    HeapBlock<float> scaledBuffer;
    HeapBlock<int16> intBuffer;
    size_t bufferSize;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NWBFile);
};

} // namespace NWBRecording

#endif
