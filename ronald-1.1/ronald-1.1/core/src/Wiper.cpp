#include "Wiper.h"

#include <algorithm>
#include <cstring>

namespace hexcore {

// ---------------------------------------------------------------------------
// PRNG — xor-shift64, seeded lazily from tick count + address entropy.
// Not cryptographic, but more than sufficient for forensic-grade overwrite:
// the goal is that any recovered bit pattern looks like noise, not the
// original file content.
// ---------------------------------------------------------------------------
uint64_t Wiper::s_rng = 0;

void Wiper::FillRandom(uint8_t* buf, size_t len) {
    if (s_rng == 0) {
        s_rng = GetTickCount64() ^ reinterpret_cast<uintptr_t>(buf) ^ 0xDEADBEEFCAFEBABEull;
        if (s_rng == 0) s_rng = 1;
    }
    for (size_t i = 0; i < len; ++i) {
        s_rng ^= s_rng << 13;
        s_rng ^= s_rng >> 7;
        s_rng ^= s_rng << 17;
        buf[i] = static_cast<uint8_t>(s_rng & 0xFF);
    }
}

void Wiper::FillZero(uint8_t* buf, size_t len) {
    std::memset(buf, 0, len);
}

// ---------------------------------------------------------------------------
// WipeFile
// ---------------------------------------------------------------------------
bool Wiper::WipeFile(const std::wstring& path, int passes) {
    if (path.empty() || passes < 1) return false;

    HANDLE h = CreateFileW(path.c_str(),
                            GENERIC_READ | GENERIC_WRITE,
                            0,          // no sharing — exclusive access
                            nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_WRITE_THROUGH,    // bypass write cache
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // File might already be gone — treat as success.
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(h, &fileSize) || fileSize.QuadPart == 0) {
        // Zero-byte file — nothing to overwrite, just close and delete.
        CloseHandle(h);
        DeleteFileW(path.c_str());
        return true;
    }

    constexpr DWORD kChunk = 64 * 1024; // 64 KB chunks
    uint8_t buf[kChunk];

    for (int pass = 0; pass < passes; ++pass) {
        // Seek to beginning.
        LARGE_INTEGER zero{};
        SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);

        uint64_t remaining = static_cast<uint64_t>(fileSize.QuadPart);
        bool passOk = true;

        while (remaining > 0) {
            DWORD toWrite = static_cast<DWORD>(std::min<uint64_t>(remaining, kChunk));
            // pass 0 → random, pass 1 → zeros, pass 2+ → random
            if (pass == 1)
                FillZero(buf, toWrite);
            else
                FillRandom(buf, toWrite);

            DWORD written = 0;
            if (!WriteFile(h, buf, toWrite, &written, nullptr) || written != toWrite) {
                passOk = false;
                break;
            }
            remaining -= written;
        }

        FlushFileBuffers(h);
        if (!passOk) break;
    }

    // Truncate to zero (makes directory entry show 0 bytes immediately).
    LARGE_INTEGER zero{};
    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    SetEndOfFile(h);

    CloseHandle(h);
    DeleteFileW(path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// WipeEditorTemps
// ---------------------------------------------------------------------------
int Wiper::WipeEditorTemps(const std::wstring& dir, int passes) {
    if (dir.empty()) return 0;

    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/')
        pattern += L'\\';
    pattern += L"hxe*.TMP";

    WIN32_FIND_DATAW wfd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &wfd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/')
        base += L'\\';

    int count = 0;
    do {
        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring full = base + wfd.cFileName;
        if (WipeFile(full, passes)) ++count;
    } while (FindNextFileW(hFind, &wfd));

    FindClose(hFind);
    return count;
}

// ---------------------------------------------------------------------------
// WipeFreeSpace
//
// Strategy: create one large fill file in `driveRoot`, write zeros in 1 MB
// chunks until ERROR_DISK_FULL (expected — we intentionally fill the disk),
// flush, close, delete.  Every cluster that was unallocated is now zero.
// ---------------------------------------------------------------------------
int64_t Wiper::WipeFreeSpace(const std::wstring& driveRoot, HWND /*hwndOwner*/) {
    // Build fill-file path.
    std::wstring fillPath = driveRoot;
    if (!fillPath.empty() && fillPath.back() != L'\\' && fillPath.back() != L'/')
        fillPath += L'\\';
    fillPath += L"~hxewipe_fill.tmp";

    // How much free space is there?
    ULARGE_INTEGER freeBytesAvail{}, totalBytes{}, totalFree{};
    if (!GetDiskFreeSpaceExW(driveRoot.c_str(), &freeBytesAvail, &totalBytes, &totalFree))
        return -1;

    if (freeBytesAvail.QuadPart == 0) return 0; // nothing to do

    HANDLE h = CreateFileW(fillPath.c_str(),
                            GENERIC_WRITE,
                            0,
                            nullptr,
                            CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                            nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;

    constexpr DWORD kChunk = 1024 * 1024; // 1 MB
    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk,
                                                        MEM_COMMIT | MEM_RESERVE,
                                                        PAGE_READWRITE));
    if (!buf) {
        CloseHandle(h);
        DeleteFileW(fillPath.c_str());
        return -1;
    }
    std::memset(buf, 0, kChunk);

    int64_t totalWritten = 0;
    bool done = false;

    while (!done) {
        DWORD toWrite = kChunk;
        DWORD written = 0;
        BOOL ok = WriteFile(h, buf, toWrite, &written, nullptr);
        if (written > 0) totalWritten += written;

        if (!ok) {
            DWORD err = GetLastError();
            // ERROR_DISK_FULL is the expected end condition.
            if (err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL) {
                done = true;
            } else {
                // Real error — stop early but still clean up.
                done = true;
            }
        } else if (written < toWrite) {
            // Partial write also means disk is now full.
            done = true;
        }
    }

    FlushFileBuffers(h);
    CloseHandle(h);
    VirtualFree(buf, 0, MEM_RELEASE);
    DeleteFileW(fillPath.c_str());

    return totalWritten;
}

} // namespace hexcore
