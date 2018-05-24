/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "channel_mgmt.hpp"

#include "apphandler.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <boost/interprocess/sync/scoped_lock.hpp>
#include <cerrno>
#include <experimental/filesystem>
#include <fstream>
#include <phosphor-logging/log.hpp>
#include <unordered_map>

namespace ipmi
{

using namespace phosphor::logging;

static constexpr const char* channelAccessDefaultFilename =
    "/usr/share/ipmi-providers/channel_access.json";
static constexpr const char* channelConfigDefaultFilename =
    "/usr/share/ipmi-providers/channel_config.json";
static constexpr const char* channelNvDataFilename =
    "/var/lib/ipmi/channel_access_nv.json";
static constexpr const char* channelVolatileDataFilename =
    "/run/ipmi/channel_access_volatile.json";

// STRING DEFINES: Should sync with key's in JSON
static constexpr const char* nameString = "name";
static constexpr const char* isValidString = "is_valid";
static constexpr const char* activeSessionsString = "active_sessions";
static constexpr const char* channelInfoString = "channel_info";
static constexpr const char* mediumTypeString = "medium_type";
static constexpr const char* protocolTypeString = "protocol_type";
static constexpr const char* sessionSupportedString = "session_supported";
static constexpr const char* isIpmiString = "is_ipmi";
static constexpr const char* authTypeSupportedString = "auth_type_supported";
static constexpr const char* accessModeString = "access_mode";
static constexpr const char* userAuthDisabledString = "user_auth_disabled";
static constexpr const char* perMsgAuthDisabledString = "per_msg_auth_disabled";
static constexpr const char* alertingDisabledString = "alerting_disabled";
static constexpr const char* privLimitString = "priv_limit";
static constexpr const char* authTypeEnabledString = "auth_type_enabled";

// Default values
static constexpr const char* defaultChannelName = "RESERVED";
static constexpr const uint8_t defaultMediumType =
    static_cast<uint8_t>(EChannelMediumType::reserved);
static constexpr const uint8_t defaultProtocolType =
    static_cast<uint8_t>(EChannelProtocolType::reserved);
static constexpr const uint8_t defaultSessionSupported =
    static_cast<uint8_t>(EChannelSessSupported::none);
static constexpr const uint8_t defaultAuthType =
    static_cast<uint8_t>(EAuthType::none);
static constexpr const bool defaultIsIpmiState = false;

// String mappings use in JSON config file
static std::unordered_map<std::string, EChannelMediumType> mediumTypeMap = {
    {"reserved", EChannelMediumType::reserved},
    {"ipmb", EChannelMediumType::ipmb},
    {"icmb-v1.0", EChannelMediumType::icmbV10},
    {"icmb-v0.9", EChannelMediumType::icmbV09},
    {"lan-802.3", EChannelMediumType::lan8032},
    {"serial", EChannelMediumType::serial},
    {"other-lan", EChannelMediumType::otherLan},
    {"pci-smbus", EChannelMediumType::pciSmbus},
    {"smbus-v1.0", EChannelMediumType::smbusV11},
    {"smbus-v2.0", EChannelMediumType::smbusV20},
    {"usb-1x", EChannelMediumType::usbV1x},
    {"usb-2x", EChannelMediumType::usbV2x},
    {"system-interface", EChannelMediumType::systemInterface},
    {"oem", EChannelMediumType::oem},
    {"unknown", EChannelMediumType::unknown}};

static std::unordered_map<std::string, EChannelProtocolType> protocolTypeMap = {
    {"na", EChannelProtocolType::na},
    {"ipmb-1.0", EChannelProtocolType::ipmbV10},
    {"icmb-2.0", EChannelProtocolType::icmbV11},
    {"reserved", EChannelProtocolType::reserved},
    {"ipmi-smbus", EChannelProtocolType::ipmiSmbus},
    {"kcs", EChannelProtocolType::kcs},
    {"smic", EChannelProtocolType::smic},
    {"bt-10", EChannelProtocolType::bt10},
    {"bt-15", EChannelProtocolType::bt15},
    {"tmode", EChannelProtocolType::tMode},
    {"oem", EChannelProtocolType::oem}};

static std::array<std::string, 4> accessModeList = {
    "disabled", "pre-boot", "always_available", "shared"};

static std::array<std::string, 4> sessionSupportList = {
    "session-less", "single-session", "multi-session", "session-based"};

static std::array<std::string, PRIVILEGE_OEM + 1> privList = {
    "priv-reserved", "priv-callback", "priv-user",
    "priv-operator", "priv-admin",    "priv-oem"};

ChannelConfig& getChannelConfigObject()
{
    static ChannelConfig channelConfig;
    return channelConfig;
}

ChannelConfig::ChannelConfig() : bus(ipmid_get_sd_bus_connection())
{
    std::ofstream mutexCleanUpFile;
    mutexCleanUpFile.open(ipmiChMutexCleanupLockFile,
                          std::ofstream::out | std::ofstream::app);
    if (!mutexCleanUpFile.good())
    {
        log<level::DEBUG>("Unable to open mutex cleanup file");
        return;
    }
    mutexCleanUpFile.close();
    mutexCleanupLock =
        boost::interprocess::file_lock(ipmiChMutexCleanupLockFile);
    if (mutexCleanupLock.try_lock())
    {
        boost::interprocess::named_recursive_mutex::remove(ipmiChannelMutex);
        channelMutex =
            std::make_unique<boost::interprocess::named_recursive_mutex>(
                boost::interprocess::open_or_create, ipmiChannelMutex);
        mutexCleanupLock.lock_sharable();
    }
    else
    {
        mutexCleanupLock.lock_sharable();
        channelMutex =
            std::make_unique<boost::interprocess::named_recursive_mutex>(
                boost::interprocess::open_or_create, ipmiChannelMutex);
    }

    initChannelPersistData();
}

bool ChannelConfig::isValidChannel(const uint8_t& chNum)
{
    if (chNum > maxIpmiChannels)
    {
        log<level::DEBUG>("Invalid channel ID - Out of range");
        return false;
    }

    if (channelData[chNum].isChValid == false)
    {
        log<level::DEBUG>("Channel is not valid");
        return false;
    }

    return true;
}

EChannelSessSupported
    ChannelConfig::getChannelSessionSupport(const uint8_t& chNum)
{
    EChannelSessSupported chSessSupport =
        (EChannelSessSupported)channelData[chNum].chInfo.sessionSupported;
    return chSessSupport;
}

bool ChannelConfig::isValidAuthType(const uint8_t& chNum,
                                    const EAuthType& authType)
{
    if ((authType < EAuthType::md2) || (authType > EAuthType::oem))
    {
        log<level::DEBUG>("Invalid authentication type");
        return false;
    }

    uint8_t authTypeSupported = channelData[chNum].chInfo.authTypeSupported;
    if (!(authTypeSupported & (1 << static_cast<uint8_t>(authType))))
    {
        log<level::DEBUG>("Authentication type is not supported.");
        return false;
    }

    return true;
}

int ChannelConfig::getChannelActiveSessions(const uint8_t& chNum)
{
    // TODO: TEMPORARY FIX
    // Channels active session count is managed separatly
    // by monitoring channel session which includes LAN and
    // RAKP layer changes. This will be updated, once the
    // authentication part is implemented.
    return channelData[chNum].activeSessCount;
}

ipmi_ret_t ChannelConfig::getChannelInfo(const uint8_t& chNum,
                                         ChannelInfo& chInfo)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    std::copy_n(reinterpret_cast<uint8_t*>(&channelData[chNum].chInfo),
                sizeof(channelData[chNum].chInfo),
                reinterpret_cast<uint8_t*>(&chInfo));

