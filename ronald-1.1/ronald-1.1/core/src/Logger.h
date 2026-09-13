// Minimal error-only file logger.
//
// By design this never writes anything for normal, successful operations
// (open/read/write/save/search) - only for genuine error conditions, so
// the log stays small and useful for debugging instead of noisy.
#pragma once

#include <string>

namespace hexcore {

class Logger {
public:
    // Logs one line with a timestamp. The log file is created lazily on
    // the first call (next to the DLL, or %LOCALAPPDATA% as a fallback)
    // so a session with zero errors produces zero log files.
    static void LogError(const std::wstring& context, const std::wstring& detail);
};

} // namespace hexcore
