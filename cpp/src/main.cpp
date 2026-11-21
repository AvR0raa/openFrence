#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shldisp.h>    // IShellDispatch4::ToggleDesktop (show desktop)
#include <commctrl.h>
#include "render.h"
#include "fence_window.h"
#include "context_menu.h"
#include "menu_icons.h"
#include "config.h"
#include "zoom_overlay.h"
#include "resource.h"
#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cwchar>
#include <cwctype>

// TEMP startup diagnostic — remove once the 0xc0000374 crash is located.
static void StartupLog(const wchar_t* msg);

static void LogProcessContext() {
    BOOL elevated = FALSE;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION te = {};
        DWORD cb = sizeof(te);
        GetTokenInformation(tok, TokenElevation, &te, cb, &cb);
        elevated = te.TokenIsElevated;
        CloseHandle(tok);
    }
    DWORD il = 0;
    {
        BYTE buf[256] = {};
        DWORD cb = sizeof(buf);
        HANDLE itok = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &itok)) {
            if (GetTokenInformation(itok, TokenIntegrityLevel, buf, cb, &cb)) {
                auto* label = (TOKEN_MANDATORY_LABEL*)buf;
                DWORD cnt = *GetSidSubAuthorityCount(label->Label.Sid);
                il = *GetSidSubAuthority(label->Label.Sid, cnt - 1);
            }
            CloseHandle(itok);
        }
    }
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t wsta[128] = {};
    DWORD wn = 0;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                              wsta, sizeof(wsta), &wn);
    wchar_t deskName[128] = {};
    HDESK desk = GetThreadDesktop(GetCurrentThreadId());
    if (desk) GetUserObjectInformationW(desk, UOI_NAME, deskName,
                                        sizeof(deskName), &wn);
    wchar_t buf[512];
    swprintf_s(buf, L"context elevated=%d il=0x%lx session=%lu winsta=%s desk=%s",
               elevated ? 1 : 0, il, session, wsta, deskName);
    StartupLog(buf);
}

static void StartupLog(const wchar_t* msg) {
    static const bool enabled = [] {
        wchar_t b[8] = {};
        return GetEnvironmentVariableW(L"OPENFENCES_DIAG", b, 8) > 0;
    }();
    if (!enabled) return;
    wchar_t appdata[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH)) return;
    std::wstring dir = std::wstring(appdata) + L"\\openFences";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring p = dir + L"\\startup.log";
    FILE* f = _wfopen(p.c_str(), L"a, ccs=UTF-8");
    if (f) { fwprintf(f, L"%lu %s\r\n", GetTickCount(), msg); fclose(f); }
}
#include <vector>
#include <memory>
#include <algorithm>

struct FenceRect { int x, y, w, h; };
// Not file-static: config.cpp serializes / rebuilds them (see Config::SaveNow
// / Config::LoadApp).
std::vector<std::unique_ptr<FenceWindow>> g_fences;
static HWND g_owner = nullptr;
static UINT g_taskbarCreated = 0;   // broadcast when explorer (re)starts

// ── DPI 感知 ──
// Without this, Windows virtualizes the process at 96 DPI on scaled displays
// and bitmap-upscales every window we present — the classic cause of blurry
// icons/text at 125%/150% scaling. Must run before any window is created.
//
// Prefer Per-Monitor V2: that is the awareness context explorer's Progman
// runs with, and CreateWindowExW refuses to create a WS_CHILD whose context
// differs from the parent's — fence windows are Progman children, so we must
// match it. (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4.)
static void EnableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        typedef BOOL (WINAPI* SetCtxT)(HANDLE);
        auto setCtx = (SetCtxT)(void*)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI* SetDpiAwarenessT)(int);
        auto setAwareness = (SetDpiAwarenessT)(void*)GetProcAddress(shcore, "SetProcessDpiAwareness");
        HRESULT hr = setAwareness ? setAwareness(2 /* PROCESS_SYSTEM_DPI_AWARE */) : E_FAIL;
        FreeLibrary(shcore);
        if (SUCCEEDED(hr)) return;
    }
    SetProcessDPIAware();
}

// ── 桌面图标约束 ──
// LVM_* message constants come from <commctrl.h>

HWND FindDesktopLV() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    HWND defview = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (!defview) return nullptr;
    return FindWindowExW(defview, nullptr, L"SysListView32", nullptr);
}

