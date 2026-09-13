// Exported entry point that runs the ENTIRE hex editor GUI from inside
// HexEditorCore.dll - no separate .exe of our own is needed. Launch it
// with the standard Windows tool for calling an exported DLL function
// from the command line:
//
//   rundll32.exe HexEditorCore.dll,RunEditor
//
// rundll32.exe calls exported functions using this exact signature
// (WINAPI/__stdcall, these four parameters - see Microsoft's docs for
// "rundll32 entry point"). We ignore all four parameters: the window
// handle/command line aren't needed, and we deliberately do NOT use the
// HINSTANCE rundll32 passes in, because that is rundll32.exe's own
// module - our menu, dialog template, and manifest resources live in
// THIS dll, so we recover our own module handle independently below.
#include <windows.h>
#include <commctrl.h>

#include "MainWindow.h"
#include "FindDialog.h"
#include "resource.h"

namespace {

HMODULE GetOwnModuleHandle() {
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GetOwnModuleHandle), &hSelf);
    return hSelf;
}

} // namespace

extern "C" __declspec(dllexport) void CALLBACK RunEditor(HWND, HINSTANCE, LPSTR, int) {
    HMODULE hSelf = GetOwnModuleHandle();

    // rundll32.exe itself normally has no manifest requesting Common
    // Controls v6, so without this step our toolbar/buttons would
    // render in the ancient unthemed style. We push our own activation
    // context - built from the manifest resource embedded in this DLL
    // - around the whole UI session. This is the same technique legacy
    // Control Panel applet DLLs (.cpl) use to get modern-looking
    // controls despite being hosted by an unmanifested process.
    ACTCTXW actCtx{};
    actCtx.cbSize = sizeof(actCtx);
    actCtx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    actCtx.lpResourceName = MAKEINTRESOURCEW(IDR_MANIFEST);
    actCtx.hModule = hSelf;

    HANDLE hActCtx = CreateActCtxW(&actCtx);
    ULONG_PTR activationCookie = 0;
    bool activated = (hActCtx != INVALID_HANDLE_VALUE) && ActivateActCtx(hActCtx, &activationCookie);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    {
        MainWindow mainWindow;
        if (mainWindow.Create(hSelf, SW_SHOWNORMAL)) {
            HACCEL hAccel = LoadAcceleratorsW(hSelf, MAKEINTRESOURCEW(IDR_ACCEL));

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                if (FindDialog::GetHwnd() && IsDialogMessage(FindDialog::GetHwnd(), &msg)) {
                    continue;
                }
                if (hAccel && TranslateAcceleratorW(mainWindow.GetHwnd(), hAccel, &msg)) {
                    continue;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        } else {
            MessageBoxW(nullptr, L"Failed to create the main window.", L"Hex Editor", MB_OK | MB_ICONERROR);
        }
    }

    if (activated) DeactivateActCtx(0, activationCookie);
    if (hActCtx != INVALID_HANDLE_VALUE) ReleaseActCtx(hActCtx);
}
