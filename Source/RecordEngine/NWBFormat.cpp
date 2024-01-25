/*
 ------------------------------------------------------------------

 This file is part of the Open Ephys GUI
 Copyright (C) 2014 Open Ephys

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
 
 #include "NWBFormat.h"
 
 using namespace NWBRecording;

#ifndef EVENT_CHUNK_SIZE
#define EVENT_CHUNK_SIZE 8
#endif

#ifndef SPIKE_CHUNK_XSIZE
#define SPIKE_CHUNK_XSIZE 8
#endif

#ifndef SPIKE_CHUNK_YSIZE
#define SPIKE_CHUNK_YSIZE 40
#endif

 #define MAX_BUFFER_SIZE 40960

NWBFile::NWBFile(String fName, String ver, String idText) :
    HDF5FileBase(),
    filename(fName),
    identifierText(idText),
    GUIVersion(ver)
{
	 readyToOpen = true; //In KWIK this is in initFile, but the new recordEngine methods make it safe for it to be here

	 scaledBuffer.malloc(MAX_BUFFER_SIZE);
	 intBuffer.malloc(MAX_BUFFER_SIZE);
	 bufferSize = MAX_BUFFER_SIZE;
}

NWBFile::~NWBFile()
{
	continuousDataSets.clear();
	spikeDataSets.clear();
	eventDataSets.clear();
	syncMsgDataSet.reset();
}
 
int NWBFile::createFileStructure()
{

	setAttributeStr("core", "/", "namespace");
	setAttributeStr("NWBFile", "/", "neurodata_type");
	setAttributeStr("2.5.0", "/", "nwb_version");
	setAttributeStr(identifierText, "/", "object_id");

	if (createGroup("/acquisition")) return -1;

	if (createGroup("/analysis")) return -1;

	String time = Time::getCurrentTime().formatted("%Y-%m-%dT%H:%M:%S") + Time::getCurrentTime().getUTCOffsetString(true);

	createTextDataSet("", "file_create_date", time);

	if (createGroup("/general")) return -1;
	if (createGroup("general/devices")) return -1;
	if (createGroup("general/extracellular_ephys")) return -1;
	if (createGroup("general/extracellular_ephys/electrodes")) return -1;

	StringArray colnames;
	colnames.add("group");
	colnames.add("group_name");
	colnames.add("location");
	setAttributeStrArray(colnames, "general/extracellular_ephys/electrodes", "colnames");
	setAttributeStr("metadata about extracellular electrodes", "general/extracellular_ephys/electrodes", "description");
	setAttributeStr("hdmf-common", "general/extracellular_ephys/electrodes", "namespace");
	setAttributeStr("DynamicTable", "general/extracellular_ephys/electrodes", "neurodata_type");
	setAttributeStr(generateUuid(), "general/extracellular_ephys/electrodes", "object_id");

	if (createGroup("/processing")) return -1;

	if (createGroup("/stimulus")) return -1;
	if (createGroup("/stimulus/presentation")) return -1;
	if (createGroup("/stimulus/templates")) return -1;
    
    if (createGroup("/specifications")) return -1;
    if (createGroup("/specifications/core")) return -1;
    if (createGroup("/specifications/core/2.6.0-alpha")) return -1;
    if (createGroup("/specifications/hdmf-common")) return -1;
    if (createGroup("/specifications/hdmf-common/1.8.0")) return -1;
    cacheSpecifications("core/2.6.0-alpha/");
    cacheSpecifications("hdmf-common/1.8.0/");

	createStringDataSet("/session_description", "Recording with the Open Ephys GUI");
	createStringDataSet("/session_start_time", time);
	createStringDataSet("/timestamps_reference_time", time);
	createStringDataSet("/identifier", "test-identifier");

	return 0;

}

TimeSeries::TimeSeries(String rootPath, String name, String description_)
    : basePath(rootPath + name), description(description_)
{
    
}

ecephys::ElectricalSeries::ElectricalSeries(String rootPath, String name, String description_,
                                            int channel_count_, Array<float> channel_conversion_)
    : TimeSeries(rootPath, name, description_),
      channel_conversion(channel_conversion_),
      channel_count(channel_count_)
{
    
}

ecephys::SpikeEventSeries::SpikeEventSeries(String rootPath, String name, String description_,
                                            int channel_count, Array<float> channel_conversion_)
    : ecephys::ElectricalSeries(rootPath, name, description_, channel_count, channel_conversion_)
{
    
}

TTLEventSeries::TTLEventSeries(String rootPath, String name, String description_)
    : TimeSeries(rootPath, name, description_)
{
    
}

AnnotationSeries::AnnotationSeries(String rootPath, String name, String description_)
    : TimeSeries(rootPath, name, description_)
{
    
}

 
bool NWBFile::startNewRecording(
	int recordingNumber, 
	const Array<ContinuousGroup>& continuousArray,
	const Array<const ContinuousChannel*>& continuousChannels,
	const Array<const EventChannel*>& eventArray, 
	const Array<const SpikeChannel*>& electrodeArray)
{

    // all recorded data is stored in the "acquisition" group
	String rootPath = "/acquisition/";

	continuousDataSets.clearQuick(true);
	spikeDataSets.clearQuick(true);
	eventDataSets.clearQuick(true);

	Array<int> all_electrode_inds;
	StringArray groupNames;
	StringArray groupReferences;

	// 0. put global inds into electrode table
	for (auto ch : continuousChannels)
	{
		all_electrode_inds.add(ch->getGlobalIndex());

		String groupName = ch->getSourceNodeName() + "-"
			+ String(ch->getSourceNodeId())
			+ "." + ch->getStreamName();

		groupNames.add(groupName);
		groupReferences.add("/general/extracellular_ephys/" + groupName);
	}

    // 1. Create continuous datasets
	for (int i = 0; i < continuousArray.size(); i++)
	{

		// Get the scaling info for each channel
		ContinuousGroup group = continuousArray.getReference(i);
        
        Array<float> channel_conversion;
        for (int ch = 0; ch < group.size(); ch++)
        {
            channel_conversion.add(group[ch]->getBitVolts() / 1e6);
        }
        
		String groupName = group[0]->getSourceNodeName() + "-"
			+ String(group[0]->getSourceNodeId())
			+ "." + group[0]->getStreamName();

		String fullPath = "general/extracellular_ephys/" + groupName;
		createGroup(fullPath);
		setAttributeStr("description", fullPath, "description");
		setAttributeStr("unknown", fullPath, "location");
		setAttributeStr("core", fullPath, "namespace");
		setAttributeStr("ElectrodeGroup", fullPath, "neurodata_type");
		setAttributeStr(generateUuid(), fullPath, "object_id");

		createGroup("general/devices/" + groupName);

		setAttributeStr("description", "general/devices/" + groupName, "description");
		setAttributeStr("unknown", "general/devices/" + groupName, "manufacturer");
		setAttributeStr("core", "general/devices/" + groupName, "namespace");
		setAttributeStr("Device", "general/devices/" + groupName, "neurodata_type");
		setAttributeStr(generateUuid(), "general/devices/" + groupName, "object_id");

		createReference("/" + fullPath + "/device", "/general/devices/" + groupName);

        Array<int> electrode_inds;
        for (int ch = 0; ch < group.size(); ch++)
        {
			int index = group[ch]->getGlobalIndex();
            electrode_inds.add(index);
        }

        ecephys::ElectricalSeries* electricalSeries =
            new ecephys::ElectricalSeries(rootPath,
                                          groupName,
                                          "Stores continuously sampled voltage data from an extracellular ephys recording",
                                          group.size(),
                                          channel_conversion
                                          );

        if (recordingNumber == 0)
            if (!createTimeSeriesBase(electricalSeries))
                return false;

        electricalSeries->baseDataSet = createDataSet(BaseDataType::I16,
                                0,
                               electricalSeries->channel_count,
                                CHUNK_XSIZE,
                                electricalSeries->basePath + "/data");
            
        if (electricalSeries->baseDataSet == nullptr)
        {
            std::cerr << "Error creating dataset for " << groupName << std::endl;
            return false;
        } else {
            createDataAttributes(electricalSeries->basePath, channel_conversion[0], -1.0f, "volts");
        }

        electricalSeries->timestampDataSet =
            createTimestampDataSet(electricalSeries->basePath + "/timestamps", CHUNK_XSIZE, 1/group[0]->getSampleRate());
        if (electricalSeries->timestampDataSet == nullptr) return false;
        
        electricalSeries->sampleNumberDataSet =
            createSampleNumberDataSet(electricalSeries->basePath + "/sync", CHUNK_XSIZE);
        if (electricalSeries->sampleNumberDataSet == nullptr) return false;

        electricalSeries->channelConversionDataSet = createChannelConversionDataSet(electricalSeries->basePath + "/channel_conversion", "Bit volts values for all channels", CHUNK_XSIZE);
        
        if (electricalSeries->channelConversionDataSet == nullptr) return false;
        writeChannelConversions(electricalSeries);
        
        electricalSeries->electrodeDataSet = createElectrodeDataSet(electricalSeries->basePath + "/electrodes", "Electrode index for each channel", CHUNK_XSIZE);
        
        if (electricalSeries->electrodeDataSet == nullptr) return false;
        writeElectrodes(electricalSeries, electrode_inds);

		continuousDataSets.add(electricalSeries);
	}

    // 2. create spike datasets
	for (int i = 0; i < electrodeArray.size(); i++)
	{
		const SpikeChannel* sourceInfo = electrodeArray[i];

		String sourceName = sourceInfo->getSourceNodeName() + "-" + String(sourceInfo->getSourceNodeId());
		sourceName += "." + sourceInfo->getStreamName();
        sourceName += "." + sourceInfo->getName();
        
        Array<float> channel_conversion;
        
        for (int ch = 0; ch < sourceInfo->getNumChannels(); ch++)
        {
            channel_conversion.add(sourceInfo->getSourceChannels()[0]->getBitVolts() / 1e6);
        }
        
        Array<int> electrode_inds;
        
        for (int ch = 0; ch < sourceInfo->getNumChannels(); ch++)
        {
			int globalIndex = sourceInfo->getSourceChannels()[ch]->getGlobalIndex();

			electrode_inds.add(globalIndex);
        }
        
        ecephys::SpikeEventSeries* spikeEventSeries =
            new ecephys::SpikeEventSeries(rootPath, sourceName,
                                          "Stores spike waveforms from an extracellular ephys recording",
                                          sourceInfo->getNumChannels(),
                                          channel_conversion);
        
        if (recordingNumber == 0)
            if (!createTimeSeriesBase(spikeEventSeries))
                return false;

        spikeEventSeries->baseDataSet = createDataSet(BaseDataType::I16, 0, sourceInfo->getNumChannels(), sourceInfo->getTotalSamples(), SPIKE_CHUNK_XSIZE, spikeEventSeries->basePath + "/data");
        
        if (spikeEventSeries->baseDataSet == nullptr)
        {
            std::cerr << "Error creating dataset for electrode " << i << std::endl;
            return false;
        } else {
            createDataAttributes(spikeEventSeries->basePath, channel_conversion[0], -1.0f, "volts");
        }
        
        spikeEventSeries->timestampDataSet =
            createTimestampDataSet(spikeEventSeries->basePath + "/timestamps", CHUNK_XSIZE, 1/sourceInfo->getSourceChannels()[0]->getSampleRate());
        if (spikeEventSeries->timestampDataSet == nullptr) return false;
        
        spikeEventSeries->sampleNumberDataSet =
            createSampleNumberDataSet(spikeEventSeries->basePath + "/sync", CHUNK_XSIZE);
        if (spikeEventSeries->sampleNumberDataSet == nullptr) return false;

        spikeEventSeries->channelConversionDataSet = createChannelConversionDataSet(spikeEventSeries->basePath + "/channel_conversion", "Bit volts values for all channels", CHUNK_XSIZE);
        
        if (spikeEventSeries->channelConversionDataSet == nullptr) return false;
        writeChannelConversions(spikeEventSeries);
        
        spikeEventSeries->electrodeDataSet = createElectrodeDataSet(spikeEventSeries->basePath + "/electrodes", "Electrode index for each channel", CHUNK_XSIZE);
        
        if (spikeEventSeries->electrodeDataSet == nullptr) return false;
        writeElectrodes(spikeEventSeries, electrode_inds);
    
		spikeDataSets.add(spikeEventSeries);

	}
	
    // 3. Create event channel datasets
	for (int i = 0; i < eventArray.size(); i++)
	{
        
		const EventChannel* info = eventArray[i];
        
		String sourceName = info->getSourceNodeName() + "-" + String(info->getSourceNodeId());
		sourceName += "." + info->getStreamName();
        
		String typeString, description;
        
        if (info->getType() == EventChannel::TTL)
        {
            TTLEventSeries* ttlEventSeries =
                new TTLEventSeries(rootPath, sourceName + ".TTL", "Stores the times and lines of TTL events");
            
            if (recordingNumber == 0)
                if (!createTimeSeriesBase(ttlEventSeries)) return false;
            
            ttlEventSeries->baseDataSet = createDataSet(getEventH5Type(info->getType(), info->getLength()), 0, EVENT_CHUNK_SIZE, ttlEventSeries->basePath + "/data");
            
            if (ttlEventSeries->baseDataSet == nullptr)
            {
                std::cerr << "Error creating dataset for event " << info->getName() << std::endl;
                return false;
            }
            
            ttlEventSeries->timestampDataSet = 
				createTimestampDataSet(ttlEventSeries->basePath + "/timestamps", EVENT_CHUNK_SIZE, 1/info->getSampleRate());
            if (ttlEventSeries->timestampDataSet == nullptr) return false;
            
            ttlEventSeries->sampleNumberDataSet = createSampleNumberDataSet(ttlEventSeries->basePath + "/sync", EVENT_CHUNK_SIZE);
            if (ttlEventSeries->sampleNumberDataSet == nullptr) return false;
            
            ttlEventSeries->ttlWordDataSet = createDataSet(BaseDataType::U64, 0, info->getDataSize(), EVENT_CHUNK_SIZE, ttlEventSeries->basePath + "/full_word");
            if (ttlEventSeries->ttlWordDataSet == nullptr) return false;
            
            eventDataSets.add(ttlEventSeries);

        }
		else if (info->getType() == EventChannel::TEXT)
        {
            AnnotationSeries* annotationSeries = new AnnotationSeries(rootPath, "messages", "Stores timestamped messages generated during an experiment");
            
            if (recordingNumber == 0)
                if (!createTimeSeriesBase(annotationSeries)) return false;
            
            annotationSeries->baseDataSet = createDataSet(getEventH5Type(info->getType(), info->getLength()), 0, EVENT_CHUNK_SIZE, annotationSeries->basePath + "/data");
            
            if (annotationSeries->baseDataSet == nullptr)
            {
                std::cerr << "Error creating dataset for event " << info->getName() << std::endl;
                return false;
            }
            
            annotationSeries->timestampDataSet = createTimestampDataSet(annotationSeries->basePath + "/timestamps", EVENT_CHUNK_SIZE, 1/info->getSampleRate());
            if (annotationSeries->timestampDataSet == nullptr) return false;
            
            annotationSeries->sampleNumberDataSet = createSampleNumberDataSet(annotationSeries->basePath + "/sync", EVENT_CHUNK_SIZE);
            if (annotationSeries->sampleNumberDataSet == nullptr) return false;
            
            messagesDataSet.reset(annotationSeries);
        }

	}

	//4. Create sync messages dataset
	String desc = "Stores recording start timestamps for each processor in text format";
	
    AnnotationSeries* annotationSeries = new AnnotationSeries(rootPath, "sync_messages", desc);

    if (recordingNumber == 0)
    {
		if (!createTimeSeriesBase(annotationSeries)) return false;

        annotationSeries->baseDataSet = createDataSet(BaseDataType::STR(100), 0, 1, annotationSeries->basePath + "/data");
  
        if (annotationSeries->baseDataSet == nullptr)
        {
            std::cerr << "Error creating dataset for sync messages" << std::endl;
            return false;
        }
	}
	else {
        annotationSeries->baseDataSet = getDataSet(annotationSeries->basePath + "/data");
	}

    if (recordingNumber == 0)
    {
        annotationSeries->sampleNumberDataSet = createSampleNumberDataSet(annotationSeries->basePath + "/sync", 1);
        if (annotationSeries->sampleNumberDataSet == nullptr) return false;
	}
	else {
        annotationSeries->sampleNumberDataSet = getDataSet(annotationSeries->basePath + "/sync");
	}
    
	if (recordingNumber == 0)
	{
        annotationSeries->timestampDataSet = createTimestampDataSet(annotationSeries->basePath + "/timestamps", 1, 1);
		if (annotationSeries->timestampDataSet == nullptr) return false;
	}
	else {
        annotationSeries->timestampDataSet = getDataSet(annotationSeries->basePath + "/timestamps");
	}

    syncMsgDataSet.reset(annotationSeries);

	// 5. Create electrode table
	ScopedPointer<HDF5RecordingData> elSet = createDataSet(BaseDataType::I32, 1, 1, "general/extracellular_ephys/electrodes/id");

	std::vector<int> electrodeNumbers;
	for (auto i : all_electrode_inds)
		electrodeNumbers.push_back(i);

	CHECK_ERROR(elSet->writeDataBlock(electrodeNumbers.size(), BaseDataType::I32, &electrodeNumbers[0]));

	setAttributeStr("hdmf-common", "general/extracellular_ephys/electrodes/id", "namespace");
	setAttributeStr("ElementIdentifiers", "general/extracellular_ephys/electrodes/id", "neurodata_type");
	setAttributeStr(generateUuid(), "general/extracellular_ephys/electrodes/id", "object_id");

	ScopedPointer<HDF5RecordingData> groupNamesDataset = createDataSet(BaseDataType::STR(250), 0, 1, "general/extracellular_ephys/electrodes/group_name");

	for (int i = 0; i < groupNames.size(); i++)
		groupNamesDataset->writeDataBlock(1, BaseDataType::STR(groupNames[i].length()), groupNames[i].toUTF8());

	setAttributeStr("the name of the ElectrodeGroup this electrode is a part of", "general/extracellular_ephys/electrodes/group_name", "description");
	setAttributeStr("hdmf-common", "general/extracellular_ephys/electrodes/group_name", "namespace");
	setAttributeStr("VectorData", "general/extracellular_ephys/electrodes/group_name", "neurodata_type");
	setAttributeStr(generateUuid(), "general/extracellular_ephys/electrodes/group_name", "object_id");

	ScopedPointer<HDF5RecordingData> locationsDataset = createDataSet(BaseDataType::STR(250), 0, 1, "general/extracellular_ephys/electrodes/location");

	for (int i = 0; i < groupNames.size(); i++)
		locationsDataset->writeDataBlock(1, BaseDataType::STR(7), String("unknown").toUTF8());

	setAttributeStr("the location of channel within the subject e.g. brain region", "general/extracellular_ephys/electrodes/location", "description");
	setAttributeStr("hdmf-common", "general/extracellular_ephys/electrodes/location", "namespace");
	setAttributeStr("VectorData", "general/extracellular_ephys/electrodes/location", "neurodata_type");
	setAttributeStr(generateUuid(), "general/extracellular_ephys/electrodes/location", "object_id");

	createReferenceDataSet("general/extracellular_ephys/electrodes/group", groupReferences);

	setAttributeStr("a reference to the ElectrodeGroup this electrode is a part of", "general/extracellular_ephys/electrodes/group", "description");
	setAttributeStr("hdmf-common", "general/extracellular_ephys/electrodes/group", "namespace");
	setAttributeStr("VectorData", "general/extracellular_ephys/electrodes/group", "neurodata_type");
	setAttributeStr(generateUuid(), "general/extracellular_ephys/electrodes/group", "object_id");

	return true;

 }
 
 void NWBFile::stopRecording()
 {

	 const TimeSeries* tsStruct;

	 for (int i = 0; i < continuousDataSets.size(); i++)
	 {
		 tsStruct = continuousDataSets[i];
		 CHECK_ERROR(setAttribute(BaseDataType::U64, &(tsStruct->numSamples), tsStruct->basePath, "num_samples"));
	 }

	 for (int i = 0; i < spikeDataSets.size(); i++)
	 {
		 tsStruct = spikeDataSets[i];
		 CHECK_ERROR(setAttribute(BaseDataType::U64, &(tsStruct->numSamples), tsStruct->basePath, "num_samples"));
	 }
	 
	 for (int i = 0; i < eventDataSets.size(); i++)
	 {
		 tsStruct = eventDataSets[i];
		 CHECK_ERROR(setAttribute(BaseDataType::U64, &(tsStruct->numSamples), tsStruct->basePath, "num_samples"));
	 }
	 
	 CHECK_ERROR(setAttribute(BaseDataType::U64, &(syncMsgDataSet->numSamples), syncMsgDataSet->basePath, "num_samples"));
 }
 
 void NWBFile::writeData(int datasetID, int channel, int nSamples, const float* data, float bitVolts)
 {
	 if (!continuousDataSets[datasetID])
		 return;

	 if (nSamples > bufferSize) //Shouldn't happen, and if it happens it'll be slow, but better this than crashing. Will be reset on file close and reset.
	 {
		 std::cerr << "Write buffer overrun, resizing to" << nSamples << std::endl;
		 bufferSize = nSamples;
		 scaledBuffer.malloc(nSamples);
		 intBuffer.malloc(nSamples);
	 }

	 double multFactor = 1 / (float(0x7fff) * bitVolts);
	 FloatVectorOperations::copyWithMultiply(scaledBuffer.getData(), data, multFactor, nSamples);
	 AudioDataConverters::convertFloatToInt16LE(scaledBuffer.getData(), intBuffer.getData(), nSamples);

	 continuousDataSets[datasetID]->baseDataSet->writeDataRow(channel, nSamples, BaseDataType::I16, intBuffer);
	 //CHECK_ERROR();
	 
	 /* Since channels are filled asynchronouysly by the Record Thread, there is no guarantee
		that at a any point in time all channels in a dataset have the same number of filled samples.
		However, since each dataset is filled from a single source, all channels must have the
		same number of samples at acquisition stop. To keep track of the written samples we must chose
		an arbitrary channel, and at the end all channels will be the same. */

	 if (channel == 0) //there will always be a first channel or there wouldn't be dataset
		 continuousDataSets[datasetID]->numSamples += nSamples;
 }

 void NWBFile::writeSampleNumbers(int datasetID, int nSamples, const int64* data)
 {
	 if (!continuousDataSets[datasetID])
		 return;

	 CHECK_ERROR(continuousDataSets[datasetID]->sampleNumberDataSet->writeDataBlock(nSamples, BaseDataType::I64, data));
 }

 void NWBFile::writeTimestamps(int datasetID, int nSamples, const double* data)
 {
	 if (!continuousDataSets[datasetID])
		 return;

	 CHECK_ERROR(continuousDataSets[datasetID]->timestampDataSet->writeDataBlock(nSamples, BaseDataType::F64, data));
 }

