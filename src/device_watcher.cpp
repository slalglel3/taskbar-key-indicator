#include "device_watcher.h"
#include "logger.h"
#include <dbt.h>
#include <initguid.h>
#include <algorithm>

// GUID_DEVINTERFACE_HID: {4D1E55B2-F16F-11CF-88CB-001111000030}
DEFINE_GUID(GUID_DEVINTERFACE_HID_LOCAL, 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
// GUID_DEVINTERFACE_USB_DEVICE: {A5DCBF10-6530-11D2-901F-00C04FB951ED}
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE_LOCAL, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);

#ifndef DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES 0x00000004
#endif

DeviceWatcher::DeviceWatcher()
    : m_hWnd(NULL)
    , m_hDevNotify(NULL)
    , m_isTargetConnected(true)
    , m_autoDetect(true)
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

    // 모든 인터페이스 클래스 변동을 포괄적으로 수신
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

    // 초기 상태 평가
    EvaluateState(L"Initial Scan");
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

    // VID 파싱
    size_t vidPos = upper.find(L"VID_");
    if (vidPos != std::wstring::npos && vidPos + 8 <= upper.length()) {
        info.vid = upper.substr(vidPos + 4, 4);
    }

    // PID 파싱
    size_t pidPos = upper.find(L"PID_");
    if (pidPos != std::wstring::npos && pidPos + 8 <= upper.length()) {
        info.pid = upper.substr(pidPos + 4, 4);
    }

    // 외장 USB/HID 판별:
    // ACPI(노트북 메인보드 내장 키보드)나 Root(가상 드라이버)가 아닌 경우 외장 장치로 간주
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