    return IPMI_CC_OK;
}

ipmi_ret_t ChannelConfig::getChannelAccessData(const uint8_t& chNum,
                                               ChannelAccess& chAccessData)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    if (getChannelSessionSupport(chNum) == EChannelSessSupported::none)
    {
        log<level::DEBUG>("Session-less channel doesn't have access data.");
        return IPMI_CC_ACTION_NOT_SUPPORTED_FOR_CHANNEL;
    }

    if (checkAndReloadVolatileData() != 0)
    {
        return IPMI_CC_UNSPECIFIED_ERROR;
    }

    std::copy_n(
        reinterpret_cast<uint8_t*>(&channelData[chNum].chAccess.chVolatileData),
        sizeof(channelData[chNum].chAccess.chVolatileData),
        reinterpret_cast<uint8_t*>(&chAccessData));

    return IPMI_CC_OK;
}

ipmi_ret_t
    ChannelConfig::setChannelAccessData(const uint8_t& chNum,
                                        const ChannelAccess& chAccessData,
                                        const uint8_t& setFlag)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    if (getChannelSessionSupport(chNum) == EChannelSessSupported::none)
    {
        log<level::DEBUG>("Session-less channel doesn't have access data.");
        return IPMI_CC_ACTION_NOT_SUPPORTED_FOR_CHANNEL;
    }

    if ((setFlag & setAccessMode) &&
        (!isValidAccessMode(chAccessData.accessMode)))
    {
        log<level::DEBUG>("Invalid access mode specified");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};

    if (checkAndReloadVolatileData() != 0)
    {
        return IPMI_CC_UNSPECIFIED_ERROR;
    }

    if (setFlag & setAccessMode)
    {
        channelData[chNum].chAccess.chVolatileData.accessMode =
            chAccessData.accessMode;
    }
    if (setFlag & setUserAuthEnabled)
    {
        channelData[chNum].chAccess.chVolatileData.userAuthDisabled =
            chAccessData.userAuthDisabled;
    }
    if (setFlag & setMsgAuthEnabled)
    {
        channelData[chNum].chAccess.chVolatileData.perMsgAuthDisabled =
            chAccessData.perMsgAuthDisabled;
    }
    if (setFlag & setAlertingEnabled)
    {
        channelData[chNum].chAccess.chVolatileData.alertingDisabled =
            chAccessData.alertingDisabled;
    }
    if (setFlag & setPrivLimit)
    {
        channelData[chNum].chAccess.chVolatileData.privLimit =
            chAccessData.privLimit;
    }

    // Write Volatile data to file
    if (writeChannelVolatileData() != 0)
    {
        log<level::DEBUG>("Failed to update the channel volatile data");
        return IPMI_CC_UNSPECIFIED_ERROR;
    }
    return IPMI_CC_OK;
}

