/*
 * HexEditorCore - Public C ABI for the hex-editing engine DLL.
 *
 * This header is the single contract between HexEditorCore.dll and any
 * host application (the bundled Win32 GUI, the console example, or a
 * third-party host written in C/C++/C#/Delphi/etc).
 *
 * Design notes:
 *  - Pure C ABI (extern "C"), no C++ classes/STL crossing the boundary.
 *  - Opaque handle (HHEXDOC) hides all internal state.
 *  - Never touches the file on disk except when explicitly asked to
 *    (HexCore_Open reads only, HexCore_Save/HexCore_SaveAs write).
 *  - Designed for large files: reads are windowed, not "load whole file".
 */
#ifndef HEXEDITORCORE_H
#define HEXEDITORCORE_H

#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
  #if defined(HEXCORE_BUILD_DLL)
    #define HEXCORE_API __declspec(dllexport)
  #elif defined(HEXCORE_STATIC)
    #define HEXCORE_API
  #else
    #define HEXCORE_API __declspec(dllimport)
  #endif
  #define HEXCORE_CALL __cdecl
#else
  #define HEXCORE_API
  #define HEXCORE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle to an open document. */
typedef struct HexDocOpaque* HHEXDOC;

/* Status codes returned by (almost) every entry point. */
typedef enum HexCoreStatus {
    HEXCORE_OK = 0,
    HEXCORE_ERROR_INVALID_HANDLE = 1,
    HEXCORE_ERROR_INVALID_ARGUMENT = 2,
    HEXCORE_ERROR_FILE_NOT_FOUND = 3,
    HEXCORE_ERROR_ACCESS_DENIED = 4,
    HEXCORE_ERROR_FILE_LOCKED = 5,
    HEXCORE_ERROR_DISK_FULL = 6,
    HEXCORE_ERROR_IO = 7,
    HEXCORE_ERROR_OUT_OF_RANGE = 8,
    HEXCORE_ERROR_NOTHING_TO_UNDO = 9,
    HEXCORE_ERROR_NOTHING_TO_REDO = 10,
    HEXCORE_ERROR_NOT_MODIFIED = 11,
    HEXCORE_ERROR_PATH_TOO_LONG = 12,
    HEXCORE_ERROR_FILE_TOO_LARGE = 13,
    HEXCORE_ERROR_UNKNOWN = 999
} HexCoreStatus;

typedef struct HexSearchResult {
    uint64_t offset;   /* valid only when found != 0 */
    int      found;
} HexSearchResult;

/* ---------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------- */

/* Opens a file for hex editing: its entire contents are read into an
 * in-memory, freely editable buffer (bytes can be overwritten, deleted,
 * or inserted - Sublime Text-style), and the file itself is never
 * modified until HexCore_Save/HexCore_SaveAs is called. Files larger
 * than a sane in-memory limit (currently 512 MB) fail with
 * HEXCORE_ERROR_FILE_TOO_LARGE rather than risking exhausting memory.
 * On failure returns NULL and sets *outStatus (if non-NULL). */
HEXCORE_API HHEXDOC HEXCORE_CALL HexCore_Open(const wchar_t* path, HexCoreStatus* outStatus);

/* Closes the document and releases all resources. Does NOT save.
 * Passing NULL is a safe no-op. */
HEXCORE_API void HEXCORE_CALL HexCore_Close(HHEXDOC doc);

/* ---------------------------------------------------------------------
 * Information
 * ------------------------------------------------------------------- */

HEXCORE_API uint64_t HEXCORE_CALL HexCore_GetSize(HHEXDOC doc);
HEXCORE_API int      HEXCORE_CALL HexCore_IsModified(HHEXDOC doc);

/* Copies the current path (null-terminated) into buffer. Returns the
 * number of wchar_t written (excluding the terminator), or -1 if the
 * buffer is too small / doc is invalid. */
HEXCORE_API int HEXCORE_CALL HexCore_GetPath(HHEXDOC doc, wchar_t* buffer, int bufferChars);

/* ---------------------------------------------------------------------
 * Reading (transparently applies any pending, unsaved edits)
 * ------------------------------------------------------------------- */

HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_ReadBytes(
    HHEXDOC doc, uint64_t offset, uint8_t* outBuffer, size_t length, size_t* outBytesRead);

/* Non-zero if the byte at `offset` currently differs from what is on
 * disk (i.e. it has an unsaved edit). Used by the GUI to highlight it. */