// Move a fence to a new parent without changing its screen position. Used to
// park fences on SHELLDLL_DefView while their usual parent (SysListView32) is
// hidden — see ToggleDesktopIcons. SetParent across the process boundary to
// explorer-owned parents is supported (the fences already cross it at birth),
// but only for windows that are children; a popup-fallback fence is left alone.
// HWND_TOP keeps the fence above its new siblings, matching the way fences
// sit above the desktop icons under the listview.
static void ReparentFence(HWND fence, HWND newParent) {
    if (!fence || !IsWindow(fence) || !newParent) return;
    if (!(GetWindowLongW(fence, GWL_STYLE) & WS_CHILD)) return;
    if (GetParent(fence) == newParent) return;
    RECT rc;
    if (!GetWindowRect(fence, &rc)) return;
    wchar_t buf[256];
    swprintf_s(buf, L"reparent %p -> %p", fence, newParent);
    StartupLog(buf);
    if (!SetParent(fence, newParent)) { StartupLog(L"reparent SetParent FAILED"); return; }
    StartupLog(L"reparent SetParent ok");
    POINT p{ rc.left, rc.top };
    ScreenToClient(newParent, &p);
    SetWindowPos(fence, HWND_TOP, p.x, p.y, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOACTIVATE);
    StartupLog(L"reparent SetWindowPos ok");
}

void ScanAndConstrainIcons() {
    HWND lv = FindDesktopLV();
    if (!lv) return;
    int count = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    if (count <= 0 || count > 2000) return;

    std::vector<FenceRect> rects;
    for (auto& f : g_fences) {
        FenceData d = f->GetData();
        rects.push_back({ d.x, d.y, d.w, d.h });
    }
    if (rects.empty()) return;

    for (int i = 0; i < count; i++) {
        POINT pt = {};
        SendMessageW(lv, LVM_GETITEMPOSITION, i, (LPARAM)&pt);
        int x = pt.x, y = pt.y;

        for (auto& f : rects) {
            if (x < f.x - 20 || x > f.x + f.w + 20) continue;
            if (y < f.y - 20 || y > f.y + f.h + 20) continue;

            int cx = x, cy = y;
            if      (cx < f.x)           cx = f.x;
            else if (cx > f.x + f.w - 40) cx = f.x + f.w - 40;
            if      (cy < f.y + 24)      cy = f.y + 24;
            else if (cy > f.y + f.h - 20) cy = f.y + f.h - 20;

            if (cx != x || cy != y) {
                SendMessageW(lv, LVM_SETITEMPOSITION, i, (LPARAM)MAKELPARAM(cx, cy));
            }
            break;
        }
    }
}

// ── 托盘 ──
#define WM_TRAYICON (WM_APP + 1)

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hwnd; nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"openFences");
    Shell_NotifyIconW(NIM_ADD, &nid);
    // Bust the tray icon cache so a rebuilt exe with a new icon is picked up
    // immediately (Windows keys the cache by executable path).
    SHChangeNotify(SHCNE_UPDATEIMAGE, SHCNF_DWORD,
                   (LPVOID)(DWORD_PTR)GetCurrentProcessId(), nullptr);
}

bool g_allHidden = false;        // extern'd by config.cpp (persisted)
bool g_deskHidden = false;       // extern'd by config.cpp (persisted)
// Set by WatchDesktopIconVisibility when EXPLORER hid the icon list (desktop
// right-click → View → Show desktop icons) and the fences were parked on
// SHELLDLL_DefView to survive it. Distinct from g_deskHidden, which means WE
// hid the list via ToggleDesktopIcons.
static bool g_extParked = false;

void ToggleDesktopIcons() {
    HWND lv = FindDesktopLV();
    HWND defview = lv ? GetParent(lv) : nullptr;
    if (!lv || !defview) return;
    // The fences are children of the icon list, so plainly hiding the list
    // would hide them too. Instead park them one level up on SHELLDLL_DefView
    // for as long as the list stays hidden: same desktop tree, same
    // visibility, same hit-testing — only the icon list itself is gone.
    if (!g_deskHidden) {
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), defview);
        ShowWindow(lv, SW_HIDE);
        g_deskHidden = true;
    } else {
        ShowWindow(lv, SW_SHOW);
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), lv);
        g_deskHidden = false;
    }
    Config::MarkDirty();   // survive an app restart (explorer resets the list)
}

// Re-apply the persisted "desktop icons hidden" state onto the live desktop
// tree. Explorer always recreates its icon list visible — on app start and
// after an explorer restart — so the saved hide has to be replayed here,
// parking the fences on SHELLDLL_DefView exactly like ToggleDesktopIcons.
// At a Run-key launch the desktop tree may not exist yet; that must NOT
// clear the state (the old drop-on-fail left the icons up for the whole
// session) — this returns without doing anything and the timer-2 tick
// retries every 300 ms until the list appears. Applied-state ticks are
// no-ops (parents match, the list is already hidden).
static void ApplyDesktopIconsHidden() {
    if (!g_deskHidden) return;
    static int applyLogs = 0;
    const bool logThis = applyLogs++ < 5;
    if (logThis) StartupLog(L"ApplyDesktopIconsHidden enter");
    HWND lv = FindDesktopLV();
    HWND defview = lv ? GetParent(lv) : nullptr;
    if (!lv || !defview) { if (logThis) StartupLog(L"Apply: no lv/defview"); return; }
    for (auto& f : g_fences) ReparentFence(f->Hwnd(), defview);
    if (logThis) StartupLog(L"Apply: fences reparented, hiding list");
    ShowWindow(lv, SW_HIDE);
    if (logThis) StartupLog(L"Apply: list hidden");
}