ipmi_ret_t
    ChannelConfig::getChannelAccessPersistData(const uint8_t& chNum,
                                               ChannelAccess& chAccessData)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    if (getChannelSessionSupport(chNum) == EChannelSessSupported::none)
    {
        log<level::DEBUG>("Session-less channel doesn't have access data.");
        return IPMI_CC_ACTION_NOT_SUPPORTED_FOR_CHANNEL;
    }

    if (checkAndReloadNVData() != 0)
    {
        return IPMI_CC_UNSPECIFIED_ERROR;
    }

    std::copy_n(reinterpret_cast<uint8_t*>(
                    &channelData[chNum].chAccess.chNonVolatileData),
                sizeof(channelData[chNum].chAccess.chNonVolatileData),
                reinterpret_cast<uint8_t*>(&chAccessData));

    return IPMI_CC_OK;
}

ipmi_ret_t ChannelConfig::setChannelAccessPersistData(
    const uint8_t& chNum, const ChannelAccess& chAccessData,
    const uint8_t& setFlag)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    if (getChannelSessionSupport(chNum) == EChannelSessSupported::none)
    {
        log<level::DEBUG>("Session-less channel doesn't have access data.");
        return IPMI_CC_ACTION_NOT_SUPPORTED_FOR_CHANNEL;
    }

    if ((setFlag & setAccessMode) &&
        (!isValidAccessMode(chAccessData.accessMode)))
    {
        log<level::DEBUG>("Invalid access mode specified");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};

    if (checkAndReloadNVData() != 0)
    {
        return IPMI_CC_UNSPECIFIED_ERROR;
    }

    if (setFlag & setAccessMode)
    {
        channelData[chNum].chAccess.chNonVolatileData.accessMode =
            chAccessData.accessMode;
    }
    if (setFlag & setUserAuthEnabled)
    {
        channelData[chNum].chAccess.chNonVolatileData.userAuthDisabled =
            chAccessData.userAuthDisabled;
    }
    if (setFlag & setMsgAuthEnabled)
    {
        channelData[chNum].chAccess.chNonVolatileData.perMsgAuthDisabled =
            chAccessData.perMsgAuthDisabled;
    }
    if (setFlag & setAlertingEnabled)
    {
        channelData[chNum].chAccess.chNonVolatileData.alertingDisabled =
            chAccessData.alertingDisabled;
    }
    if (setFlag & setPrivLimit)
    {
        channelData[chNum].chAccess.chNonVolatileData.privLimit =
            chAccessData.privLimit;
    }

    // Write persistent data to file
    if (writeChannelPersistData() != 0)
    {
        log<level::DEBUG>("Failed to update the presist data file");
        return IPMI_CC_UNSPECIFIED_ERROR;
    }
    return IPMI_CC_OK;
}

