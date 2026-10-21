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

#ifndef NWBFILESOURCE_H_INCLUDED
#define NWBFILESOURCE_H_INCLUDED

#include <FileSourceHeaders.h>

#include <memory>
#include <string>
#include <vector>

#include "io/BaseIO.hpp"

/**
    A File Source plugin that can read data from NWB 2.x files using AqNWB

    Reads the ElectricalSeries in /acquisition as continuous data, and TTL and text events
    from the EventsTables in /events (or, for files written by earlier versions of the plugin,
    from the TTL TimeSeries in /acquisition).
 */
class NWBFileSource : public FileSource
{
public:
    /** Constructor */
    NWBFileSource();

    /** Destructor */
    ~NWBFileSource();

    /** Attempt to open the file, and return true if successful */
    bool open (File file) override;

    /** Fill the infoArray and eventInfoMap with the relevant information for all recordings*/
    void fillRecordInfo() override;

    /** Update the recording to be read in */
    void updateActiveRecord (int index) override;

    /** Reads nSamples of data from the active recording */
    int readData (float* buffer, int nSamples) override;

    /** Seek to a specific sample number within the active recording */
    void seekTo (int64 sample) override;

    /** Add info about events occurring within a sample range */
    void processEventData (EventInfo& info, int64 startTimestamp, int64 stopTimestamp) override;

    /** Return false if file is not able to be opened */
    bool isReady() override;

private:
    /** A range of consecutive rows of an ElectricalSeries with consecutive sample numbers */
    struct Segment
    {
        int64 firstRow;
        int64 firstSampleNumber;
        int64 numRows;
    };

    /** Information about an ElectricalSeries needed for reading */
    struct StreamInfo
    {
        std::string path;
        int64 numSamples = 0;
        int numChannels = 0;
        std::vector<float> scale; ///< factor to convert the stored values to microvolts, per channel
        float offset = 0.0f; ///< offset in microvolts
        std::vector<Segment> segments; ///< sample numbers of the rows (empty if unknown)
    };

    /** Read the information about the ElectricalSeries at the given path. Returns false if not supported */
    bool readStreamInfo (const std::string& path, StreamInfo& stream, RecordInfo& info);

    /** Read the sample numbers of the ElectricalSeries (if available) as segments of consecutive sample numbers */
    std::vector<Segment> readSegments (const std::string& sampleNumbersPath, int64 numRows);

    /** Row of the active ElectricalSeries for a sample number, or -1 if the sample number was not recorded */
    static int64 getRow (const StreamInfo& stream, int64 sampleNumber);

    /** Read the TTL and text events from the EventsTables in /events */
    void readEventsTables();

    /** Read the TTL events from legacy TTL TimeSeries in /acquisition (files written by the original plugin) */
    void readLegacyEvents (const std::string& path);

    /** Add an event to the events of a stream */
    void addEvent (const String& streamName, int16 channel, int16 state, int64 row, const String& text);

    /** Read a 1D dataset as a vector of doubles (or int64) */
    std::vector<double> readDoubles (const std::string& path, int64 start = 0, int64 count = -1) const;
    std::vector<int64> readInt64s (const std::string& path, int64 start = 0, int64 count = -1) const;
    std::vector<std::string> readStrings (const std::string& path) const;

    /** True if the object at the path exists and is a dataset */
    bool isDataset (const std::string& path) const;

    std::shared_ptr<AQNWB::IO::BaseIO> io;

    /** One StreamInfo per record (ElectricalSeries) */
    std::vector<StreamInfo> streams;

    int64 samplePos;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NWBFileSource);
};

#endif // NWBFILESOURCE_H_INCLUDED
