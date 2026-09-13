#include "HexDocument.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace hexcore {

namespace {

// Whole files live in memory now (that's what makes insert/delete -
// growing/shrinking a file, Sublime Text-style - possible). Cap how
// large a file we'll buffer so a huge file fails cleanly instead of
// exhausting memory.
constexpr uint64_t kMaxBufferedFileSize = 512ull * 1024 * 1024; // 512 MB
constexpr uint64_t kCopyChunk = 1024ull * 1024ull;

std::wstring FormatWin32Error(DWORD err) {
    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg;
    if (len > 0 && buf) {
        msg.assign(buf, len);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    } else {
        msg = L"Unknown system error.";
    }
    if (buf) LocalFree(buf);
    return msg;
}

bool BytesEqual(const uint8_t* a, const uint8_t* b, size_t n, bool caseInsensitive) {
    if (!caseInsensitive) return std::memcmp(a, b, n) == 0;
    for (size_t k = 0; k < n; ++k) {
        uint8_t ca = a[k];
        uint8_t cb = b[k];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<uint8_t>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<uint8_t>(cb + 32);
        if (ca != cb) return false;
    }
    return true;
}

// ReplaceFileW relies on NTFS's atomic file-replace support. On FAT/
// FAT32/exFAT volumes (which lack it), Windows falls back to an internal
// copy-based emulation that can leave an orphaned "<name>~RFxxxxxxxx.TMP"
// backup file behind next to the target. MoveFileExW's plain replace
// doesn't have this failure mode, so on a non-NTFS volume we use that
// directly instead of trying ReplaceFileW first.
bool IsNtfsVolume(const std::wstring& path) {
    wchar_t root[MAX_PATH] = {0};
    if (!GetVolumePathNameW(path.c_str(), root, MAX_PATH)) return true; // unknown: assume NTFS
    wchar_t fsName[32] = {0};
    if (!GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, fsName, 32)) return true;
    return _wcsicmp(fsName, L"NTFS") == 0;
}

} // namespace

HexDocument::HexDocument() = default;
HexDocument::~HexDocument() = default;

void HexDocument::SetLastError(const std::wstring& msg) {
    m_lastError = msg;
}

Status HexDocument::MapWin32Error(DWORD err) {
    switch (err) {
        case ERROR_SUCCESS:
            return Status::Ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return Status::FileNotFound;
        case ERROR_ACCESS_DENIED:
            return Status::AccessDenied;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
            return Status::FileLocked;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return Status::DiskFull;
        case ERROR_FILENAME_EXCED_RANGE:
        case ERROR_BUFFER_OVERFLOW:
            return Status::PathTooLong;
        case ERROR_INVALID_PARAMETER:
        case ERROR_INVALID_NAME:
            return Status::InvalidArgument;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return Status::FileTooLarge;
        default:
            return Status::Io;
    }
}

Status HexDocument::Open(const std::wstring& path) {
    if (path.empty()) {
        SetLastError(L"No file path was given.");
        return Status::InvalidArgument;
    }
    if (path.size() >= 32760) {
        SetLastError(L"The path is too long.");
        return Status::PathTooLong;
    }

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        Status st = MapWin32Error(err);
        SetLastError(L"Could not open \"" + path + L"\": " + FormatWin32Error(err));
        Logger::LogError(L"HexDocument::Open", m_lastError);
        return st;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        DWORD err = GetLastError();
        CloseHandle(h);
        SetLastError(L"Could not determine the size of \"" + path + L"\": " + FormatWin32Error(err));
        Logger::LogError(L"HexDocument::Open", m_lastError);
        return MapWin32Error(err);
    }

    if (static_cast<uint64_t>(size.QuadPart) > kMaxBufferedFileSize) {
        CloseHandle(h);
        SetLastError(L"\"" + path + L"\" is larger than this editor's " +
                     std::to_wstring(kMaxBufferedFileSize / (1024 * 1024)) +
                     L" MB limit (the whole file is edited in memory).");
        Logger::LogError(L"HexDocument::Open", m_lastError);
        return Status::FileTooLarge;
    }

    std::vector<uint8_t> data(static_cast<size_t>(size.QuadPart));
    uint64_t remaining = data.size();
    uint8_t* dst = data.data();
    while (remaining > 0) {
        DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(remaining, kCopyChunk));
        DWORD bytesRead = 0;
        if (!ReadFile(h, dst, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            DWORD err = GetLastError();
            CloseHandle(h);
            SetLastError(L"Could not read \"" + path + L"\": " + FormatWin32Error(err));
            Logger::LogError(L"HexDocument::Open", m_lastError);
            return MapWin32Error(err);
        }
        dst += bytesRead;
        remaining -= bytesRead;
    }
    CloseHandle(h);

    m_path = path;
    m_data = std::move(data);
    m_original = m_data;
    m_modified = false;
    m_undoStack.clear();
    m_redoStack.clear();
    m_lastError.clear();
    return Status::Ok;
}

