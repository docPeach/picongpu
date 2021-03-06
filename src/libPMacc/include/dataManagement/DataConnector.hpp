/**
 * Copyright 2013 Rene Widera
 *
 * This file is part of libPMacc. 
 * 
 * libPMacc is free software: you can redistribute it and/or modify 
 * it under the terms of of either the GNU General Public License or 
 * the GNU Lesser General Public License as published by 
 * the Free Software Foundation, either version 3 of the License, or 
 * (at your option) any later version. 
 * libPMacc is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the 
 * GNU General Public License and the GNU Lesser General Public License 
 * for more details. 
 * 
 * You should have received a copy of the GNU General Public License 
 * and the GNU Lesser General Public License along with libPMacc. 
 * If not, see <http://www.gnu.org/licenses/>. 
 */ 
 
#ifndef DATACONNECTOR_HPP
#define	DATACONNECTOR_HPP

#include <map>
#include <sstream>
#include <stdexcept>

#include "dataManagement/Dataset.hpp"
#include "dataManagement/ISimulationIO.hpp"
#include "dataManagement/ISimulationData.hpp"
#include "dataManagement/AbstractInitialiser.hpp"
#include "dataManagement/ListSorter.hpp"


namespace PMacc
{

    /**
     * Helper class for DataConnector.
     * Uses std::map<KeyType, ValType> for storing values and has a
     * IDataSorter for iterating over these values according to their keys.
     * 
     * \tparam KeyType type of map keys
     * \tparam ValType type of map values
     */
    template<typename KeyType, typename ValType>
    class Mapping
    {
    public:

        /**
         * Destructor.
         * 
         * Deletes the IDataSorter and clears the mapping.
         */
        ~Mapping()
        {
            mapping.clear();
        }

        std::map<KeyType, ValType> mapping;
        IDataSorter<KeyType> *sorter;
    };

    /**
     * Singleton class which registers simulation Dataset and
     * data handler (ISimulationIO).
     *
     * Data handlers are notified about pending output data.
     */
    class DataConnector
    {
    public:

        static DataConnector& getInstance()
        {
            static DataConnector instance;
            return instance;
        }

        /**
         * Notifies observers that data should be dumped.
         *
         * @param currentStep current simulation iteration step
         */
        void dumpData(uint32_t currentStep)
        {
            for (ISimulationIO* ioclass = observers.sorter->begin();
                    observers.sorter->isValid(); ioclass = observers.sorter->getNext())
            {
                uint32_t frequency = observers.mapping[ioclass];
                if (currentStep % frequency == 0)
                    ioclass->notify(currentStep);
            }
        }

        /**
         * Registers an ISimulationIO observer at this observable.
         *
         * @param observer simulation IO handler to register
         * @param notifyFrequency frequency observers should be notified
         */
        void registerObserver(ISimulationIO *observer, uint32_t notifyFrequency)
        {
            if (notifyFrequency > 0)
            {
                observers.mapping[observer] = notifyFrequency;
                observers.sorter->add(observer);
            }
        }

        /**
         * Returns if data with identifier id is registered.
         *
         * @param id id of the Dataset to query
         * @return if dataset with id is registered
         */
        bool hasData(uint32_t id)
        {
            return datasets.mapping.find(id) != datasets.mapping.end();
        }

        /**
         * Invalidates all Datasets in the DataConnector.
         */
        void invalidate()
        {
            std::map<uint32_t, Dataset*>::iterator iter = datasets.mapping.begin();
            for (; iter != datasets.mapping.end(); ++iter)
                iter->second->invalidate();
        }

        /**
         * Initialises all Datasets using initialiser.
         * After initialising, the Datasets will be invalid.
         * 
         * @param initialiser class used for initialising Datasets
         * @param currentStep current simulation step
         */
        void initialise(AbstractInitialiser& initialiser, uint32_t currentStep)
        {
            currentStep = initialiser.setup();

            for (uint32_t id = datasets.sorter->begin();
                    datasets.sorter->isValid(); id = datasets.sorter->getNext())
            {
                ISimulationData& data = datasets.mapping[id]->getData();
                
                initialiser.init(id, data, currentStep);
            }
            
            initialiser.teardown();
        }

        /**
         * Registers a new Dataset with data and identifier id.
         *
         * If a Dataset with identifier id already exists, a runtime_error is thrown.
         * (Check with DataConnector::hasData when necessary.)
         *
         * @param data simulation data to store in the Dataset
         * @param id new identifier for the Dataset
         */
        void registerData(ISimulationData &data, uint32_t id)
        {
            if (hasData(id))
                throw std::runtime_error(getExceptionStringForID("DataConnector dataset ID already exists", id));

            Dataset::DatasetStatus status = Dataset::AUTO_INVALID;

            Dataset * dataset = new Dataset(data, status);
            datasets.mapping[id] = dataset;

            datasets.sorter->add(id);
        }

        /**
         * Returns registered data.
         *
         * Reference to data in Dataset with identifier id and type TYPE is returned.
         * If the Dataset status in invalid, it is automatically synchronized.
         * Increments the reference counter to the dataset specified by id.
         * This reference has to be released after all read/write operations 
         * before the next synchronize()/getData() on this data are done using releaseData().
         *
         * @tparam TYPE if of the data to load
         * @param id id of the Dataset to load from
         * @param noSync indicates that no synchronization should be performed, regardless of dataset status
         * @return returns a reference to the data of type TYPE
         */
        template<class TYPE>
        TYPE &getData(uint32_t id, bool noSync = false)
        {
            std::map<uint32_t, Dataset*>::const_iterator iter = datasets.mapping.find(id);

            if (iter == datasets.mapping.end())
                throw std::runtime_error(getExceptionStringForID("Invalid DataConnector dataset ID", id));
            
            Dataset * dataset = iter->second;
            if (!noSync)
            {
                dataset->synchronize();
            }
            
            return (TYPE&) (dataset->getData());
        }

        /**
         * Decrements the reference counter to the data specified by id.
         * 
         * @param id id for the dataset previously acquired using getData()
         */
        void releaseData(uint32_t)
        {
        }

    private:
        Mapping<uint32_t, Dataset*> datasets;
        Mapping<ISimulationIO*, uint32_t> observers;

        DataConnector()
        {
            datasets.sorter = new ListSorter<uint32_t > ();
            observers.sorter = new ListSorter<ISimulationIO*>();
        };

        virtual ~DataConnector()
        {
            std::map<uint32_t, Dataset*>::const_iterator iter;
            for (iter = datasets.mapping.begin(); iter != datasets.mapping.end(); iter++)
                delete iter->second;

            if (datasets.sorter != NULL)
            {
                delete datasets.sorter;
                datasets.sorter = NULL;
            }

            if (observers.sorter != NULL)
            {
                delete observers.sorter;
                observers.sorter = NULL;
            }
        }
        
        std::string getExceptionStringForID(const char *msg, uint32_t id)
        {
            std::stringstream stream;
            stream << msg << " (" << id << ")";
            return stream.str();
        }
    };

}

#endif	/* DATACONNECTOR_HPP */