ipmi_ret_t
    ChannelConfig::getChannelAuthTypeSupported(const uint8_t& chNum,
                                               uint8_t& authTypeSupported)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    authTypeSupported = channelData[chNum].chInfo.authTypeSupported;
    return IPMI_CC_OK;
}

ipmi_ret_t ChannelConfig::getChannelEnabledAuthType(const uint8_t& chNum,
                                                    const uint8_t& priv,
                                                    EAuthType& authType)
{
    if (!isValidChannel(chNum))
    {
        log<level::DEBUG>("Invalid channel");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    if (getChannelSessionSupport(chNum) == EChannelSessSupported::none)
    {
        log<level::DEBUG>("Sessionless channel doesn't have access data.");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    if (!isValidPrivLimit(priv))
    {
        log<level::DEBUG>("Invalid privilege specified.");
        return IPMI_CC_INVALID_FIELD_REQUEST;
    }

    // TODO: Hardcoded for now. Need to implement.
    authType = EAuthType::none;

    return IPMI_CC_OK;
}

std::time_t ChannelConfig::getUpdatedFileTime(const std::string& fileName)
{
    struct stat fileStat;
    if (stat(fileName.c_str(), &fileStat) != 0)
    {
        log<level::DEBUG>("Error in getting last updated time stamp");
        return -EIO;
    }
    return fileStat.st_mtime;
}

EChannelAccessMode
    ChannelConfig::convertToAccessModeIndex(const std::string& mode)
{
    auto iter = std::find(accessModeList.begin(), accessModeList.end(), mode);
    if (iter == accessModeList.end())
    {
        log<level::ERR>("Invalid access mode.",
                        entry("MODE_STR=%s", mode.c_str()));
        throw std::invalid_argument("Invalid access mode.");
    }

    return static_cast<EChannelAccessMode>(
        std::distance(accessModeList.begin(), iter));
}

std::string ChannelConfig::convertToAccessModeString(const uint8_t& value)
{
    if (accessModeList.size() <= value)
    {
        log<level::ERR>("Invalid access mode.", entry("MODE_IDX=%d", value));
        throw std::invalid_argument("Invalid access mode.");
    }

    return accessModeList.at(value);
}

CommandPrivilege
    ChannelConfig::convertToPrivLimitIndex(const std::string& value)
{
    auto iter = std::find(privList.begin(), privList.end(), value);
    if (iter == privList.end())
    {
        log<level::ERR>("Invalid privilege.",
                        entry("PRIV_STR=%s", value.c_str()));
        throw std::invalid_argument("Invalid privilege.");
    }

    return static_cast<CommandPrivilege>(std::distance(privList.begin(), iter));
}

std::string ChannelConfig::convertToPrivLimitString(const uint8_t& value)
{
    if (privList.size() <= value)
    {
        log<level::ERR>("Invalid privilege.", entry("PRIV_IDX=%d", value));
        throw std::invalid_argument("Invalid privilege.");
    }

    return privList.at(value);
}

EChannelSessSupported
    ChannelConfig::convertToSessionSupportIndex(const std::string& value)
{
    auto iter =
        std::find(sessionSupportList.begin(), sessionSupportList.end(), value);
    if (iter == sessionSupportList.end())
    {
        log<level::ERR>("Invalid session supported.",
                        entry("SESS_STR=%s", value.c_str()));
        throw std::invalid_argument("Invalid session supported.");
    }

    return static_cast<EChannelSessSupported>(
        std::distance(sessionSupportList.begin(), iter));
}

EChannelMediumType
    ChannelConfig::convertToMediumTypeIndex(const std::string& value)
{
    std::unordered_map<std::string, EChannelMediumType>::iterator it =
        mediumTypeMap.find(value);
    if (it == mediumTypeMap.end())
    {
        log<level::ERR>("Invalid medium type.",
                        entry("MEDIUM_STR=%s", value.c_str()));
        throw std::invalid_argument("Invalid medium type.");
    }

    return static_cast<EChannelMediumType>(it->second);
}

EChannelProtocolType
    ChannelConfig::convertToProtocolTypeIndex(const std::string& value)
{
    std::unordered_map<std::string, EChannelProtocolType>::iterator it =
        protocolTypeMap.find(value);
    if (it == protocolTypeMap.end())
    {
        log<level::ERR>("Invalid protocol type.",
                        entry("PROTO_STR=%s", value.c_str()));
        throw std::invalid_argument("Invalid protocol type.");
    }

    return static_cast<EChannelProtocolType>(it->second);
}

Json ChannelConfig::readJsonFile(const std::string& configFile)
{
    std::ifstream jsonFile(configFile);
    if (!jsonFile.good())
    {
        log<level::ERR>("JSON file not found");
        return nullptr;
    }

    Json data = nullptr;
    try
    {
        data = Json::parse(jsonFile, nullptr, false);
    }
    catch (Json::parse_error& e)
    {
        log<level::DEBUG>("Corrupted channel config.",
                          entry("MSG: %s", e.what()));
        throw std::runtime_error("Corrupted channel config file");
    }

    return data;
}

int ChannelConfig::writeJsonFile(const std::string& configFile,
                                 const Json& jsonData)
{
    std::ofstream jsonFile(configFile);
    if (!jsonFile.good())
    {
        log<level::ERR>("JSON file not found");
        return -EIO;
    }

    // Write JSON to file
    jsonFile << jsonData;

    jsonFile.flush();
    return 0;
}

void ChannelConfig::setDefaultChannelConfig(const uint8_t& chNum,
                                            const std::string& chName)
{
    channelData[chNum].chName = chName;
    channelData[chNum].chID = chNum;
    channelData[chNum].isChValid = false;
    channelData[chNum].activeSessCount = 0;

    channelData[chNum].chInfo.mediumType = defaultMediumType;
    channelData[chNum].chInfo.protocolType = defaultProtocolType;
    channelData[chNum].chInfo.sessionSupported = defaultSessionSupported;
    channelData[chNum].chInfo.isIpmi = defaultIsIpmiState;
    channelData[chNum].chInfo.authTypeSupported = defaultAuthType;
}

int ChannelConfig::loadChannelConfig()
{
    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};

    Json data = readJsonFile(channelConfigDefaultFilename);
    if (data == nullptr)
    {
        log<level::DEBUG>("Error in opening IPMI Channel data file");
        return -EIO;
    }

    try
    {
        // Fill in global structure
        for (uint8_t chNum = 0; chNum < maxIpmiChannels; chNum++)
        {
            std::fill(reinterpret_cast<uint8_t*>(&channelData[chNum]),
                      reinterpret_cast<uint8_t*>(&channelData[chNum]) +
                          sizeof(ChannelData),
                      0);
            std::string chKey = std::to_string(chNum);
            Json jsonChData = data[chKey].get<Json>();
            if (jsonChData.is_null())
            {
                log<level::WARNING>(
                    "Channel not configured so loading default.",
                    entry("CHANNEL_NUM:%d", chNum));
                // If user didn't want to configure specific channel (say
                // reserved channel), then load that index with default values.
                std::string chName(defaultChannelName);
                setDefaultChannelConfig(chNum, chName);
            }
            else
            {
                std::string chName = jsonChData[nameString].get<std::string>();
                channelData[chNum].chName = chName;
                channelData[chNum].chID = chNum;
                channelData[chNum].isChValid =
                    jsonChData[isValidString].get<bool>();
                channelData[chNum].activeSessCount =
                    jsonChData.value(activeSessionsString, 0);
                Json jsonChInfo = jsonChData[channelInfoString].get<Json>();
                if (jsonChInfo.is_null())
                {
                    log<level::ERR>("Invalid/corrupted channel config file");
                    return -EBADMSG;
                }
                else
                {
                    std::string medTypeStr =
                        jsonChInfo[mediumTypeString].get<std::string>();
                    channelData[chNum].chInfo.mediumType = static_cast<uint8_t>(
                        convertToMediumTypeIndex(medTypeStr));
                    std::string protoTypeStr =
                        jsonChInfo[protocolTypeString].get<std::string>();
                    channelData[chNum].chInfo.protocolType =
                        static_cast<uint8_t>(
                            convertToProtocolTypeIndex(protoTypeStr));
                    std::string sessStr =
                        jsonChInfo[sessionSupportedString].get<std::string>();
                    channelData[chNum].chInfo.sessionSupported =
                        static_cast<uint8_t>(
                            convertToSessionSupportIndex(sessStr));
                    channelData[chNum].chInfo.isIpmi =
                        jsonChInfo[isIpmiString].get<bool>();
                    channelData[chNum].chInfo.authTypeSupported =
                        defaultAuthType;
                }
            }
        }
    }
    catch (const Json::exception& e)
    {
        log<level::DEBUG>("Json Exception caught.", entry("MSG:%s", e.what()));
        return -EBADMSG;
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>("Corrupted config.", entry("MSG:%s", e.what()));
        return -EBADMSG;
    }

    return 0;
}

int ChannelConfig::readChannelVolatileData()
{
    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};

    Json data = readJsonFile(channelVolatileDataFilename);
    if (data == nullptr)
    {
        log<level::DEBUG>("Error in opening IPMI Channel data file");
        return -EIO;
    }

    try
    {
        // Fill in global structure
        for (auto it = data.begin(); it != data.end(); ++it)
        {
            std::string chKey = it.key();
            uint8_t chNum = std::stoi(chKey, nullptr, 10);
            if ((chNum < 0) || (chNum > maxIpmiChannels))
            {
                log<level::DEBUG>(
                    "Invalid channel access entry in config file");
                throw std::out_of_range("Out of range - channel number");
            }
            Json jsonChData = it.value();
            if (!jsonChData.is_null())
            {
                std::string accModeStr =
                    jsonChData[accessModeString].get<std::string>();
                channelData[chNum].chAccess.chVolatileData.accessMode =
                    static_cast<uint8_t>(convertToAccessModeIndex(accModeStr));
                channelData[chNum].chAccess.chVolatileData.userAuthDisabled =
                    jsonChData[userAuthDisabledString].get<bool>();
                channelData[chNum].chAccess.chVolatileData.perMsgAuthDisabled =
                    jsonChData[perMsgAuthDisabledString].get<bool>();
                channelData[chNum].chAccess.chVolatileData.alertingDisabled =
                    jsonChData[alertingDisabledString].get<bool>();
                std::string privStr =
                    jsonChData[privLimitString].get<std::string>();
                channelData[chNum].chAccess.chVolatileData.privLimit =
                    static_cast<uint8_t>(convertToPrivLimitIndex(privStr));
            }
            else
            {
                log<level::ERR>(
                    "Invalid/corrupted volatile channel access file",
                    entry("FILE: %s", channelVolatileDataFilename));
                throw std::runtime_error(
                    "Corrupted volatile channel access file");
            }
        }
    }
    catch (const Json::exception& e)
    {
        log<level::DEBUG>("Json Exception caught.", entry("MSG:%s", e.what()));
        throw std::runtime_error("Corrupted volatile channel access file");
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>("Corrupted config.", entry("MSG:%s", e.what()));
        throw std::runtime_error("Corrupted volatile channel access file");
    }

    // Update the timestamp
    voltFileLastUpdatedTime = getUpdatedFileTime(channelVolatileDataFilename);
    return 0;
}

