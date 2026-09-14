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
std::atomic<bool>     Wiper::s_cancelRequested{false};
std::atomic<int64_t>  Wiper::s_bytesWritten{0};
std::atomic<int>      Wiper::s_tempsFilesWiped{0};
std::atomic<int>      Wiper::s_tempsFoldersDone{0};
std::atomic<int>      Wiper::s_dirEntriesWiped{0};
std::atomic<int>      Wiper::s_dirsUnresolved{0};
std::wstring          Wiper::s_firstUnresolvedDir;
std::wstring          Wiper::s_rootEntriesDump;
std::atomic<uint64_t> Wiper::s_deadlineTick{0};
Wiper::FatWipeStatus  Wiper::s_fatWipeStatus{Wiper::FatWipeStatus::NotAttempted};

bool Wiper::ShouldStop() {
    if (s_cancelRequested.load()) return true;
    uint64_t deadline = s_deadlineTick.load();
    return deadline != 0 && GetTickCount64() >= deadline;
}

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

namespace {
// Splits `fullPath` into path components relative to `rootPrefix` (e.g.
// "D:\root\DocumentosVarios" relative to "D:\" -> {"root",
// "DocumentosVarios"}), for FatVolume::ResolveDirectoryCluster.
std::vector<std::wstring> SplitRelativeToRoot(const std::wstring& fullPath,
                                               const std::wstring& rootPrefix) {
    std::wstring rel = fullPath;
    if (rel.size() >= rootPrefix.size() &&
        _wcsnicmp(rel.c_str(), rootPrefix.c_str(), rootPrefix.size()) == 0) {
        rel = rel.substr(rootPrefix.size());
    }
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start < rel.size()) {
        size_t slash = rel.find_first_of(L"\\/", start);
        std::wstring part = (slash == std::wstring::npos) ? rel.substr(start) : rel.substr(start, slash - start);
        if (!part.empty()) parts.push_back(part);
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return parts;
}
} // namespace

int Wiper::WipeEditorTempsRecursive(const std::wstring& dir, int passes) {
    if (ShouldStop()) return 0;

    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';

    WIN32_FIND_DATAW wfd{};
    HANDLE hFind = FindFirstFileW((base + L"*").c_str(), &wfd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int count = 0;
    do {
        if (ShouldStop()) break;

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
            if (WipeFile(full, passes)) {
                ++count;
                s_tempsFilesWiped = s_tempsFilesWiped.load() + 1;
            }
        }
    } while (FindNextFileW(hFind, &wfd));

    FindClose(hFind);

    s_tempsFoldersDone = s_tempsFoldersDone.load() + 1;

    return count;
}

