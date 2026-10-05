#include "BridgeLog.h"
#include <Windows.h>
#include <strsafe.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cwchar>

namespace arvr {
static HANDLE gFile = INVALID_HANDLE_VALUE;
static SRWLOCK gLock = SRWLOCK_INIT;

static void Init(){
    if(gFile != INVALID_HANDLE_VALUE) return;
    wchar_t path[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if(slash) *(slash + 1) = L'\0';
    if(FAILED(StringCchCatW(path, MAX_PATH, L"AdventRisingVRBridge.log"))) return;
    gFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ|FILE_SHARE_WRITE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void BridgeLog(const char* fmt, ...){
    AcquireSRWLockExclusive(&gLock);
    Init();
    if(gFile == INVALID_HANDLE_VALUE){ ReleaseSRWLockExclusive(&gLock); return; }
    char msg[2048]{};
    SYSTEMTIME st{}; GetLocalTime(&st);
    int n = _snprintf_s(msg, sizeof(msg), _TRUNCATE, "[%02u:%02u:%02u.%03u] ",
                        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if(n < 0) n = 0;
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(msg+n, sizeof(msg)-static_cast<size_t>(n), _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t len = strnlen_s(msg, sizeof(msg));
    if(len < sizeof(msg)-2){ msg[len++]='\r'; msg[len++]='\n'; }
    DWORD written{}; WriteFile(gFile, msg, static_cast<DWORD>(len), &written, nullptr); FlushFileBuffers(gFile);
    OutputDebugStringA(msg);
    ReleaseSRWLockExclusive(&gLock);
}
}