Status HexDocument::ReadBytes(uint64_t offset, uint8_t* outBuffer, size_t length, size_t* outBytesRead) const {
    if (outBytesRead) *outBytesRead = 0;
    if (!outBuffer && length > 0) return Status::InvalidArgument;
    if (length == 0) return Status::Ok;
    if (offset >= m_data.size()) return Status::OutOfRange;

    size_t toRead = static_cast<size_t>(std::min<uint64_t>(m_data.size() - offset, static_cast<uint64_t>(length)));
    std::memcpy(outBuffer, m_data.data() + offset, toRead);
    if (outBytesRead) *outBytesRead = toRead;
    return Status::Ok;
}

bool HexDocument::IsByteModified(uint64_t offset) const {
    if (offset >= m_data.size()) return false;
    if (offset >= m_original.size()) return true; // content beyond the old baseline is new
    return m_data[static_cast<size_t>(offset)] != m_original[static_cast<size_t>(offset)];
}

void HexDocument::RefreshModifiedFlag() {
    m_modified = (m_data != m_original);
}

Status HexDocument::WriteByte(uint64_t offset, uint8_t value) {
    return WriteBytes(offset, &value, 1);
}

Status HexDocument::WriteBytes(uint64_t offset, const uint8_t* data, size_t length) {
    if (!data && length > 0) return Status::InvalidArgument;
    if (length == 0) return Status::Ok;
    if (offset > m_data.size() || length > m_data.size() - offset) {
        SetLastError(L"The write range extends beyond the end of the file.");
        return Status::OutOfRange;
    }

    auto* dst = m_data.data() + offset;
    if (std::memcmp(dst, data, length) == 0) return Status::Ok; // no-op

    EditAction action;
    action.kind = EditKind::Overwrite;
    action.offset = offset;
    action.oldBytes.assign(dst, dst + length);
    action.newBytes.assign(data, data + length);
    std::memcpy(dst, data, length);

    RecordAndClearRedo(std::move(action));
    RefreshModifiedFlag();
    return Status::Ok;
}

Status HexDocument::DeleteRange(uint64_t offset, uint64_t length) {
    if (length == 0) return Status::Ok;
    if (offset >= m_data.size()) {
        SetLastError(L"The delete range is outside the file.");
        return Status::OutOfRange;
    }
    length = std::min<uint64_t>(length, m_data.size() - offset);

    auto first = m_data.begin() + static_cast<ptrdiff_t>(offset);
    auto last = first + static_cast<ptrdiff_t>(length);

    EditAction action;
    action.kind = EditKind::Delete;
    action.offset = offset;
    action.oldBytes.assign(first, last);
    m_data.erase(first, last);

    RecordAndClearRedo(std::move(action));
    RefreshModifiedFlag();
    return Status::Ok;
}

Status HexDocument::InsertBytes(uint64_t offset, const uint8_t* data, size_t length) {
    if (!data && length > 0) return Status::InvalidArgument;
    if (length == 0) return Status::Ok;
    if (offset > m_data.size()) {
        SetLastError(L"The insert position is outside the file.");
        return Status::OutOfRange;
    }
    if (m_data.size() + length > kMaxBufferedFileSize) {
        SetLastError(L"This would grow the file past this editor's " +
                     std::to_wstring(kMaxBufferedFileSize / (1024 * 1024)) + L" MB limit.");
        return Status::FileTooLarge;
    }

    EditAction action;
    action.kind = EditKind::Insert;
    action.offset = offset;
    action.newBytes.assign(data, data + length);
    m_data.insert(m_data.begin() + static_cast<ptrdiff_t>(offset), data, data + length);

    RecordAndClearRedo(std::move(action));
    RefreshModifiedFlag();
    return Status::Ok;
}