int Wiper::WipeEditorTemps(const std::wstring& dir, int passes) {
    if (dir.empty()) return 0;
    s_cancelRequested = false;
    s_tempsFilesWiped = 0;
    s_tempsFoldersDone = 0;
    s_dirEntriesWiped = 0;
    s_dirsUnresolved = 0;
    s_firstUnresolvedDir.clear();
    s_rootEntriesDump.clear();
    s_fatWipeStatus = FatWipeStatus::NotAttempted;

    constexpr uint64_t kGlobalBudgetMs = 45000; // hard cap so a folder-heavy drive still finishes fast
    s_deadlineTick = GetTickCount64() + kGlobalBudgetMs;

    // Phase 1: walk the tree using ordinary file APIs (find + wipe
    // orphaned temp files). This must finish - and every handle it used
    // must close - before Phase 2 locks the volume: FSCTL_LOCK_VOLUME
    // grants OUR raw handle exclusive access, which blocks any ordinary
    // file-level I/O (FindFirstFileW, CreateFileW) against that same
    // volume, including our own. Locking first and walking second, tried
    // initially, made the very first FindFirstFileW call fail
    // immediately - visiting zero folders and wiping nothing, silently.
    int result = WipeEditorTempsRecursive(dir, passes);

    // Phase 2: only for a FAT32 target with Administrator rights and an
    // exclusively lockable volume, resolve the ONE folder the user picked
    // and then recursively zero out stale directory-entry content at the
    // raw volume level, walking the FAT32 cluster tree natively from
    // there (WipeStaleEntriesRecursive) rather than re-matching a path
    // string for every folder Phase 1 visited.
    wchar_t volRoot[MAX_PATH] = {0};
    if (!ShouldStop() && GetVolumePathNameW(dir.c_str(), volRoot, MAX_PATH)) {
        wchar_t fsName[32] = {0};
        if (GetVolumeInformationW(volRoot, nullptr, 0, nullptr, nullptr, nullptr, fsName, 32) &&
            _wcsicmp(fsName, L"FAT32") == 0) {
            std::wstring devicePath = L"\\\\.\\";
            devicePath += volRoot[0];
            devicePath += L':';

            FatVolume fatVol;
            if (fatVol.Open(devicePath, true)) {
                DWORD dummy = 0;
                bool locked = DeviceIoControl(fatVol.RawHandle(), FSCTL_LOCK_VOLUME, nullptr, 0,
                                               nullptr, 0, &dummy, nullptr) != 0;
                if (locked) {
                    s_fatWipeStatus = FatWipeStatus::Ran;

                    // Resolve the ONE folder the user picked (not every
                    // folder Phase 1 visited - the folder tree from here
                    // down is walked natively by cluster number below,
                    // which needs no further name matching at all and so
                    // can't suffer this class of bug again).
                    std::wstring volumeRootPrefix = volRoot;
                    std::vector<std::wstring> rootComponents = SplitRelativeToRoot(dir, volumeRootPrefix);
                    FatResolveResult resolved = fatVol.ResolveDirectoryClusterEx(rootComponents);

                    if (resolved.cluster != 0) {
                        int wiped = fatVol.WipeStaleEntriesRecursive(resolved.cluster, s_deadlineTick.load());
                        if (wiped > 0) s_dirEntriesWiped = wiped;
                    } else {
                        s_dirsUnresolved = 1;
                        s_firstUnresolvedDir = dir;
                        std::vector<FatDirEntry> entries = fatVol.ListEntries(resolved.lastGoodCluster);
                        int totalCount = static_cast<int>(entries.size());
                        int dirCount = 0;
                        for (const auto& re : entries) if (re.isDirectory) ++dirCount;

                        std::wstring dump = L"Looking for \"" + resolved.failedComponent +
                                             L"\" (a subfolder) among the " + std::to_wstring(dirCount) +
                                             L" subfolder(s) here (of " + std::to_wstring(totalCount) +
                                             L" entries total):\n";
                        int shown = 0;
                        for (const auto& re : entries) {
                            if (!re.isDirectory) continue;
                            if (shown >= 100) { dump += L"... (more)\n"; break; }
                            dump += re.name + L"\n";
                            ++shown;
                        }
                        if (dirCount == 0) dump += L"(no subfolders found here at all)\n";
                        s_rootEntriesDump = dump;
                    }

                    DeviceIoControl(fatVol.RawHandle(), FSCTL_UNLOCK_VOLUME, nullptr, 0,
                                     nullptr, 0, &dummy, nullptr);
                } else {
                    s_fatWipeStatus = FatWipeStatus::VolumeInUse;
                }
                fatVol.Close();
            } else {
                s_fatWipeStatus = FatWipeStatus::NeedsAdmin;
            }
        } else {
            s_fatWipeStatus = FatWipeStatus::NotFat32;
        }
    }

    s_deadlineTick = 0;
    return result;
}

int Wiper::GetTempsFilesWiped() { return s_tempsFilesWiped.load(); }
int Wiper::GetTempsFoldersDone() { return s_tempsFoldersDone.load(); }
int Wiper::GetDirEntriesWiped() { return s_dirEntriesWiped.load(); }
int Wiper::GetDirsUnresolved() { return s_dirsUnresolved.load(); }
std::wstring Wiper::GetFirstUnresolvedDir() { return s_firstUnresolvedDir; }
std::wstring Wiper::GetRootEntriesDump() { return s_rootEntriesDump; }
Wiper::FatWipeStatus Wiper::GetFatWipeStatus() { return s_fatWipeStatus; }

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