void NWBFile::writeChannelConversions(ecephys::ElectricalSeries* electricalSeries)
{
   std::vector<float> conversions;
   for (auto c : electricalSeries->channel_conversion)
       conversions.push_back(c);

    CHECK_ERROR(electricalSeries->channelConversionDataSet->writeDataBlock(conversions.size(), BaseDataType::F32, &conversions[0]));
}

 void NWBFile::writeElectrodes(ecephys::ElectricalSeries* electricalSeries, Array<int> electrodeInds)
 {
	std::vector<int> electrodeNumbers;
	for (auto i : electrodeInds)
		electrodeNumbers.push_back(i);

	 CHECK_ERROR(electricalSeries->electrodeDataSet->writeDataBlock(electricalSeries->channel_count, BaseDataType::I32, &electrodeNumbers[0]));
 }

 void NWBFile::writeSpike(int electrodeId, const SpikeChannel* channel, const Spike* event)
 {
	 if (!spikeDataSets[electrodeId])
		 return;
	 int nSamples = channel->getTotalSamples() * channel->getNumChannels();

	 if (nSamples > bufferSize) //Shouldn't happen, and if it happens it'll be slow, but better this than crashing. Will be reset on file close and reset.
	 {
		 std::cerr << "Write buffer overrun, resizing to" << nSamples << std::endl;
		 bufferSize = nSamples;
		 scaledBuffer.malloc(nSamples);
		 intBuffer.malloc(nSamples);
	 }

	 double multFactor = 1 / (float(0x7fff) * channel->getChannelBitVolts(0));
	 FloatVectorOperations::copyWithMultiply(scaledBuffer.getData(), event->getDataPointer(), multFactor, nSamples);
	 AudioDataConverters::convertFloatToInt16LE(scaledBuffer.getData(), intBuffer.getData(), nSamples);

	 double timestampSec = event->getTimestampInSeconds();

	 CHECK_ERROR(spikeDataSets[electrodeId]->baseDataSet->writeDataBlock(1, BaseDataType::I16, intBuffer));
	 CHECK_ERROR(spikeDataSets[electrodeId]->timestampDataSet->writeDataBlock(1, BaseDataType::F64, &timestampSec));
	 writeEventMetadata(spikeDataSets[electrodeId], channel, event);

	 const int64 sampleNumber = event->getSampleNumber();

	 CHECK_ERROR(spikeDataSets[electrodeId]->sampleNumberDataSet->writeDataBlock(1, BaseDataType::I64, &sampleNumber));


	 spikeDataSets[electrodeId]->numSamples += 1;

 }

 void NWBFile::writeEvent(int eventID, const EventChannel* channel, const Event* event)
 {

	const void* dataSrc;
	BaseDataType type;
	int8 ttlVal;
	String text;

	switch (event->getEventType())
	{
	case EventChannel::TTL:
		ttlVal = (static_cast<const TTLEvent*>(event)->getState() ? 1 : -1) * (static_cast<const TTLEvent*>(event)->getLine() + 1);
		dataSrc = &ttlVal;
		type = BaseDataType::I8;
		break;
	case EventChannel::TEXT:
		text = static_cast<const TextEvent*>(event)->getText();
		dataSrc = text.toUTF8().getAddress();
		type = BaseDataType::STR(text.length());
		break;
	default:
		dataSrc = static_cast<const BinaryEvent*>(event)->getBinaryDataPointer();
		type = getEventH5Type(event->getEventType());
		break;
	}

	if (eventID == eventDataSets.size()) //MessageCenter event
	{
		CHECK_ERROR(messagesDataSet->baseDataSet->writeDataBlock(1, BaseDataType::STR(text.length()), text.toUTF8()));

		const int64 sampleNumber = event->getSampleNumber();

		CHECK_ERROR(messagesDataSet->sampleNumberDataSet->writeDataBlock(1, BaseDataType::I64, &sampleNumber));

		const double timeSec = event->getTimestampInSeconds();

		CHECK_ERROR(messagesDataSet->timestampDataSet->writeDataBlock(1, BaseDataType::F64, &timeSec));

		messagesDataSet->numSamples += 1;

	}
	else if (eventDataSets[eventID])
	{
		CHECK_ERROR(eventDataSets[eventID]->baseDataSet->writeDataBlock(1, type, dataSrc));

		const double timeSec = event->getTimestampInSeconds();

		CHECK_ERROR(eventDataSets[eventID]->timestampDataSet->writeDataBlock(1, BaseDataType::F64, &timeSec));

		const int64 sampleNumber = event->getSampleNumber();

		CHECK_ERROR(eventDataSets[eventID]->sampleNumberDataSet->writeDataBlock(1, BaseDataType::I64, &sampleNumber));

		if (event->getEventType() == EventChannel::TTL)
		{
			const uint64 ttlWord = static_cast<const TTLEvent*>(event)->getWord();
			CHECK_ERROR(eventDataSets[eventID]->ttlWordDataSet->writeDataBlock(1, BaseDataType::U64, &ttlWord));
		}

		eventDataSets[eventID]->numSamples += 1;
	}
	else
	{
		//Attempted to write an event to disk from unknown event source
	}
	 
 }

 void NWBFile::writeTimestampSyncText(uint16 sourceID, int64 sampleNumber, float sourceSampleRate, String text)
 {
	 CHECK_ERROR(syncMsgDataSet->baseDataSet->writeDataBlock(1, BaseDataType::STR(text.length()), text.toUTF8()));
     
	 CHECK_ERROR(syncMsgDataSet->sampleNumberDataSet->writeDataBlock(1, BaseDataType::I64, &sampleNumber));

	 double timestamp = (double)sampleNumber;

	 CHECK_ERROR(syncMsgDataSet->timestampDataSet->writeDataBlock(1, BaseDataType::F64, &timestamp));

	 syncMsgDataSet->numSamples += 1;
 }

 
 String NWBFile::getFileName()
 {
	 return filename;
 }

  bool NWBFile::createTimeSeriesBase(TimeSeries* timeSeries)
 {
	 if (createGroup(timeSeries->basePath)) return false;
	 CHECK_ERROR(setAttributeStr(" ", timeSeries->basePath, "comments"));
	 CHECK_ERROR(setAttributeStr(timeSeries->description, timeSeries->basePath, "description"));
	 CHECK_ERROR(setAttributeStr("core", timeSeries->basePath, "namespace"));
      CHECK_ERROR(setAttributeStr(generateUuid(), timeSeries->basePath, "object_id"));
	 CHECK_ERROR(setAttributeStr(timeSeries->getNeurodataType(), timeSeries->basePath, "neurodata_type"));
	 return true;
 }

  void NWBFile::createDataAttributes(String basePath, float conversion, float resolution, String unit)
  {
		  CHECK_ERROR(setAttribute(BaseDataType::F32, &conversion, basePath + "/data", "conversion"));
		  CHECK_ERROR(setAttribute(BaseDataType::F32, &resolution, basePath + "/data", "resolution"));
		  CHECK_ERROR(setAttributeStr(unit, basePath + "/data", "unit"));
  }

  HDF5RecordingData* NWBFile::createTimestampDataSet(String path, int chunk_size, float interval)
  {
	  
	  HDF5RecordingData* tsSet = createDataSet(BaseDataType::F64, 0, chunk_size, path);

	  if (!tsSet)
		  std::cerr << "Error creating timestamp dataset in " << path << std::endl;
	  else
	  {
		  CHECK_ERROR(setAttribute(BaseDataType::F32, &interval, path, "interval"));
		  CHECK_ERROR(setAttributeStr("seconds", path, "unit"));
	  }

	  return tsSet;
  }

   HDF5RecordingData* NWBFile::createSampleNumberDataSet(String path, int chunk_size)
  {
	  HDF5RecordingData* tsSet = createDataSet(BaseDataType::I64, 0, chunk_size, path);
	  if (!tsSet)
		  std::cerr << "Error creating sample number dataset in " << path << std::endl;
	  else
	  {
		  const int32 one = 1;
		  CHECK_ERROR(setAttribute(BaseDataType::I32, &one, path, "interval"));
		  CHECK_ERROR(setAttributeStr("samples", path, "unit"));
	  }
	  return tsSet;
  }