// Explorer's own "Show desktop icons" switch (desktop context menu → View)
// hides SysListView32 from outside our control. The fences are its WS_CHILD
// windows, so they vanish with it; Win32 gives a child no way to opt out of
// an ancestor's visibility. Poll for the external hide and park the fences
// on SHELLDLL_DefView for as long as the list is gone — the same parking
// ToggleDesktopIcons performs when WE hide the icons — then move them back
// when the list returns. (If explorer hides DefView as well there is no
// visible desktop surface left to sit on; the fences simply reappear with
// the tree, so that case needs no special handling.) Called from the 300ms
// owner timer-2, which already watches the icon list; IsWindowVisible is a
// cheap check so the poll costs nothing.
static void WatchDesktopIconVisibility() {
    if (g_deskHidden) return;              // our own toggle owns the parking
    HWND lv = FindDesktopLV();
    if (!lv) return;
    bool shown = IsWindowVisible(lv) != FALSE;
    if (!shown && !g_extParked) {
        StartupLog(L"Watch: external hide, parking");
        HWND defview = GetParent(lv);
        if (!defview) return;
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), defview);
        g_extParked = true;
        StartupLog(L"Watch: parked");
    } else if (shown && g_extParked) {
        StartupLog(L"Watch: list back, unparking");
        for (auto& f : g_fences) ReparentFence(f->Hwnd(), lv);
        g_extParked = false;
        StartupLog(L"Watch: unparked");
    }
}

static void CreateDesktopShortcut() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop)))
        return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring lnk = std::wstring(desktop) + L"\\openFences.lnk";

    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&sl)))
        return;
    sl->SetPath(exe);
    sl->SetDescription(L"openFences");
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
        pf->Save(lnk.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
}

// ── Show desktop hotkey ──

static bool ParseHotkey(const std::wstring& combo, UINT& mods, UINT& vk) {
    mods = 0;
    vk = 0;
    std::wstring token;
    auto flush = [&]() -> bool {
        if (token.empty()) return false;
        bool ok = true;
        if (token == L"CTRL" || token == L"CONTROL") mods |= MOD_CONTROL;
        else if (token == L"ALT") mods |= MOD_ALT;
        else if (token == L"SHIFT") mods |= MOD_SHIFT;
        else if (token == L"WIN" || token == L"WINDOWS") mods |= MOD_WIN;
        else if (vk != 0) ok = false;
        else if (token.size() == 1 && iswalnum(token[0])) vk = (UINT)towupper(token[0]);
        else if (token.size() >= 2 && token[0] == L'F') {
            int n = _wtoi(token.c_str() + 1);
            if (n >= 1 && n <= 24) vk = VK_F1 + n - 1;
            else ok = false;
        } else if (token == L"SPACE") vk = VK_SPACE;
        else if (token == L"TAB") vk = VK_TAB;
        else if (token == L"ENTER") vk = VK_RETURN;
        else if (token == L"ESC" || token == L"ESCAPE") vk = VK_ESCAPE;
        else if (token == L"HOME") vk = VK_HOME;
        else if (token == L"END") vk = VK_END;
        else if (token == L"INSERT") vk = VK_INSERT;
        else if (token == L"DELETE") vk = VK_DELETE;
        else if (token == L"LEFT") vk = VK_LEFT;
        else if (token == L"RIGHT") vk = VK_RIGHT;
        else if (token == L"UP") vk = VK_UP;
        else if (token == L"DOWN") vk = VK_DOWN;
        else if (token == L"PAGEUP") vk = VK_PRIOR;
        else if (token == L"PAGEDOWN") vk = VK_NEXT;
        else ok = false;
        token.clear();
        return ok;
    };

    for (size_t i = 0; i <= combo.size(); i++) {
        wchar_t c = (i < combo.size()) ? combo[i] : L'+';
        if (c == L'+') {
            if (!flush()) return false;
        } else if (!iswspace(c)) {
            token.push_back((wchar_t)towupper(c));
        }
    }
    return vk != 0;
}

