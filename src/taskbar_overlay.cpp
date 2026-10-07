#include "taskbar_overlay.h"
#include "logger.h"
#include <algorithm>
#include <cstdint>

static const wchar_t* OVERLAY_CLASS_NAME = L"TaskbarKeyIndicator_NeonOverlay";
static const UINT_PTR TIMER_TEST_RESTORE = 9001;

// 색상 계산 유틸리티
static inline COLORREF Lighten(COLORREF c, float factor) {
    BYTE r = (BYTE)std::min(255.0f, GetRValue(c) + (255 - GetRValue(c)) * factor);
    BYTE g = (BYTE)std::min(255.0f, GetGValue(c) + (255 - GetGValue(c)) * factor);
    BYTE b = (BYTE)std::min(255.0f, GetBValue(c) + (255 - GetBValue(c)) * factor);
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
    , m_disconnectedStyle(1) // 기본값 1: 작업표시줄 전체 틴트 (시인성 극대화)
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
        Logger::Log(L"[Overlay] Shell_TrayWnd not found!");
    }

    // 보조 모니터 작업표시줄 열거
    EnumTrayData data;
    EnumWindows(EnumWindowsProc, (LPARAM)&data);
    for (HWND hSec : data.trayHwnds) {
        allTrays.push_back(hSec);
    }

    // 기존 오버레이 중 유효하지 않은 것 정리
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
            // DWM 독립 컴포지션 서피스를 사용하여 작업표시줄 클릭 시에도 100% 완전 투과
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED;
            DWORD style = WS_POPUP;

            HWND hOverlay = CreateWindowExW(
                exStyle,
                OVERLAY_CLASS_NAME,
                L"",
                style,
                0, 0, 0, 0,
                hTray, // 작업표시줄을 소유자(Owner)로 지정
                NULL, m_hInstance, this
            );

            if (hOverlay) {
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

void TaskbarOverlayManager::RenderLayeredOverlay(HWND hOverlay, int x, int y, int w, int h, COLORREF color, bool isHorizontal, bool isFullTint) {
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

        if (isFullTint) {
            // [옵션 1: 작업표시줄 전체 틴트 덮기 모드]
            // 상단 3px는 강렬한 네온 LED 라인으로 하이라이트하고,
            // 작업표시줄 본체 전체는 은은한 반투명 틴트(약 21% 불투명도, 알파 55)로 물들여 시인성 극대화!
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
            // [옵션 2: 상단 네온 LED 바 전용 모드]
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

        // UpdateLayeredWindow 호출로 DWM 하드웨어 서피스에 비트맵 직접 전송
        UpdateLayeredWindow(hOverlay, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
    }

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

void TaskbarOverlayManager::UpdatePositions() {
    bool visible = m_shouldShow || m_isTesting;

    // 녹색(PC 유선)일 때는 항상 상단 LED 바 모드,
    // 붉은색(모바일 전환 또는 테스트)일 때만 사용자가 선택한 스타일(틴트 vs LED 바) 적용
    bool isFullTint = (!m_keyboardConnected || m_isTesting) && (m_disconnectedStyle == 1);

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
        int h = trayHeight;
        bool isHorizontal = (trayWidth >= trayHeight);

        if (isFullTint) {
            // [전체 틴트 모드]: 작업표시줄 전체 사각형 덮기
            x = rcTray.left;
            y = rcTray.top;
            w = trayWidth;
            h = trayHeight;
        } else {
            // [LED 바 모드]: m_thickness 픽셀 두께의 테두리 바
            if (isHorizontal) {
                HMONITOR hMon = MonitorFromWindow(info.hTargetTray, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(MONITORINFO) };
                GetMonitorInfoW(hMon, &mi);

                if (rcTray.top <= mi.rcMonitor.top + 10) {
                    y = rcTray.bottom - m_thickness;
                } else {
                    y = rcTray.top;
                }
                w = trayWidth;
                h = m_thickness;
            } else {
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
        }

        if (visible) {
            RenderLayeredOverlay(info.hOverlay, x, y, w, h, m_currentColor, isHorizontal, isFullTint);
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

void TaskbarOverlayManager::SetDisconnectedStyle(int style) {
    m_disconnectedStyle = style;
    UpdatePositions();
}

void TaskbarOverlayManager::ForceShowTest(int durationMs) {
    Logger::Log(L"[Overlay] ForceShowTest initiated for %d ms (Style: %s)",
        durationMs, m_disconnectedStyle == 1 ? L"Full Tint" : L"LED Bar");
    m_isTesting = true;
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
        return TRUE; // OS 시스템 종료 승인

    case WM_ENDSESSION:
        return 0;

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
