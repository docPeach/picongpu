/**
 * Copyright 2013 Axel Huebl, Heiko Burau, Rene Widera
 *
 * This file is part of PIConGPU. 
 * 
 * PIConGPU is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, or 
 * (at your option) any later version. 
 * 
 * PIConGPU is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License 
 * along with PIConGPU.  
 * If not, see <http://www.gnu.org/licenses/>. 
 */ 
 


#ifndef GATHERSLICE_HPP
#define	GATHERSLICE_HPP

#include "types.h"
#include "simulation_defines.hpp"

#include <mpi.h>
#include "mappings/simulation/GridController.hpp"

//c includes
#include "sys/stat.h"
#include "header/MessageHeader.hpp"
#include "memory/boxes/PitchedBox.hpp"

namespace picongpu
{
using namespace PMacc;


struct GatherSlice
{

    GatherSlice() : mpiRank(-1), numRanks(0), filteredData(NULL), fullData(NULL), isMPICommInitialized(false)
    {
    }

    ~GatherSlice()
    {
        reset();
    }

    /*
     * @return true if object has reduced data after reduce call else false
     */
    bool init(bool isActive)
    {
        /*free old communicator of init is called again*/
        if (isMPICommInitialized)
        {
            reset();
        }

        int countRanks = GridController<DIM3>::getInstance().getGpuNodes().getElementCount();
        int gatherRanks[countRanks];
        int groupRanks[countRanks];
        mpiRank = GridController<DIM3>::getInstance().getGlobalRank();
        if (!isActive)
            mpiRank = -1;

        MPI_CHECK(MPI_Allgather(&mpiRank, 1, MPI_INT, gatherRanks, 1, MPI_INT, MPI_COMM_WORLD));

        for (int i = 0; i < countRanks; ++i)
        {
            if (gatherRanks[i] != -1)
            {
                groupRanks[numRanks] = gatherRanks[i];
                numRanks++;
            }
        }

        MPI_Group group;
        MPI_Group newgroup;
        MPI_CHECK(MPI_Comm_group(MPI_COMM_WORLD, &group));
        MPI_CHECK(MPI_Group_incl(group, numRanks, groupRanks, &newgroup));

        MPI_CHECK(MPI_Comm_create(MPI_COMM_WORLD, newgroup, &comm));

        if (mpiRank != -1)
        {
            MPI_Comm_rank(comm, &mpiRank);
            isMPICommInitialized = true;
        }

        return mpiRank == 0;
    }

    template<class Box >
    Box operator()(Box & data, const MessageHeader & header)
    {
        typedef typename Box::ValueType ValueType;

        Box dstBox = Box(PitchedBox<ValueType, DIM2 > (
                                                       (ValueType*) filteredData,
                                                       DataSpace<DIM2 > (),
                                                       header.sim.size,
                                                       header.sim.size.x() * sizeof (ValueType)
                                                       ));

        MessageHeader* fakeHeader = MessageHeader::create();
        memcpy(fakeHeader, &header, MessageHeader::bytes);

        char* recvHeader = new char[ MessageHeader::bytes * numRanks];

        if (fullData == NULL && mpiRank == 0)
            fullData = (char*) new ValueType[header.sim.size.getElementCount()];


        MPI_CHECK(MPI_Gather(fakeHeader, MessageHeader::bytes, MPI_CHAR, recvHeader, MessageHeader::bytes,
                             MPI_CHAR, 0, comm));

        int counts[numRanks];
        int displs[numRanks];
        int offset = 0;
        for (int i = 0; i < numRanks; ++i)
        {
            MessageHeader* head = (MessageHeader*) (recvHeader + MessageHeader::bytes * i);
            counts[i] = head->node.maxSize.getElementCount() * sizeof (ValueType);
            displs[i] = offset;
            offset += counts[i];
        }

        const size_t elementsCount = header.node.maxSize.getElementCount() * sizeof (ValueType);

        MPI_CHECK(MPI_Gatherv(
                              (char*) (data.getPointer()), elementsCount, MPI_CHAR,
                              fullData, counts, displs, MPI_CHAR,
                              0, comm));



        if (mpiRank == 0)
        {
            log<picLog::DOMAINS > ("Master create image");
            if (filteredData == NULL)
                filteredData = (char*) new ValueType[header.sim.size.getElementCount()];

            /*create box with valid memory*/
            dstBox = Box(PitchedBox<ValueType, DIM2 > (
                                                       (ValueType*) filteredData,
                                                       DataSpace<DIM2 > (),
                                                       header.sim.size,
                                                       header.sim.size.x() * sizeof (ValueType)
                                                       ));

            for (int i = 0; i < numRanks; ++i)
            {
                MessageHeader* head = (MessageHeader*) (recvHeader + MessageHeader::bytes * i);

                log<picLog::DOMAINS > ("part image with offset %1%byte=%2%elements | size(%3% %4%) | offset(%5% %6%)") %
                    displs[i] % (displs[i] / sizeof (ValueType)) %
                    head->node.maxSize.x() % head->node.maxSize.y() %
                    head->node.offset.x() % head->node.offset.y();
                Box srcBox = Box(PitchedBox<ValueType, DIM2 > (
                                                               (ValueType*) (fullData + displs[i]),
                                                               DataSpace<DIM2 > (),
                                                               head->node.maxSize,
                                                               head->node.maxSize.x() * sizeof (ValueType)
                                                               ));

                insertData(dstBox, srcBox, head->node.offset, head->node.maxSize);
            }

        }

        delete[] recvHeader;
        MessageHeader::destroy(fakeHeader);

        return dstBox;
    }

    template<class DstBox, class SrcBox>
    void insertData(DstBox& dst, const SrcBox& src, Size2D offsetToSimNull, Size2D srcSize)
    {
        for (int y = 0; y < srcSize.y(); ++y)
        {
            for (int x = 0; x < srcSize.x(); ++x)
            {
                dst[y + offsetToSimNull.y()][x + offsetToSimNull.x()] = src[y][x];
            }
        }
    }

private:

    /*reset this object und set all values to initial state*/
    void reset()
    {
        mpiRank = -1;
        numRanks = 0;
        if (filteredData != NULL)
            delete[] filteredData;
        filteredData = NULL;
        if (fullData != NULL)
            delete[] fullData;
        fullData = NULL;
        if (isMPICommInitialized)
            MPI_CHECK(MPI_Comm_free(&comm));
        isMPICommInitialized = false;
    }

    char* filteredData;
    char* fullData;
    MPI_Comm comm;
    int mpiRank;
    int numRanks;
    bool isMPICommInitialized;
};

}//namespace

#endif	/* GATHERSLICE_HPP */

