# Hex Editor for Windows

A professional hexadecimal file editor for Windows 10/11 (x64), delivered
as a **single DLL** — `HexEditorCore.dll` contains the entire product:
both the editing engine and the graphical interface. There is no custom
`.exe` to ship or install.

You run it with **`rundll32.exe`**, the standard Windows tool (present on
every Windows install) for calling an exported function inside a DLL from
the command line:

```
rundll32.exe HexEditorCore.dll,RunEditor
```

That opens the full editor window — toolbar, tabs, hex/ASCII grid, menus,
Find dialog, everything.

**Multi-document, Sublime Text-style workflow:** open several files at
once (each becomes its own tab), drag or Shift+click/Shift+arrow to
select a byte range, Ctrl+A to select an entire file's hex, Del/Backspace
to delete a range (the file shrinks), and Ctrl+C/Ctrl+V to copy/paste hex
between tabs or to/from any text editor (the file grows/shrinks to fit
exactly what's pasted, just like pasting text) — see "Using the editor"
below.

A second, minimal file, **`console_host.exe`**, is included purely as a
*usage example*: it shows how a completely different program can load
`HexEditorCore.dll` dynamically and drive the same editing engine from a
script or another application (dump/patch/find a file from the CLI). It
is not part of the editor itself — delete it if you only want the DLL.

```
┌────────────────────────────────────────────┐
│              HexEditorCore.dll               │
│                                               │
│  RunEditor()  ──▶  MainWindow / HexGridControl / FindDialog (GUI)
│                          │
│                          ▼
│                  HexDocument engine:
│                  open / read / write / undo / redo / search / safe-save
│                                               │
│  HexCore_*()  ◀── public C API, used by the GUI above AND by any
│                    external host (see console_host.exe)             │
└────────────────────────────────────────────┘
          ▲
          │ rundll32.exe HexEditorCore.dll,RunEditor   (launches the GUI)
          │ LoadLibrary + GetProcAddress("HexCore_*")  (programmatic use)
┌─────────────────────┐
│  rundll32.exe /      │
│  console_host.exe /  │
│  your own program    │
└─────────────────────┘
```

## Why a DLL can't "just run" on its own

This is a genuine Windows constraint, not a design choice: a DLL is a
library, not a program, and Windows Explorer has no "run" verb for it —
double-clicking one does nothing. Every DLL needs *some* process to load
it into memory and call into it. `RunEditor` is written to the exact
calling convention `rundll32.exe` expects
(`void CALLBACK RunEditor(HWND, HINSTANCE, LPSTR, int)`, unmangled export
name), so `rundll32.exe` — which Windows already ships — is that process.
You never write or distribute an `.exe` of your own.

(If you've seen `regsvr32.exe some.dll` used to "run" a DLL: don't use
that here. `regsvr32` is for registering COM servers — it calls
`DllRegisterServer`, shows a modal "registration succeeded" dialog only
*after* that function returns, and abusing it to launch arbitrary code is
a well-known technique for evading application-whitelisting security
tools, so on a managed/corporate machine it can get a perfectly innocent
program flagged. `rundll32.exe` is the mechanism actually intended for
"run this exported function", and is what Control Panel applets and
similar DLL-hosted tools use.)

You can also make this a double-clickable shortcut: right-click on the
desktop → New → Shortcut → target
`rundll32.exe "C:\path\to\HexEditorCore.dll",RunEditor`.

## Why this design

- **C ABI boundary** (`core/include/hexcore/HexEditorCore.h`): besides
  the GUI, the DLL can be consumed from C, C++, C#/.NET (P/Invoke),
  Delphi, Rust, etc. — see `examples/console_host.c`.
- **Editing logic fully separated from the UI internally.** `HexDocument`
  (in `core/src`) never touches Win32 windowing; the GUI code
  (`core/src/gui`) never touches files directly — it only calls the same
  `HexCore_*` functions an external host would. They're compiled into one
  binary for convenience, but the layering is real and enforced by the
  same header both sides use.
- **Structural editing (grow/shrink), not just overwrite:** a file's
  content is a single in-memory, freely editable buffer, so bytes can be
  deleted (the file shrinks) or inserted (the file grows) - not just
  overwritten in place. This is what makes "select all, delete, paste a
  different file's content" behave exactly like it does in a text
  editor. The trade-off is that a file must fit in memory to be edited;
  `HexCore_Open` enforces a 512 MB cap and fails cleanly
  (`HEXCORE_ERROR_FILE_TOO_LARGE`) rather than risking exhausting memory
  on something bigger. Saving streams the buffer to disk in 1 MB chunks.
- **Crash-safe saving:** `Save`/`Save As` write to a temporary file next
  to the target, flush it to disk, and only then atomically swap it into
  place (`ReplaceFileW`/`MoveFileEx`). If anything fails partway (disk
  full, permission revoked, power loss), the **original file on disk is
  left completely untouched** and your in-memory edits are preserved so
  you can retry.
- **Save never renames or relocates your file.** It always writes back to
  the exact path it was opened from — same name, same extension, same
  folder — unless you explicitly choose "Save As".
- **Security:** the file's bytes are only ever read/written as raw
  binary data. Nothing about an opened file is ever executed, and no
  bytes are changed until you press Save.

## Repository layout

```
core/                               Everything ships in HexEditorCore.dll
  include/hexcore/HexEditorCore.h   Public C API (the contract)
  src/HexDocument.h/.cpp            Editing engine (Win32 file I/O, undo/redo, safe save, search)
  src/HexEditorCore.cpp             C ABI wrapper + handle validation
  src/Logger.h/.cpp                 Error-only file logger
  src/gui/GuiEntry.cpp              RunEditor - the rundll32 entry point
  src/gui/MainWindow.h/.cpp         Toolbar, tabs, menu, status bar, multi-document file operations
  src/gui/HexGridControl.h/.cpp     Owner-drawn hex/ASCII grid view (range selection, copy/paste)
  src/gui/TabBar.h/.cpp             Sublime Text-style document tab strip
  src/gui/FindDialog.h/.cpp         Hex/text search dialog
  resources/                        Menu, accelerators, Find dialog template, app manifest
  tests/manual_test.c               Runtime smoke test (30 assertions)
examples/
  console_host.c                    Example EXTERNAL host: dynamic-loads the DLL, dump/patch/find from the CLI
cmake/toolchain-mingw64.cmake       Cross-compilation toolchain (Linux/macOS -> Windows x64)
build.sh                            Convenience build script
```

## Building

### Option A — Windows, Visual Studio / MSVC (recommended for distribution)

Requirements: Windows 10/11, CMake 3.16+, Visual Studio 2019+ (Desktop
development with C++ workload).

```bat
cmake -B build -A x64
cmake --build build --config Release
```

Binaries land in `build\bin\Release\` (or `build\bin\` depending on your
CMake generator): `HexEditorCore.dll` and `console_host.exe`.

### Option B — Windows or Linux/macOS, MinGW-w64 (cross-compilation)

This is how the project was built and tested during development —
including on a Linux machine with no Windows available, using Wine to
run the resulting binaries for verification.

```bash
# Debian/Ubuntu:
sudo apt-get install g++-mingw-w64-x86-64 cmake ninja-build

./build.sh
```

Or manually:

```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Output: `build/bin/HexEditorCore.dll`, `build/bin/console_host.exe`,
`build/bin/hexcore_manual_test.exe`.

All binaries are genuine PE32+ x86-64 Windows images — copy
`HexEditorCore.dll` to a Windows machine and run:

```
rundll32.exe HexEditorCore.dll,RunEditor
```

### Running the automated engine test

`hexcore_manual_test.exe` exercises the DLL end-to-end (open, read,
write, undo/redo, hex/text search, safe save, error handling) and prints
PASS/FAIL for each check:

```
build\bin\hexcore_manual_test.exe
```

## Using the editor

1. Run `rundll32.exe HexEditorCore.dll,RunEditor` (or a shortcut to it —
   see above).
2. **File > Open** (or `Ctrl+O`) to open one or more files at once (the
   dialog allows multi-select) - each opens in its own **tab**, like
   Sublime Text. Opening more files never touches already-open tabs.
   Opening a file that's already open just switches to its tab instead
   of a second copy. Click a tab to switch to it, click its `×` (or
   middle-click it) to close it, `Ctrl+W` closes the current tab, and
   `Ctrl+Tab` / `Ctrl+Shift+Tab` (or `Ctrl+PageDown`/`Ctrl+PageUp`) cycle
   between tabs. Each tab keeps its own scroll position, selection, and
   undo/redo history. A file's bytes are only ever read when it's
   opened - nothing is modified until you choose to save.
3. Click any byte in the hex or ASCII column to select it.
   - **Range selection:** drag with the mouse, or hold Shift and
     click/use the arrow keys, to select a range of bytes (highlighted
     in blue) instead of just one.
   - `Ctrl+A` selects the entire file's hex (per tab).
   - `Ctrl+C` copies the selected bytes to the clipboard as hex text
     (`DE AD BE EF ...`) — paste it into another tab, another hex
     editor, or any text editor.
   - `Delete` or `Backspace` removes the selected range (or, with
     nothing selected, the byte at/before the cursor) - **the file
     shrinks**, exactly like deleting text.
   - `Ctrl+V` pastes hex text from the clipboard: if a range is
     selected, it's replaced (deleted, then the pasted bytes are
     inserted in its place); otherwise the pasted bytes are inserted at
     the cursor. **The file grows or shrinks to fit whatever was
     pasted** - copy a whole file's hex (Ctrl+A, Ctrl+C) and paste it
     over another file's selected content to make that file byte-for-
     byte identical to the first, just like pasting text in Sublime
     Text.
   - In the **hex column**, type two hex digits (`0-9`, `A-F`) to
     overwrite the byte's value in place; the cursor advances
     automatically. Typing while a range is selected overwrites starting
     from the beginning of that range (plain typing never grows/shrinks
     the file - use Delete/Backspace/Paste for that).
   - In the **ASCII column**, type any printable character to overwrite
     that byte directly.
   - Arrow keys / Page Up/Down / Home/End / mouse wheel navigate.
