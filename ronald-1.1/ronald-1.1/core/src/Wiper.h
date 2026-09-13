#pragma once
#include <windows.h>
#include <string>
#include <cstdint>

namespace hexcore {

// Secure file/space wiper.
//
// WipeFile      — overwrites every byte of a file with (random / zero /
//                 random) passes, then deletes it.  After this, forensic
//                 tools that scan unallocated clusters find only garbage.
//
// WipeEditorTemps — scans a directory for hxe*.TMP files left by the
//                   safe-save routine and wipes them all.
//
// WipeFreeSpace — fills ALL free clusters on the volume that contains
//                 `driveRoot` with zeros, then deletes the fill file.
//                 This is the operation that makes already-deleted files
//                 (the ones shown with a red X in FTK Imager) truly
//                 unrecoverable.
class Wiper {
public:
    // Wipe + delete a single file.  passes >= 1.  Returns true on success.
    static bool WipeFile(const std::wstring& path, int passes = 3);

    // Scan `dir` for hxe*.TMP editor temp files, wipe+delete each one.
    // Returns the number of files successfully wiped.
    static int WipeEditorTemps(const std::wstring& dir, int passes = 3);

    // Fill all free space on the volume that contains `driveRoot` with
    // zeros (one large fill file, written in 1 MB chunks), flush, then
    // delete the fill file.  This overwrites unallocated clusters where
    // deleted files (Untis.exe~RF*.TMP etc.) used to live.
    //
    // hwndOwner — parent window for SetCursor (can be nullptr).
    // Returns total bytes written, or -1 on a hard error before any
    // bytes were written.  Running out of disk space is expected and is
    // NOT treated as an error — it means we filled everything.
    static int64_t WipeFreeSpace(const std::wstring& driveRoot,
                                  HWND hwndOwner = nullptr);

private:
    // XOR-shift PRNG — good enough for forensic-grade overwrite.
    static uint64_t s_rng;
    static void     FillRandom(uint8_t* buf, size_t len);
    static void     FillZero  (uint8_t* buf, size_t len);
};

} // namespace hexcore
