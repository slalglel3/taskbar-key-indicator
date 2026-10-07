#include "device_watcher.h"
#include "logger.h"
#include <dbt.h>
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

    // 초기 상태 평가 및 베이스라인 수립
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

    Logger::Log(L"[Device] === Baseline Reset Established ===");
    Logger::Log(L"[Device] Baseline external keyboard count: %zu", m_baselineCount);
    for (size_t i = 0; i < externalKbs.size(); ++i) {
        Logger::Log(L"[Device]   Baseline [%zu]: VID_%s PID_%s (%s)",
            i, externalKbs[i].vid.c_str(), externalKbs[i].pid.c_str(), externalKbs[i].devicePath.c_str());
    }

    EvaluateState(L"Baseline Reset");
}

void DeviceWatcher::EvaluateState(const wchar_t* triggerReason) {
    std::vector<KeyboardDeviceInfo> keyboards = GetConnectedKeyboards();
    std::vector<KeyboardDeviceInfo> externalKbs;
    for (const auto& kb : keyboards) {
        if (kb.isExternal) externalKbs.push_back(kb);
    }

    bool newState = false;
    std::wstring matchedName = L"";

    // 1. 특정 VID / PID 지정 모드
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

        if (!newState) {
            matchedName = L"Target VID:" + m_targetVid + L" PID:" + m_targetPid + L" [Disconnected]";
        }
    }
    // 2. 스마트 베이스라인 자동 감지 모드
    else {
        if (!m_baselineEstablished) {
            m_baselineKeyboards = externalKbs;
            m_baselineCount = externalKbs.size();
            m_baselineEstablished = true;
        }

        // 새 장치가 꽂혀서 수량이 늘어난 경우 베이스라인을 상향 갱신
        if (externalKbs.size() > m_baselineCount) {
            Logger::Log(L"[Device] New keyboard plugged! Upgrading baseline from %zu to %zu",
                m_baselineCount, externalKbs.size());
            m_baselineCount = externalKbs.size();
            m_baselineKeyboards = externalKbs;
        }

        // [핵심] 베이스라인 수량과 비교:
        // PC에 마우스 동글(1개) + 멀티페어링 키보드(1개) = 총 2개였던 경우,
        // 키보드를 모바일로 넘겨서 1개로 줄어들면 즉시 DISCONNECTED 판별!
        if (m_baselineCount > 0 && externalKbs.size() < m_baselineCount) {
            newState = false;

            // 어떤 장치가 사라졌는지 식별
            std::wstring missingDesc = L"";
            for (const auto& bKb : m_baselineKeyboards) {
                bool stillPresent = false;
                for (const auto& cKb : externalKbs) {
                    if (cKb.vid == bKb.vid && cKb.pid == bKb.pid) {
                        stillPresent = true;
                        break;
                    }
                }
                if (!stillPresent) {
                    missingDesc += L"[VID:" + bKb.vid + L" PID:" + bKb.pid + L"] ";
                }
            }

            matchedName = L"Keyboard Disconnected: " + (missingDesc.empty() ? L"Wireless Active" : missingDesc);
        } else {
            // 베이스라인 수량 유지 또는 이상
            newState = !externalKbs.empty();
            if (!externalKbs.empty()) {
                matchedName = externalKbs[0].friendlyName;
                if (externalKbs.size() > 1) {
                    matchedName += L" (Total " + std::to_wstring(externalKbs.size()) + L")";
                }
            } else {
                matchedName = L"No External Keyboard Detected";
            }
        }
    }

    // 상태 또는 이름에 변동이 있을 때만 로깅 및 콜백 호출
    if (newState != m_isTargetConnected || matchedName != m_currentTargetName) {
        Logger::Log(L"[Device] State Changed (%s): %s -> %s (Target: %s)",
            triggerReason,
            m_isTargetConnected ? L"CONNECTED" : L"DISCONNECTED",
            newState ? L"CONNECTED" : L"DISCONNECTED",
            matchedName.c_str());

        m_isTargetConnected = newState;
        m_currentTargetName = matchedName;

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

    if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
        Logger::Log(L"[PnP] WM_DEVICECHANGE: %s", evtName);
        EvaluateState(evtName);
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
