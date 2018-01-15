/**
 * Copyright © 2017 IBM Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <algorithm>
#include <phosphor-logging/log.hpp>
#include "fan.hpp"
#include "types.hpp"
#include "utility.hpp"
#include "sdbusplus.hpp"

namespace phosphor
{
namespace fan
{
namespace monitor
{

using namespace phosphor::logging;

Fan::Fan(Mode mode,
         sdbusplus::bus::bus& bus,
         phosphor::fan::event::EventPtr&  events,
         std::unique_ptr<trust::Manager>& trust,
         const FanDefinition& def) :
    _bus(bus),
    _name(std::get<fanNameField>(def)),
    _deviation(std::get<fanDeviationField>(def)),
    _numSensorFailsForNonFunc(std::get<numSensorFailsForNonfuncField>(def)),
    _trustManager(trust)
{
    //Start from a known state of functional
    updateInventory(true);

    // Setup tach sensors for monitoring when in monitor mode
    if (mode != Mode::init)
    {
        auto& sensors = std::get<sensorListField>(def);
        for (auto& s : sensors)
        {
            try
            {
                _sensors.emplace_back(
                        std::make_unique<TachSensor>(
                                bus,
                                *this,
                                std::get<sensorNameField>(s),
                                std::get<hasTargetField>(s),
                                std::get<timeoutField>(def),
                                events));

                _trustManager->registerSensor(_sensors.back());
            }
            catch (InvalidSensorError& e)
            {

            }
        }

        //The TachSensors will now have already read the input
        //and target values, so check them.
        tachChanged();
    }
}


void Fan::tachChanged()
{
    for (auto& s : _sensors)
    {
        tachChanged(*s);
    }
}


void Fan::tachChanged(TachSensor& sensor)
{
    if (_trustManager->active())
    {
        if (!_trustManager->checkTrust(sensor))
        {
            return;
        }
    }

    auto running = sensor.timerRunning();

    //If this sensor is out of range at this moment, start
    //its timer, at the end of which the inventory
    //for the fan may get updated to not functional.

    //If this sensor is OK, put everything back into a good state.

    if (outOfRange(sensor))
    {
        if (sensor.functional() && !running)
        {
            sensor.startTimer();
        }
    }
    else
    {
        if (!sensor.functional())
        {
            sensor.setFunctional(true);
        }

        if (running)
        {
            sensor.stopTimer();
        }

        //If the fan was nonfunctional and enough sensors are now OK,
        //the fan can go back to functional
        if (!_functional && !tooManySensorsNonfunctional())
        {
            log<level::INFO>("Setting a fan back to functional",
                             entry("FAN=%s", _name.c_str()));

            updateInventory(true);
        }
    }
}


uint64_t Fan::findTargetSpeed()
{
    uint64_t target = 0;
    //The sensor doesn't support a target,
    //so get it from another sensor.
    auto s = std::find_if(_sensors.begin(), _sensors.end(),
                          [](const auto& s)
                          {
                              return s->hasTarget();
                          });

    if (s != _sensors.end())
    {
        target = (*s)->getTarget();
    }

    return target;
}


bool Fan::tooManySensorsNonfunctional()
{
    size_t numFailed =  std::count_if(_sensors.begin(), _sensors.end(),
                                      [](const auto& s)
                                      {
                                          return !s->functional();
                                      });

    return (numFailed >= _numSensorFailsForNonFunc);
}


bool Fan::outOfRange(const TachSensor& sensor)
{
    auto actual = static_cast<uint64_t>(sensor.getInput());
    auto target = sensor.getTarget();

    uint64_t min = target * (100 - _deviation) / 100;
    uint64_t max = target * (100 + _deviation) / 100;

    if ((actual < min) || (actual > max))
    {
        return true;
    }

    return false;
}


void Fan::timerExpired(TachSensor& sensor)
{
    sensor.setFunctional(false);

    //If the fan is currently functional, but too many
    //contained sensors are now nonfunctional, update
    //the whole fan nonfunctional.

    if (_functional && tooManySensorsNonfunctional())
    {
        log<level::ERR>("Setting a fan to nonfunctional",
                entry("FAN=%s", _name.c_str()),
                entry("TACH_SENSOR=%s", sensor.name().c_str()),
                entry("ACTUAL_SPEED=%lld", sensor.getInput()),
                entry("TARGET_SPEED=%lld", sensor.getTarget()));

        updateInventory(false);
    }
}


void Fan::updateInventory(bool functional)
{
    auto objectMap = util::getObjMap<bool>(
            _name,
            util::OPERATIONAL_STATUS_INTF,
            util::FUNCTIONAL_PROPERTY,
            functional);
    auto response = util::SDBusPlus::lookupAndCallMethod(
            _bus,
            util::INVENTORY_PATH,
            util::INVENTORY_INTF,
            "Notify",
            objectMap);
    if (response.is_method_error())
    {
        log<level::ERR>("Error in Notify call to update inventory");
        return;
    }

    //This will always track the current state of the inventory.
    _functional = functional;
}

}
}
}
