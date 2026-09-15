#include "FatVolume.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>

namespace hexcore {

namespace {
constexpr uint32_t kFatEocMin = 0x0FFFFFF8;
constexpr uint32_t kFatMask = 0x0FFFFFFF;
constexpr size_t kEntrySize = 32;

std::wstring ToUpperCopy(const std::wstring& s) {
    std::wstring r = s;
    for (wchar_t& c : r) c = static_cast<wchar_t>(std::towupper(c));
    return r;
}

// One 32-byte directory entry, copied out of its cluster buffer. A
// folder's LFN sequence can straddle a directory-cluster boundary (its
// tail in one cluster, the short entry it belongs to at the start of
// the next), and ResolveDirectoryCluster reads one cluster into a fresh
// local buffer per iteration - so entries must be copied out, not kept
// as raw pointers into that buffer, or they dangle the moment the loop
// moves to the next cluster. An earlier version kept raw pointers,
// which silently corrupted long-name reconstruction (undefined
// behavior reading freed memory) for exactly the folders whose name
// happened to straddle a cluster boundary - not something a
// single-cluster-directory test catches.
using RawEntry = std::array<uint8_t, kEntrySize>;

// Reconstructs a long file name from a run of LFN entries stored in
// on-disk order (highest sequence number first, immediately preceding
// the short entry they belong to).
std::wstring AssembleLongName(const std::vector<RawEntry>& lfnEntriesHighToLow) {
    std::wstring name;
    // Walk from the lowest sequence number (last in this vector, closest
    // to the short entry - i.e. the FIRST part of the name) to the
    // highest (first in this vector, the LAST part of the name).
    for (auto it = lfnEntriesHighToLow.rbegin(); it != lfnEntriesHighToLow.rend(); ++it) {
        const uint8_t* e = it->data();
        uint16_t units[13];
        std::memcpy(&units[0], e + 1, 10);
        std::memcpy(&units[5], e + 14, 12);
        std::memcpy(&units[11], e + 28, 4);
        for (int i = 0; i < 13; ++i) {
            if (units[i] == 0x0000) return name;
            if (units[i] == 0xFFFF) continue;
            name.push_back(static_cast<wchar_t>(units[i]));
        }
    }
    return name;
}

std::wstring ShortNameToString(const uint8_t* e) {
    char base[9] = {0};
    char ext[4] = {0};
    std::memcpy(base, e, 8);
    std::memcpy(ext, e + 8, 3);
    for (int i = 7; i >= 0 && base[i] == ' '; --i) base[i] = '\0';
    for (int i = 2; i >= 0 && ext[i] == ' '; --i) ext[i] = '\0';
    std::wstring out;
    for (char c : std::string(base)) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    if (ext[0]) {
        out.push_back(L'.');
        for (char c : std::string(ext)) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

// A CONTINUATION cluster of a directory (its 2nd, 3rd... cluster) has no
// "." / ".." header at all - only a directory's very first cluster does -
// so the "." + ".." signature check alone can never find one whose link
// from the preceding cluster in its own chain was lost (a broken FAT
// chain, e.g. from a directory that grew, got compacted, or was
// otherwise rewritten across a lot of use). This is a broader, riskier
// fallback: true only when essentially the WHOLE cluster (>= 90% of its
// 32-byte slots) is structurally consistent with real FAT32 directory
// entries - every attribute byte's top two bits are 0 (always true for a
// real one, never guaranteed for arbitrary file bytes), every LFN
// entry's reserved cluster-pointer field is the required 0, and every
// short entry's NT case-info byte is one of the 4 values Windows ever
// writes there. Requiring near-total consistency across an entire
// cluster (hundreds of independent 32-byte checks, not just one or two)
// makes an ordinary file's content matching this by chance astronomically
// unlikely - but it is still a heuristic, not a certainty, which is why
// this is opt-in (see WipeOrphanedDirectories) rather than always on.
bool ClusterLooksLikeDirectoryData(const uint8_t* data, size_t len, bool strict) {
    if (len < kEntrySize) return false;
    size_t totalSlots = len / kEntrySize;
    size_t plausible = 0;
    size_t populated = 0; // slots holding an actual entry, not just an unused (0x00) one
    for (size_t i = 0; i < totalSlots; ++i) {
        const uint8_t* e = data + i * kEntrySize;
        uint8_t firstByte = e[0];
        uint8_t attr = e[11];
        if (attr > 0x3F) continue; // real FAT attribute bytes never set the top 2 bits
        if (firstByte == 0x00) { ++plausible; continue; }
        if (firstByte == 0xE5) { ++plausible; ++populated; continue; }
        ++populated;
        if (attr == 0x0F) {
            if (e[12] != 0x00) continue; // LFN "type" byte is always 0
            uint16_t midCluster;
            std::memcpy(&midCluster, e + 26, 2);
            if (midCluster != 0) continue; // LFN's unused cluster field is always 0
            ++plausible;
            continue;
        }
        uint8_t ntRes = e[12];
        if (ntRes != 0x00 && ntRes != 0x08 && ntRes != 0x10 && ntRes != 0x18) continue;
        if (strict) {
            // In strict mode (used only for a cluster that's currently
            // ALLOCATED - i.e. this cluster is, right now, part of some
            // other live file's or directory's chain - a false positive
            // here means writing into real, live data, not just wasting
            // time on empty space. Extra corroboration: both the write-
            // date and the write-time fields must decode to values FAT32
            // itself considers valid (year 1980-2107, month 1-12, day
            // 1-31, hour <24, minute/second <60) - a check ordinary file
            // bytes satisfy only by chance, independent of the attribute
            // and NT-byte checks already applied.
            uint16_t wtime, wdate;
            std::memcpy(&wtime, e + 22, 2);
            std::memcpy(&wdate, e + 24, 2);
            uint16_t month = (wdate >> 5) & 0x0F;
            uint16_t day = wdate & 0x1F;
            uint16_t hour = (wtime >> 11) & 0x1F;
            uint16_t minute = (wtime >> 5) & 0x3F;
            uint16_t second = wtime & 0x1F;
            bool plausibleDate = month >= 1 && month <= 12 && day >= 1 && day <= 31 &&
                                  hour <= 23 && minute <= 59 && second <= 29;
            if (!plausibleDate) continue;
        }
        ++plausible;
    }
    // An unused (all-0x00) slot alone passes every check above by
    // definition, so a cluster that's entirely - or almost entirely -
    // empty space would otherwise score 100% "plausible" for having
    // nothing but unused slots. That's not a directory, it's just free
    // space (extremely common, especially right after a free-space
    // wipe) - require real content before this is worth recursing into
    // at all, more of it when strict (allocated-cluster) mode raises the
    // stakes of getting this wrong.
    if (populated < (strict ? 8u : 2u)) return false;
    size_t thresholdPct = strict ? 100 : 90;
    return plausible * 100 >= totalSlots * thresholdPct;
}
} // namespace

bool FatVolume::Open(const std::wstring& volumePath, bool writable) {
    Close();
    DWORD access = writable ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
    m_handle = CreateFileW(volumePath.c_str(), access,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) return false;
    if (!ParseBpb()) {
        Close();
        return false;
    }
    return true;
}

void FatVolume::Close() {
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
    m_bpb = FatBpb{};
}

bool FatVolume::ParseBpb() {
    uint8_t sector0[512];
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(m_handle, zero, nullptr, FILE_BEGIN)) return false;
    DWORD read = 0;
    if (!ReadFile(m_handle, sector0, sizeof(sector0), &read, nullptr) || read != sizeof(sector0))
        return false;

    if (sector0[510] != 0x55 || sector0[511] != 0xAA) return false;

    uint16_t bytesPerSector; std::memcpy(&bytesPerSector, sector0 + 0x0B, 2);
    uint8_t sectorsPerCluster = sector0[0x0D];
    uint16_t reservedSectors; std::memcpy(&reservedSectors, sector0 + 0x0E, 2);
    uint8_t numFats = sector0[0x10];
    uint16_t rootEntCnt; std::memcpy(&rootEntCnt, sector0 + 0x11, 2);
    uint16_t totSec16; std::memcpy(&totSec16, sector0 + 0x13, 2);
    uint16_t fatSz16; std::memcpy(&fatSz16, sector0 + 0x16, 2);
    uint32_t totSec32; std::memcpy(&totSec32, sector0 + 0x20, 4);
    uint32_t fatSz32; std::memcpy(&fatSz32, sector0 + 0x24, 4);
    uint32_t rootCluster; std::memcpy(&rootCluster, sector0 + 0x2C, 4);

    // FAT32 specifically: RootEntCnt must be 0 and FATSz16 must be 0
    // (FATSz32 used instead). This is how we tell it apart from FAT12/16.
    if (rootEntCnt != 0 || fatSz16 != 0 || fatSz32 == 0) return false;
    if (bytesPerSector == 0 || sectorsPerCluster == 0 || numFats == 0) return false;

    m_bpb.bytesPerSector = bytesPerSector;
    m_bpb.sectorsPerCluster = sectorsPerCluster;
    m_bpb.reservedSectors = reservedSectors;
    m_bpb.numFats = numFats;
    m_bpb.fatSizeSectors = fatSz32;
    m_bpb.rootCluster = rootCluster;
    m_bpb.fatStartByte = static_cast<uint64_t>(reservedSectors) * bytesPerSector;
    m_bpb.dataStartByte = m_bpb.fatStartByte +
        static_cast<uint64_t>(numFats) * fatSz32 * bytesPerSector;

    uint32_t totalSectors = totSec32 != 0 ? totSec32 : totSec16;
    uint64_t dataSectors = totalSectors > (static_cast<uint64_t>(reservedSectors) + numFats * fatSz32)
        ? totalSectors - (static_cast<uint64_t>(reservedSectors) + numFats * fatSz32)
        : 0;
    m_bpb.totalDataClusters = sectorsPerCluster ?
        static_cast<uint32_t>(dataSectors / sectorsPerCluster) : 0;
    return true;
}

uint64_t FatVolume::ClusterByteOffset(uint32_t cluster) const {
    uint64_t clusterSize = static_cast<uint64_t>(m_bpb.sectorsPerCluster) * m_bpb.bytesPerSector;
    return m_bpb.dataStartByte + (static_cast<uint64_t>(cluster) - 2) * clusterSize;
}

uint32_t FatVolume::NextClusterInChain(uint32_t cluster) {
    uint64_t off = m_bpb.fatStartByte + static_cast<uint64_t>(cluster) * 4;
    LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return kFatEocMin;
    uint32_t value = 0;
    DWORD read = 0;
    if (!ReadFile(m_handle, &value, 4, &read, nullptr) || read != 4) return kFatEocMin;
    return value & kFatMask;
}

std::vector<uint32_t> FatVolume::WalkClusterChain(uint32_t startCluster) {
    std::vector<uint32_t> chain;
    uint32_t cur = startCluster;
    // A generous cap, not a real filesystem limit: guards against ever
    // spinning forever if a FAT entry is corrupt/cyclic.
    for (int guard = 0; guard < 1'000'000 && cur >= 2 && cur < kFatEocMin; ++guard) {
        chain.push_back(cur);
        cur = NextClusterInChain(cur);
    }
    return chain;
}

std::vector<uint8_t> FatVolume::ReadCluster(uint32_t cluster) {
    uint64_t clusterSize = static_cast<uint64_t>(m_bpb.sectorsPerCluster) * m_bpb.bytesPerSector;
    std::vector<uint8_t> buf(static_cast<size_t>(clusterSize));
    LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(ClusterByteOffset(cluster));
    if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return {};
    DWORD read = 0;
    if (!ReadFile(m_handle, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr) ||
        read != buf.size()) {
        return {};
    }
    return buf;
}

bool FatVolume::WriteBytesAt(uint64_t absoluteOffset, const uint8_t* data, size_t len) {
    // A raw volume handle can reject a write at an arbitrary byte offset
    // (ours is 31 bytes starting 1 byte into a 32-byte entry - never
    // sector-aligned) even though the same handle reads unaligned ranges
    // just fine; this only ever showed up against a real \\.\D: volume,
    // never the plain FAT32 image file this project's own tests use.
    // Read-modify-write the whole sector(s) spanning the target range
    // instead, so both the read and the write are always sector-aligned.
    uint32_t sectorSize = m_bpb.bytesPerSector ? m_bpb.bytesPerSector : 512;
    uint64_t sectorStart = (absoluteOffset / sectorSize) * sectorSize;
    uint64_t rangeEnd = absoluteOffset + len;
    uint64_t sectorEnd = ((rangeEnd + sectorSize - 1) / sectorSize) * sectorSize;
    size_t spanLen = static_cast<size_t>(sectorEnd - sectorStart);

    std::vector<uint8_t> buf(spanLen);
    LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(sectorStart);
    if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return false;
    DWORD readBytes = 0;
    if (!ReadFile(m_handle, buf.data(), static_cast<DWORD>(buf.size()), &readBytes, nullptr) ||
        readBytes != buf.size()) {
        return false;
    }

    size_t patchOffset = static_cast<size_t>(absoluteOffset - sectorStart);
    std::memcpy(buf.data() + patchOffset, data, len);

    if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return false;
    DWORD written = 0;
    return WriteFile(m_handle, buf.data(), static_cast<DWORD>(buf.size()), &written, nullptr) &&
           written == buf.size();
}

uint32_t FatVolume::ResolveDirectoryCluster(const std::vector<std::wstring>& relativePathComponents) {
    return ResolveDirectoryClusterEx(relativePathComponents).cluster;
}

FatResolveResult FatVolume::ResolveDirectoryClusterEx(const std::vector<std::wstring>& relativePathComponents) {
    uint32_t currentCluster = m_bpb.rootCluster;

    for (const auto& wanted : relativePathComponents) {
        std::wstring wantedUpper = ToUpperCopy(wanted);
        uint32_t foundCluster = 0;
        std::vector<RawEntry> pendingLfn;

        for (uint32_t cluster : WalkClusterChain(currentCluster)) {
            std::vector<uint8_t> data = ReadCluster(cluster);
            if (data.empty()) break;

            for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
                const uint8_t* e = data.data() + off;
                uint8_t firstByte = e[0];
                // A 0x00 first byte formally means "never-used slot", and
                // the FAT32 spec's own convention is that everything after
                // the first one is unused too - so a compliant driver can
                // stop right here. Real-world drives don't always keep that
                // invariant though (entries can end up with an unused gap
                // followed by more live ones, especially after years of
                // create/delete churn) - treating it as just "skip this
                // slot" instead of "stop scanning" costs a little extra,
                // bounded I/O over the directory's already-allocated
                // clusters, but is what actually finds every subfolder on a
                // real, heavily-used volume instead of silently stopping
                // partway through it.
                if (firstByte == 0x00) { pendingLfn.clear(); continue; }
                if (firstByte == 0xE5) { pendingLfn.clear(); continue; } // deleted - not a live entry

                uint8_t attr = e[11];
                if (attr == 0x0F) {
                    RawEntry copy;
                    std::memcpy(copy.data(), e, kEntrySize);
                    pendingLfn.push_back(copy);
                    continue;
                }
                if (attr & 0x08) { pendingLfn.clear(); continue; } // volume label

                std::wstring longName = pendingLfn.empty() ? std::wstring() : AssembleLongName(pendingLfn);
                std::wstring shortName = ShortNameToString(e);
                pendingLfn.clear();

                bool isDir = (attr & 0x10) != 0;
                bool matches = (!longName.empty() && ToUpperCopy(longName) == wantedUpper) ||
                               ToUpperCopy(shortName) == wantedUpper;
                if (isDir && matches) {
                    uint16_t hi, lo;
                    std::memcpy(&hi, e + 20, 2);
                    std::memcpy(&lo, e + 26, 2);
                    foundCluster = (static_cast<uint32_t>(hi) << 16) | lo;
                }
            }
            if (foundCluster != 0) break;
        }

        if (foundCluster == 0) {
            FatResolveResult result;
            result.cluster = 0;
            result.lastGoodCluster = currentCluster;
            result.failedComponent = wanted;
            return result;
        }
        currentCluster = foundCluster;
    }

