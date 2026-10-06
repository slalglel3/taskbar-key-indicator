#include "device_watcher.h"
#include <dbt.h>
#include <initguid.h>
#include <algorithm>

// GUID_DEVINTERFACE_HID: {4D1E55B2-F16F-11CF-88CB-001111000030}
DEFINE_GUID(GUID_DEVINTERFACE_HID_LOCAL, 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
// GUID_DEVINTERFACE_KEYBOARD: {884b96c3-56ef-11d1-bc8c-00a0c91405dd}
DEFINE_GUID(GUID_DEVINTERFACE_KEYBOARD_LOCAL, 0x884b96c3, 0x56ef, 0x11d1, 0xbc, 0x8c, 0x00, 0xa0, 0xc9, 0x14, 0x05, 0xdd);

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

    // PnP HID 및 키보드 장치 인터페이스 변경 알림 수신 등록
    DEV_BROADCAST_DEVICEINTERFACE_W filter = { 0 };
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID_LOCAL;

    m_hDevNotify = RegisterDeviceNotificationW(m_hWnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    // 초기 상태 평가
    EvaluateState();
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
    // ACPI(노트북 메인보드 내장 키보드)나 Root(가상 드라이버)가 아니고, VID/PID가 유효한 경우 외장 물리 키보드로 간주
    if (!info.vid.empty() && !info.pid.empty()) {
        if (upper.find(L"ACPI") == std::wstring::npos && upper.find(L"ROOT") == std::wstring::npos) {
            info.isExternal = true;
        }
    }

    if (!info.vid.empty() && !info.pid.empty()) {
        info.friendlyName = L"Keyboard (VID:" + info.vid + L" PID:" + info.pid + L")";
    } else {
        info.friendlyName = L"Standard Keyboard";
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
            if (info.isExternal) {
                // 중복 방지 (다중 인터페이스인 경우 동일 VID/PID)
                bool alreadyExists = false;
                for (const auto& item : list) {
                    if (item.vid == info.vid && item.pid == info.pid) {
                        alreadyExists = true;
                        break;
                    }
                }
                if (!alreadyExists) {
                    list.push_back(info);
                }
            }
        }
    }

    return list;
}

void DeviceWatcher::EvaluateState() {
    std::vector<KeyboardDeviceInfo> keyboards = GetConnectedKeyboards();
    bool newState = false;
    std::wstring matchedName = L"";

    if (!m_targetVid.empty() && !m_targetPid.empty()) {
        // 특정 VID / PID 매칭 모드
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
            matchedName = L"Target Keyboard (VID:" + m_targetVid + L" PID:" + m_targetPid + L") [Disconnected]";
        }
    } else {
        // 자동 감지 모드: 외장 USB 키보드가 1개 이상 존재하면 연결된 상태로 간주
        if (!keyboards.empty()) {
            newState = true;
            matchedName = keyboards[0].friendlyName;
            if (keyboards.size() > 1) {
                matchedName += L" (+" + std::to_wstring(keyboards.size() - 1) + L")";
            }
        } else {
            newState = false;
            matchedName = L"No External Keyboard Detected";
        }
    }

    m_isTargetConnected = newState;
    m_currentTargetName = matchedName;

    if (m_callback) {
        m_callback(m_isTargetConnected, m_currentTargetName);
    }
}

void DeviceWatcher::OnDeviceChange(WPARAM wParam, LPARAM lParam) {
    if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED) {
        // USB 장치 삽입/제거 시 0ms 즉시 상태 재평가!
        EvaluateState();
    }
}

bool DeviceWatcher::CheckConnectionState() {
    EvaluateState();
    return m_isTargetConnected;
}

void DeviceWatcher::SetStateCallback(std::function<void(bool, const std::wstring&)> callback) {
    m_callback = callback;
}

void DeviceWatcher::SetTarget(const std::wstring& vid, const std::wstring& pid) {
    m_targetVid = vid;
    m_targetPid = pid;
    EvaluateState();
}

void DeviceWatcher::SetAutoDetect(bool autoDetect) {
    m_autoDetect = autoDetect;
    if (m_autoDetect) {
        m_targetVid.clear();
        m_targetPid.clear();
    }
    EvaluateState();
}
