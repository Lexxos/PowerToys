#include "pch.h"
#include "FancyZonesData.h"
#include "FancyZonesDataTypes.h"
#include "JsonHelpers.h"
#include "ZoneSet.h"
#include "Settings.h"
#include "CallTracer.h"
#include "GuidUtils.h"

#include <common/Display/dpi_aware.h>
#include <common/utils/json.h>
#include <FancyZonesLib/util.h>
#include <FancyZonesLib/FancyZonesWindowProperties.h>

#include <shlwapi.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <common/utils/process_path.h>
#include <common/logger/logger.h>

// Non-localizable strings
namespace NonLocalizable
{
    const wchar_t NullStr[] = L"null";

    const wchar_t FancyZonesSettingsFile[] = L"settings.json";
    const wchar_t FancyZonesDataFile[] = L"zones-settings.json";
    const wchar_t FancyZonesAppZoneHistoryFile[] = L"app-zone-history.json";
    const wchar_t FancyZonesEditorParametersFile[] = L"editor-parameters.json";
    const wchar_t RegistryPath[] = L"Software\\SuperFancyZones";
}

namespace
{
    std::wstring ExtractVirtualDesktopId(const std::wstring& deviceId)
    {
        // Format: <device-id>_<resolution>_<virtual-desktop-id>
        return deviceId.substr(deviceId.rfind('_') + 1);
    }

    const std::wstring& GetTempDirPath()
    {
        static std::wstring tmpDirPath;
        static std::once_flag flag;

        std::call_once(flag, []() {
            wchar_t buffer[MAX_PATH];

            auto charsWritten = GetTempPath(MAX_PATH, buffer);
            if (charsWritten > MAX_PATH || (charsWritten == 0))
            {
                abort();
            }

            tmpDirPath = std::wstring{ buffer };
        });

        return tmpDirPath;
    }

    bool DeleteRegistryKey(HKEY hKeyRoot, LPTSTR lpSubKey)
    {
        // First, see if we can delete the key without having to recurse.
        if (ERROR_SUCCESS == RegDeleteKey(hKeyRoot, lpSubKey))
        {
            return true;
        }

        HKEY hKey;
        if (ERROR_SUCCESS != RegOpenKeyEx(hKeyRoot, lpSubKey, 0, KEY_READ, &hKey))
        {
            return false;
        }

        // Check for an ending slash and add one if it is missing.
        LPTSTR lpEnd = lpSubKey + lstrlen(lpSubKey);

        if (*(lpEnd - 1) != TEXT('\\'))
        {
            *lpEnd = TEXT('\\');
            lpEnd++;
            *lpEnd = TEXT('\0');
        }

        // Enumerate the keys

        DWORD dwSize = MAX_PATH;
        TCHAR szName[MAX_PATH];
        FILETIME ftWrite;
        auto result = RegEnumKeyEx(hKey, 0, szName, &dwSize, NULL, NULL, NULL, &ftWrite);

        if (result == ERROR_SUCCESS)
        {
            do
            {
                *lpEnd = TEXT('\0');
                StringCchCat(lpSubKey, MAX_PATH * 2, szName);

                if (!DeleteRegistryKey(hKeyRoot, lpSubKey))
                {
                    break;
                }

                dwSize = MAX_PATH;
                result = RegEnumKeyEx(hKey, 0, szName, &dwSize, NULL, NULL, NULL, &ftWrite);
            } while (result == ERROR_SUCCESS);
        }

        lpEnd--;
        *lpEnd = TEXT('\0');

        RegCloseKey(hKey);

        // Try again to delete the root key.
        if (ERROR_SUCCESS == RegDeleteKey(hKeyRoot, lpSubKey))
        {
            return true;
        }

        return false;
    }

