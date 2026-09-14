// FatVolume - low-level FAT32 directory-entry scanner/wiper.
//
// A tool like FTK Imager reads a deleted file's name/size/timestamp
// directly from its parent folder's own on-disk directory table, not
// from the file's data. Wiper::WipeFreeSpace (see Wiper.h) only ever
// overwrites file *content* in already-free clusters, so it can never
// remove that listing. This is the lower-level counterpart: it opens
// the raw volume, parses the FAT32 boot sector and directory structures
// itself, finds every 32-byte directory-entry slot still marked
// "deleted" (first byte 0xE5) in a chosen folder, and overwrites just
// those slots with zeros - the only way to make that listing actually
// disappear, short of reformatting the volume.
//
// This works against either a real raw volume path (L"\\\\.\\D:", which
// needs Administrator privileges and an exclusive volume lock) or a
// plain FAT32 image file at any path (used by this project's own
// tests, built and checked against a hand-crafted synthetic image -
// there is no Windows FAT32 driver available in this project's
// Linux-only build/test environment to validate against instead).
//
// FAT32 only (not FAT12/16/exFAT/NTFS) - IsNtfsVolume-style detection
// and rejection for anything else is the caller's responsibility.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace hexcore {

struct FatBpb {
    uint16_t bytesPerSector = 0;
    uint32_t sectorsPerCluster = 0;
    uint32_t reservedSectors = 0;
    uint32_t numFats = 0;
    uint32_t fatSizeSectors = 0;
    uint32_t rootCluster = 0;
    uint64_t fatStartByte = 0;
    uint64_t dataStartByte = 0;
};

struct FatScanResult {
    int staleEntries = 0;     // 32-byte slots found with a deleted marker
    int liveEntries = 0;      // ordinary (non-deleted, non-LFN) entries found
    bool ok = false;          // false if the path/volume couldn't be parsed at all
};

struct FatDirEntry {
    std::wstring name;   // reconstructed long name if it had one, else the short name
    bool isDirectory = false;
    uint32_t cluster = 0;
};

struct FatResolveResult {
    uint32_t cluster = 0;          // final resolved cluster, or 0 if resolution failed partway
    uint32_t lastGoodCluster = 0;  // the directory being searched when resolution failed
                                    // (root cluster if the very first component failed);
                                    // meaningless when cluster != 0 (fully resolved)
    std::wstring failedComponent;  // the path component that couldn't be found; empty if
                                    // fully resolved
};

class FatVolume {
public:
    // Opens `volumePath` (a raw volume like L"\\\\.\\D:", or a plain
    // image file) and parses its FAT32 boot sector. Returns false (and
    // closes the handle) if it isn't a readable FAT32 volume.
    bool Open(const std::wstring& volumePath, bool writable);
    void Close();
    bool IsOpen() const { return m_handle != INVALID_HANDLE_VALUE; }
    // For the caller to FSCTL_LOCK_VOLUME/FSCTL_UNLOCK_VOLUME around a
    // batch of writes - locking is a concern of how this is used
    // (against a live, mounted volume), not of FAT parsing itself.
    HANDLE RawHandle() const { return m_handle; }

    // Resolves `relativePathComponents` (e.g. {"root", "DocumentosVarios"})
    // starting from the volume's root directory, following long-file-name
    // entries so names with spaces/mixed case match. Returns the target
    // directory's first cluster, or 0 if any component wasn't found.
    uint32_t ResolveDirectoryCluster(const std::vector<std::wstring>& relativePathComponents);

    // Same resolution, but on failure also reports exactly which
    // component couldn't be found and the cluster of the directory being
    // searched at that point - for diagnosing a failure rather than just
    // detecting one.
    FatResolveResult ResolveDirectoryClusterEx(const std::vector<std::wstring>& relativePathComponents);

    // Read-only: walks every cluster of the directory starting at
    // `startCluster` and counts stale vs. live entries. Never writes
    // anything - safe to run first to sanity-check what a real fix
    // would find before committing to one.
    FatScanResult ScanDirectory(uint32_t startCluster);

    // Read-only: lists every LIVE (non-deleted) entry in the directory
    // starting at `startCluster`, with the same long-name reconstruction
    // ResolveDirectoryCluster uses for matching - a direct, inspectable
    // view of exactly what this reader sees, to compare against what a
    // tool like Explorer or FTK Imager shows for the same folder.
    std::vector<FatDirEntry> ListEntries(uint32_t startCluster);

    // Overwrites every stale (deleted-marker) 32-byte slot found in the
    // directory starting at `startCluster` with zeros, leaving every
    // live entry's bytes completely untouched. Returns the number of
    // slots wiped, or -1 on error. Requires the volume to have been
    // opened with writable=true.
    int WipeStaleEntries(uint32_t startCluster);

    // Wipes stale entries in the directory at `startCluster`, then
    // recurses into every LIVE subdirectory found there (via
    // ListEntries's own cluster numbers - no path-string matching
    // involved at all, which is what makes this immune to the class of
    // bug ResolveDirectoryCluster had: it only ever needs to resolve the
    // one folder the caller picked, not re-match a name at every level
    // for every folder in the tree). Skips "." and "..". Stops
    // descending (but keeps prior results) once `deadlineTick`
    // (GetTickCount64() value, 0 = no deadline) has passed, `*cancelFlag`
    // becomes true (checked the same way, 0 = not cancellable), or
    // `maxDepth` is exhausted. Returns the total entries wiped across the
    // whole subtree, or -1 on a hard error.
    // `liveDirsVisited`/`liveEntriesWiped`, if given, are incremented as
    // this progresses (in addition to the final LastDirsVisited() /
    // return value available only once it's done) so a caller polling
    // from another thread can show live progress during a long run.
    int WipeStaleEntriesRecursive(uint32_t startCluster, uint64_t deadlineTick = 0,
                                   const std::atomic<bool>* cancelFlag = nullptr, int maxDepth = 64,
                                   std::atomic<int>* liveDirsVisited = nullptr,
                                   std::atomic<int>* liveEntriesWiped = nullptr);

    // Stats from the most recent WipeStaleEntriesRecursive call: total
    // directories actually visited, and how many of those hit a hard
    // read error (and so were silently skipped rather than scanned) -
    // distinguishes "scanned everything, found nothing" from "couldn't
    // even read parts of the tree".
    int LastDirsVisited() const { return m_dirsVisited; }
    int LastDirReadErrors() const { return m_dirReadErrors; }

    const FatBpb& Bpb() const { return m_bpb; }

private:
    bool ParseBpb();
    uint32_t NextClusterInChain(uint32_t cluster);
    std::vector<uint32_t> WalkClusterChain(uint32_t startCluster);
    uint64_t ClusterByteOffset(uint32_t cluster) const;
    std::vector<uint8_t> ReadCluster(uint32_t cluster);
    bool WriteBytesAt(uint64_t absoluteOffset, const uint8_t* data, size_t len);

    HANDLE m_handle = INVALID_HANDLE_VALUE;
    FatBpb m_bpb;
    int m_dirsVisited = 0;
    int m_dirReadErrors = 0;
};

} // namespace hexcore