static std::wstring FormatHotkey(UINT mods, UINT vk) {
    std::wstring out;
    auto add = [&](const wchar_t* part) {
        if (!out.empty()) out += L"+";
        out += part;
    };
    if (mods & MOD_CONTROL) add(L"Ctrl");
    if (mods & MOD_ALT)     add(L"Alt");
    if (mods & MOD_SHIFT)   add(L"Shift");
    if (mods & MOD_WIN)     add(L"Win");

    wchar_t key[16] = {};
    if (vk >= 'A' && vk <= 'Z')       key[0] = (wchar_t)vk;
    else if (vk >= '0' && vk <= '9')  key[0] = (wchar_t)vk;
    else if (vk >= VK_F1 && vk <= VK_F24)
        swprintf_s(key, L"F%u", vk - VK_F1 + 1);
    else {
        switch (vk) {
        case VK_SPACE:  wcscpy_s(key, L"Space"); break;
        case VK_TAB:    wcscpy_s(key, L"Tab"); break;
        case VK_RETURN: wcscpy_s(key, L"Enter"); break;
        case VK_ESCAPE: wcscpy_s(key, L"Esc"); break;
        case VK_HOME:   wcscpy_s(key, L"Home"); break;
        case VK_END:    wcscpy_s(key, L"End"); break;
        case VK_INSERT: wcscpy_s(key, L"Insert"); break;
        case VK_DELETE: wcscpy_s(key, L"Delete"); break;
        case VK_LEFT:   wcscpy_s(key, L"Left"); break;
        case VK_RIGHT:  wcscpy_s(key, L"Right"); break;
        case VK_UP:     wcscpy_s(key, L"Up"); break;
        case VK_DOWN:   wcscpy_s(key, L"Down"); break;
        case VK_PRIOR:  wcscpy_s(key, L"PageUp"); break;
        case VK_NEXT:   wcscpy_s(key, L"PageDown"); break;
        default:        swprintf_s(key, L"0x%02X", vk); break;
        }
    }
    if (key[0]) add(key);
    return out;
}

static std::wstring CurrentHotkeyText() {
    UINT mods = 0, vk = 0;
    if (ParseHotkey(Config::ShowDesktopHotkey(), mods, vk))
        return FormatHotkey(mods, vk);
    return Config::ShowDesktopHotkey();
}

static void ShowDesktop() {
    // IShellDispatch4::ToggleDesktop is the taskbar Show Desktop button's own
    // code path. Unlike synthesizing Win+D, it does not care that the user is
    // still holding Ctrl/Alt from the shortcut key chord.
    CComPtr<IShellDispatch4> shell;
    HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_ALL,
                                  IID_IShellDispatch4, (void**)&shell);
    if (SUCCEEDED(hr) && shell) {
        HRESULT th = shell->ToggleDesktop();
        // S_FALSE means the shell accepted the call but did not change the
        // desktop (it can happen while the shortcut chord is still held).
        // Fall through to the modifier-release + Win+D path in that case.
        if (th == S_OK) return;
    }
    // Last-resort fallback: release the chord's modifiers, then Win+D.
    keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, 0, 0);
    keybd_event('D', 0, 0, 0);
    keybd_event('D', 0, KEYEVENTF_KEYUP, 0);
    keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, 0);
}

// Low-level keyboard hook for the configurable shortcut. RegisterHotKey is
// unreliable here (the combination can be owned by the shell or another app,
// and some systems never deliver the WM_HOTKEY for it). The LL hook sees the
// physical/injected key before any other handler and is independent of the
// registration table.
static HHOOK g_showHotkeyHook = nullptr;
static HANDLE g_showHotkeyThread = nullptr;
static std::atomic<UINT> g_showHotMods{0};
static std::atomic<UINT> g_showHotVk{0};
static std::atomic<bool> g_showHotDown{false};
static std::atomic<bool> g_showHotSuspend{false};
// Modifier state is tracked in the hook thread. GetAsyncKeyState can be
// unreliable there (the hook thread has no foreground input queue), which
// made a perfectly received chord fail the modifier match.
static bool g_hookCtrl = false, g_hookAlt = false;
static bool g_hookShift = false, g_hookWin = false;
constexpr UINT WM_APP_SHOW_DESKTOP = WM_APP + 0x60;

static void RefreshShowHotkeyBinding() {
    UINT mods = 0, vk = 0;
    if (ParseHotkey(Config::ShowDesktopHotkey(), mods, vk)) {
        g_showHotMods.store(mods);
        g_showHotVk.store(vk);
    } else {
        g_showHotMods.store(0);
        g_showHotVk.store(0);
    }
    g_showHotDown.store(false);
}

