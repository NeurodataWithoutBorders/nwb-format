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
#include "Channel.hpp"
#include "Utils.hpp"

#include "../../plugin-GUI/Source/Processors/RecordNode/RecordNode.h"


#define MAX_BUFFER_SIZE 40960
 
 using namespace NWBRecording;
 
 NWBRecordEngine::NWBRecordEngine()
 {
 }

 NWBRecordEngine::~NWBRecordEngine()
 {    
   if (this->nwbfile != nullptr)
   {
       
       this->continuousChannels.clear();
       this->continuousChannelGroups.clear();
       this->spikeChannels.clear();
       
       this->recordingArrays.clear();
       this->spikeRecordingArrays.clear();
       this->esContainerIndexes.clear();
       this->spikeContainerIndexes.clear();

       this->nwbfile->finalize();
       this->nwbfile.reset();
   }
 }

RecordEngineManager* NWBRecordEngine::getEngineManager()
{
    //static factory that instantiates the engine manager, which allows to configure recording options among other things. See OriginalRecording to see how to create options for a record engine
    RecordEngineManager* man = new RecordEngineManager("NWB2", "NWB2", &(engineFactory<NWBRecordEngine>));
    EngineParameter* param;
    param = new EngineParameter(EngineParameter::STR, 0, "Identifier Text", String());
    man->addParameter(param);
    return man;
}


 void NWBRecordEngine::openFiles(File rootFolder, int experimentNumber, int recordingNumber)
 {  
     if (recordingNumber == 0) // new file needed
     {  
        // clear any existing data and nwbfile
        this->continuousChannels.clear();
        this->continuousChannelGroups.clear();
        this->spikeChannels.clear();
        
        this->recordingArrays.clear();
        this->spikeRecordingArrays.clear();
        this->esContainerIndexes.clear();
        this->spikeContainerIndexes.clear();
        
        if (this->nwbfile != nullptr)
        {
            this->nwbfile->finalize();
            this->nwbfile.reset();
        }

        // create the io object
        char separator = std::filesystem::path::preferred_separator;
        std::string separatorStr(1, separator); // Convert char to std::string
        std::string filename = rootFolder.getFullPathName().toStdString() + separatorStr + 
        "experiment_aqnwb" + std::to_string(experimentNumber) + ".nwb";

        this->io = AQNWB::createIO("HDF5", filename);

        // create recording array mapping for channel information
        NWBRecordEngine::createRecordingArrays();

        // create the nwbfile
        std::string dataCollection = "Open Ephys GUI Version " + CoreServices::getGUIVersion().toStdString();
        this->nwbfile = std::make_unique<AQNWB::NWB::NWBFile>(AQNWB::generateUuid(), io);
        this->nwbfile->initialize("Recording with the Open Ephys GUI", dataCollection);  
        // TODO - have option to initialize cache size based on # of channels

        // create recording containers
        this->recordingContainers = std::make_unique<AQNWB::NWB::RecordingContainers>();
        this->nwbfile->createElectricalSeries(
            this->recordingArrays, this->recordingArraysNames, AQNWB::BaseDataType::I16, this->recordingContainers.get(), this->esContainerIndexes); 
        // TODO add io_settings to set chunk size for different data types

        this->nwbfile->createSpikeEventSeries(
            this->spikeRecordingArrays, this->spikeRecordingArraysNames, AQNWB::BaseDataType::I16, this->recordingContainers.get(), this->spikeContainerIndexes);       

        // start recording
        this->io->startRecording();
    }
 }



 void NWBRecordEngine::closeFiles()
 {
    this->io->stopRecording();
    this->nwbfile->finalize();
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
    for (auto& channelVector : this->recordingArrays) {
        for (auto& ch : channelVector) {
            if (ch.globalIndex == realChannel) {
                channel = &ch;
                break;
            }
        }
    }

    std::unique_ptr<int16_t[]> intBuffer = AQNWB::transformToInt16(static_cast<SizeType>(size), channel->getBitVolts(), dataBuffer);
    this->recordingContainers->writeElectricalSeriesData(this->esContainerIndexes[channel->groupIndex],
                                            *channel,
                                            static_cast<SizeType>(size),
                                            intBuffer.get(),
                                            timestampBuffer);

    // TODO - save sample numbers as well for offline syncing
}
 
void NWBRecordEngine::writeEvent(int eventIndex, const MidiMessage& event) 
{   
    // TODO - replace with AQNWB
	// const EventChannel* channel = getEventChannel(eventIndex);
	// EventPtr eventStruct = Event::deserialize(event, channel);

	// nwb->writeEvent(eventIndex, channel, eventStruct);
}