    bool DeleteFancyZonesRegistryData()
    {
        wchar_t key[256];
        StringCchPrintf(key, ARRAYSIZE(key), L"%s", NonLocalizable::RegistryPath);

        HKEY hKey;
        if (ERROR_FILE_NOT_FOUND == RegOpenKeyEx(HKEY_CURRENT_USER, key, 0, KEY_READ, &hKey))
        {
            return true;
        }
        else
        {
            return DeleteRegistryKey(HKEY_CURRENT_USER, key);
        }
    }
}

FancyZonesData& FancyZonesDataInstance()
{
    static FancyZonesData instance;
    return instance;
}

FancyZonesData::FancyZonesData()
{
    std::wstring saveFolderPath = PTSettingsHelper::get_module_save_folder_location(NonLocalizable::FancyZonesStr);

    settingsFileName = saveFolderPath + L"\\" + std::wstring(NonLocalizable::FancyZonesSettingsFile);
    zonesSettingsFileName = saveFolderPath + L"\\" + std::wstring(NonLocalizable::FancyZonesDataFile);
    appZoneHistoryFileName = saveFolderPath + L"\\" + std::wstring(NonLocalizable::FancyZonesAppZoneHistoryFile);
    editorParametersFileName = saveFolderPath + L"\\" + std::wstring(NonLocalizable::FancyZonesEditorParametersFile);
}

void FancyZonesData::SetVirtualDesktopCheckCallback(std::function<bool(GUID)> callback)
{
    m_virtualDesktopCheckCallback = callback;
}

const JSONHelpers::TDeviceInfoMap& FancyZonesData::GetDeviceInfoMap() const
{
    std::scoped_lock lock{ dataLock };
    return deviceInfoMap;
}

const JSONHelpers::TCustomZoneSetsMap& FancyZonesData::GetCustomZoneSetsMap() const
{
    std::scoped_lock lock{ dataLock };
    return customZoneSetsMap;
}

const std::unordered_map<std::wstring, std::vector<FancyZonesDataTypes::AppZoneHistoryData>>& FancyZonesData::GetAppZoneHistoryMap() const
{
    std::scoped_lock lock{ dataLock };
    return appZoneHistoryMap;
}

std::optional<FancyZonesDataTypes::DeviceInfoData> FancyZonesData::FindDeviceInfo(const FancyZonesDataTypes::DeviceIdData& id) const
{
    std::scoped_lock lock{ dataLock };
    for (const auto& [deviceId, deviceInfo] : deviceInfoMap)
    {
        if (id.isEqualWithNullVirtualDesktopId(deviceId))
        {
            return deviceInfo;
        }
    }

    return std::nullopt;
}

std::optional<FancyZonesDataTypes::CustomZoneSetData> FancyZonesData::FindCustomZoneSet(const std::wstring& guid) const
{
    std::scoped_lock lock{ dataLock };
    auto it = customZoneSetsMap.find(guid);
    return it != end(customZoneSetsMap) ? std::optional{ it->second } : std::nullopt;
}

bool FancyZonesData::AddDevice(const FancyZonesDataTypes::DeviceIdData& deviceId)
{
    _TRACER_;
    using namespace FancyZonesDataTypes;

    auto deviceInfo = FindDeviceInfo(deviceId);

    std::scoped_lock lock{ dataLock };

    if (!deviceInfo.has_value())
    {
        wil::unique_cotaskmem_string virtualDesktopId;
        if (SUCCEEDED(StringFromCLSID(deviceId.virtualDesktopId, &virtualDesktopId)))
        {
            Logger::info(L"Create new device on virtual desktop {}", virtualDesktopId.get());
        }
        
        // Creates default entry in map when WorkArea is created
        GUID guid;
        auto result{ CoCreateGuid(&guid) };
        wil::unique_cotaskmem_string guidString;
        if (result == S_OK && SUCCEEDED(StringFromCLSID(guid, &guidString)))
        {
            const ZoneSetData zoneSetData{ guidString.get(), ZoneSetLayoutType::PriorityGrid };
            DeviceInfoData defaultDeviceInfoData{ zoneSetData, DefaultValues::ShowSpacing, DefaultValues::Spacing, DefaultValues::ZoneCount, DefaultValues::SensitivityRadius };
            deviceInfoMap[deviceId] = std::move(defaultDeviceInfoData);
            return true;
        }
        else
        {
            Logger::error("Failed to create an ID for the new layout");
        }
    }

    return false;
}

