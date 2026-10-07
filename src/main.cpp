#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#include "logger.h"
#include "config.h"
#include "taskbar_overlay.h"
#include "device_watcher.h"

static const wchar_t* MAIN_WINDOW_CLASS = L"TaskbarKeyIndicator_MsgWnd";
static const wchar_t* MUTEX_NAME = L"TaskbarKeyIndicator_SingleInstance_Mutex";
static const UINT WM_TRAYICON_MSG = WM_APP + 101;
static const UINT_PTR TIMER_WATCHDOG = 3001;

// 메뉴 ID 상수
enum MenuIDs {
    IDM_STATUS_HEADER = 1001,
    IDM_TARGET_NAME,
    IDM_SEPARATOR_1,
    IDM_TEST_OVERLAY,
    IDM_OPEN_LOG,
    IDM_RESET_BASELINE,
    IDM_SEPARATOR_2,
    IDM_AUTO_DETECT_KB,
    IDM_KB_LIST_BASE = 2000, // 감지된 개별 키보드 선택 메뉴 (동적 2000 ~ 2099)
    IDM_SEPARATOR_3 = 2100,
    IDM_COLOR_RED,
    IDM_COLOR_ORANGE,
    IDM_COLOR_AMBER,
    IDM_COLOR_PINK,
    IDM_COLOR_BLUE,
    IDM_SEPARATOR_4,
    IDM_THICKNESS_2,
    IDM_THICKNESS_3,
    IDM_THICKNESS_4,
    IDM_THICKNESS_5,
    IDM_SEPARATOR_5,
    IDM_SHOW_WHEN_CONNECTED,
    IDM_AUTO_START,
    IDM_OPEN_CONFIG,
    IDM_SEPARATOR_6,
    IDM_EXIT
};

class Application {
public:
    Application();
    ~Application();

    int Run(HINSTANCE hInstance);

private:
    HINSTANCE m_hInstance;
    HWND m_hWnd;
    HANDLE m_hMutex;
    UINT m_wmTaskbarCreated;
    NOTIFYICONDATAW m_nid;
    HICON m_hCurrentTrayIcon;

    AppConfig m_config;
    TaskbarOverlayManager m_overlayMgr;
    DeviceWatcher m_deviceWatcher;
    std::vector<KeyboardDeviceInfo> m_cachedKeyboards;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool Initialize(HINSTANCE hInstance);
    void UpdateState(bool isConnected, const std::wstring& devName);
    void SetupTrayIcon();
    void UpdateTrayIcon(bool isConnected);
    void ShowContextMenu();
    HICON CreateLedIcon(COLORREF color);
};

static Application* g_app = nullptr;

Application::Application()
    : m_hInstance(NULL)
    , m_hWnd(NULL)
    , m_hMutex(NULL)
    , m_wmTaskbarCreated(0)
    , m_hCurrentTrayIcon(NULL)
{
    memset(&m_nid, 0, sizeof(m_nid));
    g_app = this;
}
Application::~Application() {
    Logger::Log(L"[App] Shutting down application...");
    if (m_hCurrentTrayIcon) {
        DestroyIcon(m_hCurrentTrayIcon);
        m_hCurrentTrayIcon = NULL;
    }
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    if (m_hWnd) {
        KillTimer(m_hWnd, TIMER_WATCHDOG);
    }
    if (m_hMutex) {
        CloseHandle(m_hMutex);
        m_hMutex = NULL;
    }
    Logger::Close();
}


HICON Application::CreateLedIcon(COLORREF color) {
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, 16, 16);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    HBITMAP hMask = CreateBitmap(16, 16, 1, 1, NULL);
    HDC hdcMask = CreateCompatibleDC(hdcScreen);
    HBITMAP hOldMask = (HBITMAP)SelectObject(hdcMask, hMask);

    RECT rc = { 0, 0, 16, 16 };
    HBRUSH hBrBlackStock = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HBRUSH hBrWhiteStock = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(hdcMem, &rc, hBrBlackStock);
    FillRect(hdcMask, &rc, hBrWhiteStock);

    HBRUSH hBrColor = CreateSolidBrush(color);
    HBRUSH hOldBrMem = (HBRUSH)SelectObject(hdcMem, hBrColor);
    HBRUSH hOldBrMask = (HBRUSH)SelectObject(hdcMask, hBrBlackStock);

    Ellipse(hdcMem, 2, 2, 14, 14);
    Ellipse(hdcMask, 2, 2, 14, 14);

    // 하이라이트 코어
    HBRUSH hBrWhite = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(hdcMem, hBrWhite);
    Ellipse(hdcMem, 4, 4, 8, 8);

    // [중요] 원래 브러시 및 비트맵으로 완벽 복원 후 DC 삭제 (GDI 누수 원천 차단)
    SelectObject(hdcMem, hOldBrMem);
    SelectObject(hdcMask, hOldBrMask);
    SelectObject(hdcMem, hOldBmp);
    SelectObject(hdcMask, hOldMask);

    DeleteDC(hdcMem);
    DeleteDC(hdcMask);
    ReleaseDC(NULL, hdcScreen);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmColor = hBmp;
    ii.hbmMask = hMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hBmp);
    DeleteObject(hMask);
    DeleteObject(hBrColor);
    DeleteObject(hBrWhite);

    return hIcon;
}

