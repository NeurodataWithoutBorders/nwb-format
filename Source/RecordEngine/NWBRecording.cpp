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
 
#include <filesystem>
#include <string>

#include "NWBRecording.h"
#include "aqnwb/Channel.hpp"
#include "aqnwb/Utils.hpp"

#include "../../plugin-GUI/Source/Processors/RecordNode/RecordNode.h"


#define MAX_BUFFER_SIZE 40960
 
 using namespace NWBRecording;
 
 NWBRecordEngine::NWBRecordEngine()
 {
 }


 NWBRecordEngine::~NWBRecordEngine()
 {    
    //  if (nwb != nullptr)
    //  {
    //      spikeChannels.clear();
    //      eventChannels.clear();
    //      continuousChannelGroups.clear();
    //      datasetIndexes.clear();
    //      writeChannelIndexes.clear();

    //      nwb->close();
    //      nwb.reset();
    //  }
 }

RecordEngineManager* NWBRecordEngine::getEngineManager()
{
    //static factory that instantiates the engine manager, which allows to configure recording options among other things. See OriginalRecording to see how to create options for a record engine
    RecordEngineManager* man = new RecordEngineManager("NWB2", "NWB2", &(engineFactory<NWBRecordEngine>));
    EngineParameter* param;
    // param = new EngineParameter(EngineParameter::STR, 0, "Identifier Text", String());
    // man->addParameter(param);
    return man;
    
}
 
 void NWBRecordEngine::openFiles(File rootFolder, int experimentNumber, int recordingNumber)
 {  
    // setup file paths    
    char separator = std::filesystem::path::preferred_separator;
    std::string separatorStr(1, separator); // Convert char to std::string
    std::string filename = rootFolder.getFullPathName().toStdString() + separatorStr + 
    "experiment_aqnwb" + std::to_string(experimentNumber) + ".nwb";
    // std::string guiVersion = CoreServices::getGUIVersion();

    // get pointers to all continuous channels for electrode table
    std::vector<AQNWB::Channel> channelVector;  
    for (int i = 0; i < recordNode->getNumOutputs(); i++)
    {
        const ContinuousChannel* channelInfo = getContinuousChannel(i); // channel info object
        continuousChannels.add(channelInfo);
    }

    int streamIndex = -1;
    uint16 lastStreamId = 0;

    for (int ch = 0; ch < getNumRecordedContinuousChannels(); ch++)
    {
        int globalIndex = getGlobalIndex(ch); // the global channel index (across all channels entering the Record Node)
        int localIndex = getLocalIndex(ch);   // the local channel index (within a stream)

        const ContinuousChannel* channelInfo = getContinuousChannel(globalIndex); // channel info object
        if (channelInfo->getStreamId() != lastStreamId)
        {
            streamIndex++;
            ContinuousGroup newGroup;
            continuousChannelGroups.add(newGroup);
        }

        continuousChannelGroups.getReference(streamIndex).add(channelInfo);
        lastStreamId = channelInfo->getStreamId();

        // TODO - part I'm adding
        std::string name = channelInfo->getName().toStdString();
        std::string groupName = channelInfo->getSourceNodeName().toStdString() + "-"
			+ std::to_string(channelInfo->getSourceNodeId())
			+ "." + channelInfo->getStreamName().toStdString();

        float channel_conversion = channelInfo->getBitVolts() * 1e6;
        channelVector.push_back(AQNWB::Channel(name, groupName, streamIndex, localIndex, globalIndex, channel_conversion, channelInfo->getSampleRate(), channelInfo->getBitVolts()));
    }

    // TODO - I don't think this properly separates different streams / recording arrays right now
    this->recordingArrays = {channelVector};

    // initialize nwbfile object and create base structure
    nwbRecording.openFile(filename, recordingArrays);


    //  if (recordingNumber == 0) // new file needed
    //  {

    //      spikeChannels.clear();
    //      eventChannels.clear();
    //      continuousChannels.clear();
    //      continuousChannelGroups.clear();
    //      datasetIndexes.clear();
    //      writeChannelIndexes.clear();

    //      // New file for each experiment, e.g. experiment1.nwb, epxperiment2.nwb, etc.
    //      String basepath = rootFolder.getFullPathName() +
    //          rootFolder.getSeparatorString() +
    //          "experiment" + String(experimentNumber) +
    //          ".nwb";
         
    //      if (nwb != nullptr)
    //      {
    //          nwb->close();
    //          nwb.reset();
    //      }

    //      // create a unique identifier for the file if it doesn't exist
    //      Uuid identifier;
    //      identifierText = identifier.toString();

    //      nwb = std::make_unique<NWBFile>(basepath, CoreServices::getGUIVersion(), identifierText);

    //      // get pointers to all continuous channels for electrode table
    //      for (int i = 0; i < recordNode->getNumOutputs(); i++)
    //      {
    //          const ContinuousChannel* channelInfo = getContinuousChannel(i); // channel info object

    //          continuousChannels.add(channelInfo);
    //      }

    //      datasetIndexes.insertMultiple(0, 0, getNumRecordedContinuousChannels());
    //      writeChannelIndexes.insertMultiple(0, 0, getNumRecordedContinuousChannels());
    //      continuousChannelGroups.clear();

    //      int streamIndex = -1;
    //      uint16 lastStreamId = 0;
    //      int indexWithinStream = 0;

    //      for (int ch = 0; ch < getNumRecordedContinuousChannels(); ch++)
    //      {

    //          int globalIndex = getGlobalIndex(ch); // the global channel index (across all channels entering the Record Node)
    //          int localIndex = getLocalIndex(ch);   // the local channel index (within a stream)

    //          const ContinuousChannel* channelInfo = getContinuousChannel(globalIndex); // channel info object

    //          int sourceId = channelInfo->getSourceNodeId();
    //          int streamId = channelInfo->getStreamId();

    //          if (streamId != lastStreamId)
    //          {
    //              streamIndex++;
    //              indexWithinStream = 0;

    //              ContinuousGroup newGroup;
    //              continuousChannelGroups.add(newGroup);

    //          }

    //          continuousChannelGroups.getReference(streamIndex).add(channelInfo);

    //          datasetIndexes.set(ch, streamIndex);
    //          writeChannelIndexes.set(ch, indexWithinStream++);

    //          lastStreamId = streamId;
    //      }

    //      for (int i = 0; i < getNumRecordedEventChannels(); i++)
    //          eventChannels.add(getEventChannel(i));

    //      for (int i = 0; i < getNumRecordedSpikeChannels(); i++)
    //          spikeChannels.add(getSpikeChannel(i));

    //      //open the file
    //      nwb->open(getNumRecordedContinuousChannels() + continuousChannelGroups.size() + eventChannels.size() + spikeChannels.size()); //total channels + timestamp arrays, to create a big enough buffer

    //      //create the recording
    //      nwb->startNewRecording(recordingNumber, continuousChannelGroups, continuousChannels, eventChannels, spikeChannels);
    //  }
 }


 void NWBRecordEngine::closeFiles()
 {
    nwbRecording.closeFile();
	//  nwb->stopRecording();
 }