int ChannelConfig::readChannelPersistData()
{
    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};

    Json data = readJsonFile(channelNvDataFilename);
    if (data == nullptr)
    {
        log<level::DEBUG>("Error in opening IPMI Channel data file");
        return -EIO;
    }

    try
    {
        // Fill in global structure
        for (auto it = data.begin(); it != data.end(); ++it)
        {
            std::string chKey = it.key();
            uint8_t chNum = std::stoi(chKey, nullptr, 10);
            if ((chNum < 0) || (chNum > maxIpmiChannels))
            {
                log<level::DEBUG>(
                    "Invalid channel access entry in config file");
                throw std::out_of_range("Out of range - channel number");
            }
            Json jsonChData = it.value();
            if (!jsonChData.is_null())
            {
                std::string accModeStr =
                    jsonChData[accessModeString].get<std::string>();
                channelData[chNum].chAccess.chNonVolatileData.accessMode =
                    static_cast<uint8_t>(convertToAccessModeIndex(accModeStr));
                channelData[chNum].chAccess.chNonVolatileData.userAuthDisabled =
                    jsonChData[userAuthDisabledString].get<bool>();
                channelData[chNum]
                    .chAccess.chNonVolatileData.perMsgAuthDisabled =
                    jsonChData[perMsgAuthDisabledString].get<bool>();
                channelData[chNum].chAccess.chNonVolatileData.alertingDisabled =
                    jsonChData[alertingDisabledString].get<bool>();
                std::string privStr =
                    jsonChData[privLimitString].get<std::string>();
                channelData[chNum].chAccess.chNonVolatileData.privLimit =
                    static_cast<uint8_t>(convertToPrivLimitIndex(privStr));
            }
            else
            {
                log<level::ERR>("Invalid/corrupted nv channel access file",
                                entry("FILE:%s", channelNvDataFilename));
                throw std::runtime_error("Corrupted nv channel access file");
            }
        }
    }
    catch (const Json::exception& e)
    {
        log<level::DEBUG>("Json Exception caught.", entry("MSG:%s", e.what()));
        throw std::runtime_error("Corrupted nv channel access file");
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>("Corrupted config.", entry("MSG: %s", e.what()));
        throw std::runtime_error("Corrupted nv channel access file");
    }

    // Update the timestamp
    nvFileLastUpdatedTime = getUpdatedFileTime(channelNvDataFilename);
    return 0;
}

