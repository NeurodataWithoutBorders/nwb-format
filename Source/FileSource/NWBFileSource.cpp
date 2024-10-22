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

#include <H5Cpp.h>
#include "NWBFileSource.h"
#include <CoreServicesHeader.h>

using namespace H5;

#define PROCESS_ERROR std::cerr << "NWBFilesource exception: " << error.getCDetailMsg() << std::endl

NWBFileSource::NWBFileSource() : samplePos (0), skipRecordEngineCheck (false)
{
}

NWBFileSource::~NWBFileSource()
{
}

bool NWBFileSource::open (File file)
{
    ScopedPointer<H5File> tmpFile;
    Attribute ver;
    uint16 vernum;
    try
    {
        tmpFile = new H5File (file.getFullPathName().toUTF8(), H5F_ACC_RDONLY);

        //TODO: Verify NWBVersion

        sourceFile = tmpFile;
        return true;
    }
    catch (FileIException error)
    {
        PROCESS_ERROR;
        return false;
    }
    catch (AttributeIException error)
    {
        PROCESS_ERROR;
        return false;
    }

    //Code should never reach here
    return false;
}

void NWBFileSource::fillRecordInfo()
{
    Group acquisition;

    try
    {
        acquisition = sourceFile->openGroup ("/acquisition/");

        int dataSources = (int) acquisition.getNumObjs();

        std::map<String, int64> startSampleNumbers;

        for (int i = 0; i < dataSources; i++)
        {
            try
            {
                DataSet data;
                Attribute attr;
                DataSpace dSpace;
                float sampleRate;
                float bitVolts;
                hsize_t dims[3];

                H5std_string dataSourceName = acquisition.getObjnameByIdx (hsize_t (i));
                Group dataSource = acquisition.openGroup (dataSourceName);

                if (dataSource.attrExists ("neurodata_type"))
                {
                    attr = dataSource.openAttribute ("neurodata_type");
                    H5::StrType type = attr.getStrType();
                    std::string type_str;
                    attr.read (type, type_str);

                    if (! type_str.compare ("ElectricalSeries"))
                    {
                        RecordInfo info;

                        data = dataSource.openDataSet ("data");

                        dSpace = data.getSpace();
                        dSpace.getSimpleExtentDims (dims);

                        info.name = dataSourceName;
                        info.numSamples = dims[0];

                        attr = data.openAttribute ("conversion");
                        attr.read (PredType::NATIVE_FLOAT, &bitVolts);

                        data = dataSource.openDataSet ("timestamps");

                        info.sampleRate = -1.0f;

                        if (data.attrExists ("interval"))
                        {
                            attr = data.openAttribute ("interval");
                            double interval;
                            attr.read (PredType::NATIVE_DOUBLE, &interval);
                            double sampleRate = 1.0f / interval;

                            info.sampleRate = sampleRate;
                        }
                        else
                        {
                            dSpace = data.getSpace();
                            dSpace.getSimpleExtentDims (dims);

                            HeapBlock<double> tsArray (dims[0]);
                            data.read (tsArray.getData(), PredType::NATIVE_DOUBLE);

                            if (tsArray[2] > 0 && tsArray[0] > 0)
                                info.sampleRate = 2 / (tsArray[2] - tsArray[0]);
                        }

                        //Get the first sample number to align events
                        data = dataSource.openDataSet ("sync");

                        dSpace = data.getSpace();
                        dSpace.getSimpleExtentDims (dims);

                        HeapBlock<int> syncArray (dims[0]);
                        data.read (syncArray.getData(), PredType::NATIVE_INT);

                        startSampleNumbers[dataSourceName] = syncArray[0];

                        HeapBlock<float> ccArray (dims[1]);
                        data = dataSource.openDataSet ("channel_conversion");
                        data.read (ccArray.getData(), PredType::NATIVE_FLOAT);

                        HeapBlock<uint8> ctArray (dims[1]);
                        data = dataSource.openDataSet ("channel_type");
                        data.read (ctArray.getData(), PredType::NATIVE_UINT8);

                        try
                        {
                            for (int k = 0; k < dims[1]; k++)
                            {
                                RecordedChannelInfo c;
                                c.name = "CH" + String (k);
                                c.bitVolts = ccArray[k] * 1e6; //TOFIX? Scaling should depend on channel type?
                                c.type = ctArray[k];
                                info.channels.add (c);
                            }
                            infoArray.add (info);
                            availableDataSets.add (numRecords);
                            dataPaths.set (numRecords, dataSourceName);
                            numRecords++;
                        }
                        catch (GroupIException)
                        {
                            std::cout << "!!!GroupIException!!!" << std::endl;
                        }
                        catch (AttributeIException)
                        {
                            std::cout << "!!!AttributeIException!!!" << std::endl;
                        }
                    }
                    else if (! type_str.compare ("TimeSeries"))
                    {
                        // Load TTL events
                        dataSourceName.erase (dataSourceName.find_last_not_of (".TTL") + 1);

                        EventInfo info;

                        data = dataSource.openDataSet ("data");

                        dSpace = data.getSpace();
                        dSpace.getSimpleExtentDims (dims);

                        int numEvents = dims[0];

                        HeapBlock<int> stateArray (dims[0]);
                        data.read (stateArray.getData(), PredType::NATIVE_INT);

                        data = dataSource.openDataSet ("sync");

                        HeapBlock<double> tsArray (dims[0]);
                        data.read (tsArray.getData(), PredType::NATIVE_DOUBLE);

                        for (int k = 0; k < numEvents; k++)
                        {
                            info.channels.push_back (abs (stateArray[k]));
                            info.channelStates.push_back (stateArray[k] > 0);
                            info.sampleNumbers.push_back (tsArray[k] - startSampleNumbers[dataSourceName]);
                        }

                        eventInfoMap[dataSourceName] = info;
                    }
                }
            }
            catch (GroupIException)
            {
                std::cout << "!!!GroupIException!!!" << std::endl;
            }
            catch (DataSetIException)
            {
                std::cout << "!!!DataSetIException!!!" << std::endl;
            }
            catch (AttributeIException)
            {
                std::cout << "!!!AttributeIException!!!" << std::endl;
            }
            catch (DataSpaceIException error)
            {
                std::cout << "!!!DataSpaceIException!!!" << std::endl;
            }
        }
    }
    catch (FileIException error)
    {
        std::cout << "!!!FileIException!!!" << std::endl;
        PROCESS_ERROR;
    }
    catch (GroupIException error)
    {
        std::cout << "!!!GroupIException!!!" << std::endl;
        PROCESS_ERROR;
    }
}

