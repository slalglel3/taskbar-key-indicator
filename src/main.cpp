#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <algorithm>

#include "resource.h"
#include "logger.h"
#include "config.h"
#include "taskbar_overlay.h"
#include "browser_watcher.h"

static const wchar_t* MAIN_WINDOW_CLASS = L"KeyIndicator_MsgWnd";
static const wchar_t* MUTEX_NAME = L"KeyIndicator_SingleInstance_Mutex";
static const UINT WM_TRAYICON_MSG = WM_APP + 101;
static const UINT_PTR TIMER_WATCHDOG = 3001;

// 트레이 메뉴 ID 상수
enum MenuIDs {
    IDM_STATUS_HEADER = 1001,
    IDM_SYNC_TITLE,
    IDM_SEPARATOR_1,
    IDM_TEST_OVERLAY,
    IDM_OPEN_LOG,
    IDM_SEPARATOR_2,
    IDM_STYLE_TINT,
    IDM_STYLE_BAR,
    IDM_SEPARATOR_STYLE,
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
    void Cleanup();

private:
    HINSTANCE m_hInstance;
    HWND m_hWnd;
    HANDLE m_hMutex;
    UINT m_wmTaskbarCreated;
    NOTIFYICONDATAW m_nid;
    HICON m_hCurrentTrayIcon;
    bool m_cleanedUp;

    AppConfig m_config;
    TaskbarOverlayManager m_overlayMgr;
    BrowserWatcher m_browserWatcher;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool Initialize(HINSTANCE hInstance);
    void UpdateState(bool isConnected, const std::wstring& title);
    void SetupTrayIcon();
    void UpdateTrayIcon(bool isConnected);
    void ShowContextMenu();
    HICON CreateKeycapIcon(COLORREF color);
};

static Application* g_app = nullptr;

Application::Application()
    : m_hInstance(NULL)
    , m_hWnd(NULL)
    , m_hMutex(NULL)
    , m_wmTaskbarCreated(0)
    , m_hCurrentTrayIcon(NULL)
    , m_cleanedUp(false)
{
    memset(&m_nid, 0, sizeof(m_nid));
    g_app = this;
}

Application::~Application() {
    Cleanup();
}

void Application::Cleanup() {
    if (m_cleanedUp) return;
    m_cleanedUp = true;

    Logger::Log(L"[App] Shutting down KeyIndicator and cleaning up all resources...");
    if (m_hCurrentTrayIcon) {
        DestroyIcon(m_hCurrentTrayIcon);
        m_hCurrentTrayIcon = NULL;
    }
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    m_overlayMgr.Cleanup();
    m_browserWatcher.Cleanup();
    if (m_hWnd) {
        KillTimer(m_hWnd, TIMER_WATCHDOG);
        DestroyWindow(m_hWnd);
        m_hWnd = NULL;
    }
    if (m_hMutex) {
        CloseHandle(m_hMutex);
        m_hMutex = NULL;
    }
    Logger::Close();
}

