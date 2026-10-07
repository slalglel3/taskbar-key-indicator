#include "device_watcher.h"
#include "logger.h"
#include <dbt.h>
#include <setupapi.h>
#include <initguid.h>
#include <algorithm>

DEFINE_GUID(GUID_DEVINTERFACE_HID_LOCAL, 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
DEFINE_GUID(GUID_DEVINTERFACE_KEYBOARD_LOCAL, 0x884B96C3, 0x56EF, 0x11D1, 0xBC, 0x8C, 0x00, 0xA0, 0xC9, 0x14, 0x05, 0xDD);

#ifndef DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES 0x00000004
#endif

DeviceWatcher::DeviceWatcher()
    : m_hWnd(NULL)
    , m_hDevNotify(NULL)
    , m_isTargetConnected(true)
    , m_autoDetect(true)
    , m_baselineCount(0)
    , m_baselineEstablished(false)
{
}

DeviceWatcher::~DeviceWatcher() {
    Cleanup();
}

bool DeviceWatcher::Initialize(HWND hWnd) {
    m_hWnd = hWnd;

    DEV_BROADCAST_DEVICEINTERFACE_W filter = { 0 };
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID_LOCAL;

    m_hDevNotify = RegisterDeviceNotificationW(
        m_hWnd,
        &filter,
        DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
    );

    if (m_hDevNotify) {
        Logger::Log(L"[Device] RegisterDeviceNotification succeeded (hDevNotify: %p)", m_hDevNotify);
    } else {
        Logger::Log(L"[Device] RegisterDeviceNotification failed! Error: %lu", GetLastError());
    }

    ResetBaseline();
    return true;
}

void DeviceWatcher::Cleanup() {
    if (m_hDevNotify) {
        UnregisterDeviceNotification(m_hDevNotify);
        m_hDevNotify = NULL;
    }
}

KeyboardDeviceInfo DeviceWatcher::ParseDevicePath(const std::wstring& path) {
    KeyboardDeviceInfo info;
    info.devicePath = path;
    info.isExternal = false;

    std::wstring upper = path;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);

    size_t vidPos = upper.find(L"VID_");
    if (vidPos != std::wstring::npos && vidPos + 8 <= upper.length()) {
        info.vid = upper.substr(vidPos + 4, 4);
    }

    size_t pidPos = upper.find(L"PID_");
    if (pidPos != std::wstring::npos && pidPos + 8 <= upper.length()) {
        info.pid = upper.substr(pidPos + 4, 4);
    }

    if (!info.vid.empty() && !info.pid.empty()) {
        if (upper.find(L"ACPI") == std::wstring::npos && upper.find(L"ROOT") == std::wstring::npos) {
            info.isExternal = true;
        }
    }

    if (!info.vid.empty() && !info.pid.empty()) {
        info.friendlyName = L"Keyboard (VID:" + info.vid + L" PID:" + info.pid + L")";
    } else {
        info.friendlyName = L"Internal/Virtual Keyboard";
    }

    return info;
}

bool DeviceWatcher::IsDevicePhysicallyPresent(const std::wstring& vid, const std::wstring& pid) {
    if (vid.empty() || pid.empty()) return false;

    std::wstring uVid = vid;
    std::wstring uPid = pid;
    std::transform(uVid.begin(), uVid.end(), uVid.begin(), ::towupper);
    std::transform(uPid.begin(), uPid.end(), uPid.begin(), ::towupper);

    // SetupAPI DIGCF_PRESENT: Windows 하드웨어 관리자와 동일한 물리적 실재성 실시간 검사
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_HID_LOCAL,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return false;
    }

    SP_DEVICE_INTERFACE_DATA ifData = { sizeof(SP_DEVICE_INTERFACE_DATA) };
    bool found = false;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_HID_LOCAL, i, &ifData); ++i) {
        DWORD reqSize = 0;
        SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &reqSize, NULL);
        if (reqSize > 0) {
            std::vector<BYTE> buf(reqSize);
            PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

            if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, NULL, NULL)) {
                std::wstring path = detail->DevicePath;
                std::wstring upper = path;
                std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);

                if (upper.find(L"VID_" + uVid) != std::wstring::npos &&
                    upper.find(L"PID_" + uPid) != std::wstring::npos) {
                    found = true;
                    break;
                }
            }
        }
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