    FatResolveResult result;
    result.cluster = currentCluster;
    return result;
}

FatScanResult FatVolume::ScanDirectory(uint32_t startCluster) {
    FatScanResult result;
    if (startCluster < 2) return result;

    for (uint32_t cluster : WalkClusterChain(startCluster)) {
        std::vector<uint8_t> data = ReadCluster(cluster);
        if (data.empty()) return result;

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            const uint8_t* e = data.data() + off;
            uint8_t firstByte = e[0];
            if (firstByte == 0x00) continue; // unused slot, not necessarily the true end (see comment above)
            if (firstByte == 0xE5) { ++result.staleEntries; continue; }
            uint8_t attr = e[11];
            if (attr & 0x08) continue; // volume label
            if (attr != 0x0F) ++result.liveEntries; // count short entries only, not LFN fragments
        }
    }
    result.ok = true;
    return result;
}

std::vector<FatDirEntry> FatVolume::ListEntries(uint32_t startCluster) {
    std::vector<FatDirEntry> out;
    if (startCluster < 2) return out;

    std::vector<RawEntry> pendingLfn;

    for (uint32_t cluster : WalkClusterChain(startCluster)) {
        std::vector<uint8_t> data = ReadCluster(cluster);
        if (data.empty()) break;

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            const uint8_t* e = data.data() + off;
            uint8_t firstByte = e[0];
            if (firstByte == 0x00) { pendingLfn.clear(); continue; } // unused slot, not necessarily the true end
            if (firstByte == 0xE5) { pendingLfn.clear(); continue; }

            uint8_t attr = e[11];
            if (attr == 0x0F) {
                RawEntry copy;
                std::memcpy(copy.data(), e, kEntrySize);
                pendingLfn.push_back(copy);
                continue;
            }
            if (attr & 0x08) { pendingLfn.clear(); continue; }

            std::wstring longName = pendingLfn.empty() ? std::wstring() : AssembleLongName(pendingLfn);
            std::wstring shortName = ShortNameToString(e);
            pendingLfn.clear();

            FatDirEntry entry;
            entry.name = !longName.empty() ? longName : shortName;
            entry.isDirectory = (attr & 0x10) != 0;
            uint16_t hi, lo;
            std::memcpy(&hi, e + 20, 2);
            std::memcpy(&lo, e + 26, 2);
            entry.cluster = (static_cast<uint32_t>(hi) << 16) | lo;
            out.push_back(entry);
        }
    }
    return out;
}

