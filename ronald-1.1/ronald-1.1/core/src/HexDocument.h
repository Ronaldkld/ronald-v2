// HexDocument - internal engine behind the public C API (HexEditorCore.h).
//
// The document's content lives in a single in-memory buffer (m_data).
// This is what makes Sublime Text-style editing possible: bytes can be
// deleted (the file shrinks) or inserted (the file grows), not just
// overwritten in place. The trade-off is that the whole file must fit
// in memory - Open() enforces a sane size cap and reports
// HEXCORE_ERROR_FILE_TOO_LARGE rather than risking an out-of-memory
// crash on a multi-gigabyte file.
//
// Responsibilities:
//  - Open a file read-only, buffer its bytes, and never touch it on
//    disk again until Save.
//  - Serve/edit byte ranges from the buffer (overwrite, insert, delete).
//  - Provide unlimited undo/redo across all three edit kinds.
//  - Save safely: stream the buffer to a temp file, flush it, then
//    atomically replace the target so a crash/power-loss/disk-full
//    mid-write can never corrupt the original file.
//  - Linear hex/text search over the buffer.
//
// This class is pure C++ and knows nothing about the public C ABI; all
// wrapping happens in HexEditorCore.cpp.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hexcore {

enum class Status {
    Ok = 0,
    InvalidArgument,
    FileNotFound,
    AccessDenied,
    FileLocked,
    DiskFull,
    Io,
    OutOfRange,
    NothingToUndo,
    NothingToRedo,
    NotModified,
    PathTooLong,
    FileTooLarge,
    Unknown
};

struct SearchResult {
    uint64_t offset = 0;
    bool found = false;
};

class HexDocument {
public:
    HexDocument();
    ~HexDocument();

    HexDocument(const HexDocument&) = delete;
    HexDocument& operator=(const HexDocument&) = delete;

    // Opens `path`, reading its entire contents into memory. The file
    // is never written to by Open - only by Save/SaveAs.
    Status Open(const std::wstring& path);

    uint64_t GetSize() const { return m_data.size(); }
    bool IsModified() const { return m_modified; }
    const std::wstring& GetPath() const { return m_path; }

    Status ReadBytes(uint64_t offset, uint8_t* outBuffer, size_t length, size_t* outBytesRead) const;
    bool IsByteModified(uint64_t offset) const;

    // Pure in-place overwrite: offset..offset+length must already exist
    // (no implicit grow). Use InsertBytes to grow the file.
    Status WriteByte(uint64_t offset, uint8_t value);
    Status WriteBytes(uint64_t offset, const uint8_t* data, size_t length);

    // Removes `length` bytes starting at `offset`, shifting everything
    // after them left. Shrinks the file.
    Status DeleteRange(uint64_t offset, uint64_t length);

    // Inserts `length` bytes at `offset` (offset == GetSize() appends),
    // shifting everything at/after `offset` right. Grows the file.
    Status InsertBytes(uint64_t offset, const uint8_t* data, size_t length);

    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    Status Undo();
    Status Redo();

    Status Save();
    Status SaveAs(const std::wstring& newPath, bool rebindPath);

    SearchResult FindHex(const uint8_t* pattern, size_t patternLen, uint64_t startOffset, bool forward) const;
    SearchResult FindText(const std::string& text, uint64_t startOffset, bool caseSensitive, bool forward) const;

    const std::wstring& GetLastErrorMessage() const { return m_lastError; }

private:
    enum class EditKind { Overwrite, Insert, Delete };

    struct EditAction {
        EditKind kind;
        uint64_t offset;
        std::vector<uint8_t> oldBytes; // Overwrite: previous bytes. Delete: the bytes removed.
        std::vector<uint8_t> newBytes; // Overwrite: new bytes. Insert: the bytes inserted.
    };

    void ApplyUndo(const EditAction& action);
    void ApplyRedo(const EditAction& action);
    void RecordAndClearRedo(EditAction action);
    void RefreshModifiedFlag();

    Status DoSaveTo(const std::wstring& targetPath, bool targetIsCurrentFile, bool rebindPath);

    // Shared implementation behind FindHex/FindText: scans forward or
    // backward from startOffset, optionally case-insensitively.
    SearchResult SearchCore(const uint8_t* pattern, size_t patternLen, uint64_t startOffset,
                             bool forward, bool caseInsensitive) const;

    void SetLastError(const std::wstring& msg);
    static Status MapWin32Error(DWORD err);

    std::wstring m_path;
    std::vector<uint8_t> m_data;      // current, editable content
    std::vector<uint8_t> m_original;  // baseline snapshot as of last open/save, for modified-highlighting
    bool m_modified = false;

    std::vector<EditAction> m_undoStack;
    std::vector<EditAction> m_redoStack;

    mutable std::wstring m_lastError;
};

} // namespace hexcore
