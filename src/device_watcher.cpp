#include "device_watcher.h"
#include "logger.h"
#include <dbt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <algorithm>

DEFINE_GUID(GUID_DEVINTERFACE_HID_LOCAL, 0x4D1E55B2, 0xF16F, 0x11CF, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30);
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE_LOCAL, 0xA5DCBF10, 0x6530, 0x11D2, 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED);
DEFINE_GUID(GUID_DEVINTERFACE_KEYBOARD_LOCAL, 0x884B96C3, 0x56EF, 0x11D1, 0xBC, 0x8C, 0x00, 0xA0, 0xC9, 0x14, 0x05, 0xDD);
DEFINE_GUID(GUID_DEVINTERFACE_USB_HUB_LOCAL, 0xF18A0E88, 0xC30C, 0x11D0, 0x88, 0x15, 0x00, 0xA0, 0xC9, 0x06, 0xBE, 0xD8);

#ifndef DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES 0x00000004
#endif

DeviceWatcher::DeviceWatcher()
    : m_hWnd(NULL)
    , m_isTargetConnected(true)
    , m_autoDetect(true)
    , m_baselineCount(0)
    , m_baselineEstablished(false)
    , m_hasLastProbe(false)
{
    memset(&m_lastProbe, 0, sizeof(m_lastProbe));
}

DeviceWatcher::~DeviceWatcher() {
    Cleanup();
}

bool DeviceWatcher::Initialize(HWND hWnd) {
    m_hWnd = hWnd;

    // 4대 주요 PnP 인터페이스 클래스 전체 등록
    const GUID* guids[] = {
        &GUID_DEVINTERFACE_HID_LOCAL,
        &GUID_DEVINTERFACE_USB_DEVICE_LOCAL,
        &GUID_DEVINTERFACE_KEYBOARD_LOCAL,
        &GUID_DEVINTERFACE_USB_HUB_LOCAL
    };

    for (const auto* guid : guids) {
        DEV_BROADCAST_DEVICEINTERFACE_W filter = { 0 };
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = *guid;

        HDEVNOTIFY hNotify = RegisterDeviceNotificationW(
            m_hWnd,
            &filter,
            DEVICE_NOTIFY_WINDOW_HANDLE | DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
        );

        if (hNotify) {
            m_devNotifyHandles.push_back(hNotify);
        }
    }

    Logger::Log(L"[Device] Registered %zu Device Notification filter(s)", m_devNotifyHandles.size());

    ResetBaseline();
    return true;
}

