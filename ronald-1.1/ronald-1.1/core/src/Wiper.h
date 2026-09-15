#pragma once
#include <windows.h>
#include <string>
#include <cstdint>
#include <atomic>
#include <vector>

#include "FatVolume.h"

namespace hexcore {

// Secure file/space wiper.
//
// WipeFile      — overwrites every byte of a file with (random / zero /
//                 random) passes, then deletes it.  After this, forensic
//                 tools that scan unallocated clusters find only garbage.
//
// WipeEditorTemps — recursively walks a chosen folder/drive. In every
//                   folder it visits it (1) wipes any still-present
//                   orphaned safe-save temp files - this editor's own
//                   hxe*.TMP files, and the <name>~RFxxxxxxxx.TMP backup
//                   files the Win32 ReplaceFile API can leave behind on
//                   FAT/FAT32/exFAT volumes - and, when the target is a
//                   FAT32 volume and the process has Administrator
//                   rights, (2) uses FatVolume to directly zero out that
//                   folder's own stale (already-deleted) directory-entry
//                   slots' name/size/date content at the raw volume
//                   level. Step 2 is what actually clears the listing a
//                   tool like FTK Imager shows for ANY previously
//                   deleted file in that folder - a PDF, an EXE,
//                   anything - not just temp files: wiping free space
//                   only ever overwrites file *content*, never the
//                   parent folder's own stale entry for a deleted file,
//                   and ordinary file-API tricks (creating/deleting
//                   throwaway files to try to force slot reuse) were
//                   tried and confirmed, in real-world testing, not to
//                   reliably clear that listing at all.
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

    // Recursively scan `dir` and its subfolders: wipe+delete orphaned
    // safe-save temp files, and recycle every visited folder's freed
    // directory-entry slots (see class comment). Cancellable via
    // RequestCancel(); poll GetTempsFilesWiped()/GetTempsFoldersDone()
    // for progress. Returns the number of temp files wiped.
    //
    // `deepScanAllocated` extends the FAT32 pass's orphan-directory sweep
    // to clusters the FAT currently marks IN USE by some other file or
    // directory, not just free ones (see FatVolume::WipeOrphanedDirectories's
    // own parameter of the same name for the full explanation). This is a
    // materially different risk from every other mode this class has -
    // false positives here mean writing into real, currently-live data,
    // not just wasted time - and must only ever be set true from a UI
    // path that has separately made that trade-off explicit to the user;
    // never as a default or in response to anything but a direct request.
    //
    // `skipOrphanSweep`, when true, stops after the ordinary pointer-
    // following pass (live directory tree, plus any whole-deleted
    // subfolder still reachable from it) and never runs the full-volume
    // orphan-directory sweep at all - the slow part, since it has to
    // touch every free cluster on the drive at least once. Trades away
    // finding a directory a broken FAT chain has cut loose (a folder FTK
    // Imager's own deeper recovery can still show) for running in
    // roughly the time it takes to walk the live tree once. Ignored
    // (the full sweep still runs) when deepScanAllocated is also true,
    // since that mode's entire point is the thorough pass.
    static int WipeEditorTemps(const std::wstring& dir, int passes = 3, bool deepScanAllocated = false,
                                bool skipOrphanSweep = false);

    static int GetTempsFilesWiped();
    static int GetTempsFoldersDone();
    static int GetDirEntriesWiped();
    // Of the directories the raw FAT32 pass actually visited, how many
    // hit a hard read error partway through (and so contributed nothing,
    // silently, rather than genuinely having zero stale entries).
    static int GetDirsVisitedRaw();
    static int GetDirReadErrors();
    // Diagnostic: how many subdirectory entries the raw FAT32 pass found
    // in total (see FatVolume::LastSubdirsFound). If this is bigger than
    // GetDirsVisitedRaw(), the walk found more folders than it visited
    // (something stopped it partway); if it matches, the scan itself
    // never saw any more subfolders past what it visited.
    static int GetSubdirsFound();
    // Live progress for the full-volume orphan-directory sweep (the
    // slowest phase, since it has to touch every free cluster at least
    // once): clusters scanned so far vs. the volume's total data
    // clusters, so a caller can show a real percentage instead of this
    // phase looking stalled. Both are 0 before/without that phase.
    static int64_t GetOrphanClustersScanned();
    static int64_t GetOrphanTotalClusters();
    // GetTickCount64() value when the orphan sweep started, so a caller
    // can compute elapsed time and, from that plus clusters-scanned vs.
    // total, an ETA - reading a large volume's whole free space at real
    // USB speed can legitimately take a while, and a percentage alone
    // doesn't say whether that means two more minutes or twenty.
    static uint64_t GetOrphanStartTick();

    // Diagnostic-only, read-only, never writes anything: resolves `dir`
    // to its FAT32 cluster (by name, from the volume root - this is the
    // OLD path-based lookup the wipe itself no longer relies on, fine
    // here since nothing destructive follows a wrong answer) and returns
    // FatVolume::DumpDirectoryRaw's unfiltered listing of every 32-byte
    // slot found in its full cluster chain. An error is returned as a
    // plain "ERROR: ..." string rather than thrown.
    static std::wstring DumpFolderRaw(const std::wstring& dir);

    // What happened with the raw FAT32 directory-entry pass during the
    // most recent WipeEditorTemps call.
    enum class FatWipeStatus {
        NotAttempted,  // WipeEditorTemps hasn't run yet
        NotFat32,      // the target volume isn't FAT32 - nothing to do here
        NeedsAdmin,    // opening the raw volume for write access failed;
                       // re-run this program as Administrator
        VolumeInUse,   // opened the volume, but FSCTL_LOCK_VOLUME failed -
                       // close other programs/Explorer windows using that
                       // drive (including this editor's own open tabs on
                       // it) and try again
        Ran,           // the raw directory-entry pass ran (see
                       // GetDirEntriesWiped() for how much it found)
    };
    static FatWipeStatus GetFatWipeStatus();

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
    // Phase 1 of WipeEditorTemps: ordinary file-API walk only (find +
    // wipe orphaned temp files). Must finish - and close every handle it
    // used - before Phase 2 locks the volume (see WipeEditorTemps).
    static int  WipeEditorTempsRecursive(const std::wstring& dir, int passes);

    // True once RequestCancel() was called, or the WipeEditorTemps call
    // in progress has run past its overall time budget - checked
    // throughout the recursive walk so a folder-heavy tree (hundreds of
    // subfolders, each with its own recycling pass) can't run away.
    static bool ShouldStop();

    static std::atomic<bool>     s_cancelRequested;
    static std::atomic<int64_t>  s_bytesWritten;
    static std::atomic<int>      s_tempsFilesWiped;
    static std::atomic<int>      s_tempsFoldersDone;
    static std::atomic<int>      s_dirEntriesWiped;
    static std::atomic<int>      s_dirsVisitedRaw;
    static std::atomic<int>      s_dirReadErrors;
    static std::atomic<int>      s_subdirsFound;
    static std::atomic<int64_t>  s_orphanClustersScanned;
    static std::atomic<int64_t>  s_orphanTotalClusters;
    static std::atomic<uint64_t> s_orphanStartTick;
    static std::atomic<uint64_t> s_deadlineTick; // GetTickCount64() value to stop by; 0 = no deadline
    static FatWipeStatus         s_fatWipeStatus;
};

} // namespace hexcore
