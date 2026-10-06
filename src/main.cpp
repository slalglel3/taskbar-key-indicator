#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#include "config.h"
#include "taskbar_overlay.h"
#include "device_watcher.h"

static const wchar_t* MAIN_WINDOW_CLASS = L"TaskbarKeyIndicator_MsgWnd";
static const wchar_t* MUTEX_NAME = L"TaskbarKeyIndicator_SingleInstance_Mutex";
static const UINT WM_TRAYICON_MSG = WM_APP + 101;

// 메뉴 ID 상수
enum MenuIDs {
    IDM_STATUS_HEADER = 1001,
    IDM_TARGET_NAME,
    IDM_SEPARATOR_1,
    IDM_LOCK_CURRENT_KB,
    IDM_AUTO_DETECT_KB,
    IDM_SEPARATOR_2,
    IDM_COLOR_RED,
    IDM_COLOR_ORANGE,
    IDM_COLOR_AMBER,
    IDM_COLOR_PINK,
    IDM_COLOR_BLUE,
    IDM_SEPARATOR_3,
    IDM_THICKNESS_2,
    IDM_THICKNESS_3,
    IDM_THICKNESS_4,
    IDM_THICKNESS_5,
    IDM_SEPARATOR_4,
    IDM_SHOW_WHEN_CONNECTED,
    IDM_AUTO_START,
    IDM_OPEN_CONFIG,
    IDM_SEPARATOR_5,
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
    if (m_hCurrentTrayIcon) {
        DestroyIcon(m_hCurrentTrayIcon);
        m_hCurrentTrayIcon = NULL;
    }
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    if (m_hMutex) {
        CloseHandle(m_hMutex);
        m_hMutex = NULL;
    }
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
    FillRect(hdcMem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    FillRect(hdcMask, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));

    HBRUSH hBrColor = CreateSolidBrush(color);
    SelectObject(hdcMem, hBrColor);
    SelectObject(hdcMask, (HBRUSH)GetStockObject(BLACK_BRUSH));

    Ellipse(hdcMem, 2, 2, 14, 14);
    Ellipse(hdcMask, 2, 2, 14, 14);

    // 하이라이트 코어
    HBRUSH hBrWhite = CreateSolidBrush(RGB(255, 255, 255));
    SelectObject(hdcMem, hBrWhite);
    Ellipse(hdcMem, 4, 4, 8, 8);

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

    Shell_NotifyIconW(NIM_ADD, &m_nid);
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