std::vector<KeyboardDeviceInfo> DeviceWatcher::GetConnectedKeyboards() {
    std::vector<KeyboardDeviceInfo> list;

    UINT numDevices = 0;
    if (GetRawInputDeviceList(NULL, &numDevices, sizeof(RAWINPUTDEVICELIST)) != 0 || numDevices == 0) {
        Logger::Log(L"[Device] GetRawInputDeviceList returned 0 devices or failed.");
        return list;
    }

    std::vector<RAWINPUTDEVICELIST> rawList(numDevices);
    if (GetRawInputDeviceList(rawList.data(), &numDevices, sizeof(RAWINPUTDEVICELIST)) == (UINT)-1) {
        Logger::Log(L"[Device] Failed to get RawInput device list.");
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

            // 중복 방지 (다중 인터페이스 동일 장치)
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

void DeviceWatcher::EvaluateState(const wchar_t* triggerReason) {
    Logger::Log(L"[Device] === Evaluating State (Trigger: %s) ===", triggerReason);

    std::vector<KeyboardDeviceInfo> keyboards = GetConnectedKeyboards();
    Logger::Log(L"[Device] Total keyboards found in system: %zu", keyboards.size());

    std::vector<KeyboardDeviceInfo> externalKbs;
    for (size_t i = 0; i < keyboards.size(); ++i) {
        const auto& kb = keyboards[i];
        Logger::Log(L"[Device]   [%zu] External=%s, VID=%s, PID=%s, Path=%s",
            i, kb.isExternal ? L"YES" : L"NO",
            kb.vid.empty() ? L"N/A" : kb.vid.c_str(),
            kb.pid.empty() ? L"N/A" : kb.pid.c_str(),
            kb.devicePath.c_str());

        if (kb.isExternal) {
            externalKbs.push_back(kb);
        }
    }

    Logger::Log(L"[Device] Total external keyboards: %zu", externalKbs.size());

    bool newState = false;
    std::wstring matchedName = L"";

    // 1. 특정 VID / PID 지정 타겟 모드
    if (!m_targetVid.empty() && !m_targetPid.empty()) {
        std::wstring uTargetVid = m_targetVid;
        std::wstring uTargetPid = m_targetPid;
        std::transform(uTargetVid.begin(), uTargetVid.end(), uTargetVid.begin(), ::towupper);
        std::transform(uTargetPid.begin(), uTargetPid.end(), uTargetPid.begin(), ::towupper);

        for (const auto& kb : keyboards) {
            if (kb.vid == uTargetVid && kb.pid == uTargetPid) {
                newState = true;
                matchedName = kb.friendlyName;
                break;
            }
        }

        if (newState) {
            Logger::Log(L"[Device] Target Matched & CONNECTED: VID_%s PID_%s", uTargetVid.c_str(), uTargetPid.c_str());
        } else {
            matchedName = L"Target VID:" + m_targetVid + L" PID:" + m_targetPid + L" [Disconnected]";
            Logger::Log(L"[Device] Target NOT Found (DISCONNECTED): VID_%s PID_%s", uTargetVid.c_str(), uTargetPid.c_str());
        }
    }
    // 2. 자동 감지 모드
    else {
        // 최초 실행이거나 직전 목록이 없을 때
        if (m_lastKnownKeyboards.empty() && !externalKbs.empty()) {
            m_lastKnownKeyboards = externalKbs;
            Logger::Log(L"[Device] Initial Baseline established with %zu external keyboard(s)", externalKbs.size());
        }

        if (!externalKbs.empty()) {
            newState = true;
            matchedName = externalKbs[0].friendlyName;
            if (externalKbs.size() > 1) {
                matchedName += L" (Total " + std::to_wstring(externalKbs.size()) + L")";
            }
        } else {
            newState = false;
            matchedName = L"No External Keyboard Detected";
        }

        Logger::Log(L"[Device] Auto-detect result: %s (External count: %zu)",
            newState ? L"CONNECTED" : L"DISCONNECTED", externalKbs.size());
    }

    m_isTargetConnected = newState;
    m_currentTargetName = matchedName;
    m_lastKnownKeyboards = externalKbs;

    if (m_callback) {
        m_callback(m_isTargetConnected, m_currentTargetName);
    }
}

void DeviceWatcher::OnDeviceChange(WPARAM wParam, LPARAM lParam) {
    const wchar_t* evtName = L"UNKNOWN";
    if (wParam == DBT_DEVICEARRIVAL) evtName = L"DBT_DEVICEARRIVAL (Device Plugged)";
    else if (wParam == DBT_DEVICEREMOVECOMPLETE) evtName = L"DBT_DEVICEREMOVECOMPLETE (Device Unplugged)";
    else if (wParam == DBT_DEVNODES_CHANGED) evtName = L"DBT_DEVNODES_CHANGED";

    Logger::Log(L"[PnP] WM_DEVICECHANGE received: %s (wParam: 0x%IX)", evtName, (UINT_PTR)wParam);

    if (lParam) {
        DEV_BROADCAST_HDR* hdr = (DEV_BROADCAST_HDR*)lParam;
        if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            DEV_BROADCAST_DEVICEINTERFACE_W* di = (DEV_BROADCAST_DEVICEINTERFACE_W*)lParam;
            Logger::Log(L"[PnP]   Device Interface Name: %s", di->dbcc_name);

            // 방금 연결/해제된 장치의 VID/PID 추출
            KeyboardDeviceInfo eventInfo = ParseDevicePath(di->dbcc_name);
            if (!eventInfo.vid.empty() && !eventInfo.pid.empty()) {
                Logger::Log(L"[PnP]   Parsed Event Device: VID_%s PID_%s", eventInfo.vid.c_str(), eventInfo.pid.c_str());
            }
        }
    }

    if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
        EvaluateState(evtName);
    }
}

bool DeviceWatcher::CheckConnectionState() {
    EvaluateState(L"Manual Check");
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
        Logger::Log(L"[Device] SetAutoDetect -> Enabled (Target cleared)");
    }
    EvaluateState(L"AutoDetect Toggled");
}