static LRESULT CALLBACK ShowHotkeyKeyboardProc(int code, WPARAM wp, LPARAM lp) {
    UINT vk = g_showHotVk.load();
    UINT mods = g_showHotMods.load();
    if (code == HC_ACTION) {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lp;
        const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
        const bool up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        if (down || up) {
            switch (kb->vkCode) {
            case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: g_hookCtrl = down; break;
            case VK_MENU:    case VK_LMENU:    case VK_RMENU:    g_hookAlt = down; break;
            case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:   g_hookShift = down; break;
            case VK_LWIN:    case VK_RWIN:                       g_hookWin = down; break;
            default: break;
            }
        }
        if (vk && !g_showHotSuspend.load() && down && kb->vkCode == vk) {
            if (g_hookCtrl == ((mods & MOD_CONTROL) != 0) &&
                g_hookAlt == ((mods & MOD_ALT) != 0) &&
                g_hookShift == ((mods & MOD_SHIFT) != 0) &&
                g_hookWin == ((mods & MOD_WIN) != 0)) {
                if (!g_showHotDown.exchange(true))
                    PostMessageW(g_owner, WM_APP_SHOW_DESKTOP, 0, 0);
                return 1;   // swallow the configured chord
            }
        } else if (kb->vkCode == g_showHotVk.load() && up) {
            g_showHotDown.store(false);
        }
    }
    return CallNextHookEx(g_showHotkeyHook, code, wp, lp);
}

static DWORD WINAPI ShowHotkeyThreadProc(LPVOID) {
    // A dedicated thread owns the low-level hook. Keeping it off the UI/COM
    // thread matters: Windows silently drops a WH_KEYBOARD_LL hook whose
    // callback is delayed past LowLevelHooksTimeout, which made the shortcut
    // work once and then stop while ShowDesktop was still on the UI thread.
    SetLastError(0);
    g_showHotkeyHook = SetWindowsHookExW(WH_KEYBOARD_LL,
        ShowHotkeyKeyboardProc, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_showHotkeyHook) {
        UnhookWindowsHookEx(g_showHotkeyHook);
        g_showHotkeyHook = nullptr;
    }
    return 0;
}

static void InstallShowHotkeyHook() {
    RefreshShowHotkeyBinding();
    if (!g_showHotkeyThread) {
        SetLastError(0);
        g_showHotkeyThread = CreateThread(nullptr, 0, ShowHotkeyThreadProc,
                                          nullptr, 0, nullptr);
    }
}

static constexpr int kHotkeyCtrlId = 200;

