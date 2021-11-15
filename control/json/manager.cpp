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

#include "manager.hpp"

#include "../utils/flight_recorder.hpp"
#include "action.hpp"
#include "event.hpp"
#include "fan.hpp"
#include "group.hpp"
#include "json_config.hpp"
#include "power_state.hpp"
#include "profile.hpp"
#include "sdbusplus.hpp"
#include "zone.hpp"

#include <systemd/sd-bus.h>

#include <nlohmann/json.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/server/manager.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/utility/timer.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace phosphor::fan::control::json
{

using json = nlohmann::json;

std::vector<std::string> Manager::_activeProfiles;
std::map<std::string,
         std::map<std::string, std::pair<bool, std::vector<std::string>>>>
    Manager::_servTree;
std::map<std::string,
         std::map<std::string, std::map<std::string, PropertyVariantType>>>
    Manager::_objects;
std::unordered_map<std::string, PropertyVariantType> Manager::_parameters;

const std::string Manager::dumpFile = "/tmp/fan_control_dump.json";

Manager::Manager(const sdeventplus::Event& event) :
    _bus(util::SDBusPlus::getBus()), _event(event),
    _mgr(util::SDBusPlus::getBus(), CONTROL_OBJPATH), _loadAllowed(true),
    _powerState(std::make_unique<PGoodState>(
        util::SDBusPlus::getBus(),
        std::bind(std::mem_fn(&Manager::powerStateChanged), this,
                  std::placeholders::_1)))
{}

void Manager::sighupHandler(sdeventplus::source::Signal&,
                            const struct signalfd_siginfo*)
{
    // Save current set of available and active profiles
    std::map<configKey, std::unique_ptr<Profile>> profiles;
    profiles.swap(_profiles);
    std::vector<std::string> activeProfiles;
    activeProfiles.swap(_activeProfiles);

    try
    {
        _loadAllowed = true;
        load();
    }
    catch (const std::runtime_error& re)
    {
        // Restore saved available and active profiles
        _loadAllowed = false;
        _profiles.swap(profiles);
        _activeProfiles.swap(activeProfiles);
        log<level::ERR>("Error reloading configs, no changes made",
                        entry("LOAD_ERROR=%s", re.what()));
    }
}

void Manager::sigUsr1Handler(sdeventplus::source::Signal&,
                             const struct signalfd_siginfo*)
{
    debugDumpEventSource = std::make_unique<sdeventplus::source::Defer>(
        _event, std::bind(std::mem_fn(&Manager::dumpDebugData), this,
                          std::placeholders::_1));
}

void Manager::dumpDebugData(sdeventplus::source::EventBase& /*source*/)
{
    json data;
    FlightRecorder::instance().dump(data);
    dumpCache(data);

    std::for_each(_zones.begin(), _zones.end(), [&data](const auto& zone) {
        data["zones"][zone.second->getName()] = zone.second->dump();
    });

    std::ofstream file{Manager::dumpFile};
    if (!file)
    {
        log<level::ERR>("Could not open file for fan dump");
        return;
    }

    file << std::setw(4) << data;

    debugDumpEventSource.reset();
}

void Manager::dumpCache(json& data)
{
    auto& objects = data["objects"];
    for (const auto& [path, interfaces] : _objects)
    {
        auto& interfaceJSON = objects[path];

        for (const auto& [interface, properties] : interfaces)
        {
            auto& propertyJSON = interfaceJSON[interface];
            for (const auto& [propName, propValue] : properties)
            {
                std::visit(
                    [&obj = propertyJSON[propName]](auto&& val) { obj = val; },
                    propValue);
            }
        }
    }

    auto& parameters = data["parameters"];
    for (const auto& [name, value] : _parameters)
    {
        std::visit([&obj = parameters[name]](auto&& val) { obj = val; }, value);
    }

    data["services"] = _servTree;
}

void Manager::load()
{
    if (_loadAllowed)
    {
        // Load the available profiles and which are active
        setProfiles();

        // Load the zone configurations
        auto zones = getConfig<Zone>(false, _event, this);
        // Load the fan configurations and move each fan into its zone
        auto fans = getConfig<Fan>(false);
        for (auto& fan : fans)
        {
            configKey fanProfile =
                std::make_pair(fan.second->getZone(), fan.first.second);
            auto itZone = std::find_if(
                zones.begin(), zones.end(), [&fanProfile](const auto& zone) {
                    return Manager::inConfig(fanProfile, zone.first);
                });
            if (itZone != zones.end())
            {
                if (itZone->second->getTarget() != fan.second->getTarget() &&
                    fan.second->getTarget() != 0)
                {
                    // Update zone target to current target of the fan in the
                    // zone
                    itZone->second->setTarget(fan.second->getTarget());
                }
                itZone->second->addFan(std::move(fan.second));
            }
        }

        // Save all currently available groups, if any, then clear for reloading
        auto groups = std::move(Event::getAllGroups(false));
        Event::clearAllGroups();

        std::map<configKey, std::unique_ptr<Event>> events;
        try
        {
            // Load any events configured, including all the groups
            events = getConfig<Event>(true, this, zones);
        }
        catch (const std::runtime_error& re)
        {
            // Restore saved set of all available groups for current events
            Event::setAllGroups(std::move(groups));
            throw re;
        }

        // Enable zones
        _zones = std::move(zones);
        std::for_each(_zones.begin(), _zones.end(),
                      [](const auto& entry) { entry.second->enable(); });

        // Clear current timers and signal subscriptions before enabling events
        // To save reloading services and/or objects into cache, do not clear
        // cache
        _timers.clear();
        _signals.clear();

        // Enable events
        _events = std::move(events);
        std::for_each(_events.begin(), _events.end(),
                      [](const auto& entry) { entry.second->enable(); });

        _loadAllowed = false;
    }
}

void Manager::powerStateChanged(bool powerStateOn)
{
    if (powerStateOn)
    {
        if (_zones.empty())
        {
            throw std::runtime_error("No configured zones found at poweron");
        }
        std::for_each(_zones.begin(), _zones.end(), [](const auto& entry) {
            entry.second->setTarget(entry.second->getPoweronTarget());
        });

        // Tell events to run their power on triggers
        std::for_each(_events.begin(), _events.end(),
                      [](const auto& entry) { entry.second->powerOn(); });
    }
    else
    {
        // Tell events to run their power off triggers
        std::for_each(_events.begin(), _events.end(),
                      [](const auto& entry) { entry.second->powerOff(); });
    }
}

const std::vector<std::string>& Manager::getActiveProfiles()
{
    return _activeProfiles;
}

bool Manager::inConfig(const configKey& input, const configKey& comp)
{
    // Config names dont match, do not include in config
    if (input.first != comp.first)
    {
        return false;
    }
    // No profiles specified by input config, can be used in any config
    if (input.second.empty())
    {
        return true;
    }
    else
    {
        // Profiles must have one match in the other's profiles(and they must be
        // an active profile) to be used in the config
        return std::any_of(
            input.second.begin(), input.second.end(),
            [&comp](const auto& lProfile) {
                return std::any_of(
                    comp.second.begin(), comp.second.end(),
                    [&lProfile](const auto& rProfile) {
                        if (lProfile != rProfile)
                        {
                            return false;
                        }
                        auto activeProfs = getActiveProfiles();
                        return std::find(activeProfs.begin(), activeProfs.end(),
                                         lProfile) != activeProfs.end();
                    });
            });
    }
}

bool Manager::hasOwner(const std::string& path, const std::string& intf)
{
    auto itServ = _servTree.find(path);
    if (itServ == _servTree.end())
    {
        // Path not found in cache, therefore owner missing
        return false;
    }
    for (const auto& service : itServ->second)
    {
        auto itIntf = std::find_if(
            service.second.second.begin(), service.second.second.end(),
            [&intf](const auto& interface) { return intf == interface; });
        if (itIntf != std::end(service.second.second))
        {
            // Service found, return owner state
            return service.second.first;
        }
    }
    // Interface not found in cache, therefore owner missing
    return false;
}

void Manager::setOwner(const std::string& path, const std::string& serv,
                       const std::string& intf, bool isOwned)
{
    // Set owner state for specific object given
    auto& ownIntf = _servTree[path][serv];
    ownIntf.first = isOwned;
    auto itIntf = std::find_if(
        ownIntf.second.begin(), ownIntf.second.end(),
        [&intf](const auto& interface) { return intf == interface; });
    if (itIntf == std::end(ownIntf.second))
    {
        ownIntf.second.emplace_back(intf);
    }

    // Update owner state on all entries of the same `serv` & `intf`
    for (auto& itPath : _servTree)
    {
        if (itPath.first == path)
        {
            // Already set/updated owner on this path for `serv` & `intf`
            continue;
        }
        for (auto& itServ : itPath.second)
        {
            if (itServ.first != serv)
            {
                continue;
            }
            auto itIntf = std::find_if(
                itServ.second.second.begin(), itServ.second.second.end(),
                [&intf](const auto& interface) { return intf == interface; });
            if (itIntf != std::end(itServ.second.second))
            {
                itServ.second.first = isOwned;
            }
        }
    }
}

const std::string& Manager::findService(const std::string& path,
                                        const std::string& intf)
{
    static const std::string empty = "";

    auto itServ = _servTree.find(path);
    if (itServ != _servTree.end())
    {
        for (const auto& service : itServ->second)
        {
            auto itIntf = std::find_if(
                service.second.second.begin(), service.second.second.end(),
                [&intf](const auto& interface) { return intf == interface; });
            if (itIntf != std::end(service.second.second))
            {
                // Service found, return service name
                return service.first;
            }
        }
    }

    return empty;
}

void Manager::addServices(const std::string& intf, int32_t depth)
{
    // Get all subtree objects for the given interface
    auto objects = util::SDBusPlus::getSubTreeRaw(util::SDBusPlus::getBus(),
                                                  "/", intf, depth);
    // Add what's returned to the cache of path->services
    for (auto& itPath : objects)
    {
        auto pathIter = _servTree.find(itPath.first);
        if (pathIter != _servTree.end())
        {
            // Path found in cache
            for (auto& itServ : itPath.second)
            {
                auto servIter = pathIter->second.find(itServ.first);
                if (servIter != pathIter->second.end())
                {
                    // Service found in cache
                    for (auto& itIntf : itServ.second)
                    {
                        if (std::find(servIter->second.second.begin(),
                                      servIter->second.second.end(),
                                      itIntf) == servIter->second.second.end())
                        {
                            // Add interface to cache
                            servIter->second.second.emplace_back(itIntf);
                        }
                    }
                }
                else
                {
                    // Service not found in cache
                    auto intfs = {intf};
                    pathIter->second[itServ.first] =
                        std::make_pair(true, intfs);
                }
            }
        }
        else
        {
            // Path not found in cache
            auto intfs = {intf};
            for (const auto& [servName, servIntfs] : itPath.second)
            {
                _servTree[itPath.first][servName] = std::make_pair(true, intfs);
            }
        }
    }
}

const std::string& Manager::getService(const std::string& path,
                                       const std::string& intf)
{
    // Retrieve service from cache
    const auto& serviceName = findService(path, intf);
    if (serviceName.empty())
    {
        addServices(intf, 0);
        return findService(path, intf);
    }

    return serviceName;
}

std::vector<std::string> Manager::findPaths(const std::string& serv,
                                            const std::string& intf)
{
    std::vector<std::string> paths;

    for (const auto& path : _servTree)
    {
        auto itServ = path.second.find(serv);
        if (itServ != path.second.end())
        {
            if (std::find(itServ->second.second.begin(),
                          itServ->second.second.end(),
                          intf) != itServ->second.second.end())
            {
                if (std::find(paths.begin(), paths.end(), path.first) ==
                    paths.end())
                {
                    paths.push_back(path.first);
                }
            }
        }
    }

    return paths;
}

std::vector<std::string> Manager::getPaths(const std::string& serv,
                                           const std::string& intf)
{
    auto paths = findPaths(serv, intf);
    if (paths.empty())
    {
        addServices(intf, 0);
        return findPaths(serv, intf);
    }

    return paths;
}

void Manager::addObjects(const std::string& path, const std::string& intf,
                         const std::string& prop)
{
    auto service = getService(path, intf);
    if (service.empty())
    {
        // Log service not found for object
        log<level::DEBUG>(
            fmt::format("Unable to get service name for path {}, interface {}",
                        path, intf)
                .c_str());
        return;
    }

    auto objMgrPaths = getPaths(service, "org.freedesktop.DBus.ObjectManager");
    if (objMgrPaths.empty())
    {
        // No object manager interface provided by service?
        // Attempt to retrieve property directly
        auto variant = util::SDBusPlus::getPropertyVariant<PropertyVariantType>(
            _bus, service, path, intf, prop);
        _objects[path][intf][prop] = variant;
        return;
    }

    for (const auto& objMgrPath : objMgrPaths)
    {
        // Get all managed objects of service
        auto objects = util::SDBusPlus::getManagedObjects<PropertyVariantType>(
            _bus, service, objMgrPath);

        // Add what's returned to the cache of objects
        for (auto& object : objects)
        {
            auto itPath = _objects.find(object.first);
            if (itPath != _objects.end())
            {
                // Path found in cache
                for (auto& interface : itPath->second)
                {
                    auto itIntf = itPath->second.find(interface.first);
                    if (itIntf != itPath->second.end())
                    {
                        // Interface found in cache
                        for (auto& property : itIntf->second)
                        {
                            auto itProp = itIntf->second.find(property.first);
                            if (itProp != itIntf->second.end())
                            {
                                // Property found, update value
                                itProp->second = property.second;
                            }
                            else
                            {
                                itIntf->second.insert(property);
                            }
                        }
                    }
                    else
                    {
                        // Interface not found in cache
                        itPath->second.insert(interface);
                    }
                }
            }
            else
            {
                // Path not found in cache
                _objects.insert(object);
            }
        }
    }
}

const std::optional<PropertyVariantType>
    Manager::getProperty(const std::string& path, const std::string& intf,
                         const std::string& prop)
{
    // TODO Objects hosted by fan control (i.e. ThermalMode) are required to
    // update the cache upon being set/updated
    auto itPath = _objects.find(path);
    if (itPath != _objects.end())
    {
        auto itIntf = itPath->second.find(intf);
        if (itIntf != itPath->second.end())
        {
            auto itProp = itIntf->second.find(prop);
            if (itProp != itIntf->second.end())
            {
                return itProp->second;
            }
        }
    }

    return std::nullopt;
}

void Manager::addTimer(const TimerType type,
                       const std::chrono::microseconds interval,
                       std::unique_ptr<TimerPkg> pkg)
{
    auto dataPtr =
        std::make_unique<TimerData>(std::make_pair(type, std::move(*pkg)));
    Timer timer(_event,
                std::bind(&Manager::timerExpired, this, std::ref(*dataPtr)));
    if (type == TimerType::repeating)
    {
        timer.restart(interval);
    }
    else if (type == TimerType::oneshot)
    {
        timer.restartOnce(interval);
    }
    else
    {
        throw std::invalid_argument("Invalid Timer Type");
    }
    _timers.emplace_back(std::move(dataPtr), std::move(timer));
}

void Manager::addGroup(const Group& group)
{
    const auto& members = group.getMembers();
    for (const auto& member : members)
    {
        try
        {
            auto service = getService(member, group.getInterface());

            auto variant =
                util::SDBusPlus::getPropertyVariant<PropertyVariantType>(
                    service, member, group.getInterface(), group.getProperty());

            setProperty(member, group.getInterface(), group.getProperty(),
                        variant);
        }
        catch (const std::exception& e)
        {
            try
            {
                _objects.at(member)
                    .at(group.getInterface())
                    .erase(group.getProperty());
            }
            catch (const std::out_of_range&)
            {}
        }
    }
}

void Manager::timerExpired(TimerData& data)
{
    if (std::get<bool>(data.second))
    {
        const auto& groups = std::get<const std::vector<Group>&>(data.second);
        std::for_each(groups.begin(), groups.end(),
                      [this](const auto& group) { addGroup(group); });
    }

    auto& actions =
        std::get<std::vector<std::unique_ptr<ActionBase>>&>(data.second);
    // Perform the actions in the timer data
    std::for_each(actions.begin(), actions.end(),
                  [](auto& action) { action->run(); });

    // Remove oneshot timers after they expired
    if (data.first == TimerType::oneshot)
    {
        auto itTimer = std::find_if(
            _timers.begin(), _timers.end(), [&data](const auto& timer) {
                return (data.first == timer.first->first &&
                        (std::get<std::string>(data.second) ==
                         std::get<std::string>(timer.first->second)));
            });
        if (itTimer != std::end(_timers))
        {
            _timers.erase(itTimer);
        }
    }
}

void Manager::handleSignal(sdbusplus::message::message& msg,
                           const std::vector<SignalPkg>* pkgs)
{
    for (auto& pkg : *pkgs)
    {
        // Handle the signal callback and only run the actions if the handler
        // updated the cache for the given SignalObject
        if (std::get<SignalHandler>(pkg)(msg, std::get<SignalObject>(pkg),
                                         *this))
        {
            // Perform the actions in the handler package
            auto& actions = std::get<SignalActions>(pkg);
            std::for_each(actions.begin(), actions.end(), [](auto& action) {
                if (action.get())
                {
                    action.get()->run();
                }
            });
        }
        // Only rewind message when not last package
        if (&pkg != &pkgs->back())
        {
            sd_bus_message_rewind(msg.get(), true);
        }
    }
}

void Manager::setProfiles()
{
    // Profiles JSON config file is optional
    auto confFile = fan::JsonConfig::getConfFile(_bus, confAppName,
                                                 Profile::confFileName, true);

    _profiles.clear();
    if (!confFile.empty())
    {
        for (const auto& entry : fan::JsonConfig::load(confFile))
        {
            auto obj = std::make_unique<Profile>(entry);
            _profiles.emplace(
                std::make_pair(obj->getName(), obj->getProfiles()),
                std::move(obj));
        }
    }

    // Ensure all configurations use the same set of active profiles
    // (In case a profile's active state changes during configuration)
    _activeProfiles.clear();
    for (const auto& profile : _profiles)
    {
        if (profile.second->isActive())
        {
            _activeProfiles.emplace_back(profile.first.first);
        }
    }
}

} // namespace phosphor::fan::control::json
