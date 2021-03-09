/**
 * Copyright © 2020 IBM Corporation
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
#include "config.h"

#include "zone.hpp"

#include "fan.hpp"
#include "functor.hpp"
#include "handlers.hpp"

#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <utility>
#include <vector>

namespace phosphor::fan::control::json
{

using json = nlohmann::json;
using namespace phosphor::logging;
namespace fs = std::filesystem;

const std::map<std::string, std::map<std::string, propHandler>>
    Zone::_intfPropHandlers = {{thermModeIntf,
                                {{supportedProp, zone::property::supported},
                                 {currentProp, zone::property::current}}}};

Zone::Zone(sdbusplus::bus::bus& bus, const json& jsonObj) :
    ConfigBase(jsonObj),
    ThermalObject(bus, (fs::path{CONTROL_OBJPATH} /= getName()).c_str(), true),
    _incDelay(0), _floor(0), _target(0)
{
    if (jsonObj.contains("profiles"))
    {
        for (const auto& profile : jsonObj["profiles"])
        {
            _profiles.emplace_back(profile.get<std::string>());
        }
    }
    // Speed increase delay is optional, defaults to 0
    if (jsonObj.contains("increase_delay"))
    {
        _incDelay = jsonObj["increase_delay"].get<uint64_t>();
    }
    setFullSpeed(jsonObj);
    setDefaultFloor(jsonObj);
    setDecInterval(jsonObj);
    // Setting properties on interfaces to be served are optional
    if (jsonObj.contains("interfaces"))
    {
        setInterfaces(jsonObj);
    }
}

void Zone::addFan(std::unique_ptr<Fan> fan)
{
    _fans.emplace_back(std::move(fan));
}

void Zone::setFloor(uint64_t target)
{
    // Check all entries are set to allow floor to be set
    auto pred = [](auto const& entry) { return entry.second; };
    if (std::all_of(_floorChange.begin(), _floorChange.end(), pred))
    {
        _floor = target;
        // Floor above target, update target to floor
        if (_target < _floor)
        {
            requestIncrease(_floor - _target);
        }
    }
}

void Zone::requestIncrease(uint64_t targetDelta)
{
    // TODO Add from `requestSpeedIncrease` method in YAML zone object
}

void Zone::setPersisted(const std::string& intf, const std::string& prop)
{
    if (std::find_if(_propsPersisted[intf].begin(), _propsPersisted[intf].end(),
                     [&prop](const auto& p) { return prop == p; }) !=
        _propsPersisted[intf].end())
    {
        _propsPersisted[intf].emplace_back(prop);
    }
}

std::string Zone::current(std::string value)
{
    auto current = ThermalObject::current();
    std::transform(value.begin(), value.end(), value.begin(), toupper);

    auto supported = ThermalObject::supported();
    auto isSupported =
        std::any_of(supported.begin(), supported.end(), [&value](auto& s) {
            std::transform(s.begin(), s.end(), s.begin(), toupper);
            return value == s;
        });

    if (value != current && isSupported)
    {
        current = ThermalObject::current(value);
        if (isPersisted("xyz.openbmc_project.Control.ThermalMode", "Current"))
        {
            saveCurrentMode();
        }
        // TODO Trigger event(s) for current mode property change
        // auto eData =
        // _objects[_path]["xyz.openbmc_project.Control.ThermalMode"]
        //                      ["Current"];
        // if (eData != nullptr)
        // {
        //     sdbusplus::message::message nullMsg{nullptr};
        //     handleEvent(nullMsg, eData);
        // }
    }

    return current;
}

void Zone::setFullSpeed(const json& jsonObj)
{
    if (!jsonObj.contains("full_speed"))
    {
        log<level::ERR>("Missing required zone's full speed",
                        entry("JSON=%s", jsonObj.dump().c_str()));
        throw std::runtime_error("Missing required zone's full speed");
    }
    _fullSpeed = jsonObj["full_speed"].get<uint64_t>();
    // Start with the current target set as the default
    _target = _fullSpeed;
}

void Zone::setDefaultFloor(const json& jsonObj)
{
    if (!jsonObj.contains("default_floor"))
    {
        log<level::ERR>("Missing required zone's default floor speed",
                        entry("JSON=%s", jsonObj.dump().c_str()));
        throw std::runtime_error("Missing required zone's default floor speed");
    }
    _defaultFloor = jsonObj["default_floor"].get<uint64_t>();
    // Start with the current floor set as the default
    _floor = _defaultFloor;
}

void Zone::setDecInterval(const json& jsonObj)
{
    if (!jsonObj.contains("decrease_interval"))
    {
        log<level::ERR>("Missing required zone's decrease interval",
                        entry("JSON=%s", jsonObj.dump().c_str()));
        throw std::runtime_error("Missing required zone's decrease interval");
    }
    _decInterval = jsonObj["decrease_interval"].get<uint64_t>();
}

void Zone::setInterfaces(const json& jsonObj)
{
    for (const auto& interface : jsonObj["interfaces"])
    {
        if (!interface.contains("name") || !interface.contains("properties"))
        {
            log<level::ERR>("Missing required zone interface attributes",
                            entry("JSON=%s", interface.dump().c_str()));
            throw std::runtime_error(
                "Missing required zone interface attributes");
        }
        auto propFuncs =
            _intfPropHandlers.find(interface["name"].get<std::string>());
        if (propFuncs == _intfPropHandlers.end())
        {
            // Construct list of available configurable interfaces
            auto intfs = std::accumulate(
                std::next(_intfPropHandlers.begin()), _intfPropHandlers.end(),
                _intfPropHandlers.begin()->first, [](auto list, auto intf) {
                    return std::move(list) + ", " + intf.first;
                });
            log<level::ERR>("Configured interface not available",
                            entry("JSON=%s", interface.dump().c_str()),
                            entry("AVAILABLE_INTFS=%s", intfs.c_str()));
            throw std::runtime_error("Configured interface not available");
        }

        for (const auto& property : interface["properties"])
        {
            if (!property.contains("name"))
            {
                log<level::ERR>(
                    "Missing required interface property attributes",
                    entry("JSON=%s", property.dump().c_str()));
                throw std::runtime_error(
                    "Missing required interface property attributes");
            }
            // Attribute "persist" is optional, defaults to `false`
            auto persist = false;
            if (property.contains("persist"))
            {
                persist = property["persist"].get<bool>();
            }
            // Property name from JSON must exactly match supported
            // index names to functions in property namespace
            auto propFunc =
                propFuncs->second.find(property["name"].get<std::string>());
            if (propFunc == propFuncs->second.end())
            {
                // Construct list of available configurable properties
                auto props = std::accumulate(
                    std::next(propFuncs->second.begin()),
                    propFuncs->second.end(), propFuncs->second.begin()->first,
                    [](auto list, auto prop) {
                        return std::move(list) + ", " + prop.first;
                    });
                log<level::ERR>("Configured property not available",
                                entry("JSON=%s", property.dump().c_str()),
                                entry("AVAILABLE_PROPS=%s", props.c_str()));
                throw std::runtime_error(
                    "Configured property function not available");
            }
            auto zHandler = propFunc->second(property, persist);
            // Only add non-null zone handler functions
            if (zHandler)
            {
                _zoneHandlers.emplace_back(zHandler);
            }
        }
    }
}

bool Zone::isPersisted(const std::string& intf, const std::string& prop)
{
    auto it = _propsPersisted.find(intf);
    if (it == _propsPersisted.end())
    {
        return false;
    }

    return std::any_of(it->second.begin(), it->second.end(),
                       [&prop](const auto& p) { return prop == p; });
}

void Zone::saveCurrentMode()
{
    fs::path path{CONTROL_PERSIST_ROOT_PATH};
    // Append zone name and property description
    path /= getName();
    path /= "CurrentMode";
    std::ofstream ofs(path.c_str(), std::ios::binary);
    cereal::JSONOutputArchive oArch(ofs);
    oArch(ThermalObject::current());
}

/**
 * Properties of interfaces supported by the zone configuration that return
 * a ZoneHandler function that sets the zone's property value(s).
 */
