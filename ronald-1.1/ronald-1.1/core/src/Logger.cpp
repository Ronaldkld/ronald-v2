#include "Logger.h"

#include <windows.h>
#include <fstream>
#include <mutex>

namespace hexcore {

namespace {

std::mutex g_logMutex;

std::wstring GetLogFilePath() {
    // Prefer the folder the DLL lives in (portable installs); fall back
    // to %LOCALAPPDATA%\HexEditor when that folder is not writable
    // (e.g. Program Files without elevation).
    wchar_t modulePath[MAX_PATH] = {0};
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetLogFilePath),
        &hSelf);
    if (hSelf && GetModuleFileNameW(hSelf, modulePath, MAX_PATH) > 0) {
        std::wstring path(modulePath);
        size_t slash = path.find_last_of(L"\\/");
        std::wstring dir = (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
        std::wstring candidate = dir + L"\\hexcore_errors.log";

        HANDLE hTest = CreateFileW(candidate.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hTest != INVALID_HANDLE_VALUE) {
            CloseHandle(hTest);
            return candidate;
        }
    }

    wchar_t localAppData[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(localAppData) + L"\\HexEditor" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\hexcore_errors.log";
}

} // namespace

void Logger::LogError(const std::wstring& context, const std::wstring& detail) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timestamp[64];
    swprintf(timestamp, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wofstream file(GetLogFilePath().c_str(), std::ios::app);
    if (!file.is_open()) {
        return; // Logging must never crash the app; silently give up.
    }
    file << L"[" << timestamp << L"] " << context << L": " << detail << std::endl;
}

} // namespace hexcore