static LRESULT CALLBACK HotkeyDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND label = CreateWindowExW(0, L"STATIC",
            FenceWindow::Loc(L"Press the new Show Desktop shortcut:", L"按下新的显示桌面快捷键："),
            WS_CHILD | WS_VISIBLE, 18, 18, 324, 22, hwnd, nullptr,
            GetModuleHandle(nullptr), nullptr);
        HWND hk = CreateWindowExW(0, L"msctls_hotkey32", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP, 18, 48, 324, 28, hwnd,
            (HMENU)(INT_PTR)kHotkeyCtrlId, GetModuleHandle(nullptr), nullptr);
        HWND ok = CreateWindowExW(0, L"BUTTON",
            FenceWindow::Loc(L"OK", L"确定"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            176, 130, 76, 28, hwnd, (HMENU)IDOK, GetModuleHandle(nullptr), nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON",
            FenceWindow::Loc(L"Cancel", L"取消"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            264, 130, 78, 28, hwnd, (HMENU)IDCANCEL, GetModuleHandle(nullptr), nullptr);
        for (HWND h : { label, hk, ok, cancel }) {
            if (h) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
        }
        UINT mods = 0, vk = 0;
        ParseHotkey(Config::ShowDesktopHotkey(), mods, vk);
        UINT flags = 0;
        if (mods & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
        if (mods & MOD_ALT)     flags |= HOTKEYF_ALT;
        if (mods & MOD_SHIFT)   flags |= HOTKEYF_SHIFT;
        if (hk) {
            SendMessageW(hk, HKM_SETHOTKEY, MAKEWORD(vk, flags), 0);
            SetFocus(hk);
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            WORD hk = (WORD)SendMessageW(GetDlgItem(hwnd, kHotkeyCtrlId),
                                         HKM_GETHOTKEY, 0, 0);
            UINT vk = LOBYTE(hk);
            UINT flags = HIBYTE(hk);
            UINT mods = 0;
            if (flags & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
            if (flags & HOTKEYF_ALT)     mods |= MOD_ALT;
            if (flags & HOTKEYF_SHIFT)   mods |= MOD_SHIFT;
            if (!vk || !mods) {
                MessageBoxW(hwnd,
                    FenceWindow::Loc(L"Press a modifier plus a key, e.g. Ctrl+Alt+Home.",
                                     L"请按至少一个修饰键加一个主键，例如 Ctrl+Alt+Home。"),
                    FenceWindow::Loc(L"Show Desktop Hotkey", L"显示桌面快捷键"),
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            Config::SetShowDesktopHotkey(FormatHotkey(mods, vk));
            RefreshShowHotkeyBinding();
            Config::SaveNow();
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowHotkeyDialog() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = HotkeyDlgProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"OpenFencesHotkeyDlg";
        registered = RegisterClassExW(&wc) != 0;
    }
    if (!registered) return;

    const int w = 360, h = 200;
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    int x = (wa.right - w) / 2, y = (wa.bottom - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW,
        L"OpenFencesHotkeyDlg",
        FenceWindow::Loc(L"Show Desktop Hotkey", L"显示桌面快捷键"),
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        x, y, w, h, g_owner, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!dlg) return;

    g_showHotSuspend.store(true);
    EnableWindow(g_owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (IsWindow(dlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    g_showHotSuspend.store(false);
    EnableWindow(g_owner, TRUE);
    SetForegroundWindow(g_owner);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 100, FenceWindow::Loc(L"New Fence", L"新建围栏"));
    AppendMenuW(menu, MF_STRING, 101, g_allHidden
        ? FenceWindow::Loc(L"Show All Fences", L"显示全部围栏")
        : FenceWindow::Loc(L"Hide All Fences", L"隐藏全部围栏"));
    AppendMenuW(menu, MF_STRING, 103, g_deskHidden
        ? FenceWindow::Loc(L"Show Desktop Icons", L"显示桌面图标")
        : FenceWindow::Loc(L"Hide Desktop Icons", L"隐藏桌面图标"));

    // Language is a global setting, so it lives here (tray) rather than in the
    // per-fence appearance panel.
    HMENU langMenu = CreatePopupMenu();
    AppendMenuW(langMenu, MF_STRING | (FenceWindow::GetLanguage() == 0 ? MF_CHECKED : 0),
        110, L"English");
    AppendMenuW(langMenu, MF_STRING | (FenceWindow::GetLanguage() == 1 ? MF_CHECKED : 0),
        111, L"中文");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)langMenu, FenceWindow::Loc(L"Language", L"语言"));
    AppendMenuW(menu, MF_STRING | (Config::AutoStartEnabled() ? MF_CHECKED : 0),
        112, FenceWindow::Loc(L"Start with Windows", L"开机自启动"));
    AppendMenuW(menu, MF_STRING, 113,
        FenceWindow::Loc(L"Config File Location", L"配置文件位置"));
    AppendMenuW(menu, MF_STRING, 114,
        FenceWindow::Loc(L"Create Desktop Shortcut", L"创建桌面快捷方式"));

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        std::wstring show = FenceWindow::Loc(L"Show Desktop", L"显示桌面");
        std::wstring hk = CurrentHotkeyText();
        if (!hk.empty()) show += L"  " + hk;
        AppendMenuW(menu, MF_STRING, 104, show.c_str());
    }
    AppendMenuW(menu, MF_STRING, 105,
        FenceWindow::Loc(L"Set Show Desktop Hotkey...", L"设置显示桌面快捷键..."));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 102, FenceWindow::Loc(L"Exit", L"退出"));

    // Fluent glyph icons, by menu position (0 New Fence, 1 Hide/Show All
    // Fences, 2 Hide/Show Desktop Icons, 3 separator, 4 Language ▸,
    // 5 Start with Windows, 6 Config File Location, 7 separator, 8 Exit).
    // The set owns the HBITMAPs until the menu interaction below has fully
    // returned (MIIM_BITMAP is by reference).
    GlyphBitmapSet glyphs;
    {
        int gsz = MenuGlyphSizePx();
        auto icon = [&](UINT pos, MenuGlyph g) {
            HBITMAP b = glyphs.add(g, gsz);
            if (!b) return;
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_BITMAP;
            mii.hbmpItem = b;
            SetMenuItemInfoW(menu, pos, TRUE, &mii);
        };
        icon(0, MenuGlyph::Plus);
        icon(1, g_allHidden ? MenuGlyph::Eye : MenuGlyph::EyeOff);
        icon(2, MenuGlyph::Grid);
        icon(4, MenuGlyph::Globe);
        icon(5, MenuGlyph::Rocket);
        icon(6, MenuGlyph::Folder);
        icon(9, MenuGlyph::Eye);       // Show Desktop
        icon(10, MenuGlyph::Sliders);  // Set Show Desktop Hotkey
        icon(12, MenuGlyph::Power);    // Exit
    }

    SetForegroundWindow(hwnd);
    // Prefer the Win11 Fluent rendering; fall back to the classic menu only
    // if it could not be built (-1). The Fluent loop swallows the click that
    // dismisses the menu — exactly right over the desktop, where a leaked
    // press would start a selection band or icon drag.
    int cmd = FluentMenu::Run(hwnd, menu, pt);
    if (cmd < 0)
        cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);   // also destroys the attached langMenu
    if (cmd == 100) {
        HDC hdc = GetDC(nullptr);
        float s = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
        ReleaseDC(nullptr, hdc);
        FenceData fd{ L"f" + std::to_wstring(g_fences.size()+1),
                      FenceWindow::Loc(L"New Fence", L"新建围栏"),
                      (int)(100*s), (int)(100*s), (int)(300*s), (int)(220*s) };
        g_fences.push_back(std::make_unique<FenceWindow>(fd));
        if (g_deskHidden || g_extParked) {
            // It was created under the hidden icon list; park it with the rest.
            HWND lv = FindDesktopLV();
            if (lv) ReparentFence(g_fences.back()->Hwnd(), GetParent(lv));
        }
        Config::MarkDirty();
    } else if (cmd == 101) {
        g_allHidden = !g_allHidden;
        for (auto& f : g_fences) {
            if (g_allHidden) f->Hide(); else f->Show();
        }
        Config::MarkDirty();
    } else if (cmd == 103) {
        ToggleDesktopIcons();
    } else if (cmd == 110) {
        FenceWindow::SetLanguage(0);   // English
        Config::MarkDirty();
    } else if (cmd == 111) {
        FenceWindow::SetLanguage(1);   // 中文
        Config::MarkDirty();
    } else if (cmd == 112) {
        Config::SetAutoStart(!Config::AutoStartEnabled());
    } else if (cmd == 113) {
        Config::RevealFile();
    } else if (cmd == 114) {
        CreateDesktopShortcut();
    } else if (cmd == 104) {
        ShowDesktop();
    } else if (cmd == 105) {
        ShowHotkeyDialog();
    } else if (cmd == 102) {
        Config::SaveNow();   // do not ride on the debounce for a real exit
        NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW), hwnd, 1 };
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
    }
}