int ChannelConfig::writeChannelVolatileData()
{
    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};
    Json outData;

    try
    {
        for (uint8_t chNum = 0; chNum < maxIpmiChannels; chNum++)
        {
            if (getChannelSessionSupport(chNum) != EChannelSessSupported::none)
            {
                Json jsonObj;
                std::string chKey = std::to_string(chNum);
                std::string accModeStr = convertToAccessModeString(
                    channelData[chNum].chAccess.chVolatileData.accessMode);
                jsonObj[accessModeString] = accModeStr;
                jsonObj[userAuthDisabledString] =
                    channelData[chNum].chAccess.chVolatileData.userAuthDisabled;
                jsonObj[perMsgAuthDisabledString] =
                    channelData[chNum]
                        .chAccess.chVolatileData.perMsgAuthDisabled;
                jsonObj[alertingDisabledString] =
                    channelData[chNum].chAccess.chVolatileData.alertingDisabled;
                std::string privStr = convertToPrivLimitString(
                    channelData[chNum].chAccess.chVolatileData.privLimit);
                jsonObj[privLimitString] = privStr;

                outData[chKey] = jsonObj;
            }
        }
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>("Corrupted config.", entry("MSG: %s", e.what()));
        return -EINVAL;
    }

    if (writeJsonFile(channelVolatileDataFilename, outData) != 0)
    {
        log<level::DEBUG>("Error in write JSON data to file");
        return -EIO;
    }

    // Update the timestamp
    voltFileLastUpdatedTime = getUpdatedFileTime(channelVolatileDataFilename);
    return 0;
}