bool FatVolume::IsClusterFree(uint32_t cluster) {
    if (cluster < 2) return false;
    uint64_t off = m_bpb.fatStartByte + static_cast<uint64_t>(cluster) * 4;
    LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(off);
    if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return false;
    uint32_t value = 0;
    DWORD read = 0;
    if (!ReadFile(m_handle, &value, 4, &read, nullptr) || read != 4) return false;
    return (value & kFatMask) == 0;
}

std::vector<uint32_t> FatVolume::FindDeletedSubdirClusters(uint32_t startCluster) {
    std::vector<uint32_t> out;
    if (startCluster < 2) return out;

    for (uint32_t cluster : WalkClusterChain(startCluster)) {
        std::vector<uint8_t> data = ReadCluster(cluster);
        if (data.empty()) break;

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            const uint8_t* e = data.data() + off;
            if (e[0] != 0xE5) continue;
            uint8_t attr = e[11];
            if (attr == 0x0F) continue;   // an LFN fragment, not a real entry
            if (!(attr & 0x10)) continue; // only a deleted DIRECTORY pointer is of interest here
            if (attr & 0x08) continue;    // volume label

            uint16_t hi, lo;
            std::memcpy(&hi, e + 20, 2);
            std::memcpy(&lo, e + 26, 2);
            uint32_t subCluster = (static_cast<uint32_t>(hi) << 16) | lo;
            if (subCluster < 2 || subCluster == startCluster) continue;
            out.push_back(subCluster);
        }
    }
    return out;
}

