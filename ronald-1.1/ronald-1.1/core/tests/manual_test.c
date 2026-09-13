/* Manual runtime smoke test for HexEditorCore.dll.
 * Links directly against the DLL's import library and exercises:
 * open, read, write, undo/redo, hex/text search, and a safe save,
 * then verifies the saved file on disk byte-for-byte.
 *
 * Not a unit test framework - just asserts and exits non-zero on
 * first failure, printing a PASS/FAIL trail to stdout.
 */
#include <hexcore/HexEditorCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        wprintf(L"FAIL: %hs (line %d)\n", msg, __LINE__); \
        g_failures++; \
    } else { \
        wprintf(L"PASS: %hs\n", msg); \
    } \
} while (0)

int wmain(void) {
    const wchar_t* testPath = L"test_sample.bin";

    /* Create a 32-byte sample file: 00 01 02 ... 1F */
    FILE* f = _wfopen(testPath, L"wb");
    if (!f) { wprintf(L"Could not create sample file\n"); return 1; }
    for (int i = 0; i < 32; ++i) fputc(i, f);
    fclose(f);

    HexCoreStatus st;
    HHEXDOC doc = HexCore_Open(testPath, &st);
    CHECK(doc != NULL && st == HEXCORE_OK, "Open sample file");
    CHECK(HexCore_GetSize(doc) == 32, "File size is 32 bytes");
    CHECK(HexCore_IsModified(doc) == 0, "Freshly opened file is not modified");

    uint8_t buf[32] = {0};
    size_t got = 0;
    HexCoreStatus rst = HexCore_ReadBytes(doc, 0, buf, 32, &got);
    CHECK(rst == HEXCORE_OK && got == 32 && buf[5] == 5, "Read full contents back correctly");

    /* Edit byte 5 from 0x05 to 0xAA */
    CHECK(HexCore_WriteByte(doc, 5, 0xAA) == HEXCORE_OK, "Write byte at offset 5");
    CHECK(HexCore_IsModified(doc) == 1, "Document reports modified after edit");
    CHECK(HexCore_IsByteModified(doc, 5) == 1, "Byte 5 flagged as modified");
    CHECK(HexCore_IsByteModified(doc, 4) == 0, "Neighbouring byte not flagged");

    HexCore_ReadBytes(doc, 0, buf, 32, &got);
    CHECK(buf[5] == 0xAA, "Overlay applied on read");
    CHECK(buf[4] == 4, "Untouched bytes unaffected");

    /* Undo */
    CHECK(HexCore_CanUndo(doc) == 1, "CanUndo is true after edit");
    CHECK(HexCore_Undo(doc) == HEXCORE_OK, "Undo succeeds");
    CHECK(HexCore_IsModified(doc) == 0, "Modified flag clears after undoing only edit");
    HexCore_ReadBytes(doc, 0, buf, 32, &got);
    CHECK(buf[5] == 5, "Value restored after undo");

    /* Redo */
    CHECK(HexCore_CanRedo(doc) == 1, "CanRedo is true after undo");
    CHECK(HexCore_Redo(doc) == HEXCORE_OK, "Redo succeeds");
    HexCore_ReadBytes(doc, 0, buf, 32, &got);
    CHECK(buf[5] == 0xAA, "Value re-applied after redo");

    /* A few more edits for search + save testing */
    HexCore_WriteByte(doc, 10, 'H');
    HexCore_WriteByte(doc, 11, 'I');

    /* Hex search: find pattern 48 49 ("HI") starting at 0 */
    uint8_t pattern[2] = {0x48, 0x49};
    HexSearchResult sr = HexCore_FindHex(doc, pattern, 2, 0, 1);
    CHECK(sr.found == 1 && sr.offset == 10, "FindHex locates edited HI bytes");

    /* Text search, case-insensitive */
    HexSearchResult sr2 = HexCore_FindText(doc, "hi", 0, 0, 1);
    CHECK(sr2.found == 1 && sr2.offset == 10, "FindText case-insensitive finds HI");

    HexSearchResult sr3 = HexCore_FindText(doc, "hi", 0, 1, 1);
    CHECK(sr3.found == 0, "FindText case-sensitive does not match lowercase pattern");

    /* ParseHexString utility */
    uint8_t parsed[8];
    size_t parsedLen = 0;
    int ok = HexCore_ParseHexString("DE AD-BE:EF", parsed, 8, &parsedLen);
    CHECK(ok == 1 && parsedLen == 4 && parsed[0] == 0xDE && parsed[3] == 0xEF, "ParseHexString handles mixed separators");
    ok = HexCore_ParseHexString("ZZ", parsed, 8, &parsedLen);
    CHECK(ok == 0, "ParseHexString rejects invalid hex");

    /* Save: must overwrite the SAME path, same name/extension. */
    CHECK(HexCore_Save(doc) == HEXCORE_OK, "Save succeeds");
    CHECK(HexCore_IsModified(doc) == 0, "Not modified immediately after save");

    wchar_t pathBuf[260];
    int pathLen = HexCore_GetPath(doc, pathBuf, 260);
    CHECK(pathLen > 0 && wcscmp(pathBuf, testPath) == 0, "Path unchanged after Save (same name/location)");

    /* Undo across a save boundary must still work correctly. */
    CHECK(HexCore_CanUndo(doc) == 1, "Undo history survives Save");
    HexCore_Undo(doc); /* undoes byte 11 'I' -> original */
    HexCore_ReadBytes(doc, 0, buf, 32, &got);
    CHECK(buf[11] == 11, "Undo-after-save restores correct original value");
    HexCore_Redo(doc);

    HexCore_Close(doc);

    /* Verify the file on disk actually matches what we saved. */
    FILE* check = _wfopen(testPath, L"rb");
    CHECK(check != NULL, "Reopen saved file with CRT");
    if (check) {
        uint8_t onDisk[32];
        fread(onDisk, 1, 32, check);
        fclose(check);
        CHECK(onDisk[5] == 0xAA && onDisk[10] == 'H' && onDisk[11] == 'I' && onDisk[4] == 4 && onDisk[31] == 31,
              "Bytes on disk match saved edits exactly, rest untouched");
    }

    /* --- Sublime Text-style structural editing: delete/insert (grow/shrink) --- */
    const wchar_t* pathA = L"test_alpha.bin";
    const wchar_t* pathB = L"test_beta.bin";
    {
        FILE* fa = _wfopen(pathA, L"wb");
        for (int i = 0; i < 10; ++i) fputc('A' + i, fa); /* "ABCDEFGHIJ" */
        fclose(fa);
        FILE* fb = _wfopen(pathB, L"wb");
        for (int i = 0; i < 20; ++i) fputc('0' + (i % 10), fb); /* "01234567890123456789" truncated to 20 */
        fclose(fb);
    }

    HHEXDOC docA = HexCore_Open(pathA, &st);
    CHECK(docA != NULL, "Open alpha (10 bytes)");
    CHECK(HexCore_GetSize(docA) == 10, "Alpha starts at 10 bytes");

    CHECK(HexCore_DeleteRange(docA, 2, 3) == HEXCORE_OK, "DeleteRange removes 3 bytes from the middle");
    CHECK(HexCore_GetSize(docA) == 7, "Size shrank by 3 after delete");
    HexCore_ReadBytes(docA, 0, buf, 7, &got);
    CHECK(got == 7 && memcmp(buf, "ABFGHIJ", 7) == 0, "Remaining bytes shifted left correctly after delete");
    CHECK(HexCore_IsModified(docA) == 1, "Delete marks the document modified");

    CHECK(HexCore_Undo(docA) == HEXCORE_OK, "Undo the delete");
    CHECK(HexCore_GetSize(docA) == 10, "Size restored after undoing delete");
    HexCore_ReadBytes(docA, 0, buf, 10, &got);
    CHECK(got == 10 && memcmp(buf, "ABCDEFGHIJ", 10) == 0, "Original bytes restored after undoing delete");
    CHECK(HexCore_Redo(docA) == HEXCORE_OK, "Redo the delete");
    CHECK(HexCore_GetSize(docA) == 7, "Size shrinks again after redo");

    uint8_t insertData[3] = {'X', 'Y', 'Z'};
    CHECK(HexCore_InsertBytes(docA, 2, insertData, 3) == HEXCORE_OK, "InsertBytes grows the file");
    CHECK(HexCore_GetSize(docA) == 10, "Size grew back to 10 after insert");
    HexCore_ReadBytes(docA, 0, buf, 10, &got);
    CHECK(got == 10 && memcmp(buf, "ABXYZFGHIJ", 10) == 0, "Inserted bytes shifted the tail right correctly");

    /* The exact user scenario: select-all + delete on one file, then
     * select-all + copy from another and paste (== delete selection,
     * then insert) into the first. The two files should end up
     * byte-for-byte identical, and saving must not change either
     * file's own name/path. */
    uint64_t sizeA = HexCore_GetSize(docA);
    CHECK(HexCore_DeleteRange(docA, 0, sizeA) == HEXCORE_OK, "Select-all + delete empties alpha");
    CHECK(HexCore_GetSize(docA) == 0, "Alpha is now 0 bytes");

    HHEXDOC docB = HexCore_Open(pathB, &st);
    CHECK(docB != NULL, "Open beta (20 bytes)");
    uint64_t sizeB = HexCore_GetSize(docB);
    uint8_t betaContent[64];
    size_t betaLen = 0;
    HexCore_ReadBytes(docB, 0, betaContent, sizeof(betaContent), &betaLen);
    CHECK(betaLen == sizeB, "Read all of beta's content (simulating Select All + Copy)");

    CHECK(HexCore_InsertBytes(docA, 0, betaContent, betaLen) == HEXCORE_OK,
          "Paste beta's content into emptied alpha");
    CHECK(HexCore_GetSize(docA) == sizeB, "Alpha now matches beta's size exactly");
    HexCore_ReadBytes(docA, 0, buf, (size_t)sizeB, &got);
    CHECK(got == sizeB && memcmp(buf, betaContent, betaLen) == 0,
          "Alpha's content now matches beta's exactly, byte for byte");

    CHECK(HexCore_Save(docA) == HEXCORE_OK, "Save All: alpha saves successfully after growing");
    wchar_t alphaPathBuf[260];
    HexCore_GetPath(docA, alphaPathBuf, 260);
    CHECK(wcscmp(alphaPathBuf, pathA) == 0, "Alpha keeps its own name/path even though its size changed");

    HexCore_Close(docA);
    HexCore_Close(docB);

    FILE* checkA = _wfopen(pathA, L"rb");
    CHECK(checkA != NULL, "Reopen alpha.bin with CRT after save");
    if (checkA) {
        fseek(checkA, 0, SEEK_END);
        long onDiskSize = ftell(checkA);
        fseek(checkA, 0, SEEK_SET);
        uint8_t onDisk[64];
        fread(onDisk, 1, (size_t)onDiskSize, checkA);
        fclose(checkA);
        CHECK((size_t)onDiskSize == betaLen && memcmp(onDisk, betaContent, betaLen) == 0,
              "alpha.bin on disk now holds exactly beta's original content");
    }

    _wremove(pathA);
    _wremove(pathB);

    /* Error path: opening a nonexistent file must fail cleanly. */
    HexCoreStatus st2;
    HHEXDOC bad = HexCore_Open(L"this_file_does_not_exist.bin", &st2);
    CHECK(bad == NULL && st2 == HEXCORE_ERROR_FILE_NOT_FOUND, "Opening missing file returns FILE_NOT_FOUND");

    /* Using a closed/invalid handle must not crash. */
    HexCoreStatus st3 = HexCore_WriteByte(doc, 0, 0);
    CHECK(st3 == HEXCORE_ERROR_INVALID_HANDLE, "Using closed handle returns INVALID_HANDLE, no crash");

    _wremove(testPath);

    wprintf(L"\n%hs (%d failure(s))\n", g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
