#include "taskbar_overlay.h"
#include "logger.h"
#include <algorithm>

static const wchar_t* OVERLAY_CLASS_NAME = L"TaskbarLedOverlayClass";
static const UINT_PTR TIMER_TEST_RESTORE = 2001;

static COLORREF BlendColor(COLORREF c1, COLORREF c2, float t) {
    BYTE r = (BYTE)(GetRValue(c1) * (1.0f - t) + GetRValue(c2) * t);
    BYTE g = (BYTE)(GetGValue(c1) * (1.0f - t) + GetGValue(c2) * t);
    BYTE b = (BYTE)(GetBValue(c1) * (1.0f - t) + GetBValue(c2) * t);
    return RGB(r, g, b);
}

static COLORREF Lighten(COLORREF c, float amount) {
    return BlendColor(c, RGB(255, 255, 255), amount);
}

static COLORREF Darken(COLORREF c, float amount) {
    return BlendColor(c, RGB(0, 0, 0), amount);
}

TaskbarOverlayManager::TaskbarOverlayManager()
    : m_hInstance(NULL)
    , m_keyboardConnected(true)
    , m_currentColor(RGB(255, 45, 85))
    , m_shouldShow(false)
    , m_isTesting(false)
    , m_thickness(3)
{
}

TaskbarOverlayManager::~TaskbarOverlayManager() {
    Cleanup();
}

bool TaskbarOverlayManager::Initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = TaskbarOverlayManager::WndProc;
    wc.hInstance = m_hInstance;
    wc.lpszClassName = OVERLAY_CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::Log(L"[Overlay] Failed to register overlay window class. Error: %lu", err);
            return false;
        }
    }

    Logger::Log(L"[Overlay] Overlay class registered successfully.");
    return true;
}

void TaskbarOverlayManager::Cleanup() {
    for (auto& info : m_overlays) {
        if (info.hOverlay && IsWindow(info.hOverlay)) {
            DestroyWindow(info.hOverlay);
        }
    }
    m_overlays.clear();
}

struct EnumTrayData {
    std::vector<HWND> trayHwnds;
};

static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    wchar_t className[256];
    if (GetClassNameW(hwnd, className, 256)) {
        if (wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
            auto* data = (EnumTrayData*)lParam;
            data->trayHwnds.push_back(hwnd);
        }
    }
    return TRUE;
}

void TaskbarOverlayManager::CreateOrUpdateOverlays() {
    std::vector<HWND> allTrays;

    // 주 작업표시줄
    HWND hPrimaryTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hPrimaryTray) {
        allTrays.push_back(hPrimaryTray);
    } else {
        Logger::Log(L"[Overlay] Shell_TrayWnd not found! Taskbar handle is NULL.");
    }

    // 보조 모니터 작업표시줄들
    EnumTrayData data;
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    for (HWND hSec : data.trayHwnds) {
        allTrays.push_back(hSec);
    }

    Logger::Log(L"[Overlay] Found %zu taskbar window(s) (Primary: %s, Secondaries: %zu)",
        allTrays.size(), hPrimaryTray ? L"Yes" : L"No", data.trayHwnds.size());

    // 불필요한 기존 오버레이 정리
    std::vector<OverlayWindowInfo> validOverlays;
    for (auto& info : m_overlays) {
        bool found = false;
        for (HWND hTray : allTrays) {
            if (info.hTargetTray == hTray) {
                found = true;
                break;
            }
        }
        if (found && IsWindow(info.hOverlay)) {
            validOverlays.push_back(info);
        } else {
            if (info.hOverlay && IsWindow(info.hOverlay)) {
                DestroyWindow(info.hOverlay);
            }
        }
    }
    m_overlays = validOverlays;

    // 신규 트레이에 대한 오버레이 생성
    for (HWND hTray : allTrays) {
        bool exists = false;
        for (auto& info : m_overlays) {
            if (info.hTargetTray == hTray) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            // WS_EX_TRANSPARENT: 클릭 100% 하위 통과
            // WS_EX_TOOLWINDOW: 작업표시줄/Alt+Tab 제외
            // WS_EX_TOPMOST: 최상위 Z-order
            // WS_EX_NOACTIVATE: 포커스 빼앗지 않음
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
            DWORD style = WS_POPUP;

            HWND hOverlay = CreateWindowExW(
                exStyle,
                OVERLAY_CLASS_NAME,
                L"",
                style,
                0, 0, 0, 0,
                NULL, NULL, m_hInstance, this
            );

            if (hOverlay) {
                SetWindowLongPtrW(hOverlay, GWLP_USERDATA, (LONG_PTR)this);
                OverlayWindowInfo info;
                info.hOverlay = hOverlay;
                info.hTargetTray = hTray;
                info.isPrimary = (hTray == hPrimaryTray);
                m_overlays.push_back(info);
                Logger::Log(L"[Overlay] Created overlay HWND: %p for Tray: %p", hOverlay, hTray);
            } else {
                Logger::Log(L"[Overlay] Failed to create overlay window! Error: %lu", GetLastError());
            }
        }
    }

    UpdatePositions();
}

