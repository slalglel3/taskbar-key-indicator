#include "taskbar_overlay.h"
#include "logger.h"
#include <algorithm>
#include <cstdint>

static const wchar_t* OVERLAY_CLASS_NAME = L"TaskbarKeyIndicator_NeonOverlay";
static const UINT_PTR TIMER_TEST_RESTORE = 9001;

// 색상 계산 유틸리티
static inline COLORREF Lighten(COLORREF c, float factor) {
    BYTE r = (BYTE)(std::min)(255.0f, GetRValue(c) + (255 - GetRValue(c)) * factor);
    BYTE g = (BYTE)(std::min)(255.0f, GetGValue(c) + (255 - GetGValue(c)) * factor);
    BYTE b = (BYTE)(std::min)(255.0f, GetBValue(c) + (255 - GetBValue(c)) * factor);
    return RGB(r, g, b);
}

static inline COLORREF Darken(COLORREF c, float factor) {
    BYTE r = (BYTE)(GetRValue(c) * (1.0f - factor));
    BYTE g = (BYTE)(GetGValue(c) * (1.0f - factor));
    BYTE b = (BYTE)(GetBValue(c) * (1.0f - factor));
    return RGB(r, g, b);
}

static inline uint32_t MakePremultipliedArgb(BYTE r, BYTE g, BYTE b, BYTE a) {
    BYTE pr = (BYTE)((r * a + 127) / 255);
    BYTE pg = (BYTE)((g * a + 127) / 255);
    BYTE pb = (BYTE)((b * a + 127) / 255);
    return ((uint32_t)a << 24) | ((uint32_t)pr << 16) | ((uint32_t)pg << 8) | (uint32_t)pb;
}

TaskbarOverlayManager::TaskbarOverlayManager()
    : m_hInstance(NULL)
    , m_keyboardConnected(true)
    , m_currentColor(RGB(255, 45, 85))
    , m_shouldShow(false)
    , m_isTesting(false)
    , m_thickness(3)
    , m_disconnectedStyle(1) // 기본값 1: 작업표시줄 전체 틴트
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

// 보조 모니터 작업표시줄 열거 컨텍스트
struct EnumTrayContext {
    std::vector<HWND> secondaryTrays;
};

static BOOL CALLBACK EnumSecondaryTraysProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return TRUE;

    wchar_t cls[64] = { 0 };
    if (GetClassNameW(hwnd, cls, 64)) {
        if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
            RECT rc;
            if (GetWindowRect(hwnd, &rc)) {
                if (rc.right > rc.left && rc.bottom > rc.top) {
                    auto* ctx = (EnumTrayContext*)lParam;
                    ctx->secondaryTrays.push_back(hwnd);
                }
            }
        }
    }
    return TRUE;
}

// [동적 자가 치유 트레이 동기화]
// 다중 모니터 변경, VDI 해상도 전환, 작업표시줄 재생성 시 100% 능동 감지 및 복구
void TaskbarOverlayManager::SyncTrayOverlays() {
    std::vector<HWND> activeTrays;

    // 주 작업표시줄
    HWND hPrimaryTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hPrimaryTray && IsWindow(hPrimaryTray) && IsWindowVisible(hPrimaryTray)) {
        RECT rc;
        if (GetWindowRect(hPrimaryTray, &rc) && (rc.right > rc.left && rc.bottom > rc.top)) {
            activeTrays.push_back(hPrimaryTray);
        }
    }

    // 보조 작업표시줄들
    EnumTrayContext ctx;
    EnumWindows(EnumSecondaryTraysProc, (LPARAM)&ctx);
    for (HWND hSec : ctx.secondaryTrays) {
        activeTrays.push_back(hSec);
    }

    // 1. 유효하지 않은 트레이의 오버레이 창 정리
    std::vector<OverlayWindowInfo> retained;
    for (auto& info : m_overlays) {
        bool stillActive = false;
        for (HWND hTray : activeTrays) {
            if (info.hTargetTray == hTray) {
                stillActive = true;
                break;
            }
        }
        if (stillActive && info.hOverlay && IsWindow(info.hOverlay)) {
            info.hMonitor = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            retained.push_back(info);
        } else {
            if (info.hOverlay && IsWindow(info.hOverlay)) {
                Logger::Log(L"[Overlay] Destroying obsolete overlay HWND %p (Tray was %p)", info.hOverlay, info.hTargetTray);
                DestroyWindow(info.hOverlay);
            }
        }
    }
    m_overlays = retained;

    // 2. 신규 트레이에 대한 오버레이 창 생성
    for (HWND hTray : activeTrays) {
        bool exists = false;
        for (auto& info : m_overlays) {
            if (info.hTargetTray == hTray) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            // [소유자 분리 + 완전 투과]
            // hWndParent=NULL로 생성하여 작업표시줄이 VDI 전체화면 위로 딸려 올라가는 Z-Order 결함을 원천 차단
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED;
            DWORD style = WS_POPUP;

            HWND hOverlay = CreateWindowExW(
                exStyle,
                OVERLAY_CLASS_NAME,
                L"",
                style,
                0, 0, 0, 0,
                NULL,
                NULL, m_hInstance, this
            );

            if (hOverlay) {
                OverlayWindowInfo info;
                info.hOverlay = hOverlay;
                info.hTargetTray = hTray;
                info.hMonitor = MonitorFromWindow(hTray, MONITOR_DEFAULTTONEAREST);
                info.isPrimary = (hTray == hPrimaryTray);
                m_overlays.push_back(info);
                Logger::Log(L"[Overlay] Created independent overlay HWND: %p for %s Tray: %p (Mon: %p)",
                    hOverlay, info.isPrimary ? L"Primary" : L"Secondary", hTray, info.hMonitor);
            } else {
                Logger::Log(L"[Overlay] Failed to create overlay HWND! Error: %lu", GetLastError());
            }
        }
    }
}