// An explorer restart tears down the desktop tree, taking our child fences
// with it. Salvage their state from the (now dead) objects and recreate
// them; the constructor re-discovers the fresh desktop host on its own.
static void RebuildFences() {
    // The fences are recreated under the fresh icon list; drop the parked
    // state (the watcher re-parks on the next tick if explorer still hides
    // the list). The zoom shade died with the old tree — drop its state too.
    g_extParked = false;
    ZoomOverlay::Close();
    FenceWindow::SetTeardown(true);
    struct Saved { FenceData data; std::vector<IconEntry> icons; bool collapsed; };
    std::vector<Saved> saved;
    saved.reserve(g_fences.size());
    for (auto& f : g_fences)
        saved.push_back({ f->GetData(), f->Icons(), f->IsCollapsed() });
    g_fences.clear();   // dtors see the dead HWNDs and skip DestroyWindow
    FenceWindow::SetTeardown(false);
    for (auto& s : saved) {
        auto f = std::make_unique<FenceWindow>(s.data);
        if (!s.data.sourceFolder.empty()) {
            f->SetSortPreset(s.data.sortCol, s.data.sortAsc);
            f->MapToFolder(s.data.sourceFolder);
        } else
            f->SetIcons(s.icons);
        if (s.collapsed) f->ToggleCollapse();
        if (g_allHidden) f->Hide();
        g_fences.push_back(std::move(f));
    }
    // The fresh desktop came back with its icons visible; if the user had
    // them hidden, re-hide them (and park the fences on SHELLDLL_DefView
    // again, exactly like ToggleDesktopIcons does).
    ApplyDesktopIconsHidden();
}