HDF5RecordingData *NWBFile::createChannelConversionDataSet(String path, String description, int chunk_size)
{
   HDF5RecordingData *elSet = createDataSet(BaseDataType::F32, 1, chunk_size, path);
    
    if (!elSet)
      std::cerr << "Error creating electrode dataset in " << path << std::endl;
  else
  {
      CHECK_ERROR(setAttributeStr(description, path, "description"));
      CHECK_ERROR(setAttributeStr("hdmf-common", path, "namespace"));
      CHECK_ERROR(setAttributeStr(generateUuid(), path, "object_id"));
  }
  return elSet;
}

HDF5RecordingData *NWBFile::createElectrodeDataSet(String path, String description, int chunk_size)
{
	HDF5RecordingData *elSet = createDataSet(BaseDataType::I32, 1, chunk_size, path);
	if (!elSet)
		std::cerr << "Error creating electrode dataset in " << path << std::endl;
	else
	{
		CHECK_ERROR(setAttributeStr(description, path, "description"));
		CHECK_ERROR(setAttributeStr("hdmf-common", path, "namespace"));
		CHECK_ERROR(setAttributeStr("DynamicTableRegion", path, "neurodata_type"));
		CHECK_ERROR(setAttributeStr(generateUuid(), path, "object_id"));
		CHECK_ERROR(setAttributeRef("general/extracellular_ephys/electrodes", path, "table"));
	}
	return elSet;
}

  bool NWBFile::createExtraInfo(String basePath, String name, String desc, String id, uint16 index, uint16 typeIndex)
  {
	  if (createGroup(basePath)) return false;
	  CHECK_ERROR(setAttributeStr("openephys:<channel_info>/", basePath, "schema_id"));
	  CHECK_ERROR(setAttributeStr(name, basePath, "name"));
	  CHECK_ERROR(setAttributeStr(desc, basePath, "description"));
	  CHECK_ERROR(setAttributeStr(id, basePath, "identifier"));
	  CHECK_ERROR(setAttribute(BaseDataType::U16, &index, basePath, "source_index"));
	  CHECK_ERROR(setAttribute(BaseDataType::U16, &typeIndex, basePath, "source_type_index"));
	  return true;
  }

  bool NWBFile::createChannelMetadataSets(String basePath, const MetadataObject* info)
  {
	  if (!info) return false;
	  if (createGroup(basePath)) return false;
	  CHECK_ERROR(setAttributeStr("openephys:<metadata>/", basePath, "schema_id"));
	  int nMetadata = info->getMetadataCount();
	  
	  for (int i 
	  	= 0; i < nMetadata; i++)
	  {
		  const MetadataDescriptor* desc = info->getMetadataDescriptor(i);
		  String fieldName = "Field_" + String(i+1);
		  String name = desc->getName();
		  String description = desc->getDescription();
		  String identifier = desc->getIdentifier();
		  BaseDataType type = getMetadataH5Type(desc->getType(), desc->getLength()); //only string types use length, for others is always set to 1. If array types are implemented, change this
		  int length = desc->getType() == MetadataDescriptor::CHAR ? 1 : desc->getLength(); //strings are a single element of length set in the type (see above) while other elements are saved a
		  HeapBlock<char> data(desc->getDataSize());
		  info->getMetadataValue(i)->getValue(static_cast<void*>(data.getData()));
		  createBinaryDataSet(basePath, fieldName, type, length, data.getData());
		  String fullPath = basePath + "/" + fieldName;
		  CHECK_ERROR(setAttributeStr("openephys:<metadata>/", fullPath, "schema_id"));
		  CHECK_ERROR(setAttributeStr(name, fullPath, "name"));
		  CHECK_ERROR(setAttributeStr(description, fullPath, "description"));
		  CHECK_ERROR(setAttributeStr(identifier, fullPath, "identifier"));
	  }
	  return true;
  }

 
  bool NWBFile::createEventMetadataSets(String basePath, TimeSeries* timeSeries, const MetadataEventObject* info)
  {
	  if (!info) return false;
	  if (createGroup(basePath)) return false;
	  CHECK_ERROR(setAttributeStr("openephys:<metadata>/", basePath, "schema_id"));
	  int nMetadata = info->getEventMetadataCount();

	  timeSeries->metaDataSet.clear(); //just in case
	  for (int i = 0; i < nMetadata; i++)
	  {
		  const MetadataDescriptor* desc = info->getEventMetadataDescriptor(i);
		  String fieldName = "Field_" + String(i+1);
		  String name = desc->getName();
		  String description = desc->getDescription();
		  String identifier = desc->getIdentifier();
		  BaseDataType type = getMetadataH5Type(desc->getType(), desc->getLength()); //only string types use length, for others is always set to 1. If array types are implemented, change this
		  int length = desc->getType() == MetadataDescriptor::CHAR ? 1 : desc->getLength(); //strings are a single element of length set in the type (see above) while other elements are saved as arrays
		  String fullPath = basePath + "/" + fieldName;
		  HDF5RecordingData* dSet = createDataSet(type, 0, length, EVENT_CHUNK_SIZE, fullPath);
		  if (!dSet) return false;
		  timeSeries->metaDataSet.add(dSet);

		  CHECK_ERROR(setAttributeStr("openephys:<metadata>/", fullPath, "schema_id"));
		  CHECK_ERROR(setAttributeStr(name, fullPath, "name"));
		  CHECK_ERROR(setAttributeStr(description, fullPath, "description"));
		  CHECK_ERROR(setAttributeStr(identifier, fullPath, "identifier"));
	  }
      return true;
  }

  void NWBFile::writeEventMetadata(TimeSeries* timeSeries, const MetadataEventObject* info, const MetadataEvent* event)
  {
	  jassert(timeSeries->metaDataSet.size() == event->getMetadataValueCount());
	  jassert(info->getEventMetadataCount() == event->getMetadataValueCount());
	  int nMetadata = event->getMetadataValueCount();
	  for (int i = 0; i < nMetadata; i++)
	  {
		  BaseDataType type = getMetadataH5Type(info->getEventMetadataDescriptor(i)->getType(), info->getEventMetadataDescriptor(i)->getLength());
		  timeSeries->metaDataSet[i]->writeDataBlock(1, type, event->getMetadataValue(i)->getRawValuePointer());
	  }

  }
 
  void NWBFile::createTextDataSet(String path, String name, String text)
  {
	  ScopedPointer<HDF5RecordingData> dSet;

	  if (text.isEmpty()) text = " "; //to avoid 0-length strings, which cause errors
	  BaseDataType type = BaseDataType::STR(text.length());

	  dSet = createDataSet(type, 1, 0, path + "/" + name);
	  if (!dSet) return;
	  dSet->writeDataBlock(1, type, text.toUTF8());
  }

  void NWBFile::createBinaryDataSet(String path, String name, BaseDataType type, int length, void* data)
  {
	  ScopedPointer<HDF5RecordingData> dSet;
	  if ((length < 1) || !data) return;

	  dSet = createDataSet(type, 1, length, 1, path + "/" + name);
	  if (!dSet) return;
	  dSet->writeDataBlock(1, type, data);
  }

