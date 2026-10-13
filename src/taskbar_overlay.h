#pragma once

#include <windows.h>
#include <vector>

struct OverlayWindowInfo {
    HWND hOverlay;
    HWND hTargetTray;
    bool isPrimary;

    int lastX;
    int lastY;
    int lastW;
    int lastH;
    COLORREF lastColor;
    bool lastFullTint;
    bool lastVisible;

    OverlayWindowInfo()
        : hOverlay(NULL), hTargetTray(NULL), isPrimary(false)
        , lastX(-1), lastY(-1), lastW(-1), lastH(-1)
        , lastColor(0), lastFullTint(false), lastVisible(false) {}
};

class TaskbarOverlayManager {
public:
    TaskbarOverlayManager();
    ~TaskbarOverlayManager();

    bool Initialize(HINSTANCE hInstance);
    void UpdatePositions();
    void SetState(bool keyboardConnected, COLORREF disconnectedColor, COLORREF connectedColor, bool showWhenConnected, int thickness, int disconnectedStyle);
    void SetThickness(int thickness);
    void SetColor(COLORREF color);
    void SetDisconnectedStyle(int style);
    void ForceShowTest(int durationMs = 5000);
    void Cleanup();

    bool IsOverlayVisible() const { return m_shouldShow || m_isTesting; }
    COLORREF GetCurrentColor() const { return m_currentColor; }
    int GetDisconnectedStyle() const { return m_disconnectedStyle; }

private:
    HINSTANCE m_hInstance;
    std::vector<OverlayWindowInfo> m_overlays;
    bool m_keyboardConnected;
    COLORREF m_currentColor;
    bool m_shouldShow;
    bool m_isTesting;
    int m_thickness;
    int m_disconnectedStyle; // 0: 상단 LED 바, 1: 작업표시줄 전체 틴트

    void SyncTrayOverlays();
    void RenderLayeredOverlay(HWND hOverlay, int x, int y, int w, int h, COLORREF color, bool isHorizontal, bool isFullTint);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
