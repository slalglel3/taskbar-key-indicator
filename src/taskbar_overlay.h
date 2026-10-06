#pragma once

#include <windows.h>
#include <vector>

struct OverlayWindowInfo {
    HWND hOverlay;
    HWND hTargetTray;
    bool isPrimary;
};

class TaskbarOverlayManager {
public:
    TaskbarOverlayManager();
    ~TaskbarOverlayManager();

    bool Initialize(HINSTANCE hInstance);
    void UpdatePositions();
    void SetState(bool keyboardConnected, COLORREF disconnectedColor, COLORREF connectedColor, bool showWhenConnected, int thickness);
    void SetThickness(int thickness);
    void SetColor(COLORREF color);
    void ForceShowTest(int durationMs = 5000);
    void Cleanup();

    bool IsOverlayVisible() const { return m_shouldShow || m_isTesting; }
    COLORREF GetCurrentColor() const { return m_currentColor; }

private:
    HINSTANCE m_hInstance;
    std::vector<OverlayWindowInfo> m_overlays;
    bool m_keyboardConnected;
    COLORREF m_currentColor;
    bool m_shouldShow;
    bool m_isTesting;
    int m_thickness;

    void CreateOrUpdateOverlays();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void DrawNeonLedBar(HDC hdc, int width, int height, COLORREF baseColor, bool isHorizontal);
};