int FatVolume::WipeStaleEntries(uint32_t startCluster) {
    if (startCluster < 2) return -1;
    int wiped = 0;
    // Byte 0 stays 0xE5 - the FAT "this slot is deleted/available" marker
    // - and only bytes 1..31 (name characters, size, timestamps, cluster
    // pointer: everything a forensic tool actually reads) are zeroed.
    // Zeroing byte 0 too would turn it into 0x00, which FAT32 reads as
    // "end of directory" - that would make the filesystem driver itself
    // stop seeing any LIVE entry stored after this slot in the same
    // directory, silently hiding real files instead of just erasing a
    // stale one. Caught by this project's own synthetic-image test
    // before ever touching a real volume.
    static const uint8_t zeroTail[kEntrySize - 1] = {0};

    for (uint32_t cluster : WalkClusterChain(startCluster)) {
        std::vector<uint8_t> data = ReadCluster(cluster);
        if (data.empty()) return -1;
        uint64_t clusterBase = ClusterByteOffset(cluster);

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            uint8_t firstByte = data[off];
            if (firstByte == 0x00) continue; // unused slot, not necessarily the true end (see ListEntries)
            if (firstByte == 0xE5) {
                if (!WriteBytesAt(clusterBase + off + 1, zeroTail, sizeof(zeroTail))) return -1;
                ++wiped;
            }
        }
    }
    return wiped;
}