String NWBFile::generateUuid()
{
    Uuid id;
    return id.toDashedString();
}

  //These two methods whould be easy to adapt to support array types for all base types, for now
  //length is only used for string types.
  NWBFile::BaseDataType NWBFile::getEventH5Type(EventChannel::Type type, int length)
  {
	  switch (type)
	  {
	  case EventChannel::INT8_ARRAY:
		  return BaseDataType::I8;
	  case EventChannel::UINT8_ARRAY:
		  return BaseDataType::U8;
	  case EventChannel::INT16_ARRAY:
		  return BaseDataType::I16;
	  case EventChannel::UINT16_ARRAY:
		  return BaseDataType::U16;
	  case EventChannel::INT32_ARRAY:
		  return BaseDataType::I32;
	  case EventChannel::UINT32_ARRAY:
		  return BaseDataType::U32;
	  case EventChannel::INT64_ARRAY:
		  return BaseDataType::I64;
	  case EventChannel::UINT64_ARRAY:
		  return BaseDataType::U64;
	  case EventChannel::FLOAT_ARRAY:
		  return BaseDataType::F32;
	  case EventChannel::DOUBLE_ARRAY:
		  return BaseDataType::F64;
	  case EventChannel::TEXT:
		  return BaseDataType::STR(length);
	  default:
		  return BaseDataType::I8;
	  }
  }
  NWBFile::BaseDataType NWBFile::getMetadataH5Type(MetadataDescriptor::MetadataType type, int length)
  {
	  switch (type)
	  {
	  case MetadataDescriptor::INT8:
		  return BaseDataType::I8;
	  case MetadataDescriptor::UINT8:
		  return BaseDataType::U8;
	  case MetadataDescriptor::INT16:
		  return BaseDataType::I16;
	  case MetadataDescriptor::UINT16:
		  return BaseDataType::U16;
	  case MetadataDescriptor::INT32:
		  return BaseDataType::I32;
	  case MetadataDescriptor::UINT32:
		  return BaseDataType::U32;
	  case MetadataDescriptor::INT64:
		  return BaseDataType::I64;
	  case MetadataDescriptor::UINT64:
		  return BaseDataType::U64;
	  case MetadataDescriptor::FLOAT:
		  return BaseDataType::F32;
	  case MetadataDescriptor::DOUBLE:
		  return BaseDataType::F64;
	  case MetadataDescriptor::CHAR:
		  return BaseDataType::STR(length);
	  default:
		  return BaseDataType::I8;
	  }
  }

void NWBFile::cacheSpecifications(String specPath)
{
  File currentFile(__FILE__);
  File schemaDir = currentFile.getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("Resources/NWBSchema/" + specPath);
  for (auto& schemaFile : schemaDir.findChildFiles(File::findFiles, false, "*.json")){
      String specName = schemaFile.getFileNameWithoutExtension();
      if (specName.contains("namespace")) specName = "namespace";
      createStringDataSet("/specifications/" + specPath + specName, schemaFile.loadFileAsString());
  }
}
