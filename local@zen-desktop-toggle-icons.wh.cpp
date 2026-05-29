// ==WindhawkMod==
// @id              zen-desktop-toggle-icons
// @name            ZenDesktop: Double Click to Hide Icons
// @description     Native C++ Windhawk mod to hide/show desktop icons by double-clicking. Auto-hides on system-wide inactivity and restores on any user input.
// @version         3.1.0
// @author          Lanbo
// @github          https://github.com/Liset999
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lcomctl32 -lole32 -loleaut32 -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# ZenDesktop: Double Click to Hide Icons

Have you ever wanted a clean, clutter-free desktop but found it annoying to right-click -> View -> Show desktop icons every time? 

**ZenDesktop** brings the ultimate native solution to Windows. It allows you to **double-click empty space on your desktop** to instantly hide or show your icons, and optionally **automatically hide icons** after a configurable period of system inactivity.

### 🌟 Key Features:
* **Zero UI Overhead**: Embedded natively inside `explorer.exe` process space. No background EXE running, no taskbar/tray icons, 0% CPU and virtually 0MB extra RAM.
* **Process-Native Hit-Testing**: When you double-click, it performs a real-time ListView hit-test. If you double-click a file, folder, or shortcut, the default "open" action is triggered normally. If you double-click empty space, it toggles your desktop icons.
* **System-Wide Inactivity Detection**: Uses `GetLastInputInfo()` to track ALL user input (keyboard, mouse anywhere on screen) — icons only auto-hide when you are truly idle, not just away from the desktop.
* **Smart Auto-Restore**: When icons are auto-hidden, any user input (mouse or keyboard anywhere) instantly restores them. Manually hidden icons are never auto-restored.
* **Dynamic Hooking**: Automatically subclasses the shell views, meaning even if your Explorer crashes, restarts, or you plug/unplug monitors, the mod remains fully active and stable.
* **Completely Safe**: Uses native Windows `0x7402` (WM_COMMAND) toggle signals, ensuring Windows handles the fade animations and desktop state natively without breaking icon grid arrangements.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- enableAutoHide: true
  $name: "Auto-Hide Icons on Inactivity (自动隐藏)"
  $description: "Automatically hides desktop icons after the system has been idle for the specified duration."
- autoHideDelay: 5
  $name: "Auto-Hide Delay in Seconds (延迟秒数)"
  $description: "Seconds of system-wide inactivity before icons are hidden. Range: 3–60 seconds."
- showIconsOnAnyInput: true
  $name: "Restore Icons on Any Input (任意输入恢复)"
  $description: "When icons were auto-hidden, any mouse or keyboard input instantly restores them. Does NOT affect manually hidden icons."
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <windhawk_utils.h>

#define TIMER_AUTOHIDE   1001
#define WM_REFRESH_TIMER (WM_USER + 5001)

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK DesktopListViewSubclassProc(HWND, UINT, WPARAM, LPARAM, DWORD_PTR);
LRESULT CALLBACK DesktopShellViewSubclassProc(HWND, UINT, WPARAM, LPARAM, DWORD_PTR);
BOOL    CALLBACK EnumWindowsProc(HWND, LPARAM);

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t Real_CreateWindowExW;

// ─────────────────────────────────────────────────────────────────────────────
// Global settings
// ─────────────────────────────────────────────────────────────────────────────
struct Settings {
    bool enableAutoHide;
    int  autoHideDelay;       // seconds, clamped 3–60
    bool showIconsOnAnyInput; // restore on any input after auto-hide
} g_settings;

// ─────────────────────────────────────────────────────────────────────────────
// Settings
// ─────────────────────────────────────────────────────────────────────────────
static void LoadSettings()
{
    g_settings.enableAutoHide      = Wh_GetIntSetting(L"enableAutoHide") != 0;
    g_settings.autoHideDelay       = Wh_GetIntSetting(L"autoHideDelay");
    if (g_settings.autoHideDelay < 3)  g_settings.autoHideDelay = 3;
    if (g_settings.autoHideDelay > 60) g_settings.autoHideDelay = 60;
    g_settings.showIconsOnAnyInput = Wh_GetIntSetting(L"showIconsOnAnyInput") != 0;

    Wh_Log(L"[ZenDesktop] Settings loaded: enableAutoHide=%d, autoHideDelay=%d, showIconsOnAnyInput=%d",
           (int)g_settings.enableAutoHide, g_settings.autoHideDelay, (int)g_settings.showIconsOnAnyInput);
}

