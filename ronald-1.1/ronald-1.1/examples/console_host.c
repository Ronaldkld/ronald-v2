/* console_host.exe - minimal example host for HexEditorCore.dll.
 *
 * Unlike the GUI (which links against the DLL's import library at
 * build time), this example demonstrates the OTHER supported way to
 * consume the DLL: dynamic loading via LoadLibrary/GetProcAddress,
 * which is how a host written in a different language/toolchain
 * (Delphi, C#, a plugin loader, etc.) would typically use it.
 *
 * Usage:
 *   console_host.exe dump   <file>
 *   console_host.exe patch  <file> <offset-hex> <byte-hex>
 *   console_host.exe find   <file> <hex-or-text-pattern> [--text]
 */
#define HEXCORE_STATIC /* we call the API via GetProcAddress, not by linking */
#include <hexcore/HexEditorCore.h>

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

typedef HHEXDOC (HEXCORE_CALL *PFN_Open)(const wchar_t*, HexCoreStatus*);
typedef void (HEXCORE_CALL *PFN_Close)(HHEXDOC);
typedef uint64_t (HEXCORE_CALL *PFN_GetSize)(HHEXDOC);
typedef int (HEXCORE_CALL *PFN_IsModified)(HHEXDOC);
typedef HexCoreStatus (HEXCORE_CALL *PFN_ReadBytes)(HHEXDOC, uint64_t, uint8_t*, size_t, size_t*);
typedef int (HEXCORE_CALL *PFN_IsByteModified)(HHEXDOC, uint64_t);
typedef HexCoreStatus (HEXCORE_CALL *PFN_WriteByte)(HHEXDOC, uint64_t, uint8_t);
typedef HexCoreStatus (HEXCORE_CALL *PFN_Save)(HHEXDOC);
typedef HexSearchResult (HEXCORE_CALL *PFN_FindHex)(HHEXDOC, const uint8_t*, size_t, uint64_t, int);
typedef HexSearchResult (HEXCORE_CALL *PFN_FindText)(HHEXDOC, const char*, uint64_t, int, int);
typedef int (HEXCORE_CALL *PFN_ParseHexString)(const char*, uint8_t*, size_t, size_t*);
typedef const wchar_t* (HEXCORE_CALL *PFN_DescribeStatus)(HexCoreStatus);
typedef const wchar_t* (HEXCORE_CALL *PFN_GetLastErrorMessage)(HHEXDOC);

typedef struct {
    HMODULE dll;
    PFN_Open Open;
    PFN_Close Close;
    PFN_GetSize GetSize;
    PFN_IsModified IsModified;
    PFN_ReadBytes ReadBytes;
    PFN_IsByteModified IsByteModified;
    PFN_WriteByte WriteByte;
    PFN_Save Save;
    PFN_FindHex FindHex;
    PFN_FindText FindText;
    PFN_ParseHexString ParseHexString;
    PFN_DescribeStatus DescribeStatus;
    PFN_GetLastErrorMessage GetLastErrorMessage;
} HexCoreApi;

