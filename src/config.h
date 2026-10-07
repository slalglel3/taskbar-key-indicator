#pragma once

#include <windows.h>
#include <string>

struct AppConfig {
    COLORREF disconnectedColor; // 키보드 모바일 전환 시 LED 색상 (기본: 네온 레드)
    COLORREF connectedColor;    // 키보드 PC 연결 시 LED 색상 (기본: 네온 그린)
    bool showWhenConnected;     // PC 연결 시에도 LED 표시할지 여부 (기본: false - 평상시 숨김)
    int barThickness;           // LED 바 두께 (픽셀, 기본: 3)
    bool autoStart;             // 윈도우 시작 시 자동 실행 여부
};

class ConfigManager {
public:
    static std::wstring GetConfigPath();
    static AppConfig LoadConfig();
    static void SaveConfig(const AppConfig& cfg);
    static bool IsAutoStartEnabled();
    static void SetAutoStart(bool enable);
    static COLORREF ParseColor(const std::wstring& hexOrRgb, COLORREF defaultColor);
    static std::wstring ColorToHex(COLORREF color);
};
