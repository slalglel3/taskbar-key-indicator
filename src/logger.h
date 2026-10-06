#pragma once

#include <windows.h>
#include <string>

class Logger {
public:
    static void Init();
    static void Log(const wchar_t* format, ...);
    static void LogA(const char* format, ...);
    static std::wstring GetLogPath();
    static void OpenLogFile();
    static void Close();
};