HEXCORE_API int HEXCORE_CALL HexCore_IsByteModified(HHEXDOC doc, uint64_t offset);

/* ---------------------------------------------------------------------
 * Editing - changes are kept in memory until HexCore_Save/SaveAs.
 * ------------------------------------------------------------------- */

HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_WriteByte(HHEXDOC doc, uint64_t offset, uint8_t value);
HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_WriteBytes(HHEXDOC doc, uint64_t offset, const uint8_t* data, size_t length);

/* Deletes `length` bytes starting at `offset`, shrinking the file and
 * shifting everything after them left (like pressing Delete/Backspace
 * over a text selection). */
HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_DeleteRange(HHEXDOC doc, uint64_t offset, uint64_t length);

/* Inserts `length` bytes at `offset` (offset == HexCore_GetSize(doc) to
 * append), growing the file and shifting everything at/after `offset`
 * right. This is how pasted content of a different size than the
 * current selection is applied. */
HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_InsertBytes(HHEXDOC doc, uint64_t offset, const uint8_t* data, size_t length);

/* ---------------------------------------------------------------------
 * Undo / Redo (byte-level, unlimited depth within this session)
 * ------------------------------------------------------------------- */

HEXCORE_API int           HEXCORE_CALL HexCore_CanUndo(HHEXDOC doc);
HEXCORE_API int           HEXCORE_CALL HexCore_CanRedo(HHEXDOC doc);
HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_Undo(HHEXDOC doc);
HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_Redo(HHEXDOC doc);

/* ---------------------------------------------------------------------
 * Saving
 *
 * HexCore_Save() ALWAYS writes back to the exact path the document was
 * opened from (or last "saved as" to) - same name, same extension, same
 * folder. It never silently creates a new file.
 *
 * Both functions use a crash-safe write: content is streamed to a
 * temporary file next to the target, flushed to disk, and only then
 * atomically swapped into place. If anything fails partway (disk full,
 * permission revoked, etc.) the original file on disk is left
 * completely untouched and the in-memory edits are preserved so the
 * user can retry or "Save As" elsewhere.
 * ------------------------------------------------------------------- */

HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_Save(HHEXDOC doc);

/* Saves to a new path. If rebindPath is non-zero, the document adopts
 * newPath as its path for future HexCore_Save() calls (standard
 * "Save As" behaviour). If rebindPath is zero, this behaves as
 * "export a copy" and the document keeps editing its original path. */
HEXCORE_API HexCoreStatus HEXCORE_CALL HexCore_SaveAs(HHEXDOC doc, const wchar_t* newPath, int rebindPath);

/* ---------------------------------------------------------------------
 * Search
 * ------------------------------------------------------------------- */

HEXCORE_API HexSearchResult HEXCORE_CALL HexCore_FindHex(
    HHEXDOC doc, const uint8_t* pattern, size_t patternLen, uint64_t startOffset, int searchForward);

HEXCORE_API HexSearchResult HEXCORE_CALL HexCore_FindText(
    HHEXDOC doc, const char* text, uint64_t startOffset, int caseSensitive, int searchForward);

/* ---------------------------------------------------------------------
 * Utilities
 * ------------------------------------------------------------------- */

/* Parses a user-typed hex string ("DEADBEEF", "DE AD BE EF", "de-ad-be")
 * into raw bytes. Returns 1 on success, 0 if the string contains
 * anything other than hex digit pairs and separators (space/'-'/':').
 * outLen receives the number of bytes produced (<= bufferCap). */
HEXCORE_API int HEXCORE_CALL HexCore_ParseHexString(
    const char* hexString, uint8_t* outBuffer, size_t bufferCap, size_t* outLen);

/* Human-readable description of the last error for this document
 * (localized to English/Spanish is out of scope; message is in
 * English). Valid until the next call on the same handle. May be
 * called with doc == NULL to get a generic message for a status code
 * returned by HexCore_Open. */
HEXCORE_API const wchar_t* HEXCORE_CALL HexCore_GetLastErrorMessage(HHEXDOC doc);

/* Generic, doc-independent description of a status code. */
HEXCORE_API const wchar_t* HEXCORE_CALL HexCore_DescribeStatus(HexCoreStatus status);

#ifdef __cplusplus
}
#endif

#endif /* HEXEDITORCORE_H */
