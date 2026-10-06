#include "taskbar_overlay.h"
#include <algorithm>

static const wchar_t* OVERLAY_CLASS_NAME = L"TaskbarLedOverlayClass";

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
    wc.hbrBackground = NULL; // 직접 더블 버퍼링 드로잉

    RegisterClassExW(&wc);
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
    }

    // 보조 모니터 작업표시줄들
    EnumTrayData data;
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    for (HWND hSec : data.trayHwnds) {
        allTrays.push_back(hSec);
    }

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
            // WS_EX_TRANSPARENT: 모든 마우스 클릭이 100% 아래의 작업표시줄로 통과!
            // WS_EX_TOOLWINDOW: 작업표시줄이나 Alt+Tab에 표시되지 않음
            // WS_EX_TOPMOST: 최상위 유지
            // WS_EX_NOACTIVATE: 활성화 포커스 뺏지 않음
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED;
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
                // 불투명도 255 (단색 더블 버퍼링 렌더링)
                SetLayeredWindowAttributes(hOverlay, 0, 255, LWA_ALPHA);
                OverlayWindowInfo info;
                info.hOverlay = hOverlay;
                info.hTargetTray = hTray;
                info.isPrimary = (hTray == hPrimaryTray);
                m_overlays.push_back(info);
            }
        }
    }

    UpdatePositions();
}

void TaskbarOverlayManager::UpdatePositions() {
    for (auto& info : m_overlays) {
        if (!info.hTargetTray || !IsWindow(info.hTargetTray)) continue;

        RECT rcTray;
        if (!GetWindowRect(info.hTargetTray, &rcTray)) continue;

        int trayWidth = rcTray.right - rcTray.left;
        int trayHeight = rcTray.bottom - rcTray.top;

        // 작업표시줄 위치(하단, 상단, 좌측, 우측) 자동 판별
        int x = rcTray.left;
        int y = rcTray.top;
        int w = trayWidth;
        int h = m_thickness;

        // 수평 작업표시줄(대다수 윈도우 기본) vs 수직 작업표시줄
        if (trayWidth >= trayHeight) {
            // 가로형 작업표시줄
            // 화면 상단에 붙어있는 경우 vs 하단에 붙어있는 경우
            HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfoW(hMon, &mi);

            if (rcTray.top <= mi.rcMonitor.top + 5) {
                // 작업표시줄이 화면 맨 위에 위치함 -> LED 바는 작업표시줄 하단 테두리에 배치
                y = rcTray.bottom - m_thickness;
            } else {
                // 작업표시줄이 화면 맨 아래에 위치함 -> LED 바는 작업표시줄 상단 테두리에 배치!
                y = rcTray.top;
            }
            w = trayWidth;
            h = m_thickness;
        } else {
            // 세로형 작업표시줄
            HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfoW(hMon, &mi);

            if (rcTray.left <= mi.rcMonitor.left + 5) {
                // 좌측 작업표시줄 -> 우측 테두리에 배치
                x = rcTray.right - m_thickness;
            } else {
                // 우측 작업표시줄 -> 좌측 테두리에 배치
                x = rcTray.left;
            }
            w = m_thickness;
            h = trayHeight;
        }

        SetWindowPos(
            info.hOverlay,
            HWND_TOPMOST,
            x, y, w, h,
            SWP_NOACTIVATE | (m_shouldShow ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)
        );

        if (m_shouldShow) {
            InvalidateRect(info.hOverlay, NULL, FALSE);
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
        m_shouldShow = true; // 유선 분리 시에는 항상 선명한 LED 바 점등!
    }

    CreateOrUpdateOverlays();
}

void TaskbarOverlayManager::SetThickness(int thickness) {
    m_thickness = thickness;
    UpdatePositions();
}

void TaskbarOverlayManager::DrawNeonLedBar(HDC hdc, int width, int height, COLORREF baseColor, bool isHorizontal) {
    // 세련된 하드웨어 게이밍 LED 스트립 / 앰비언트 네온 효과:
    // 중심 코어는 고휘도(White Tinted), 가장자리는 은은한 글로우로 렌더링
    if (isHorizontal) {
        for (int y = 0; y < height; ++y) {
            float norm = (float)y / (float)(height > 1 ? height - 1 : 1); // 0.0 ~ 1.0
            // 상단 1px(또는 0번 라인)은 가장 밝은 네온 코어, 아래로 갈수록 본연의 색상과 부드러운 섀도우
            COLORREF lineCol;
            if (y == 0) {
                lineCol = Lighten(baseColor, 0.45f); // 중심 코어 하이라이트 (눈부신 네온 라인)
            } else if (y == 1) {
                lineCol = Lighten(baseColor, 0.15f); // 메인 발광 컬러
            } else if (y == height - 1) {
                lineCol = Darken(baseColor, 0.25f);  // 작업표시줄과 자연스럽게 녹아드는 글로우
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
                lineCol = Lighten(baseColor, 0.45f);
            } else if (x == 1) {
                lineCol = Lighten(baseColor, 0.15f);
            } else if (x == width - 1) {
                lineCol = Darken(baseColor, 0.25f);
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
    static TaskbarOverlayManager* s_mgr = nullptr;

    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
        if (cs && cs->lpCreateParams) {
            s_mgr = (TaskbarOverlayManager*)cs->lpCreateParams;
        }
        return 0;
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        // 더블 버퍼링으로 깜빡임 0%
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

        COLORREF color = s_mgr ? s_mgr->m_currentColor : RGB(255, 45, 85);
        bool isHorizontal = (w >= h);
        DrawNeonLedBar(memDC, w, h, color, isHorizontal);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // 깜빡임 방지
    case WM_NCHITTEST:
        return HTTRANSPARENT; // 마우스 클릭 100% 하부 통과
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}