void HexDocument::RecordAndClearRedo(EditAction action) {
    m_undoStack.push_back(std::move(action));
    m_redoStack.clear();
}

void HexDocument::ApplyUndo(const EditAction& action) {
    switch (action.kind) {
        case EditKind::Overwrite:
            std::memcpy(m_data.data() + action.offset, action.oldBytes.data(), action.oldBytes.size());
            break;
        case EditKind::Insert: {
            auto first = m_data.begin() + static_cast<ptrdiff_t>(action.offset);
            m_data.erase(first, first + static_cast<ptrdiff_t>(action.newBytes.size()));
            break;
        }
        case EditKind::Delete:
            m_data.insert(m_data.begin() + static_cast<ptrdiff_t>(action.offset),
                          action.oldBytes.begin(), action.oldBytes.end());
            break;
    }
}

void HexDocument::ApplyRedo(const EditAction& action) {
    switch (action.kind) {
        case EditKind::Overwrite:
            std::memcpy(m_data.data() + action.offset, action.newBytes.data(), action.newBytes.size());
            break;
        case EditKind::Insert:
            m_data.insert(m_data.begin() + static_cast<ptrdiff_t>(action.offset),
                          action.newBytes.begin(), action.newBytes.end());
            break;
        case EditKind::Delete: {
            auto first = m_data.begin() + static_cast<ptrdiff_t>(action.offset);
            m_data.erase(first, first + static_cast<ptrdiff_t>(action.oldBytes.size()));
            break;
        }
    }
}

Status HexDocument::Undo() {
    if (m_undoStack.empty()) return Status::NothingToUndo;
    EditAction action = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    ApplyUndo(action);
    RefreshModifiedFlag();
    m_redoStack.push_back(std::move(action));
    return Status::Ok;
}

Status HexDocument::Redo() {
    if (m_redoStack.empty()) return Status::NothingToRedo;
    EditAction action = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    ApplyRedo(action);
    RefreshModifiedFlag();
    m_undoStack.push_back(std::move(action));
    return Status::Ok;
}