void FancyZonesData::CloneDeviceInfo(const FancyZonesDataTypes::DeviceIdData& source, const FancyZonesDataTypes::DeviceIdData& destination)
{
    if (source == destination)
    {
        return;
    }
    std::scoped_lock lock{ dataLock };

    // The source virtual desktop is deleted, simply ignore it.
    if (!FindDeviceInfo(source).has_value())
    {
        return;
    }

    deviceInfoMap[destination] = deviceInfoMap[source];
}

void FancyZonesData::SyncVirtualDesktops(GUID currentVirtualDesktopId)
{
    _TRACER_;
    // Explorer persists current virtual desktop identifier to registry on a per session basis,
    // but only after first virtual desktop switch happens. If the user hasn't switched virtual
    // desktops in this session value in registry will be empty and we will use default GUID in
    // that case (00000000-0000-0000-0000-000000000000).
    // This method will go through all our persisted data with default GUID and update it with
    // valid one.

    std::scoped_lock lock{ dataLock };
    bool dirtyFlag = false;

    for (auto& [path, perDesktopData] : appZoneHistoryMap)
    {
        for (auto& data : perDesktopData)
        {
            if (data.deviceId.virtualDesktopId == GUID_NULL)
            {
                data.deviceId.virtualDesktopId = currentVirtualDesktopId;
                dirtyFlag = true;
            }
            else
            {
                if (m_virtualDesktopCheckCallback && !m_virtualDesktopCheckCallback(data.deviceId.virtualDesktopId))
                {
                    data.deviceId.virtualDesktopId = GUID_NULL;
                    dirtyFlag = true;
                }
            }
        }
    }
    
    std::vector<FancyZonesDataTypes::DeviceIdData> replaceWithCurrentId{};
    std::vector<FancyZonesDataTypes::DeviceIdData> replaceWithNullId{};

    for (const auto& [desktopId, data] : deviceInfoMap)
    {
        if (desktopId.virtualDesktopId == GUID_NULL)
        {
            replaceWithCurrentId.push_back(desktopId);
            dirtyFlag = true;
        }
        else
        {
            if (m_virtualDesktopCheckCallback && !m_virtualDesktopCheckCallback(desktopId.virtualDesktopId))
            {
                replaceWithNullId.push_back(desktopId);
                dirtyFlag = true;
            }
        }
    }
    
    for (const auto& id : replaceWithCurrentId)
    {
        auto mapEntry = deviceInfoMap.extract(id);
        mapEntry.key().virtualDesktopId = currentVirtualDesktopId;
        deviceInfoMap.insert(std::move(mapEntry));
    }

    for (const auto& id : replaceWithNullId)
    {
        auto mapEntry = deviceInfoMap.extract(id);
        mapEntry.key().virtualDesktopId = GUID_NULL;
        deviceInfoMap.insert(std::move(mapEntry));
    }
    
    if (dirtyFlag)
    {
        wil::unique_cotaskmem_string virtualDesktopIdStr;
        if (SUCCEEDED(StringFromCLSID(currentVirtualDesktopId, &virtualDesktopIdStr)))
        {
            Logger::info(L"Update Virtual Desktop id to {}", virtualDesktopIdStr.get()); 
        }

        SaveAppZoneHistoryAndZoneSettings();
    }
}

