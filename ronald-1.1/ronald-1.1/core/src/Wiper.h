#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include <atomic>

namespace hexcore {

// Secure file/space wiper.
//
// WipeFile      — overwrites every byte of a file with (random / zero /
//                 random) passes, then deletes it.  After this, forensic
//                 tools that scan unallocated clusters find only garbage.
//
// WipeEditorTemps — recursively scans a chosen folder/drive for orphaned
//                   safe-save temp files — this editor's own hxe*.TMP
//                   files, and the <name>~RFxxxxxxxx.TMP backup files the
//                   Win32 ReplaceFile API can leave behind on FAT/FAT32/
//                   exFAT volumes (which lack NTFS's atomic replace
//                   support) — and wipes them all.
//
// WipeFreeSpace — fills free clusters on the volume that contains
//                 `driveRoot` with zeros (leaving a small safety margin),
//                 then deletes the fill file. This is the operation that
//                 makes already-deleted files (the ones shown with a red
//                 X in FTK Imager) truly unrecoverable. Runs until done,
//                 the safety margin is reached, or RequestCancel() is
//                 called from another thread — callers should run it on
//                 a worker thread and poll GetBytesWrittenSoFar().
class Wiper {
public:
    // Wipe + delete a single file.  passes >= 1.  Returns true on success.
    static bool WipeFile(const std::wstring& path, int passes = 3);

    // Recursively scan `dir` and its subfolders for orphaned safe-save
    // temp files (see class comment) and wipe+delete each one. Returns
    // the number of files successfully wiped.
    static int WipeEditorTemps(const std::wstring& dir, int passes = 3);

    // Fill the free space on the volume that contains `driveRoot` with
    // zeros (one large fill file, written in 1 MB chunks), flush, then
    // delete the fill file. This overwrites unallocated clusters where
    // deleted files (Untis.exe~RF*.TMP etc.) used to live. A ~32 MB
    // safety margin is left so the volume never actually hits 0 bytes
    // free while this runs.
    //
    // hwndOwner — parent window (currently unused, reserved).
    // Returns total bytes written, or -1 on a hard error before any
    // bytes were written. Cancelling still returns the bytes written so
    // far, not -1 — the fill file is always cleaned up before returning.
    static int64_t WipeFreeSpace(const std::wstring& driveRoot,
                                  HWND hwndOwner = nullptr);

    // Cooperative cancellation for an in-progress WipeFreeSpace call
    // running on another thread: makes it stop at the next chunk boundary
    // (it still flushes, closes and deletes its fill file before
    // returning) instead of running until the margin/disk-full point.
    static void RequestCancel();

    // Bytes written by the WipeFreeSpace call currently in progress (or
    // the last one that ran), for a caller to poll and show progress.
    static int64_t GetBytesWrittenSoFar();

private:
    // XOR-shift PRNG — good enough for forensic-grade overwrite.
    static uint64_t s_rng;
    static void     FillRandom(uint8_t* buf, size_t len);
    static void     FillZero  (uint8_t* buf, size_t len);

    // True if `name` (a bare file name) looks like an orphaned safe-save
    // temp file this tool should wipe.
    static bool IsOrphanTempName(const std::wstring& name);
    static int  WipeEditorTempsRecursive(const std::wstring& dir, int passes);

    static std::atomic<bool>    s_cancelRequested;
    static std::atomic<int64_t> s_bytesWritten;
};

} // namespace hexcore
