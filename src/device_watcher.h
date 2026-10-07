#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <functional>

struct KeyboardDeviceInfo {
    std::wstring devicePath;
    std::wstring vid;
    std::wstring pid;
    std::wstring friendlyName;
    bool isExternal;
};

struct HidInterfaceProbeInfo {
    std::wstring path;
    USHORT usagePage = 0;
    USHORT usage = 0;
    USHORT inputReportLen = 0;
    USHORT outputReportLen = 0;
    USHORT featureReportLen = 0;

    bool canOpen = false;
    DWORD openError = 0;

    bool attrSuccess = false;
    USHORT vid = 0;
    USHORT pid = 0;
    USHORT version = 0;

    std::wstring manufacturer;
    std::wstring product;

    bool getFeatureSuccess = false;
    DWORD getFeatureError = 0;

    bool getInputReportSuccess = false;
    DWORD getInputReportError = 0;

    bool setOutputReportSuccess = false;
    DWORD setOutputReportError = 0;
};

// 5대 센서 + HID 심층 패킷 진단 결과 구조체
struct DeviceProbeResult {
    bool rawInputPresent = false;       // 센서 1: RawInput 목록에 존재하는가
    bool setupApiPresent = false;       // 센서 2: SetupAPI DIGCF_PRESENT에 존재하는가
    bool canOpenFile = false;           // 센서 3: CreateFile로 핸들을 열 수 있는가
    DWORD openFileError = 0;            // 센서 3 에러 코드
    bool devNodeFound = false;          // 센서 4: DevNode 찾음 여부
    ULONG devNodeStatus = 0;            // 센서 4: CM_Get_DevNode_Status 상태 플래그
    ULONG devNodeProblem = 0;           // 센서 4: CM_Get_DevNode_Status 문제 코드
    std::wstring devInstanceId;         // 디바이스 인스턴스 ID
    std::wstring devicePath;            // 주 디바이스 경로

    std::vector<HidInterfaceProbeInfo> hidInterfaces; // 센서 5: 각 HID 인터페이스별 심층 프로브
};

struct DetailedDeviceInfo {
    std::wstring friendlyName;
    std::wstring description;
    std::wstring hardwareId;
    std::wstring vid;
    std::wstring pid;
    std::wstring devicePath;
    std::wstring instanceId;
    ULONG status = 0;
    ULONG problem = 0;
    bool isForceLinkCandidate = false;
};

class DeviceWatcher {
public:
    DeviceWatcher();
    ~DeviceWatcher();

    bool Initialize(HWND hWnd);
    void Cleanup();

    // WM_DEVICECHANGE 메시지 처리
    void OnDeviceChange(WPARAM wParam, LPARAM lParam);

    // 현재 연결된 키보드 상태 즉시 확인 및 평가
    bool CheckConnectionState();

    // 5대 센서 전방위 정밀 진단 실행 및 상세 로깅
    DeviceProbeResult ProbeTargetDevice(bool logDetailed = true);

    // 시스템의 모든 입력 장치 및 ForceLink 동글 전수 상세 검색 및 인벤토리 덤프
    std::vector<DetailedDeviceInfo> ScanAllInputDevices(bool logDetailed = true);

    // 감지된 외장 키보드 목록 반환
    std::vector<KeyboardDeviceInfo> GetConnectedKeyboards();

    // 콜백 등록
    void SetStateCallback(std::function<void(bool isConnected, const std::wstring& targetName)> callback);

    // 타겟 VID/PID 설정
    void SetTarget(const std::wstring& vid, const std::wstring& pid, const std::wstring& name = L"");
    void SetAutoDetect(bool autoDetect);
    void ResetBaseline();

    bool IsTargetConnected() const { return m_isTargetConnected; }
    std::wstring GetCurrentTargetName() const { return m_currentTargetName; }
    std::wstring GetTargetVid() const { return m_targetVid.empty() ? m_autoLockedVid : m_targetVid; }
    std::wstring GetTargetPid() const { return m_targetPid.empty() ? m_autoLockedPid : m_targetPid; }
    bool IsAutoDetect() const { return m_autoDetect; }

private:
    HWND m_hWnd;
    std::vector<HDEVNOTIFY> m_devNotifyHandles;
    bool m_isTargetConnected;
    std::wstring m_targetVid;
    std::wstring m_targetPid;
    std::wstring m_currentTargetName;
    bool m_autoDetect;
    std::function<void(bool, const std::wstring&)> m_callback;

    // 베이스라인 키보드 목록 및 수량 추적
    std::vector<KeyboardDeviceInfo> m_baselineKeyboards;
    size_t m_baselineCount;
    bool m_baselineEstablished;

    // 자동 감지 시 잠금된 타겟 VID/PID
    std::wstring m_autoLockedVid;
    std::wstring m_autoLockedPid;

    // 직전 프로브 결과 캐시 (변화 감지용)
    DeviceProbeResult m_lastProbe;
    bool m_hasLastProbe;

    static KeyboardDeviceInfo ParseDevicePath(const std::wstring& path);
    void EvaluateState(const wchar_t* triggerReason);
    void UpdateStateInternal(bool isConnected, const std::wstring& reason, const std::wstring& name);
};