// 큼직하고 세련된 미니멀 기계식 키캡(Keycap) 형태의 트레이 아이콘 동적 생성
HICON Application::CreateKeycapIcon(COLORREF color) {
    int iconSize = GetSystemMetrics(SM_CXSMICON);
    if (iconSize < 16) iconSize = 16;
    if (iconSize > 32) iconSize = 32;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, iconSize, iconSize);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    HBITMAP hMask = CreateBitmap(iconSize, iconSize, 1, 1, NULL);
    HDC hdcMask = CreateCompatibleDC(hdcScreen);
    HBITMAP hOldMask = (HBITMAP)SelectObject(hdcMask, hMask);

    // 배경 초기화
    RECT rcFull = { 0, 0, iconSize, iconSize };
    HBRUSH hBrBlack = (HBRUSH)GetStockObject(BLACK_BRUSH);
    HBRUSH hBrWhite = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(hdcMem, &rcFull, hBrBlack);
    FillRect(hdcMask, &rcFull, hBrWhite);

    int pad = 1;
    int baseLeft = pad;
    int baseTop = pad + 1;
    int baseRight = iconSize - pad;
    int baseBottom = iconSize - pad;
    int cornerR = (std::max)(3, iconSize / 5);

    // 1. 마스크에 키캡 실루엣을 검정색(불투명)으로 그리기
    HBRUSH hOldMaskBr = (HBRUSH)SelectObject(hdcMask, hBrBlack);
    HPEN hOldMaskPen = (HPEN)SelectObject(hdcMask, GetStockObject(NULL_PEN));
    RoundRect(hdcMask, baseLeft, baseTop, baseRight, baseBottom, cornerR * 2, cornerR * 2);

    // 2. 키캡 하단 베이스 (Dark Slate Charcoal)
    HBRUSH hBrBase = CreateSolidBrush(RGB(30, 33, 40));
    HPEN hPenBase = CreatePen(PS_SOLID, 1, RGB(18, 20, 24));
    HBRUSH hOldBr = (HBRUSH)SelectObject(hdcMem, hBrBase);
    HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPenBase);
    RoundRect(hdcMem, baseLeft, baseTop, baseRight, baseBottom, cornerR * 2, cornerR * 2);

    // 3. 키캡 상단 탑 페이스 (Deep Matte Key Surface)
    int insetX = (std::max)(2, iconSize / 7);
    int insetTop = (std::max)(1, iconSize / 9);
    int insetBottom = (std::max)(3, iconSize / 5);
    int topR = (std::max)(2, cornerR - 1);

    HBRUSH hBrTop = CreateSolidBrush(RGB(48, 53, 62));
    HPEN hPenTopRim = CreatePen(PS_SOLID, 1, RGB(72, 78, 92)); // 상단 림 하이라이트
    SelectObject(hdcMem, hBrTop);
    SelectObject(hdcMem, hPenTopRim);
    RoundRect(hdcMem, baseLeft + insetX, baseTop + insetTop, baseRight - insetX, baseBottom - insetBottom, topR * 2, topR * 2);

    // 4. 중앙 네온 LED 인디케이터 바
    int ledMarginX = (std::max)(3, iconSize / 4);
    int ledTop = baseTop + insetTop + (std::max)(2, (baseBottom - insetBottom - baseTop - insetTop) / 3);
    int ledHeight = (std::max)(3, iconSize / 5);
    int ledBottom = ledTop + ledHeight;
    int ledLeft = baseLeft + ledMarginX;
    int ledRight = baseRight - ledMarginX;
    int ledR = (std::max)(2, ledHeight / 2);

    // 네온 글로우 외곽
    COLORREF glowCol = RGB(
        (GetRValue(color) + 48) / 2,
        (GetGValue(color) + 52) / 2,
        (GetBValue(color) + 60) / 2
    );
    HBRUSH hBrGlow = CreateSolidBrush(glowCol);
    SelectObject(hdcMem, hBrGlow);
    SelectObject(hdcMem, GetStockObject(NULL_PEN));
    RoundRect(hdcMem, ledLeft - 1, ledTop - 1, ledRight + 1, ledBottom + 1, (ledR + 1) * 2, (ledR + 1) * 2);

    // 네온 메인 컬러
    HBRUSH hBrLed = CreateSolidBrush(color);
    SelectObject(hdcMem, hBrLed);
    RoundRect(hdcMem, ledLeft, ledTop, ledRight, ledBottom, ledR * 2, ledR * 2);

    // 퓨어 화이트 핫스팟 코어
    if (ledHeight >= 3 && (ledRight - ledLeft) >= 4) {
        int coreLeft = ledLeft + (std::max)(1, (ledRight - ledLeft) / 4);
        int coreRight = ledRight - (std::max)(1, (ledRight - ledLeft) / 4);
        int coreTop = ledTop + 1;
        int coreBottom = ledBottom - 1;
        if (coreBottom > coreTop && coreRight > coreLeft) {
            HBRUSH hBrCore = CreateSolidBrush(RGB(255, 255, 255));
            SelectObject(hdcMem, hBrCore);
            RoundRect(hdcMem, coreLeft, coreTop, coreRight, coreBottom, 2, 2);
            SelectObject(hdcMem, hOldBr);
            DeleteObject(hBrCore);
        }
    }

    // DC 및 스톡 객체 원상 복원
    SelectObject(hdcMem, hOldBr);
    SelectObject(hdcMem, hOldPen);
    SelectObject(hdcMask, hOldMaskBr);
    SelectObject(hdcMask, hOldMaskPen);
    SelectObject(hdcMem, hOldBmp);
    SelectObject(hdcMask, hOldMask);

    DeleteDC(hdcMem);
    DeleteDC(hdcMask);
    ReleaseDC(NULL, hdcScreen);

    // 커스텀 GDI 리소스 삭제
    DeleteObject(hBrBase);
    DeleteObject(hPenBase);
    DeleteObject(hBrTop);
    DeleteObject(hPenTopRim);
    DeleteObject(hBrGlow);
    DeleteObject(hBrLed);

    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmColor = hBmp;
    ii.hbmMask = hMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hBmp);
    DeleteObject(hMask);

    return hIcon;
}

