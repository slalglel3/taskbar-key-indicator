#pragma once

#include <windows.h>
#include <string>
#include <functional>

class BrowserWatcher {
public:
    BrowserWatcher();
    ~BrowserWatcher();

    bool Initialize(HWND hMainWnd);
    void Cleanup();

    // 현재 열려 있는 브라우저 창 목록에서 초기 상태 즉시 검색
    bool CheckCurrentState();

    void SetStateCallback(std::function<void(bool isConnected, const std::wstring& title)> callback);

    bool IsConnected() const { return m_isConnected; }
    std::wstring GetMatchedTitle() const { return m_matchedTitle; }

private:
    HWND m_hMainWnd;
    HWINEVENTHOOK m_hHook;
    bool m_isConnected;
    bool m_hasState;
    std::wstring m_matchedTitle;
    std::function<void(bool, const std::wstring&)> m_callback;

    static BrowserWatcher* s_instance;

    static void CALLBACK WinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime
    );

    static BOOL CALLBACK EnumWindowsInitProc(HWND hwnd, LPARAM lParam);

    void HandleTitleChange(HWND hwnd);
};