void NWBRecordEngine::writeTimestampSyncText(uint64 streamId, int64 timestamp, float sourceSampleRate, String text)
{
    // TODO - replacew with AQNWB
	// nwb->writeTimestampSyncText(streamId, timestamp, sourceSampleRate, text);
}


void NWBRecordEngine::writeSpike(int electrodeIndex, const Spike* spike) 
{
    // extract info from spike channel
	const SpikeChannel* spikeChannel = getSpikeChannel(electrodeIndex);
    SizeType numSamplesPerChannel = static_cast<SizeType>(spikeChannel->getTotalSamples());
    SizeType numChannels = static_cast<SizeType>(spikeChannel->getNumChannels());
    SizeType numSamples = numSamplesPerChannel * numChannels;
    
    // extract info from spike object
    double timestamps = spike->getTimestampInSeconds();
    std::unique_ptr<int16_t[]> intData = AQNWB::transformToInt16(static_cast<SizeType>(numSamples), 
                                                                 spikeChannel->getSourceChannels()[0]->getBitVolts(),
                                                                 spike->getDataPointer());

    // write spike data
    this->recordingContainers->writeSpikeEventData(this->spikeContainerIndexes[electrodeIndex],
                                                   numSamplesPerChannel,
                                                   numChannels,
                                                   intData.get(),
                                                   &timestamps);

    // TODO - add writeEventMetadata functionalities
}

void NWBRecordEngine::setParameter(EngineParameter& parameter)
{
	strParameter(0, identifierText);
}

void NWBRecordEngine::createRecordingArrays()
{
    // get pointers to all continuous channels for electrode table
    for (int i = 0; i < recordNode->getNumOutputs(); i++)
    {
        const ContinuousChannel* channelInfo = getContinuousChannel(i); // channel info object
        this->continuousChannels.add(channelInfo);
    }

    // add continuous channels
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
            this->continuousChannelGroups.add(newGroup);
        }

        this->continuousChannelGroups.getReference(streamIndex).add(channelInfo);
        lastStreamId = channelInfo->getStreamId();        
    }

    // add spike channels
    for (int i = 0; i < getNumRecordedSpikeChannels(); i++) {
        spikeChannels.add(getSpikeChannel(i));
    }

    // create recording arrays for continuous groups in nwb file
    for (int streamIndex = 0; streamIndex < this->continuousChannelGroups.size(); streamIndex++)
    {
        std::vector<AQNWB::Channel> channelVector;

        for (auto& channelInfo : this->continuousChannelGroups[streamIndex]) 
        {
            std::string name = channelInfo->getName().toStdString();
            std::string groupName = channelInfo->getSourceNodeName().toStdString() + "-"
            + std::to_string(channelInfo->getSourceNodeId())
            + "." + channelInfo->getStreamName().toStdString();            

            channelVector.push_back(AQNWB::Channel(name, 
                                                   groupName,
                                                   streamIndex, 
                                                   channelInfo->getLocalIndex(),
                                                   channelInfo->getGlobalIndex(),
                                                   1e6,
                                                   channelInfo->getSampleRate(), 
                                                   channelInfo->getBitVolts()));
        }
        this->recordingArrays.push_back(channelVector);
        this->recordingArraysNames.push_back(channelVector[0].groupName);
    }

    // create recording arrays for spike channels in nwb file
    for (int i = 0; i < this->spikeChannels.size(); i++)
    {
        std::vector<AQNWB::Channel> channelVector;

        const SpikeChannel* spikeChannel = this->spikeChannels[i];
        std::string sourceName = spikeChannel->getSourceNodeName().toStdString() + "-"
        + std::to_string(spikeChannel->getSourceNodeId())
        + "." + spikeChannel->getStreamName().toStdString() + '.' + spikeChannel->getName().toStdString();

        for (int ch = 0; ch < spikeChannel->getNumChannels(); ch++)
        {   
            const ContinuousChannel* schan = spikeChannel->getSourceChannels()[ch];
            std::string continuousSourceName = schan->getSourceNodeName().toStdString() + "-"
            + std::to_string(schan->getSourceNodeId())
            + "." + schan->getStreamName().toStdString();  
            AQNWB::Channel channel(schan->getName().toStdString(), 
                                   continuousSourceName,
                                   i,
                                   schan->getLocalIndex(),
                                   schan->getGlobalIndex(),
                                   1e6,
                                   schan->getSampleRate(), 
                                   schan->getBitVolts());
            channelVector.push_back(channel);
        }
        this->spikeRecordingArrays.push_back(channelVector);
        this->spikeRecordingArraysNames.push_back(sourceName);
    }
}