std::vector<KeyboardDeviceInfo> DeviceWatcher::GetConnectedKeyboards() {
    std::vector<KeyboardDeviceInfo> list;

    UINT numDevices = 0;
    if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0 || numDevices == 0) {
        return list;
    }

    std::vector<RAWINPUTDEVICELIST> rawList(numDevices);
    if (GetRawInputDeviceList(rawList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        return list;
    }

    for (const auto& rawDev : rawList) {
        if (rawDev.dwType != RIM_TYPEKEYBOARD) continue;

        UINT nameSize = 0;
        GetRawInputDeviceInfoW(rawDev.hDevice, RIDI_DEVICENAME, NULL, &nameSize);
        if (nameSize == 0) continue;

        std::vector<wchar_t> nameBuf(nameSize);
        if (GetRawInputDeviceInfoW(rawDev.hDevice, RIDI_DEVICENAME, nameBuf.data(), &nameSize) > 0) {
            std::wstring devPath(nameBuf.data());
            KeyboardDeviceInfo info = ParseDevicePath(devPath);

            bool alreadyExists = false;
            for (const auto& item : list) {
                if (!info.vid.empty() && item.vid == info.vid && item.pid == info.pid) {
                    alreadyExists = true;
                    break;
                }
            }
            if (!alreadyExists) {
                list.push_back(info);
            }
        }
    }

    return list;
}

void DeviceWatcher::ResetBaseline() {
    std::vector<KeyboardDeviceInfo> keyboards = GetConnectedKeyboards();
    std::vector<KeyboardDeviceInfo> externalKbs;
    for (const auto& kb : keyboards) {
        if (kb.isExternal) externalKbs.push_back(kb);
    }

    m_baselineKeyboards = externalKbs;
    m_baselineCount = externalKbs.size();
    m_baselineEstablished = true;

    if (!externalKbs.empty()) {
        m_autoLockedVid = externalKbs[0].vid;
        m_autoLockedPid = externalKbs[0].pid;
    } else {
        m_autoLockedVid.clear();
        m_autoLockedPid.clear();
    }

    Logger::Log(L"[Device] === Baseline Reset Established ===");
    Logger::Log(L"[Device] Baseline external count: %zu (Auto-locked Target: VID_%s PID_%s)",
        m_baselineCount, m_autoLockedVid.c_str(), m_autoLockedPid.c_str());

    for (size_t i = 0; i < externalKbs.size(); ++i) {
        Logger::Log(L"[Device]   Baseline [%zu]: VID_%s PID_%s (%s)",
            i, externalKbs[i].vid.c_str(), externalKbs[i].pid.c_str(), externalKbs[i].friendlyName.c_str());
    }

    EvaluateState(L"Baseline Reset");
}

void DeviceWatcher::UpdateStateInternal(bool isConnected, const std::wstring& reason, const std::wstring& name) {
    if (isConnected != m_isTargetConnected || name != m_currentTargetName) {
        Logger::Log(L"[Device] *** STATE TRANSITION *** (%s): %s -> %s (Target: %s)",
            reason.c_str(),
            m_isTargetConnected ? L"CONNECTED" : L"DISCONNECTED",
            isConnected ? L"CONNECTED" : L"DISCONNECTED",
            name.c_str());

        m_isTargetConnected = isConnected;
        m_currentTargetName = name;

        if (m_callback) {
            m_callback(m_isTargetConnected, m_currentTargetName);
        }
    }
}