void NWBRecordEngine::writeContinuousData(int writeChannel,
                                          int realChannel,
                                          const float* dataBuffer,
                                          const double* timestampBuffer,
                                          int size)
{   
    // get channel info - add this to RecordingArray or ChannelVector when we make it a class
    AQNWB::Channel* channel = nullptr;
    AQNWB::Types::SizeType datasetIndex = 0;
    for (auto& channelVector : recordingArrays) {
        for (auto& ch : channelVector) {
            if (ch.globalIndex == realChannel) {
                datasetIndex = ch.groupIndex; 
                channel = &ch;
                break;
            }
        }
    }

    // get data info
    std::vector<SizeType> dataShape = {static_cast<SizeType>(size), 1};
    std::vector<SizeType> positionOffset = {static_cast<SizeType>(0), static_cast<SizeType>(writeChannel)};
    // TODO - update positionOffset tracking

    // write data
    // std::unique_ptr<int16_t[]> intBuffer = AQNWB::transformToInt16(static_cast<SizeType>(size), channel->getBitVolts() / 1e6, dataBuffer);
    nwbRecording.writeTimeseriesData("ElectricalSeries",
                                     datasetIndex,
                                     *channel,  // to do, get recording channel information
                                     dataShape,
                                     positionOffset,
                                     intBuffer.get(),
                                     timestampBuffer);

    // nwb->writeData(datasetIndexes[writeChannel], 
    //     writeChannelIndexes[writeChannel],
    //     size, 
    //     dataBuffer, 
    //     getContinuousChannel(realChannel)->getBitVolts());

    // /* All channels in a dataset have the same number of samples and share timestamps. 
    //    But since this method is called asynchronously, the timestamps might not be 
    //    in sync during acquisition, so we chose a channel and write the timestamps 
    //    when writing that channel's data */
    // if (writeChannelIndexes[writeChannel] == 0)
    // {
    //     int64 baseTS = getLatestSampleNumber(writeChannel);
        
    //     for (int i = 0; i < size; i++)
    //     {
    //         smpBuffer[i] = baseTS + i;
    //     }
        
    //     nwb->writeTimestamps(datasetIndexes[writeChannel], size, timestampBuffer);
    //     nwb->writeSampleNumbers(datasetIndexes[writeChannel], size, smpBuffer);
    // }
}
 
void NWBRecordEngine::writeEvent(int eventIndex, const MidiMessage& event) 
{
	// const EventChannel* channel = getEventChannel(eventIndex);
	// EventPtr eventStruct = Event::deserialize(event, channel);

	// nwb->writeEvent(eventIndex, channel, eventStruct);
}

void NWBRecordEngine::writeTimestampSyncText(uint64 streamId, int64 timestamp, float sourceSampleRate, String text)
{
	// nwb->writeTimestampSyncText(streamId, timestamp, sourceSampleRate, text);
}


void NWBRecordEngine::writeSpike(int electrodeIndex, const Spike* spike) 
{
	// const SpikeChannel* channel = getSpikeChannel(electrodeIndex);

	// nwb->writeSpike(electrodeIndex, channel, spike);
}



// void NWBRecordEngine::setParameter(EngineParameter& parameter)
// {
// 	strParameter(0, identifierText);
// }
