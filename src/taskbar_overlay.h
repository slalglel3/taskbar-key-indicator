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
    void Cleanup();

private:
    HINSTANCE m_hInstance;
    std::vector<OverlayWindowInfo> m_overlays;
    bool m_keyboardConnected;
    COLORREF m_currentColor;
    bool m_shouldShow;
    int m_thickness;

    void CreateOrUpdateOverlays();
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static void DrawNeonLedBar(HDC hdc, int width, int height, COLORREF baseColor, bool isHorizontal);
};
