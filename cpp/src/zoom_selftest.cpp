// Zoom self-test harness. Built as a DLL so a signed host can load it (an
// unsigned standalone test exe trips SmartScreen on this machine); a small
// PowerShell LoadLibrary/GetProcAddress loader works, and so does rundll32
// where its DLL policy allows unsigned libraries:
//
//   rundll32.exe zoom_selftest.dll,ZoomSelfTest
//
// It creates two fences under a hidden host window, drives the real
// ZoomOpen/ZoomRequestClose paths (including the 16 ms timer and the owner's
// 300 ms poll), and asserts geometry, icon metrics and sibling visibility.
// Results go to OPENFENCES_TEST_LOG (or %TEMP%\openfences-zoomselftest.log).
#include "fence_window.h"
#include "zoom_overlay.h"
#include "render.h"
#include "config.h"
#include <memory>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cmath>
#include <algorithm>

std::vector<std::unique_ptr<FenceWindow>> g_fences;
bool g_allHidden = false;
bool g_deskHidden = false;

namespace {

FILE* g_log = nullptr;
int g_failures = 0;

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    va_end(ap);
    fwprintf(g_log, L"\n");
    fflush(g_log);
}

void Check(bool ok, const wchar_t* fmt, ...) {
    if (!ok) g_failures++;
    if (!g_log) return;
    wchar_t text[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(text, _TRUNCATE, fmt, ap);
    va_end(ap);
    fwprintf(g_log, L"%s %s\n", ok ? L"PASS" : L"FAIL", text);
    fflush(g_log);
}

bool StyleVisible(HWND h) {
    return h && (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) != 0;
}

void Pump(FenceWindow* f, int ms, int pollIconPx) {
    DWORD end = GetTickCount() + (DWORD)ms;
    DWORD nextPoll = GetTickCount();
    while ((int)(end - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DWORD now = GetTickCount();
        if (f && pollIconPx > 0 && (int)(now - nextPoll) >= 0) {
            f->RestoreAnchor();
            f->SyncDesktopIconSize(pollIconPx);
            nextPoll = now + 300;
        }
        Sleep(5);
    }
}

RECT ExpectedZoomRect(HWND h) {
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    int ww = mi.rcWork.right - mi.rcWork.left;
    int wh = mi.rcWork.bottom - mi.rcWork.top;
    int tw = ww * 58 / 100;
    int th = wh * 66 / 100;
    return RECT{ mi.rcWork.left + (ww - tw) / 2,
                 mi.rcWork.top + (wh - th) / 2,
                 mi.rcWork.left + (ww - tw) / 2 + tw,
                 mi.rcWork.top + (wh - th) / 2 + th };
}

void CheckRect(const wchar_t* what, const RECT& got, const RECT& want, int tol = 2) {
    Check(abs(got.left - want.left) <= tol && abs(got.top - want.top) <= tol &&
          abs(got.right - want.right) <= tol && abs(got.bottom - want.bottom) <= tol,
          L"%s rect got (%ld,%ld,%ld,%ld) want (%ld,%ld,%ld,%ld)",
          what, got.left, got.top, got.right, got.bottom,
          want.left, want.top, want.right, want.bottom);
}

struct SavedPos { std::wstring path; float x, y; };

std::vector<SavedPos> Snapshot(const FenceWindow* f) {
    std::vector<SavedPos> out;
    for (const auto& ic : f->Icons()) out.push_back({ ic.path, ic.x, ic.y });
    return out;
}

void CheckPositions(FenceWindow* f, const std::vector<SavedPos>& want, const wchar_t* what) {
    bool same = want.size() == f->Icons().size();
    if (same) {
        for (const auto& p : want) {
            auto it = std::find_if(f->Icons().begin(), f->Icons().end(),
                [&](const IconEntry& e) { return e.path == p.path; });
            if (it == f->Icons().end() || it->x != p.x || it->y != p.y) { same = false; break; }
        }
    }
    Check(same, L"%s icon positions restored", what);
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

HWND CreateHost() {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"OpenFencesZoomTestHost";
    RegisterClassExW(&wc);
    // Kept WS_VISIBLE (so IsWindowVisible works for the children, exactly
    // like the real desktop host) but parked far off the virtual screen.
    return CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"",
                           WS_POPUP | WS_VISIBLE,
                           -5000, -5000, 3000, 2000, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

void RunTests() {
    FenceWindow::RegisterClass(GetModuleHandleW(nullptr));
    HWND host = CreateHost();
    if (!host) { Log(L"RESULT: FAIL — test host window not created"); return; }
    FenceWindow::SetHostOverride(host);

    FenceData fd1{ L"t1", L"ZoomTest", 100, 100, 420, 320 };
    FenceData fd2{ L"t2", L"Sibling", 700, 100, 300, 220 };
    g_fences.push_back(std::make_unique<FenceWindow>(fd1));
    g_fences.push_back(std::make_unique<FenceWindow>(fd2));
    FenceWindow::SetHostOverride(nullptr);

    FenceWindow* f = g_fences[0].get();
    FenceWindow* sibling = g_fences[1].get();
    if (!f->Hwnd() || !sibling->Hwnd()) {
        Log(L"fence hwnd=%p sibling=%p lastError=%lu",
            f->Hwnd(), sibling->Hwnd(), GetLastError());
        Log(L"RESULT: FAIL — fence windows not created");
        FenceWindow::SetTeardown(true);
        g_fences.clear();
        DestroyWindow(host);
        return;
    }

    const wchar_t* paths[] = {
        L"C:\\Windows\\System32\\notepad.exe",
        L"C:\\Windows\\System32\\explorer.exe",
        L"C:\\Windows\\System32\\cmd.exe",
        L"C:\\Windows\\System32\\mspaint.exe"
    };
    std::vector<IconEntry> icons;
    for (int i = 0; i < 4; i++) {
        IconEntry e;
        e.name = L"Icon";
        e.name += (wchar_t)(L'0' + i);
        e.path = paths[i];
        icons.push_back(e);
    }
    f->SetIcons(icons);
    f->RelayoutIcons();
    sibling->SetIcons({});

    RECT base = {};
    GetWindowRect(f->Hwnd(), &base);
    int baseW = (int)f->GetRender().Width();
    int baseH = (int)f->GetRender().Height();
    float baseImg = f->GetRender().Appearance().imgSize;
    auto basePos = Snapshot(f);
    RECT want = ExpectedZoomRect(f->Hwnd());
    Log(L"== Zoom self-test ==");
    Log(L"base rect (%ld,%ld,%ld,%ld) render %dx%d img=%.1f",
        base.left, base.top, base.right, base.bottom, baseW, baseH, baseImg);

    // A. Normal open, live poll churn, dismiss by clicking the fence body.
    f->ZoomOpen();
    Pump(f, 500, (int)baseImg);
    RECT mid = {};
    GetWindowRect(f->Hwnd(), &mid);
    Pump(f, 800, (int)baseImg);
    RECT open = {};
    GetWindowRect(f->Hwnd(), &open);
    Check(f->IsZoomed(), L"A: zoom armed");
    Check(ZoomOverlay::Active(), L"A: shade active");
    CheckRect(L"A: open", open, want);
    Check(mid.left == open.left && mid.top == open.top &&
          mid.right == open.right && mid.bottom == open.bottom,
          L"A: rect stable after open (no snap-back)");
    Check(f->GetData().x == base.left && f->GetData().y == base.top &&
          f->GetData().w == baseW && f->GetData().h == baseH,
          L"A: GetData reports the base rect while zoomed");
    Check(f->GetRender().Appearance().imgSize > baseImg * 1.2f,
          L"A: icons scaled (%.1f -> %.1f)", baseImg, f->GetRender().Appearance().imgSize);
    Check(!StyleVisible(sibling->Hwnd()), L"A: sibling hidden while zoomed");

    int cx = (open.right - open.left) - 24;
    int cy = (open.bottom - open.top) - 24;
    SendMessageW(f->Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
    SendMessageW(f->Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(cx, cy));
    Pump(f, 1200, (int)baseImg);
    RECT closed = {};
    GetWindowRect(f->Hwnd(), &closed);
    Check(!f->IsZoomed(), L"A: closed after body click");
    Check(!ZoomOverlay::Active(), L"A: shade destroyed");
    CheckRect(L"A: closed", closed, base, 1);
    Check((int)f->GetRender().Width() == baseW && (int)f->GetRender().Height() == baseH,
          L"A: render size restored");
    Check(fabs(f->GetRender().Appearance().imgSize - baseImg) < 0.01f,
          L"A: icon metrics restored");
    CheckPositions(f, basePos, L"A:");
    Check(StyleVisible(sibling->Hwnd()), L"A: sibling restored");

    // B. A click that arrives while the opening is still in flight must not
    //    be swallowed (the old code returned early and left the zoom armed).
    f->ZoomOpen();
    f->ZoomRequestClose();
    Pump(f, 1200, (int)baseImg);
    GetWindowRect(f->Hwnd(), &closed);
    Check(!f->IsZoomed(), L"B: close during opening settles");
    Check(!ZoomOverlay::Active(), L"B: shade destroyed");
    CheckRect(L"B: closed", closed, base, 1);
    Check((int)f->GetRender().Width() == baseW, L"B: render size restored");

    // C. A timer that never fires (message loop starved) must be picked up
    //    by the owner's watchdog instead of leaving the zoom half-open.
    f->ZoomOpen();
    Sleep(900);
    f->ZoomStallCheck();
    GetWindowRect(f->Hwnd(), &open);
    Check(f->IsZoomed(), L"C: watchdog left the zoom armed");
    CheckRect(L"C: watchdog settled the target", open, want);
    f->ZoomRequestClose();
    Pump(f, 1200, (int)baseImg);
    GetWindowRect(f->Hwnd(), &closed);
    Check(!f->IsZoomed(), L"C: watchdog path closes cleanly");
    CheckRect(L"C: watchdog path restored", closed, base, 1);

    // D. Destroying the zoomed fence must release the shade and the siblings.
    f->ZoomOpen();
    Pump(f, 500, (int)baseImg);
    Check(f->IsZoomed(), L"D: zoom armed before destroy");
    g_fences.erase(std::remove_if(g_fences.begin(), g_fences.end(),
        [&](const std::unique_ptr<FenceWindow>& p) { return p.get() == f; }),
        g_fences.end());
    Check(!ZoomOverlay::Active(), L"D: shade destroyed with fence");
    Check(StyleVisible(sibling->Hwnd()), L"D: sibling restored with fence");

    FenceWindow::SetTeardown(true);
    g_fences.clear();
    FenceWindow::SetTeardown(false);
    DestroyWindow(host);
    Log(L"RESULT: %s (%d failures)", g_failures ? L"FAIL" : L"PASS", g_failures);
}

} // namespace

// Diagnostic export: load the REAL user config under a hidden host. This
// reproduces the startup path (appearance + icon restore + relayout) that a
// sandboxed app cannot reach because it cannot parent to explorer's list.
extern "C" __declspec(dllexport) void CALLBACK LoadConfigTest(HWND, HINSTANCE, LPWSTR, int) {
    OleInitialize(nullptr);
    wchar_t path[MAX_PATH * 2] = {};
    GetEnvironmentVariableW(L"OPENFENCES_TEST_LOG", path,
                            (DWORD)(sizeof(path) / sizeof(path[0])));
    if (!path[0]) return;
    FILE* log = _wfopen(path, L"w, ccs=UTF-8");
    if (log) { fwprintf(log, L"LoadConfigTest begin\n"); fflush(log); }

    FenceWindow::RegisterClass(GetModuleHandleW(nullptr));
    HWND host = CreateHost();
    if (log) { fwprintf(log, L"host=%p\n", host); fflush(log); }
    FenceWindow::SetHostOverride(host);
    bool ok = Config::LoadApp();
    if (log) { fwprintf(log, L"LoadApp=%d fences=%zu\n", ok ? 1 : 0, g_fences.size()); fflush(log); }
    for (auto& f : g_fences) {
        if (log) { fwprintf(log, L"fence '%ls' hwnd=%p icons=%zu size=%ux%u\n",
            f->GetData().title.c_str(), f->Hwnd(), f->Icons().size(),
            f->GetRender().Width(), f->GetRender().Height()); fflush(log); }
    }
    FenceWindow::SetHostOverride(nullptr);
    FenceWindow::SetTeardown(true);
    g_fences.clear();
    FenceWindow::SetTeardown(false);
    if (host) DestroyWindow(host);
    if (log) { fwprintf(log, L"LoadConfigTest done\n"); fclose(log); }
    OleUninitialize();
}

extern "C" __declspec(dllexport) void CALLBACK ZoomSelfTest(HWND, HINSTANCE, LPWSTR, int) {
    OleInitialize(nullptr);

    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(L"OPENFENCES_TEST_LOG", path,
                                      (DWORD)(sizeof(path) / sizeof(path[0])));
    if (!n) {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        wcscpy_s(path, tmp);
        wcscat_s(path, L"openfences-zoomselftest.log");
    }
    g_log = _wfopen(path, L"w, ccs=UTF-8");
    if (!g_log) return;

    RunTests();
    fclose(g_log);
    g_log = nullptr;
    OleUninitialize();
}