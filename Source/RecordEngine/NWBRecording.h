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

#ifndef NWBRECORDING_H
#define NWBRECORDING_H

#include <RecordingLib.h>

#include "../aqnwb/nwb/NWBRecording.hpp"

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
        void openFiles(File rootFolder, int experimentNumber, int recordingNumber) override;

        /** Called when recording stops to close all files and do all the necessary cleanup */
        void closeFiles() override;

        /** Write continuous data for a channel, including synchronized float timestamps for each sample */
        void writeContinuousData(int writeChannel,
                                 int realChannel,
                                 const float *dataBuffer,
                                 const double *timestampBuffer,
                                 int size) override;

        /** Write a single event to disk (TTL or TEXT) */
        void writeEvent(int eventIndex, const MidiMessage &event) override;

        /** Write a spike to disk */
        void writeSpike(int electrodeIndex, const Spike *spike) override;

        /** Write the timestamp sync text messages to disk*/
        void writeTimestampSyncText(uint64 streamId, int64 timestamp, float sourceSampleRate, String text) override;

        // /** Allows the file identifier to be set externally*/
        // void setParameter(EngineParameter &parameter) override;

    private:
        /** NWB recording manager */
        AQNWB::NWB::NWBRecording nwbRecording;

        /** Holds channel information and ids */
        std::vector<AQNWB::Types::ChannelVector> recordingArrays;

        // /** For each incoming recorded channel, which dataset (stream) is it associated with? */
        // Array<int> datasetIndexes;

        // /** For each incoming recorded channel, what is the local index within a stream? */
        // Array<int> writeChannelIndexes;

        /** Holds pointers to all recorded channels within a stream */
        Array<ContinuousGroup> continuousChannelGroups;

        // /** Holds pointers to all recorded event channels*/
        // Array<const EventChannel*> eventChannels;

        // /** Holds pointers to all recorded spike channels*/
        // Array<const SpikeChannel*> spikeChannels;

        /** Holds pointers to all incoming continuous channels (used for electrode table)*/
        Array<const ContinuousChannel*> continuousChannels;

        /** Holds integer sample numbers for writing */
        HeapBlock<int64> smpBuffer;

        // /** The identifier for the current file (can be set externally) */
        // String identifierText;

        // JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NWBRecordEngine);
    };
}

#endif
