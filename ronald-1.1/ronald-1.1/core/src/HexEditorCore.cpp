// Implementation of the public C ABI declared in HexEditorCore.h.
// Thin, defensive wrapper around HexDocument: validates every handle
// against a live registry so a misbehaving host gets a clean
// HEXCORE_ERROR_INVALID_HANDLE instead of a crash.
#include "hexcore/HexEditorCore.h"
#include "HexDocument.h"
#include "Logger.h"

#include <windows.h>
#include <mutex>
#include <unordered_set>
#include <algorithm>
#include <cctype>

using hexcore::HexDocument;
using hexcore::Status;

namespace {

std::mutex g_registryMutex;
std::unordered_set<HexDocument*> g_liveHandles;
std::wstring g_lastOpenFailureMessage;

HexDocument* Validate(HHEXDOC doc) {
    if (!doc) return nullptr;
    std::lock_guard<std::mutex> lock(g_registryMutex);
    auto* p = reinterpret_cast<HexDocument*>(doc);
    return g_liveHandles.count(p) ? p : nullptr;
}

HexCoreStatus ToC(Status s) {
    switch (s) {
        case Status::Ok: return HEXCORE_OK;
        case Status::InvalidArgument: return HEXCORE_ERROR_INVALID_ARGUMENT;
        case Status::FileNotFound: return HEXCORE_ERROR_FILE_NOT_FOUND;
        case Status::AccessDenied: return HEXCORE_ERROR_ACCESS_DENIED;
        case Status::FileLocked: return HEXCORE_ERROR_FILE_LOCKED;
        case Status::DiskFull: return HEXCORE_ERROR_DISK_FULL;
        case Status::Io: return HEXCORE_ERROR_IO;
        case Status::OutOfRange: return HEXCORE_ERROR_OUT_OF_RANGE;
        case Status::NothingToUndo: return HEXCORE_ERROR_NOTHING_TO_UNDO;
        case Status::NothingToRedo: return HEXCORE_ERROR_NOTHING_TO_REDO;
        case Status::NotModified: return HEXCORE_ERROR_NOT_MODIFIED;
        case Status::PathTooLong: return HEXCORE_ERROR_PATH_TOO_LONG;
        case Status::FileTooLarge: return HEXCORE_ERROR_FILE_TOO_LARGE;
        default: return HEXCORE_ERROR_UNKNOWN;
    }
}

} // namespace

extern "C" {

HHEXDOC HEXCORE_CALL HexCore_Open(const wchar_t* path, HexCoreStatus* outStatus) {
    if (!path) {
        if (outStatus) *outStatus = HEXCORE_ERROR_INVALID_ARGUMENT;
        return nullptr;
    }

    auto* doc = new HexDocument();
    Status st = doc->Open(path);
    if (st != Status::Ok) {
        {
            std::lock_guard<std::mutex> lock(g_registryMutex);
            g_lastOpenFailureMessage = doc->GetLastErrorMessage();
        }
        delete doc;
        if (outStatus) *outStatus = ToC(st);
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        g_liveHandles.insert(doc);
    }
    if (outStatus) *outStatus = HEXCORE_OK;
    return reinterpret_cast<HHEXDOC>(doc);
}

void HEXCORE_CALL HexCore_Close(HHEXDOC doc) {
    if (!doc) return;
    auto* p = reinterpret_cast<HexDocument*>(doc);
    {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        auto it = g_liveHandles.find(p);
        if (it == g_liveHandles.end()) return;
        g_liveHandles.erase(it);
    }
    delete p;
}

uint64_t HEXCORE_CALL HexCore_GetSize(HHEXDOC doc) {
    auto* p = Validate(doc);
    return p ? p->GetSize() : 0;
}

int HEXCORE_CALL HexCore_IsModified(HHEXDOC doc) {
    auto* p = Validate(doc);
    return p ? (p->IsModified() ? 1 : 0) : 0;
}

int HEXCORE_CALL HexCore_GetPath(HHEXDOC doc, wchar_t* buffer, int bufferChars) {
    auto* p = Validate(doc);
    if (!p || !buffer || bufferChars <= 0) return -1;
    const std::wstring& path = p->GetPath();
    if (static_cast<int>(path.size()) >= bufferChars) return -1;
    wcscpy_s(buffer, static_cast<size_t>(bufferChars), path.c_str());
    return static_cast<int>(path.size());
}

HexCoreStatus HEXCORE_CALL HexCore_ReadBytes(HHEXDOC doc, uint64_t offset, uint8_t* outBuffer, size_t length, size_t* outBytesRead) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    size_t dummy;
    return ToC(p->ReadBytes(offset, outBuffer, length, outBytesRead ? outBytesRead : &dummy));
}

int HEXCORE_CALL HexCore_IsByteModified(HHEXDOC doc, uint64_t offset) {
    auto* p = Validate(doc);
    return p ? (p->IsByteModified(offset) ? 1 : 0) : 0;
}

HexCoreStatus HEXCORE_CALL HexCore_WriteByte(HHEXDOC doc, uint64_t offset, uint8_t value) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->WriteByte(offset, value));
}

HexCoreStatus HEXCORE_CALL HexCore_WriteBytes(HHEXDOC doc, uint64_t offset, const uint8_t* data, size_t length) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->WriteBytes(offset, data, length));
}

HexCoreStatus HEXCORE_CALL HexCore_DeleteRange(HHEXDOC doc, uint64_t offset, uint64_t length) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->DeleteRange(offset, length));
}