4. Edited bytes are highlighted in **red** until saved.
5. **Ctrl+Z / Ctrl+Y** undo/redo (per tab; works across an in-session
   Save, too; covers overwrites, deletes, and inserts alike).
6. **Ctrl+F** opens Find: search by hex bytes (`DE AD BE EF`, with or
   without spaces/dashes) or ASCII text, forward or backward, optionally
   case-sensitive.
7. **File > Save** (`Ctrl+S`) shows a confirmation naming the exact file
   that will be overwritten, then writes back to that same file — same
   name, extension and folder, even if its size changed. **File > Save
   As** lets you pick a different destination (and continues editing
   there). **File > Save All Tabs** saves every modified tab at once,
   after one confirmation listing every file that will be overwritten.
8. Closing a tab or the whole window with unsaved changes prompts you to
   save first, one file at a time.

The status bar always shows the active tab's full path, its size, the
cursor's current offset, and whether it has unsaved changes; the tab
itself shows a dot for unsaved changes.

## Using the DLL from your own host application

Include the header and either link the import library (static linking)
or load it dynamically at runtime (what `console_host.exe` does — no
import library needed at all):

```c
#define HEXCORE_STATIC   /* only needed for the dynamic-loading pattern */
#include <hexcore/HexEditorCore.h>

HexCoreStatus status;
HHEXDOC doc = HexCore_Open(L"C:\\data\\firmware.bin", &status);
if (!doc) { /* inspect `status` / HexCore_DescribeStatus(status) */ }

uint8_t buf[16];
size_t got;
HexCore_ReadBytes(doc, 0, buf, sizeof(buf), &got);   /* read first 16 bytes */

HexCore_WriteByte(doc, 0x10, 0xFF);                  /* edit one byte */

if (HexCore_IsModified(doc)) {
    HexCoreStatus saveStatus = HexCore_Save(doc);     /* overwrites the SAME file */
}

HexCore_Close(doc);
```