int FatVolume::WipeStaleEntriesRecursive(uint32_t startCluster, uint64_t deadlineTick,
                                          const std::atomic<bool>* cancelFlag, int maxDepth,
                                          std::atomic<int>* liveDirsVisited,
                                          std::atomic<int>* liveEntriesWiped) {
    if (maxDepth <= 0) return 0;
    if (deadlineTick != 0 && GetTickCount64() >= deadlineTick) return 0;
    if (cancelFlag && cancelFlag->load()) return 0;

    ++m_dirsVisited;
    if (liveDirsVisited) liveDirsVisited->fetch_add(1);

    // Must run BEFORE WipeStaleEntries: a subfolder that was deleted as a
    // whole (not just emptied out) only shows up as a 0xE5 entry here,
    // and wiping that entry's tail destroys its cluster pointer along
    // with its name - this is the only chance to learn where its content
    // actually lives before that happens.
    std::vector<uint32_t> deletedSubdirs = FindDeletedSubdirClusters(startCluster);

    int total = WipeStaleEntries(startCluster);
    if (total < 0) {
        ++m_dirReadErrors;
        total = 0; // don't let one bad directory abort scanning its siblings
    } else if (total > 0 && liveEntriesWiped) {
        liveEntriesWiped->fetch_add(total);
    }

    for (const auto& entry : ListEntries(startCluster)) {
        if (!entry.isDirectory) continue;
        if (entry.name == L"." || entry.name == L"..") continue;
        if (entry.cluster < 2) continue;

        ++m_subdirsFound;
        int sub = WipeStaleEntriesRecursive(entry.cluster, deadlineTick, cancelFlag, maxDepth - 1,
                                             liveDirsVisited, liveEntriesWiped);
        if (sub > 0) total += sub;
    }

    // A folder that was deleted whole - "ilovepdf_extracted-pages",
    // say - still shows up in FTK Imager with its own contents
    // browsable as long as its cluster(s) haven't been reused for
    // something else since. Only descend when IsClusterFree confirms
    // that: a non-zero FAT entry means the cluster now belongs to some
    // other, live chain, and treating its bytes as directory entries -
    // let alone zeroing whatever looks like a stale slot inside it -
    // would corrupt that live data instead of truly-deleted data.
    for (uint32_t cluster : deletedSubdirs) {
        if (!IsClusterFree(cluster)) continue;
        ++m_subdirsFound;
        int sub = WipeStaleEntriesRecursive(cluster, deadlineTick, cancelFlag, maxDepth - 1,
                                             liveDirsVisited, liveEntriesWiped);
        if (sub > 0) total += sub;
    }
    return total;
}