void Application::SetupTrayIcon() {
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON_MSG;

    m_hCurrentTrayIcon = CreateLedIcon(RGB(50, 215, 75));
    m_nid.hIcon = m_hCurrentTrayIcon;
    wcscpy_s(m_nid.szTip, L"Taskbar Key Indicator");

    if (Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        Logger::Log(L"[Tray] Tray icon added successfully.");
    } else {
        Logger::Log(L"[Tray] Failed to add tray icon! Error: %lu", GetLastError());
    }
}

void Application::UpdateTrayIcon(bool isConnected) {
    COLORREF icoColor = isConnected ? m_config.connectedColor : m_config.disconnectedColor;

    if (m_hCurrentTrayIcon) {
        DestroyIcon(m_hCurrentTrayIcon);
    }
    m_hCurrentTrayIcon = CreateLedIcon(icoColor);
    m_nid.hIcon = m_hCurrentTrayIcon;

    std::wstring tip = L"Key Indicator: ";
    if (isConnected) {
        tip += L"Wired Connected [PC Active]";
    } else {
        tip += L"Wireless Disconnected [Mobile Active]";
    }
    wcsncpy_s(m_nid.szTip, tip.c_str(), 127);

    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void Application::UpdateState(bool isConnected, const std::wstring& devName) {
    Logger::Log(L"[App] UpdateState -> Connected: %s, Device: %s",
        isConnected ? L"YES" : L"NO", devName.c_str());

    m_overlayMgr.SetState(
        isConnected,
        m_config.disconnectedColor,
        m_config.connectedColor,
        m_config.showWhenConnected,
        m_config.barThickness
    );

    UpdateTrayIcon(isConnected);
}

void Application::ShowContextMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    bool isConnected = m_deviceWatcher.IsTargetConnected();
    std::wstring targetName = m_deviceWatcher.GetCurrentTargetName();

    // 1. 상태 헤더
    std::wstring statusStr = isConnected ? L"● 상태: 유선 연결됨 (PC 활성)" : L"○ 상태: 무선 전환됨 (유선 분리)";
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_STATUS_HEADER, statusStr.c_str());

    std::wstring kbDesc = L"  타겟: " + targetName;
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_TARGET_NAME, kbDesc.c_str());

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_1, NULL);

    // 2. 진단 및 테스트 기능 (핵심!)
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TEST_OVERLAY, L"⚡ LED 바 강제 테스트 (5초간 점등)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_LOG, L"📋 실시간 진단 로그 열기 (debug.log)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_RESET_BASELINE, L"🔄 현재 연결 상태를 기준(Baseline)으로 재설정");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_2, NULL);

    // 3. 키보드 타겟 선택 서브메뉴
    m_cachedKeyboards = m_deviceWatcher.GetConnectedKeyboards();
    HMENU hKbMenu = CreatePopupMenu();
    InsertMenuW(hKbMenu, -1, MF_BYPOSITION | MF_STRING | (m_deviceWatcher.IsAutoDetect() ? MF_CHECKED : 0),
        IDM_AUTO_DETECT_KB, L"자동 감지 모드 (Auto-Detect)");

    if (!m_cachedKeyboards.empty()) {
        InsertMenuW(hKbMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
        for (size_t i = 0; i < m_cachedKeyboards.size() && i < 10; ++i) {
            const auto& kb = m_cachedKeyboards[i];
            bool isCurrentTarget = (!m_deviceWatcher.IsAutoDetect() &&
                kb.vid == m_deviceWatcher.GetTargetVid() &&
                kb.pid == m_deviceWatcher.GetTargetPid());

            std::wstring itemText = L"[" + kb.vid + L":" + kb.pid + L"] " + kb.friendlyName;
            InsertMenuW(hKbMenu, -1, MF_BYPOSITION | MF_STRING | (isCurrentTarget ? MF_CHECKED : 0),
                IDM_KB_LIST_BASE + (UINT)i, itemText.c_str());
        }
    }
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hKbMenu, L"🎯 감시할 키보드 선택");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_3, NULL);

    // 4. LED 색상 서브메뉴
    HMENU hColorMenu = CreatePopupMenu();
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 45, 85) ? MF_CHECKED : 0), IDM_COLOR_RED, L"네온 레드 (#FF2D55) [기본]");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 149, 0) ? MF_CHECKED : 0), IDM_COLOR_ORANGE, L"네온 오렌지 (#FF9500)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 204, 0) ? MF_CHECKED : 0), IDM_COLOR_AMBER, L"네온 앰버 (#FFCC00)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 55, 95) ? MF_CHECKED : 0), IDM_COLOR_PINK, L"네온 핑크 (#FF375F)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(0, 122, 255) ? MF_CHECKED : 0), IDM_COLOR_BLUE, L"네온 블루 (#007AFF)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hColorMenu, L"LED 바 색상 설정");

    // 5. LED 바 두께 서브메뉴
    HMENU hThickMenu = CreatePopupMenu();
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 2 ? MF_CHECKED : 0), IDM_THICKNESS_2, L"2 픽셀");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 3 ? MF_CHECKED : 0), IDM_THICKNESS_3, L"3 픽셀 [권장]");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 4 ? MF_CHECKED : 0), IDM_THICKNESS_4, L"4 픽셀");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 5 ? MF_CHECKED : 0), IDM_THICKNESS_5, L"5 픽셀");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hThickMenu, L"LED 바 두께 설정");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_4, NULL);

    // 6. 옵션
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.showWhenConnected ? MF_CHECKED : 0), IDM_SHOW_WHEN_CONNECTED, L"유선 연결 시에도 초록 LED 표시");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.autoStart ? MF_CHECKED : 0), IDM_AUTO_START, L"윈도우 시작 시 자동 실행");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_5, NULL);

    // 7. 설정 파일 열기 & 종료
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_CONFIG, L"설정 파일 열기 (config.ini)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"종료 (Exit)");

    // 팝업 메뉴 트랙
    SetForegroundWindow(m_hWnd);
    int cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, m_hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    if (cmd >= IDM_KB_LIST_BASE && cmd < IDM_KB_LIST_BASE + (int)m_cachedKeyboards.size()) {
        size_t idx = cmd - IDM_KB_LIST_BASE;
        const auto& kb = m_cachedKeyboards[idx];
        m_config.targetVid = kb.vid;
        m_config.targetPid = kb.pid;
        m_config.targetDeviceName = kb.friendlyName;
        m_config.autoDetect = false;
        m_deviceWatcher.SetTarget(kb.vid, kb.pid, kb.friendlyName);
        ConfigManager::SaveConfig(m_config);
        Logger::Log(L"[Tray] User locked target keyboard to VID_%s PID_%s", kb.vid.c_str(), kb.pid.c_str());
        return;
    }

    switch (cmd) {
    case IDM_TEST_OVERLAY:
        m_overlayMgr.ForceShowTest(5000);
        break;

    case IDM_OPEN_LOG:
        Logger::OpenLogFile();
        break;

    case IDM_RESET_BASELINE:
        m_deviceWatcher.ResetBaseline();
        break;

    case IDM_AUTO_DETECT_KB:
        m_config.autoDetect = true;
        m_config.targetVid.clear();
        m_config.targetPid.clear();
        m_config.targetDeviceName.clear();
        m_deviceWatcher.SetAutoDetect(true);
        ConfigManager::SaveConfig(m_config);
        break;

    case IDM_COLOR_RED:
        m_config.disconnectedColor = RGB(255, 45, 85);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());
        break;
    case IDM_COLOR_ORANGE:
        m_config.disconnectedColor = RGB(255, 149, 0);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());
        break;
    case IDM_COLOR_AMBER:
        m_config.disconnectedColor = RGB(255, 204, 0);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());
        break;
    case IDM_COLOR_PINK:
        m_config.disconnectedColor = RGB(255, 55, 95);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());
        break;
    case IDM_COLOR_BLUE:
        m_config.disconnectedColor = RGB(0, 122, 255);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());
        break;

    case IDM_THICKNESS_2:
    case IDM_THICKNESS_3:
    case IDM_THICKNESS_4:
    case IDM_THICKNESS_5:
        m_config.barThickness = (cmd - IDM_THICKNESS_2) + 2;
        ConfigManager::SaveConfig(m_config);
        m_overlayMgr.SetThickness(m_config.barThickness);
        break;

    case IDM_SHOW_WHEN_CONNECTED:
        m_config.showWhenConnected = !m_config.showWhenConnected;
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());
        break;

    case IDM_AUTO_START:
        m_config.autoStart = !m_config.autoStart;
        ConfigManager::SetAutoStart(m_config.autoStart);
        ConfigManager::SaveConfig(m_config);
        break;

    case IDM_OPEN_CONFIG:
        ShellExecuteW(NULL, L"open", ConfigManager::GetConfigPath().c_str(), NULL, NULL, SW_SHOWNORMAL);
        break;

    case IDM_EXIT:
        PostQuitMessage(0);
        break;
    }
}