void DeviceWatcher::Cleanup() {
    for (HDEVNOTIFY h : m_devNotifyHandles) {
        if (h) {
            UnregisterDeviceNotification(h);
        }
    }
    m_devNotifyHandles.clear();
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

DeviceProbeResult DeviceWatcher::ProbeTargetDevice(bool logDetailed) {
    DeviceProbeResult res = { 0 };
    res.rawInputPresent = false;
    res.setupApiPresent = false;
    res.canOpenFile = false;
    res.openFileError = 0;
    res.devNodeFound = false;
    res.devNodeStatus = 0;
    res.devNodeProblem = 0;

    std::wstring targetVid = GetTargetVid();
    std::wstring targetPid = GetTargetPid();

    if (targetVid.empty() || targetPid.empty()) {
        if (logDetailed) {
            Logger::Log(L"[Probe] Target VID/PID is empty. Cannot probe.");
        }
        return res;
    }

    std::wstring uTargetVid = targetVid;
    std::wstring uTargetPid = targetPid;
    std::transform(uTargetVid.begin(), uTargetVid.end(), uTargetVid.begin(), ::towupper);
    std::transform(uTargetPid.begin(), uTargetPid.end(), uTargetPid.begin(), ::towupper);

    // 1. Raw Input 센서 검사
    std::vector<KeyboardDeviceInfo> rawKeyboards = GetConnectedKeyboards();
    for (const auto& kb : rawKeyboards) {
        if (kb.vid == uTargetVid && kb.pid == uTargetPid) {
            res.rawInputPresent = true;
            if (res.devicePath.empty()) res.devicePath = kb.devicePath;
            break;
        }
    }

    // 2. SetupAPI & CfgMgr 센서 검사
    HDEVINFO hDevInfo = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_HID_LOCAL,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (hDevInfo != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA ifData = { sizeof(SP_DEVICE_INTERFACE_DATA) };

        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, NULL, &GUID_DEVINTERFACE_HID_LOCAL, i, &ifData); ++i) {
            DWORD reqSize = 0;
            SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, NULL, 0, &reqSize, NULL);
            if (reqSize > 0) {
                std::vector<BYTE> buf(reqSize);
                PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                SP_DEVINFO_DATA devInfoData = { sizeof(SP_DEVINFO_DATA) };

                if (SetupDiGetDeviceInterfaceDetailW(hDevInfo, &ifData, detail, reqSize, NULL, &devInfoData)) {
                    std::wstring path = detail->DevicePath;
                    std::wstring upper = path;
                    std::transform(upper.begin(), upper.end(), upper.begin(), ::towupper);

                    if (upper.find(L"VID_" + uTargetVid) != std::wstring::npos &&
                        upper.find(L"PID_" + uTargetPid) != std::wstring::npos) {
                        res.setupApiPresent = true;
                        res.devicePath = path;

                        // DevNode 하드웨어 상태 질의
                        res.devNodeFound = true;
                        ULONG status = 0, problem = 0;
                        if (CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0) == CR_SUCCESS) {
                            res.devNodeStatus = status;
                            res.devNodeProblem = problem;
                        }

                        // 디바이스 인스턴스 ID 질의
                        wchar_t instId[MAX_DEVICE_ID_LEN] = { 0 };
                        if (CM_Get_Device_IDW(devInfoData.DevInst, instId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
                            res.devInstanceId = instId;
                        }

                        break; // 대상 장치 발견
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList(hDevInfo);
    }

    // 3. 파일 핸들 I/O 센서 검사 (DevicePath를 직접 열 수 있는지)
    if (!res.devicePath.empty()) {
        HANDLE hFile = CreateFileW(
            res.devicePath.c_str(),
            0, // 쿼리 전용 (관리자 권한 불필요)
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hFile != INVALID_HANDLE_VALUE) {
            res.canOpenFile = true;
            res.openFileError = 0;
            CloseHandle(hFile);
        } else {
            res.canOpenFile = false;
            res.openFileError = GetLastError();
        }
    }

    if (logDetailed) {
        Logger::Log(L"============================================================");
        Logger::Log(L"[Diagnostic Probe] Target: VID_%s PID_%s", uTargetVid.c_str(), uTargetPid.c_str());
        Logger::Log(L"  - Sensor 1 (RawInput Present) : %s", res.rawInputPresent ? L"YES" : L"NO");
        Logger::Log(L"  - Sensor 2 (SetupAPI Present) : %s", res.setupApiPresent ? L"YES" : L"NO");
        Logger::Log(L"  - Sensor 3 (Device Node Found): %s (Status: 0x%08X, Problem: %lu)",
            res.devNodeFound ? L"YES" : L"NO", res.devNodeStatus, res.devNodeProblem);
        Logger::Log(L"  - Sensor 4 (Can Open Handle)  : %s (Error Code: %lu)",
            res.canOpenFile ? L"YES" : L"NO", res.openFileError);
        Logger::Log(L"  - Device Instance ID         : %s",
            res.devInstanceId.empty() ? L"(None)" : res.devInstanceId.c_str());
        Logger::Log(L"  - Active Device Path          : %s",
            res.devicePath.empty() ? L"(None)" : res.devicePath.c_str());
        Logger::Log(L"============================================================");
    }

    return res;
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
    Logger::Log(L"[Device] Baseline external count: %zu (Target: VID_%s PID_%s)",
        m_baselineCount, m_autoLockedVid.c_str(), m_autoLockedPid.c_str());

    for (size_t i = 0; i < externalKbs.size(); ++i) {
        Logger::Log(L"[Device]   Baseline [%zu]: VID_%s PID_%s (%s)",
            i, externalKbs[i].vid.c_str(), externalKbs[i].pid.c_str(), externalKbs[i].friendlyName.c_str());
    }

    // 초기 상태 정밀 프로브 수행
    ProbeTargetDevice(true);
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

    Logger::Log(L"[PnP Event] WM_DEVICECHANGE: %s (wParam: 0x%IX)", evtName, (UINT_PTR)wParam);

    if (lParam) {
        DEV_BROADCAST_HDR* hdr = (DEV_BROADCAST_HDR*)lParam;
        if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
            DEV_BROADCAST_DEVICEINTERFACE_W* di = (DEV_BROADCAST_DEVICEINTERFACE_W*)lParam;
            KeyboardDeviceInfo evInfo = ParseDevicePath(di->dbcc_name);

            Logger::Log(L"[PnP Event]   Device: %s (VID_%s PID_%s, External=%s)",
                di->dbcc_name, evInfo.vid.c_str(), evInfo.pid.c_str(), evInfo.isExternal ? L"YES" : L"NO");

            std::wstring targetVid = GetTargetVid();
            std::wstring targetPid = GetTargetPid();

            bool matchesTarget = (!targetVid.empty() && !targetPid.empty() &&
                                  evInfo.vid == targetVid && evInfo.pid == targetPid);

            if (wParam == DBT_DEVICEREMOVECOMPLETE) {
                if (matchesTarget || evInfo.isExternal) {
                    std::wstring desc = L"Keyboard Disconnected: [VID:" + evInfo.vid + L" PID:" + evInfo.pid + L"]";
                    UpdateStateInternal(false, L"Authoritative PnP Removed", desc);
                    ProbeTargetDevice(true);
                    return;
                }
            } else if (wParam == DBT_DEVICEARRIVAL) {
                if (matchesTarget || evInfo.isExternal) {
                    std::wstring desc = L"Keyboard Connected: [VID:" + evInfo.vid + L" PID:" + evInfo.pid + L"]";
                    UpdateStateInternal(true, L"Authoritative PnP Arrived", desc);
                    ProbeTargetDevice(true);
                    return;
                }
            }
        }
    }

    EvaluateState(evtName);
}

void DeviceWatcher::EvaluateState(const wchar_t* triggerReason) {
    std::wstring targetVid = GetTargetVid();
    std::wstring targetPid = GetTargetPid();

    if (targetVid.empty() || targetPid.empty()) {
        std::vector<KeyboardDeviceInfo> kbs = GetConnectedKeyboards();
        if (!kbs.empty()) {
            m_autoLockedVid = kbs[0].vid;
            m_autoLockedPid = kbs[0].pid;
            targetVid = m_autoLockedVid;
            targetPid = m_autoLockedPid;
        }
    }

    // 5대 센서 프로브 실행
    DeviceProbeResult probe = ProbeTargetDevice(false);

    // 센서 상태 변화 감지 로깅
    if (!m_hasLastProbe ||
        probe.rawInputPresent != m_lastProbe.rawInputPresent ||
        probe.setupApiPresent != m_lastProbe.setupApiPresent ||
        probe.canOpenFile != m_lastProbe.canOpenFile ||
        probe.openFileError != m_lastProbe.openFileError ||
        probe.devNodeProblem != m_lastProbe.devNodeProblem) {

        Logger::Log(L"[Sensor Diff] Trigger: %s | RawInput: %d->%d | SetupAPI: %d->%d | CanOpen: %d->%d (Err: %lu->%lu) | Problem: %lu->%lu",
            triggerReason,
            m_lastProbe.rawInputPresent ? 1 : 0, probe.rawInputPresent ? 1 : 0,
            m_lastProbe.setupApiPresent ? 1 : 0, probe.setupApiPresent ? 1 : 0,
            m_lastProbe.canOpenFile ? 1 : 0, probe.canOpenFile ? 1 : 0,
            m_lastProbe.openFileError, probe.openFileError,
            m_lastProbe.devNodeProblem, probe.devNodeProblem);

        m_lastProbe = probe;
        m_hasLastProbe = true;
    }

    // 다중 센서 종합 판별식:
    // SetupAPI에 물리적으로 존재하고, 파일 핸들이 유효하거나 에러가 치명적이지 않은 경우 -> CONNECTED
    bool isConnected = false;
    if (probe.setupApiPresent && probe.devNodeProblem == 0) {
        // 장치가 존재하고 하드웨어 오류가 없음
        // 만약 파일 핸들 열기 실패 코드가 ERROR_DEVICE_NOT_CONNECTED(1167)라면 연결 끊김으로 처리
        if (!probe.canOpenFile && probe.openFileError == ERROR_DEVICE_NOT_CONNECTED) {
            isConnected = false;
        } else {
            isConnected = true;
        }
    } else {
        isConnected = false;
    }

    std::wstring name = isConnected
        ? (L"Keyboard (VID:" + targetVid + L" PID:" + targetPid + L")")
        : (L"Target VID:" + targetVid + L" PID:" + targetPid + L" [Disconnected]");

    UpdateStateInternal(isConnected, triggerReason, name);
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
    ProbeTargetDevice(true);
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