// [모니터별 독립 전체화면 감지]
// 특정 트레이가 위치한 모니터에 한해서만 전체화면 앱(개발 VDI 전체화면 등)이 덮고 있는지 판별
bool TaskbarOverlayManager::IsTrayCoveredByFullScreen(HWND hTray, HMONITOR hMon) {
    if (!hMon) return false;

    HWND hFg = GetForegroundWindow();
    if (!hFg || hFg == GetDesktopWindow() || hFg == GetShellWindow()) return false;

    // 전경 창이 작업표시줄 자체라면 전체화면 상태가 아님
    wchar_t cls[64] = { 0 };
    if (GetClassNameW(hFg, cls, 64)) {
        if (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
            return false;
        }
    }

    // 전경 창이 속한 모니터 확인
    HMONITOR hFgMon = MonitorFromWindow(hFg, MONITOR_DEFAULTTONEAREST);
    if (!hFgMon || hFgMon != hMon) {
        // [모니터 격리 핵심]: 전경 창이 다른 모니터에 있다면, 이 트레이가 있는 모니터는 전체화면에 덮여있지 않음!
        return false;
    }

    RECT rcWnd;
    if (!GetWindowRect(hFg, &rcWnd)) return false;

    MONITORINFO mi = { sizeof(MONITORINFO) };
    if (!GetMonitorInfoW(hMon, &mi)) return false;

    // 전경 창이 해당 모니터 전체 영역을 완전히 덮고 있는가?
    return (rcWnd.left <= mi.rcMonitor.left && rcWnd.top <= mi.rcMonitor.top &&
            rcWnd.right >= mi.rcMonitor.right && rcWnd.bottom >= mi.rcMonitor.bottom);
}