void FancyZonesData::RemoveDeletedDesktops(const std::vector<GUID>& activeDesktops)
{
    std::unordered_set<GUID> active(std::begin(activeDesktops), std::end(activeDesktops));
    std::scoped_lock lock{ dataLock };
    bool dirtyFlag = false;

    for (auto it = std::begin(deviceInfoMap); it != std::end(deviceInfoMap);)
    {
        GUID desktopId = it->first.virtualDesktopId;

        if (desktopId != GUID_NULL)
        {
            auto foundId = active.find(desktopId);
            if (foundId == std::end(active))
            {
                wil::unique_cotaskmem_string virtualDesktopIdStr;
                if (SUCCEEDED(StringFromCLSID(desktopId, &virtualDesktopIdStr)))
                {
                    Logger::info(L"Remove Virtual Desktop id {}", virtualDesktopIdStr.get());
                }

                RemoveDesktopAppZoneHistory(desktopId);
                it = deviceInfoMap.erase(it);
                dirtyFlag = true;
                continue;
            }
        }
        ++it;
    }

    if (dirtyFlag)
    {
        SaveAppZoneHistoryAndZoneSettings();
    }
}

bool FancyZonesData::IsAnotherWindowOfApplicationInstanceZoned(HWND window, const FancyZonesDataTypes::DeviceIdData& deviceId) const
{
    std::scoped_lock lock{ dataLock };
    auto processPath = get_process_path(window);
    if (!processPath.empty())
    {
        auto history = appZoneHistoryMap.find(processPath);
        if (history != std::end(appZoneHistoryMap))
        {
            auto& perDesktopData = history->second;
            for (auto& data : perDesktopData)
            {
                if (data.deviceId.isEqualWithNullVirtualDesktopId(deviceId))
                {
                    DWORD processId = 0;
                    GetWindowThreadProcessId(window, &processId);

                    auto processIdIt = data.processIdToHandleMap.find(processId);

                    if (processIdIt == std::end(data.processIdToHandleMap))
                    {
                        return false;
                    }
                    else if (processIdIt->second != window && IsWindow(processIdIt->second))
                    {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

void FancyZonesData::UpdateProcessIdToHandleMap(HWND window, const FancyZonesDataTypes::DeviceIdData& deviceId)
{
    std::scoped_lock lock{ dataLock };
    auto processPath = get_process_path(window);
    if (!processPath.empty())
    {
        auto history = appZoneHistoryMap.find(processPath);
        if (history != std::end(appZoneHistoryMap))
        {
            auto& perDesktopData = history->second;
            for (auto& data : perDesktopData)
            {
                if (data.deviceId.isEqualWithNullVirtualDesktopId(deviceId))
                {
                    DWORD processId = 0;
                    GetWindowThreadProcessId(window, &processId);
                    data.processIdToHandleMap[processId] = window;
                    break;
                }
            }
        }
    }
}

ZoneIndexSet FancyZonesData::GetAppLastZoneIndexSet(HWND window, const FancyZonesDataTypes::DeviceIdData& deviceId, const std::wstring_view& zoneSetId) const
{
    std::scoped_lock lock{ dataLock };
    auto processPath = get_process_path(window);
    if (!processPath.empty())
    {
        auto history = appZoneHistoryMap.find(processPath);
        if (history != std::end(appZoneHistoryMap))
        {
            const auto& perDesktopData = history->second;
            for (const auto& data : perDesktopData)
            {
                if (data.zoneSetUuid == zoneSetId && data.deviceId.isEqualWithNullVirtualDesktopId(deviceId))
                {
                    return data.zoneIndexSet;
                }
            }
        }
    }

    return {};
}

bool FancyZonesData::RemoveAppLastZone(HWND window, const FancyZonesDataTypes::DeviceIdData& deviceId, const std::wstring_view& zoneSetId)
{
    _TRACER_;
    std::scoped_lock lock{ dataLock };
    auto processPath = get_process_path(window);
    if (!processPath.empty())
    {
        auto history = appZoneHistoryMap.find(processPath);
        if (history != std::end(appZoneHistoryMap))
        {
            auto& perDesktopData = history->second;
            for (auto data = std::begin(perDesktopData); data != std::end(perDesktopData);)
            {
                if (data->deviceId.isEqualWithNullVirtualDesktopId(deviceId) && data->zoneSetUuid == zoneSetId)
                {
                    if (!IsAnotherWindowOfApplicationInstanceZoned(window, deviceId))
                    {
                        DWORD processId = 0;
                        GetWindowThreadProcessId(window, &processId);

                        data->processIdToHandleMap.erase(processId);
                    }

                    // if there is another instance of same application placed in the same zone don't erase history
                    ZoneIndex windowZoneStamp = reinterpret_cast<ZoneIndex>(::GetProp(window, ZonedWindowProperties::PropertyMultipleZoneID));
                    for (auto placedWindow : data->processIdToHandleMap)
                    {
                        ZoneIndex placedWindowZoneStamp = reinterpret_cast<ZoneIndex>(::GetProp(placedWindow.second, ZonedWindowProperties::PropertyMultipleZoneID));
                        if (IsWindow(placedWindow.second) && (windowZoneStamp == placedWindowZoneStamp))
                        {
                            return false;
                        }
                    }

                    data = perDesktopData.erase(data);
                    if (perDesktopData.empty())
                    {
                        appZoneHistoryMap.erase(processPath);
                    }
                    SaveAppZoneHistory();
                    return true;
                }
                else
                {
                    ++data;
                }
            }
        }
    }

    return false;
}

bool FancyZonesData::SetAppLastZones(HWND window, const FancyZonesDataTypes::DeviceIdData& deviceId, const std::wstring& zoneSetId, const ZoneIndexSet& zoneIndexSet)
{
    _TRACER_;
    std::scoped_lock lock{ dataLock };

    if (IsAnotherWindowOfApplicationInstanceZoned(window, deviceId))
    {
        return false;
    }

    auto processPath = get_process_path(window);
    if (processPath.empty())
    {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);

    auto history = appZoneHistoryMap.find(processPath);
    if (history != std::end(appZoneHistoryMap))
    {
        auto& perDesktopData = history->second;
        for (auto& data : perDesktopData)
        {
            if (data.deviceId.isEqualWithNullVirtualDesktopId(deviceId))
            {
                // application already has history on this work area, update it with new window position
                data.processIdToHandleMap[processId] = window;
                data.zoneSetUuid = zoneSetId;
                data.zoneIndexSet = zoneIndexSet;
                SaveAppZoneHistory();
                return true;
            }
        }
    }

    std::unordered_map<DWORD, HWND> processIdToHandleMap{};
    processIdToHandleMap[processId] = window;
    FancyZonesDataTypes::AppZoneHistoryData data{ .processIdToHandleMap = processIdToHandleMap,
                                                  .zoneSetUuid = zoneSetId,
                                                  .deviceId = deviceId,
                                                  .zoneIndexSet = zoneIndexSet };

    if (appZoneHistoryMap.contains(processPath))
    {
        // application already has history but on other desktop, add with new desktop info
        appZoneHistoryMap[processPath].push_back(data);
    }
    else
    {
        // new application, create entry in app zone history map
        appZoneHistoryMap[processPath] = std::vector<FancyZonesDataTypes::AppZoneHistoryData>{ data };
    }

    SaveAppZoneHistory();
    return true;
}

void FancyZonesData::SetActiveZoneSet(const FancyZonesDataTypes::DeviceIdData& deviceId, const FancyZonesDataTypes::ZoneSetData& data)
{
    std::scoped_lock lock{ dataLock };

    for (auto& [deviceIdData, deviceInfo] : deviceInfoMap)
    {
        if (deviceId.isEqualWithNullVirtualDesktopId(deviceIdData))
        {
            deviceInfo.activeZoneSet = data;

            // If the zone set is custom, we need to copy its properties to the device
            auto zonesetIt = customZoneSetsMap.find(data.uuid);
            if (zonesetIt != customZoneSetsMap.end())
            {
                if (zonesetIt->second.type == FancyZonesDataTypes::CustomLayoutType::Grid)
                {
                    auto layoutInfo = std::get<FancyZonesDataTypes::GridLayoutInfo>(zonesetIt->second.info);
                    deviceInfo.sensitivityRadius = layoutInfo.sensitivityRadius();
                    deviceInfo.showSpacing = layoutInfo.showSpacing();
                    deviceInfo.spacing = layoutInfo.spacing();
                    deviceInfo.zoneCount = layoutInfo.zoneCount();
                }
                else if (zonesetIt->second.type == FancyZonesDataTypes::CustomLayoutType::Canvas)
                {
                    auto layoutInfo = std::get<FancyZonesDataTypes::CanvasLayoutInfo>(zonesetIt->second.info);
                    deviceInfo.sensitivityRadius = layoutInfo.sensitivityRadius;
                    deviceInfo.zoneCount = (int)layoutInfo.zones.size();
                }
            }

            break;
        }
    }
}

json::JsonObject FancyZonesData::GetPersistFancyZonesJSON()
{
    return JSONHelpers::GetPersistFancyZonesJSON(zonesSettingsFileName, appZoneHistoryFileName);
}

void FancyZonesData::LoadFancyZonesData()
{
    if (!std::filesystem::exists(zonesSettingsFileName))
    {
        SaveAppZoneHistoryAndZoneSettings();
    }
    else
    {
        json::JsonObject fancyZonesDataJSON = GetPersistFancyZonesJSON();

        appZoneHistoryMap = JSONHelpers::ParseAppZoneHistory(fancyZonesDataJSON);
        deviceInfoMap = JSONHelpers::ParseDeviceInfos(fancyZonesDataJSON);
        customZoneSetsMap = JSONHelpers::ParseCustomZoneSets(fancyZonesDataJSON);
        quickKeysMap = JSONHelpers::ParseQuickKeys(fancyZonesDataJSON);
    }
}

void FancyZonesData::SaveAppZoneHistoryAndZoneSettings() const
{
    SaveZoneSettings();
    SaveAppZoneHistory();
}

void FancyZonesData::SaveZoneSettings() const
{
    _TRACER_;
    std::scoped_lock lock{ dataLock };

    bool dirtyFlag = false;
    JSONHelpers::TDeviceInfoMap updatedDeviceInfoMap;
    if (m_virtualDesktopCheckCallback)
    {
        for (const auto& [id, data] : deviceInfoMap)
        {
            auto updatedId = id;
            if (!m_virtualDesktopCheckCallback(id.virtualDesktopId))
            {
                updatedId.virtualDesktopId = GUID_NULL;
                dirtyFlag = true;
            }

            updatedDeviceInfoMap.insert({ updatedId, data });
        }
    }
    
    if (dirtyFlag)
    {
        JSONHelpers::SaveZoneSettings(zonesSettingsFileName, updatedDeviceInfoMap, customZoneSetsMap, quickKeysMap);
    }
    else
    {
        JSONHelpers::SaveZoneSettings(zonesSettingsFileName, deviceInfoMap, customZoneSetsMap, quickKeysMap);
    }
}

void FancyZonesData::SaveAppZoneHistory() const
{
    _TRACER_;
    std::scoped_lock lock{ dataLock };

    bool dirtyFlag = false;
    std::unordered_map<std::wstring, std::vector<FancyZonesDataTypes::AppZoneHistoryData>> updatedHistory;
    if (m_virtualDesktopCheckCallback)
    {
        for (const auto& [path, dataVector] : appZoneHistoryMap)
        {
            auto updatedVector = dataVector;
            for (auto& data : updatedVector)
            {
                if (!m_virtualDesktopCheckCallback(data.deviceId.virtualDesktopId))
                {
                    data.deviceId.virtualDesktopId = GUID_NULL;
                    dirtyFlag = true;
                }
            }

            updatedHistory.insert(std::make_pair(path, updatedVector));
        }
    }

    if (dirtyFlag)
    {
        JSONHelpers::SaveAppZoneHistory(appZoneHistoryFileName, updatedHistory);
    }
    else
    {
        JSONHelpers::SaveAppZoneHistory(appZoneHistoryFileName, appZoneHistoryMap);
    }    
}

void FancyZonesData::SaveFancyZonesEditorParameters(bool spanZonesAcrossMonitors, const std::wstring& virtualDesktopId, const HMONITOR& targetMonitor, const std::vector<std::pair<HMONITOR, MONITORINFOEX>>& allMonitors) const
{
    JSONHelpers::EditorArgs argsJson; /* json arguments */
    argsJson.processId = GetCurrentProcessId(); /* Process id */
    argsJson.spanZonesAcrossMonitors = spanZonesAcrossMonitors; /* Span zones */

    if (spanZonesAcrossMonitors)
    {
        auto monitorRect = FancyZonesUtils::GetAllMonitorsCombinedRect<&MONITORINFOEX::rcWork>();
        std::wstring monitorId = FancyZonesUtils::GenerateUniqueIdAllMonitorsArea(virtualDesktopId);

        JSONHelpers::MonitorInfo monitorJson;
        monitorJson.id = monitorId;
        monitorJson.top = monitorRect.top;
        monitorJson.left = monitorRect.left;
        monitorJson.width = monitorRect.right - monitorRect.left;
        monitorJson.height = monitorRect.bottom - monitorRect.top;
        monitorJson.isSelected = true;
        monitorJson.dpi = 0; // unused

        argsJson.monitors.emplace_back(std::move(monitorJson)); /* add monitor data */
    }
    else
    {
        // device id map for correct device ids
        std::unordered_map<std::wstring, DWORD> displayDeviceIdxMap;

        for (auto& monitorData : allMonitors)
        {
            HMONITOR monitor = monitorData.first;
            auto monitorInfo = monitorData.second;

            JSONHelpers::MonitorInfo monitorJson;

            std::wstring deviceId = FancyZonesUtils::GetDisplayDeviceId(monitorInfo.szDevice, displayDeviceIdxMap);
            std::wstring monitorId = FancyZonesUtils::GenerateUniqueId(monitor, deviceId, virtualDesktopId);

            if (monitor == targetMonitor)
            {
                monitorJson.isSelected = true; /* Is monitor selected for the main editor window opening */
            }

            monitorJson.id = monitorId; /* Monitor id */

            UINT dpi = 0;
            if (DPIAware::GetScreenDPIForMonitor(monitor, dpi) != S_OK)
            {
                continue;
            }

            monitorJson.dpi = dpi; /* DPI */
            monitorJson.top = monitorInfo.rcWork.top; /* Top coordinate */
            monitorJson.left = monitorInfo.rcWork.left; /* Left coordinate */
            monitorJson.width = monitorInfo.rcWork.right - monitorInfo.rcWork.left; /* Width */
            monitorJson.height = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top; /* Height */
            
            argsJson.monitors.emplace_back(std::move(monitorJson)); /* add monitor data */
        }
    }
    

    json::to_file(editorParametersFileName, JSONHelpers::EditorArgs::ToJson(argsJson));
}

void FancyZonesData::RemoveDesktopAppZoneHistory(GUID desktopId)
{
    for (auto it = std::begin(appZoneHistoryMap); it != std::end(appZoneHistoryMap);)
    {
        auto& perDesktopData = it->second;
        for (auto desktopIt = std::begin(perDesktopData); desktopIt != std::end(perDesktopData);)
        {
            if (desktopIt->deviceId.virtualDesktopId == desktopId)
            {
                desktopIt = perDesktopData.erase(desktopIt);
            }
            else
            {
                ++desktopIt;
            }
        }

        if (perDesktopData.empty())
        {
            it = appZoneHistoryMap.erase(it);
        }
        else
        {
            ++it;
        }
    }
}