static int LoadHexCoreApi(HexCoreApi* api) {
    api->dll = LoadLibraryW(L"HexEditorCore.dll");
    if (!api->dll) {
        fwprintf(stderr, L"Could not load HexEditorCore.dll (error %lu). "
                          L"Make sure it is next to this executable.\n", GetLastError());
        return 0;
    }
#define BIND(name) do { \
        api->name = (PFN_##name)GetProcAddress(api->dll, "HexCore_" #name); \
        if (!api->name) { fprintf(stderr, "Missing export: HexCore_" #name "\n"); return 0; } \
    } while (0)

    BIND(Open);
    BIND(Close);
    BIND(GetSize);
    BIND(IsModified);
    BIND(ReadBytes);
    BIND(IsByteModified);
    BIND(WriteByte);
    BIND(Save);
    BIND(FindHex);
    BIND(FindText);
    BIND(ParseHexString);
    BIND(DescribeStatus);
    BIND(GetLastErrorMessage);
#undef BIND
    return 1;
}

static void PrintRow(HexCoreApi* api, HHEXDOC doc, uint64_t rowStart, uint64_t fileSize) {
    uint8_t buf[16];
    size_t got = 0;
    api->ReadBytes(doc, rowStart, buf, 16, &got);

    wprintf(L"%08llX  ", (unsigned long long)rowStart);
    for (size_t i = 0; i < 16; ++i) {
        if (i < got) {
            int modified = api->IsByteModified(doc, rowStart + i);
            wprintf(L"%hs%02X%hs ", modified ? "*" : " ", buf[i], "");
        } else {
            wprintf(L"    ");
        }
        if (i == 7) wprintf(L" ");
    }
    wprintf(L" ");
    for (size_t i = 0; i < got; ++i) {
        wchar_t c = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (wchar_t)buf[i] : L'.';
        wprintf(L"%c", c);
    }
    wprintf(L"\n");
    (void)fileSize;
}

static int CmdDump(HexCoreApi* api, const wchar_t* path) {
    HexCoreStatus st;
    HHEXDOC doc = api->Open(path, &st);
    if (!doc) {
        fwprintf(stderr, L"Open failed: %ls\n", api->DescribeStatus(st));
        return 1;
    }
    uint64_t size = api->GetSize(doc);
    wprintf(L"File: %ls (%llu bytes)\n\n", path, (unsigned long long)size);
    for (uint64_t off = 0; off < size; off += 16) {
        PrintRow(api, doc, off, size);
    }
    api->Close(doc);
    return 0;
}

static int CmdPatch(HexCoreApi* api, const wchar_t* path, const wchar_t* offsetHex, const wchar_t* byteHex) {
    HexCoreStatus st;
    HHEXDOC doc = api->Open(path, &st);
    if (!doc) {
        fwprintf(stderr, L"Open failed: %ls\n", api->DescribeStatus(st));
        return 1;
    }

    uint64_t offset = wcstoull(offsetHex, NULL, 16);
    uint8_t value = (uint8_t)wcstoul(byteHex, NULL, 16);

    wprintf(L"Before: ");
    PrintRow(api, doc, offset - (offset % 16), api->GetSize(doc));

    HexCoreStatus ws = api->WriteByte(doc, offset, value);
    if (ws != HEXCORE_OK) {
        fwprintf(stderr, L"Write failed: %ls\n", api->DescribeStatus(ws));
        api->Close(doc);
        return 1;
    }

    wprintf(L"After:  ");
    PrintRow(api, doc, offset - (offset % 16), api->GetSize(doc));

    wprintf(L"Saving (overwrites %ls in place)...\n", path);
    HexCoreStatus ss = api->Save(doc);
    if (ss != HEXCORE_OK) {
        fwprintf(stderr, L"Save failed: %ls\n", api->DescribeStatus(ss));
        fwprintf(stderr, L"Details: %ls\n", api->GetLastErrorMessage(doc));
        api->Close(doc);
        return 1;
    }
    wprintf(L"Saved successfully.\n");
    api->Close(doc);
    return 0;
}

static int CmdFind(HexCoreApi* api, const wchar_t* path, const wchar_t* patternW, int textMode) {
    HexCoreStatus st;
    HHEXDOC doc = api->Open(path, &st);
    if (!doc) {
        fwprintf(stderr, L"Open failed: %ls\n", api->DescribeStatus(st));
        return 1;
    }

    char narrow[512];
    WideCharToMultiByte(CP_ACP, 0, patternW, -1, narrow, sizeof(narrow), NULL, NULL);

    HexSearchResult result;
    if (textMode) {
        result = api->FindText(doc, narrow, 0, 0, 1);
    } else {
        uint8_t bytes[256];
        size_t len = 0;
        if (!api->ParseHexString(narrow, bytes, sizeof(bytes), &len)) {
            fwprintf(stderr, L"Invalid hex pattern.\n");
            api->Close(doc);
            return 1;
        }
        result = api->FindHex(doc, bytes, len, 0, 1);
    }

    if (result.found) {
        wprintf(L"Found at offset 0x%08llX\n", (unsigned long long)result.offset);
    } else {
        wprintf(L"Not found.\n");
    }
    api->Close(doc);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    HexCoreApi api;
    if (!LoadHexCoreApi(&api)) return 1;

    int rc = 1;
    if (argc >= 3 && wcscmp(argv[1], L"dump") == 0) {
        rc = CmdDump(&api, argv[2]);
    } else if (argc >= 5 && wcscmp(argv[1], L"patch") == 0) {
        rc = CmdPatch(&api, argv[2], argv[3], argv[4]);
    } else if (argc >= 4 && wcscmp(argv[1], L"find") == 0) {
        int textMode = (argc >= 5 && wcscmp(argv[4], L"--text") == 0);
        rc = CmdFind(&api, argv[2], argv[3], textMode);
    } else {
        wprintf(L"Usage:\n"
                L"  console_host.exe dump   <file>\n"
                L"  console_host.exe patch  <file> <offset-hex> <byte-hex>\n"
                L"  console_host.exe find   <file> <pattern> [--text]\n");
        rc = 1;
    }

    FreeLibrary(api.dll);
    return rc;
}