int FatVolume::WipeSingleClusterStrict(uint32_t cluster, uint64_t deadlineTick,
                                        const std::atomic<bool>* cancelFlag,
                                        std::atomic<int>* liveDirsVisited,
                                        std::atomic<int>* liveEntriesWiped, int maxDepth) {
    if (maxDepth <= 0) return 0;
    if (deadlineTick != 0 && GetTickCount64() >= deadlineTick) return 0;
    if (cancelFlag && cancelFlag->load()) return 0;
    if (cluster < 2) return 0;

    std::vector<uint8_t> data = ReadCluster(cluster);
    if (data.empty()) return 0;

    ++m_dirsVisited;
    if (liveDirsVisited) liveDirsVisited->fetch_add(1);

    uint64_t clusterBase = ClusterByteOffset(cluster);
    static const uint8_t zeroTail[kEntrySize - 1] = {0};
    int total = 0;
    std::vector<uint32_t> subdirClusters;

    for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
        const uint8_t* e = data.data() + off;
        uint8_t firstByte = e[0];
        if (firstByte == 0xE5) {
            if (WriteBytesAt(clusterBase + off + 1, zeroTail, sizeof(zeroTail))) ++total;
            continue;
        }
        if (firstByte == 0x00) continue;
        uint8_t attr = e[11];
        if (attr == 0x0F) continue;    // LFN fragment - nothing to recurse into directly
        if (!(attr & 0x10)) continue;  // not a directory entry
        uint16_t hi, lo;
        std::memcpy(&hi, e + 20, 2);
        std::memcpy(&lo, e + 26, 2);
        uint32_t subCluster = (static_cast<uint32_t>(hi) << 16) | lo;
        if (subCluster >= 2 && subCluster != cluster) subdirClusters.push_back(subCluster);
    }
    if (total > 0 && liveEntriesWiped) liveEntriesWiped->fetch_add(total);
    m_subdirsFound += static_cast<int>(subdirClusters.size());

    for (uint32_t sub : subdirClusters) {
        int subTotal = 0;
        if (IsClusterFree(sub)) {
            // A free cluster's own chain is safe to follow fully - same
            // guarantee every other pass in this class already relies on.
            subTotal = WipeStaleEntriesRecursive(sub, deadlineTick, cancelFlag, 64,
                                                  liveDirsVisited, liveEntriesWiped);
        } else {
            // Still allocated: re-validate independently before touching
            // it at all, rather than trusting a pointer found inside
            // already-uncertain data.
            std::vector<uint8_t> subData = ReadCluster(sub);
            if (subData.size() >= kEntrySize * 2) {
                const uint8_t* dot = subData.data();
                const uint8_t* dotdot = subData.data() + kEntrySize;
                bool looksLikeDir =
                    dot[0] == '.' && (dot[11] & 0x10) && std::memcmp(dot + 1, "          ", 10) == 0 &&
                    dotdot[0] == '.' && dotdot[1] == '.' && (dotdot[11] & 0x10) &&
                    std::memcmp(dotdot + 2, "         ", 9) == 0;
                if (!looksLikeDir) {
                    looksLikeDir = ClusterLooksLikeDirectoryData(subData.data(), subData.size(), true);
                }
                if (looksLikeDir) {
                    subTotal = WipeSingleClusterStrict(sub, deadlineTick, cancelFlag, liveDirsVisited,
                                                        liveEntriesWiped, maxDepth - 1);
                }
            }
        }
        if (subTotal > 0) total += subTotal;
    }
    return total;
}

