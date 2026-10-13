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
// 다중 모니터 변경, 해상도 전환, VDI 재접속 시에도 항상 유효한 작업표시줄을 추적
void TaskbarOverlayManager::SyncTrayOverlays() {
    std::vector<HWND> activeTrays;

    // 1. 주 모니터 작업표시줄
    HWND hPrimaryTray = FindWindowW(L"Shell_TrayWnd", NULL);
    if (hPrimaryTray && IsWindow(hPrimaryTray) && IsWindowVisible(hPrimaryTray)) {
        RECT rc;
        if (GetWindowRect(hPrimaryTray, &rc) && (rc.right > rc.left && rc.bottom > rc.top)) {
            activeTrays.push_back(hPrimaryTray);
        }
    }

    // 2. 보조 모니터 작업표시줄들
    EnumTrayContext ctx;
    EnumWindows(EnumSecondaryTraysProc, (LPARAM)&ctx);
    for (HWND hSec : ctx.secondaryTrays) {
        activeTrays.push_back(hSec);
    }

    // 3. 더 이상 유효하지 않은 오버레이 정리
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
            retained.push_back(info);
        } else {
            if (info.hOverlay && IsWindow(info.hOverlay)) {
                DestroyWindow(info.hOverlay);
            }
        }
    }
    m_overlays = retained;

    // 4. 새롭게 추가된 트레이에 대해 독립 오버레이 창 생성
    for (HWND hTray : activeTrays) {
        bool exists = false;
        for (auto& info : m_overlays) {
            if (info.hTargetTray == hTray) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            // [소유자 분리 핵심]: hWndParent=NULL 독립 창으로 생성
            // 기본 VDI 작업표시줄 본체가 전체화면(개발 VDI) 위로 끌려 올라가는 Z-Order 결함을 원천 차단
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED;
            DWORD style = WS_POPUP;

            HWND hOverlay = CreateWindowExW(
                exStyle,
                OVERLAY_CLASS_NAME,
                L"",
                style,
                0, 0, 0, 0,
                NULL, // No Owner Window
                NULL, m_hInstance, this
            );

            if (hOverlay) {
                OverlayWindowInfo info;
                info.hOverlay = hOverlay;
                info.hTargetTray = hTray;
                info.isPrimary = (hTray == hPrimaryTray);
                m_overlays.push_back(info);
                Logger::Log(L"[Overlay] Created independent overlay HWND: %p for %s Tray: %p",
                    hOverlay, info.isPrimary ? L"Primary" : L"Secondary", hTray);
            } else {
                Logger::Log(L"[Overlay] Failed to create overlay HWND! Error: %lu", GetLastError());
            }
        }
    }
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

        UpdateLayeredWindow(hOverlay, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void TaskbarOverlayManager::UpdatePositions() {
    SyncTrayOverlays();

    bool visible = m_shouldShow || m_isTesting;
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

        int x = rcTray.left;
        int y = rcTray.top;
        int w = trayWidth;
        int h = trayHeight;
        bool isHorizontal = (trayWidth >= trayHeight);

        if (!isFullTint) {
            // 상단/측면 LED 바 모드
            HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = { sizeof(MONITORINFO) };
            GetMonitorInfoW(hMon, &mi);

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

        if (visible) {
            bool needRender = (!info.lastVisible ||
                               info.lastX != x || info.lastY != y ||
                               info.lastW != w || info.lastH != h ||
                               info.lastColor != m_currentColor ||
                               info.lastFullTint != isFullTint);

            if (needRender) {
                RenderLayeredOverlay(info.hOverlay, x, y, w, h, m_currentColor, isHorizontal, isFullTint);
                SetWindowPos(
                    info.hOverlay,
                    HWND_TOPMOST,
                    x, y, w, h,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW
                );

                info.lastX = x;
                info.lastY = y;
                info.lastW = w;
                info.lastH = h;
                info.lastColor = m_currentColor;
                info.lastFullTint = isFullTint;
                info.lastVisible = true;
            } else {
                // 이미 화면에 정상 렌더링된 상태:
                // 사용자가 개발 VDI를 전체화면으로 띄우거나, 다른 창을 클릭하더라도
                // 오버레이가 최상위 TOPMOST로 화면 맨 앞(작업표시줄 위치)에 항상 표시되도록 Z-Order 유지
                // (hOverlay의 소유자가 NULL이므로 기본 VDI 작업표시줄은 절대 끌려 올라오지 않음!)
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

    // 상태 변경 시 캐시 무효화 후 즉시 반영
    for (auto& info : m_overlays) { info.lastVisible = false; }
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