// ─────────────────────────────────────────────────────────────────────────────
// Timer management — post WM_REFRESH_TIMER to SHELLDLL_DefView so the timer
// is always created/destroyed on the window-owning thread.
// ─────────────────────────────────────────────────────────────────────────────
static void UpdateTimerState(HWND hwndShell)
{
    if (hwndShell)
        PostMessageW(hwndShell, WM_REFRESH_TIMER, 0, 0);
}

static void UpdateAllTimers()
{
    HWND hwndProgman = FindWindowW(L"Progman", L"Program Manager");
    if (hwndProgman) {
        HWND hwndShell = FindWindowExW(hwndProgman, NULL, L"SHELLDLL_DefView", NULL);
        if (hwndShell) UpdateTimerState(hwndShell);
    }
    HWND hwndWorkerW = NULL;
    while ((hwndWorkerW = FindWindowExW(NULL, hwndWorkerW, L"WorkerW", NULL)) != NULL) {
        HWND hwndShell = FindWindowExW(hwndWorkerW, NULL, L"SHELLDLL_DefView", NULL);
        if (hwndShell) UpdateTimerState(hwndShell);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Icon visibility helpers
//
// ManualToggleIcons  — user double-clicked: toggle and clear auto-hide marker.
// AutoHideIcons      — timer fired: hide only if visible, set auto-hide marker.
// AutoRestoreIcons   — input detected: restore only auto-hidden icons.
//
// "ZenAutoHiddenAt" window property (stores GetTickCount) distinguishes
// auto-hidden from manually-hidden state.
// ─────────────────────────────────────────────────────────────────────────────
static void ManualToggleIcons(HWND hwndShellView)
{
    HWND hwndListView = FindWindowExW(hwndShellView, NULL, L"SysListView32", NULL);
    if (hwndListView) {
        bool isVisible = IsWindowVisible(hwndListView) != 0;
        ShowWindow(hwndListView, isVisible ? SW_HIDE : SW_SHOW);
        Wh_Log(L"[ZenDesktop] ManualToggle: %s", isVisible ? L"HIDE" : L"SHOW");
    } else {
        // Fallback: use the native shell toggle if ListView handle is missing
        PostMessageW(hwndShellView, WM_COMMAND, 0x7402, 0);
        Wh_Log(L"[ZenDesktop] ManualToggle: fallback WM_COMMAND 0x7402");
    }
    // Clear auto-hide marker — this was a deliberate user action
    RemovePropW(hwndShellView, L"ZenAutoHiddenAt");
}

static void AutoHideIcons(HWND hwndShellView)
{
    HWND hwndListView = FindWindowExW(hwndShellView, NULL, L"SysListView32", NULL);
    if (hwndListView && IsWindowVisible(hwndListView)) {
        ShowWindow(hwndListView, SW_HIDE);
        DWORD now = GetTickCount();
        // Avoid storing 0 (which means "not set"); extremely unlikely but be safe
        if (now == 0) now = 1;
        SetPropW(hwndShellView, L"ZenAutoHiddenAt", (HANDLE)(ULONG_PTR)now);
        Wh_Log(L"[ZenDesktop] AutoHide: icons hidden at tick=%u", now);
    }
}

static void AutoRestoreIcons(HWND hwndShellView)
{
    HWND hwndListView = FindWindowExW(hwndShellView, NULL, L"SysListView32", NULL);
    if (hwndListView && !IsWindowVisible(hwndListView)) {
        ShowWindow(hwndListView, SW_SHOW);
        Wh_Log(L"[ZenDesktop] AutoRestore: icons restored");
    }
    RemovePropW(hwndShellView, L"ZenAutoHiddenAt");
}

// ─────────────────────────────────────────────────────────────────────────────
// Window enumeration / subclassing
// ─────────────────────────────────────────────────────────────────────────────
BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM)
{
    WCHAR className[256] = {};
    if (!GetClassNameW(hWnd, className, 256)) return TRUE;

    if (wcscmp(className, L"WorkerW") == 0 || wcscmp(className, L"Progman") == 0) {
        HWND hwndShell = FindWindowExW(hWnd, NULL, L"SHELLDLL_DefView", NULL);
        if (hwndShell) {
            Wh_Log(L"[ZenDesktop] Subclassing ShellView=%p (parent=%s)", hwndShell, className);
            WindhawkUtils::SetWindowSubclassFromAnyThread(hwndShell, DesktopShellViewSubclassProc, 0);
            UpdateTimerState(hwndShell);

            HWND hwndListView = FindWindowExW(hwndShell, NULL, L"SysListView32", NULL);
            if (hwndListView) {
                Wh_Log(L"[ZenDesktop] Subclassing ListView=%p", hwndListView);
                WindhawkUtils::SetWindowSubclassFromAnyThread(hwndListView, DesktopListViewSubclassProc, 0);
            }
        }
    }
    return TRUE;
}

static void SubclassExistingWindows() { EnumWindows(EnumWindowsProc, 0); }

static void UnsubclassWindows()
{
    auto Cleanup = [](HWND hwndShell) {
        if (!hwndShell) return;
        KillTimer(hwndShell, TIMER_AUTOHIDE);
        RemovePropW(hwndShell, L"ZenTimerInit");
        RemovePropW(hwndShell, L"ZenAutoHiddenAt");
        
        // Ensure SysListView32 is shown when mod is disabled/unloaded
        HWND lv = FindWindowExW(hwndShell, NULL, L"SysListView32", NULL);
        if (lv) {
            if (!IsWindowVisible(lv)) {
                ShowWindow(lv, SW_SHOW);
                Wh_Log(L"[ZenDesktop] Cleanup: restored SysListView32 visibility");
            }
            WindhawkUtils::RemoveWindowSubclassFromAnyThread(lv, DesktopListViewSubclassProc);
        }
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(hwndShell, DesktopShellViewSubclassProc);
    };

    HWND hwndProgman = FindWindowW(L"Progman", L"Program Manager");
    if (hwndProgman)
        Cleanup(FindWindowExW(hwndProgman, NULL, L"SHELLDLL_DefView", NULL));

    HWND hwndWorkerW = NULL;
    while ((hwndWorkerW = FindWindowExW(NULL, hwndWorkerW, L"WorkerW", NULL)) != NULL)
        Cleanup(FindWindowExW(hwndWorkerW, NULL, L"SHELLDLL_DefView", NULL));
}

// ─────────────────────────────────────────────────────────────────────────────
// CreateWindowExW hook — dynamically subclass new desktop windows
// ─────────────────────────────────────────────────────────────────────────────
// Note: If you experience Explorer flashing on Windows 11 Build 26100 (24H2),
// please disable the mod in the Windhawk UI.
HWND WINAPI Hook_CreateWindowExW(
    DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName,
    DWORD dwStyle, int X, int Y, int nWidth, int nHeight,
    HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
{
    HWND hWnd = Real_CreateWindowExW(dwExStyle, lpClassName, lpWindowName,
        dwStyle, X, Y, nWidth, nHeight, hWndParent, hMenu, hInstance, lpParam);

    if (hWnd && lpClassName && !IS_INTRESOURCE(lpClassName)) {
        if (wcscmp(lpClassName, L"SHELLDLL_DefView") == 0) {
            Wh_Log(L"[ZenDesktop] Hook: new SHELLDLL_DefView=%p created", hWnd);
            WindhawkUtils::SetWindowSubclassFromAnyThread(hWnd, DesktopShellViewSubclassProc, 0);
            UpdateTimerState(hWnd);
        }
        else if (wcscmp(lpClassName, L"SysListView32") == 0) {
            WCHAR parentClass[256] = {};
            if (hWndParent &&
                GetClassNameW(hWndParent, parentClass, 256) &&
                wcscmp(parentClass, L"SHELLDLL_DefView") == 0)
            {
                Wh_Log(L"[ZenDesktop] Hook: new desktop ListView=%p created", hWnd);
                WindhawkUtils::SetWindowSubclassFromAnyThread(hWnd, DesktopListViewSubclassProc, 0);
            }
        }
    }
    return hWnd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Subclass Proc: SysListView32 (Desktop icon ListView)
//   Receives events only when icons are VISIBLE (ListView is the hit target).
//   Responsibility: detect double-click on empty desktop space → manual toggle.
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK DesktopListViewSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, DWORD_PTR)
{
    if (uMsg == WM_LBUTTONDBLCLK || uMsg == WM_LBUTTONDOWN) {
        static DWORD lastClickTime = 0;
        static POINT lastClickPt   = {};

        bool isDblClick = (uMsg == WM_LBUTTONDBLCLK);

        // Fallback manual double-click detection for windows without CS_DBLCLKS.
        if (uMsg == WM_LBUTTONDOWN) {
            DWORD now          = GetTickCount();
            DWORD dblClickTime = GetDoubleClickTime();
            POINT pt = { (short)GET_X_LPARAM(lParam), (short)GET_Y_LPARAM(lParam) };

            if (now - lastClickTime <= dblClickTime &&
                abs(pt.x - lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                abs(pt.y - lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
            {
                isDblClick = true;
            } else {
                lastClickTime = now;
                lastClickPt   = pt;
            }
        }

        if (isDblClick) {
            // Native LVM_HITTEST to check if click landed on empty space.
            LVHITTESTINFO ht = {};
            ht.pt = { (short)GET_X_LPARAM(lParam), (short)GET_Y_LPARAM(lParam) };
            SendMessageW(hWnd, LVM_HITTEST, 0, (LPARAM)&ht);

            if (ht.iItem == -1) {
                // Empty space — manual toggle (not auto-hide).
                HWND hwndParent = GetParent(hWnd);
                if (hwndParent)
                    ManualToggleIcons(hwndParent);
                return 0; // suppress default double-click action
            }
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Subclass Proc: SHELLDLL_DefView
//   Receives events when icons are HIDDEN (ListView hidden → DefView is hit).
//   Responsibilities:
//     • Run the 500 ms auto-hide/restore polling timer.
//     • Immediate restore on mouse move over desktop (when icons are auto-hidden).
//     • Immediate restore on click on desktop (when icons are auto-hidden).
//     • Double-click restore (always works, including manually hidden icons).
// ─────────────────────────────────────────────────────────────────────────────
LRESULT CALLBACK DesktopShellViewSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, DWORD_PTR)
{
    // ── One-time timer bootstrap ─────────────────────────────────────────────
    // On the very first message after subclassing, set up the polling timer.
    bool timerInitialized = GetPropW(hWnd, L"ZenTimerInit") != NULL;
    if (!timerInitialized && g_settings.enableAutoHide) {
        if (SetTimer(hWnd, TIMER_AUTOHIDE, 500, NULL)) {
            SetPropW(hWnd, L"ZenTimerInit", (HANDLE)1);
            Wh_Log(L"[ZenDesktop] Timer bootstrapped for ShellView=%p", hWnd);
        }
    }

    // ── Cross-thread timer refresh (settings change / new window subclassed) ──
    if (uMsg == WM_REFRESH_TIMER) {
        if (g_settings.enableAutoHide) {
            if (SetTimer(hWnd, TIMER_AUTOHIDE, 500, NULL)) {
                SetPropW(hWnd, L"ZenTimerInit", (HANDLE)1);
                Wh_Log(L"[ZenDesktop] Timer refreshed (ON) for ShellView=%p", hWnd);
            }
        } else {
            KillTimer(hWnd, TIMER_AUTOHIDE);
            RemovePropW(hWnd, L"ZenTimerInit");
            Wh_Log(L"[ZenDesktop] Timer killed (OFF) for ShellView=%p", hWnd);
        }
        return 0;
    }

    // ── Auto-hide / auto-restore timer tick (every 500 ms) ───────────────────
    if (uMsg == WM_TIMER && wParam == TIMER_AUTOHIDE) {
        if (!g_settings.enableAutoHide) return 0;

        HWND hwndListView = FindWindowExW(hWnd, NULL, L"SysListView32", NULL);
        if (!hwndListView) return 0;

        bool isVis = IsWindowVisible(hwndListView) != 0;

        LASTINPUTINFO lii = { sizeof(LASTINPUTINFO) };
        if (!GetLastInputInfo(&lii)) return 0;
        DWORD now = GetTickCount();

        DWORD autoHiddenAt = (DWORD)(ULONG_PTR)GetPropW(hWnd, L"ZenAutoHiddenAt");

        if (isVis) {
            // ── Icons visible → check if system has been idle long enough ────
            DWORD idleMs = now - lii.dwTime;
            if (idleMs >= (DWORD)g_settings.autoHideDelay * 1000) {
                Wh_Log(L"[ZenDesktop] Timer: idle %ums >= %us threshold → AutoHide",
                       idleMs, g_settings.autoHideDelay);
                AutoHideIcons(hWnd);
            }
        }
        else if (autoHiddenAt != 0 && g_settings.showIconsOnAnyInput) {
            // ── Icons auto-hidden → check if user provided new input ─────────
            // lii.dwTime = tick of last real user input
            // autoHiddenAt = tick when we auto-hid
            // Require: last input happened AFTER we hid, with ≥500ms gap to
            // avoid restoring from lingering input timestamps.
            if (lii.dwTime > autoHiddenAt && (lii.dwTime - autoHiddenAt) > 500) {
                Wh_Log(L"[ZenDesktop] Timer: new input detected (lastInput=%u, hidAt=%u) → AutoRestore",
                       lii.dwTime, autoHiddenAt);
                AutoRestoreIcons(hWnd);
            }
        }

        return 0;
    }

    // ── Immediate mouse/click restore (only when auto-hidden + restore-on-input enabled) ──
    // Mouse clicks and moves directly on the desktop are immediate indications of activity.
    // To prevent the synthetic WM_MOUSEMOVE generated during ShowWindow(SW_HIDE) from 
    // immediately triggering a restore, we enforce a 100ms time guard after auto-hide.
    {
        DWORD autoHiddenAt = (DWORD)(ULONG_PTR)GetPropW(hWnd, L"ZenAutoHiddenAt");
        if (autoHiddenAt != 0 && g_settings.showIconsOnAnyInput) {
            if (uMsg == WM_MOUSEMOVE || uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN) {
                DWORD now = GetTickCount();
                if (now - autoHiddenAt > 100) {
                    Wh_Log(L"[ZenDesktop] Immediate mouse/click restore on DefView (uMsg=%u)", uMsg);
                    AutoRestoreIcons(hWnd);
                }
            }
        }
    }

    // ── Double-click restore (works for both auto-hidden AND manually hidden) ──
    if (uMsg == WM_LBUTTONDBLCLK || uMsg == WM_LBUTTONDOWN) {
        static DWORD lastClickTime = 0;
        static POINT lastClickPt   = {};

        bool isDblClick = (uMsg == WM_LBUTTONDBLCLK);

        if (uMsg == WM_LBUTTONDOWN) {
            DWORD now          = GetTickCount();
            DWORD dblClickTime = GetDoubleClickTime();
            POINT pt = { (short)GET_X_LPARAM(lParam), (short)GET_Y_LPARAM(lParam) };

            if (now - lastClickTime <= dblClickTime &&
                abs(pt.x - lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
                abs(pt.y - lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
            {
                isDblClick = true;
            } else {
                lastClickTime = now;
                lastClickPt   = pt;
            }
        }

        if (isDblClick) {
            ManualToggleIcons(hWnd);
            return 0;
        }
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Windhawk lifecycle
// ─────────────────────────────────────────────────────────────────────────────
bool Wh_ModInit()
{
    Wh_Log(L"[ZenDesktop] === Wh_ModInit v3.1.0 ===");
    LoadSettings();

    if (!Wh_SetFunctionHook(
            (void*)CreateWindowExW,
            (void*)Hook_CreateWindowExW,
            (void**)&Real_CreateWindowExW)) {
        Wh_Log(L"[ZenDesktop] FAILED to hook CreateWindowExW");
        return false;
    }

    SubclassExistingWindows();
    Wh_Log(L"[ZenDesktop] Init complete");
    return true;
}

void Wh_ModUninit()
{
    Wh_Log(L"[ZenDesktop] === Wh_ModUninit ===");
    UnsubclassWindows();
}

void Wh_ModSettingsChanged()
{
    Wh_Log(L"[ZenDesktop] === Settings changed ===");
    LoadSettings();
    UpdateAllTimers();
}