int ChannelConfig::writeChannelPersistData()
{
    boost::interprocess::scoped_lock<boost::interprocess::named_recursive_mutex>
        channelLock{*channelMutex};
    Json outData;

    try
    {
        for (uint8_t chNum = 0; chNum < maxIpmiChannels; chNum++)
        {
            if (getChannelSessionSupport(chNum) != EChannelSessSupported::none)
            {
                Json jsonObj;
                std::string chKey = std::to_string(chNum);
                std::string accModeStr = convertToAccessModeString(
                    channelData[chNum].chAccess.chNonVolatileData.accessMode);
                jsonObj[accessModeString] = accModeStr;
                jsonObj[userAuthDisabledString] =
                    channelData[chNum]
                        .chAccess.chNonVolatileData.userAuthDisabled;
                jsonObj[perMsgAuthDisabledString] =
                    channelData[chNum]
                        .chAccess.chNonVolatileData.perMsgAuthDisabled;
                jsonObj[alertingDisabledString] =
                    channelData[chNum]
                        .chAccess.chNonVolatileData.alertingDisabled;
                std::string privStr = convertToPrivLimitString(
                    channelData[chNum].chAccess.chNonVolatileData.privLimit);
                jsonObj[privLimitString] = privStr;

                outData[chKey] = jsonObj;
            }
        }
    }
    catch (const std::invalid_argument& e)
    {
        log<level::ERR>("Corrupted config.", entry("MSG: %s", e.what()));
        return -EINVAL;
    }

    if (writeJsonFile(channelNvDataFilename, outData) != 0)
    {
        log<level::DEBUG>("Error in write JSON data to file");
        return -EIO;
    }

    // Update the timestamp
    nvFileLastUpdatedTime = getUpdatedFileTime(channelNvDataFilename);
    return 0;
}