LRESULT CALLBACK OwnerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated) { RebuildFences(); return 0; }
    switch (msg) {
    case WM_CREATE: AddTrayIcon(hwnd); return 0;
    case WM_TRAYICON: if (LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu(hwnd); return 0;
    case WM_HOTKEY:
        if (wp == 1) ToggleDesktopIcons();
        return 0;

    case WM_APP_SHOW_DESKTOP:
        ShowDesktop();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        // Resolution / personalization broadcasts only reach top-level
        // windows; the fences are children of explorer's desktop tree, so
        // fan out here. Each fence debounces the actual re-capture on its
        // own timer-2 — without this the frosted glass keeps showing stale
        // wallpaper pixels after a resolution switch.
        for (auto& f : g_fences) f->ScheduleBackdropRefresh();
        return 0;
    case WM_TIMER:
        if (wp == 2) {   // desktop icon-size watcher
            static int ticks = 0;
            if (ticks < 5) StartupLog(L"owner tick begin");
            ticks++;
            // Keep replaying a persisted hide until the icon list exists
            // (a Run-key boot launch can beat explorer's desktop tree).
            ApplyDesktopIconsHidden();
            if (ticks < 5) StartupLog(L"owner tick after apply");
            // Keep fences alive across explorer's own icon hide/show.
            WatchDesktopIconVisibility();
            if (ticks < 5) StartupLog(L"owner tick after watch");
            // One live measurement per tick, shared by all fences (the query
            // opens explorer's process — not worth repeating per fence).
            int px = QueryDesktopIconSizePx();
            if (ticks < 5) StartupLog(L"owner tick after query");
            for (auto& f : g_fences) {
                // A zoom animation that stalled (timer lost mid-flight)
                // would freeze the desktop half-open — force-finish it.
                f->ZoomStallCheck();
                // Display topology changes move the desktop icon list; keep
                // every fence at its saved screen position.
                f->RestoreAnchor();
                if (px > 0) f->SyncDesktopIconSize(px);
            }
            return 0;
        }
        if (wp == 3) {   // config save debounce (Config::MarkDirty)
            Config::SaveNow();
            return 0;
        }
        ScanAndConstrainIcons();
        return 0;
    case WM_FENCE_DELETE: {
        HWND target = (HWND)wp;
        auto it = std::find_if(g_fences.begin(), g_fences.end(),
            [target](auto& f) { return f->Hwnd() == target; });
        if (it != g_fences.end()) {
            g_fences.erase(it);
            Config::MarkDirty();
        }
        return 0;
    }
    case WM_DESTROY:
        Config::SaveNow();   // safety net; the tray Exit path saves too
        NOTIFYICONDATAW nid = { sizeof(NOTIFYICONDATAW), hwnd, 1 };
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Entry ──
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    StartupLog(L"wWinMain enter");
    LogProcessContext();
    // 第二个实例会把围栏重复绘制在同一桌面上；命名互斥体保证只有一个实例
    // （进程崩溃时系统自动释放，不会留下死锁）。
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"openFences_SingleInstance");
    if (!singleInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        StartupLog(L"mutex already exists — exit");
        return 0;
    }
    StartupLog(L"mutex created");

    EnableDpiAwareness();
    StartupLog(L"dpi awareness");
    ComInit com;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    FenceWindow::RegisterClass(hInst);
    StartupLog(L"classes registered");

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = OwnerWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"OpenFencesOwner";
    RegisterClassExW(&wc);

    g_owner = CreateWindowExW(0, L"OpenFencesOwner", nullptr, WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);

    FenceWindow::SetOwner(g_owner);
    StartupLog(g_owner ? L"owner window created" : L"owner window FAILED");
    RegisterHotKey(g_owner, 1, MOD_CONTROL | MOD_SHIFT, 'H');
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // Scale default layout to physical pixels (design values are at 96 DPI)
    HDC hdc = GetDC(nullptr);
    float s = GetDeviceCaps(hdc, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, hdc);

    // Restore the persisted layout (language, fences, icons, appearance).
    // Only a missing/unreadable config falls back to the three starter
    // fences — an empty list is a valid saved state (everything deleted).
    StartupLog(L"before Config::LoadApp");
    if (!Config::LoadApp()) {
        // Default titles follow the startup language (they are fence names,
        // set once at birth — a later language switch does not rename
        // existing fences).
        FenceData fences[] = {
            { L"f1", FenceWindow::Loc(L"Applications", L"应用"),
              (int)(50*s),  (int)(50*s),  (int)(280*s), (int)(280*s) },
            { L"f2", FenceWindow::Loc(L"Documents", L"文档"),
              (int)(370*s), (int)(50*s),  (int)(280*s), (int)(380*s) },
            { L"f3", FenceWindow::Loc(L"Projects", L"项目"),
              (int)(50*s),  (int)(360*s), (int)(280*s), (int)(220*s) },
        };
        for (auto& fd : fences)
            g_fences.push_back(std::make_unique<FenceWindow>(fd));
    }
    InstallShowHotkeyHook();

    // Replay OUR persisted hide (config deskHidden) right away — explorer
    // reset the list to visible on its way up. When a Run-key launch beat
    // the desktop tree, the timer-2 tick keeps retrying until it exists.
    StartupLog(L"after Config::LoadApp");
    // Do NOT reparent the freshly-created fences synchronously. SetParent on
    // a WS_EX_LAYERED child immediately after CreateWindowEx (before the
    // message loop has drained the creation-time messages) corrupts the heap
    // on this shell build. The 300ms owner tick replays the same hide once
    // the fences have settled; the icons stay visible for one tick.
    StartupLog(L"deferred ApplyDesktopIconsHidden to owner tick");
    WatchDesktopIconVisibility();
    StartupLog(L"after WatchDesktopIconVisibility");

    // ScanAndConstrainIcons disabled for now — cross-process SendMessage
    // to explorer's ListView can cause deadlocks.
    // SetTimer(g_owner, 1, 500, nullptr);

    // Follow desktop icon zoom live (Ctrl+wheel / View menu): each tick
    // measures the icon list's live glyph size out of explorer's address
    // space (the registry value is persisted lazily and lags wheel zoom)
    // and every fence re-adopts it. Each fence early-outs unless the size
    // actually changed.
    SetTimer(g_owner, 2, 300, nullptr);

    StartupLog(L"entering message loop");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    FenceWindow::SetTeardown(true);   // teardown is not a zoom dismissal
    g_fences.clear();
    return 0;
}