See `examples/console_host.c` for a complete, runnable example
(`dump`, `patch`, `find` subcommands) that loads the DLL dynamically via
`LoadLibrary`/`GetProcAddress` — the pattern to use from a host written
in a different language or toolchain.

### API summary

| Function | Purpose |
|---|---|
| `HexCore_Open` / `HexCore_Close` | Open/close a document |
| `HexCore_GetSize` / `HexCore_GetPath` / `HexCore_IsModified` | Document info |
| `HexCore_ReadBytes` / `HexCore_IsByteModified` | Read bytes (overlay-aware) |
| `HexCore_WriteByte` / `HexCore_WriteBytes` | Overwrite bytes in place (in memory only) |
| `HexCore_DeleteRange` | Remove bytes, shrinking the file |
| `HexCore_InsertBytes` | Insert bytes, growing the file (offset == size appends) |
| `HexCore_CanUndo` / `HexCore_CanRedo` / `HexCore_Undo` / `HexCore_Redo` | Undo/redo |
| `HexCore_Save` | Overwrite the file at its current path (safe, atomic) |
| `HexCore_SaveAs` | Save to a new path, optionally rebinding future saves to it |
| `HexCore_FindHex` / `HexCore_FindText` | Search, forward or backward |
| `HexCore_ParseHexString` | Validate/parse a user-typed hex string |
| `HexCore_GetLastErrorMessage` / `HexCore_DescribeStatus` | Error details |
| `RunEditor` | rundll32 entry point that launches the whole GUI (not part of the editing API) |

Full signatures and documentation are in
`core/include/hexcore/HexEditorCore.h`.

## Error handling

Every operation that can fail returns a `HexCoreStatus`. The engine maps
Win32 errors to specific statuses so a host (or the GUI) can show a
precise message instead of a generic failure:

- `HEXCORE_ERROR_FILE_NOT_FOUND` — bad path
- `HEXCORE_ERROR_ACCESS_DENIED` — insufficient permissions
- `HEXCORE_ERROR_FILE_LOCKED` — the file is open exclusively elsewhere
- `HEXCORE_ERROR_DISK_FULL` — not enough free space to complete a save
- `HEXCORE_ERROR_OUT_OF_RANGE` — offset outside the file
- `HEXCORE_ERROR_INVALID_HANDLE` — a closed/invalid document handle was used

`HexCore_GetLastErrorMessage` returns a human-readable detail string
(includes the underlying Windows error text) for the last operation on a
document. The DLL also writes a line to `hexcore_errors.log` (next to
the DLL, or `%LOCALAPPDATA%\HexEditor\` as a fallback) **only when an
error actually occurs** — a session with no errors produces no log file.

## Testing performed

This project was developed and built in a Linux container without a
Windows machine available, so verification used cross-compilation +
[Wine](https://www.winehq.org/) rather than a real Windows install:

- `objdump` confirms all binaries are genuine PE32+ x86-64 Windows
  images; `HexEditorCore.dll` exports exactly the `HexCore_*`
  functions declared in the public header, plus `RunEditor` with its
  exact unmangled name (verified so `rundll32.exe dllname,RunEditor`
  resolves it correctly).
- `hexcore_manual_test.exe` (51 assertions) runs successfully under
  Wine: open/read/write/undo/redo/save/search/error-handling all pass,
  including verifying the saved file's bytes on disk match expectations
  exactly and that the file's path/name never changes on Save. This
  includes structural editing: `DeleteRange` shrinking a file with
  correct undo/redo, `InsertBytes` growing one, and the exact
  select-all-delete-then-paste-another-file's-content scenario verified
  down to the bytes written to disk.
- `console_host.exe` was run under Wine against a real file end-to-end
  (`dump` → `patch` → verify on disk) — the byte-level patch is
  reflected in the file on disk after Save.
- **`rundll32.exe HexEditorCore.dll,RunEditor`** was run under Wine with
  a virtual display (Xvfb) and driven with `xdotool`: the full GUI
  rendered correctly (themed toolbar via a runtime activation context,
  since `rundll32.exe` itself has no Common Controls v6 manifest),
  opening a file, editing a byte (with live red highlighting), the Save
  confirmation dialog, and the resulting bytes on disk were all
  confirmed to work exactly as when run directly.
- The multi-tab workflow was driven end-to-end under the same Wine+Xvfb
  setup: opened two files at once via multi-select in the Open dialog
  (two tabs appeared); `Ctrl+A`/`Ctrl+C` on one tab followed by clicking
  the other tab and `Ctrl+V` correctly copied that file's entire hex
  content into the second file, clamped to its (shorter) length; `Ctrl+S`
  saved it and the bytes on disk matched exactly; `Ctrl+W` on a
  since-modified tab correctly raised the unsaved-changes prompt naming
  that exact file, and choosing "No" closed it, discarded the edit, and
  correctly switched back to the remaining tab with its own scroll/
  selection state intact; Shift+click was confirmed to extend a range
  selection across multiple rows.
- The exact "two files, empty one, fill it with the other's content"
  workflow was verified end-to-end: opened `Hola.exe` (39 bytes) and
  `Chau.exe` (48 bytes) as two tabs, `Ctrl+A`+`Delete` on Hola.exe
  correctly shrank it to **0 bytes**, `Ctrl+A`+`Ctrl+C` on Chau.exe then
  `Ctrl+V` on the emptied Hola.exe grew it back to exactly 48 bytes with
  Chau's content, and **File > Save All Tabs** showed one confirmation
  naming Hola.exe and wrote it to disk byte-for-byte identical to
  Chau.exe while keeping its own filename.

Because this environment has no real Windows, a final pass on genuine
Windows 10/11 (mouse/keyboard feel, DPI scaling on multiple monitors,
antivirus interaction, etc.) is still recommended before shipping.

## Extending

- Add a custom `.ico` and reference it from `core/resources/resource.rc`
  for a distinct taskbar icon (currently uses the system default icon).
- `HexGridControl`'s colors are plain constants at the top of
  `HexGridControl.cpp` — swap them for a theming system if you want
  runtime light/dark mode switching.
- The engine's undo/redo is byte-granular and unbounded for the current
  session; add a size cap in `HexDocument` if you need to bound memory
  use for extremely long editing sessions with millions of edits.
- If you ever do want a conventional standalone `.exe` again (e.g. to
  pin a taskbar icon more cleanly, or for environments that restrict
  `rundll32.exe`), it's a few lines: a `wWinMain` that calls the same
  `RunEditor` code path `core/src/gui/GuiEntry.cpp` already implements.