LRESULT CALLBACK Application::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_app) return DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == g_app->m_wmTaskbarCreated) {
        Logger::Log(L"[WndProc] TaskbarCreated message received from Shell. Recreating tray and overlays.");
        g_app->SetupTrayIcon();
        g_app->m_overlayMgr.UpdatePositions();
        return 0;
    }

    switch (msg) {
    case WM_QUERYENDSESSION:
        Logger::Log(L"[WndProc] WM_QUERYENDSESSION received. Allowing clean OS shutdown.");
        return TRUE; // 시스템 종료 동의

    case WM_ENDSESSION:
        if (wParam == TRUE) {
            Logger::Log(L"[WndProc] WM_ENDSESSION received (EndSession=TRUE). Initiating clean exit.");
            PostQuitMessage(0); // 셧다운 차단 방지 및 클린 종료
        }
        return 0;

    case WM_DEVICECHANGE:
        Logger::Log(L"[WndProc] WM_DEVICECHANGE intercepted by Top-level Window.");
        g_app->m_deviceWatcher.OnDeviceChange(wParam, lParam);
        return TRUE;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        Logger::Log(L"[WndProc] Display/Setting change detected. Updating overlay positions.");
        g_app->m_overlayMgr.UpdatePositions();
        return 0;

    case WM_TRAYICON_MSG:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
            g_app->ShowContextMenu();
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_WATCHDOG) {
            g_app->m_deviceWatcher.CheckConnectionState();
            if (g_app->m_overlayMgr.IsOverlayVisible()) {
                g_app->m_overlayMgr.UpdatePositions();
            }
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

bool Application::Initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    // 1. 로거 초기화 (가장 먼저 실행)
    Logger::Init();
    Logger::Log(L"[App] Initializing TaskbarKeyIndicator v1.0.1...");

    // 2. 단일 인스턴스 중복 실행 방지
    m_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::Log(L"[App] Another instance is already running. Exiting.");
        MessageBoxW(NULL, L"Taskbar Key Indicator가 이미 실행 중입니다.\n작업표시줄 우측 트레이 영역을 확인하세요.", L"알림", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // 3. 설정 로드
    m_config = ConfigManager::LoadConfig();
    Logger::Log(L"[App] Config loaded: AutoDetect=%d, TargetVID=%s, TargetPID=%s, BarThickness=%d",
        m_config.autoDetect ? 1 : 0, m_config.targetVid.c_str(), m_config.targetPid.c_str(), m_config.barThickness);

    // 4. 최상위 숨김 윈도우 생성 (WS_POPUP, 0,0,0,0)
    // [중요] HWND_MESSAGE는 WM_DEVICECHANGE 브로드캐스트를 받지 못하므로, 반드시 최상위 윈도우여야 함!
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = Application::WndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    if (!RegisterClassExW(&wc)) {
        Logger::Log(L"[App] Failed to register main window class! Error: %lu", GetLastError());
    }

    m_hWnd = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"TaskbarKeyIndicator_Core",
        WS_POPUP,
        0, 0, 0, 0,
        NULL, NULL, m_hInstance, NULL
    );

    if (!m_hWnd) {
        Logger::Log(L"[App] Failed to create main window! Error: %lu", GetLastError());
        return false;
    }

    ShowWindow(m_hWnd, SW_HIDE);
    Logger::Log(L"[App] Main Top-level Hidden Window created: HWND %p", m_hWnd);

    // Explorer 재시작 감지 등록
    m_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 5. 오버레이 매니저 초기화
    m_overlayMgr.Initialize(m_hInstance);

    // 6. 트레이 아이콘 설정
    SetupTrayIcon();

    // 7. 디바이스 와처 초기화 및 콜백 연결
    m_deviceWatcher.SetStateCallback([this](bool isConnected, const std::wstring& devName) {
        this->UpdateState(isConnected, devName);
    });

    if (!m_config.autoDetect && !m_config.targetVid.empty() && !m_config.targetPid.empty()) {
        m_deviceWatcher.SetTarget(m_config.targetVid, m_config.targetPid, m_config.targetDeviceName);
    } else {
        m_deviceWatcher.SetAutoDetect(true);
    }

    m_deviceWatcher.Initialize(m_hWnd);

    // 8. 초기 상태 즉시 반영
    UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());

    // 9. 1초 주기 경량 워치독 타이머 시작 (PnP 누락/지연 방지 안전망)
    SetTimer(m_hWnd, TIMER_WATCHDOG, 1000, NULL);

    Logger::Log(L"[App] Initialization completed successfully.");
    return true;
}

int Application::Run(HINSTANCE hInstance) {
    if (!Initialize(hInstance)) {
        return 1;
    }

    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessageW(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) {
            Logger::Log(L"[App] GetMessageW returned -1 (Error). Terminating message loop.");
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    Application app;
    return app.Run(hInstance);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    Application app;
    return app.Run(hInstance);
}