int FatVolume::WipeOrphanedDirectories(uint64_t deadlineTick, const std::atomic<bool>* cancelFlag,
                                        std::atomic<int>* liveDirsVisited,
                                        std::atomic<int>* liveEntriesWiped,
                                        std::atomic<int64_t>* clustersScanned,
                                        bool includeAllocatedClusters) {
    int total = 0;
    if (m_bpb.totalDataClusters < 1) return 0;
    uint32_t maxCluster = m_bpb.totalDataClusters + 1; // cluster numbering starts at 2

    // One bulk read of the whole FAT instead of a seek+read per candidate
    // cluster - the difference between one read and up to millions of
    // 4-byte ones on a large, mostly-empty volume.
    size_t fatBytes = static_cast<size_t>(m_bpb.fatSizeSectors) * m_bpb.bytesPerSector;
    std::vector<uint8_t> fatTable(fatBytes);
    {
        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(m_bpb.fatStartByte);
        if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return 0;
        DWORD readBytes = 0;
        if (!ReadFile(m_handle, fatTable.data(), static_cast<DWORD>(fatTable.size()), &readBytes, nullptr) ||
            readBytes != fatTable.size()) {
            return 0;
        }
    }
    auto isFree = [&](uint32_t cluster) {
        size_t off = static_cast<size_t>(cluster) * 4;
        if (off + 4 > fatTable.size()) return false;
        uint32_t value;
        std::memcpy(&value, fatTable.data() + off, 4);
        return (value & kFatMask) == 0;
    };

    uint64_t clusterSize64 = static_cast<uint64_t>(m_bpb.sectorsPerCluster) * m_bpb.bytesPerSector;
    if (clusterSize64 == 0 || clusterSize64 > (64u * 1024 * 1024)) return 0;
    size_t clusterSize = static_cast<size_t>(clusterSize64);

    // Data clusters are laid out contiguously on disk by construction
    // (cluster N+1 immediately follows cluster N), so the whole data
    // region can be read as one long sequential stream in large chunks -
    // far faster on real media than one small read per free cluster.
    constexpr size_t kChunkBytes = 64 * 1024 * 1024;
    uint32_t clustersPerChunk = static_cast<uint32_t>(std::max<size_t>(1, kChunkBytes / clusterSize));
    std::vector<uint8_t> chunk;

    for (uint32_t chunkStart = 2; chunkStart <= maxCluster; chunkStart += clustersPerChunk) {
        if (deadlineTick != 0 && GetTickCount64() >= deadlineTick) break;
        if (cancelFlag && cancelFlag->load()) break;

        uint32_t chunkEnd = std::min(chunkStart + clustersPerChunk - 1, maxCluster);
        uint32_t clustersInChunk = chunkEnd - chunkStart + 1;

        // Cheap, in-memory check first: if nothing in this whole span is
        // even free - and allocated clusters aren't in scope either -
        // there is nothing here a live directory tree couldn't already
        // reach, so skip the data read outright. On a drive that's
        // mostly full of live files this alone can skip most of the
        // volume's I/O in the default (free-only) mode.
        bool anyCandidate = includeAllocatedClusters;
        if (!anyCandidate) {
            for (uint32_t c = chunkStart; c <= chunkEnd; ++c) {
                if (isFree(c)) { anyCandidate = true; break; }
            }
        }
        if (!anyCandidate) {
            if (clustersScanned) clustersScanned->fetch_add(clustersInChunk);
            continue;
        }

        size_t chunkLen = static_cast<size_t>(clustersInChunk) * clusterSize;
        if (chunk.size() < chunkLen) chunk.resize(chunkLen);

        LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(ClusterByteOffset(chunkStart));
        bool readOk = SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN) != 0;
        DWORD readBytes = 0;
        if (readOk) {
            readOk = ReadFile(m_handle, chunk.data(), static_cast<DWORD>(chunkLen), &readBytes, nullptr) != 0 &&
                     readBytes == chunkLen;
        }
        if (!readOk) {
            // A read failure on one span (a bad sector on well-used
            // flash media, say) shouldn't abort the whole sweep - the
            // same "one bad spot doesn't stop the rest" approach
            // WipeStaleEntriesRecursive already takes for a directory
            // read error. Count it as covered and move on.
            if (clustersScanned) clustersScanned->fetch_add(clustersInChunk);
            continue;
        }

        if (clusterSize >= kEntrySize * 2) {
            for (uint32_t cluster = chunkStart; cluster <= chunkEnd; ++cluster) {
                if (cancelFlag && cancelFlag->load()) break;
                bool free = isFree(cluster);
                if (!free && !includeAllocatedClusters) continue;
                const uint8_t* data = chunk.data() + static_cast<size_t>(cluster - chunkStart) * clusterSize;
                const uint8_t* dot = data;
                const uint8_t* dotdot = data + kEntrySize;
                bool looksLikeDir =
                    dot[0] == '.' && (dot[11] & 0x10) && std::memcmp(dot + 1, "          ", 10) == 0 &&
                    dotdot[0] == '.' && dotdot[1] == '.' && (dotdot[11] & 0x10) &&
                    std::memcmp(dotdot + 2, "         ", 9) == 0;
                // A directory's own first cluster always has this
                // signature; one of its later clusters, reachable only if
                // the chain linking it back is broken, never does - the
                // broader structural check is the only way to still find
                // one of those. An allocated candidate is held to the
                // strict variant regardless of whether the "." signature
                // already matched, and is repaired via
                // WipeSingleClusterStrict - never the ordinary chain-
                // following recursive wipe, which would follow this
                // cluster's REAL chain (whatever file or directory
                // currently owns it) rather than staying confined to the
                // one cluster that was actually validated.
                if (!free) {
                    if (!looksLikeDir && !ClusterLooksLikeDirectoryData(data, clusterSize, true)) continue;
                    int sub = WipeSingleClusterStrict(cluster, deadlineTick, cancelFlag, liveDirsVisited,
                                                       liveEntriesWiped);
                    if (sub > 0) total += sub;
                    continue;
                }
                bool looksLikeContinuation =
                    !looksLikeDir && ClusterLooksLikeDirectoryData(data, clusterSize, false);
                if (!looksLikeDir && !looksLikeContinuation) continue;

                int sub = WipeStaleEntriesRecursive(cluster, deadlineTick, cancelFlag, 64,
                                                     liveDirsVisited, liveEntriesWiped);
                if (sub > 0) total += sub;
            }
        }
        if (clustersScanned) clustersScanned->fetch_add(clustersInChunk);
    }
    return total;
}