void NWBFileSource::updateActiveRecord (int index)
{
    samplePos = 0;

    try
    {
        String path = "/acquisition/" + dataPaths[index] + "/data";
        dataSet = new DataSet (sourceFile->openDataSet (path.toUTF8()));
    }
    catch (FileIException error)
    {
        PROCESS_ERROR;
    }
    catch (DataSetIException error)
    {
        PROCESS_ERROR;
    }

    currentStream = dataPaths[index];
}

void NWBFileSource::seekTo (int64 sample)
{
    samplePos = sample % getActiveNumSamples();
}

int NWBFileSource::readData (float* buffer, int nSamples)
{
    int64 samplesToRead;
    int nChannels = getActiveNumChannels();

    if (samplePos + nSamples > getActiveNumSamples())
    {
        samplesToRead = getActiveNumSamples() - samplePos;
    }
    else
    {
        samplesToRead = nSamples;
    }

    try
    {
        DataSpace fSpace = dataSet->getSpace();
        hsize_t dim[2] = {static_cast<hsize_t>(samplesToRead), static_cast<hsize_t>(nChannels)};
        hsize_t offset[2] = {static_cast<hsize_t>(samplePos), 0};

        fSpace.selectHyperslab(H5S_SELECT_SET, dim, offset);
        DataSpace mSpace(2, dim);

        // Read data into a temporary int16 buffer
        HeapBlock<int16> tempBuffer(samplesToRead * nChannels);
        dataSet->read(tempBuffer.getData(), PredType::NATIVE_INT16, mSpace, fSpace);

        // Convert int16 to float and apply bitVolts
        for (int i = 0; i < samplesToRead * nChannels; i++)
        {
            *(buffer + i) = tempBuffer[i] * getChannelInfo(activeRecord.get(), i % nChannels).bitVolts;
        }

        samplePos += samplesToRead;
        return static_cast<int>(samplesToRead);
    }
    catch (DataSetIException error)
    {
        PROCESS_ERROR;
        return 0;
    }
    catch (DataSpaceIException error)
    {
        PROCESS_ERROR;
        return 0;
    }
}

/* DEPRECATED: Convert nSamples of data from int16 to float
void NWBFileSource::processChannelData (int16* inBuffer, float* outBuffer, int channel, int64 numSamples)
{
    int n = getActiveNumChannels();
    float bitVolts = getChannelInfo (activeRecord.get(), channel).bitVolts;

    for (int i = 0; i < numSamples; i++)
    {
        *(outBuffer + i) = *(inBuffer + (n * i) + channel) * bitVolts;
    }
}
*/

void NWBFileSource::processEventData (EventInfo& eventInfo, int64 start, int64 stop)
{
    int local_start = start % getActiveNumSamples();
    ;
    int local_stop = stop % getActiveNumSamples();
    int loop_count = start / getActiveNumSamples();

    EventInfo info = eventInfoMap[currentStream];

    int i = 0;

    while (i < info.sampleNumbers.size())
    {
        if (info.sampleNumbers[i] >= local_start && info.sampleNumbers[i] < local_stop)
        {
            eventInfo.channels.push_back (info.channels[i] - 1);
            eventInfo.channelStates.push_back ((info.channelStates[i]));
            eventInfo.sampleNumbers.push_back (info.sampleNumbers[i] + loop_count * getActiveNumSamples());
        }
        i++;
    }
}

bool NWBFileSource::isReady()
{
    /*
	//HDF5 is by default not thread-safe, so we must warn the user.
	if ((!skipRecordEngineCheck) && (CoreServices::getSelectedRecordEngineId() == "NWB"))
	{
		int res = AlertWindow::showYesNoCancelBox(AlertWindow::WarningIcon, "Record format conflict",
			"Both the selected input file for the File Reader and the output file format for recording use the HDF5 library.\n"
			"This library is, by default, not thread safe, so running both at the same time might cause unexpected crashes (chances increase with signal complexity and number of recorded channels).\n\n"
			"If you have a custom-built hdf5 library with the thread safe features turned on, you can safely continue, but performance will be reduced.\n"
			"More information on:\n"
			"https://www.hdfgroup.org/HDF5/doc/TechNotes/ThreadSafeLibrary.html\n"
			"https://www.hdfgroup.org/hdf5-quest.html\n\n"
			"Do you want to continue acquisition?", "Yes", "Yes and don't ask again", "No");
		switch (res)
		{
		case 2:
			skipRecordEngineCheck = true;
		case 1:
			return true;
			break;
		default:
			return false;
		}
	}
	else
		return true;
    */

    return true;
}
