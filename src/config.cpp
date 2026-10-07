#include "config.h"
#include <shlwapi.h>
#include <sstream>
#include <iomanip>

static const wchar_t* CONFIG_FILENAME = L"config.ini";
static const wchar_t* SECTION_GENERAL = L"Settings";
static const wchar_t* RUN_KEY_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* APP_REG_NAME = L"TaskbarKeyIndicator";

std::wstring ConfigManager::GetConfigPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, CONFIG_FILENAME);
    return std::wstring(path);
}

COLORREF ConfigManager::ParseColor(const std::wstring& hexStr, COLORREF defaultColor) {
    if (hexStr.empty()) return defaultColor;
    std::wstring s = hexStr;
    if (s[0] == L'#') s = s.substr(1);
    if (s.length() != 6) return defaultColor;

    try {
        unsigned long val = std::stoul(s, nullptr, 16);
        BYTE r = (BYTE)((val >> 16) & 0xFF);
        BYTE g = (BYTE)((val >> 8) & 0xFF);
        BYTE b = (BYTE)(val & 0xFF);
        return RGB(r, g, b);
    } catch (...) {
        return defaultColor;
    }
}

std::wstring ConfigManager::ColorToHex(COLORREF color) {
    std::wstringstream ss;
    ss << L"#"
       << std::hex << std::uppercase << std::setfill(L'0')
       << std::setw(2) << (int)GetRValue(color)
       << std::setw(2) << (int)GetGValue(color)
       << std::setw(2) << (int)GetBValue(color);
    return ss.str();
}

AppConfig ConfigManager::LoadConfig() {
    AppConfig cfg;
    // 기본값 설정
    cfg.disconnectedColor = RGB(255, 45, 85); // 세련된 네온 레드/코랄 (#FF2D55)
    cfg.connectedColor = RGB(50, 215, 75);     // 네온 그린 (#32D74B)
    cfg.showWhenConnected = false;             // 유선 연결 시에는 평상시 작업표시줄 유지 (숨김)
    cfg.barThickness = 3;                      // 3px
    cfg.disconnectedStyle = 1;                 // 1: 작업표시줄 전체 틴트 덮기 (시인성 극대화 기본 권장)
    cfg.autoStart = IsAutoStartEnabled();

    std::wstring iniPath = GetConfigPath();
    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // 최초 실행 시 기본 설정 파일 생성
        SaveConfig(cfg);
        return cfg;
    }

    wchar_t buf[256];

    // DisconnectedColor
    GetPrivateProfileStringW(SECTION_GENERAL, L"DisconnectedColor", L"#FF2D55", buf, 256, iniPath.c_str());
    cfg.disconnectedColor = ParseColor(buf, cfg.disconnectedColor);

    // ConnectedColor
    GetPrivateProfileStringW(SECTION_GENERAL, L"ConnectedColor", L"#32D74B", buf, 256, iniPath.c_str());
    cfg.connectedColor = ParseColor(buf, cfg.connectedColor);

    // ShowWhenConnected
    cfg.showWhenConnected = (GetPrivateProfileIntW(SECTION_GENERAL, L"ShowWhenConnected", 0, iniPath.c_str()) != 0);

    // BarThickness
    int th = GetPrivateProfileIntW(SECTION_GENERAL, L"BarThickness", 3, iniPath.c_str());
    if (th < 1) th = 1;
    if (th > 10) th = 10;
    cfg.barThickness = th;

    // DisconnectedStyle (0: 상단 LED 바, 1: 작업표시줄 전체 틴트)
    cfg.disconnectedStyle = GetPrivateProfileIntW(SECTION_GENERAL, L"DisconnectedStyle", 1, iniPath.c_str());
    if (cfg.disconnectedStyle != 0 && cfg.disconnectedStyle != 1) {
        cfg.disconnectedStyle = 1;
    }

    return cfg;
}

void ConfigManager::SaveConfig(const AppConfig& cfg) {
    std::wstring iniPath = GetConfigPath();

    WritePrivateProfileStringW(SECTION_GENERAL, L"DisconnectedColor", ColorToHex(cfg.disconnectedColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(SECTION_GENERAL, L"ConnectedColor", ColorToHex(cfg.connectedColor).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(SECTION_GENERAL, L"ShowWhenConnected", cfg.showWhenConnected ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(SECTION_GENERAL, L"BarThickness", std::to_wstring(cfg.barThickness).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(SECTION_GENERAL, L"DisconnectedStyle", std::to_wstring(cfg.disconnectedStyle).c_str(), iniPath.c_str());
}

bool ConfigManager::IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH];
        DWORD valSize = sizeof(value);
        LONG res = RegQueryValueExW(hKey, APP_REG_NAME, NULL, NULL, (LPBYTE)value, &valSize);
        RegCloseKey(hKey);
        return (res == ERROR_SUCCESS);
    }
    return false;
}

void ConfigManager::SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY_PATH, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
            RegSetValueExW(hKey, APP_REG_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.length() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, APP_REG_NAME);
        }
        RegCloseKey(hKey);
    }
}