void TaskbarOverlayManager::RenderLayeredOverlay(HWND hOverlay, int x, int y, int w, int h, COLORREF color, bool isHorizontal, bool isFullTint) {
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // Top-down DIB
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

        if (isFullTint) {
            // [작업표시줄 전체 틴트 덮기 모드]
            BYTE tintAlpha = 55;
            uint32_t tintPixel = MakePremultipliedArgb(GetRValue(color), GetGValue(color), GetBValue(color), tintAlpha);

            if (isHorizontal) {
                for (int row = 0; row < h; ++row) {
                    uint32_t rowPixel;
                    if (row == 0) {
                        rowPixel = MakePremultipliedArgb(GetRValue(Lighten(color, 0.50f)), GetGValue(Lighten(color, 0.50f)), GetBValue(Lighten(color, 0.50f)), 255);
                    } else if (row == 1) {
                        rowPixel = MakePremultipliedArgb(GetRValue(Lighten(color, 0.20f)), GetGValue(Lighten(color, 0.20f)), GetBValue(Lighten(color, 0.20f)), 255);
                    } else if (row == 2) {
                        rowPixel = MakePremultipliedArgb(GetRValue(color), GetGValue(color), GetBValue(color), 220);
                    } else {
                        rowPixel = tintPixel;
                    }

                    int rowStart = row * w;
                    for (int col = 0; col < w; ++col) {
                        pixels[rowStart + col] = rowPixel;
                    }
                }
            } else {
                for (int col = 0; col < w; ++col) {
                    uint32_t colPixel;
                    if (col == 0) {
                        colPixel = MakePremultipliedArgb(GetRValue(Lighten(color, 0.50f)), GetGValue(Lighten(color, 0.50f)), GetBValue(Lighten(color, 0.50f)), 255);
                    } else if (col == 1) {
                        colPixel = MakePremultipliedArgb(GetRValue(Lighten(color, 0.20f)), GetGValue(Lighten(color, 0.20f)), GetBValue(Lighten(color, 0.20f)), 255);
                    } else if (col == 2) {
                        colPixel = MakePremultipliedArgb(GetRValue(color), GetGValue(color), GetBValue(color), 220);
                    } else {
                        colPixel = tintPixel;
                    }

                    for (int row = 0; row < h; ++row) {
                        pixels[row * w + col] = colPixel;
                    }
                }
            }
        } else {
            // [상단 네온 LED 바 전용 모드]
            if (isHorizontal) {
                for (int row = 0; row < h; ++row) {
                    COLORREF lineCol;
                    BYTE alpha = 255;
                    if (row == 0) {
                        lineCol = Lighten(color, 0.50f);
                        alpha = 255;
                    } else if (row == 1) {
                        lineCol = Lighten(color, 0.15f);
                        alpha = 255;
                    } else if (row == h - 1) {
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
        }

        POINT ptDst = { x, y };
        POINT ptSrc = { 0, 0 };
        SIZE sizeWnd = { w, h };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

        BOOL ulwRet = UpdateLayeredWindow(hOverlay, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
        if (!ulwRet) {
            Logger::Log(L"[Overlay] UpdateLayeredWindow failed for HWND %p at (%d,%d %dx%d). Error: %lu",
                hOverlay, x, y, w, h, GetLastError());
        }
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void TaskbarOverlayManager::UpdatePositions() {
    // 1. 매 주기마다 동적 트레이 라이프사이클 동기화 (자가 치유)
    SyncTrayOverlays();

    bool shouldBeVisible = m_shouldShow || m_isTesting;
    bool isFullTint = (!m_keyboardConnected || m_isTesting) && (m_disconnectedStyle == 1);

    for (auto& info : m_overlays) {
        if (!info.hTargetTray || !IsWindow(info.hTargetTray) || !info.hOverlay || !IsWindow(info.hOverlay)) {
            continue;
        }

        RECT rcTray;
        if (!GetWindowRect(info.hTargetTray, &rcTray)) {
            continue;
        }

        int trayWidth = rcTray.right - rcTray.left;
        int trayHeight = rcTray.bottom - rcTray.top;
        if (trayWidth <= 0 || trayHeight <= 0) continue;

        info.hMonitor = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);

        // 해당 모니터에 대한 독립 전체화면 감지
        bool isCoveredByFullScreen = IsTrayCoveredByFullScreen(info.hTargetTray, info.hMonitor);

        // 해당 모니터에서 전체화면 앱이 해제되는 순간 감지 (DWM 서피스 즉각 강제 재전송)
        bool returnedFromFullScreen = (info.lastCoveredFullScreen && !isCoveredByFullScreen);
        info.lastCoveredFullScreen = isCoveredByFullScreen;

        // 이 오버레이 창이 실제로 화면에 보여야 하는 조건
        bool overlayVisible = shouldBeVisible && !isCoveredByFullScreen;

        if (overlayVisible) {
            int x = rcTray.left;
            int y = rcTray.top;
            int w = trayWidth;
            int h = trayHeight;
            bool isHorizontal = (trayWidth >= trayHeight);

            if (!isFullTint) {
                // 상단/측면 LED 바 모드
                MONITORINFO mi = { sizeof(MONITORINFO) };
                GetMonitorInfoW(info.hMonitor, &mi);

                if (isHorizontal) {
                    if (rcTray.top <= mi.rcMonitor.top + 10) {
                        y = rcTray.bottom - m_thickness;
                    } else {
                        y = rcTray.top;
                    }
                    w = trayWidth;
                    h = m_thickness;
                } else {
                    if (rcTray.left <= mi.rcMonitor.left + 10) {
                        x = rcTray.right - m_thickness;
                    } else {
                        x = rcTray.left;
                    }
                    w = m_thickness;
                    h = trayHeight;
                }
            }

            // DWM 서피스 재전송 필요 여부 판단
            bool needRender = (!info.lastVisible ||
                               info.lastX != x || info.lastY != y ||
                               info.lastW != w || info.lastH != h ||
                               info.lastColor != m_currentColor ||
                               info.lastFullTint != isFullTint ||
                               returnedFromFullScreen);

            if (needRender) {
                RenderLayeredOverlay(info.hOverlay, x, y, w, h, m_currentColor, isHorizontal, isFullTint);

                // Z-Order 최상위 보정 (UpdateLayeredWindow로 위치/크기는 이미 설정됨)
                SetWindowPos(
                    info.hOverlay,
                    HWND_TOPMOST,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW
                );

                info.lastX = x;
                info.lastY = y;
                info.lastW = w;
                info.lastH = h;
                info.lastColor = m_currentColor;
                info.lastFullTint = isFullTint;
                info.lastVisible = true;
            } else {
                // 서피스가 이미 유효한 상태:
                // 사용자가 작업표시줄이나 다른 창을 클릭해 작업표시줄이 최상위로 올라와도
                // 오버레이가 뒤로 숨지 않도록 Z-Order만 가볍게 보정
                SetWindowPos(
                    info.hOverlay,
                    HWND_TOPMOST,
                    0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOREDRAW | SWP_NOACTIVATE | SWP_SHOWWINDOW
                );
            }
        } else {
            if (info.lastVisible) {
                ShowWindow(info.hOverlay, SW_HIDE);
                info.lastVisible = false;
            }
        }
    }
}

void TaskbarOverlayManager::SetState(bool keyboardConnected, COLORREF disconnectedColor, COLORREF connectedColor, bool showWhenConnected, int thickness, int disconnectedStyle) {
    m_keyboardConnected = keyboardConnected;
    m_thickness = thickness;
    m_disconnectedStyle = disconnectedStyle;

    if (m_keyboardConnected) {
        m_currentColor = connectedColor;
        m_shouldShow = showWhenConnected;
    } else {
        m_currentColor = disconnectedColor;
        m_shouldShow = true;
    }

    Logger::Log(L"[Overlay] SetState -> Connected: %s, Color: #%02X%02X%02X, Show: %s, Thick: %dpx, Style: %s",
        m_keyboardConnected ? L"YES" : L"NO",
        GetRValue(m_currentColor), GetGValue(m_currentColor), GetBValue(m_currentColor),
        m_shouldShow ? L"YES" : L"NO",
        m_thickness,
        m_disconnectedStyle == 1 ? L"Full Tint" : L"LED Bar");

    UpdatePositions();
}

void TaskbarOverlayManager::SetThickness(int thickness) {
    m_thickness = thickness;
    for (auto& info : m_overlays) { info.lastVisible = false; }
    UpdatePositions();
}

void TaskbarOverlayManager::SetColor(COLORREF color) {
    m_currentColor = color;
    for (auto& info : m_overlays) { info.lastVisible = false; }
    UpdatePositions();
}

void TaskbarOverlayManager::SetDisconnectedStyle(int style) {
    m_disconnectedStyle = style;
    for (auto& info : m_overlays) { info.lastVisible = false; }
    UpdatePositions();
}

void TaskbarOverlayManager::ForceShowTest(int durationMs) {
    Logger::Log(L"[Overlay] ForceShowTest initiated for %d ms (Style: %s)",
        durationMs, m_disconnectedStyle == 1 ? L"Full Tint" : L"LED Bar");
    m_isTesting = true;
    for (auto& info : m_overlays) { info.lastVisible = false; }
    UpdatePositions();

    if (!m_overlays.empty() && m_overlays[0].hOverlay) {
        SetTimer(m_overlays[0].hOverlay, TIMER_TEST_RESTORE, durationMs, NULL);
    }
}

LRESULT CALLBACK TaskbarOverlayManager::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* pCS = (CREATESTRUCTW*)lParam;
        if (pCS && pCS->lpCreateParams) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pCS->lpCreateParams);
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    TaskbarOverlayManager* pThis = (TaskbarOverlayManager*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_TEST_RESTORE && pThis) {
            KillTimer(hwnd, TIMER_TEST_RESTORE);
            pThis->m_isTesting = false;
            Logger::Log(L"[Overlay] ForceShowTest ended. Restoring original state.");
            for (auto& info : pThis->m_overlays) { info.lastVisible = false; }
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