HexCoreStatus HEXCORE_CALL HexCore_InsertBytes(HHEXDOC doc, uint64_t offset, const uint8_t* data, size_t length) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->InsertBytes(offset, data, length));
}

int HEXCORE_CALL HexCore_CanUndo(HHEXDOC doc) {
    auto* p = Validate(doc);
    return p ? (p->CanUndo() ? 1 : 0) : 0;
}

int HEXCORE_CALL HexCore_CanRedo(HHEXDOC doc) {
    auto* p = Validate(doc);
    return p ? (p->CanRedo() ? 1 : 0) : 0;
}

HexCoreStatus HEXCORE_CALL HexCore_Undo(HHEXDOC doc) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->Undo());
}

HexCoreStatus HEXCORE_CALL HexCore_Redo(HHEXDOC doc) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->Redo());
}

HexCoreStatus HEXCORE_CALL HexCore_Save(HHEXDOC doc) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    return ToC(p->Save());
}

HexCoreStatus HEXCORE_CALL HexCore_SaveAs(HHEXDOC doc, const wchar_t* newPath, int rebindPath) {
    auto* p = Validate(doc);
    if (!p) return HEXCORE_ERROR_INVALID_HANDLE;
    if (!newPath) return HEXCORE_ERROR_INVALID_ARGUMENT;
    return ToC(p->SaveAs(newPath, rebindPath != 0));
}

HexSearchResult HEXCORE_CALL HexCore_FindHex(HHEXDOC doc, const uint8_t* pattern, size_t patternLen, uint64_t startOffset, int searchForward) {
    HexSearchResult out{0, 0};
    auto* p = Validate(doc);
    if (!p) return out;
    auto r = p->FindHex(pattern, patternLen, startOffset, searchForward != 0);
    out.offset = r.offset;
    out.found = r.found ? 1 : 0;
    return out;
}

HexSearchResult HEXCORE_CALL HexCore_FindText(HHEXDOC doc, const char* text, uint64_t startOffset, int caseSensitive, int searchForward) {
    HexSearchResult out{0, 0};
    auto* p = Validate(doc);
    if (!p || !text) return out;
    auto r = p->FindText(std::string(text), startOffset, caseSensitive != 0, searchForward != 0);
    out.offset = r.offset;
    out.found = r.found ? 1 : 0;
    return out;
}

int HEXCORE_CALL HexCore_ParseHexString(const char* hexString, uint8_t* outBuffer, size_t bufferCap, size_t* outLen) {
    if (outLen) *outLen = 0;
    if (!hexString) return 0;

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string digits;
    for (const char* c = hexString; *c; ++c) {
        if (*c == ' ' || *c == '\t' || *c == '-' || *c == ':' || *c == ',') continue;
        if (hexVal(*c) < 0) return 0; // invalid character
        digits.push_back(*c);
    }
    if (digits.empty() || (digits.size() % 2) != 0) return 0;

    size_t byteCount = digits.size() / 2;
    if (!outBuffer) {
        if (outLen) *outLen = byteCount;
        return 1;
    }
    if (byteCount > bufferCap) return 0;

    for (size_t i = 0; i < byteCount; ++i) {
        int hi = hexVal(digits[i * 2]);
        int lo = hexVal(digits[i * 2 + 1]);
        outBuffer[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    if (outLen) *outLen = byteCount;
    return 1;
}

const wchar_t* HEXCORE_CALL HexCore_GetLastErrorMessage(HHEXDOC doc) {
    auto* p = Validate(doc);
    if (p) return p->GetLastErrorMessage().c_str();
    std::lock_guard<std::mutex> lock(g_registryMutex);
    return g_lastOpenFailureMessage.c_str();
}

const wchar_t* HEXCORE_CALL HexCore_DescribeStatus(HexCoreStatus status) {
    switch (status) {
        case HEXCORE_OK: return L"Success.";
        case HEXCORE_ERROR_INVALID_HANDLE: return L"Invalid or closed document handle.";
        case HEXCORE_ERROR_INVALID_ARGUMENT: return L"Invalid argument.";
        case HEXCORE_ERROR_FILE_NOT_FOUND: return L"The file or path was not found.";
        case HEXCORE_ERROR_ACCESS_DENIED: return L"Access to the file was denied. Check permissions.";
        case HEXCORE_ERROR_FILE_LOCKED: return L"The file is locked by another program.";
        case HEXCORE_ERROR_DISK_FULL: return L"There is not enough free disk space.";
        case HEXCORE_ERROR_IO: return L"An input/output error occurred.";
        case HEXCORE_ERROR_OUT_OF_RANGE: return L"The requested offset is outside the file.";
        case HEXCORE_ERROR_NOTHING_TO_UNDO: return L"There is nothing to undo.";
        case HEXCORE_ERROR_NOTHING_TO_REDO: return L"There is nothing to redo.";
        case HEXCORE_ERROR_NOT_MODIFIED: return L"The file has no unsaved changes.";
        case HEXCORE_ERROR_PATH_TOO_LONG: return L"The file path is too long.";
        case HEXCORE_ERROR_FILE_TOO_LARGE: return L"The file is too large for this editor's in-memory editing limit.";
        default: return L"An unknown error occurred.";
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        std::lock_guard<std::mutex> lock(g_registryMutex);
        for (auto* p : g_liveHandles) delete p;
        g_liveHandles.clear();
    }
    return TRUE;
}

} // extern "C"