int ChannelConfig::checkAndReloadNVData()
{
    std::time_t updateTime = getUpdatedFileTime(channelNvDataFilename);
    int ret = 0;
    if (updateTime != nvFileLastUpdatedTime || updateTime == -EIO)
    {
        try
        {
            ret = readChannelPersistData();
        }
        catch (const std::exception& e)
        {
            log<level::ERR>("Exception caught in readChannelPersistData.",
                            entry("MSG=%s", e.what()));
            ret = -EIO;
        }
    }
    return ret;
}

int ChannelConfig::checkAndReloadVolatileData()
{
    std::time_t updateTime = getUpdatedFileTime(channelVolatileDataFilename);
    int ret = 0;
    if (updateTime != voltFileLastUpdatedTime || updateTime == -EIO)
    {
        try
        {
            ret = readChannelVolatileData();
        }
        catch (const std::exception& e)
        {
            log<level::ERR>("Exception caught in readChannelVolatileData.",
                            entry("MSG=%s", e.what()));
            ret = -EIO;
        }
    }
    return ret;
}

void ChannelConfig::initChannelPersistData()
{
    /* Always read the channel config */
    if (loadChannelConfig() != 0)
    {
        log<level::ERR>("Failed to read channel config file");
        throw std::ios_base::failure("Failed to load channel configuration");
    }

    /* Populate the channel persist data */
    if (readChannelPersistData() != 0)
    {
        // Copy default NV data to RW location
        std::experimental::filesystem::copy_file(channelAccessDefaultFilename,
                                                 channelNvDataFilename);

        // Load the channel access NV data
        if (readChannelPersistData() != 0)
        {
            log<level::ERR>("Failed to read channel access NV data");
            throw std::ios_base::failure(
                "Failed to read channel access NV configuration");
        }
    }

    // First check the volatile data file
    // If not present, load the default values
    if (readChannelVolatileData() != 0)
    {
        // Copy default volatile data to temporary location
        // NV file(channelNvDataFilename) must have created by now.
        std::experimental::filesystem::copy_file(channelNvDataFilename,
                                                 channelVolatileDataFilename);

        // Load the channel access volatile data
        if (readChannelVolatileData() != 0)
        {
            log<level::ERR>("Failed to read channel access volatile data");
            throw std::ios_base::failure(
                "Failed to read channel access volatile configuration");
        }
    }
    return;
}

} // namespace ipmi