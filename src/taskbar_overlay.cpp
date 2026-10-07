#include "taskbar_overlay.h"
#include "logger.h"
#include <algorithm>
#include <cstdint>

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

// 32비트 ARGB 프리멀티플라이드 픽셀 생성
static inline uint32_t MakePremultipliedArgb(BYTE r, BYTE g, BYTE b, BYTE a) {
    float f = a / 255.0f;
    BYTE pr = (BYTE)(r * f);
    BYTE pg = (BYTE)(g * f);
    BYTE pb = (BYTE)(b * f);
    return ((uint32_t)a << 24) | ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
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
            // [핵심] WS_EX_LAYERED + WS_EX_TRANSPARENT + WS_EX_TOPMOST
            // DWM 독립 컴포지션 서피스를 사용하여 작업표시줄 클릭 시에도 0.0001초도 지워지지 않음!
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
                SetWindowLongPtrW(hOverlay, GWLP_USERDATA, (LONG_PTR)this);
                OverlayWindowInfo info;
                info.hOverlay = hOverlay;
                info.hTargetTray = hTray;
                info.isPrimary = (hTray == hPrimaryTray);
                m_overlays.push_back(info);
                Logger::Log(L"[Overlay] Created layered overlay HWND: %p for Tray: %p", hOverlay, hTray);
            } else {
                Logger::Log(L"[Overlay] Failed to create overlay window! Error: %lu", GetLastError());
            }
        }
    }

    UpdatePositions();
}

void TaskbarOverlayManager::RenderLayeredOverlay(HWND hOverlay, int x, int y, int w, int h, COLORREF color, bool isHorizontal) {
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // Top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = CreateDIBSection(hdcMem, &bi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HBITMAP hOldBmp = (HBITMAP)SelectObject(hdcMem, hBmp);

    if (pBits) {
        uint32_t* pixels = (uint32_t*)pBits;

        if (isHorizontal) {
            for (int row = 0; row < h; ++row) {
                COLORREF lineCol;
                BYTE alpha = 255;
                if (row == 0) {
                    lineCol = Lighten(color, 0.50f); // 코어 하이라이트
                    alpha = 255;
                } else if (row == 1) {
                    lineCol = Lighten(color, 0.15f); // 메인 네온 바
                    alpha = 255;
                } else if (row == h - 1) {
                    lineCol = Darken(color, 0.20f);  // 소프트 글로우
                    alpha = 220;
                } else {
                    lineCol = color;
                    alpha = 240;
                }

                uint32_t pixelValue = MakePremultipliedArgb(
                    GetRValue(lineCol),
                    GetGValue(lineCol),
                    GetBValue(lineCol),
                    alpha
                );

                int rowStart = row * w;
                for (int col = 0; col < w; ++col) {
                    pixels[rowStart + col] = pixelValue;
                }
            }
        } else {
            for (int col = 0; col < w; ++col) {
                COLORREF lineCol;
                BYTE alpha = 255;
                if (col == 0) {
                    lineCol = Lighten(color, 0.50f);
                    alpha = 255;
                } else if (col == 1) {
                    lineCol = Lighten(color, 0.15f);
                    alpha = 255;
                } else if (col == w - 1) {
                    lineCol = Darken(color, 0.20f);
                    alpha = 220;
                } else {
                    lineCol = color;
                    alpha = 240;
                }

                uint32_t pixelValue = MakePremultipliedArgb(
                    GetRValue(lineCol),
                    GetGValue(lineCol),
                    GetBValue(lineCol),
                    alpha
                );

                for (int row = 0; row < h; ++row) {
                    pixels[row * w + col] = pixelValue;
                }
            }
        }

        POINT ptDst = { x, y };
        POINT ptSrc = { 0, 0 };
        SIZE sizeWnd = { w, h };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

        // [핵심] UpdateLayeredWindow 호출로 DWM 하드웨어 서피스에 비트맵 직접 전송
        UpdateLayeredWindow(hOverlay, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
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
        bool isHorizontal = true;

        if (trayWidth >= trayHeight) {
            // 가로형 작업표시줄
            HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfoW(hMon, &mi);

            if (rcTray.top <= mi.rcMonitor.top + 10) {
                // 상단 작업표시줄
                y = rcTray.bottom - m_thickness;
            } else {
                // 하단 작업표시줄 (기본): 작업표시줄 상단 테두리에 완벽 밀착
                y = rcTray.top;
            }
            w = trayWidth;
            h = m_thickness;
            isHorizontal = true;
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
            isHorizontal = false;
        }

        if (visible) {
            // DWM 레이어드 윈도우 비트맵 전송 및 위치 갱신
            RenderLayeredOverlay(info.hOverlay, x, y, w, h, m_currentColor, isHorizontal);
            SetWindowPos(
                info.hOverlay,
                HWND_TOPMOST,
                x, y, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW
            );
        } else {
            ShowWindow(info.hOverlay, SW_HIDE);
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
    UpdatePositions();
}

void TaskbarOverlayManager::ForceShowTest(int durationMs) {
    Logger::Log(L"[Overlay] ForceShowTest initiated for %d ms", durationMs);
    m_isTesting = true;
    UpdatePositions();

    if (!m_overlays.empty() && m_overlays[0].hOverlay) {
        SetTimer(m_overlays[0].hOverlay, TIMER_TEST_RESTORE, durationMs, NULL);
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

    case WM_NCHITTEST:
        return HTTRANSPARENT; // 마우스 클릭 100% 하위 통과

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}