std::wstring FatVolume::DumpDirectoryRaw(uint32_t startCluster, int maxEntries) {
    std::wstring out;
    if (startCluster < 2) return out;

    int count = 0;
    int clusterIdx = 0;
    std::vector<RawEntry> pendingLfn;
    for (uint32_t cluster : WalkClusterChain(startCluster)) {
        std::vector<uint8_t> data = ReadCluster(cluster);
        if (data.empty()) {
            wchar_t line[128];
            swprintf(line, 128, L"[cluster #%d = %u] READ FAILED\r\n", clusterIdx, cluster);
            out += line;
            ++clusterIdx;
            continue;
        }

        for (size_t off = 0; off + kEntrySize <= data.size() && count < maxEntries; off += kEntrySize, ++count) {
            const uint8_t* e = data.data() + off;
            uint8_t firstByte = e[0];
            uint8_t attr = e[11];

            // Full raw hex of the 32-byte slot - the interpreted fields
            // below are only as good as the parsing logic that produces
            // them, so this is what lets a real discrepancy (a name that
            // reconstructs to one character when it plainly shouldn't)
            // be diagnosed against the actual bytes rather than trusted
            // blindly. Built character-by-character into a std::wstring
            // rather than via a formatted-into-fixed-buffer loop, which
            // was producing a stray embedded NUL a few characters in on
            // this toolchain for reasons not worth chasing further -
            // this sidesteps that whole class of bug.
            static const wchar_t kHexDigits[] = L"0123456789ABCDEF";
            std::wstring hexStr;
            hexStr.reserve(kEntrySize * 3);
            for (size_t i = 0; i < kEntrySize; ++i) {
                hexStr.push_back(kHexDigits[(e[i] >> 4) & 0xF]);
                hexStr.push_back(kHexDigits[e[i] & 0xF]);
                hexStr.push_back(L' ');
            }
            const wchar_t* hex = hexStr.c_str();

            wchar_t line[500];
            if (attr == 0x0F) {
                if (firstByte != 0xE5) {
                    RawEntry copy;
                    std::memcpy(copy.data(), e, kEntrySize);
                    pendingLfn.push_back(copy);
                } else {
                    pendingLfn.clear();
                }
                swprintf(line, 500, L"c#%d(clu=%u) off=0x%04X first=0x%02X  LFN-fragment  hex=%ls\r\n",
                         clusterIdx, cluster, static_cast<unsigned>(off), firstByte, hex);
            } else {
                uint16_t hi, lo;
                std::memcpy(&hi, e + 20, 2);
                std::memcpy(&lo, e + 26, 2);
                uint32_t entCluster = (static_cast<uint32_t>(hi) << 16) | lo;
                std::wstring shortName = ShortNameToString(e);
                std::wstring longName = pendingLfn.empty() ? std::wstring() : AssembleLongName(pendingLfn);
                pendingLfn.clear();
                swprintf(line, 500,
                         L"c#%d(clu=%u) off=0x%04X first=0x%02X attr=0x%02X dir=%ls short=\"%ls\" "
                         L"long=\"%ls\" cluster=%u  hex=%ls\r\n",
                         clusterIdx, cluster, static_cast<unsigned>(off), firstByte, attr,
                         (attr & 0x10) ? L"Y" : L"N", shortName.c_str(), longName.c_str(), entCluster, hex);
            }
            out += line;
        }
        ++clusterIdx;
        if (count >= maxEntries) break;
    }

    wchar_t summary[160];
    swprintf(summary, 160, L"\r\n--- total clusters in chain: %d, entries listed: %d%ls ---\r\n",
             clusterIdx, count, count >= maxEntries ? L" (hit the cap)" : L"");
    out += summary;
    return out;
}

} // namespace hexcore