void TaskbarOverlayManager::UpdatePositions() {
    bool visible = m_shouldShow || m_isTesting;

    for (auto& info : m_overlays) {
        if (!info.hTargetTray || !IsWindow(info.hTargetTray)) continue;

        RECT rcTray;
        if (!GetWindowRect(info.hTargetTray, &rcTray)) {
            Logger::Log(L"[Overlay] Failed to GetWindowRect for Tray %p", info.hTargetTray);
            continue;
        }

        int trayWidth = rcTray.right - rcTray.left;
        int trayHeight = rcTray.bottom - rcTray.top;

        int x = rcTray.left;
        int y = rcTray.top;
        int w = trayWidth;
        int h = m_thickness;

        if (trayWidth >= trayHeight) {
            // 가로형 작업표시줄
            HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfoW(hMon, &mi);

            if (rcTray.top <= mi.rcMonitor.top + 10) {
                // 작업표시줄이 상단인 경우 -> 하단 테두리에 배치
                y = rcTray.bottom - m_thickness;
            } else {
                // 작업표시줄이 하단인 경우 -> 상단 테두리에 배치
                y = rcTray.top;
            }
            w = trayWidth;
            h = m_thickness;
        } else {
            // 세로형 작업표시줄
            HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfoW(hMon, &mi);

            if (rcTray.left <= mi.rcMonitor.left + 10) {
                x = rcTray.right - m_thickness;
            } else {
                x = rcTray.left;
            }
            w = m_thickness;
            h = trayHeight;
        }

        Logger::Log(L"[Overlay] Pos: x=%d, y=%d, w=%d, h=%d, Visible=%s (Tray: %d,%d - %d,%d)",
            x, y, w, h, visible ? L"TRUE" : L"FALSE",
            rcTray.left, rcTray.top, rcTray.right, rcTray.bottom);

        SetWindowPos(
            info.hOverlay,
            HWND_TOPMOST,
            x, y, w, h,
            SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)
        );

        if (visible) {
            InvalidateRect(info.hOverlay, NULL, TRUE);
            UpdateWindow(info.hOverlay);
        }
    }
}

void TaskbarOverlayManager::SetState(bool keyboardConnected, COLORREF disconnectedColor, COLORREF connectedColor, bool showWhenConnected, int thickness) {
    m_keyboardConnected = keyboardConnected;
    m_thickness = thickness;

    if (m_keyboardConnected) {
        m_currentColor = connectedColor;
        m_shouldShow = showWhenConnected;
    } else {
        m_currentColor = disconnectedColor;
        m_shouldShow = true;
    }

    Logger::Log(L"[Overlay] SetState -> Connected: %s, Color: #%02X%02X%02X, Show: %s, Thick: %dpx",
        m_keyboardConnected ? L"YES" : L"NO",
        GetRValue(m_currentColor), GetGValue(m_currentColor), GetBValue(m_currentColor),
        m_shouldShow ? L"YES" : L"NO",
        m_thickness);

    CreateOrUpdateOverlays();
}

void TaskbarOverlayManager::SetThickness(int thickness) {
    m_thickness = thickness;
    UpdatePositions();
}

void TaskbarOverlayManager::SetColor(COLORREF color) {
    m_currentColor = color;
    for (auto& info : m_overlays) {
        if (info.hOverlay && IsWindow(info.hOverlay)) {
            InvalidateRect(info.hOverlay, NULL, TRUE);
        }
    }
}

void TaskbarOverlayManager::ForceShowTest(int durationMs) {
    Logger::Log(L"[Overlay] ForceShowTest initiated for %d ms", durationMs);
    m_isTesting = true;
    UpdatePositions();

    if (!m_overlays.empty() && m_overlays[0].hOverlay) {
        SetTimer(m_overlays[0].hOverlay, TIMER_TEST_RESTORE, durationMs, NULL);
    }
}

void TaskbarOverlayManager::DrawNeonLedBar(HDC hdc, int width, int height, COLORREF baseColor, bool isHorizontal) {
    if (isHorizontal) {
        for (int y = 0; y < height; ++y) {
            COLORREF lineCol;
            if (y == 0) {
                lineCol = Lighten(baseColor, 0.50f); // 최상단 밝은 네온 코어
            } else if (y == 1) {
                lineCol = Lighten(baseColor, 0.20f); // 중간 발광 라인
            } else if (y == height - 1) {
                lineCol = Darken(baseColor, 0.20f);  // 하단 소프트 섀도우
            } else {
                lineCol = baseColor;
            }

            HBRUSH hBrush = CreateSolidBrush(lineCol);
            RECT r = { 0, y, width, y + 1 };
            FillRect(hdc, &r, hBrush);
            DeleteObject(hBrush);
        }
    } else {
        for (int x = 0; x < width; ++x) {
            COLORREF lineCol;
            if (x == 0) {
                lineCol = Lighten(baseColor, 0.50f);
            } else if (x == 1) {
                lineCol = Lighten(baseColor, 0.20f);
            } else if (x == width - 1) {
                lineCol = Darken(baseColor, 0.20f);
            } else {
                lineCol = baseColor;
            }

            HBRUSH hBrush = CreateSolidBrush(lineCol);
            RECT r = { x, 0, x + 1, height };
            FillRect(hdc, &r, hBrush);
            DeleteObject(hBrush);
        }
    }
}

LRESULT CALLBACK TaskbarOverlayManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TaskbarOverlayManager* pThis = (TaskbarOverlayManager*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_TEST_RESTORE && pThis) {
            KillTimer(hwnd, TIMER_TEST_RESTORE);
            pThis->m_isTesting = false;
            Logger::Log(L"[Overlay] ForceShowTest ended. Restoring original state.");
            pThis->UpdatePositions();
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        if (w > 0 && h > 0) {
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            COLORREF color = pThis ? pThis->m_currentColor : RGB(255, 45, 85);
            bool isHorizontal = (w >= h);
            DrawNeonLedBar(memDC, w, h, color, isHorizontal);

            BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT; // 마우스 클릭 100% 투과
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

