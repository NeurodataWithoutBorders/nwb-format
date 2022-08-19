/*
    ------------------------------------------------------------------

    This file is part of the Open Ephys GUI
    Copyright (C) 2013 Open Ephys

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

NWBFileSource::NWBFileSource() : samplePos(0), skipRecordEngineCheck(false)
{
}

NWBFileSource::~NWBFileSource()
{
}

bool NWBFileSource::open(File file)
{
    ScopedPointer<H5File> tmpFile;
    Attribute ver;
    uint16 vernum;
    try
    {
        tmpFile = new H5File(file.getFullPathName().toUTF8(),H5F_ACC_RDONLY);

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

        acquisition = sourceFile->openGroup("/acquisition/");
        
        int dataSources = (int) acquisition.getNumObjs();

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

                H5std_string dataSourceName = acquisition.getObjnameByIdx(hsize_t(i));
                Group dataSource = acquisition.openGroup(dataSourceName);

                if (dataSource.attrExists("neurodata_type"))
                {
                    attr = dataSource.openAttribute("neurodata_type");
                    H5::StrType type = attr.getStrType();
                    std::string type_str;
                    attr.read(type, type_str);

                    if (!type_str.compare("ElectricalSeries"))
                    {

                        RecordInfo info;

                        data = dataSource.openDataSet("data");

                        dSpace = data.getSpace();
                        dSpace.getSimpleExtentDims(dims);

                        info.name = dataSourceName;
                        info.numSamples = dims[0];

                        attr = data.openAttribute("conversion");
                        attr.read(PredType::NATIVE_FLOAT, &bitVolts);

                        //Compute sample rate from first few timestamps
                        data = dataSource.openDataSet("timestamps");

                        dSpace = data.getSpace();
                        dSpace.getSimpleExtentDims(dims);

                        HeapBlock<double> tsArray(dims[0]);
                        data.read(tsArray.getData(), PredType::NATIVE_DOUBLE);

                        info.sampleRate = 2 / (tsArray[2] - tsArray[0]);

                        HeapBlock<float> ccArray(dims[1]);
                        data = dataSource.openDataSet("channel_conversion");
                        data.read(ccArray.getData(), PredType::NATIVE_FLOAT);

                        try
                        {
                            for (int k = 0; k < dims[1]; k++)
                            {
                                RecordedChannelInfo c;
                                c.name = "CH" + String(k);
                                c.bitVolts = ccArray[k] * 1e6;
                                info.channels.add(c);
                            }   
                            infoArray.add(info);
                            availableDataSets.add(numRecords);
                            dataPaths.set(numRecords, dataSourceName);
                            numRecords++;
                            
                        } catch (GroupIException)
                        {
                            std::cout << "!!!GroupIException!!!" << std::endl; 
                        } catch (AttributeIException)
                        {
                            std::cout << "!!!AttributeIException!!!" << std::endl;
                        }

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

void NWBFileSource::updateActiveRecord(int index)
{

    samplePos = 0;
    
    try
    {
        String path = "/acquisition/" + dataPaths[index] + "/data";
        dataSet = new DataSet(sourceFile->openDataSet(path.toUTF8()));
    }
    catch (FileIException error)
    {
        PROCESS_ERROR;
    }
    catch (DataSetIException error)
    {
        PROCESS_ERROR;
    }
}

void NWBFileSource::seekTo(int64 sample)
{
    samplePos = sample % getActiveNumSamples();
}

int NWBFileSource::readData(int16* buffer, int nSamples)
{

    DataSpace fSpace,mSpace;
    int samplesToRead;
    int nChannels = getActiveNumChannels();
    hsize_t dim[3],offset[3];

    if (samplePos + nSamples > getActiveNumSamples())
    {
        samplesToRead = (int) getActiveNumSamples() - (int) samplePos;
    }
    else
    {
        samplesToRead = nSamples;
    }

    try
    {
        fSpace = dataSet->getSpace();
        dim[0] = samplesToRead;
        dim[1] = nChannels;
        dim[2] = 1;
        offset[0] = samplePos;
        offset[1] = 0;
        offset[2] = 0;

        fSpace.selectHyperslab(H5S_SELECT_SET,dim,offset);
        mSpace = DataSpace(2,dim);

        dataSet->read(buffer,PredType::NATIVE_INT16,mSpace,fSpace);
        samplePos += samplesToRead;
        return samplesToRead;

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
    return 0;
}

void NWBFileSource::processChannelData(int16* inBuffer, float* outBuffer, int channel, int64 numSamples)
{
    int n = getActiveNumChannels();
    float bitVolts = getChannelInfo(activeRecord.get(), channel).bitVolts;

    for (int i=0; i < numSamples; i++)
    {
        *(outBuffer+i) = *(inBuffer+(n*i)+channel) * bitVolts;
    }

}

void NWBFileSource::processEventData(EventInfo &eventInfo, int64 start, int64 stop)
{
    //TODO
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