    std::wstring kbDesc = L"  감지: " + targetName;
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_TARGET_NAME, kbDesc.c_str());

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_1, NULL);

    // 2. 키보드 타겟 설정
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.autoDetect ? 0 : MF_CHECKED), IDM_LOCK_CURRENT_KB, L"현재 키보드를 감시 타겟으로 고정");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.autoDetect ? MF_CHECKED : 0), IDM_AUTO_DETECT_KB, L"자동 감지 모드 (기본값)");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_2, NULL);

    // 3. LED 색상 서브메뉴
    HMENU hColorMenu = CreatePopupMenu();
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 45, 85) ? MF_CHECKED : 0), IDM_COLOR_RED, L"네온 레드 (#FF2D55) [기본]");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 149, 0) ? MF_CHECKED : 0), IDM_COLOR_ORANGE, L"네온 오렌지 (#FF9500)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 204, 0) ? MF_CHECKED : 0), IDM_COLOR_AMBER, L"네온 앰버 (#FFCC00)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 55, 95) ? MF_CHECKED : 0), IDM_COLOR_PINK, L"네온 핑크 (#FF375F)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(0, 122, 255) ? MF_CHECKED : 0), IDM_COLOR_BLUE, L"네온 블루 (#007AFF)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hColorMenu, L"LED 바 색상 설정");

    // 4. LED 바 두께 서브메뉴
    HMENU hThickMenu = CreatePopupMenu();
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 2 ? MF_CHECKED : 0), IDM_THICKNESS_2, L"2 픽셀");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 3 ? MF_CHECKED : 0), IDM_THICKNESS_3, L"3 픽셀 [권장]");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 4 ? MF_CHECKED : 0), IDM_THICKNESS_4, L"4 픽셀");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 5 ? MF_CHECKED : 0), IDM_THICKNESS_5, L"5 픽셀");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hThickMenu, L"LED 바 두께 설정");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_3, NULL);

    // 5. 옵션
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.showWhenConnected ? MF_CHECKED : 0), IDM_SHOW_WHEN_CONNECTED, L"유선 연결 시에도 초록 LED 표시");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.autoStart ? MF_CHECKED : 0), IDM_AUTO_START, L"윈도우 시작 시 자동 실행");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_4, NULL);

    // 6. 설정 파일 열기 & 종료
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_CONFIG, L"설정 파일 열기 (config.ini)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"종료 (Exit)");

    // 팝업 메뉴 트랙
    SetForegroundWindow(m_hWnd);
    int cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, m_hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    switch (cmd) {
    case IDM_LOCK_CURRENT_KB: {
        auto kbs = m_deviceWatcher.GetConnectedKeyboards();
        if (!kbs.empty()) {
            m_config.targetVid = kbs[0].vid;
            m_config.targetPid = kbs[0].pid;
            m_config.targetDeviceName = kbs[0].friendlyName;
            m_config.autoDetect = false;
            m_deviceWatcher.SetTarget(m_config.targetVid, m_config.targetPid);
            ConfigManager::SaveConfig(m_config);
        }
        break;
    }
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
        // 탐색기(Explorer) 재시작 감지: 트레이 아이콘 복원 및 작업표시줄 오버레이 재동기화
        g_app->SetupTrayIcon();
        g_app->m_overlayMgr.UpdatePositions();
        return 0;
    }

    switch (msg) {
    case WM_DEVICECHANGE:
        g_app->m_deviceWatcher.OnDeviceChange(wParam, lParam);
        return TRUE;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        // 해상도 변경 또는 작업표시줄 설정 변경 시 오버레이 위치 자동 갱신
        g_app->m_overlayMgr.UpdatePositions();
        return 0;

    case WM_TRAYICON_MSG:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
            g_app->ShowContextMenu();
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool Application::Initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    // 단일 인스턴스 중복 실행 방지
    m_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Taskbar Key Indicator가 이미 실행 중입니다.\n작업표시줄 우측 트레이 영역을 확인하세요.", L"알림", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // 설정 로드
    m_config = ConfigManager::LoadConfig();

    // 윈도우 클래스 등록 (백그라운드 메시지 전용 윈도우)
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = Application::WndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    RegisterClassExW(&wc);

    m_hWnd = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"TaskbarKeyIndicator_Core",
        0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, m_hInstance, NULL
    );

    if (!m_hWnd) return false;

    // Explorer 재시작 메시지 등록
    m_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 오버레이 매니저 초기화
    m_overlayMgr.Initialize(m_hInstance);

    // 트레이 아이콘 설정
    SetupTrayIcon();

    // 디바이스 와처 초기화 및 콜백 연결
    m_deviceWatcher.SetStateCallback([this](bool isConnected, const std::wstring& devName) {
        this->UpdateState(isConnected, devName);
    });

    if (!m_config.autoDetect && !m_config.targetVid.empty() && !m_config.targetPid.empty()) {
        m_deviceWatcher.SetTarget(m_config.targetVid, m_config.targetPid);
    } else {
        m_deviceWatcher.SetAutoDetect(true);
    }

    m_deviceWatcher.Initialize(m_hWnd);

    // 초기 상태 반영
    UpdateState(m_deviceWatcher.IsTargetConnected(), m_deviceWatcher.GetCurrentTargetName());

    return true;
}

int Application::Run(HINSTANCE hInstance) {
    if (!Initialize(hInstance)) {
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
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

