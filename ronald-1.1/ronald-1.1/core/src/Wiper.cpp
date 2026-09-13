#include "Wiper.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <vector>

namespace hexcore {

// ---------------------------------------------------------------------------
// PRNG — xor-shift64, seeded lazily from tick count + address entropy.
// Not cryptographic, but more than sufficient for forensic-grade overwrite:
// the goal is that any recovered bit pattern looks like noise, not the
// original file content.
// ---------------------------------------------------------------------------
uint64_t Wiper::s_rng = 0;
std::atomic<bool>    Wiper::s_cancelRequested{false};
std::atomic<int64_t> Wiper::s_bytesWritten{0};

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
namespace {
std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) c = static_cast<wchar_t>(std::towlower(c));
    return r;
}
} // namespace

bool Wiper::IsOrphanTempName(const std::wstring& name) {
    std::wstring lower = ToLowerCopy(name);
    if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".tmp") != 0) return false;
    // This editor's own safe-save temp file (HexDocument::DoSaveTo).
    if (lower.compare(0, 3, L"hxe") == 0) return true;
    // The "<name>~RFxxxxxxxx.TMP" backup Win32's ReplaceFile can leave
    // behind on FAT/FAT32/exFAT volumes, which lack the atomic replace
    // support NTFS has.
    if (lower.find(L"~rf") != std::wstring::npos) return true;
    return false;
}

int Wiper::WipeEditorTempsRecursive(const std::wstring& dir, int passes) {
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';

    WIN32_FIND_DATAW wfd{};
    HANDLE hFind = FindFirstFileW((base + L"*").c_str(), &wfd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        std::wstring name = wfd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = base + name;

        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Don't follow reparse points (junctions/symlinks) — avoids
            // loops and straying outside the folder the user picked.
            if (!(wfd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                count += WipeEditorTempsRecursive(full, passes);
            }
        } else if (IsOrphanTempName(name)) {
            if (WipeFile(full, passes)) ++count;
        }
    } while (FindNextFileW(hFind, &wfd));

    FindClose(hFind);
    return count;
}

int Wiper::WipeEditorTemps(const std::wstring& dir, int passes) {
    if (dir.empty()) return 0;
    return WipeEditorTempsRecursive(dir, passes);
}

// ---------------------------------------------------------------------------
// WipeFreeSpace
//
// Strategy: create a series of fill files in `driveRoot` and write zeros
// into them until the safety margin is reached, cancellation is requested,
// or the disk is actually full, then flush, close and delete them all.
// Every unallocated cluster written to is now zero. Multiple files are
// used (rather than one huge one) because a single file is capped well
// under the volume's free space on FAT-family filesystems — FAT32 refuses
// any one file past just under 4 GiB, so one giant fill file silently
// stops there, leaving most of a larger drive's free space (and whatever
// deleted-file remnants live in it) untouched.
//
// Intended to be called from a worker thread — it can take a long time on
// a large or mostly-empty volume, and must not block a UI thread.
// ---------------------------------------------------------------------------
void Wiper::RequestCancel() {
    s_cancelRequested = true;
}

int64_t Wiper::GetBytesWrittenSoFar() {
    return s_bytesWritten.load();
}

int64_t Wiper::WipeFreeSpace(const std::wstring& driveRoot, HWND /*hwndOwner*/) {
    s_cancelRequested = false;
    s_bytesWritten = 0;

    std::wstring base = driveRoot;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';

    // How much free space is there?
    ULARGE_INTEGER freeBytesAvail{}, totalBytes{}, totalFree{};
    if (!GetDiskFreeSpaceExW(driveRoot.c_str(), &freeBytesAvail, &totalBytes, &totalFree))
        return -1;

    if (freeBytesAvail.QuadPart == 0) return 0; // nothing to do

    // Never try to reach exactly 0 bytes free: on a live system drive
    // that can make unrelated things (paging, other apps, even this one)
    // fail outright while the fill files still occupy the space. Stop a
    // bit short instead.
    constexpr uint64_t kSafetyMargin = 32ull * 1024 * 1024; // 32 MB
    uint64_t target = (freeBytesAvail.QuadPart > kSafetyMargin)
                           ? freeBytesAvail.QuadPart - kSafetyMargin
                           : freeBytesAvail.QuadPart;
    if (target == 0) return 0;

    // Comfortably under FAT32's ~4 GiB-1 per-file cap (and FAT16's ~2
    // GiB one) so a single fill file never hits that ceiling early;
    // harmless overhead of a few extra file creations on NTFS/exFAT.
    constexpr uint64_t kPerFileCap = 3ull * 1024 * 1024 * 1024; // 3 GiB
    // No FILE_FLAG_WRITE_THROUGH here: forcing every 1 MB write straight
    // to disk (as the previous version did) serializes on physical I/O
    // and is dramatically slower, especially over USB. A single flush
    // per fill file is enough to guarantee the zeros actually reach
    // disk before it's deleted.
    constexpr DWORD kChunk = 8 * 1024 * 1024; // 8 MB

    uint8_t* buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, kChunk,
                                                        MEM_COMMIT | MEM_RESERVE,
                                                        PAGE_READWRITE));
    if (!buf) return -1;
    std::memset(buf, 0, kChunk);

    std::vector<std::wstring> fillPaths;
    int64_t totalWritten = 0;
    bool stopAll = false;
    int fileIndex = 0;

    while (!stopAll && static_cast<uint64_t>(totalWritten) < target && !s_cancelRequested.load()) {
        wchar_t suffix[16];
        swprintf(suffix, 16, L"%04d", fileIndex++);
        std::wstring fillPath = base + L"~hxewipe_fill" + suffix + L".tmp";

        HANDLE h = CreateFileW(fillPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) break; // e.g. permission denied
        fillPaths.push_back(fillPath);

        uint64_t writtenThisFile = 0;
        bool fileDone = false;

        while (!fileDone) {
            if (s_cancelRequested.load()) { stopAll = true; break; }

            uint64_t remainingToTarget = target - static_cast<uint64_t>(totalWritten);
            uint64_t remainingToFileCap = kPerFileCap - writtenThisFile;
            DWORD toWrite = kChunk;
            if (remainingToTarget < toWrite) toWrite = static_cast<DWORD>(remainingToTarget);
            if (remainingToFileCap < toWrite) toWrite = static_cast<DWORD>(remainingToFileCap);
            if (toWrite == 0) break; // hit this file's cap exactly - move to the next one

            DWORD written = 0;
            BOOL ok = WriteFile(h, buf, toWrite, &written, nullptr);
            if (written > 0) {
                totalWritten += written;
                writtenThisFile += written;
                s_bytesWritten = totalWritten;
            }

            if (!ok || written < toWrite) {
                // Real disk-full (the volume was fuller than
                // GetDiskFreeSpaceEx reported) or a genuine I/O error —
                // either way, this is as far as we can go.
                stopAll = true;
                break;
            }
        }

        FlushFileBuffers(h);
        CloseHandle(h);
    }

    VirtualFree(buf, 0, MEM_RELEASE);
    for (const auto& p : fillPaths) DeleteFileW(p.c_str());

    if (fillPaths.empty()) return -1; // never managed to create even one fill file
    return totalWritten;
}

} // namespace hexcore