void Application::SetupTrayIcon() {
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON_MSG;

    m_hCurrentTrayIcon = CreateKeycapIcon(RGB(50, 215, 75));
    m_nid.hIcon = m_hCurrentTrayIcon;
    wcscpy_s(m_nid.szTip, L"Key Indicator");

    if (Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        Logger::Log(L"[Tray] Keycap tray icon added successfully.");
    } else {
        Logger::Log(L"[Tray] Failed to add tray icon! Error: %lu", GetLastError());
    }
}

void Application::UpdateTrayIcon(bool isConnected) {
    COLORREF icoColor = isConnected ? m_config.connectedColor : m_config.disconnectedColor;

    if (m_hCurrentTrayIcon) {
        DestroyIcon(m_hCurrentTrayIcon);
    }
    m_hCurrentTrayIcon = CreateKeycapIcon(icoColor);
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

void Application::UpdateState(bool isConnected, const std::wstring& title) {
    Logger::Log(L"[App] UpdateState -> Connected: %s, Title: %s, Style: %s",
        isConnected ? L"YES" : L"NO", title.c_str(),
        m_config.disconnectedStyle == 1 ? L"Full Tint" : L"LED Bar");

    m_overlayMgr.SetState(
        isConnected,
        m_config.disconnectedColor,
        m_config.connectedColor,
        m_config.showWhenConnected,
        m_config.barThickness,
        m_config.disconnectedStyle
    );

    UpdateTrayIcon(isConnected);
}

void Application::ShowContextMenu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    m_browserWatcher.CheckCurrentState();
    bool isConnected = m_browserWatcher.IsConnected();
    std::wstring matchedTitle = m_browserWatcher.GetMatchedTitle();

    // 1. 상태 헤더
    std::wstring statusStr = isConnected ? L"● 상태: PC 연결됨 (초록 LED)" : L"○ 상태: 모바일 전환됨 (적색 틴트/LED)";
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_STATUS_HEADER, statusStr.c_str());

    std::wstring syncDesc = matchedTitle.empty() ? L"  동기화: [대기 중]" : (L"  동기화: " + matchedTitle);
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | MF_GRAYED, IDM_SYNC_TITLE, syncDesc.c_str());

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_1, NULL);

    // 2. 테스트 및 로그
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_TEST_OVERLAY, L"⚡ 모바일 전환 효과 강제 테스트 (5초간 표시)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_LOG, L"📋 실시간 로그 열기 (debug.log)");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_2, NULL);

    // 3. 모바일 전환 시 표시 스타일 서브메뉴
    HMENU hStyleMenu = CreatePopupMenu();
    InsertMenuW(hStyleMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedStyle == 1 ? MF_CHECKED : 0),
        IDM_STYLE_TINT, L"작업표시줄 전체 틴트 덮기 (시인성 극대화) [추천]");
    InsertMenuW(hStyleMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedStyle == 0 ? MF_CHECKED : 0),
        IDM_STYLE_BAR, L"상단 네온 LED 바만 표시 (심플)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hStyleMenu, L"🎨 모바일 전환 시 표시 스타일");

    // 4. LED 바/틴트 색상 서브메뉴
    HMENU hColorMenu = CreatePopupMenu();
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 45, 85) ? MF_CHECKED : 0), IDM_COLOR_RED, L"네온 레드 (#FF2D55) [기본]");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 149, 0) ? MF_CHECKED : 0), IDM_COLOR_ORANGE, L"네온 오렌지 (#FF9500)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 204, 0) ? MF_CHECKED : 0), IDM_COLOR_AMBER, L"네온 앰버 (#FFCC00)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(255, 55, 95) ? MF_CHECKED : 0), IDM_COLOR_PINK, L"네온 핑크 (#FF375F)");
    InsertMenuW(hColorMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.disconnectedColor == RGB(0, 122, 255) ? MF_CHECKED : 0), IDM_COLOR_BLUE, L"네온 블루 (#007AFF)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hColorMenu, L"모바일 전환 색상 설정");

    // 5. LED 바 두께 서브메뉴
    HMENU hThickMenu = CreatePopupMenu();
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 2 ? MF_CHECKED : 0), IDM_THICKNESS_2, L"2 픽셀");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 3 ? MF_CHECKED : 0), IDM_THICKNESS_3, L"3 픽셀 [권장]");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 4 ? MF_CHECKED : 0), IDM_THICKNESS_4, L"4 픽셀");
    InsertMenuW(hThickMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.barThickness == 5 ? MF_CHECKED : 0), IDM_THICKNESS_5, L"5 픽셀");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_POPUP, (UINT_PTR)hThickMenu, L"LED 바 두께 설정");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_3, NULL);

    // 6. 옵션
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.showWhenConnected ? MF_CHECKED : 0), IDM_SHOW_WHEN_CONNECTED, L"유선 연결 시에도 초록 LED 표시");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING | (m_config.autoStart ? MF_CHECKED : 0), IDM_AUTO_START, L"윈도우 시작 시 자동 실행");

    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, IDM_SEPARATOR_4, NULL);

    // 7. 설정 파일 열기 & 종료
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_OPEN_CONFIG, L"설정 파일 열기 (config.ini)");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_STRING, IDM_EXIT, L"종료 (Exit)");

    // 팝업 메뉴 트랙
    SetForegroundWindow(m_hWnd);
    int cmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, m_hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == 0) return;

    switch (cmd) {
    case IDM_TEST_OVERLAY:
        m_overlayMgr.ForceShowTest(5000);
        break;

    case IDM_OPEN_LOG:
        Logger::OpenLogFile();
        break;

    case IDM_STYLE_TINT:
        m_config.disconnectedStyle = 1;
        ConfigManager::SaveConfig(m_config);
        m_overlayMgr.SetDisconnectedStyle(1);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
        break;

    case IDM_STYLE_BAR:
        m_config.disconnectedStyle = 0;
        ConfigManager::SaveConfig(m_config);
        m_overlayMgr.SetDisconnectedStyle(0);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
        break;

    case IDM_COLOR_RED:
        m_config.disconnectedColor = RGB(255, 45, 85);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
        break;
    case IDM_COLOR_ORANGE:
        m_config.disconnectedColor = RGB(255, 149, 0);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
        break;
    case IDM_COLOR_AMBER:
        m_config.disconnectedColor = RGB(255, 204, 0);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
        break;
    case IDM_COLOR_PINK:
        m_config.disconnectedColor = RGB(255, 55, 95);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
        break;
    case IDM_COLOR_BLUE:
        m_config.disconnectedColor = RGB(0, 122, 255);
        ConfigManager::SaveConfig(m_config);
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
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
        UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());
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
        return TRUE; // 시스템 종료 즉시 동의

    case WM_ENDSESSION:
        if (wParam == TRUE) {
            Logger::Log(L"[WndProc] WM_ENDSESSION received (EndSession=TRUE). Performing synchronous clean shutdown.");
            g_app->Cleanup();
            PostQuitMessage(0); // 클린 탈출
        }
        return 0;

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
            g_app->m_browserWatcher.CheckCurrentState();
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

    // 1. 로거 초기화
    Logger::Init();
    Logger::Log(L"[App] Initializing KeyIndicator v1.3.1 (Keycap Icon Edition)...");

    // 2. 단일 인스턴스 중복 실행 방지
    m_hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Logger::Log(L"[App] Another instance is already running. Exiting.");
        MessageBoxW(NULL, L"Key Indicator가 이미 실행 중입니다.\n작업표시줄 우측 트레이 영역을 확인하세요.", L"알림", MB_OK | MB_ICONINFORMATION);
        return false;
    }

    // 3. 설정 로드
    m_config = ConfigManager::LoadConfig();
    Logger::Log(L"[App] Config loaded: BarThickness=%d, ShowWhenConnected=%d, DisconnectedStyle=%d",
        m_config.barThickness, m_config.showWhenConnected ? 1 : 0, m_config.disconnectedStyle);

    // 4. 최상위 숨김 메시지 윈도우 생성 (WS_POPUP, 0,0,0,0)
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = Application::WndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = MAIN_WINDOW_CLASS;
    wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(m_hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!RegisterClassExW(&wc)) {
        Logger::Log(L"[App] Failed to register main window class! Error: %lu", GetLastError());
    }

    m_hWnd = CreateWindowExW(
        0, MAIN_WINDOW_CLASS, L"KeyIndicator_Core",
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

    // 6. 트레이 아이콘 설정 (키캡 모양)
    SetupTrayIcon();

    // 7. 브라우저 실시간(0ms) 타이틀 와처 초기화 및 콜백 연결
    m_browserWatcher.SetStateCallback([this](bool isConnected, const std::wstring& title) {
        this->UpdateState(isConnected, title);
    });
    m_browserWatcher.Initialize();

    // 초기 브라우저 타이틀 상태 스캔 및 즉시 반영
    m_browserWatcher.CheckCurrentState();
    UpdateState(m_browserWatcher.IsConnected(), m_browserWatcher.GetMatchedTitle());

    // 8. 1초 주기 경량 워치독 타이머 시작
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