Status HexDocument::DoSaveTo(const std::wstring& targetPath, bool targetIsCurrentFile, bool rebindPath) {
    size_t slash = targetPath.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"." : targetPath.substr(0, slash);
    if (dir.empty()) dir = L".";

    wchar_t tempName[MAX_PATH];
    if (GetTempFileNameW(dir.c_str(), L"hxe", 0, tempName) == 0) {
        DWORD err = GetLastError();
        SetLastError(L"Could not create a temporary file next to the target: " + FormatWin32Error(err));
        Logger::LogError(L"HexDocument::Save", m_lastError);
        return MapWin32Error(err);
    }
    std::wstring tempPath = tempName;

    HANDLE hTemp = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hTemp == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        DeleteFileW(tempPath.c_str());
        SetLastError(L"Could not create a temporary file for the safe save: " + FormatWin32Error(err));
        Logger::LogError(L"HexDocument::Save", m_lastError);
        return MapWin32Error(err);
    }

    uint64_t remaining = m_data.size();
    const uint8_t* src = m_data.data();
    bool writeFailed = false;
    DWORD writeErr = ERROR_SUCCESS;
    while (remaining > 0) {
        DWORD toWrite = static_cast<DWORD>(std::min<uint64_t>(remaining, kCopyChunk));
        DWORD bytesWritten = 0;
        if (!WriteFile(hTemp, src, toWrite, &bytesWritten, nullptr) || bytesWritten != toWrite) {
            writeFailed = true;
            writeErr = GetLastError();
            break;
        }
        src += toWrite;
        remaining -= toWrite;
    }

    if (writeFailed) {
        CloseHandle(hTemp);
        DeleteFileW(tempPath.c_str());
        SetLastError(L"Writing failed while saving (original file left untouched): " + FormatWin32Error(writeErr));
        Logger::LogError(L"HexDocument::Save", m_lastError);
        return MapWin32Error(writeErr);
    }

    if (!FlushFileBuffers(hTemp)) {
        DWORD err = GetLastError();
        CloseHandle(hTemp);
        DeleteFileW(tempPath.c_str());
        SetLastError(L"Could not flush data to disk (original file left untouched): " + FormatWin32Error(err));
        Logger::LogError(L"HexDocument::Save", m_lastError);
        return MapWin32Error(err);
    }
    CloseHandle(hTemp);

    bool destExists = (GetFileAttributesW(targetPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    BOOL replaced = FALSE;
    DWORD replaceErr = ERROR_SUCCESS;

    if (destExists) {
        // ReplaceFileW is preferred on NTFS (atomic, and what the safe-
        // save design relies on), but skipped entirely on FAT/FAT32/
        // exFAT: there it can silently leave a "~RFxxxxxxxx.TMP" backup
        // orphan behind instead of cleaning up (see IsNtfsVolume above).
        if (IsNtfsVolume(targetPath)) {
            replaced = ReplaceFileW(targetPath.c_str(), tempPath.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
            replaceErr = GetLastError();
        }
        if (!replaced) {
            replaced = MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            replaceErr = GetLastError();
        }
    } else {
        replaced = MoveFileExW(tempPath.c_str(), targetPath.c_str(), MOVEFILE_WRITE_THROUGH);
        replaceErr = GetLastError();
    }

    if (!replaced) {
        // The original file (if it existed) is guaranteed untouched by
        // ReplaceFileW/MoveFileExW semantics on failure. We deliberately
        // do NOT delete tempPath: it holds the user's edits safely.
        SetLastError(L"Could not replace \"" + targetPath + L"\". Your edits were preserved in \"" +
                     tempPath + L"\". " + FormatWin32Error(replaceErr));
        Logger::LogError(L"HexDocument::Save", m_lastError);
        return MapWin32Error(replaceErr);
    }

    if (targetIsCurrentFile || rebindPath) {
        m_path = targetPath;
        m_original = m_data;
        m_modified = false;
        m_lastError.clear();
        // Undo/Redo stacks are intentionally preserved across a save.
    }

    return Status::Ok;
}

Status HexDocument::Save() {
    if (!m_modified) return Status::NotModified;
    return DoSaveTo(m_path, true, true);
}

Status HexDocument::SaveAs(const std::wstring& newPath, bool rebindPath) {
    if (newPath.empty()) {
        SetLastError(L"No destination path was given.");
        return Status::InvalidArgument;
    }

    bool sameFile = false;
    std::vector<wchar_t> fullCurrent(32768), fullNew(32768);
    DWORD lenCur = GetFullPathNameW(m_path.c_str(), 32768, fullCurrent.data(), nullptr);
    DWORD lenNew = GetFullPathNameW(newPath.c_str(), 32768, fullNew.data(), nullptr);
    if (lenCur > 0 && lenCur < 32768 && lenNew > 0 && lenNew < 32768) {
        sameFile = (_wcsicmp(fullCurrent.data(), fullNew.data()) == 0);
    }

    if (sameFile) {
        return DoSaveTo(m_path, true, true);
    }
    return DoSaveTo(newPath, false, rebindPath);
}

SearchResult HexDocument::SearchCore(const uint8_t* pattern, size_t patternLen, uint64_t startOffset,
                                      bool forward, bool caseInsensitive) const {
    SearchResult result;
    uint64_t size = m_data.size();
    if (!pattern || patternLen == 0 || size == 0 || patternLen > size) return result;
    const uint8_t* base = m_data.data();

    if (forward) {
        for (uint64_t pos = startOffset; pos + patternLen <= size; ++pos) {
            if (BytesEqual(base + pos, pattern, patternLen, caseInsensitive)) {
                result.offset = pos;
                result.found = true;
                return result;
            }
        }
    } else {
        uint64_t hi = std::min<uint64_t>(startOffset, size);
        if (hi < patternLen) return result;
        for (int64_t pos = static_cast<int64_t>(hi - patternLen); pos >= 0; --pos) {
            if (BytesEqual(base + pos, pattern, patternLen, caseInsensitive)) {
                result.offset = static_cast<uint64_t>(pos);
                result.found = true;
                return result;
            }
        }
    }
    return result;
}

SearchResult HexDocument::FindHex(const uint8_t* pattern, size_t patternLen, uint64_t startOffset, bool forward) const {
    return SearchCore(pattern, patternLen, startOffset, forward, /*caseInsensitive=*/false);
}

SearchResult HexDocument::FindText(const std::string& text, uint64_t startOffset, bool caseSensitive, bool forward) const {
    if (text.empty()) return SearchResult{};
    const uint8_t* pattern = reinterpret_cast<const uint8_t*>(text.data());
    return SearchCore(pattern, text.size(), startOffset, forward, /*caseInsensitive=*/!caseSensitive);
}

} // namespace hexcore
