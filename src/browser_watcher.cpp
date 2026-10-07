#include "browser_watcher.h"
#include "logger.h"

BrowserWatcher* BrowserWatcher::s_instance = nullptr;

BrowserWatcher::BrowserWatcher()
    : m_hHook(NULL)
    , m_isConnected(true)
    , m_hasState(false)
{
    s_instance = this;
}

BrowserWatcher::~BrowserWatcher() {
    Cleanup();
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool BrowserWatcher::SafeGetWindowTitle(HWND hwnd, wchar_t* buf, int maxLen) {
    if (!hwnd || !buf || maxLen <= 0) return false;
    buf[0] = L'\0';

    // 대상 윈도우 프로세스가 Hang(응답 없음) 상태인 경우 프로세스 교착을 원천 차단하기 위해
    // SMTO_ABORTIFHUNG + 50ms 타임아웃 적용
    DWORD_PTR res = 0;
    LRESULT lr = SendMessageTimeoutW(
        hwnd,
        WM_GETTEXT,
        (WPARAM)maxLen,
        (LPARAM)buf,
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        50,
        &res
    );

    if (lr != 0 && res > 0) {
        return true;
    }

    // 대상 프로세스가 아닌 가상 데스크톱/특수 창 대비 GetWindowTextW 보조 조회
    int len = GetWindowTextW(hwnd, buf, maxLen);
    return (len > 0);
}

bool BrowserWatcher::Initialize() {
    // Windows OS 레벨 창 이름 변경 이벤트(EVENT_OBJECT_NAMECHANGE) 훅 등록 (CPU 부하 0.00%)
    m_hHook = SetWinEventHook(
        EVENT_OBJECT_NAMECHANGE,
        EVENT_OBJECT_NAMECHANGE,
        NULL,
        WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
    );

    if (m_hHook) {
        Logger::Log(L"[BrowserWatcher] WinEventHook registered successfully (EVENT_OBJECT_NAMECHANGE)");
    } else {
        Logger::Log(L"[BrowserWatcher] Failed to register WinEventHook! Error: %lu", GetLastError());
    }

    // 기동 즉시 현재 열려 있는 브라우저 창에서 초기 상태 획득
    CheckCurrentState();
    return (m_hHook != NULL);
}

void BrowserWatcher::Cleanup() {
    if (m_hHook) {
        UnhookWinEvent(m_hHook);
        m_hHook = NULL;
        Logger::Log(L"[BrowserWatcher] WinEventHook unhooked cleanly.");
    }
}

void BrowserWatcher::SetStateCallback(std::function<void(bool, const std::wstring&)> callback) {
    m_callback = callback;
}

BOOL CALLBACK BrowserWatcher::EnumWindowsInitProc(HWND hwnd, LPARAM lParam) {
    BrowserWatcher* self = (BrowserWatcher*)lParam;
    wchar_t buf[512] = { 0 };
    if (SafeGetWindowTitle(hwnd, buf, 512)) {
        std::wstring title(buf);
        if (title.find(L"Donagy") != std::wstring::npos) {
            if (title.find(L"[MB]") != std::wstring::npos || title.find(L"[PC]") != std::wstring::npos) {
                self->HandleTitleChange(hwnd);
                return FALSE; // 최초 일치 창 발견 시 열거 종료
            }
        }
    }
    return TRUE;
}

bool BrowserWatcher::CheckCurrentState() {
    EnumWindows(EnumWindowsInitProc, (LPARAM)this);
    return m_hasState;
}

void CALLBACK BrowserWatcher::WinEventProc(
    HWINEVENTHOOK hWinEventHook,
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD idEventThread,
    DWORD dwmsEventTime
) {
    if (event == EVENT_OBJECT_NAMECHANGE && idObject == OBJID_WINDOW && idChild == CHILDID_SELF) {
        if (s_instance && hwnd) {
            s_instance->HandleTitleChange(hwnd);
        }
    }
}

void BrowserWatcher::HandleTitleChange(HWND hwnd) {
    wchar_t buf[512] = { 0 };
    if (!SafeGetWindowTitle(hwnd, buf, 512)) {
        return;
    }

    std::wstring title(buf);

    // Donagy 웹 콘솔 탭/창인지 식별
    if (title.find(L"Donagy") != std::wstring::npos) {
        bool isMb = (title.find(L"[MB]") != std::wstring::npos);
        bool isPc = (title.find(L"[PC]") != std::wstring::npos);

        if (isMb || isPc) {
            bool connected = isPc; // [PC]면 유선 활성(true), [MB]면 모바일 전환(false)
            m_matchedTitle = title;

            // 이미 동일 상태라면 불필요한 재렌더링 방지
            if (m_hasState && m_isConnected == connected) {
                return;
            }

            m_isConnected = connected;
            m_hasState = true;

            Logger::Log(L"============================================================");
            Logger::Log(L"[BrowserWatcher] 0ms Title Event Captured!");
            Logger::Log(L"  - Browser Title: '%s'", title.c_str());
            Logger::Log(L"  - Key State    : %s", connected ? L"● PC CONNECTED (Green LED)" : L"○ MOBILE SWITCHED (Red LED)");
            Logger::Log(L"============================================================");

            if (m_callback) {
                m_callback(connected, title);
            }
        }
    }
}
