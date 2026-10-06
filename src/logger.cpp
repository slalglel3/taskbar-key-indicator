#include "logger.h"
#include <shlwapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>

static const wchar_t* LOG_FILENAME = L"debug.log";
static CRITICAL_SECTION s_cs;
static bool s_initialized = false;
static FILE* s_fp = nullptr;
static long s_bytesWritten = 0;
static const long MAX_LOG_SIZE = 1024 * 1024; // 1MB 제한으로 VDI 디스크 보호

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

        std::wstring logPath = GetLogPath();
        s_fp = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");
        if (s_fp) {
            fwprintf(s_fp, L"=====================================================\n");
            fwprintf(s_fp, L" Taskbar Key Indicator Debug Log (Optimized)\n");
            time_t now = time(nullptr);
            tm tmNow;
            localtime_s(&tmNow, &now);
            wchar_t timeBuf[64];
            wcsftime(timeBuf, 64, L"%Y-%m-%d %H:%M:%S", &tmNow);
            fwprintf(s_fp, L" Started at: %s\n", timeBuf);
            fwprintf(s_fp, L"=====================================================\n\n");
            fflush(s_fp);
            s_bytesWritten = 200;
        }
    }
}

void Logger::Log(const wchar_t* format, ...) {
    if (!s_initialized) Init();

    EnterCriticalSection(&s_cs);

    if (s_fp) {
        // 파일 크기가 1MB를 초과하면 무제한 비대화 방지
        if (s_bytesWritten > MAX_LOG_SIZE) {
            fclose(s_fp);
            std::wstring logPath = GetLogPath();
            s_fp = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");
            s_bytesWritten = 0;
            if (s_fp) {
                fwprintf(s_fp, L"--- Log wrapped (size limit 1MB reached) ---\n");
            }
        }

        if (s_fp) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            int n1 = fwprintf(s_fp, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

            va_list args;
            va_start(args, format);
            int n2 = vfwprintf(s_fp, format, args);
            va_end(args);

            fwprintf(s_fp, L"\n");
            fflush(s_fp);

            if (n1 > 0 && n2 > 0) {
                s_bytesWritten += (n1 + n2 + 2) * sizeof(wchar_t);
            }
        }
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
    EnterCriticalSection(&s_cs);
    if (s_fp) {
        fflush(s_fp);
    }
    LeaveCriticalSection(&s_cs);

    std::wstring logPath = GetLogPath();
    ShellExecuteW(NULL, L"open", logPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
}

void Logger::Close() {
    if (s_initialized) {
        EnterCriticalSection(&s_cs);
        if (s_fp) {
            fwprintf(s_fp, L"[App] Log closed cleanly.\n");
            fclose(s_fp);
            s_fp = nullptr;
        }
        LeaveCriticalSection(&s_cs);
        DeleteCriticalSection(&s_cs);
        s_initialized = false;
    }
}