void DeviceWatcher::OnDeviceChange(WPARAM wParam, LPARAM lParam) {
    const wchar_t* evtName = L"UNKNOWN";
    if (wParam == DBT_DEVICEARRIVAL) evtName = L"DBT_DEVICEARRIVAL";
    else if (wParam == DBT_DEVICEREMOVECOMPLETE) evtName = L"DBT_DEVICEREMOVECOMPLETE";
    else if (wParam == DBT_DEVNODES_CHANGED) evtName = L"DBT_DEVNODES_CHANGED";

    Logger::Log(L"[PnP] WM_DEVICECHANGE: %s (wParam: 0x%IX)", evtName, (UINT_PTR)wParam);

    // [핵심] 권위적(Authoritative) PnP 이벤트 즉결 처리
    if (lParam) {
        DEV_BROADCAST_HDR* hdr = (DEV_BROADCAST_HDR*)lParam;
        if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            DEV_BROADCAST_DEVICEINTERFACE_W* di = (DEV_BROADCAST_DEVICEINTERFACE_W*)lParam;
            KeyboardDeviceInfo evInfo = ParseDevicePath(di->dbcc_name);

            Logger::Log(L"[PnP]   Target Path: %s (Parsed VID_%s PID_%s, External=%s)",
                di->dbcc_name, evInfo.vid.c_str(), evInfo.pid.c_str(), evInfo.isExternal ? L"YES" : L"NO");

            std::wstring activeVid = (!m_targetVid.empty()) ? m_targetVid : m_autoLockedVid;
            std::wstring activePid = (!m_targetPid.empty()) ? m_targetPid : m_autoLockedPid;

            bool matchesTarget = (!activeVid.empty() && !activePid.empty() &&
                                  evInfo.vid == activeVid && evInfo.pid == activePid);

            // 타겟 장치가 명시적으로 분리되었을 때 -> 0ms 즉시 DISCONNECTED!
            if (wParam == DBT_DEVICEREMOVECOMPLETE) {
                if (matchesTarget || evInfo.isExternal) {
                    std::wstring desc = L"Keyboard Disconnected: [VID:" + evInfo.vid + L" PID:" + evInfo.pid + L"]";
                    UpdateStateInternal(false, L"Authoritative PnP Removed", desc);
                    return;
                }
            }
            // 타겟 장치가 명시적으로 다시 연결되었을 때 -> 0ms 즉시 CONNECTED!
            else if (wParam == DBT_DEVICEARRIVAL) {
                if (matchesTarget || evInfo.isExternal) {
                    if (m_autoLockedVid.empty() && !evInfo.vid.empty()) {
                        m_autoLockedVid = evInfo.vid;
                        m_autoLockedPid = evInfo.pid;
                    }
                    std::wstring desc = L"Keyboard Connected: [VID:" + evInfo.vid + L" PID:" + evInfo.pid + L"]";
                    UpdateStateInternal(true, L"Authoritative PnP Arrived", desc);
                    return;
                }
            }
        }
    }

    EvaluateState(evtName);
}

void DeviceWatcher::EvaluateState(const wchar_t* triggerReason) {
    std::wstring activeVid = (!m_targetVid.empty()) ? m_targetVid : m_autoLockedVid;
    std::wstring activePid = (!m_targetPid.empty()) ? m_targetPid : m_autoLockedPid;

    // 타겟 VID/PID가 결정되어 있는 경우 -> SetupAPI 커널 물리 존재 여부 1:1 확인
    if (!activeVid.empty() && !activePid.empty()) {
        bool physicallyPresent = IsDevicePhysicallyPresent(activeVid, activePid);
        std::wstring name = physicallyPresent
            ? (L"Keyboard (VID:" + activeVid + L" PID:" + activePid + L")")
            : (L"Target VID:" + activeVid + L" PID:" + activePid + L" [Disconnected]");

        UpdateStateInternal(physicallyPresent, triggerReason, name);
        return;
    }

    // 타겟이 아직 없는 경우 -> RawInput 및 SetupAPI 폴백
    std::vector<KeyboardDeviceInfo> keyboards = GetConnectedKeyboards();
    std::vector<KeyboardDeviceInfo> externalKbs;
    for (const auto& kb : keyboards) {
        if (kb.isExternal) externalKbs.push_back(kb);
    }

    if (!externalKbs.empty()) {
        m_autoLockedVid = externalKbs[0].vid;
        m_autoLockedPid = externalKbs[0].pid;
        UpdateStateInternal(true, triggerReason, externalKbs[0].friendlyName);
    } else {
        UpdateStateInternal(false, triggerReason, L"No External Keyboard Detected");
    }
}

bool DeviceWatcher::CheckConnectionState() {
    EvaluateState(L"Watchdog Check");
    return m_isTargetConnected;
}

void DeviceWatcher::SetStateCallback(std::function<void(bool, const std::wstring&)> callback) {
    m_callback = callback;
}

void DeviceWatcher::SetTarget(const std::wstring& vid, const std::wstring& pid, const std::wstring& name) {
    m_targetVid = vid;
    m_targetPid = pid;
    m_autoDetect = false;
    Logger::Log(L"[Device] SetTarget -> VID: %s, PID: %s (%s)", vid.c_str(), pid.c_str(), name.c_str());
    EvaluateState(L"Target Set");
}

void DeviceWatcher::SetAutoDetect(bool autoDetect) {
    m_autoDetect = autoDetect;
    if (m_autoDetect) {
        m_targetVid.clear();
        m_targetPid.clear();
        Logger::Log(L"[Device] SetAutoDetect -> Enabled");
        ResetBaseline();
    } else {
        EvaluateState(L"AutoDetect Toggled");
    }
}