namespace zone::property
{
// Get a zone handler function for the configured values of the "Supported"
// property
ZoneHandler supported(const json& jsonObj, bool persist)
{
    std::vector<std::string> values;
    if (!jsonObj.contains("values"))
    {
        log<level::ERR>(
            "No 'values' found for \"Supported\" property, using an empty list",
            entry("JSON=%s", jsonObj.dump().c_str()));
    }
    else
    {
        for (const auto& value : jsonObj["values"])
        {
            if (!value.contains("value"))
            {
                log<level::ERR>("No 'value' found for \"Supported\" property "
                                "entry, skipping",
                                entry("JSON=%s", value.dump().c_str()));
            }
            else
            {
                values.emplace_back(value["value"].get<std::string>());
            }
        }
    }

    // TODO Use this zone object's extended `supported` method in the handler
    return make_zoneHandler(handler::setZoneProperty<std::vector<std::string>>(
        Zone::thermModeIntf, Zone::supportedProp, &control::Zone::supported,
        std::move(values), persist));
}

// Get a zone handler function for a configured value of the "Current"
// property
ZoneHandler current(const json& jsonObj, bool persist)
{
    // Use default value for "Current" property if no "value" entry given
    if (!jsonObj.contains("value"))
    {
        log<level::ERR>("No 'value' found for \"Current\" property, "
                        "using default",
                        entry("JSON=%s", jsonObj.dump().c_str()));
        return {};
    }

    // TODO Use this zone object's `current` method in the handler
    return make_zoneHandler(handler::setZoneProperty<std::string>(
        Zone::thermModeIntf, Zone::currentProp, &control::Zone::current,
        jsonObj["value"].get<std::string>(), persist));
}
} // namespace zone::property

} // namespace phosphor::fan::control::json
