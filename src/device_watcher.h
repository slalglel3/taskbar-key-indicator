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

    // 감지된 외장 키보드 목록 반환
    std::vector<KeyboardDeviceInfo> GetConnectedKeyboards();

    // 콜백 등록
    void SetStateCallback(std::function<void(bool isConnected, const std::wstring& targetName)> callback);

    // 타겟 VID/PID 설정
    void SetTarget(const std::wstring& vid, const std::wstring& pid, const std::wstring& name = L"");
    void SetAutoDetect(bool autoDetect);

    bool IsTargetConnected() const { return m_isTargetConnected; }
    std::wstring GetCurrentTargetName() const { return m_currentTargetName; }
    std::wstring GetTargetVid() const { return m_targetVid; }
    std::wstring GetTargetPid() const { return m_targetPid; }
    bool IsAutoDetect() const { return m_autoDetect; }

private:
    HWND m_hWnd;
    HDEVNOTIFY m_hDevNotify;
    bool m_isTargetConnected;
    std::wstring m_targetVid;
    std::wstring m_targetPid;
    std::wstring m_currentTargetName;
    bool m_autoDetect;
    std::function<void(bool, const std::wstring&)> m_callback;

    // 직전 연결된 키보드 목록 (차이점 감지용)
    std::vector<KeyboardDeviceInfo> m_lastKnownKeyboards;

    static KeyboardDeviceInfo ParseDevicePath(const std::wstring& path);
    void EvaluateState(const wchar_t* triggerReason);
};
