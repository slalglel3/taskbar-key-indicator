#include "logger.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>

static const wchar_t* LOG_FILENAME = L"debug.log";
static CRITICAL_SECTION s_cs;
static bool s_initialized = false;

std::wstring Logger::GetLogPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    PathAppendW(path, LOG_FILENAME);
    return std::wstring(path);
}

void Logger::Init() {
    if (!s_initialized) {
        InitializeCriticalSection(&s_cs);
        s_initialized = true;

        // 세션 시작 시 기존 로그 덮어쓰기/구분
        std::wstring logPath = GetLogPath();
        FILE* fp = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");
        if (fp) {
            fwprintf(fp, L"=====================================================\n");
            fwprintf(fp, L" Taskbar Key Indicator Debug Log\n");
            time_t now = time(nullptr);
            tm tmNow;
            localtime_s(&tmNow, &now);
            wchar_t timeBuf[64];
            wcsftime(timeBuf, 64, L"%Y-%m-%d %H:%M:%S", &tmNow);
            fwprintf(fp, L" Started at: %s\n", timeBuf);
            fwprintf(fp, L"=====================================================\n\n");
            fclose(fp);
        }
    }
}

void Logger::Log(const wchar_t* format, ...) {
    if (!s_initialized) Init();

    EnterCriticalSection(&s_cs);

    std::wstring logPath = GetLogPath();
    FILE* fp = _wfopen(logPath.c_str(), L"a, ccs=UTF-8");
    if (fp) {
        // 타임스탬프
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(fp, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        va_list args;
        va_start(args, format);
        vfwprintf(fp, format, args);
        va_end(args);

        fwprintf(fp, L"\n");
        fflush(fp);
        fclose(fp);
    }

    LeaveCriticalSection(&s_cs);
}

void Logger::LogA(const char* format, ...) {
    char buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    wchar_t wbuf[1024];
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 1024);
    Log(L"%s", wbuf);
}

void Logger::OpenLogFile() {
    std::wstring logPath = GetLogPath();
    ShellExecuteW(NULL, L"open", logPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
}
