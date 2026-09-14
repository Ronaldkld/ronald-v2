#include "FatVolume.h"

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
    uint16_t fatSz16; std::memcpy(&fatSz16, sector0 + 0x16, 2);
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
    LARGE_INTEGER li; li.QuadPart = static_cast<LONGLONG>(absoluteOffset);
    if (!SetFilePointerEx(m_handle, li, nullptr, FILE_BEGIN)) return false;
    DWORD written = 0;
    return WriteFile(m_handle, data, static_cast<DWORD>(len), &written, nullptr) && written == len;
}

uint32_t FatVolume::ResolveDirectoryCluster(const std::vector<std::wstring>& relativePathComponents) {
    uint32_t currentCluster = m_bpb.rootCluster;

    for (const auto& wanted : relativePathComponents) {
        std::wstring wantedUpper = ToUpperCopy(wanted);
        uint32_t foundCluster = 0;
        std::vector<RawEntry> pendingLfn;

        for (uint32_t cluster : WalkClusterChain(currentCluster)) {
            std::vector<uint8_t> data = ReadCluster(cluster);
            if (data.empty()) return 0;
            bool endOfDir = false;

            for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
                const uint8_t* e = data.data() + off;
                uint8_t firstByte = e[0];
                if (firstByte == 0x00) { endOfDir = true; break; }
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
            if (endOfDir || foundCluster != 0) break;
        }

        if (foundCluster == 0) return 0;
        currentCluster = foundCluster;
    }
    return currentCluster;
}

FatScanResult FatVolume::ScanDirectory(uint32_t startCluster) {
    FatScanResult result;
    if (startCluster < 2) return result;

    for (uint32_t cluster : WalkClusterChain(startCluster)) {
        std::vector<uint8_t> data = ReadCluster(cluster);
        if (data.empty()) return result;
        bool endOfDir = false;

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            const uint8_t* e = data.data() + off;
            uint8_t firstByte = e[0];
            if (firstByte == 0x00) { endOfDir = true; break; }
            if (firstByte == 0xE5) { ++result.staleEntries; continue; }
            uint8_t attr = e[11];
            if (attr & 0x08) continue; // volume label
            if (attr != 0x0F) ++result.liveEntries; // count short entries only, not LFN fragments
        }
        if (endOfDir) break;
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
        bool endOfDir = false;

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            const uint8_t* e = data.data() + off;
            uint8_t firstByte = e[0];
            if (firstByte == 0x00) { endOfDir = true; break; }
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
        if (endOfDir) break;
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
        bool endOfDir = false;
        uint64_t clusterBase = ClusterByteOffset(cluster);

        for (size_t off = 0; off + kEntrySize <= data.size(); off += kEntrySize) {
            uint8_t firstByte = data[off];
            if (firstByte == 0x00) { endOfDir = true; break; }
            if (firstByte == 0xE5) {
                if (!WriteBytesAt(clusterBase + off + 1, zeroTail, sizeof(zeroTail))) return -1;
                ++wiped;
            }
        }
        if (endOfDir) break;
    }
    return wiped;
}

} // namespace hexcore
