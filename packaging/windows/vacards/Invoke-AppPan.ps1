#Requires -Version 5.1
# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  Deterministic SendInput driver for an already-running VA Studio process.
  Workload=pan injects the fixed middle-button canvas pan trajectory;
  Workload=object-drag injects a native left-button selection drag of one live
  content rectangle and native Ctrl+S snapshots. Writes input-summary.json plus
  client start/end PNGs.

.DESCRIPTION
  Reusable, self-contained Windows PowerShell 5.1 helper. It owns exactly the
  supplied process and its main HWND, verifies the physical console session and
  non-elevated identity, fixes/verifies the client area at scale 1 inside the
  physical monitor, foregrounds only the owned window, then injects a fixed
  trajectory with SendInput (absolute virtual-desktop moves).

  Workload=pan (default) is the unchanged middle-button pan: exactly one
  position, then Loops middle-button down/out/back/up legs. The middle button is
  released in finally. No keyboard input.

  Workload=object-drag additionally requires -DocumentPath: an existing
  isolated copy of fixture-simple.svg that resolves under the parent directory
  of OutputDirectory (rejected otherwise). Outside the measured window it saves
  the document through native Ctrl+S (baseline.svg), selects the live-content
  target with one left click at client(250,580), waits a bounded
  double-click-spacing interval (max(1000, GetDoubleClickTime+100), capped
  6000 ms; the old repeated-loop overlap rejection no longer applies to the
  single continuous-v2 drag), then arms ONE continuous native left-button drag:
  it presses at client(246,580) and sends exactly ONE motion to client(250,580)
  so the native selector's first-motion grab anchor is the exact fixed point
  (select-tool.cpp:562-657 grab(p) then moveTo(p)), waits a bounded 1000 ms
  settle, and then keeps the button held through the measured gesture: nine
  out/back cycles at y=580 between x=250 and x=370 (60 steps/leg, 16 ms) and one
  final outbound leg to x=370, released exactly once at the end. The measured
  count is 1141 = 9*120 + 60 + 1 (nine out/back cycles, one final outbound leg,
  one release); there is no measured initial
  position or press. After the measured gesture it saves endpoint.svg, captures
  client-drag-endpoint.png, performs one analogous armed native return drag
  (position 374,580, down, ONE anchor motion to 370,580, bounded 1000 ms settle,
  60 motions 370->250, one release) and saves returned.svg. The
  select/wait/arm/capture/return/save events are recorded separately and are
  never part of the measured 1141-event count or the phase window. Ctrl and the
  left button are always released in finally. No app commands, no OS display
  change, no new dependency.

  Opt-in maximized-workarea calibration (object-drag only): when BOTH
  MaximizedCalibrationPath and ExpectedMaximizedCalibrationSha256 are supplied,
  the helper validates the pinned vacards-maximized-drag-calibration/1 JSON
  (hash, schema, every field/type/bound, whole gesture path inside the
  calibrated physical client) BEFORE any input, establishes a verified
  PER_MONITOR_AWARE_V2 thread context with finally-restore, moves only the owned
  window onto the calibrated monitor, natively maximizes it, and proves the
  maximized work-area geometry (exact client/monitor/work/DPI plus a bounded
  nonclient overhang derived from DPI-aware frame metrics). Both defaults empty
  preserve the legacy normal-client path without change.

.NOTES
  Exit codes: 0 ok, 2 usage/params, 3 physical/session/identity, 4 window/size,
  5 foreground/process lost, 6 SendInput, 7 unexpected/save verification,
  8 GLib clock contract.

  The GLib monotonic clock basis is verified against the exact DLL the app
  loaded: the caller passes GlibDllPath and ExpectedGlibSha256, and this helper
  loads that absolute path with the Windows loader (dependencies resolved from
  the DLL's own directory; no global PATH, no download) and checks
  g_get_monotonic_time() against QPC in 10 samples. A mismatch is a hard
  failure; no offset is ever invented.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 2147483647)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [string]$OutputDirectory,

    [ValidateRange(16, 16384)]
    [int]$ClientWidth = 1280,

    [ValidateRange(16, 16384)]
    [int]$ClientHeight = 800,

    [ValidateRange(1, 100000)]
    [int]$Loops = 10,

    [ValidateRange(1, 100000)]
    [int]$StepsPerLeg = 60,

    [ValidateRange(1, 10000)]
    [int]$StepIntervalMs = 16,

    # Workload selector. 'pan' is the original middle-button pan (default and
    # unchanged). 'object-drag' adds the native left-button selection drag and
    # native Ctrl+S snapshots and requires DocumentPath.
    [ValidateSet('pan', 'object-drag')]
    [string]$Workload = 'pan',

    # Object-drag only. Absolute path of the single isolated copy of
    # fixture-simple.svg the owned app has opened. It must exist and resolve
    # under the parent directory of OutputDirectory; any other path is rejected
    # (exit 2). Ignored/optional for Workload=pan.
    [string]$DocumentPath = '',

    # Exact libglib-2.0-0.dll the owned app has loaded (absolute path + hash).
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$GlibDllPath,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9a-fA-F]{64}$')]
    [string]$ExpectedGlibSha256,

    # Opt-in (default false). When no native HTCAPTION point exists (GTK
    # client-side decoration exposes no Windows caption), allow exactly ONE
    # preparatory click at the visually-verified beta5 CSD titlebar client point
    # (ClientW/2, 30), root-verified before move and again before press.
    # Without this switch the helper fails closed and injects nothing.
    [switch]$AllowVerifiedCsdCaption,

    # Opt-in maximized-workarea calibration (object-drag only). Both must be
    # supplied together or both left empty. Supplying both switches this helper
    # to the calibrated physical select point and native maximized geometry
    # proof. Defaults empty preserve the legacy normal-client path byte-for-byte.
    # ExpectedMaximizedCalibrationSha256 is the pinned SHA-256 of the file.
    [string]$MaximizedCalibrationPath = '',

    [ValidatePattern('^([0-9a-fA-F]{64})?$')]
    [string]$ExpectedMaximizedCalibrationSha256 = '',

    # Optional screen-observer hook (object-drag maximized-workarea only). All
    # four must be supplied together or all left empty (fail closed otherwise).
    # ObserverExePath is the compiled app-screen-observer.exe (hash pinned).
    # ObserverRoiConfigPath is the pinned vacards-screen-observer-roi/1 JSON
    # (hash pinned); Read-ObserverRoiConfig pins it to the SAME maximized
    # calibration monitor/work geometry. Supplying all four runs the observer
    # over the measured drag window and records its evidence. Defaults empty
    # preserve the legacy protocol and launch no observer.
    [string]$ObserverExePath = '',

    [ValidatePattern('^([0-9a-fA-F]{64})?$')]
    [string]$ExpectedObserverExeSha256 = '',

    [string]$ObserverRoiConfigPath = '',

    [ValidatePattern('^([0-9a-fA-F]{64})?$')]
    [string]$ExpectedObserverRoiConfigSha256 = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Embedded Win32 interop. Kept as a here-string so the isolated check can
# extract and compile exactly this source. Only small struct wrappers and pure
# geometry helpers live here.
# ---------------------------------------------------------------------------
$VAPanInteropSource = @'
using System;
using System.Runtime.InteropServices;

namespace VAPan
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    // Windows MONITORINFOEXW is 104 bytes (40-byte fixed part + 32 WCHAR).
    // The Unicode CharSet is required so the ByValTStr field marshals as 32
    // WCHAR (offset 40, total 104) to match the GetMonitorInfoW cbSize check;
    // the default Ansi layout is 72 bytes and the API rejects it.
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct MONITORINFOEX
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WINDOWPLACEMENT
    {
        public int length;
        public int flags;
        public int showCmd;
        public POINT ptMinPosition;
        public POINT ptMaxPosition;
        public RECT rcNormalPosition;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT
    {
        public int dx;
        public int dy;
        public uint mouseData;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT
    {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION
    {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT
    {
        public uint type;
        public INPUTUNION u;
    }

    public static class Win32
    {
        public const uint INPUT_MOUSE = 0;
        public const uint INPUT_KEYBOARD = 1;
        public const uint MOUSEEVENTF_MOVE = 0x0001;
        public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
        public const uint MOUSEEVENTF_LEFTUP = 0x0004;
        public const uint MOUSEEVENTF_MIDDLEDOWN = 0x0020;
        public const uint MOUSEEVENTF_MIDDLEUP = 0x0040;
        public const uint MOUSEEVENTF_ABSOLUTE = 0x8000;
        public const uint MOUSEEVENTF_VIRTUALDESK = 0x4000;
        public const uint MOVE_FLAGS = 0xC001; // MOVE | ABSOLUTE | VIRTUALDESK
        public const uint KEYEVENTF_KEYUP = 0x0002;

        // Virtual-key codes used only for the native Ctrl+S save and the
        // modifier-held refusal gate.
        public const int VK_SHIFT = 0x10;
        public const int VK_CONTROL = 0x11;
        public const int VK_MENU = 0x12;
        public const int VK_LWIN = 0x5B;
        public const int VK_RWIN = 0x5C;
        public const int VK_LSHIFT = 0xA0;
        public const int VK_RSHIFT = 0xA1;
        public const int VK_LCONTROL = 0xA2;
        public const int VK_RCONTROL = 0xA3;
        public const int VK_LMENU = 0xA4;
        public const int VK_RMENU = 0xA5;
        public const int VK_S = 0x53;

        // Preparatory-focus hit-test constants (no input).
        public const uint WM_NCHITTEST = 0x0084;
        public const int HTCAPTION = 2;
        public const uint GA_ROOT = 2;
        public const uint SWP_NOSIZE = 0x0001;
        public const uint SWP_NOMOVE = 0x0002;
        public const uint SMTO_ABORTIFHUNG = 0x0002;
        // SetWindowPos hWndInsertAfter sentinels for the temporary topmost raise.
        public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
        public static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);

        public const int SM_XVIRTUALSCREEN = 76;
        public const int SM_YVIRTUALSCREEN = 77;
        public const int SM_CXVIRTUALSCREEN = 78;
        public const int SM_CYVIRTUALSCREEN = 79;

        public const uint SWP_NOZORDER = 0x0004;
        public const uint SWP_NOACTIVATE = 0x0010;
        public const uint SWP_SHOWWINDOW = 0x0040;
        public const int SW_RESTORE = 9;
        public const int SW_SHOWNORMAL = 1;
        public const int SW_SHOWMINIMIZED = 2;
        public const int SW_SHOWMAXIMIZED = 3;
        public const uint WM_SYSCOMMAND = 0x0112;
        public const int SC_RESTORE = 0xF120;
        public const int SC_MAXIMIZE = 0xF030;
        // DPI-aware frame metrics (Windows 10+). SM_CXSIZEFRAME/SM_CYSIZEFRAME
        // and SM_CXPADDEDBORDER give the native maximized nonclient overhang.
        public const int SM_CXSIZEFRAME = 32;
        public const int SM_CYSIZEFRAME = 33;
        public const int SM_CXPADDEDBORDER = 92;
        // PER_MONITOR_AWARE_V2 pseudo-handle (-4). SetThreadDpiAwarenessContext
        // returns the previous context; a finally restores it.
        public const int DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4;
        public const uint MONITOR_DEFAULTTONEAREST = 2;
        public const int GWL_STYLE = -16;
        public const int GWL_EXSTYLE = -20;
        public const int WS_EX_TOPMOST = 0x00000008;
        public const uint GW_OWNER = 4;
        public const uint PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
        public const uint TOKEN_QUERY = 0x0008;
        public const int TokenUser = 1;
        public const int TokenElevation = 20;
        public const int WTSClientProtocolType = 16;
        public const int WTSIsRemoteSession = 29;
        public const uint INVALID_SESSION = 0xFFFFFFFF;

        [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
        [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
        [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
        [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
        [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
        [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
        [DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
        [DllImport("user32.dll")] public static extern bool SetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
        [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
        [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
        [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
        [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
        [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, uint flags);
        [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr mon, ref MONITORINFO mi);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetMonitorInfoW", SetLastError = true)]
        public static extern bool GetMonitorInfoExW(IntPtr mon, ref MONITORINFOEX mi);
        [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
        [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam, uint fuFlags, uint uTimeout, out IntPtr lpdwResult);
        [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
        [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
        [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
        [DllImport("user32.dll")] public static extern bool AdjustWindowRectEx(ref RECT r, uint style, bool menu, uint exstyle);
        [DllImport("user32.dll")] public static extern int GetSystemMetrics(int index);
        [DllImport("user32.dll")] public static extern int GetSystemMetricsForDpi(int index, uint dpi);
        [DllImport("user32.dll", SetLastError = true)] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);
        [DllImport("user32.dll")] public static extern IntPtr GetThreadDpiAwarenessContext();
        [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr a, IntPtr b);
        [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int index);
        [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
        [DllImport("user32.dll", EntryPoint = "GetDpiForWindow")] public static extern uint GetDpiForWindow(IntPtr h);
        [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
        // Read-only system double-click interval (ms). Never set by this helper.
        [DllImport("user32.dll")] public static extern uint GetDoubleClickTime();
        [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
        [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
        public delegate bool EnumWindowsProc(IntPtr h, IntPtr lParam);

        [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] public static extern bool ProcessIdToSessionId(uint pid, out uint session);
        [DllImport("kernel32.dll")] public static extern uint WTSGetActiveConsoleSessionId();
        [DllImport("kernel32.dll")] public static extern bool QueryPerformanceCounter(out long v);
        [DllImport("kernel32.dll")] public static extern bool QueryPerformanceFrequency(out long v);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern IntPtr LoadLibraryExW(string lpLibFileName, IntPtr hFile, uint dwFlags);
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Ansi, ExactSpelling = true)]
        public static extern IntPtr GetProcAddress(IntPtr hModule, string lpProcName);
        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern bool FreeLibrary(IntPtr hModule);
        [DllImport("kernel32.dll")] public static extern uint GetLastError();

        public const uint LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100;
        public const uint LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000;
        // Resolve the GLib import only from the DLL's own directory + system
        // dirs; never from the process PATH and never from any other app arm.
        public const uint GLIB_LOAD_FLAGS = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

        public delegate long GlibMonotonicDelegate();

        public static IntPtr LoadLibraryAbsolute(string absolutePath)
        {
            return LoadLibraryExW(absolutePath, IntPtr.Zero, GLIB_LOAD_FLAGS);
        }

        public static IntPtr GetExport(IntPtr module, string name)
        {
            if (module == IntPtr.Zero) return IntPtr.Zero;
            return GetProcAddress(module, name);
        }

        public static long CallGlibMonotonic(IntPtr fn)
        {
            if (fn == IntPtr.Zero) throw new InvalidOperationException("g_get_monotonic_time export is null");
            GlibMonotonicDelegate d = (GlibMonotonicDelegate)Marshal.GetDelegateForFunctionPointer(fn, typeof(GlibMonotonicDelegate));
            return d();
        }

        [DllImport("advapi32.dll", SetLastError = true)] public static extern bool OpenProcessToken(IntPtr h, uint access, out IntPtr token);
        [DllImport("advapi32.dll", SetLastError = true)] public static extern bool GetTokenInformation(IntPtr token, int cls, IntPtr info, uint len, out uint ret);

        [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool WTSQuerySessionInformationW(IntPtr server, uint session, int cls, out IntPtr buf, out uint bytes);
        [DllImport("wtsapi32.dll")] public static extern void WTSFreeMemory(IntPtr p);

        // --- pure helpers (no window/desktop needed; exercised by isolated check) ---

        public static int InputStructSize() { return Marshal.SizeOf(typeof(INPUT)); }

        public static int[] NormalizeAbsolute(int sx, int sy, int vx, int vy, int vw, int vh)
        {
            if (vw < 2) vw = 2;
            if (vh < 2) vh = 2;
            long ax = (long)(sx - vx) * 65535L / (long)(vw - 1);
            long ay = (long)(sy - vy) * 65535L / (long)(vh - 1);
            if (ax < 0) ax = 0; if (ax > 65535) ax = 65535;
            if (ay < 0) ay = 0; if (ay > 65535) ay = 65535;
            return new int[] { (int)ax, (int)ay };
        }

        // Physical-pixel-bin centre normalization for the opt-in maximized
        // calibration mode ONLY (the legacy NormalizeAbsolute above is untouched).
        //
        // SendInput with MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK maps the
        // 0..65535 axis to physical pixel  v + floor(a * extent / 65536). The
        // legacy 65535/(extent-1) mapping is not the exact inverse: at 4K
        // (extent 2160) target y=30 normalizes to a=910, which Windows maps back
        // to pixel 29, so an exact-cursor guard correctly refuses the press.
        // This returns the centre of the target pixel's Windows bin instead:
        // a = floor((2*(p-v) + 1) * 65536 / (2*extent)). 64-bit arithmetic keeps
        // every pixel of every axis round-tripping; results are clamped to
        // 0..65535. Returns null (fail closed) for an invalid extent or a
        // coordinate outside the virtual desktop.
        public static int[] NormalizeAbsolutePixelCenter(int sx, int sy, int vx, int vy, int vw, int vh)
        {
            if (vw < 1 || vh < 1) return null;
            long rx = (long)sx - (long)vx;
            long ry = (long)sy - (long)vy;
            if (rx < 0 || rx >= (long)vw || ry < 0 || ry >= (long)vh) return null;
            long ax = ((2L * rx + 1L) * 65536L) / (2L * (long)vw);
            long ay = ((2L * ry + 1L) * 65536L) / (2L * (long)vh);
            if (ax < 0) ax = 0; if (ax > 65535) ax = 65535;
            if (ay < 0) ay = 0; if (ay > 65535) ay = 65535;
            return new int[] { (int)ax, (int)ay };
        }

        public static int StepCoord(int start, int end, int s, int steps)
        {
            if (steps <= 0) return start;
            double dx = (double)(end - start) / (double)steps;
            return (int)Math.Round((double)start + dx * (double)s, MidpointRounding.AwayFromZero);
        }

        public static long Qpc() { long v; QueryPerformanceCounter(out v); return v; }
        public static long QpcFrequency() { long v; QueryPerformanceFrequency(out v); return v; }

        // --- window helpers ---

        public static bool ForegroundIs(IntPtr h) { return GetForegroundWindow() == h; }
        public static bool Visible(IntPtr h) { return IsWindowVisible(h); }
        public static bool Minimized(IntPtr h) { return IsIconic(h); }
        public static bool Maximized(IntPtr h) { return IsZoomed(h); }
        public static void Foreground(IntPtr h) { SetForegroundWindow(h); }
        public static void Restore(IntPtr h) { ShowWindow(h, SW_RESTORE); }

        // Restore from EITHER maximized or minimized to the normal state. A
        // maximized window ignores SetWindowPos sizing, so this MUST run before
        // any client-area resize. No-op when the window is already normal.
        //
        // ShowWindow(SW_RESTORE) alone did NOT clear the maximized state for the
        // real GTK app (isolated geometry probe 2026-09-21: still is_zoomed=true,
        // showCmd=3 after 5 s). The canonical supported path is
        // SetWindowPlacement with showCmd=SW_SHOWNORMAL; ShowWindow(SW_RESTORE)
        // and WM_SYSCOMMAND/SC_RESTORE remain as fallbacks. No app change.
        public static void RestoreNormal(IntPtr h)
        {
            if (!IsIconic(h) && !IsZoomed(h)) return;
            if (IsIconic(h)) { ShowWindow(h, SW_RESTORE); }
            if (IsZoomed(h))
            {
                WINDOWPLACEMENT wp = new WINDOWPLACEMENT();
                wp.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
                if (GetWindowPlacement(h, ref wp))
                {
                    wp.showCmd = SW_SHOWNORMAL;
                    SetWindowPlacement(h, ref wp);
                }
                if (IsZoomed(h)) { ShowWindow(h, SW_RESTORE); }
                if (IsZoomed(h)) { PostMessage(h, WM_SYSCOMMAND, new IntPtr(SC_RESTORE), IntPtr.Zero); }
            }
        }

        public static bool NormalState(IntPtr h) { return !IsIconic(h) && !IsZoomed(h); }

        // Bounded wait until the window is neither minimized nor maximized.
        public static bool WaitNormal(IntPtr h, int timeoutMs)
        {
            int waited = 0;
            while (true)
            {
                if (!IsIconic(h) && !IsZoomed(h)) return true;
                if (waited >= timeoutMs) return false;
                System.Threading.Thread.Sleep(25);
                waited += 25;
            }
        }

        // Actively restore (SetWindowPlacement/SW_SHOWNORMAL first) and wait,
        // retrying within the bound, so a re-maximizing window is still caught.
        public static bool ForceNormal(IntPtr h, int timeoutMs)
        {
            int waited = 0;
            while (true)
            {
                if (!IsIconic(h) && !IsZoomed(h)) return true;
                RestoreNormal(h);
                if (!IsIconic(h) && !IsZoomed(h)) return true;
                if (waited >= timeoutMs) return false;
                System.Threading.Thread.Sleep(25);
                waited += 25;
            }
        }

        // Bounded wait for the exact client size. Never accepts a different size.
        public static bool WaitClientSize(IntPtr h, int cw, int ch, int timeoutMs)
        {
            int waited = 0;
            while (true)
            {
                RECT r;
                if (GetClientRect(h, out r) && (r.Right - r.Left) == cw && (r.Bottom - r.Top) == ch) return true;
                if (waited >= timeoutMs) return false;
                System.Threading.Thread.Sleep(50);
                waited += 50;
            }
        }

        // [showCmd, flags, normalLeft, normalTop, normalRight, normalBottom]
        public static int[] WindowPlacementInfo(IntPtr h)
        {
            WINDOWPLACEMENT wp = new WINDOWPLACEMENT();
            wp.length = Marshal.SizeOf(typeof(WINDOWPLACEMENT));
            if (!GetWindowPlacement(h, ref wp)) return null;
            return new int[] { wp.showCmd, wp.flags,
                wp.rcNormalPosition.Left, wp.rcNormalPosition.Top,
                wp.rcNormalPosition.Right, wp.rcNormalPosition.Bottom };
        }

        public static bool BoundsInsideMonitor(IntPtr h)
        {
            int[] b = WindowBounds(h);
            int[] m = MonitorRects(h);
            if (b == null || m == null) return false;
            return b[0] >= m[0] && b[1] >= m[1] && b[2] <= m[2] && b[3] <= m[3];
        }

        public static uint WindowPid(IntPtr h) { uint p; GetWindowThreadProcessId(h, out p); return p; }

        public static int[] ClientSize(IntPtr h) { RECT r; GetClientRect(h, out r); return new int[] { r.Right - r.Left, r.Bottom - r.Top }; }
        public static int[] WindowBounds(IntPtr h) { RECT r; GetWindowRect(h, out r); return new int[] { r.Left, r.Top, r.Right, r.Bottom }; }
        public static int[] ClientScreenOrigin(IntPtr h) { POINT p; p.X = 0; p.Y = 0; ClientToScreen(h, ref p); return new int[] { p.X, p.Y }; }
        public static int[] ClientToScreenPoint(IntPtr h, int cx, int cy) { POINT p; p.X = cx; p.Y = cy; ClientToScreen(h, ref p); return new int[] { p.X, p.Y }; }
        public static int[] CursorClient(IntPtr h) { POINT p; GetCursorPos(out p); int sx = p.X; int sy = p.Y; ScreenToClient(h, ref p); return new int[] { p.X, p.Y, sx, sy }; }

        public static int[] VirtualScreen()
        {
            return new int[] { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        }
        public static int[] ScreenToAbsolute(int sx, int sy)
        {
            int[] v = VirtualScreen();
            return NormalizeAbsolute(sx, sy, v[0], v[1], v[2], v[3]);
        }
        // Explicit mode dispatch: pixelCenter=false is the legacy normal-client
        // path byte-for-byte; pixelCenter=true is the opt-in maximized
        // calibration path. May return null in pixel-center mode (fail closed).
        public static int[] ScreenToAbsoluteForMode(int sx, int sy, bool pixelCenter)
        {
            int[] v = VirtualScreen();
            if (!pixelCenter) return NormalizeAbsolute(sx, sy, v[0], v[1], v[2], v[3]);
            return NormalizeAbsolutePixelCenter(sx, sy, v[0], v[1], v[2], v[3]);
        }

        public static int[] MonitorRects(IntPtr h)
        {
            IntPtr mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi = new MONITORINFO();
            mi.cbSize = Marshal.SizeOf(typeof(MONITORINFO));
            if (!GetMonitorInfoW(mon, ref mi)) return null;
            return new int[] { mi.rcMonitor.Left, mi.rcMonitor.Top, mi.rcMonitor.Right, mi.rcMonitor.Bottom, mi.rcWork.Left, mi.rcWork.Top, mi.rcWork.Right, mi.rcWork.Bottom };
        }

        // Diagnostics for the most recent MonitorDevice call: the cbSize that was
        // passed and the Win32 error on failure (0 on success). Test-MaximizedGeometryState
        // reports these so an API failure is never mistaken for a genuine device
        // mismatch or an empty name.
        public static int MonitorDeviceLastCbSize = 0;
        public static int MonitorDeviceLastError = 0;

        // Device name of the monitor nearest the window (e.g. \\.\DISPLAY2).
        public static string MonitorDevice(IntPtr h)
        {
            IntPtr mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEX mi = new MONITORINFOEX();
            mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
            MonitorDeviceLastCbSize = mi.cbSize;
            if (!GetMonitorInfoExW(mon, ref mi))
            {
                MonitorDeviceLastError = Marshal.GetLastWin32Error();
                return null;
            }
            MonitorDeviceLastError = 0;
            return mi.szDevice;
        }

        // --- maximized-workarea geometry (pure; exercised by isolated check) ---

        // Per-edge nonclient overhang of the window rect past the monitor rect:
        // [left, top, right, bottom] = mon.edge - win.edge. A maximized window
        // extends past the monitor by the frame border and is therefore positive.
        public static int[] Overhang(int[] win, int[] mon)
        {
            if (win == null || mon == null || win.Length < 4 || mon.Length < 4) return null;
            return new int[] { mon[0] - win[0], mon[1] - win[1], win[2] - mon[2], win[3] - mon[3] };
        }

        // Per-edge nonclient frame width: [left, top, right, bottom] =
        // screen-space client edge - window edge.
        public static int[] FrameDeltas(int[] win, int[] clientScreen)
        {
            if (win == null || clientScreen == null || win.Length < 4 || clientScreen.Length < 4) return null;
            return new int[] { clientScreen[0] - win[0], clientScreen[1] - win[1], win[2] - clientScreen[2], win[3] - clientScreen[3] };
        }

        public static int[] ClientScreenRect(int[] origin, int cw, int ch)
        {
            if (origin == null || origin.Length < 2) return null;
            return new int[] { origin[0], origin[1], origin[0] + cw, origin[1] + ch };
        }

        // outer contains inner on every edge.
        public static bool RectCovers(int[] outer, int[] inner)
        {
            if (outer == null || inner == null || outer.Length < 4 || inner.Length < 4) return false;
            return outer[0] <= inner[0] && outer[1] <= inner[1] && outer[2] >= inner[2] && outer[3] >= inner[3];
        }

        // DPI-aware native frame metrics [cx, cy] for a maximized window.
        public static int[] FrameMetricsForDpi(int dpi)
        {
            int cxFrame, cyFrame, pad;
            try
            {
                cxFrame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, (uint)dpi);
                cyFrame = GetSystemMetricsForDpi(SM_CYSIZEFRAME, (uint)dpi);
                pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, (uint)dpi);
            }
            catch (EntryPointNotFoundException)
            {
                cxFrame = GetSystemMetrics(SM_CXSIZEFRAME);
                cyFrame = GetSystemMetrics(SM_CYSIZEFRAME);
                pad = GetSystemMetrics(SM_CXPADDEDBORDER);
            }
            return new int[] { cxFrame + pad, cyFrame + pad };
        }

        // Establish a verified PER_MONITOR_AWARE_V2 thread context. Returns
        // [ok, already_pmv2, previous_context_raw, last_error]. A finally MUST
        // restore previous_context_raw. Fail closed (ok=0) when the call fails.
        public static long[] EstablishPerMonitorV2()
        {
            long ok = 0, already = 0, prevRaw = 0, err = 0;
            try
            {
                IntPtr pmv2 = new IntPtr(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
                IntPtr prev = SetThreadDpiAwarenessContext(pmv2);
                if (prev != IntPtr.Zero)
                {
                    ok = 1;
                    prevRaw = prev.ToInt64();
                    already = AreDpiAwarenessContextsEqual(prev, pmv2) ? 1 : 0;
                }
                else { err = GetLastError(); }
            }
            catch (EntryPointNotFoundException) { err = 127; }
            return new long[] { ok, already, prevRaw, err };
        }

        public static bool RestoreThreadContext(long raw)
        {
            try { return SetThreadDpiAwarenessContext(new IntPtr(raw)) != IntPtr.Zero; }
            catch (EntryPointNotFoundException) { return false; }
        }

        // Verified check that the calling thread is actually PMv2 now.
        public static bool CurrentThreadIsPmv2()
        {
            try
            {
                return AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), new IntPtr(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
            }
            catch (EntryPointNotFoundException) { return false; }
        }

        public static int DpiForWindow(IntPtr h)
        {
            try { return (int)GetDpiForWindow(h); }
            catch (EntryPointNotFoundException) { return 0; }
        }

        public static int[] ComputeOuter(int cw, int ch, uint style, uint exstyle)
        {
            RECT r = new RECT();
            r.Left = 0; r.Top = 0; r.Right = cw; r.Bottom = ch;
            AdjustWindowRectEx(ref r, style, false, exstyle);
            return new int[] { r.Left, r.Top, r.Right, r.Bottom };
        }

        public static bool MoveWindowTo(IntPtr h, int x, int y, int cx, int cy)
        {
            return SetWindowPos(h, IntPtr.Zero, x, y, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        public static IntPtr FindMainWindow(uint pid)
        {
            IntPtr best = IntPtr.Zero;
            long bestArea = -1;
            EnumWindows(delegate(IntPtr h, IntPtr l)
            {
                uint wp;
                GetWindowThreadProcessId(h, out wp);
                if (wp != pid) return true;
                if (!IsWindowVisible(h)) return true;
                if (GetWindow(h, GW_OWNER) != IntPtr.Zero) return true;
                RECT r;
                if (!GetWindowRect(h, out r)) return true;
                long area = (long)(r.Right - r.Left) * (long)(r.Bottom - r.Top);
                if (area > bestArea) { bestArea = area; best = h; }
                return true;
            }, IntPtr.Zero);
            return best;
        }

        public static uint SendMouse(uint flags, int ax, int ay)
        {
            INPUT inp = new INPUT();
            inp.type = INPUT_MOUSE;
            inp.u.mi.dx = ax;
            inp.u.mi.dy = ay;
            inp.u.mi.mouseData = 0;
            inp.u.mi.dwFlags = flags;
            inp.u.mi.time = 0;
            inp.u.mi.dwExtraInfo = IntPtr.Zero;
            INPUT[] arr = new INPUT[] { inp };
            return SendInput(1, arr, Marshal.SizeOf(typeof(INPUT)));
        }

        // One keyboard event through the SAME SendInput union used by the mouse
        // path. `up` sets KEYEVENTF_KEYUP. Returns the SendInput count (1 ok).
        public static uint SendKey(ushort vk, bool up)
        {
            INPUT inp = new INPUT();
            inp.type = INPUT_KEYBOARD;
            inp.u.ki.wVk = vk;
            inp.u.ki.wScan = 0;
            inp.u.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            inp.u.ki.time = 0;
            inp.u.ki.dwExtraInfo = IntPtr.Zero;
            INPUT[] arr = new INPUT[] { inp };
            return SendInput(1, arr, Marshal.SizeOf(typeof(INPUT)));
        }

        // Async physical/synthetic key state. Returns the VK codes of every
        // modifier currently held (Shift/Ctrl/Alt/Win and their left/right
        // variants). An empty array means none are held. GetAsyncKeyState's high
        // bit (0x8000) is the down state.
        public static int[] HeldModifiers()
        {
            int[] vks = new int[] {
                VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN,
                VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU
            };
            System.Collections.Generic.List<int> held = new System.Collections.Generic.List<int>();
            for (int i = 0; i < vks.Length; i++)
            {
                if ((GetAsyncKeyState(vks[i]) & 0x8000) != 0) held.Add(vks[i]);
            }
            return held.ToArray();
        }

        // --- preparatory focus: verified caption hit-test (no input) ---

        // Bounded WM_NCHITTEST. ok=1 when SendMessageTimeout completed inside the
        // bound (the HT value is the return); ok=0 on timeout or a hung target.
        // Never blocks longer than timeoutMs.
        public static int HitTestBounded(IntPtr h, int x, int y, int timeoutMs, out int ok)
        {
            IntPtr lParam = (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF));
            IntPtr result = IntPtr.Zero;
            IntPtr rv = SendMessageTimeout(h, WM_NCHITTEST, IntPtr.Zero, lParam,
                SMTO_ABORTIFHUNG, (uint)timeoutMs, out result);
            ok = (rv != IntPtr.Zero) ? 1 : 0;
            return unchecked((int)result.ToInt64());
        }

        public static IntPtr RootFromPoint(int x, int y)
        {
            POINT p; p.X = x; p.Y = y;
            IntPtr w = WindowFromPoint(p);
            if (w == IntPtr.Zero) return IntPtr.Zero;
            return GetAncestor(w, GA_ROOT);
        }

        // Bring OUR window to the top of the z-order WITHOUT activating it; only
        // NOSIZE|NOMOVE|NOACTIVATE (no SWP_NOZORDER, so HWND_TOP applies).
        public static bool BringToTopNoActivate(IntPtr h)
        {
            return SetWindowPos(h, IntPtr.Zero, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        }

        // --- temporary topmost raise (opt-in CSD fallback only) ---
        // Read the persisted WS_EX_TOPMOST bit of an owned window. This is the
        // state that MUST be saved before any temporary raise and verified again
        // after the restore in the function's finally block.
        public static bool IsTopmost(IntPtr h)
        {
            return (GetWindowLong(h, GWL_EXSTYLE) & WS_EX_TOPMOST) == WS_EX_TOPMOST;
        }

        // Raise ONLY the already-verified owned HWND into the topmost band without
        // moving, resizing or activating it. Never touches any other window.
        public static bool SetTopmostNoActivate(IntPtr h)
        {
            return SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        }

        // Restore the saved WS_EX_TOPMOST state (topmost or not) without moving,
        // resizing or activating. Always call this before verifying IsTopmost(h).
        public static bool RestoreTopmostNoActivate(IntPtr h, bool wasTopmost)
        {
            return SetWindowPos(h, wasTopmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        }

        // Search the top band of the owned window for a point that is
        // simultaneously (a) inside the owned root (WindowFromPoint ->
        // GetAncestor(GA_ROOT) is h and its PID is expectedPid) and (b) reported
        // by that window's own WM_NCHITTEST as HTCAPTION. Returns
        // [x, y, ht, attempts, found]. x is scanned from the horizontal centre
        // outward so caption buttons are the last thing tried. A client point can
        // never pass because only HTCAPTION is accepted.
        public static int[] FindCaptionPoint(IntPtr h, uint expectedPid, int timeoutMs, int bandPx, int xStep, int yStep)
        {
            int[] wb = WindowBounds(h);
            if (wb == null) return new int[] { 0, 0, -1, 0, 0 };
            int left = wb[0], top = wb[1], right = wb[2], bottom = wb[3];
            int height = bottom - top;
            int band = bandPx;
            if (band > height) band = height;
            if (band < 1) band = 1;
            int l = left + 8, r = right - 8;
            if (r < l) { l = left; r = right; }
            int c = (l + r) / 2;
            int attempts = 0;
            for (int y = top + 1; y < top + band; y += yStep)
            {
                for (int d = 0; d <= (r - l); d += xStep)
                {
                    int xa = (d == 0) ? c : c - d;
                    int xb = (d == 0) ? -1 : c + d;
                    int[] xs = (d == 0) ? new int[] { xa } : new int[] { xa, xb };
                    for (int xi = 0; xi < xs.Length; xi++)
                    {
                        int x = xs[xi];
                        if (x < l || x > r) continue;
                        attempts++;
                        POINT p; p.X = x; p.Y = y;
                        IntPtr w = WindowFromPoint(p);
                        if (w == IntPtr.Zero) continue;
                        IntPtr root = GetAncestor(w, GA_ROOT);
                        if (root != h) continue;
                        uint rp; GetWindowThreadProcessId(root, out rp);
                        if (rp != expectedPid) continue;
                        int ok;
                        int ht = HitTestBounded(w, x, y, timeoutMs, out ok);
                        if (ok != 1) continue;
                        if (ht != HTCAPTION) continue;
                        return new int[] { x, y, ht, attempts, 1 };
                    }
                }
            }
            return new int[] { 0, 0, -1, attempts, 0 };
        }

        // --- session / token helpers ---

        public static int ProcessSession(uint pid)
        {
            uint s;
            if (!ProcessIdToSessionId(pid, out s)) return -1;
            return (int)s;
        }

        public static uint ActiveConsoleSession() { return WTSGetActiveConsoleSessionId(); }

        // WTSQuerySessionInformationW reports the byte count it actually wrote in
        // pBytesReturned. Microsoft documents WTSIsRemoteSession (class 29) only as
        // TRUE/FALSE; it does NOT document a byte count. Empirically, Windows
        // 10.0.26200 returns a 1-byte boolean for a local session. Accept the
        // observed 1-byte form and the conventional 4-byte BOOL; any other length,
        // a failed call, or a null buffer is -1 (UNKNOWN). The gate fails closed on
        // UNKNOWN and never treats an API failure as local or as remote.
        public static int DecodeRemoteSession(bool ok, IntPtr buf, uint len)
        {
            if (!ok || buf == IntPtr.Zero) return -1;
            if (len == 4) return Marshal.ReadInt32(buf) != 0 ? 1 : 0;
            if (len == 1) return Marshal.ReadByte(buf) != 0 ? 1 : 0;
            return -1;
        }

        public static int[] WtsSession(uint session)
        {
            int protocol = -1;
            int remote = -1;
            IntPtr buf = IntPtr.Zero;
            uint len = 0;
            bool ok1 = WTSQuerySessionInformationW(IntPtr.Zero, session, WTSClientProtocolType, out buf, out len);
            if (ok1 && len >= 2) protocol = (int)Marshal.ReadInt16(buf);
            if (buf != IntPtr.Zero) WTSFreeMemory(buf);

            buf = IntPtr.Zero; len = 0;
            bool ok2 = WTSQuerySessionInformationW(IntPtr.Zero, session, WTSIsRemoteSession, out buf, out len);
            remote = DecodeRemoteSession(ok2, buf, len);
            if (buf != IntPtr.Zero) WTSFreeMemory(buf);
            return new int[] { (ok1 && ok2) ? 1 : 0, protocol, remote };
        }

        public static IntPtr OpenToken(uint pid)
        {
            IntPtr hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid);
            if (hp == IntPtr.Zero) return IntPtr.Zero;
            IntPtr tok = IntPtr.Zero;
            bool ok = OpenProcessToken(hp, TOKEN_QUERY, out tok);
            CloseHandle(hp);
            return ok ? tok : IntPtr.Zero;
        }

        public static IntPtr OpenCurrentToken()
        {
            IntPtr tok = IntPtr.Zero;
            return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, out tok) ? tok : IntPtr.Zero;
        }

        public static int TokenElevationValue(IntPtr token)
        {
            IntPtr buf = Marshal.AllocHGlobal(4);
            try
            {
                uint ret = 0;
                if (!GetTokenInformation(token, TokenElevation, buf, 4, out ret)) return -1;
                return Marshal.ReadInt32(buf);
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        public static string TokenUserSid(IntPtr token)
        {
            uint len = 0;
            GetTokenInformation(token, TokenUser, IntPtr.Zero, 0, out len);
            if (len == 0) return null;
            IntPtr buf = Marshal.AllocHGlobal((int)len);
            try
            {
                uint ret = 0;
                if (!GetTokenInformation(token, TokenUser, buf, len, out ret)) return null;
                IntPtr sid = Marshal.ReadIntPtr(buf);
                return new System.Security.Principal.SecurityIdentifier(sid).Value;
            }
            finally { Marshal.FreeHGlobal(buf); }
        }

        public static void Close(IntPtr h) { if (h != IntPtr.Zero) CloseHandle(h); }
    }
}
'@

if ($null -eq ('VAPan.Win32' -as [type])) {
    Add-Type -TypeDefinition $VAPanInteropSource -Language CSharp
}

# ---------------------------------------------------------------------------
# State + logging
# ---------------------------------------------------------------------------
$script:exitCode   = 7
$script:status     = 'init'
$script:events     = New-Object System.Collections.ArrayList
$script:eventIndex = 0
$script:midDown    = $false
$script:leftDown   = $false
$script:hwnd       = [IntPtr]::Zero
$script:process    = $null
$script:qpcFreq    = 0
$script:logPath    = $null
$script:summary    = [ordered]@{}
$script:lastClientX = 0
$script:lastClientY = 0
$script:startPng   = $false
$script:endPng     = $false
$script:ctrlDown   = $false
$script:sDown      = $false
$script:measuredGeometryGuard  = $null
$script:measuredGeometryGuards = $null

function Write-PanLog([string]$Message) {
    $line = ('{0:o} {1}' -f [DateTime]::UtcNow, $Message)
    if ($script:logPath) { Add-Content -LiteralPath $script:logPath -Value $line -Encoding UTF8 }
    Write-Verbose $line
}

function Throw-PanFailure([int]$Code, [string]$Message) {
    if ($script:exitCode -eq 7) { $script:exitCode = $Code }
    throw $Message
}

function Add-PanEvent {
    param(
        [int]$Loop, [string]$Phase,
        [int]$DesiredX, [int]$DesiredY,
        [int]$ActualX, [int]$ActualY,
        [int]$ActualScreenX, [int]$ActualScreenY,
        [long]$Return,
        [bool]$ForegroundOk,
        [long]$Qpc, [string]$Utc
    )
    $idx = $script:eventIndex
    $script:eventIndex = $idx + 1
    [void]$script:events.Add([ordered]@{
        index            = $idx
        loop             = $Loop
        phase            = $Phase
        desired_client   = [ordered]@{ x = $DesiredX; y = $DesiredY }
        actual_client    = [ordered]@{ x = $ActualX; y = $ActualY }
        actual_screen    = [ordered]@{ x = $ActualScreenX; y = $ActualScreenY }
        sendinput_return = $Return
        foreground_ok    = $ForegroundOk
        qpc              = $Qpc
        utc              = $Utc
    })
    return $idx
}

function Get-GlibClockContract {
    <#
      Verify the actual loaded GLib monotonic clock basis against QPC. The
      function is intentionally self-contained (hash + Windows loader + 10
      samples) so the session-0 integration check can extract and exercise the
      real implementation via the PowerShell AST. It never invents an offset.
    #>
    param(
        [string]$GlibDllPath,
        [string]$ExpectedSha256,
        [long]$QpcFrequency,
        [int]$Samples = 10,
        [double]$ToleranceUs = 2000.0
    )
    $result = [ordered]@{
        schema               = 'vacards-app-glib-clock/1'
        dll_path             = $GlibDllPath
        expected_sha256      = $ExpectedSha256
        actual_sha256        = $null
        sha256_matches       = $false
        qpc_frequency        = $QpcFrequency
        sample_count         = 0
        tolerance_us         = $ToleranceUs
        samples              = @()
        all_in_bracket       = $false
        max_abs_residual_us  = $null
        status               = 'failed'
        error                = $null
    }
    $module = [IntPtr]::Zero
    try {
        if ([string]::IsNullOrWhiteSpace($GlibDllPath) -or -not (Test-Path -LiteralPath $GlibDllPath -PathType Leaf)) {
            $result.error = 'GlibDllPath is missing or not a file'
            return $result
        }
        $full = (Resolve-Path -LiteralPath $GlibDllPath).Path
        $result.dll_path = $full
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $stream = [IO.File]::Open($full, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            try { $result.actual_sha256 = ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
            finally { $stream.Dispose() }
        } finally { $sha.Dispose() }
        $expected = if ([string]::IsNullOrEmpty($ExpectedSha256)) { '' } else { $ExpectedSha256.ToLowerInvariant() }
        $result.sha256_matches = ($result.actual_sha256 -eq $expected)
        if (-not $result.sha256_matches) { $result.error = 'GLib DLL sha256 does not match ExpectedGlibSha256'; return $result }
        if ($QpcFrequency -le 0) { $result.error = 'invalid QPC frequency'; return $result }

        $module = [VAPan.Win32]::LoadLibraryAbsolute($full)
        if ($module -eq [IntPtr]::Zero) {
            $result.error = ('LoadLibraryExW failed for {0} (Win32 {1})' -f $full, [VAPan.Win32]::GetLastError())
            return $result
        }
        $fn = [VAPan.Win32]::GetExport($module, 'g_get_monotonic_time')
        if ($fn -eq [IntPtr]::Zero) { $result.error = 'g_get_monotonic_time export not found'; return $result }

        $list = New-Object System.Collections.ArrayList
        $allInBracket = $true
        $maxResidual = 0.0
        for ($i = 0; $i -lt $Samples; $i++) {
            $qb = [VAPan.Win32]::Qpc()
            $gu = [VAPan.Win32]::CallGlibMonotonic($fn)
            $qa = [VAPan.Win32]::Qpc()
            $lo = (([double]$qb) * 1000000.0 / [double]$QpcFrequency) - $ToleranceUs
            $hi = (([double]$qa) * 1000000.0 / [double]$QpcFrequency) + $ToleranceUs
            $centre = ((([double]$qb + [double]$qa) / 2.0) * 1000000.0 / [double]$QpcFrequency)
            $resid = [double]$gu - $centre
            $inBracket = ($gu -ge $lo -and $gu -le $hi)
            if (-not $inBracket) { $allInBracket = $false }
            if ([Math]::Abs($resid) -gt $maxResidual) { $maxResidual = [Math]::Abs($resid) }
            [void]$list.Add([ordered]@{
                index                  = $i
                qpc_before             = [long]$qb
                qpc_after              = [long]$qa
                glib_us                = [long]$gu
                bracket_low_us         = [Math]::Round($lo, 3)
                bracket_high_us        = [Math]::Round($hi, 3)
                residual_from_centre_us = [Math]::Round($resid, 3)
                in_bracket             = [bool]$inBracket
            })
        }
        $result.sample_count = $list.Count
        $result.samples = @($list)
        $result.all_in_bracket = $allInBracket
        $result.max_abs_residual_us = [Math]::Round($maxResidual, 3)
        if ($allInBracket) {
            $result.status = 'ok'
        } else {
            $result.error = 'g_get_monotonic_time left the QPC interval bracket in at least one sample'
        }
    } catch {
        $result.status = 'failed'
        if (-not $result.error) { $result.error = "$_" }
    } finally {
        if ($module -ne [IntPtr]::Zero) { try { [void][VAPan.Win32]::FreeLibrary($module) } catch {} }
    }
    return $result
}

function Save-PanSummary {
    $path = Join-Path -Path $OutputDirectory -ChildPath 'input-summary.json'
    $script:summary['status']    = $script:status
    $script:summary['exit_code'] = $script:exitCode
    $script:summary['events']    = $script:events
    # Runtime leg-boundary/return geometry-guard evidence (maximized mode only).
    $script:summary['measured_geometry_guards'] = if ($null -ne $script:measuredGeometryGuards) { @($script:measuredGeometryGuards) } else { @() }

    # Explicit phase boundary schema. Every key is present; a key never reached
    # is null and `complete` is false. No boundary is fabricated from helper
    # entry time or wall clock, so a failed run persists a partial/null shape.
    $qpcFrequencyPb = if ($script:summary.Contains('qpc_frequency')) { $script:summary['qpc_frequency'] } else { $null }
    $pb = [ordered]@{
        schema            = 'vacards-app-pan-phase-boundaries/1'
        complete          = $false
        status            = 'absent'
        qpc_frequency     = $qpcFrequencyPb
        input_start_qpc   = $null
        input_end_qpc     = $null
        input_start_utc   = $null
        input_end_utc     = $null
        capture_start_qpc = $null
        capture_start_utc = $null
        capture_end_qpc   = $null
        capture_end_utc   = $null
    }
    foreach ($key in @('input_start_qpc', 'input_end_qpc', 'input_start_utc', 'input_end_utc')) {
        if ($script:summary.Contains($key)) { $pb[$key] = $script:summary[$key] }
    }
    foreach ($capKey in @('capture_start', 'capture_end')) {
        if ($script:summary.Contains($capKey)) {
            $cap = $script:summary[$capKey]
            $pb[($capKey + '_utc')] = $cap.utc
            $pb[($capKey + '_qpc')] = $cap.qpc
        }
    }
    if ($null -ne $pb.input_start_qpc -and $null -ne $pb.input_end_qpc -and
        $null -ne $pb.qpc_frequency -and [long]$pb.qpc_frequency -gt 0) {
        $pb.complete = $true
        $pb.status = 'complete'
    } elseif ($null -ne $pb.input_start_qpc -or $null -ne $pb.capture_start_qpc) {
        $pb.status = 'partial'
    }
    $script:summary['phase_boundaries'] = $pb

    $json = $script:summary | ConvertTo-Json -Depth 8
    $enc = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($path, $json, $enc)
}

function Invoke-PreparatoryCaptionFocus {
    <#
      ONE user-like click on a VERIFIED caption point of ONLY the owned HWND.

      Runs only after the existing bounded SetForegroundWindow attempt was
      denied. It re-verifies the owned window/session/identity/geometry, raises
      only our window without activating it, then selects ONE point: a point
      whose own WM_NCHITTEST is HTCAPTION, or - only when AllowVerifiedCsdCaption
      is set and no HTCAPTION exists - the single fixed client-side-titlebar point
      ClientScreenOrigin + (ClientW/2, 30). Either way WindowFromPoint /
      GetAncestor(GA_ROOT) must be the owned HWND with the target PID before the
      move and again immediately before the press; the CSD point additionally
      requires dpi 96 and its own hit-test to be HTCLIENT only. The move must
      return SendInput==1 and the real cursor must be confirmed at the intended
      point before any press; the ownership/hit-test re-check before the press
      uses that ACTUAL cursor point. It then injects exactly one absolute move +
      left down + left up through the existing SendMouse INPUT marshal helper and
      counts one delivered click ONLY when move/down/up all returned 1; the
      pre-click cursor is restored with SetCursorPos (a non-input API, so no
      extra injected event), including on any abort before the press. The click
      is NOT part of the measured gesture and is recorded in its own
      `preparatory_focus` summary object. Returns the result object; the caller
      fails closed unless foreground_after_wait is true. When the opt-in CSD point
      is not owned after the normal HWND_TOP raise, this function temporarily sets
      HWND_TOPMOST (NOMOVE|NOSIZE|NOACTIVATE) on the verified owned HWND, rechecks
      root/PID/HTCLIENT, and ALWAYS restores/verifies the saved WS_EX_TOPMOST in a
      finally block. A topmost run is successful only when the click was delivered,
      the original topmost state is verified restored, the owned HWND is still
      foreground, and the point is still owned after the restore; restoration
      failure never returns success.
    #>
    param(
        [IntPtr]$WindowHandle,
        [int]$TargetPid,
        [int]$ClientW,
        [int]$ClientH,
        [switch]$AllowVerifiedCsdCaption,
        [bool]$MaximizedMode = $false,
        $Calibration = $null,
        [scriptblock]$GeometryCheck = $null
    )
    $res = [ordered]@{
        schema                    = 'vacards-app-pan-preparatory-focus/1'
        attempted                 = $true
        precondition_ok           = $false
        precondition_problems     = @()
        reason                    = $null
        method                    = $null
        csd_optin                 = [bool]$AllowVerifiedCsdCaption
        csd_client_point          = $null
        csd_dpi                   = $null
        topmost_raise_attempted   = $false
        topmost_used              = $false
        topmost_orig_exstyle_topmost = $null
        topmost_set_call_ok       = $null
        root_check_after_raise    = $null
        root_pid_after_raise      = $null
        hittest_after_raise       = $null
        hittest_after_raise_ok    = $null
        point_owned_after_raise   = $null
        topmost_restore_call_ok   = $null
        topmost_after_restore     = $null
        topmost_restore_verified  = $null
        foreground_after_restore  = $null
        point_owned_after_restore = $null
        point_root_pid_after_restore = $null
        attempts                  = 0
        point_screen              = $null
        hittest                   = $null
        hittest_ok                = $null
        root_check_before_move    = $null
        root_pid_before_move      = $null
        root_is_target            = $null
        root_pid                  = $null
        root_check_before_press   = $null
        root_pid_before_press     = $null
        hittest_before_press      = $null
        hittest_before_press_ok   = $null
        brought_to_top            = $null
        cursor_before_screen      = $null
        cursor_after_click_screen = $null
        cursor_restored_screen    = $null
        move_sendinput_return     = $null
        cursor_after_move_screen     = $null
        cursor_at_point_before_press = $null
        move_confirmed               = $false
        click_delivered              = $false
        left_down_return          = $null
        left_up_return            = $null
        left_down_done            = $false
        left_up_done              = $false
        click_count               = 0
        foreground_before         = $null
        foreground_after          = $null
        foreground_after_wait     = $false
        started_qpc               = [VAPan.Win32]::Qpc()
        started_utc               = [DateTime]::UtcNow.ToString('o')
        finished_qpc              = $null
        finished_utc              = $null
    }
    $finish = {
        $res['finished_qpc'] = [VAPan.Win32]::Qpc()
        $res['finished_utc'] = [DateTime]::UtcNow.ToString('o')
        return $res
    }
    # Temporary-topmost bookkeeping. $topmostAttempted survives to the finally
    # block (early returns and exceptions included) so the saved state is always
    # restored and verified.
    $topmostAttempted = $false
    $origTopmost = $false
    $px = $null
    $py = $null
    try {
        $problems = New-Object System.Collections.ArrayList

        # --- PID / visibility / normal state / exact client / monitor bounds ---
        if ([VAPan.Win32]::WindowPid($WindowHandle) -ne $TargetPid) { [void]$problems.Add('pid-mismatch') }
        if (-not [VAPan.Win32]::Visible($WindowHandle)) { [void]$problems.Add('not-visible') }
        if ([VAPan.Win32]::Minimized($WindowHandle)) { [void]$problems.Add('minimized') }
        if ($MaximizedMode) {
            # Mode-aware strict preconditions: the calibrated maximized proof
            # replaces the legacy normal-state / exact-1280x800 / containment
            # checks. It is not a bypass.
            $geo = if ($null -ne $GeometryCheck) { & $GeometryCheck $WindowHandle } else { Test-MaximizedGeometryState -WindowHandle $WindowHandle -Calibration $Calibration }
            $res['maximized_geometry_precondition'] = $geo
            if (-not [bool]$geo['ok']) { [void]$problems.Add('maximized-geometry') }
        } else {
            if ([VAPan.Win32]::Maximized($WindowHandle)) { [void]$problems.Add('zoomed') }
            $cs = [VAPan.Win32]::ClientSize($WindowHandle)
            if ($cs[0] -ne $ClientW -or $cs[1] -ne $ClientH) { [void]$problems.Add('client-not-exact') }
            if (-not [VAPan.Win32]::BoundsInsideMonitor($WindowHandle)) { [void]$problems.Add('bounds-outside-monitor') }
        }

        # --- physical console session + SID ---
        $hSess = ([System.Diagnostics.Process]::GetCurrentProcess()).SessionId
        $tSess = [VAPan.Win32]::ProcessSession([uint32]$TargetPid)
        $active = [VAPan.Win32]::ActiveConsoleSession()
        if ($hSess -ne $tSess) { [void]$problems.Add('helper-target-session-mismatch') }
        if ([uint32]$tSess -ne $active) { [void]$problems.Add('target-not-active-console') }
        $wts = [VAPan.Win32]::WtsSession([uint32]$tSess)
        if ($wts[0] -ne 1 -or $wts[1] -ne 0 -or $wts[2] -ne 0) { [void]$problems.Add('session-not-console') }
        $curTok = [VAPan.Win32]::OpenCurrentToken()
        $tgtTok = [VAPan.Win32]::OpenToken([uint32]$TargetPid)
        try {
            if ($curTok -eq [IntPtr]::Zero -or $tgtTok -eq [IntPtr]::Zero) {
                [void]$problems.Add('token-open-failed')
            } else {
                $curSid = [VAPan.Win32]::TokenUserSid($curTok)
                $tgtSid = [VAPan.Win32]::TokenUserSid($tgtTok)
                if ([string]::IsNullOrEmpty($curSid) -or $curSid -ne $tgtSid) { [void]$problems.Add('sid-mismatch') }
            }
        } finally {
            [VAPan.Win32]::Close($curTok); [VAPan.Win32]::Close($tgtTok)
        }

        $res['precondition_problems'] = @($problems)
        $res['foreground_before'] = [VAPan.Win32]::ForegroundIs($WindowHandle)
        if ($problems.Count -gt 0) {
            $res['reason'] = ('preconditions failed: ' + ($problems -join ','))
            return (& $finish)
        }
        $res['precondition_ok'] = $true

        # --- raise OUR window without activating it (only window op here) ---
        $res['brought_to_top'] = [VAPan.Win32]::BringToTopNoActivate($WindowHandle)

        # --- verified point: native HTCAPTION, else opt-in fixed CSD titlebar ---
        $requiredHt = 0
        $find = [VAPan.Win32]::FindCaptionPoint($WindowHandle, [uint32]$TargetPid, 250, 100, 16, 4)
        $res['attempts'] = $find[3]
        if ($find[4] -eq 1) {
            $res['method'] = 'native_htcaption'
            $requiredHt = 2
            $px = $find[0]; $py = $find[1]
        } elseif ($AllowVerifiedCsdCaption) {
            # Opt-in beta5 fixture: GTK client-side decoration exposes no Windows
            # caption. Use the ONE root-verified fixed client point (ClientW/2, 30)
            # in the client-side titlebar label area. Never an arbitrary client
            # point and never a scan of other client points.
            $dpi = [VAPan.Win32]::DpiForWindow($WindowHandle)
            $res['csd_dpi'] = $dpi
            $csdDpiExpected = if ($MaximizedMode -and $null -ne $Calibration) { [int](Get-CalValue $Calibration 'dpi') } else { 96 }
            if ($dpi -ne $csdDpiExpected) {
                $res['reason'] = ('CSD opt-in requires the expected dpi ' + $csdDpiExpected + ' (observed ' + $dpi + '); not clicking')
                return (& $finish)
            }
            $cx = [int][math]::Floor($ClientW / 2)
            $cy = 30
            if ($cx -lt 0 -or $cy -lt 0 -or $cx -ge $ClientW -or $cy -ge $ClientH) {
                $res['reason'] = 'CSD fixed client point is outside the exact client; not clicking'
                return (& $finish)
            }
            $org = [VAPan.Win32]::ClientScreenOrigin($WindowHandle)
            $px = $org[0] + $cx
            $py = $org[1] + $cy
            $res['csd_client_point'] = @($cx, $cy)
            $res['method'] = 'visually_verified_beta5_csd_titlebar'
            # root/owner required BEFORE the move. Opt-in CSD fallback ONLY: when
            # the planned point is not owned after the normal HWND_TOP raise,
            # temporarily raise ONLY this already-verified owned HWND with
            # HWND_TOPMOST (NOMOVE|NOSIZE|NOACTIVATE), then recheck the point.
            $rootPre = [VAPan.Win32]::RootFromPoint($px, $py)
            $res['root_check_before_move'] = ($rootPre -eq $WindowHandle)
            $res['root_pid_before_move'] = [int][VAPan.Win32]::WindowPid($rootPre)
            if (-not $res['root_check_before_move'] -or $res['root_pid_before_move'] -ne $TargetPid) {
                $res['topmost_raise_attempted'] = $true
                $topmostAttempted = $true
                $origTopmost = [bool][VAPan.Win32]::IsTopmost($WindowHandle)
                $res['topmost_orig_exstyle_topmost'] = $origTopmost
                $setTop = [bool][VAPan.Win32]::SetTopmostNoActivate($WindowHandle)
                $res['topmost_set_call_ok'] = $setTop
                $res['topmost_used'] = $setTop
                if (-not $setTop) {
                    $res['reason'] = 'temporary topmost SetWindowPos call failed (not clicking)'
                    return (& $finish)
                }
                $rootRaise = [VAPan.Win32]::RootFromPoint($px, $py)
                $res['root_check_after_raise'] = ($rootRaise -eq $WindowHandle)
                $res['root_pid_after_raise'] = [int][VAPan.Win32]::WindowPid($rootRaise)
                $htRaiseOk = 0
                $htRaise = [VAPan.Win32]::HitTestBounded($WindowHandle, $px, $py, 250, [ref]$htRaiseOk)
                $res['hittest_after_raise'] = $htRaise
                $res['hittest_after_raise_ok'] = $htRaiseOk
                $res['point_owned_after_raise'] = ($res['root_check_after_raise'] -and $res['root_pid_after_raise'] -eq $TargetPid)
                if (-not $res['point_owned_after_raise'] -or $htRaiseOk -ne 1 -or $htRaise -ne 1) {
                    $res['reason'] = 'CSD point still not owned/HTCLIENT after temporary topmost (not clicking)'
                    return (& $finish)
                }
            }
            # The whole CSD window is client area: accept only HTCLIENT here.
            $requiredHt = 1
        } else {
            $res['reason'] = 'no verified HTCAPTION point and CSD opt-in is disabled (not clicking)'
            return (& $finish)
        }
        $res['point_screen'] = @($px, $py)

        # --- final verification immediately before the move ---
        $rootNow = [VAPan.Win32]::RootFromPoint($px, $py)
        $res['root_is_target'] = ($rootNow -eq $WindowHandle)
        $res['root_pid'] = [int][VAPan.Win32]::WindowPid($rootNow)
        $htOk = 0
        $htNow = [VAPan.Win32]::HitTestBounded($WindowHandle, $px, $py, 250, [ref]$htOk)
        $res['hittest'] = $htNow
        $res['hittest_ok'] = $htOk
        if (-not $res['root_is_target'] -or $res['root_pid'] -ne $TargetPid -or $htOk -ne 1 -or $htNow -ne $requiredHt) {
            $res['reason'] = ('verified point is not root-owned with expected hittest ' + $requiredHt + ' before move (not clicking)')
            return (& $finish)
        }

        # --- ONE user-like left click at the verified point ---
        $cur = [VAPan.Win32]::CursorClient($WindowHandle)
        $res['cursor_before_screen'] = @($cur[2], $cur[3])
        $abs = [VAPan.Win32]::ScreenToAbsoluteForMode($px, $py, [bool]$MaximizedMode)
        if ($null -eq $abs) {
            $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3])
            $res['reason'] = 'verified point cannot be represented in the virtual desktop (not clicking)'
            return (& $finish)
        }
        $res['move_sendinput_return'] = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOVE_FLAGS, $abs[0], $abs[1])

        # The MOVE must have been injected (SendInput==1) AND the real cursor must
        # actually be at the intended screen point before anything is pressed.
        # Abort without pressing and restore the pre-click cursor on any failure.
        $curAt = [VAPan.Win32]::CursorClient($WindowHandle)
        $res['cursor_after_move_screen'] = @($curAt[2], $curAt[3])
        $res['cursor_at_point_before_press'] = (($curAt[2] -eq $px) -and ($curAt[3] -eq $py))
        $res['move_confirmed'] = (($res['move_sendinput_return'] -eq 1) -and $res['cursor_at_point_before_press'])
        if (-not $res['move_confirmed']) {
            $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3])
            $res['reason'] = 'SendInput move not confirmed at the intended point (not pressing)'
            return (& $finish)
        }

        # --- ownership/hittest re-verified at the ACTUAL cursor point before the press ---
        $actualX = $curAt[2]; $actualY = $curAt[3]
        $rootPress = [VAPan.Win32]::RootFromPoint($actualX, $actualY)
        $res['root_check_before_press'] = ($rootPress -eq $WindowHandle)
        $res['root_pid_before_press'] = [int][VAPan.Win32]::WindowPid($rootPress)
        $htPressOk = 0
        $htPress = [VAPan.Win32]::HitTestBounded($WindowHandle, $actualX, $actualY, 250, [ref]$htPressOk)
        $res['hittest_before_press'] = $htPress
        $res['hittest_before_press_ok'] = $htPressOk
        if (-not $res['root_check_before_press'] -or $res['root_pid_before_press'] -ne $TargetPid -or
            $htPressOk -ne 1 -or $htPress -ne $requiredHt) {
            $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3])
            $res['reason'] = 'actual cursor point not owned with expected hittest immediately before press (not pressing)'
            return (& $finish)
        }

        $res['left_down_return'] = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTDOWN, 0, 0)
        if ($res['left_down_return'] -eq 1) { $script:leftDown = $true; $res['left_down_done'] = $true }
        $res['left_up_return'] = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP, 0, 0)
        if ($res['left_up_return'] -eq 1) { $script:leftDown = $false; $res['left_up_done'] = $true }
        # A click is counted ONLY when the move, the left down AND the left up were
        # all injected (SendInput==1) and the cursor was confirmed at the point. A
        # failed send is never reported as one delivered click.
        $res['click_delivered'] = ($res['move_confirmed'] -and ($res['left_down_return'] -eq 1) -and ($res['left_up_return'] -eq 1))
        $res['click_count'] = if ($res['click_delivered']) { 1 } else { 0 }
        if (-not $res['click_delivered']) {
            $res['reason'] = 'SendInput did not deliver all of move/down/up; no click counted'
        }

        # restore the pre-click cursor (non-input API; gesture start unchanged)
        $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3])
        $curAfter = [VAPan.Win32]::CursorClient($WindowHandle)
        $res['cursor_after_click_screen'] = @($curAfter[2], $curAfter[3])
        $res['cursor_restored_screen'] = @($cur[2], $cur[3])

        # --- bounded re-check that the click actually foregrounded the target ---
        $fgDeadline = [DateTime]::UtcNow.AddSeconds(3)
        while ([DateTime]::UtcNow -lt $fgDeadline) {
            if ([VAPan.Win32]::ForegroundIs($WindowHandle)) { break }
            Start-Sleep -Milliseconds 50
        }
        $res['foreground_after'] = [VAPan.Win32]::ForegroundIs($WindowHandle)
        # Success requires the click to have been actually delivered. A failed
        # SendInput must not be reported as success merely because the owned HWND
        # was already foreground.
        $res['foreground_after_wait'] = ($res['click_delivered'] -and $res['foreground_after'])
        if (-not $res['click_delivered']) {
            $res['reason'] = 'preparatory click not delivered (SendInput/cursor confirmation failed)'
        } elseif (-not $res['foreground_after_wait']) {
            $res['reason'] = 'owned HWND still not foreground after the preparatory click'
        }
        return (& $finish)
    } catch {
        $res['reason'] = ('preparatory-focus exception: ' + $_)
        return (& $finish)
    } finally {
        # ALWAYS restore the saved WS_EX_TOPMOST state, including on every early
        # return and on exceptions, then verify it. The temporary raise must never
        # change the window's persistent topmost state.
        try {
            if ($topmostAttempted) {
                $restoreCallOk = [bool][VAPan.Win32]::RestoreTopmostNoActivate($WindowHandle, $origTopmost)
                $res['topmost_restore_call_ok'] = $restoreCallOk
                $res['topmost_after_restore'] = [bool][VAPan.Win32]::IsTopmost($WindowHandle)
                $res['topmost_restore_verified'] = ($res['topmost_after_restore'] -eq $origTopmost)
                # Post-restore re-verification: the owned HWND must still be
                # foreground and the planned point must still be owned by it.
                $res['foreground_after_restore'] = [bool][VAPan.Win32]::ForegroundIs($WindowHandle)
                $rootAfterRestore = [IntPtr]::Zero
                if ($null -ne $px -and $null -ne $py) {
                    $rootAfterRestore = [VAPan.Win32]::RootFromPoint([int]$px, [int]$py)
                }
                $res['point_root_pid_after_restore'] = [int][VAPan.Win32]::WindowPid($rootAfterRestore)
                $res['point_owned_after_restore'] = (($rootAfterRestore -eq $WindowHandle) -and ($res['point_root_pid_after_restore'] -eq $TargetPid))
                # Success after a temporary topmost is allowed ONLY when the click
                # was delivered, the original topmost state is verifiably restored,
                # the owned HWND is still foreground, and the point is still owned
                # after the restore. Restoration failure never passes, even when
                # foreground is true.
                $res['foreground_after_wait'] = ([bool]$res['click_delivered'] -and [bool]$res['foreground_after'] -and [bool]$res['topmost_restore_verified'] -and [bool]$res['foreground_after_restore'] -and [bool]$res['point_owned_after_restore'])
                if (-not $res['topmost_restore_verified']) {
                    $res['reason'] = ('temporary topmost restoration not verified (orig=' + [string]$origTopmost + ' actual=' + [string]$res['topmost_after_restore'] + '); not a success')
                } elseif (-not $res['foreground_after_restore']) {
                    $res['reason'] = 'owned HWND not foreground after topmost restore; not a success'
                } elseif (-not $res['point_owned_after_restore']) {
                    $res['reason'] = 'planned point not owned after topmost restore; not a success'
                }
            }
        } catch {
            $res['topmost_restore_verified'] = $false
            $res['foreground_after_wait'] = $false
            $res['reason'] = ('topmost restore/finally exception: ' + $_)
        } finally {
            $res['finished_qpc'] = [VAPan.Win32]::Qpc()
            $res['finished_utc'] = [DateTime]::UtcNow.ToString('o')
        }
    }
}

# ---------------------------------------------------------------------------
# Object-drag support (pure helpers + separately-counted aux emitters)
#
# None of these touch $script:eventIndex or the measured $script:events array.
# The measured QPC window is [input_start_qpc, input_end_qpc]; every select,
# save and return event is recorded in its own object so it can never inflate
# the pan/object-drag measured counts or the CPU scope.
# ---------------------------------------------------------------------------

function Test-DocumentPathIsolation {
    <#
      Pure, side-effect-free validator for -DocumentPath. Accepts only an
      existing file literally named fixture-simple.svg whose resolved location
      is the parent directory of OutputDirectory or below it. Any arbitrary
      path outside that staging tree, any other file name, and any missing file
      are rejected. Returns { schema, ok, reason, document_path, output_parent }.
    #>
    param(
        [string]$DocPath,
        [string]$OutputDir
    )
    $res = [ordered]@{
        schema        = 'vacards-app-pan-document-path/1'
        ok            = $false
        reason        = $null
        document_path = $null
        output_parent = $null
        fixture_name  = 'fixture-simple.svg'
    }
    if ([string]::IsNullOrWhiteSpace($DocPath)) { $res['reason'] = 'DocumentPath is empty'; return $res }
    if ([string]::IsNullOrWhiteSpace($OutputDir)) { $res['reason'] = 'OutputDirectory is empty'; return $res }
    try {
        $outFull = [System.IO.Path]::GetFullPath($OutputDir)
        $outParent = [System.IO.Path]::GetDirectoryName($outFull)
        if ([string]::IsNullOrEmpty($outParent)) { $res['reason'] = 'OutputDirectory has no parent directory'; return $res }
        if (-not (Test-Path -LiteralPath $DocPath -PathType Leaf)) { $res['reason'] = 'DocumentPath does not exist as a file'; return $res }
        $docFull = (Resolve-Path -LiteralPath $DocPath).Path
        $leaf = [System.IO.Path]::GetFileName($docFull)
        if ($leaf -ne 'fixture-simple.svg') { $res['reason'] = ('DocumentPath must be named fixture-simple.svg (got ' + $leaf + ')'); return $res }
        $docDir = [System.IO.Path]::GetDirectoryName($docFull)
        $trimmed = $outParent.TrimEnd([char[]]@([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar))
        $prefix = $trimmed + [System.IO.Path]::DirectorySeparatorChar
        $inside = $docDir.Equals($outParent, [System.StringComparison]::OrdinalIgnoreCase) -or
                  $docFull.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
        if (-not $inside) { $res['reason'] = 'DocumentPath is outside the parent directory of OutputDirectory'; return $res }
        $res['ok'] = $true
        $res['document_path'] = $docFull
        $res['output_parent'] = $outParent
        return $res
    } catch {
        $res['reason'] = ('DocumentPath resolution failed: ' + $_)
        return $res
    }
}

# ---------------------------------------------------------------------------
# Opt-in maximized-workarea calibration (pure validators + geometry proof).
# Read-only, no input. Every field/type/bound is checked before any use.
# ---------------------------------------------------------------------------

function Get-CalValue($Calibration, [string]$Name) {
    if ($null -eq $Calibration) { return $null }
    if ($Calibration -is [System.Collections.IDictionary]) { return $Calibration[$Name] }
    $prop = $Calibration.PSObject.Properties[$Name]
    if ($null -eq $prop) { return $null }
    return $prop.Value
}

function Test-StrictJsonInt($Value) {
    # JSON integer: a real numeric scalar with no fractional part. Strings,
    # booleans, arrays and objects are rejected so a type-confused calibration
    # can never be silently coerced.
    if ($null -eq $Value) { return $false }
    if ($Value -is [bool] -or $Value -is [string] -or $Value -is [System.Array]) { return $false }
    if ($Value -is [System.Management.Automation.PSCustomObject]) { return $false }
    if ($Value -is [System.Collections.IDictionary]) { return $false }
    try {
        $d = [double]$Value
        if ([double]::IsNaN($d) -or [double]::IsInfinity($d)) { return $false }
        return ($d -eq [Math]::Floor($d))
    } catch { return $false }
}

function Read-CalibrationBounds($Object, [string]$Name, $Errors) {
    $v = Get-CalValue $Object $Name
    if ($v -isnot [System.Array] -or $v.Count -ne 4) {
        [void]$Errors.Add($Name + ' must be an array of exactly 4 integers [left, top, right, bottom]')
        return $null
    }
    foreach ($e in $v) {
        if (-not (Test-StrictJsonInt $e)) { [void]$Errors.Add($Name + ' must contain only integers'); return $null }
    }
    return @([int]$v[0], [int]$v[1], [int]$v[2], [int]$v[3])
}

function Get-ArmPressOffset {
    <#
      Pure derivation of the ONE outside-timing drag-arm press offset shared by
      the forward and return arms. The native selector anchors a live-content
      drag on the first accepted button1 motion, so the press must land far
      enough from the anchor that a mixed-DPI (window 96 / physical monitor 144)
      absolute-move quantization cannot collapse the press motion below the
      native 4-logical-pixel drag threshold. The physical displacement of a
      logical pixel is the independently measured
      `physical_pixels_per_logical_pixel` (never assumed 1 or 1.5), so the
      maximized offset is `ceil(4 * scale) + 2`, giving 8 at the pinned
      1.491279 (6 at scale 1.0). Legacy normal-client mode keeps 4.

      Returns the positive integer offset, or $null when the scale is missing,
      non-numeric, NaN/Infinity or outside (0, 1000]. The caller fails closed on
      $null; no fallback offset is invented.
    #>
    param(
        [bool]$MaximizedMode = $false,
        $Calibration = $null
    )
    if (-not $MaximizedMode) { return 4 }
    $pptRaw = Get-CalValue $Calibration 'physical_pixels_per_logical_pixel'
    if ($null -eq $pptRaw -or $pptRaw -is [bool] -or $pptRaw -is [string] -or
        $pptRaw -is [System.Array] -or $pptRaw -is [System.Management.Automation.PSCustomObject]) {
        return $null
    }
    try {
        $ppt = [double]$pptRaw
        if ([double]::IsNaN($ppt) -or [double]::IsInfinity($ppt) -or $ppt -le 0.0 -or $ppt -gt 1000.0) { return $null }
        $offset = [int]([Math]::Ceiling(4.0 * $ppt) + 2)
        if ($offset -lt 1) { return $null }
        return $offset
    } catch { return $null }
}

function Read-MaximizedCalibration {
    <#
      Strictly validate the pinned vacards-maximized-drag-calibration/1 JSON.
      Hash first, then ConvertFrom-Json (never Invoke-Expression), then every
      field/type/bound and the whole gesture path inside the physical client.
      Returns { ok, reason, actual_sha256, schema, calibration, errors }.
    #>
    param(
        [string]$Path,
        [string]$ExpectedSha256
    )
    $res = [ordered]@{
        ok = $false; reason = $null; actual_sha256 = $null; schema = $null
        calibration = $null; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    $res['errors'] = @($errs)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        [void]$errs.Add('MaximizedCalibrationPath is missing or not a file'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res
    }
    $full = $null
    try { $full = (Resolve-Path -LiteralPath $Path).Path } catch { [void]$errs.Add('MaximizedCalibrationPath could not be resolved'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    try { $res['actual_sha256'] = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() }
    catch { [void]$errs.Add('calibration sha256 could not be computed'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    $expected = ([string]$ExpectedSha256).ToLowerInvariant()
    if ($res['actual_sha256'] -ne $expected) {
        [void]$errs.Add('calibration sha256 does not match ExpectedMaximizedCalibrationSha256')
        $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res
    }
    $text = $null
    try { $text = [IO.File]::ReadAllText($full) } catch { [void]$errs.Add('calibration file is unreadable'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    $obj = $null
    try { $obj = $text | ConvertFrom-Json } catch { [void]$errs.Add('calibration is not valid JSON'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    if ($null -eq $obj) { [void]$errs.Add('calibration JSON is empty'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }

    $schema = [string](Get-CalValue $obj 'schema')
    $res['schema'] = $schema
    if ($schema -ne 'vacards-maximized-drag-calibration/1') {
        [void]$errs.Add('schema is not vacards-maximized-drag-calibration/1 (got ' + $schema + ')')
    }
    $calId = Get-CalValue $obj 'calibration_id'
    if ($calId -isnot [string] -or [string]::IsNullOrWhiteSpace($calId)) { [void]$errs.Add('calibration_id must be a non-empty string') }
    $dev = Get-CalValue $obj 'monitor_device'
    if ($dev -isnot [string] -or [string]::IsNullOrWhiteSpace($dev)) { [void]$errs.Add('monitor_device must be a non-empty string') }

    $mon = Read-CalibrationBounds $obj 'monitor_bounds' $errs
    $work = Read-CalibrationBounds $obj 'work_bounds' $errs
    if ($null -ne $mon) {
        if ($mon[0] -ge $mon[2] -or $mon[1] -ge $mon[3]) { [void]$errs.Add('monitor_bounds must satisfy left<right and top<bottom') }
    }
    if ($null -ne $work) {
        if ($work[0] -ge $work[2] -or $work[1] -ge $work[3]) { [void]$errs.Add('work_bounds must satisfy left<right and top<bottom') }
        if ($null -ne $mon) {
            if ($work[0] -lt $mon[0] -or $work[1] -lt $mon[1] -or $work[2] -gt $mon[2] -or $work[3] -gt $mon[3]) {
                [void]$errs.Add('work_bounds must lie inside monitor_bounds')
            }
        }
    }

    $dpiRaw = Get-CalValue $obj 'dpi'
    if (-not (Test-StrictJsonInt $dpiRaw) -or [int]$dpiRaw -le 0 -or [int]$dpiRaw -gt 960) {
        [void]$errs.Add('dpi must be an integer in 1..960')
    }
    $cwRaw = Get-CalValue $obj 'client_width'
    $chRaw = Get-CalValue $obj 'client_height'
    if (-not (Test-StrictJsonInt $cwRaw) -or [int]$cwRaw -le 0) { [void]$errs.Add('client_width must be a positive integer') }
    if (-not (Test-StrictJsonInt $chRaw) -or [int]$chRaw -le 0) { [void]$errs.Add('client_height must be a positive integer') }
    if ($null -ne $mon -and (Test-StrictJsonInt $cwRaw) -and (Test-StrictJsonInt $chRaw)) {
        if ([int]$cwRaw -gt ($mon[2] - $mon[0]) -or [int]$chRaw -gt ($mon[3] - $mon[1])) {
            [void]$errs.Add('client_width/client_height must not exceed the monitor bounds')
        }
    }
    $sxRaw = Get-CalValue $obj 'select_client_x'
    $syRaw = Get-CalValue $obj 'select_client_y'
    if (-not (Test-StrictJsonInt $sxRaw) -or [int]$sxRaw -lt 0) { [void]$errs.Add('select_client_x must be a non-negative integer') }
    if (-not (Test-StrictJsonInt $syRaw) -or [int]$syRaw -lt 0) { [void]$errs.Add('select_client_y must be a non-negative integer') }
    if ((Test-StrictJsonInt $cwRaw) -and (Test-StrictJsonInt $chRaw) -and (Test-StrictJsonInt $sxRaw) -and (Test-StrictJsonInt $syRaw)) {
        $cw = [int]$cwRaw; $ch = [int]$chRaw; $sx = [int]$sxRaw; $sy = [int]$syRaw
        if ($sx -ge $cw) { [void]$errs.Add('select_client_x must be inside [0, client_width)') }
        if ($sy -ge $ch) { [void]$errs.Add('select_client_y must be inside [0, client_height)') }
        if (($sx + 120) -gt ($cw - 1)) { [void]$errs.Add('drag endpoint (select_client_x+120) is outside the client') }
    }
    $pptRaw = Get-CalValue $obj 'physical_pixels_per_logical_pixel'
    $ppt = $null
    if ($null -eq $pptRaw -or $pptRaw -is [bool] -or $pptRaw -is [string] -or $pptRaw -is [System.Array] -or $pptRaw -is [System.Management.Automation.PSCustomObject]) {
        [void]$errs.Add('physical_pixels_per_logical_pixel must be a finite positive number')
    } else {
        try {
            $ppt = [double]$pptRaw
            if ([double]::IsNaN($ppt) -or [double]::IsInfinity($ppt) -or $ppt -le 0.0 -or $ppt -gt 1000.0) {
                [void]$errs.Add('physical_pixels_per_logical_pixel must be finite in (0, 1000]'); $ppt = $null
            }
        } catch { [void]$errs.Add('physical_pixels_per_logical_pixel must be a finite positive number'); $ppt = $null }
    }
    # Whole injected gesture path must stay inside the physical client:
    # arm press sx-offset -> anchor sx -> endpoint sx+120 -> return press
    # sx+120+offset -> sx. The offset is derived from the independent measured
    # physical scale (see Get-ArmPressOffset); both new press points are admitted
    # here, and an invalid scale has already failed closed above.
    if ((Test-StrictJsonInt $cwRaw) -and (Test-StrictJsonInt $chRaw) -and (Test-StrictJsonInt $sxRaw) -and (Test-StrictJsonInt $syRaw)) {
        $armOffset = Get-ArmPressOffset -MaximizedMode:$true -Calibration $obj
        if ($null -ne $armOffset) {
            if (($sx - $armOffset) -lt 0) { [void]$errs.Add('arm press (select_client_x-' + $armOffset + ') is outside the client') }
            if (($sx + 120 + $armOffset) -gt ($cw - 1)) { [void]$errs.Add('return press (select_client_x+120+' + $armOffset + ') is outside the client') }
        }
    }
    $csRaw = Get-CalValue $obj 'canvas_scale'
    if (-not (Test-StrictJsonInt $csRaw) -or [int]$csRaw -le 0) { [void]$errs.Add('canvas_scale must be a positive integer') }

    if ($errs.Count -gt 0) {
        $res['errors'] = @($errs)
        $res['reason'] = ($errs -join ' | ')
        return $res
    }
    $res['calibration'] = [ordered]@{
        schema = $schema
        calibration_id = [string]$calId
        monitor_device = [string]$dev
        monitor_bounds = $mon
        work_bounds = $work
        dpi = [int]$dpiRaw
        client_width = [int]$cwRaw
        client_height = [int]$chRaw
        select_client_x = [int]$sxRaw
        select_client_y = [int]$syRaw
        physical_pixels_per_logical_pixel = [double]$ppt
        canvas_scale = [int]$csRaw
        source_path = $full
        source_sha256 = $res['actual_sha256']
    }
    $res['ok'] = $true
    $res['errors'] = @()
    return $res
}

function Read-ObserverRoiConfig {
    <#
      Strictly validate the pinned vacards-screen-observer-roi/1 JSON and pin it
      to the SAME maximized calibration monitor/work geometry. Hash first, then
      ConvertFrom-Json (never Invoke-Expression), then every field/type/bound.
      The config supplies the observer's monitor DPI (e.g. 144) and the narrow
      desktop ROI strip. That monitor DPI is deliberately NOT equated with the
      measured window DPI (the calibration `dpi`, e.g. 96). The ROI must lie
      fully inside the pinned calibration monitor_bounds AND work_bounds; the
      strip is never auto-calibrated or guessed here.
      Returns { ok, reason, actual_sha256, schema, roi, errors }.
    #>
    param(
        [string]$Path,
        [string]$ExpectedSha256,
        $Calibration
    )
    $res = [ordered]@{ ok = $false; reason = $null; actual_sha256 = $null; schema = $null; roi = $null; errors = @() }
    $errs = New-Object System.Collections.Generic.List[string]
    $res['errors'] = @($errs)
    if ($null -eq $Calibration) {
        [void]$errs.Add('observer ROI config requires the maximized calibration')
        $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        [void]$errs.Add('ObserverRoiConfigPath is missing or not a file'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res
    }
    $full = $null
    try { $full = (Resolve-Path -LiteralPath $Path).Path } catch { [void]$errs.Add('ObserverRoiConfigPath could not be resolved'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    try { $res['actual_sha256'] = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() }
    catch { [void]$errs.Add('observer ROI config sha256 could not be computed'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    if ($res['actual_sha256'] -ne ([string]$ExpectedSha256).ToLowerInvariant()) {
        [void]$errs.Add('observer ROI config sha256 does not match ExpectedObserverRoiConfigSha256'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res
    }
    $text = $null
    try { $text = [IO.File]::ReadAllText($full) } catch { [void]$errs.Add('observer ROI config is unreadable'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    $obj = $null
    try { $obj = $text | ConvertFrom-Json } catch { [void]$errs.Add('observer ROI config is not valid JSON'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }
    if ($null -eq $obj) { [void]$errs.Add('observer ROI config JSON is empty'); $res['reason'] = $errs[0]; $res['errors'] = @($errs); return $res }

    $schema = [string](Get-CalValue $obj 'schema')
    $res['schema'] = $schema
    if ($schema -ne 'vacards-screen-observer-roi/1') { [void]$errs.Add('schema is not vacards-screen-observer-roi/1 (got ' + $schema + ')') }
    $roiId = Get-CalValue $obj 'roi_id'
    if ($roiId -isnot [string] -or [string]::IsNullOrWhiteSpace($roiId)) { [void]$errs.Add('roi_id must be a non-empty string') }
    $dev = Get-CalValue $obj 'monitor_device'
    if ($dev -isnot [string] -or [string]::IsNullOrWhiteSpace($dev)) { [void]$errs.Add('monitor_device must be a non-empty string') }

    $mon = Read-CalibrationBounds $obj 'monitor_bounds' $errs
    $dpiRaw = Get-CalValue $obj 'monitor_dpi'
    $dxRaw = Get-CalValue $obj 'desktop_x'
    $dyRaw = Get-CalValue $obj 'desktop_y'
    $dwRaw = Get-CalValue $obj 'desktop_width'
    $dhRaw = Get-CalValue $obj 'desktop_height'
    $ewRaw = Get-CalValue $obj 'expected_blue_width'
    if (-not (Test-StrictJsonInt $dpiRaw) -or [int]$dpiRaw -le 0 -or [int]$dpiRaw -gt 960) { [void]$errs.Add('monitor_dpi must be an integer in 1..960') }
    if (-not (Test-StrictJsonInt $dxRaw) -or [int]$dxRaw -lt 0) { [void]$errs.Add('desktop_x must be a non-negative integer') }
    if (-not (Test-StrictJsonInt $dyRaw) -or [int]$dyRaw -lt 0) { [void]$errs.Add('desktop_y must be a non-negative integer') }
    if (-not (Test-StrictJsonInt $dwRaw) -or [int]$dwRaw -le 0 -or [int]$dwRaw -gt 1024) { [void]$errs.Add('desktop_width must be an integer in 1..1024') }
    if (-not (Test-StrictJsonInt $dhRaw) -or [int]$dhRaw -le 0 -or [int]$dhRaw -gt 64) { [void]$errs.Add('desktop_height must be an integer in 1..64') }
    if (-not (Test-StrictJsonInt $ewRaw) -or [int]$ewRaw -le 0 -or [int]$ewRaw -gt 1024) { [void]$errs.Add('expected_blue_width must be an integer in 1..1024') }
    if ($null -ne $mon -and ($mon[0] -ge $mon[2] -or $mon[1] -ge $mon[3])) { [void]$errs.Add('monitor_bounds must satisfy left<right and top<bottom') }
    if ((Test-StrictJsonInt $dwRaw) -and (Test-StrictJsonInt $dhRaw) -and (([int]$dwRaw * [int]$dhRaw) -gt 65536)) { [void]$errs.Add('desktop_width*desktop_height exceeds the observer 65536 px narrow-strip budget') }
    # desktop_width is the capture STRIP width (target run + surrounding
    # background + movement margin); expected_blue_width is the target rectangle
    # run width. A target run needs a strip at least as wide as itself to observe
    # its full excursion, so the only width contract here is positive target <=
    # strip. Equality is deliberately NOT required: the actual baseline and
    # trajectory margins are verified separately by the runtime pilot.
    if ((Test-StrictJsonInt $ewRaw) -and (Test-StrictJsonInt $dwRaw)) {
        if ([int]$ewRaw -gt [int]$dwRaw) {
            [void]$errs.Add('expected_blue_width must be positive and no wider than desktop_width (target ' + [string][int]$ewRaw + ' > strip ' + [string][int]$dwRaw + ')')
        }
    }

    $calMon = @(Get-CalValue $Calibration 'monitor_bounds')
    $calWork = @(Get-CalValue $Calibration 'work_bounds')
    $calDev = [string](Get-CalValue $Calibration 'monitor_device')
    if ($null -ne $mon -and $calMon.Count -eq 4) {
        if ($mon[0] -ne $calMon[0] -or $mon[1] -ne $calMon[1] -or $mon[2] -ne $calMon[2] -or $mon[3] -ne $calMon[3]) {
            [void]$errs.Add('monitor_bounds do not match the pinned MaximizedCalibration monitor_bounds')
        }
    }
    if ($dev -is [string] -and $dev -ne $calDev) { [void]$errs.Add('monitor_device does not match the pinned MaximizedCalibration monitor_device') }
    if ((Test-StrictJsonInt $dxRaw) -and (Test-StrictJsonInt $dyRaw) -and (Test-StrictJsonInt $dwRaw) -and (Test-StrictJsonInt $dhRaw)) {
        $rx = [int]$dxRaw; $ry = [int]$dyRaw; $rw = [int]$dwRaw; $rh = [int]$dhRaw
        if ($null -ne $mon -and $calMon.Count -eq 4) {
            if ($rx -lt $calMon[0] -or $ry -lt $calMon[1] -or ($rx + $rw) -gt $calMon[2] -or ($ry + $rh) -gt $calMon[3]) {
                [void]$errs.Add('ROI desktop rectangle lies outside the pinned MaximizedCalibration monitor_bounds')
            }
        }
        if ($calWork.Count -eq 4) {
            if ($rx -lt $calWork[0] -or $ry -lt $calWork[1] -or ($rx + $rw) -gt $calWork[2] -or ($ry + $rh) -gt $calWork[3]) {
                [void]$errs.Add('ROI desktop rectangle lies outside the pinned MaximizedCalibration work_bounds')
            }
        }
    }

    if ($errs.Count -gt 0) { $res['errors'] = @($errs); $res['reason'] = ($errs -join ' | '); return $res }
    $res['roi'] = [ordered]@{
        schema = $schema
        roi_id = [string]$roiId
        monitor_device = [string]$dev
        monitor_bounds = $mon
        monitor_dpi = [int]$dpiRaw
        desktop_x = [int]$dxRaw
        desktop_y = [int]$dyRaw
        desktop_width = [int]$dwRaw
        desktop_height = [int]$dhRaw
        expected_blue_width = [int]$ewRaw
        source_path = $full
        source_sha256 = $res['actual_sha256']
    }
    $res['ok'] = $true
    $res['errors'] = @()
    return $res
}

function Test-ObserverReadyRecord {
    <#
      Pure admission check for the observer's observer-ready.json baseline record.
      The observer writes it only after its first VALID target frame, outside the
      measured window. Accept only ready=true with a POSITIVE INTEGER qpc_ready
      and a qpc_frequency equal to the live QPC frequency; null or merely
      present values are NOT enough. Returns { ok, reason, qpc_ready,
      qpc_frequency }.
    #>
    param($Ready, [long]$ExpectedQpcFrequency)
    $res = [ordered]@{ ok = $false; reason = $null; qpc_ready = $null; qpc_frequency = $null }
    if ($null -eq $Ready) { $res['reason'] = 'ready record is null'; return $res }
    if ((Get-CalValue $Ready 'ready') -ne $true) { $res['reason'] = 'ready is not true'; return $res }
    $qpcRaw = Get-CalValue $Ready 'qpc_ready'
    $freqRaw = Get-CalValue $Ready 'qpc_frequency'
    if (-not (Test-StrictJsonInt $qpcRaw) -or [long]$qpcRaw -le 0) {
        $res['reason'] = 'qpc_ready is not a positive integer'; return $res
    }
    if ([long]$ExpectedQpcFrequency -le 0) {
        $res['reason'] = 'expected QPC frequency is not positive'; return $res
    }
    if (-not (Test-StrictJsonInt $freqRaw) -or [long]$freqRaw -ne [long]$ExpectedQpcFrequency) {
        $res['reason'] = 'qpc_frequency does not match the active QPC frequency'; return $res
    }
    $res['qpc_ready'] = [long]$qpcRaw
    $res['qpc_frequency'] = [long]$freqRaw
    $res['ok'] = $true
    return $res
}

function Test-ObserverMetadataCoverage {
    <#
      Pure admission check that the observer's observer-metadata.json proves the
      observer's OWN QPC lifetime actually covered the measured input window.
      Requires the pinned schema, the live helper QPC frequency, fatal strictly
      false, exit_code exactly 0 and exit_reason exactly 'stop_file', positive
      integer qpc_start/qpc_ready/qpc_end, qpc_start <= input_start_qpc,
      qpc_end >= input_end_qpc and qpc_ready <= qpc_end. Missing/null/wrong-typed
      fields fail closed. Motion (content_changes) is deliberately NOT required:
      a real stall is a valid measured outcome. Observer lifetime endpoints are
      used instead of a LastPresentTime-near-input_end rule. No duration/limit is
      extended here. Returns { ok, reasons, schema, qpc_frequency, qpc_start,
      qpc_ready, qpc_end, exit_code, exit_reason, fatal, input_start_qpc,
      input_end_qpc, start_within_input, end_covers_input,
      ready_within_lifetime, ready_before_input, frequency_matches,
      complete_coverage }.
    #>
    param($Metadata, [long]$ExpectedQpcFrequency, $InputStartQpc, $InputEndQpc)
    $res = [ordered]@{
        ok = $false; reasons = @()
        schema = $null; qpc_frequency = $null
        qpc_start = $null; qpc_ready = $null; qpc_end = $null
        exit_code = $null; exit_reason = $null; fatal = $null
        input_start_qpc = $null; input_end_qpc = $null
        start_within_input = $false; end_covers_input = $false
        ready_within_lifetime = $false; ready_before_input = $false
        frequency_matches = $false
        complete_coverage = $false
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Metadata) { [void]$errs.Add('observer metadata is null'); $res['reasons'] = @($errs); return $res }
    $schema = Get-CalValue $Metadata 'schema'
    if ($schema -isnot [string] -or [string]$schema -ne 'vacards.screen-observer/1') {
        [void]$errs.Add('schema is not vacards.screen-observer/1')
    } else { $res['schema'] = [string]$schema }
    if ([long]$ExpectedQpcFrequency -le 0) { [void]$errs.Add('helper QPC frequency is not positive') }
    $freq = Get-CalValue $Metadata 'qpc_frequency'
    if (-not (Test-StrictJsonInt $freq) -or [long]$freq -le 0) {
        [void]$errs.Add('qpc_frequency is not a positive integer')
    } else {
        $res['qpc_frequency'] = [long]$freq
        if ([long]$ExpectedQpcFrequency -gt 0 -and [long]$freq -ne [long]$ExpectedQpcFrequency) {
            [void]$errs.Add('qpc_frequency does not match the helper QPC frequency')
        } elseif ([long]$ExpectedQpcFrequency -gt 0) { $res['frequency_matches'] = $true }
    }
    $fatal = Get-CalValue $Metadata 'fatal'
    if ($fatal -isnot [bool] -or $fatal -ne $false) { [void]$errs.Add('fatal is not strictly false') }
    else { $res['fatal'] = $false }
    $exitCode = Get-CalValue $Metadata 'exit_code'
    if (-not (Test-StrictJsonInt $exitCode) -or [long]$exitCode -ne 0) { [void]$errs.Add('exit_code is not 0') }
    else { $res['exit_code'] = 0 }
    $exitReason = Get-CalValue $Metadata 'exit_reason'
    if ($exitReason -isnot [string] -or [string]$exitReason -ne 'stop_file') { [void]$errs.Add('exit_reason is not stop_file') }
    else { $res['exit_reason'] = 'stop_file' }
    $qs = Get-CalValue $Metadata 'qpc_start'
    $qr = Get-CalValue $Metadata 'qpc_ready'
    $qe = Get-CalValue $Metadata 'qpc_end'
    if (-not (Test-StrictJsonInt $qs) -or [long]$qs -le 0) { [void]$errs.Add('qpc_start is not a positive integer') } else { $res['qpc_start'] = [long]$qs }
    if (-not (Test-StrictJsonInt $qr) -or [long]$qr -le 0) { [void]$errs.Add('qpc_ready is not a positive integer') } else { $res['qpc_ready'] = [long]$qr }
    if (-not (Test-StrictJsonInt $qe) -or [long]$qe -le 0) { [void]$errs.Add('qpc_end is not a positive integer') } else { $res['qpc_end'] = [long]$qe }
    if (-not (Test-StrictJsonInt $InputStartQpc) -or [long]$InputStartQpc -le 0) {
        [void]$errs.Add('input_start_qpc is missing/null/wrong type')
    } else { $res['input_start_qpc'] = [long]$InputStartQpc }
    if (-not (Test-StrictJsonInt $InputEndQpc) -or [long]$InputEndQpc -le 0) {
        [void]$errs.Add('input_end_qpc is missing/null/wrong type')
    } else { $res['input_end_qpc'] = [long]$InputEndQpc }
    if ((Test-StrictJsonInt $InputStartQpc) -and (Test-StrictJsonInt $InputEndQpc) -and
        [long]$InputStartQpc -gt 0 -and [long]$InputEndQpc -gt 0 -and [long]$InputEndQpc -lt [long]$InputStartQpc) {
        [void]$errs.Add('input window end is before input window start')
    }
    if ($null -ne $res['qpc_start'] -and $null -ne $res['input_start_qpc']) {
        if ([long]$res['qpc_start'] -le [long]$res['input_start_qpc']) { $res['start_within_input'] = $true }
        else { [void]$errs.Add('qpc_start is after input_start_qpc (observer did not cover the window start)') }
    }
    if ($null -ne $res['qpc_end'] -and $null -ne $res['input_end_qpc']) {
        if ([long]$res['qpc_end'] -ge [long]$res['input_end_qpc']) { $res['end_covers_input'] = $true }
        else { [void]$errs.Add('qpc_end is before input_end_qpc (observer stopped before the measured window ended)') }
    }
    if ($null -ne $res['qpc_ready'] -and $null -ne $res['qpc_end']) {
        if ([long]$res['qpc_ready'] -le [long]$res['qpc_end']) { $res['ready_within_lifetime'] = $true }
        else { [void]$errs.Add('qpc_ready is after qpc_end') }
    }
    if ($null -ne $res['qpc_ready'] -and $null -ne $res['input_start_qpc']) {
        # The ready baseline must be established strictly before the measured
        # window opens; otherwise observer warmup extended into the workload and
        # the valid baseline is not a pre-input baseline.
        if ([long]$res['qpc_ready'] -le [long]$res['input_start_qpc']) { $res['ready_before_input'] = $true }
        else { [void]$errs.Add('qpc_ready is after input_start_qpc (warmup extended into the measured window)') }
    }
    if ($null -ne $res['qpc_start'] -and $null -ne $res['qpc_ready'] -and
        [long]$res['qpc_start'] -gt [long]$res['qpc_ready']) {
        [void]$errs.Add('qpc_start is after qpc_ready')
    }
    $res['complete_coverage'] = ($errs.Count -eq 0)
    $res['ok'] = ($errs.Count -eq 0)
    $res['reasons'] = @($errs)
    return $res
}

function Start-ScreenObserver {
    <#
      Launch the compiled observer in its own retained child process and wait,
      bounded, for the ready file. The ready file is written only after the
      observer's first VALID target baseline, so a returned ready_seen=$true is
      an actual valid baseline before any input. stdout/stderr are drained with
      the existing owned CopyToAsync pattern. Any failure is returned, not
      thrown here; the caller decides the exit code.
    #>
    param(
        [string]$ExePath,
        [string]$ExpectedExeSha256,
        $Roi,
        [int]$TargetPid,
        [long]$Hwnd,
        [string]$OutDir,
        [int]$ReadyTimeoutMs = 20000
    )
    $res = [ordered]@{
        requested = $true; started = $false; ready_seen = $false; ready_qpc = $null
        ready_wait_ms = $null; exe_path = $ExePath; exe_actual_sha256 = $null
        expected_exe_sha256 = ([string]$ExpectedExeSha256).ToLowerInvariant()
        out_dir = $OutDir; ready_file = $null; stop_file = $null
        started_qpc = $null; started_utc = $null; process_id = $null
        cpu_seconds_before = $null; error = $null
    }
    if ([string]::IsNullOrWhiteSpace($ExePath) -or -not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        $res['error'] = 'ObserverExePath is missing or not a file'; return $res
    }
    try { $res['exe_actual_sha256'] = (Get-FileHash -LiteralPath $ExePath -Algorithm SHA256).Hash.ToLowerInvariant() }
    catch { $res['error'] = 'observer exe sha256 could not be computed'; return $res }
    if ($res['exe_actual_sha256'] -ne $res['expected_exe_sha256']) {
        $res['error'] = 'observer exe sha256 does not match ExpectedObserverExeSha256'; return $res
    }
    if (Test-Path -LiteralPath $OutDir) { $res['error'] = 'observer out-dir already exists'; return $res }
    $res['ready_file'] = Join-Path $OutDir 'observer-ready.json'
    $res['stop_file'] = Join-Path $OutDir 'observer-stop'
    $bounds = @($Roi['monitor_bounds'])
    $roiRect = @([int]$Roi['desktop_x'], [int]$Roi['desktop_y'], [int]$Roi['desktop_width'], [int]$Roi['desktop_height'])
    $argList = @(
        ('--pid=' + [string]$TargetPid),
        ('--hwnd=0x' + $Hwnd.ToString('X')),
        ('--monitor=' + [string]$Roi['monitor_device']),
        ('--bounds=' + (@($bounds) -join ',')),
        ('--dpi=' + [string]$Roi['monitor_dpi']),
        ('--roi=' + (@($roiRect) -join ',')),
        '--rgb=204080',
        ('--expected-width=' + [string]$Roi['expected_blue_width']),
        ('--out-dir=' + $OutDir),
        ('--ready-file=' + $res['ready_file']),
        ('--stop-file=' + $res['stop_file'])
    )
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $ExePath
        $psi.Arguments = (($argList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' ')
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.CreateNoWindow = $true
        $psi.WorkingDirectory = (Split-Path -Parent $ExePath)
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        if (-not $proc.Start()) { $res['error'] = 'could not start the screen observer'; return $res }
        # Retain the owned handle before anything else so the finally safety net
        # can always stop this process.
        $script:observerProcess = $proc
        # Logs live in the existing parent of OutDir: the observer itself owns
        # creating the (non-existing) out-dir, so writing logs under OutDir
        # would race its CreateDirectoryW.
        $logDir = Split-Path -Parent $OutDir
        $script:observerOutStream = [IO.File]::Create((Join-Path $logDir 'observer-stdout.log'))
        $script:observerErrStream = [IO.File]::Create((Join-Path $logDir 'observer-stderr.log'))
        $script:observerOutTask = $proc.StandardOutput.BaseStream.CopyToAsync($script:observerOutStream)
        $script:observerErrTask = $proc.StandardError.BaseStream.CopyToAsync($script:observerErrStream)
        $res['started'] = $true
        $res['process_id'] = [int]$proc.Id
        $res['started_qpc'] = [VAPan.Win32]::Qpc()
        $res['started_utc'] = [DateTime]::UtcNow.ToString('o')
        try { $proc.Refresh(); if (-not $proc.HasExited) { $res['cpu_seconds_before'] = $proc.TotalProcessorTime.TotalSeconds } } catch {}
        $script:observerCpuBefore = $res['cpu_seconds_before']
    } catch {
        $res['error'] = $_.Exception.Message
        return $res
    }
    $deadline = [DateTime]::UtcNow.AddMilliseconds($ReadyTimeoutMs)
    $waitStart = [DateTime]::UtcNow
    $readyFreq = [long][VAPan.Win32]::QpcFrequency()
    $readyFailReason = $null
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $res['ready_file'] -PathType Leaf) {
            # The observer writes this file once, right after its first valid
            # baseline. Windows can expose the directory entry before the bytes
            # are complete, so re-read and re-parse until the bounded deadline
            # rather than trusting a transient partial record or a bare name.
            $ready = $null
            $parsed = $false
            try {
                $ready = (Get-Content -Raw -LiteralPath $res['ready_file'] -ErrorAction Stop | ConvertFrom-Json)
                $parsed = $true
            } catch { $parsed = $false }
            if ($parsed) {
                $check = Test-ObserverReadyRecord -Ready $ready -ExpectedQpcFrequency $readyFreq
                if ([bool]$check['ok']) {
                    $res['ready_seen'] = $true
                    $res['ready_qpc'] = $check['qpc_ready']
                    break
                }
                # A complete but semantically invalid record will not heal by
                # waiting; fail fast with its concrete reason.
                $readyFailReason = [string]$check['reason']
                break
            }
            $readyFailReason = 'observer ready file was not yet a complete parseable record'
        }
        if ($script:observerProcess.HasExited) { break }
        Start-Sleep -Milliseconds 50
    }
    $res['ready_wait_ms'] = [int]([DateTime]::UtcNow - $waitStart).TotalMilliseconds
    if (-not $res['ready_seen']) {
        if ($null -ne $readyFailReason) { $res['error'] = $readyFailReason }
        elseif ($script:observerProcess.HasExited) { $res['error'] = 'observer exited before writing a valid baseline ready file' }
        else { $res['error'] = 'observer ready file did not appear within ' + $ReadyTimeoutMs + ' ms' }
    }
    return $res
}

function Stop-ScreenObserver {
    <#
      Stop the owned observer after the measured input window: create the stop
      file, wait a bounded interval for a normal exit, and force-kill ONLY the
      owned process if it failed to exit. Collect its CPU, raw frames.csv and
      observer-metadata.json paths/hashes/counts. Never kills a process that is
      not the one this helper started.
    #>
    param(
        [int]$StopTimeoutMs = 15000,
        $InputStartQpc = $null,
        $InputEndQpc = $null
    )
    $res = [ordered]@{
        exited = $false; exit_code = $null; forced_kill = $false; timed_out = $false
        stopped_qpc = $null; stopped_utc = $null
        cpu_seconds_before = $script:observerCpuBefore
        cpu_seconds_after = $null; cpu_seconds_delta = $null
        metadata_path = $null; metadata_sha256 = $null
        metadata_schema = $null; metadata_qpc_frequency = $null
        metadata_qpc_start = $null; metadata_qpc_ready = $null; metadata_qpc_end = $null
        metadata_exit_code = $null; metadata_exit_reason = $null; metadata_fatal = $null
        metadata_input_start_qpc = $null; metadata_input_end_qpc = $null
        coverage_start_within_input = $false; coverage_end_covers_input = $false
        coverage_ready_within_lifetime = $false; coverage_ready_before_input = $false
        coverage_frequency_matches = $false
        complete_coverage = $false; coverage_reasons = @()
        frames_csv_path = $null; frames_csv_sha256 = $null
        frames_written = $null; valid_frames = $null; missing_frames = $null
        merged_frames = $null; ambiguous_frames = $null
        content_changes = $null; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if ($null -eq $script:observerProcess) { [void]$errs.Add('observer process was never started'); $res['errors'] = @($errs); return $res }
    $proc = $script:observerProcess
    try {
        if (-not [string]::IsNullOrWhiteSpace($script:observerStopFile)) { [IO.File]::WriteAllText($script:observerStopFile, 'stop') }
    } catch { [void]$errs.Add('could not create the observer stop file: ' + $_.Exception.Message) }
    $deadline = [DateTime]::UtcNow.AddMilliseconds($StopTimeoutMs)
    try { $proc.Refresh() } catch {}
    while (-not $proc.HasExited -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
    try { $proc.Refresh() } catch {}
    if (-not $proc.HasExited) {
        $res['timed_out'] = $true
        $res['forced_kill'] = $true
        try { $proc.Kill() } catch {}
        try { [void]$proc.WaitForExit(5000) } catch {}
    }
    $res['stopped_qpc'] = [VAPan.Win32]::Qpc()
    $res['stopped_utc'] = [DateTime]::UtcNow.ToString('o')
    try { $proc.Refresh() } catch {}
    if ($proc.HasExited) {
        $res['exited'] = $true
        try { $res['exit_code'] = [int]$proc.ExitCode } catch { $res['exit_code'] = $null }
    }
    try { $res['cpu_seconds_after'] = $proc.TotalProcessorTime.TotalSeconds } catch {}
    if ($null -ne $res['cpu_seconds_before'] -and $null -ne $res['cpu_seconds_after']) {
        $res['cpu_seconds_delta'] = $res['cpu_seconds_after'] - $res['cpu_seconds_before']
    }
    try { [System.Threading.Tasks.Task]::WaitAll(@($script:observerOutTask, $script:observerErrTask), 8000) | Out-Null } catch {}
    foreach ($stream in @($script:observerOutStream, $script:observerErrStream)) {
        if ($null -ne $stream) { try { $stream.Flush(); $stream.Dispose() } catch {} }
    }
    $script:observerOutStream = $null; $script:observerErrStream = $null

    if (-not [string]::IsNullOrWhiteSpace($script:observerOutDir)) {
        $meta = Join-Path $script:observerOutDir 'observer-metadata.json'
        $csv = Join-Path $script:observerOutDir 'frames.csv'
        if (Test-Path -LiteralPath $meta -PathType Leaf) {
            $res['metadata_path'] = $meta
            try { $res['metadata_sha256'] = (Get-FileHash -LiteralPath $meta -Algorithm SHA256).Hash.ToLowerInvariant() }
            catch { [void]$errs.Add('observer-metadata.json sha256 could not be computed: ' + $_.Exception.Message) }
            try {
                $m = (Get-Content -Raw -LiteralPath $meta | ConvertFrom-Json)
                $counts = Get-CalValue $m 'counts'
                if ($null -ne $counts) {
                    $res['frames_written'] = Get-CalValue $counts 'frames_written'
                    $res['valid_frames'] = Get-CalValue $counts 'valid'
                    $res['missing_frames'] = Get-CalValue $counts 'missing'
                    $res['merged_frames'] = Get-CalValue $counts 'merged'
                    $res['ambiguous_frames'] = Get-CalValue $counts 'ambiguous'
                    $res['content_changes'] = Get-CalValue $counts 'content_changes'
                }
                # Fail-closed coverage admission: the observer must have lived
                # from before input_start_qpc through after input_end_qpc and
                # stopped for the stop file. The raw metadata fields and the
                # recomputed flag are retained in the summary so the Trial can
                # independently revalidate them against its own window.
                $cov = Test-ObserverMetadataCoverage -Metadata $m -ExpectedQpcFrequency ([long][VAPan.Win32]::QpcFrequency()) -InputStartQpc $InputStartQpc -InputEndQpc $InputEndQpc
                $res['metadata_schema'] = $cov['schema']
                $res['metadata_qpc_frequency'] = $cov['qpc_frequency']
                $res['metadata_qpc_start'] = $cov['qpc_start']
                $res['metadata_qpc_ready'] = $cov['qpc_ready']
                $res['metadata_qpc_end'] = $cov['qpc_end']
                $res['metadata_exit_code'] = $cov['exit_code']
                $res['metadata_exit_reason'] = $cov['exit_reason']
                $res['metadata_fatal'] = $cov['fatal']
                $res['metadata_input_start_qpc'] = $cov['input_start_qpc']
                $res['metadata_input_end_qpc'] = $cov['input_end_qpc']
                $res['coverage_start_within_input'] = [bool]$cov['start_within_input']
                $res['coverage_end_covers_input'] = [bool]$cov['end_covers_input']
                $res['coverage_ready_within_lifetime'] = [bool]$cov['ready_within_lifetime']
                $res['coverage_ready_before_input'] = [bool]$cov['ready_before_input']
                $res['coverage_frequency_matches'] = [bool]$cov['frequency_matches']
                $res['complete_coverage'] = [bool]$cov['complete_coverage']
                $res['coverage_reasons'] = @($cov['reasons'])
                foreach ($cr in @($cov['reasons'])) { [void]$errs.Add('observer metadata coverage: ' + [string]$cr) }
            } catch { [void]$errs.Add('observer-metadata.json could not be parsed: ' + $_.Exception.Message) }
        } else { [void]$errs.Add('observer-metadata.json is missing') }
        if (Test-Path -LiteralPath $csv -PathType Leaf) {
            $res['frames_csv_path'] = $csv
            try { $res['frames_csv_sha256'] = (Get-FileHash -LiteralPath $csv -Algorithm SHA256).Hash.ToLowerInvariant() }
            catch { [void]$errs.Add('frames.csv sha256 could not be computed: ' + $_.Exception.Message) }
        } else { [void]$errs.Add('frames.csv is missing') }
    }
    if (-not $res['exited']) { [void]$errs.Add('observer process did not exit within the bound') }
    elseif ([int]$res['exit_code'] -ne 0) { [void]$errs.Add('observer exit code was ' + [string]$res['exit_code']) }
    if ($res['forced_kill']) { [void]$errs.Add('observer process had to be force-killed (owned)') }
    if ($null -eq $res['frames_written'] -or [long]$res['frames_written'] -lt 1) { [void]$errs.Add('observer wrote no frames') }
    # counts.frames_written includes warmup frames. counts.valid is incremented
    # ONLY in the measured loop (post-warmup), so a run with warmup frames but no
    # valid measured-loop frame carries no measured evidence and is inadmissible.
    # Motion (content_changes) is deliberately NOT required: a real stall is a
    # valid measured outcome. missing/merged/ambiguous are retained for review.
    if ($null -eq $res['valid_frames'] -or [long]$res['valid_frames'] -lt 1) { [void]$errs.Add('observer recorded no valid measured-loop frames (counts.valid is post-warmup)') }
    $res['errors'] = @($errs)
    return $res
}

function Set-ObserverSummary {
    param($Start, $Stop, $Meta, [string]$ExePath, [string]$ExpectedExeSha256, [string]$OutDir, [string]$StopFile)
    $startOk = $null -ne $Start
    $stopOk = $null -ne $Stop
    # Hoist the two diagnostic string collections into REAL arrays. A PowerShell
    # `if` used as an expression emits AutomationNull when its branch produces no
    # output, and ConvertTo-Json then serializes that as `{}` instead of `[]`,
    # which the Trial consumer cannot distinguish from an error object. These
    # locals are always true Object[] (empty, singleton or multiple).
    $stopErrors = @()
    $stopCoverageReasons = @()
    if ($stopOk) {
        $stopErrors = @($Stop['errors'])
        $stopCoverageReasons = @($Stop['coverage_reasons'])
    }
    $script:summary['observer'] = [ordered]@{
        schema                       = 'vacards-app-pan-observer/1'
        requested                    = $true
        exe_path                     = $ExePath
        exe_actual_sha256            = if ($startOk) { $Start['exe_actual_sha256'] } else { $null }
        expected_exe_sha256          = ([string]$ExpectedExeSha256).ToLowerInvariant()
        out_dir                      = $OutDir
        roi_config_path              = if ($null -ne $Meta) { $Meta['path'] } else { $null }
        roi_config_actual_sha256     = if ($null -ne $Meta) { $Meta['actual_sha256'] } else { $null }
        expected_roi_config_sha256   = if ($null -ne $Meta) { $Meta['expected_sha256'] } else { $null }
        roi_config_schema            = if ($null -ne $Meta) { $Meta['schema'] } else { $null }
        roi_id                       = if ($null -ne $Meta) { $Meta['roi_id'] } else { $null }
        ready_file                   = if ($startOk) { $Start['ready_file'] } else { $null }
        ready_seen                   = if ($startOk) { $Start['ready_seen'] } else { $false }
        ready_qpc                    = if ($startOk) { $Start['ready_qpc'] } else { $null }
        ready_wait_ms                = if ($startOk) { $Start['ready_wait_ms'] } else { $null }
        stop_file                    = $StopFile
        process_id                   = if ($startOk) { $Start['process_id'] } else { $null }
        started_qpc                  = if ($startOk) { $Start['started_qpc'] } else { $null }
        started_utc                  = if ($startOk) { $Start['started_utc'] } else { $null }
        stopped_qpc                  = if ($stopOk) { $Stop['stopped_qpc'] } else { $null }
        stopped_utc                  = if ($stopOk) { $Stop['stopped_utc'] } else { $null }
        exited                       = if ($stopOk) { $Stop['exited'] } else { $false }
        exit_code                    = if ($stopOk) { $Stop['exit_code'] } else { $null }
        forced_kill                  = if ($stopOk) { $Stop['forced_kill'] } else { $false }
        timed_out                    = if ($stopOk) { $Stop['timed_out'] } else { $false }
        cpu_seconds_before           = if ($stopOk) { $Stop['cpu_seconds_before'] } else { if ($startOk) { $Start['cpu_seconds_before'] } else { $null } }
        cpu_seconds_after            = if ($stopOk) { $Stop['cpu_seconds_after'] } else { $null }
        cpu_seconds_delta            = if ($stopOk) { $Stop['cpu_seconds_delta'] } else { $null }
        metadata_path                = if ($stopOk) { $Stop['metadata_path'] } else { $null }
        metadata_sha256              = if ($stopOk) { $Stop['metadata_sha256'] } else { $null }
        metadata_schema              = if ($stopOk) { $Stop['metadata_schema'] } else { $null }
        metadata_qpc_frequency       = if ($stopOk) { $Stop['metadata_qpc_frequency'] } else { $null }
        metadata_qpc_start           = if ($stopOk) { $Stop['metadata_qpc_start'] } else { $null }
        metadata_qpc_ready           = if ($stopOk) { $Stop['metadata_qpc_ready'] } else { $null }
        metadata_qpc_end             = if ($stopOk) { $Stop['metadata_qpc_end'] } else { $null }
        metadata_exit_code           = if ($stopOk) { $Stop['metadata_exit_code'] } else { $null }
        metadata_exit_reason         = if ($stopOk) { $Stop['metadata_exit_reason'] } else { $null }
        metadata_fatal               = if ($stopOk) { $Stop['metadata_fatal'] } else { $null }
        metadata_input_start_qpc     = if ($stopOk) { $Stop['metadata_input_start_qpc'] } else { $null }
        metadata_input_end_qpc       = if ($stopOk) { $Stop['metadata_input_end_qpc'] } else { $null }
        coverage_start_within_input  = if ($stopOk) { $Stop['coverage_start_within_input'] } else { $false }
        coverage_end_covers_input    = if ($stopOk) { $Stop['coverage_end_covers_input'] } else { $false }
        coverage_ready_within_lifetime = if ($stopOk) { $Stop['coverage_ready_within_lifetime'] } else { $false }
        coverage_ready_before_input  = if ($stopOk) { $Stop['coverage_ready_before_input'] } else { $false }
        coverage_frequency_matches   = if ($stopOk) { $Stop['coverage_frequency_matches'] } else { $false }
        complete_coverage            = if ($stopOk) { $Stop['complete_coverage'] } else { $false }
        coverage_reasons             = $stopCoverageReasons
        frames_csv_path              = if ($stopOk) { $Stop['frames_csv_path'] } else { $null }
        frames_csv_sha256            = if ($stopOk) { $Stop['frames_csv_sha256'] } else { $null }
        frames_written               = if ($stopOk) { $Stop['frames_written'] } else { $null }
        valid_frames                 = if ($stopOk) { $Stop['valid_frames'] } else { $null }
        missing_frames               = if ($stopOk) { $Stop['missing_frames'] } else { $null }
        merged_frames                = if ($stopOk) { $Stop['merged_frames'] } else { $null }
        ambiguous_frames             = if ($stopOk) { $Stop['ambiguous_frames'] } else { $null }
        content_changes              = if ($stopOk) { $Stop['content_changes'] } else { $null }
        errors                       = $stopErrors
    }
}

function Test-MaximizedRectProof {
    <#
      Pure maximized-workarea geometry math (no window/desktop needed; exercised
      by the isolated check). Root ROOT-NOTE: the requirement is to cover the
      WORK AREA, not the whole monitor. Given measured [l,t,r,b] rects and the
      DPI it proves:
        - monitor and work identity are checked separately by the caller;
        - the client screen rect covers the work area;
        - the window rect covers the work area;
        - the client is inside the window;
        - the window's native nonclient overhang past the WORK AREA, and each
          client/window frame delta, are non-negative and bounded by the
          DPI-aware frame metric plus the calibrated monitor/work gap. A taskbar
          makes bottom/right gaps (and a classic maximized window that overdraws
          the taskbar band) legitimately larger than the frame, so the bound is
          derived from the calibration, never a blanket containment relaxation.
    #>
    param(
        $WinRect, $ClientScreenRect, $MonitorRect, $WorkRect,
        [int]$Dpi,
        [int]$RoundingPx = 2
    )
    $res = [ordered]@{
        work_overhang = @(); work_overhang_bound = @(); work_overhang_ok = $false
        frame = @(); frame_bound = @(); frame_ok = $false
        coverage_ok = $false; window_covers_work = $false; client_inside_window = $false
        ok = $false; reason = $null; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if ($WinRect.Count -ne 4 -or $ClientScreenRect.Count -ne 4 -or $MonitorRect.Count -ne 4 -or $WorkRect.Count -ne 4) {
        [void]$errs.Add('maximized geometry rects must each have 4 elements')
        $res['errors'] = @($errs); $res['reason'] = $errs[0]; return $res
    }
    $fm = @([VAPan.Win32]::FrameMetricsForDpi($Dpi))
    # Window past the WORK area, per edge [left, top, right, bottom]. Positive
    # means the native maximized window overhangs the work area.
    $res['work_overhang'] = @(
        ($WorkRect[0] - $WinRect[0]),
        ($WorkRect[1] - $WinRect[1]),
        ($WinRect[2] - $WorkRect[2]),
        ($WinRect[3] - $WorkRect[3])
    )
    $res['frame'] = @([VAPan.Win32]::FrameDeltas($WinRect, $ClientScreenRect))
    $boundLeft = ($WorkRect[0] - $MonitorRect[0]) + $fm[0]
    $boundTop = ($WorkRect[1] - $MonitorRect[1]) + $fm[1]
    $boundRight = ($MonitorRect[2] - $WorkRect[2]) + $fm[0]
    $boundBottom = ($MonitorRect[3] - $WorkRect[3]) + $fm[1]
    $res['work_overhang_bound'] = @($boundLeft, $boundTop, $boundRight, $boundBottom)
    $res['frame_bound'] = @($boundLeft, $boundTop, $boundRight, $boundBottom)
    $res['window_covers_work'] = [bool][VAPan.Win32]::RectCovers($WinRect, $WorkRect)
    $res['coverage_ok'] = [bool][VAPan.Win32]::RectCovers($ClientScreenRect, $WorkRect)
    $res['client_inside_window'] = [bool][VAPan.Win32]::RectCovers($WinRect, $ClientScreenRect)
    $res['work_overhang_ok'] = $true
    for ($i = 0; $i -lt 4; $i++) {
        if ($res['work_overhang'][$i] -lt 0 -or $res['work_overhang'][$i] -gt ($res['work_overhang_bound'][$i] + $RoundingPx)) { $res['work_overhang_ok'] = $false }
    }
    $res['frame_ok'] = $true
    for ($i = 0; $i -lt 4; $i++) {
        if ($res['frame'][$i] -lt 0 -or $res['frame'][$i] -gt ($res['frame_bound'][$i] + $RoundingPx)) { $res['frame_ok'] = $false }
    }
    if (-not $res['work_overhang_ok']) { [void]$errs.Add('window overhang past the work area is outside the DPI-aware frame bound (measured ' + (@($res['work_overhang']) -join ',') + ')') }
    if (-not $res['frame_ok']) { [void]$errs.Add('client/window frame deltas are outside the derived bound (measured ' + (@($res['frame']) -join ',') + ')') }
    if (-not $res['coverage_ok']) { [void]$errs.Add('client screen rect does not cover the work area') }
    if (-not $res['window_covers_work']) { [void]$errs.Add('window rect does not cover the work area') }
    if (-not $res['client_inside_window']) { [void]$errs.Add('client screen rect is not inside the window rect') }
    $res['errors'] = @($errs)
    $res['ok'] = ($errs.Count -eq 0)
    $res['reason'] = if ($res['ok']) { $null } else { ($errs -join '; ') }
    return $res
}

function Test-MaximizedGeometryState {
    <#
      Strict maximized-workarea proof for the owned window. Requires
      IsZoomed/showCmd==3/not iconic, exact calibrated physical client,
      exact monitor and work rects, calibrated DPI, full work-area client
      coverage, and a nonclient overhang/frame bounded by the DPI-aware native
      frame metric. Any doubt fails closed; no blanket containment relaxation.
    #>
    param(
        [IntPtr]$WindowHandle,
        $Calibration,
        [int]$RoundingPx = 2
    )
    $res = [ordered]@{
        schema = 'vacards-app-pan-maximized-geometry/1'
        ok = $false; reason = $null
        is_zoomed = $false; is_iconic = $true; placement_show_cmd = $null
        client = @(); bounds = @(); client_screen = @(); monitor = @(); work = @()
        monitor_device = $null
        dpi = $null; exact_client = $false; monitor_match = $false; work_match = $false; dpi_match = $false
        work_overhang = @(); work_overhang_bound = @(); work_overhang_ok = $false
        frame = @(); frame_bound = @(); frame_ok = $false
        coverage_ok = $false
        window_covers_work = $false
        client_inside_window = $false
        requested_monitor = @(); requested_work = @(); requested_client = @(); requested_dpi = $null; requested_device = $null
        rounding_px = $RoundingPx
        errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Calibration) {
        [void]$errs.Add('calibration missing'); $res['errors'] = @($errs); $res['reason'] = $errs[0]; return $res
    }
    $reqMon = @(Get-CalValue $Calibration 'monitor_bounds')
    $reqWork = @(Get-CalValue $Calibration 'work_bounds')
    $reqCw = [int](Get-CalValue $Calibration 'client_width')
    $reqCh = [int](Get-CalValue $Calibration 'client_height')
    $reqDpi = [int](Get-CalValue $Calibration 'dpi')
    $reqDev = [string](Get-CalValue $Calibration 'monitor_device')
    $res['requested_monitor'] = $reqMon; $res['requested_work'] = $reqWork
    $res['requested_client'] = @($reqCw, $reqCh); $res['requested_dpi'] = $reqDpi; $res['requested_device'] = $reqDev

    $wp = [VAPan.Win32]::WindowPlacementInfo($WindowHandle)
    $res['is_zoomed'] = [bool][VAPan.Win32]::Maximized($WindowHandle)
    $res['is_iconic'] = [bool][VAPan.Win32]::Minimized($WindowHandle)
    if ($null -ne $wp) { $res['placement_show_cmd'] = [int]$wp[0] }
    $res['client'] = @([VAPan.Win32]::ClientSize($WindowHandle))
    $res['bounds'] = @([VAPan.Win32]::WindowBounds($WindowHandle))
    $org = @([VAPan.Win32]::ClientScreenOrigin($WindowHandle))
    $res['client_screen'] = @([VAPan.Win32]::ClientScreenRect($org, $res['client'][0], $res['client'][1]))
    $mon = [VAPan.Win32]::MonitorRects($WindowHandle)
    if ($null -ne $mon) {
        $res['monitor'] = @($mon[0], $mon[1], $mon[2], $mon[3])
        $res['work'] = @($mon[4], $mon[5], $mon[6], $mon[7])
    }
    $res['monitor_device'] = [string][VAPan.Win32]::MonitorDevice($WindowHandle)
    $res['dpi'] = [int][VAPan.Win32]::DpiForWindow($WindowHandle)

    $res['exact_client'] = ($res['client'].Count -eq 2 -and $res['client'][0] -eq $reqCw -and $res['client'][1] -eq $reqCh)
    $res['monitor_match'] = ($res['monitor'].Count -eq 4 -and $reqMon.Count -eq 4 -and
        $res['monitor'][0] -eq $reqMon[0] -and $res['monitor'][1] -eq $reqMon[1] -and
        $res['monitor'][2] -eq $reqMon[2] -and $res['monitor'][3] -eq $reqMon[3])
    $res['work_match'] = ($res['work'].Count -eq 4 -and $reqWork.Count -eq 4 -and
        $res['work'][0] -eq $reqWork[0] -and $res['work'][1] -eq $reqWork[1] -and
        $res['work'][2] -eq $reqWork[2] -and $res['work'][3] -eq $reqWork[3])
    $res['dpi_match'] = ($res['dpi'] -eq $reqDpi)
    $devMatch = [string]::IsNullOrWhiteSpace($reqDev) -or ($res['monitor_device'] -ieq $reqDev)

    $rectProof = Test-MaximizedRectProof -WinRect $res['bounds'] -ClientScreenRect $res['client_screen'] -MonitorRect $res['monitor'] -WorkRect $res['work'] -Dpi $res['dpi'] -RoundingPx $RoundingPx
    $res['work_overhang'] = @($rectProof['work_overhang']); $res['work_overhang_bound'] = @($rectProof['work_overhang_bound']); $res['work_overhang_ok'] = [bool]$rectProof['work_overhang_ok']
    $res['frame'] = @($rectProof['frame']); $res['frame_bound'] = @($rectProof['frame_bound']); $res['frame_ok'] = [bool]$rectProof['frame_ok']
    $res['coverage_ok'] = [bool]$rectProof['coverage_ok']
    $res['window_covers_work'] = [bool]$rectProof['window_covers_work']
    $res['client_inside_window'] = [bool]$rectProof['client_inside_window']

    if (-not $res['is_zoomed']) { [void]$errs.Add('window is not maximized (IsZoomed false)') }
    if ($res['is_iconic']) { [void]$errs.Add('window is iconic') }
    if ($res['placement_show_cmd'] -ne 3) { [void]$errs.Add('window placement showCmd is not SW_SHOWMAXIMIZED (3)') }
    if (-not $res['exact_client']) { [void]$errs.Add('client is not the calibrated ' + $reqCw + 'x' + $reqCh) }
    if (-not $res['monitor_match']) { [void]$errs.Add('monitor rect does not match the calibrated monitor_bounds') }
    if (-not $res['work_match']) { [void]$errs.Add('work rect does not match the calibrated work_bounds') }
    if (-not $res['dpi_match']) { [void]$errs.Add('DPI ' + [string]$res['dpi'] + ' is not the calibrated ' + $reqDpi) }
    if (-not $devMatch) {
        $observedDev = [string]$res['monitor_device']
        if ([string]::IsNullOrWhiteSpace($observedDev)) {
            [void]$errs.Add('monitor device query failed (GetMonitorInfoW returned empty; cbSize=' + [int][VAPan.Win32]::MonitorDeviceLastCbSize + ', win32_error=' + [int][VAPan.Win32]::MonitorDeviceLastError + '); calibrated ' + $reqDev)
        } else {
            [void]$errs.Add('monitor device ' + $observedDev + ' is not the calibrated ' + $reqDev)
        }
    }
    if (-not $res['work_overhang_ok']) { [void]$errs.Add('window overhang past the work area is outside the DPI-aware frame bound (measured ' + (@($res['work_overhang']) -join ',') + ')') }
    if (-not $res['frame_ok']) { [void]$errs.Add('client/window frame deltas are outside the DPI-aware frame bound (measured ' + (@($res['frame']) -join ',') + ')') }
    if (-not $res['coverage_ok']) { [void]$errs.Add('client screen rect does not cover the calibrated work area') }
    if (-not $res['window_covers_work']) { [void]$errs.Add('window rect does not cover the calibrated work area') }
    if (-not $res['client_inside_window']) { [void]$errs.Add('client screen rect is not inside the window rect') }

    $res['errors'] = @($errs)
    $res['ok'] = ($errs.Count -eq 0)
    $res['reason'] = if ($res['ok']) { $null } else { ($errs -join '; ') }
    return $res
}

function Test-MeasuredGeometryGuard {
    <#
      Maximized-workarea invariant guard for the MEASURED drag window and the
      return gesture. In the default legacy mode it is a no-op. In
      maximized-workarea mode it re-runs the strict calibrated proof
      (IsZoomed/showCmd/monitor/work/DPI/client/work coverage) and appends its
      result to `measured_geometry_guards`; any drift throws exit 4, so the
      caller's unconditional finally releases the held left button. It is called
      at leg boundaries and after the final release/return, never on every event
      (no heavyweight per-event allocation). A script-scope
      `measuredGeometryGuard` scriptblock may be injected by the isolated check.
    #>
    param([string]$Where)
    if (-not $script:maximizedMode) { return $true }
    $g = if ($null -ne $script:measuredGeometryGuard) { & $script:measuredGeometryGuard $script:hwnd } else { Test-MaximizedGeometryState -WindowHandle $script:hwnd -Calibration $script:maximizedCalibration }
    if ($null -eq $script:measuredGeometryGuards) { $script:measuredGeometryGuards = New-Object System.Collections.ArrayList }
    [void]$script:measuredGeometryGuards.Add([ordered]@{
        where = $Where
        ok = [bool]$g['ok']
        is_zoomed = $g['is_zoomed']
        is_iconic = $g['is_iconic']
        exact_client = $g['exact_client']
        monitor_match = $g['monitor_match']
        work_match = $g['work_match']
        dpi_match = $g['dpi_match']
        coverage_ok = $g['coverage_ok']
        work_overhang_ok = $g['work_overhang_ok']
        qpc = [VAPan.Win32]::Qpc()
    })
    if (-not [bool]$g['ok']) {
        Throw-PanFailure 4 ('maximized-workarea measured geometry guard failed at ' + $Where + ': ' + [string]$g['reason'])
    }
    return $true
}

function Get-FileSignature {
    param([string]$Path)
    $fi = Get-Item -LiteralPath $Path -ErrorAction Stop
    return [ordered]@{
        path           = $fi.FullName
        length         = [long]$fi.Length
        last_write_utc = $fi.LastWriteTimeUtc.ToString('o')
        mtime_ticks    = [long]$fi.LastWriteTimeUtc.Ticks
        sha256         = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Test-XmlFileLoadable {
    # DTD/external entities are never resolved; a well-formed element tree is
    # all this proves.
    param([string]$Path)
    try {
        $doc = New-Object System.Xml.XmlDocument
        $doc.XmlResolver = $null
        $doc.Load($Path)
        return ($null -ne $doc.DocumentElement)
    } catch {
        return $false
    }
}

function Wait-StableXmlFile {
    <#
      Bounded wait for a save to settle. For the native-save path the caller
      passes -PreSignature (the document signature captured before Ctrl+S),
      -SaveStartTicks and -RequireFresh, and the wait then accepts a state ONLY
      when the file is demonstrably freshly written relative to that pre-save
      signature/time AND its sha256 is unchanged across QuietPolls consecutive
      polls AND it is XML-loadable. An old, already-quiet file is therefore
      never returned as stable; the wait keeps polling until the (possibly
      asynchronous) save lands or the original deadline expires. True timeout is
      returned as stable=false; callers fail closed and never blind-retry.
      Without -RequireFresh it is a pure quiet-XML wait.
    #>
    param(
        [string]$Path,
        [int]$TimeoutMs = 8000,
        [int]$QuietPolls = 3,
        [int]$PollMs = 100,
        [System.Collections.IDictionary]$PreSignature = $null,
        [long]$SaveStartTicks = 0,
        [bool]$RequireFresh = $false
    )
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    $prevHash = $null
    $lastSig = $null
    $same = 0
    while ([DateTime]::UtcNow -lt $deadline) {
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            try {
                $sig = Get-FileSignature -Path $Path
                $fresh = $true
                if ($RequireFresh) {
                    $fresh = $false
                    if ($null -ne $PreSignature) {
                        $fresh = (($sig['sha256'] -ne [string]$PreSignature['sha256']) -or
                                  ([long]$sig['length'] -ne [long]$PreSignature['length']) -or
                                  ([long]$sig['mtime_ticks'] -ge $SaveStartTicks))
                    }
                }
                if ($fresh) {
                    if ($null -ne $prevHash -and $sig['sha256'] -eq $prevHash) { $same++ } else { $same = 0 }
                    if ($same -ge $QuietPolls -and (Test-XmlFileLoadable -Path $Path)) {
                        return [ordered]@{ stable = $true; fresh = $true; signature = $sig; polls_waited = $same }
                    }
                } else {
                    # Old unchanged content: reset the quiet counter and keep
                    # waiting inside the same bounded deadline for the save.
                    $same = 0
                }
                $prevHash = $sig['sha256']
                $lastSig = $sig
            } catch {
                $same = 0
            }
        }
        Start-Sleep -Milliseconds $PollMs
    }
    return [ordered]@{ stable = $false; fresh = $false; signature = $lastSig; polls_waited = $same }
}

function Add-AuxPanEvent {
    param(
        [System.Collections.ArrayList]$Sink,
        [int]$Loop, [string]$Phase,
        [int]$DesiredX, [int]$DesiredY,
        [int]$ActualX, [int]$ActualY,
        [int]$ActualScreenX, [int]$ActualScreenY,
        [int]$Return,
        [bool]$ForegroundOk,
        [long]$Qpc, [string]$Utc
    )
    [void]$Sink.Add([ordered]@{
        index            = $Sink.Count
        loop             = $Loop
        phase            = $Phase
        desired_client   = [ordered]@{ x = $DesiredX; y = $DesiredY }
        actual_client    = [ordered]@{ x = $ActualX; y = $ActualY }
        actual_screen    = [ordered]@{ x = $ActualScreenX; y = $ActualScreenY }
        sendinput_return = $Return
        foreground_ok    = $ForegroundOk
        qpc              = $Qpc
        utc              = $Utc
    })
}

function Wait-UntilQpc {
    param([long]$Target, [long]$Frequency)
    if ($Frequency -le 0) { return }
    while ($true) {
        $now = [VAPan.Win32]::Qpc()
        if ($now -ge $Target) { return }
        $remainMs = (($Target - $now) * 1000.0) / [double]$Frequency
        if ($remainMs -gt 2.0) { [System.Threading.Thread]::Sleep(1) } else { [System.Threading.Thread]::SpinWait(50) }
    }
}

function Send-AuxMouseEvent {
    # Same ownership/foreground gates and SendInput marshalling as the measured
    # Emit-Event, but records into a caller-owned sink and never touches the
    # measured counters.
    param(
        [IntPtr]$WindowHandle,
        [System.Collections.ArrayList]$Sink,
        [string]$Phase, [int]$Loop,
        [int]$ClientX, [int]$ClientY,
        [bool]$IsButton, [uint32]$Flags
    )
    $qpc = [VAPan.Win32]::Qpc()
    $utc = [DateTime]::UtcNow.ToString('o')
    if ($script:process -and $script:process.HasExited) {
        Add-AuxPanEvent -Sink $Sink -Loop $Loop -Phase "$Phase-lost" -DesiredX $ClientX -DesiredY $ClientY -ActualX -1 -ActualY -1 -ActualScreenX -1 -ActualScreenY -1 -Return -1 -ForegroundOk $false -Qpc $qpc -Utc $utc
        Throw-PanFailure 5 "target process exited before aux event (phase=$Phase loop=$Loop)"
    }
    if (-not [VAPan.Win32]::ForegroundIs($WindowHandle) -or -not [VAPan.Win32]::Visible($WindowHandle)) {
        Add-AuxPanEvent -Sink $Sink -Loop $Loop -Phase "$Phase-lost" -DesiredX $ClientX -DesiredY $ClientY -ActualX -1 -ActualY -1 -ActualScreenX -1 -ActualScreenY -1 -Return -1 -ForegroundOk $false -Qpc $qpc -Utc $utc
        Throw-PanFailure 5 "foreground/visibility lost before aux event (phase=$Phase loop=$Loop)"
    }
    $rc = 0
    if ($IsButton) {
        $rc = [int][VAPan.Win32]::SendMouse($Flags, 0, 0)
    } else {
        $scr = [VAPan.Win32]::ClientToScreenPoint($WindowHandle, $ClientX, $ClientY)
        $pixelCenter = ((Get-Variable -Name 'maximizedMode' -Scope Script -ValueOnly -ErrorAction SilentlyContinue) -eq $true)
        $abs = [VAPan.Win32]::ScreenToAbsoluteForMode($scr[0], $scr[1], $pixelCenter)
        if ($null -eq $abs) { Throw-PanFailure 6 "point $($scr[0]),$($scr[1]) cannot be represented in the virtual desktop at aux phase=$Phase loop=$Loop" }
        $rc = [int][VAPan.Win32]::SendMouse($Flags, $abs[0], $abs[1])
    }
    $cur = [VAPan.Win32]::CursorClient($WindowHandle)
    Add-AuxPanEvent -Sink $Sink -Loop $Loop -Phase $Phase -DesiredX $ClientX -DesiredY $ClientY -ActualX $cur[0] -ActualY $cur[1] -ActualScreenX $cur[2] -ActualScreenY $cur[3] -Return $rc -ForegroundOk $true -Qpc $qpc -Utc $utc
    if ($rc -ne 1) { Throw-PanFailure 6 "SendInput returned $rc (expected 1) at aux phase=$Phase loop=$Loop" }
    return [ordered]@{ return = $rc; client = @($cur[0], $cur[1]); screen = @($cur[2], $cur[3]) }
}

function Get-PointerAdmission {
    <#
      Pure pointer-admission proof shared by the selection click and both drag
      arms. In maximized calibrated mode the system-aware window and mixed-DPI
      ClientToScreen/ScreenToClient round trip is not pixel-bijective, so the
      client readback can differ by one pixel while the injected absolute move
      landed on the exact intended physical pixel. Admission therefore compares
      the ACTUAL physical GetCursorPos coordinates (CursorClient indices 2/3)
      with the exact intended ClientToScreenPoint physical point, derived only
      from the window/calibration, never from an observed cursor. Legacy
      normal-client mode keeps the byte-for-byte client comparison. There is no
      tolerance: a one-physical-pixel miss fails closed.

      Arity is part of admission: a missing cursor, or a cursor shorter than the
      four CursorClient fields (client x/y, screen x/y), must NOT be coerced to
      (0,0) and admitted. Physical admission requires >= 4 fields; legacy client
      admission requires >= 2; diagnostics stay null rather than invented.
    #>
    param(
        [int[]]$Cursor,
        [int]$ClientX,
        [int]$ClientY,
        [int[]]$ExpectedScreen,
        [bool]$MaximizedMode = $false
    )
    $hasClient = ($null -ne $Cursor -and $Cursor.Count -ge 2)
    $hasPhysical = ($null -ne $Cursor -and $Cursor.Count -ge 4)
    $actualClient = $null
    $actualScreen = $null
    if ($hasClient) { $actualClient = @([int]$Cursor[0], [int]$Cursor[1]) }
    if ($hasPhysical) { $actualScreen = @([int]$Cursor[2], [int]$Cursor[3]) }
    $expectedClient = @([int]$ClientX, [int]$ClientY)
    $clientMatch = $false
    if ($hasClient) { $clientMatch = (($actualClient[0] -eq $ClientX) -and ($actualClient[1] -eq $ClientY)) }
    $expScreen = $null
    $screenMatch = $false
    if ($null -ne $ExpectedScreen -and $ExpectedScreen.Count -ge 2) {
        $expScreen = @([int]$ExpectedScreen[0], [int]$ExpectedScreen[1])
        if ($hasPhysical) {
            $screenMatch = (($actualScreen[0] -eq $expScreen[0]) -and ($actualScreen[1] -eq $expScreen[1]))
        }
    }
    if ($MaximizedMode) {
        $admitted = ($hasPhysical -and $screenMatch)
    } else {
        $admitted = ($hasClient -and $clientMatch)
    }
    return [ordered]@{
        coordinate_space = if ($MaximizedMode) { 'physical-screen' } else { 'client' }
        expected_client  = $expectedClient
        actual_client    = $actualClient
        client_match     = $clientMatch
        expected_screen  = $expScreen
        actual_screen    = $actualScreen
        screen_match     = $screenMatch
        admitted         = $admitted
    }
}

function Invoke-DragArm {
    <#
      Shared one-shot arming step for the object-drag workload, OUTSIDE the
      measured window.

      The native selector anchors a live-content drag on the FIRST accepted
      button1 motion after the press (select-tool.cpp:562-657 calls
      _seltrans->grab(p) and then moveTo(p)), not on the press itself. This
      helper therefore presses at PressX (offset from the intended anchor) and
      sends exactly ONE motion to AnchorX, so the anchor is the exact fixed
      point regardless of where the press landed. It records position/down/
      anchor as three separate auxiliary events (QPC + cursor) plus the
      requested/observed settle wait. The bounded wait is only a settling
      assumption; it is never treated as proof that the app consumed the arm.

      On success the left button is left held for the caller
      ($script:leftDown = $true) so the measured window is one continuous drag
      with a single release; any arm error is covered by the caller's
      unconditional finally left-button release. This helper never touches the
      measured event counters.

      The immediate post-anchor cursor readback is telemetry only (the pointer
      may not have caught up yet); after the bounded settle the actual cursor
      MUST equal the fixed anchor, otherwise the arm fails closed before the
      measured window.
    #>
    param(
        [IntPtr]$WindowHandle,
        [int]$PressX,
        [int]$AnchorX,
        [int]$Y,
        [int]$WaitMs,
        [int]$ClientW,
        [int]$ClientH,
        [long]$QpcFrequency,
        [System.Collections.IDictionary]$Summary,
        [string]$Slot,
        [scriptblock]$ForegroundCheck = $null,
        [scriptblock]$CursorReader = $null,
        [scriptblock]$ScreenPointResolver = $null,
        [bool]$MaximizedMode = $false,
        $Calibration = $null,
        [scriptblock]$GeometryCheck = $null
    )
    $res = [ordered]@{
        schema                  = 'vacards-app-pan-object-drag-arm/1'
        attempted               = $true
        status                  = 'init'
        reason                  = $null
        press_client            = @($PressX, $Y)
        anchor_client           = @($AnchorX, $Y)
        modifiers_held          = @()
        requested_wait_ms       = $null
        observed_wait_ms        = $null
        start_qpc               = $null
        start_utc               = $null
        end_qpc                 = $null
        end_utc                 = $null
        foreground_ok           = $false
        cursor_before_anchor    = $null
        anchor_cursor_match     = $false
        anchor_coordinate_space = $null
        anchor_expected_screen  = $null
        anchor_actual_screen    = $null
        anchor_screen_match     = $null
        anchor_client_match     = $null
        cursor_after_wait       = $null
        cursor_after_wait_screen = $null
        anchor_after_wait_match = $null
        anchor_after_wait_screen_match = $null
        foreground_ok_after_wait = $null
        client_after_wait       = $null
        bounds_after_wait       = $null
        in_monitor_after_wait   = $null
        exact_client_after_wait = $null
        normal_state_after_wait = $null
        maximized_state_after_wait = $null
        maximized_geometry_after_wait = $null
        accepted_event_count    = 0
        events                  = (New-Object System.Collections.ArrayList)
    }
    if ($null -ne $Summary) { $Summary[$Slot] = $res }
    $fgCheck = if ($null -ne $ForegroundCheck) { $ForegroundCheck } else { { param($h) [VAPan.Win32]::ForegroundIs($h) } }
    $curReader = if ($null -ne $CursorReader) { $CursorReader } else { { param($h) [VAPan.Win32]::CursorClient($h) } }
    $screenResolver = if ($null -ne $ScreenPointResolver) { $ScreenPointResolver } else { { param($h, $cx, $cy) [VAPan.Win32]::ClientToScreenPoint($h, $cx, $cy) } }
    $held = @([VAPan.Win32]::HeldModifiers())
    if ($held.Count -gt 0) { Start-Sleep -Milliseconds 200; $held = @([VAPan.Win32]::HeldModifiers()) }
    $res['modifiers_held'] = $held
    if ($held.Count -gt 0) {
        $res['status'] = 'rejected'
        $res['reason'] = 'modifier key(s) already held before drag arm; refusing to inject'
        Throw-PanFailure 6 $res['reason']
    }
    if (-not [bool](& $fgCheck $WindowHandle)) {
        $res['status'] = 'rejected'
        $res['reason'] = 'owned HWND is not foreground before drag arm'
        Throw-PanFailure 5 $res['reason']
    }
    $res['foreground_ok'] = $true

    # Press slightly off the anchor, then ONE motion to the exact anchor. This
    # motion is the anchor the native selector locks onto; the pressed button is
    # tracked so a later arm/capture failure is still released by finally.
    Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'position' -Loop 0 -ClientX $PressX -ClientY $Y -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS) | Out-Null
    $dn = Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'down' -Loop 0 -ClientX $PressX -ClientY $Y -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTDOWN)
    if ($dn['return'] -eq 1) { $script:leftDown = $true }
    $curBefore = @(& $curReader $WindowHandle)
    $res['cursor_before_anchor'] = @($curBefore[0], $curBefore[1])
    Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'anchor' -Loop 0 -ClientX $AnchorX -ClientY $Y -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS) | Out-Null
    $cur = @(& $curReader $WindowHandle)
    # In maximized calibrated mode admission uses the exact physical screen
    # point derived from ClientToScreen (never the observed cursor); the client
    # readback is retained for diagnostics only. The legacy normal-client path
    # keeps the byte-for-byte client comparison. This immediate readback remains
    # telemetry only (the pointer may not have caught up yet); the settled check
    # below is the hard gate.
    $anchorExpScreen = @(& $screenResolver $WindowHandle $AnchorX $Y)
    $admImmediate = Get-PointerAdmission -Cursor $cur -ClientX $AnchorX -ClientY $Y -ExpectedScreen $anchorExpScreen -MaximizedMode $MaximizedMode
    $res['anchor_coordinate_space'] = $admImmediate['coordinate_space']
    $res['anchor_expected_screen'] = $admImmediate['expected_screen']
    $res['anchor_actual_screen'] = $admImmediate['actual_screen']
    $res['anchor_screen_match'] = $admImmediate['screen_match']
    $res['anchor_client_match'] = $admImmediate['client_match']
    $res['anchor_cursor_match'] = $admImmediate['admitted']
    $script:lastClientX = $AnchorX
    $script:lastClientY = $Y

    $res['requested_wait_ms'] = $WaitMs
    $startQpc = [VAPan.Win32]::Qpc()
    $res['start_qpc'] = $startQpc
    $res['start_utc'] = [DateTime]::UtcNow.ToString('o')
    Wait-UntilQpc -Target ([long]($startQpc + [long]($QpcFrequency * $WaitMs / 1000.0))) -Frequency $QpcFrequency
    $endQpc = [VAPan.Win32]::Qpc()
    $res['end_qpc'] = $endQpc
    $res['end_utc'] = [DateTime]::UtcNow.ToString('o')
    $res['observed_wait_ms'] = [Math]::Round((([double]$endQpc - [double]$startQpc) * 1000.0) / [double]$QpcFrequency, 3)

    # After the settle the actual cursor MUST be on the fixed anchor. A mismatch
    # is a hard failure, never accepted telemetry: a mis-landed pointer means the
    # measured drag would not start from the intended point.
    $curAfterWait = @(& $curReader $WindowHandle)
    $res['cursor_after_wait'] = @($curAfterWait[0], $curAfterWait[1])
    $res['cursor_after_wait_screen'] = @($curAfterWait[2], $curAfterWait[3])
    $anchorExpScreenSettled = @(& $screenResolver $WindowHandle $AnchorX $Y)
    $admSettled = Get-PointerAdmission -Cursor $curAfterWait -ClientX $AnchorX -ClientY $Y -ExpectedScreen $anchorExpScreenSettled -MaximizedMode $MaximizedMode
    $res['anchor_after_wait_screen_match'] = $admSettled['screen_match']
    $res['anchor_after_wait_match'] = $admSettled['admitted']
    if (-not $res['anchor_after_wait_match']) {
        $res['status'] = 'failed'
        if ($MaximizedMode) {
            $res['reason'] = ('cursor did not settle on the exact physical anchor after the arm wait (expected ' + [string]$admSettled['expected_screen'][0] + ',' + [string]$admSettled['expected_screen'][1] + '; got ' + [string]$curAfterWait[2] + ',' + [string]$curAfterWait[3] + ')')
        } else {
            $res['reason'] = ('cursor did not settle on the fixed anchor after the arm wait (expected ' + $AnchorX + ',' + $Y + '; got ' + $curAfterWait[0] + ',' + $curAfterWait[1] + ')')
        }
        Throw-PanFailure 6 $res['reason']
    }

    # Re-verify the owned foreground and geometry before the measured window.
    # Emit-Event keeps its own per-event guards. In maximized-workarea mode the
    # strict check is the calibrated maximized proof; the legacy normal-state,
    # exact-1280x800 and rcMonitor-containment checks apply only by default.
    $fg = [bool](& $fgCheck $WindowHandle)
    $res['foreground_ok_after_wait'] = $fg
    if ($MaximizedMode) {
        $geo = if ($null -ne $GeometryCheck) { & $GeometryCheck $WindowHandle } else { Test-MaximizedGeometryState -WindowHandle $WindowHandle -Calibration $Calibration }
        $res['maximized_geometry_after_wait'] = $geo
        $res['maximized_state_after_wait'] = [bool]$geo['is_zoomed']
        $res['client_after_wait'] = @($geo['client'])
        $res['bounds_after_wait'] = @($geo['bounds'])
        $res['in_monitor_after_wait'] = [bool]$geo['coverage_ok']
        $res['exact_client_after_wait'] = [bool]$geo['exact_client']
        $res['normal_state_after_wait'] = $false
        if (-not $fg) {
            $res['status'] = 'failed'
            $res['reason'] = 'owned HWND lost foreground during the drag arm wait'
            Throw-PanFailure 5 $res['reason']
        }
        if (-not [bool]$geo['ok']) {
            $res['status'] = 'failed'
            $res['reason'] = ('maximized geometry changed during the drag arm wait: ' + [string]$geo['reason'])
            Throw-PanFailure 4 $res['reason']
        }
    } else {
        $cs = [VAPan.Win32]::ClientSize($WindowHandle)
        $wb = [VAPan.Win32]::WindowBounds($WindowHandle)
        $mon = [VAPan.Win32]::MonitorRects($WindowHandle)
        $inMon = $false
        if ($null -ne $mon) {
            $inMon = ($wb[0] -ge $mon[0]) -and ($wb[1] -ge $mon[1]) -and ($wb[2] -le $mon[2]) -and ($wb[3] -le $mon[3])
        }
        $exact = (($cs[0] -eq $ClientW) -and ($cs[1] -eq $ClientH))
        $normal = (-not [VAPan.Win32]::Minimized($WindowHandle)) -and (-not [VAPan.Win32]::Maximized($WindowHandle))
        $res['client_after_wait'] = @($cs[0], $cs[1])
        $res['bounds_after_wait'] = @($wb[0], $wb[1], $wb[2], $wb[3])
        $res['in_monitor_after_wait'] = $inMon
        $res['exact_client_after_wait'] = $exact
        $res['normal_state_after_wait'] = $normal
        if (-not $fg) {
            $res['status'] = 'failed'
            $res['reason'] = 'owned HWND lost foreground during the drag arm wait'
            Throw-PanFailure 5 $res['reason']
        }
        if (-not $normal -or -not $exact -or -not $inMon) {
            $res['status'] = 'failed'
            $res['reason'] = 'owned HWND geometry changed during the drag arm wait'
            Throw-PanFailure 4 $res['reason']
        }
    }
    $res['accepted_event_count'] = @($res['events'] | Where-Object { $_.sendinput_return -eq 1 }).Count
    $res['status'] = 'ok'
    return $res
}

function Invoke-CtrlSKeySequence {
    <#
      The four-key Ctrl+S chord, injectable so the failure path is testable
      without a GUI. Before EVERY key injection the foreground predicate is
      re-evaluated; a loss aborts before that key is sent, so no later key can
      be pressed after focus moved away. Ctrl-down and S-down are tracked in the
      script-held flags so the caller's finally releases exactly the keys that
      were actually injected (a failed SendInput never marks a key held).
    #>
    param(
        [IntPtr]$WindowHandle,
        [System.Collections.ArrayList]$KeyEvents,
        [System.Collections.IDictionary]$Result,
        [scriptblock]$ForegroundCheck,
        [scriptblock]$KeySender
    )
    $steps = @(
        [ordered]@{ vk = [int][VAPan.Win32]::VK_CONTROL; up = $false; label = 'ctrl-down' },
        [ordered]@{ vk = [int][VAPan.Win32]::VK_S; up = $false; label = 's-down' },
        [ordered]@{ vk = [int][VAPan.Win32]::VK_S; up = $true; label = 's-up' },
        [ordered]@{ vk = [int][VAPan.Win32]::VK_CONTROL; up = $true; label = 'ctrl-up' }
    )
    $accepted = 0
    foreach ($st in $steps) {
        $fg = [bool](& $ForegroundCheck $WindowHandle)
        if (-not $fg) {
            $Result['foreground_ok'] = $false
            $Result['status'] = 'failed'
            $Result['reason'] = ('owned HWND lost foreground before ' + $st['label'] + '; no further key injected')
            Throw-PanFailure 5 $Result['reason']
        }
        $q = [VAPan.Win32]::Qpc()
        $u = [DateTime]::UtcNow.ToString('o')
        $rc = [int](& $KeySender ([int]$st['vk']) ([bool]$st['up']))
        if ($st['label'] -eq 'ctrl-down' -and $rc -eq 1) { $script:ctrlDown = $true }
        if ($st['label'] -eq 'ctrl-up' -and $rc -eq 1) { $script:ctrlDown = $false }
        if ($st['label'] -eq 's-down' -and $rc -eq 1) { $script:sDown = $true }
        if ($st['label'] -eq 's-up' -and $rc -eq 1) { $script:sDown = $false }
        Add-AuxPanEvent -Sink $KeyEvents -Loop 0 -Phase $st['label'] -DesiredX 0 -DesiredY 0 -ActualX 0 -ActualY 0 -ActualScreenX 0 -ActualScreenY 0 -Return $rc -ForegroundOk $true -Qpc $q -Utc $u
        if ($rc -eq 1) { $accepted++ }
        if ($rc -ne 1) {
            $Result['status'] = 'failed'
            $Result['reason'] = ('SendInput keyboard returned ' + $rc + ' at ' + $st['label'])
            Throw-PanFailure 6 $Result['reason']
        }
    }
    $Result['accepted_event_count'] = $accepted
    return $accepted
}

function Invoke-SelectClick {
    # ONE verified native left select-click at a fixed client point, outside the
    # measured window. Rejects any physically held modifier first. The click is
    # counted only when move+down+up were accepted and the pointer arrived. In
    # maximized calibrated mode arrival is the exact physical screen point
    # (compared against the ClientToScreenPoint-derived expectation); the legacy
    # normal-client path keeps the byte-for-byte client comparison.
    param(
        [IntPtr]$WindowHandle,
        [int]$ClientX,
        [int]$ClientY,
        [System.Collections.IDictionary]$Summary,
        [string]$Slot,
        [bool]$MaximizedMode = $false,
        [scriptblock]$CursorReader = $null,
        [scriptblock]$ScreenPointResolver = $null,
        [scriptblock]$ForegroundCheck = $null
    )
    $res = [ordered]@{
        schema               = 'vacards-app-pan-object-drag-prep/1'
        attempted            = $true
        client_point         = @($ClientX, $ClientY)
        modifiers_held       = @()
        status               = 'init'
        reason               = $null
        move_return          = $null
        cursor_before_press  = $null
        cursor_at_point      = $null
        coordinate_space     = $null
        expected_client      = $null
        actual_client        = $null
        client_match         = $null
        expected_screen      = $null
        actual_screen        = $null
        screen_match         = $null
        down_return          = $null
        up_return            = $null
        accepted_event_count = 0
        click_delivered      = $false
        foreground_ok        = $false
        events               = (New-Object System.Collections.ArrayList)
    }
    if ($null -ne $Summary) { $Summary[$Slot] = $res }
    $fgCheck = if ($null -ne $ForegroundCheck) { $ForegroundCheck } else { { param($h) [VAPan.Win32]::ForegroundIs($h) } }
    $curReader = if ($null -ne $CursorReader) { $CursorReader } else { { param($h) [VAPan.Win32]::CursorClient($h) } }
    $screenResolver = if ($null -ne $ScreenPointResolver) { $ScreenPointResolver } else { { param($h, $cx, $cy) [VAPan.Win32]::ClientToScreenPoint($h, $cx, $cy) } }
    $held = @([VAPan.Win32]::HeldModifiers())
    if ($held.Count -gt 0) { Start-Sleep -Milliseconds 200; $held = @([VAPan.Win32]::HeldModifiers()) }
    $res['modifiers_held'] = $held
    if ($held.Count -gt 0) {
        $res['status'] = 'rejected'
        $res['reason'] = 'modifier key(s) already held before selection click; refusing to inject'
        Throw-PanFailure 6 $res['reason']
    }
    if (-not [bool](& $fgCheck $WindowHandle)) {
        $res['status'] = 'rejected'
        $res['reason'] = 'owned HWND is not foreground before selection click'
        Throw-PanFailure 5 $res['reason']
    }
    $res['foreground_ok'] = $true
    $mv = Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'position' -Loop 0 -ClientX $ClientX -ClientY $ClientY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
    $res['move_return'] = $mv['return']
    $cur = @(& $curReader $WindowHandle)
    $res['cursor_before_press'] = @($cur[0], $cur[1])
    $selectExpScreen = @(& $screenResolver $WindowHandle $ClientX $ClientY)
    $admSelect = Get-PointerAdmission -Cursor $cur -ClientX $ClientX -ClientY $ClientY -ExpectedScreen $selectExpScreen -MaximizedMode $MaximizedMode
    $res['coordinate_space'] = $admSelect['coordinate_space']
    $res['expected_client'] = $admSelect['expected_client']
    $res['actual_client'] = $admSelect['actual_client']
    $res['client_match'] = $admSelect['client_match']
    $res['expected_screen'] = $admSelect['expected_screen']
    $res['actual_screen'] = $admSelect['actual_screen']
    $res['screen_match'] = $admSelect['screen_match']
    $res['cursor_at_point'] = $admSelect['admitted']
    if (-not $res['cursor_at_point']) {
        $res['status'] = 'failed'
        $res['reason'] = 'pointer did not arrive at the selection point'
        Throw-PanFailure 6 $res['reason']
    }
    $dn = Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'down' -Loop 0 -ClientX $ClientX -ClientY $ClientY -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTDOWN)
    if ($dn['return'] -eq 1) { $script:leftDown = $true }
    $res['down_return'] = $dn['return']
    $up = Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'up' -Loop 0 -ClientX $ClientX -ClientY $ClientY -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP)
    if ($up['return'] -eq 1) { $script:leftDown = $false }
    $res['up_return'] = $up['return']
    $res['click_delivered'] = (($res['move_return'] -eq 1) -and ($res['down_return'] -eq 1) -and ($res['up_return'] -eq 1) -and $res['cursor_at_point'])
    $res['accepted_event_count'] = @($res['events'] | Where-Object { $_.sendinput_return -eq 1 }).Count
    if (-not $res['click_delivered']) {
        $res['status'] = 'failed'
        $res['reason'] = 'selection click not fully delivered'
        Throw-PanFailure 6 $res['reason']
    }
    $res['status'] = 'ok'
    return $res
}

function Invoke-ClickSpacingWait {
    <#
      Pre-measurement click-spacing guard for Workload=object-drag, inserted
      AFTER the selection click and OUTSIDE the measured QPC/CPU input window.

      The native selector treats two clicks inside GetDoubleClickTime() as a
      double-click and switches to the rectangle tool (select-tool.cpp:483-495),
      which would corrupt the arm press that follows. Screenshot overhead is
      not a reliable wait, so this reads the system setting (never changes it;
      no app settings, no keyboard action) and waits a monotonic-QPC bound:
        required_wait_ms = max(1000, system_double_click_ms + 100), capped 6000.
      Under the continuous-v2 protocol there is only ONE measured press (the
      single drag arm), so the former repeated-loop press-overlap rejection is
      inapplicable and has been removed; the bounded spacing wait alone keeps
      that arm press out of the selection click's double-click window. It
      records the observed setting, the required wait and its own start/end QPC
      + UTC separately, then re-verifies the owned foreground and the exact
      client/monitor/normal-state geometry before the measured window; the
      per-event Emit-Event guards remain in force afterwards.
    #>
    param(
        [IntPtr]$WindowHandle,
        [int]$ClientW,
        [int]$ClientH,
        [System.Collections.IDictionary]$Summary,
        [string]$Slot,
        [bool]$MaximizedMode = $false,
        $Calibration = $null,
        [scriptblock]$GeometryCheck = $null
    )
    $res = [ordered]@{
        schema                      = 'vacards-app-pan-click-spacing/1'
        attempted                   = $true
        status                      = 'init'
        reason                      = $null
        system_double_click_time_ms = $null
        required_wait_ms            = $null
        bounded_max_ms              = 6000
        start_qpc                   = $null
        start_utc                   = $null
        end_qpc                     = $null
        end_utc                     = $null
        observed_wait_ms            = $null
        foreground_ok_after_wait    = $null
        client_after_wait           = $null
        bounds_after_wait           = $null
        in_monitor_after_wait       = $null
        exact_client_after_wait     = $null
        normal_state_after_wait     = $null
        maximized_state_after_wait  = $null
        maximized_geometry_after_wait = $null
    }
    if ($null -ne $Summary) { $Summary[$Slot] = $res }

    $dct = [int][VAPan.Win32]::GetDoubleClickTime()
    $res['system_double_click_time_ms'] = $dct
    $waitMs = [Math]::Max(1000, $dct + 100)
    if ($waitMs -gt 6000) { $waitMs = 6000 }
    $res['required_wait_ms'] = $waitMs

    $freq = $script:qpcFreq
    $startQpc = [VAPan.Win32]::Qpc()
    $res['start_qpc'] = $startQpc
    $res['start_utc'] = [DateTime]::UtcNow.ToString('o')
    Wait-UntilQpc -Target ([long]($startQpc + [long]($freq * $waitMs / 1000.0))) -Frequency $freq
    $endQpc = [VAPan.Win32]::Qpc()
    $res['end_qpc'] = $endQpc
    $res['end_utc'] = [DateTime]::UtcNow.ToString('o')
    $res['observed_wait_ms'] = [Math]::Round((([double]$endQpc - [double]$startQpc) * 1000.0) / [double]$freq, 3)

    # Re-verify the owned foreground and geometry before the measured window.
    # Emit-Event keeps its own per-event guards. Maximized mode uses the strict
    # calibrated maximized proof instead of the legacy normal/exact/containment
    # checks; the default path is unchanged.
    $fg = [bool][VAPan.Win32]::ForegroundIs($WindowHandle)
    $res['foreground_ok_after_wait'] = $fg
    if ($MaximizedMode) {
        $geo = if ($null -ne $GeometryCheck) { & $GeometryCheck $WindowHandle } else { Test-MaximizedGeometryState -WindowHandle $WindowHandle -Calibration $Calibration }
        $res['maximized_geometry_after_wait'] = $geo
        $res['maximized_state_after_wait'] = [bool]$geo['is_zoomed']
        $res['client_after_wait'] = @($geo['client'])
        $res['bounds_after_wait'] = @($geo['bounds'])
        $res['in_monitor_after_wait'] = [bool]$geo['coverage_ok']
        $res['exact_client_after_wait'] = [bool]$geo['exact_client']
        $res['normal_state_after_wait'] = $false
        if (-not $fg) {
            $res['status'] = 'failed'
            $res['reason'] = 'owned HWND lost foreground during the click-spacing wait'
            Throw-PanFailure 5 $res['reason']
        }
        if (-not [bool]$geo['ok']) {
            $res['status'] = 'failed'
            $res['reason'] = ('maximized geometry changed during the click-spacing wait: ' + [string]$geo['reason'])
            Throw-PanFailure 4 $res['reason']
        }
    } else {
        $cs = [VAPan.Win32]::ClientSize($WindowHandle)
        $wb = [VAPan.Win32]::WindowBounds($WindowHandle)
        $mon = [VAPan.Win32]::MonitorRects($WindowHandle)
        $inMon = $false
        if ($null -ne $mon) {
            $inMon = ($wb[0] -ge $mon[0]) -and ($wb[1] -ge $mon[1]) -and ($wb[2] -le $mon[2]) -and ($wb[3] -le $mon[3])
        }
        $exact = (($cs[0] -eq $ClientW) -and ($cs[1] -eq $ClientH))
        $normal = (-not [VAPan.Win32]::Minimized($WindowHandle)) -and (-not [VAPan.Win32]::Maximized($WindowHandle))
        $res['client_after_wait'] = @($cs[0], $cs[1])
        $res['bounds_after_wait'] = @($wb[0], $wb[1], $wb[2], $wb[3])
        $res['in_monitor_after_wait'] = $inMon
        $res['exact_client_after_wait'] = $exact
        $res['normal_state_after_wait'] = $normal
        if (-not $fg) {
            $res['status'] = 'failed'
            $res['reason'] = 'owned HWND lost foreground during the click-spacing wait'
            Throw-PanFailure 5 $res['reason']
        }
        if (-not $normal -or -not $exact -or -not $inMon) {
            $res['status'] = 'failed'
            $res['reason'] = 'owned HWND geometry changed during the click-spacing wait'
            Throw-PanFailure 4 $res['reason']
        }
    }
    $res['status'] = 'ok'
    return $res
}

function Invoke-SaveDocumentSnapshot {
    <#
      Native Ctrl+S of the owned foreground app, then a bounded
      stable+XML-loadable wait, a freshness check against the pre-save
      signature, and a hash-verified copy into the pan output directory.
      Records the four keyboard events in its own key_events sink. Fails closed
      on a held modifier, lost foreground, non-fresh save, or bad copy.
    #>
    param(
        [IntPtr]$WindowHandle,
        [string]$DocumentPath,
        [string]$SnapshotName,
        [string]$SnapshotDirectory,
        [System.Collections.IDictionary]$Summary,
        [string]$Slot
    )
    $res = [ordered]@{
        schema               = 'vacards-app-pan-native-save/1'
        snapshot_name        = $SnapshotName
        snapshot_path        = (Join-Path -Path $SnapshotDirectory -ChildPath $SnapshotName)
        attempted            = $true
        status               = 'init'
        reason               = $null
        modifiers_held       = @()
        foreground_ok        = $false
        pre_signature        = $null
        post_signature       = $null
        stable               = $false
        xml_loadable         = $false
        fresh                = $false
        copied               = $false
        copy_sha256_matches  = $false
        accepted_event_count = 0
        key_events           = (New-Object System.Collections.ArrayList)
    }
    if ($null -ne $Summary) { $Summary[$Slot] = $res }
    $held = @([VAPan.Win32]::HeldModifiers())
    if ($held.Count -gt 0) { Start-Sleep -Milliseconds 200; $held = @([VAPan.Win32]::HeldModifiers()) }
    $res['modifiers_held'] = $held
    if ($held.Count -gt 0) {
        $res['status'] = 'rejected'
        $res['reason'] = 'modifier key(s) already held before Ctrl+S; refusing to inject'
        Throw-PanFailure 6 $res['reason']
    }
    if (-not [VAPan.Win32]::ForegroundIs($WindowHandle)) {
        $res['status'] = 'rejected'
        $res['reason'] = 'owned HWND is not foreground before Ctrl+S'
        Throw-PanFailure 5 $res['reason']
    }
    $res['foreground_ok'] = $true
    if (-not (Test-Path -LiteralPath $DocumentPath -PathType Leaf)) {
        $res['status'] = 'failed'
        $res['reason'] = 'DocumentPath is not an existing file at save time'
        Throw-PanFailure 7 $res['reason']
    }
    $pre = Get-FileSignature -Path $DocumentPath
    $res['pre_signature'] = $pre
    $saveStartTicks = [DateTime]::UtcNow.Ticks
    $res['save_start_utc'] = [DateTime]::UtcNow.ToString('o')
    # Foreground is re-checked before each single key; the default predicates use
    # the real owned window and the real SendInput keyboard path.
    $fgCheck = { param($h) [VAPan.Win32]::ForegroundIs($h) }
    $keySender = { param($vk, $up) [int][VAPan.Win32]::SendKey([uint16]$vk, [bool]$up) }
    Invoke-CtrlSKeySequence -WindowHandle $WindowHandle -KeyEvents $res['key_events'] -Result $res -ForegroundCheck $fgCheck -KeySender $keySender | Out-Null
    # Bounded wait: the result must be BOTH freshly written relative to the
    # pre-save signature/time (so an old quiet snapshot cannot pass) AND quiet
    # stable XML. If the save never lands the wait times out and we fail closed.
    $wait = Wait-StableXmlFile -Path $DocumentPath -PreSignature $pre -SaveStartTicks $saveStartTicks -RequireFresh $true
    $res['stable'] = [bool]$wait['stable']
    $res['fresh'] = [bool]$wait['fresh']
    $res['xml_loadable'] = [bool](Test-XmlFileLoadable -Path $DocumentPath)
    if (-not $res['stable']) {
        $res['status'] = 'failed'
        $res['reason'] = 'native save was not freshly written and stable within the bound (stale or absent save)'
        Throw-PanFailure 7 $res['reason']
    }
    if (-not $res['xml_loadable']) {
        $res['status'] = 'failed'
        $res['reason'] = 'native save is not XML-loadable'
        Throw-PanFailure 7 $res['reason']
    }
    $post = $wait['signature']
    $res['post_signature'] = $post
    if (-not $res['fresh']) {
        $res['status'] = 'failed'
        $res['reason'] = 'stale output: no fresh native save observed'
        Throw-PanFailure 7 $res['reason']
    }
    $dest = Join-Path -Path $SnapshotDirectory -ChildPath $SnapshotName
    Copy-Item -LiteralPath $DocumentPath -Destination $dest -Force
    $res['copied'] = $true
    $res['snapshot_path'] = $dest
    $copySig = Get-FileSignature -Path $dest
    $res['copy_signature'] = $copySig
    $res['copy_sha256_matches'] = ($copySig['sha256'] -eq $post['sha256'])
    if (-not $res['copy_sha256_matches']) {
        $res['status'] = 'failed'
        $res['reason'] = 'snapshot copy hash does not match the saved document'
        Throw-PanFailure 7 $res['reason']
    }
    if (-not (Test-XmlFileLoadable -Path $dest)) {
        $res['status'] = 'failed'
        $res['reason'] = 'snapshot copy is not XML-loadable'
        Throw-PanFailure 7 $res['reason']
    }
    $res['status'] = 'ok'
    return $res
}

function Invoke-ReturnLeg {
    # Continuation of an already-armed native left-button return drag, outside
    # the measured window. The analogous arm (position 374 / down / ONE anchor
    # motion to 370 / bounded settle) is performed by Invoke-DragArm first and
    # leaves the button held, so this leg sends only the 60 out motions
    # 370->250 and ONE release. Records into its own return events sink; the
    # auxiliary return count is the arm's 3 events plus these 61.
    param(
        [IntPtr]$WindowHandle,
        [int]$FromX, [int]$FromY,
        [int]$ToX, [int]$ToY,
        [int]$Legs,
        [int]$StepIntervalMs,
        [long]$QpcFrequency,
        [System.Collections.IDictionary]$Summary,
        [string]$Slot
    )
    $res = [ordered]@{
        schema               = 'vacards-app-pan-object-drag-return/1'
        attempted            = $true
        status               = 'init'
        reason               = $null
        from_client          = @($FromX, $FromY)
        to_client            = @($ToX, $ToY)
        steps                = $Legs
        modifiers_held       = @()
        accepted_event_count = 0
        start_qpc            = $null
        start_utc            = $null
        end_qpc              = $null
        end_utc              = $null
        events               = (New-Object System.Collections.ArrayList)
    }
    if ($null -ne $Summary) { $Summary[$Slot] = $res }
    $held = @([VAPan.Win32]::HeldModifiers())
    if ($held.Count -gt 0) { Start-Sleep -Milliseconds 200; $held = @([VAPan.Win32]::HeldModifiers()) }
    $res['modifiers_held'] = $held
    if ($held.Count -gt 0) {
        $res['status'] = 'rejected'
        $res['reason'] = 'modifier key(s) already held before return leg; refusing to inject'
        Throw-PanFailure 6 $res['reason']
    }
    $res['start_qpc'] = [VAPan.Win32]::Qpc()
    $res['start_utc'] = [DateTime]::UtcNow.ToString('o')
    $legStart = [VAPan.Win32]::Qpc()
    for ($s = 1; $s -le $Legs; $s++) {
        Wait-UntilQpc -Target ([long]($legStart + [long]($QpcFrequency * ($StepIntervalMs * $s) / 1000.0))) -Frequency $QpcFrequency
        $cx = [VAPan.Win32]::StepCoord($FromX, $ToX, $s, $Legs)
        $cy = [VAPan.Win32]::StepCoord($FromY, $ToY, $s, $Legs)
        Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'out' -Loop 1 -ClientX $cx -ClientY $cy -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS) | Out-Null
        $script:lastClientX = $cx
        $script:lastClientY = $cy
    }
    $up = Send-AuxMouseEvent -WindowHandle $WindowHandle -Sink $res['events'] -Phase 'up' -Loop 1 -ClientX $ToX -ClientY $ToY -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP)
    if ($up['return'] -eq 1) { $script:leftDown = $false }
    $res['end_qpc'] = [VAPan.Win32]::Qpc()
    $res['end_utc'] = [DateTime]::UtcNow.ToString('o')
    $res['accepted_event_count'] = @($res['events'] | Where-Object { $_.sendinput_return -eq 1 }).Count
    $res['status'] = 'ok'
    return $res
}

# ---------------------------------------------------------------------------
# Ensure a fresh output directory
# ---------------------------------------------------------------------------
if (Test-Path -LiteralPath $OutputDirectory) {
    [Console]::Error.WriteLine("OutputDirectory already exists (must be a new path): $OutputDirectory")
    exit 2
}
try {
    $null = New-Item -ItemType Directory -Path $OutputDirectory -Force
    $OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
}
catch {
    [Console]::Error.WriteLine("cannot create OutputDirectory '$OutputDirectory': $_")
    exit 2
}

# --- DocumentPath isolation gate (object-drag only) ---
# Object-drag may only touch an isolated fixture-simple.svg that lives under the
# parent directory of OutputDirectory. Arbitrary user documents are rejected
# before any input or save happens.
$script:docIsolation = $null
if ($Workload -eq 'object-drag' -and [string]::IsNullOrWhiteSpace($DocumentPath)) {
    [Console]::Error.WriteLine('Workload object-drag requires -DocumentPath pointing at the isolated fixture-simple.svg under the parent of OutputDirectory')
    exit 2
}
if (-not [string]::IsNullOrWhiteSpace($DocumentPath)) {
    $script:docIsolation = Test-DocumentPathIsolation -DocPath $DocumentPath -OutputDir $OutputDirectory
    if (-not $script:docIsolation['ok']) {
        [Console]::Error.WriteLine("DocumentPath rejected (exit 2): $($script:docIsolation['reason'])")
        exit 2
    }
    $DocumentPath = [string]$script:docIsolation['document_path']
}

# --- opt-in maximized-workarea calibration gate (before any launch/input) ---
# Both parameters must be supplied together or both left empty. Supplying both
# opts in only for Workload=object-drag; the pinned calibration file is hashed
# and strictly parsed here, before any geometry or input. A fresh in-run
# measurement is never accepted as the expected calibration.
$script:maximizedMode = $false
$script:maximizedCalibration = $null
$script:maximizedCalibrationMeta = $null
$script:pmv2Ok = $false
$script:pmv2Prev = 0L
$script:pmv2Already = $false
$hasCalPath = -not [string]::IsNullOrWhiteSpace($MaximizedCalibrationPath)
$hasCalSha = -not [string]::IsNullOrWhiteSpace($ExpectedMaximizedCalibrationSha256)
if ($hasCalPath -xor $hasCalSha) {
    [Console]::Error.WriteLine('MaximizedCalibrationPath and ExpectedMaximizedCalibrationSha256 must be supplied together or both left empty (exit 2).')
    exit 2
}
if ($hasCalPath) {
    if ($Workload -ne 'object-drag') {
        [Console]::Error.WriteLine('MaximizedCalibrationPath is object-drag only (exit 2).')
        exit 2
    }
    $calResult = Read-MaximizedCalibration -Path $MaximizedCalibrationPath -ExpectedSha256 $ExpectedMaximizedCalibrationSha256
    if (-not $calResult['ok']) {
        [Console]::Error.WriteLine(('MaximizedCalibrationPath rejected (exit 2): ' + [string]$calResult['reason']))
        exit 2
    }
    $script:maximizedMode = $true
    $script:maximizedCalibration = $calResult['calibration']
    $script:maximizedCalibrationMeta = [ordered]@{
        provided = $true
        path = $calResult['calibration']['source_path']
        expected_sha256 = ([string]$ExpectedMaximizedCalibrationSha256).ToLowerInvariant()
        actual_sha256 = $calResult['actual_sha256']
        schema = $calResult['schema']
        calibration_id = $calResult['calibration']['calibration_id']
    }
}

# --- optional screen-observer hook gate (object-drag maximized only) ---
# All four observer parameters together or all empty. When supplied, the exe
# hash and the pinned vacards-screen-observer-roi/1 config are validated BEFORE
# any launch/input; the ROI is pinned to the SAME maximized calibration
# monitor/work geometry. Nothing is auto-calibrated or guessed here.
$script:observerRequested = $false
$script:observerResolvedExePath = $null
$script:observerExeSha = $null
$script:observerRoi = $null
$script:observerRoiMeta = $null
$script:observerProcess = $null
$script:observerOutDir = $null
$script:observerStopFile = $null
$script:observerOutStream = $null
$script:observerErrStream = $null
$script:observerOutTask = $null
$script:observerErrTask = $null
$script:observerCpuBefore = $null
$script:observerStartResult = $null
$hasObsExe = -not [string]::IsNullOrWhiteSpace($ObserverExePath)
$hasObsExeSha = -not [string]::IsNullOrWhiteSpace($ExpectedObserverExeSha256)
$hasObsCfg = -not [string]::IsNullOrWhiteSpace($ObserverRoiConfigPath)
$hasObsCfgSha = -not [string]::IsNullOrWhiteSpace($ExpectedObserverRoiConfigSha256)
$obsCount = @(@($hasObsExe, $hasObsExeSha, $hasObsCfg, $hasObsCfgSha) | Where-Object { $_ -eq $true }).Count
if ($obsCount -gt 0 -and $obsCount -lt 4) {
    [Console]::Error.WriteLine('ObserverExePath, ExpectedObserverExeSha256, ObserverRoiConfigPath and ExpectedObserverRoiConfigSha256 must be supplied together or all left empty (exit 2).')
    exit 2
}
if ($obsCount -eq 4) {
    if ($Workload -ne 'object-drag') {
        [Console]::Error.WriteLine('Observer hook is object-drag only (exit 2).')
        exit 2
    }
    if (-not $script:maximizedMode) {
        [Console]::Error.WriteLine('Observer hook requires the maximized-workarea calibration (exit 2).')
        exit 2
    }
    if (-not (Test-Path -LiteralPath $ObserverExePath -PathType Leaf)) {
        [Console]::Error.WriteLine('ObserverExePath is missing or not a file (exit 2).')
        exit 2
    }
    $obsExeSha = $null
    try { $obsExeSha = (Get-FileHash -LiteralPath $ObserverExePath -Algorithm SHA256).Hash.ToLowerInvariant() }
    catch { [Console]::Error.WriteLine('ObserverExePath sha256 could not be computed (exit 2).'); exit 2 }
    if ($obsExeSha -ne ([string]$ExpectedObserverExeSha256).ToLowerInvariant()) {
        [Console]::Error.WriteLine('ObserverExePath sha256 does not match ExpectedObserverExeSha256 (exit 2).')
        exit 2
    }
    $obsRoi = Read-ObserverRoiConfig -Path $ObserverRoiConfigPath -ExpectedSha256 $ExpectedObserverRoiConfigSha256 -Calibration $script:maximizedCalibration
    if (-not $obsRoi['ok']) {
        [Console]::Error.WriteLine('ObserverRoiConfigPath rejected (exit 2): ' + [string]$obsRoi['reason'])
        exit 2
    }
    $script:observerRequested = $true
    $script:observerResolvedExePath = (Resolve-Path -LiteralPath $ObserverExePath).Path
    $script:observerExeSha = $obsExeSha
    $script:observerRoi = $obsRoi['roi']
    $script:observerRoiMeta = [ordered]@{
        path = $obsRoi['roi']['source_path']
        expected_sha256 = ([string]$ExpectedObserverRoiConfigSha256).ToLowerInvariant()
        actual_sha256 = $obsRoi['actual_sha256']
        schema = $obsRoi['schema']
        roi_id = $obsRoi['roi']['roi_id']
    }
}

$script:logPath = Join-Path -Path $OutputDirectory -ChildPath 'pan.log'

$script:summary = [ordered]@{
    helper              = 'Invoke-AppPan.ps1'
    helper_version      = 1
    utc_started         = [DateTime]::UtcNow.ToString('o')
    target_process_id   = $TargetProcessId
    parameters          = [ordered]@{
        output_directory = $OutputDirectory
        client_width     = $ClientWidth
        client_height    = $ClientHeight
        loops            = $Loops
        steps_per_leg    = $StepsPerLeg
        step_interval_ms = $StepIntervalMs
        window_mode      = if ($script:maximizedMode) { 'maximized-workarea' } else { 'normal-client' }
        maximized_calibration_path = if ($script:maximizedMode) { $MaximizedCalibrationPath } else { '' }
        expected_maximized_calibration_sha256 = if ($script:maximizedMode) { ([string]$ExpectedMaximizedCalibrationSha256).ToLowerInvariant() } else { '' }
        observer_requested = $script:observerRequested
        observer_exe_path = if ($script:observerRequested) { $script:observerResolvedExePath } else { '' }
        expected_observer_exe_sha256 = if ($script:observerRequested) { ([string]$ExpectedObserverExeSha256).ToLowerInvariant() } else { '' }
        observer_roi_config_path = if ($script:observerRequested) { [string]$script:observerRoiMeta['path'] } else { '' }
        expected_observer_roi_config_sha256 = if ($script:observerRequested) { ([string]$ExpectedObserverRoiConfigSha256).ToLowerInvariant() } else { '' }
    }
    window_mode         = if ($script:maximizedMode) { 'maximized-workarea' } else { 'normal-client' }
    requested_mode      = if ($script:maximizedMode) { 'maximized-workarea' } else { 'normal-client' }
    maximized_calibration = $script:maximizedCalibrationMeta
    observer_requested  = $script:observerRequested
    observer_roi_config = $script:observerRoiMeta
    helper_pid          = $PID
    helper_user         = ('{0}\{1}' -f [Environment]::UserDomainName, [Environment]::UserName)
    ps_version          = $PSVersionTable.PSVersion.ToString()
    status              = 'init'
    exit_code           = 7
}

if ($Workload -eq 'object-drag') {
    $script:summary['workload'] = 'object-drag'
    $script:summary['protocol_identity'] = 'continuous-v2'
    $script:summary['object_drag'] = [ordered]@{
        schema                  = 'vacards-app-pan-object-drag/1'
        protocol_identity       = 'continuous-v2'
        document_path           = $DocumentPath
        document_path_isolation = $script:docIsolation
        baseline_save           = $null
        selection               = $null
        click_spacing           = $null
        arm                     = $null
        expected_events         = $null
        endpoint_save           = $null
        endpoint_capture        = $null
        endpoint_capture_note   = $null
        return_arm              = $null
        return_gesture          = $null
        returned_save           = $null
    }
    $script:summary['accepted_event_count'] = $null
    $script:summary['owned_focus'] = [ordered]@{
        schema                 = 'vacards-app-pan-owned-focus/1'
        actual_foreground_hwnd = $null
        owned_hwnd             = $null
        matches                = $null
        at_measured_start      = $null
        at_measured_start_utc  = $null
        at_measured_end        = $null
        at_measured_end_utc    = $null
        accepted_event_count   = $null
    }
}

try {
    Write-PanLog "start TargetProcessId=$TargetProcessId OutputDirectory=$OutputDirectory"

    $script:qpcFreq = [VAPan.Win32]::QpcFrequency()
    if ($script:qpcFreq -le 0) { Throw-PanFailure 7 'QueryPerformanceFrequency failed' }
    $script:summary['qpc_frequency'] = $script:qpcFreq
    $script:summary['input_struct_size'] = [VAPan.Win32]::InputStructSize()

    # --- verified PER_MONITOR_AWARE_V2 thread context (opt-in mode only) ---
    # Own Win32 geometry/input/capture must use physical pixels including the
    # monitor origin. The previous thread context is restored in finally.
    if ($script:maximizedMode) {
        $aw = @([VAPan.Win32]::EstablishPerMonitorV2())
        $script:pmv2Ok = ($aw[0] -eq 1)
        $script:pmv2Prev = [long]$aw[2]
        $script:pmv2Already = ($aw[1] -eq 1)
        $script:summary['dpi_awareness'] = [ordered]@{
            schema = 'vacards-app-pan-dpi-awareness/1'
            api = 'SetThreadDpiAwarenessContext'
            requested_context = 'PER_MONITOR_AWARE_V2'
            established = $script:pmv2Ok
            already_pmv2 = $script:pmv2Already
            previous_context_raw = $script:pmv2Prev
            last_error = [long]$aw[3]
        }
        if (-not $script:pmv2Ok) {
            Throw-PanFailure 4 ('PER_MONITOR_AWARE_V2 thread context could not be established (Win32 ' + [long]$aw[3] + ')')
        }
        $script:pmv2Verified = [bool][VAPan.Win32]::CurrentThreadIsPmv2()
        $script:summary['dpi_awareness']['verified_current_thread_pmv2'] = $script:pmv2Verified
        if (-not $script:pmv2Verified) {
            Throw-PanFailure 4 'PER_MONITOR_AWARE_V2 thread context was not verified after SetThreadDpiAwarenessContext'
        }
        $script:summary['virtual_screen_physical'] = [VAPan.Win32]::VirtualScreen()
        Write-PanLog ("PER_MONITOR_AWARE_V2 established already={0} virtualscreen={1}" -f $script:pmv2Already, (@([VAPan.Win32]::VirtualScreen()) -join ','))
    }

    # --- verify the actual GLib monotonic clock basis against QPC ---
    # The DLL path/hash come from the caller's own loaded app module. A mismatch
    # fails the run (exit 8); we never apply a guessed offset.
    Write-PanLog "GLib clock contract dll=$GlibDllPath"
    $script:clockContract = Get-GlibClockContract -GlibDllPath $GlibDllPath -ExpectedSha256 $ExpectedGlibSha256 -QpcFrequency $script:qpcFreq
    $script:summary['clock_contract'] = $script:clockContract
    Write-PanLog "GLib clock contract status=$($script:clockContract.status) samples=$($script:clockContract.sample_count) max_residual_us=$($script:clockContract.max_abs_residual_us)"
    if ($script:clockContract.status -ne 'ok') {
        Throw-PanFailure 8 "GLib monotonic clock contract failed: $($script:clockContract.error)"
    }

    # --- bind the supplied process only ---
    try { $script:process = [System.Diagnostics.Process]::GetProcessById($TargetProcessId) }
    catch { Throw-PanFailure 3 "Target process $TargetProcessId not found: $_" }
    $script:process.Refresh()
    $exePath = $null
    try { $exePath = $script:process.MainModule.FileName } catch { $exePath = $null }
    $script:summary['target_process'] = [ordered]@{
        id         = $script:process.Id
        name       = $script:process.ProcessName
        session_id = $script:process.SessionId
        path       = $exePath
    }
    Write-PanLog "bound pid=$($script:process.Id) name=$($script:process.ProcessName)"

    # --- main HWND of exactly this pid ---
    $hwnd = [IntPtr]::Zero
    try { $hwnd = $script:process.MainWindowHandle } catch { $hwnd = [IntPtr]::Zero }
    if ($hwnd -eq [IntPtr]::Zero -or [VAPan.Win32]::WindowPid($hwnd) -ne $TargetProcessId) {
        $hwnd = [VAPan.Win32]::FindMainWindow([uint32]$TargetProcessId)
    }
    if ($hwnd -eq [IntPtr]::Zero) { Throw-PanFailure 4 "No main HWND found for pid $TargetProcessId" }
    if ([VAPan.Win32]::WindowPid($hwnd) -ne $TargetProcessId) {
        Throw-PanFailure 4 "HWND $hwnd does not belong to pid $TargetProcessId"
    }
    if (-not [VAPan.Win32]::Visible($hwnd)) { Throw-PanFailure 4 "Main HWND is not visible" }
    $script:hwnd = $hwnd
    $script:summary['hwnd'] = ('0x{0:X}' -f $hwnd.ToInt64())
    Write-PanLog "main hwnd=$($script:summary['hwnd'])"

    # --- physical console session gate ---
    $helperSession = ([System.Diagnostics.Process]::GetCurrentProcess()).SessionId
    $targetSession = [VAPan.Win32]::ProcessSession([uint32]$TargetProcessId)
    $activeConsole = [VAPan.Win32]::ActiveConsoleSession()
    $wts = [VAPan.Win32]::WtsSession([uint32]$targetSession)
    $script:summary['session'] = [ordered]@{
        helper_session        = $helperSession
        target_session        = $targetSession
        active_console_session = $activeConsole
        wts_query_ok          = ($wts[0] -eq 1)
        client_protocol       = $wts[1]
        is_remote             = $wts[2]
    }
    Write-PanLog "session helper=$helperSession target=$targetSession console=$activeConsole protocol=$($wts[1]) remote=$($wts[2])"

    if ($targetSession -le 0 -or $helperSession -ne $targetSession) {
        Throw-PanFailure 3 "target/helper session mismatch or session 0 (helper=$helperSession target=$targetSession)"
    }
    if ($activeConsole -eq [uint32]::MaxValue -or $targetSession -ne [int]$activeConsole) {
        Throw-PanFailure 3 "target session $targetSession is not the active physical console ($activeConsole)"
    }
    if ($wts[0] -ne 1) { Throw-PanFailure 3 'WTS session query failed' }
    if ($wts[1] -ne 0) { Throw-PanFailure 3 "WTS client protocol $($wts[1]) is not console (0)" }
    # remote==1 is an actual remote session; remote==-1 means class 29 produced no
    # usable boolean. Both fail closed, but they are reported distinctly.
    if ($wts[2] -eq 1) { Throw-PanFailure 3 'WTS reports a remote session' }
    if ($wts[2] -ne 0) { Throw-PanFailure 3 'WTS remote-session state unknown (class 29 returned no usable boolean)' }

    # --- non-elevated normal user gate ---
    $curTok = [VAPan.Win32]::OpenCurrentToken()
    $tgtTok = [VAPan.Win32]::OpenToken([uint32]$TargetProcessId)
    if ($curTok -eq [IntPtr]::Zero -or $tgtTok -eq [IntPtr]::Zero) {
        [VAPan.Win32]::Close($curTok); [VAPan.Win32]::Close($tgtTok)
        Throw-PanFailure 3 'could not open process tokens'
    }
    try {
        $curElev = [VAPan.Win32]::TokenElevationValue($curTok)
        $tgtElev = [VAPan.Win32]::TokenElevationValue($tgtTok)
        $curSid  = [VAPan.Win32]::TokenUserSid($curTok)
        $tgtSid  = [VAPan.Win32]::TokenUserSid($tgtTok)
    } finally {
        [VAPan.Win32]::Close($curTok); [VAPan.Win32]::Close($tgtTok)
    }
    $script:summary['identity'] = [ordered]@{
        helper_elevated = $curElev
        target_elevated = $tgtElev
        helper_user_sid = $curSid
        target_user_sid = $tgtSid
    }
    Write-PanLog "elevation helper=$curElev target=$tgtElev sid match=$($curSid -eq $tgtSid)"
    if ($curElev -ne 0) { Throw-PanFailure 3 'helper process is elevated; refusing' }
    if ($tgtElev -ne 0) { Throw-PanFailure 3 'target process is elevated; refusing' }
    if ([string]::IsNullOrEmpty($curSid) -or $curSid -ne $tgtSid) {
        Throw-PanFailure 3 'target process user differs from helper user'
    }

    # --- scale + monitor + client area ---
    $dpi = [VAPan.Win32]::DpiForWindow($hwnd)
    $script:summary['dpi'] = $dpi
    if ($script:maximizedMode) {
        # --- calibrated maximized-workarea branch (opt-in only) ---
        $cal = $script:maximizedCalibration
        $calDpi = [int](Get-CalValue $cal 'dpi')
        if ($dpi -ne $calDpi) {
            Throw-PanFailure 4 ("window DPI $dpi is not the calibrated DPI $calDpi")
        }
        $wpBeforeM = [VAPan.Win32]::WindowPlacementInfo($hwnd)
        $script:summary['window_state_before'] = [ordered]@{
            is_iconic = [VAPan.Win32]::Minimized($hwnd)
            is_zoomed = [VAPan.Win32]::Maximized($hwnd)
            placement_show_cmd = if ($null -ne $wpBeforeM) { $wpBeforeM[0] } else { $null }
            placement_normal_rect = if ($null -ne $wpBeforeM) { @($wpBeforeM[2], $wpBeforeM[3], $wpBeforeM[4], $wpBeforeM[5]) } else { $null }
        }
        # Restore-normal is permitted ONLY to reposition a maximized window
        # BEFORE maximize. It is never called after maximize or before foreground.
        $pre = Test-MaximizedGeometryState -WindowHandle $hwnd -Calibration $cal
        $script:summary['maximized_geometry_before'] = $pre
        if (-not [bool]$pre['ok']) {
            if ([VAPan.Win32]::Minimized($hwnd)) {
                [void][VAPan.Win32]::ShowWindow($hwnd, [int][VAPan.Win32]::SW_RESTORE)
                $iconDeadline = [DateTime]::UtcNow.AddSeconds(5)
                while ([VAPan.Win32]::Minimized($hwnd) -and [DateTime]::UtcNow -lt $iconDeadline) { Start-Sleep -Milliseconds 25 }
                if ([VAPan.Win32]::Minimized($hwnd)) { Throw-PanFailure 4 'maximized-workarea: owned window could not leave the minimized state' }
            }
            if ([VAPan.Win32]::Maximized($hwnd)) {
                if (-not [VAPan.Win32]::ForceNormal($hwnd, 5000)) {
                    Throw-PanFailure 4 'maximized-workarea: owned window could not return to the normal state for monitor placement'
                }
            }
            $monB = @(Get-CalValue $cal 'monitor_bounds')
            $workB = @(Get-CalValue $cal 'work_bounds')
            $cwB = [int](Get-CalValue $cal 'client_width')
            $chB = [int](Get-CalValue $cal 'client_height')
            $placeW = [Math]::Min(($workB[2] - $workB[0]), $cwB)
            $placeH = [Math]::Min(($workB[3] - $workB[1]), $chB)
            if ($placeW -lt 16) { $placeW = 16 }
            if ($placeH -lt 16) { $placeH = 16 }
            $placeX = $workB[0] + [int]((($workB[2] - $workB[0]) - $placeW) / 2)
            $placeY = $workB[1] + [int]((($workB[3] - $workB[1]) - $placeH) / 2)
            $placeOk = [VAPan.Win32]::MoveWindowTo($hwnd, $placeX, $placeY, $placeW, $placeH)
            $script:summary['maximized_place_return'] = $placeOk
            if (-not $placeOk) { Throw-PanFailure 4 'maximized-workarea: SetWindowPos placement on the expected monitor failed' }
            $monDeadline = [DateTime]::UtcNow.AddSeconds(5)
            $onExpected = $false
            while ([DateTime]::UtcNow -lt $monDeadline) {
                $mr = [VAPan.Win32]::MonitorRects($hwnd)
                if ($null -ne $mr -and $mr[0] -eq $monB[0] -and $mr[1] -eq $monB[1] -and $mr[2] -eq $monB[2] -and $mr[3] -eq $monB[3]) { $onExpected = $true; break }
                Start-Sleep -Milliseconds 50
            }
            $script:summary['monitor_placement_match'] = $onExpected
            if (-not $onExpected) { Throw-PanFailure 4 'maximized-workarea: owned window is not on the calibrated monitor after placement' }
            # Native WM_SYSCOMMAND/SC_MAXIMIZE, NOT raw ShowWindow(SW_MAXIMIZE):
            # GTK only sets impl->maximizing on the native command entry point,
            # which is what triggers its non-primary-monitor clamp. The raw
            # ShowWindow path bypasses it and leaves the OS-compensated oversized
            # rect. Never force-set-size. Then wait, bounded, for both the zoomed
            # state and the calibrated geometry to settle; the strict proof below
            # still runs and is the authority.
            [void][VAPan.Win32]::PostMessage($hwnd, [uint32][VAPan.Win32]::WM_SYSCOMMAND, [IntPtr][int][VAPan.Win32]::SC_MAXIMIZE, [IntPtr]::Zero)
            $maxDeadline = [DateTime]::UtcNow.AddSeconds(5)
            while (-not [VAPan.Win32]::Maximized($hwnd) -and [DateTime]::UtcNow -lt $maxDeadline) { Start-Sleep -Milliseconds 25 }
            if (-not [VAPan.Win32]::Maximized($hwnd)) { Throw-PanFailure 4 'maximized-workarea: WM_SYSCOMMAND/SC_MAXIMIZE did not reach the zoomed state within the bound' }
            $geoDeadline = [DateTime]::UtcNow.AddSeconds(5)
            $geoOk = $false
            while ([DateTime]::UtcNow -lt $geoDeadline) {
                $maxGeo = Test-MaximizedGeometryState -WindowHandle $hwnd -Calibration $cal
                if ([bool]$maxGeo['ok']) { $geoOk = $true; break }
                Start-Sleep -Milliseconds 50
            }
            if (-not $geoOk) { Throw-PanFailure 4 'maximized-workarea: calibrated geometry did not settle after WM_SYSCOMMAND/SC_MAXIMIZE within the bound' }
        }
        [void][VAPan.Win32]::WaitClientSize($hwnd, [int](Get-CalValue $cal 'client_width'), [int](Get-CalValue $cal 'client_height'), 5000)
        $proof = Test-MaximizedGeometryState -WindowHandle $hwnd -Calibration $cal
        $script:summary['maximized_proof'] = $proof
        if ($proof['monitor'].Count -eq 4 -and $proof['work'].Count -eq 4) {
            $script:summary['monitor'] = [ordered]@{
                left = $proof['monitor'][0]; top = $proof['monitor'][1]; right = $proof['monitor'][2]; bottom = $proof['monitor'][3]
                work_left = $proof['work'][0]; work_top = $proof['work'][1]; work_right = $proof['work'][2]; work_bottom = $proof['work'][3]
                width = ($proof['monitor'][2] - $proof['monitor'][0]); height = ($proof['monitor'][3] - $proof['monitor'][1])
                device = $proof['monitor_device']
            }
        }
        $script:summary['client_after'] = @($proof['client'])
        $script:summary['bounds_after'] = @($proof['bounds'])
        $script:summary['window_state_after_maximize'] = [ordered]@{
            is_iconic = $proof['is_iconic']; is_zoomed = $proof['is_zoomed']; placement_show_cmd = $proof['placement_show_cmd']
        }
        $script:summary['observed_mode'] = if ([bool]$proof['is_zoomed']) { 'maximized-workarea' } else { 'not-maximized' }
        if (-not [bool]$proof['ok']) {
            Throw-PanFailure 4 ('maximized-workarea proof failed: ' + [string]$proof['reason'])
        }
        Write-PanLog ('maximized proof ok client=' + (@($proof['client']) -join 'x') + ' monitor=' + (@($proof['monitor']) -join ',') + ' work=' + (@($proof['work']) -join ',') + ' dpi=' + [string]$proof['dpi'])
    } else {
    if ($dpi -ne 96) { Throw-PanFailure 4 "window DPI $dpi is not scale 1 (96)" }
    # Record the real window state (maximized/minimized/normal) before touching
    # geometry. A maximized window ignores SetWindowPos sizing, so it must be
    # returned to the normal state first. The hypothesis is never assumed: the
    # actual IsIconic/IsZoomed/WINDOWPLACEMENT values are logged and summarized.
    $wpBefore = [VAPan.Win32]::WindowPlacementInfo($hwnd)
    $script:summary['window_state_before'] = [ordered]@{
        is_iconic = [VAPan.Win32]::Minimized($hwnd)
        is_zoomed = [VAPan.Win32]::Maximized($hwnd)
        placement_show_cmd = if ($null -ne $wpBefore) { $wpBefore[0] } else { $null }
        placement_normal_rect = if ($null -ne $wpBefore) { @($wpBefore[2], $wpBefore[3], $wpBefore[4], $wpBefore[5]) } else { $null }
    }
    Write-PanLog ("window state before iconic={0} zoomed={1} showCmd={2}" -f `
        $script:summary['window_state_before']['is_iconic'], `
        $script:summary['window_state_before']['is_zoomed'], `
        $script:summary['window_state_before']['placement_show_cmd'])

    if ($script:summary['window_state_before']['is_iconic'] -or $script:summary['window_state_before']['is_zoomed']) {
        Write-PanLog 'restore-normal requested (window was iconic or zoomed)'
    }
    # Supported Win32 restore (SetWindowPlacement/SW_SHOWNORMAL first, then
    # ShowWindow/SC_RESTORE), retried and bounded: the real app re-applies the
    # maximized state, so a single SW_RESTORE is not sufficient.
    if (-not [VAPan.Win32]::ForceNormal($hwnd, 5000)) {
        $wpStuck = [VAPan.Win32]::WindowPlacementInfo($hwnd)
        $script:summary['window_state_after_restore'] = [ordered]@{
            is_iconic = [VAPan.Win32]::Minimized($hwnd)
            is_zoomed = [VAPan.Win32]::Maximized($hwnd)
            placement_show_cmd = if ($null -ne $wpStuck) { $wpStuck[0] } else { $null }
        }
        Throw-PanFailure 4 'window did not reach the normal (not iconic/zoomed) state within 5s'
    }
    $wpAfter = [VAPan.Win32]::WindowPlacementInfo($hwnd)
    $script:summary['window_state_after_restore'] = [ordered]@{
        is_iconic = [VAPan.Win32]::Minimized($hwnd)
        is_zoomed = [VAPan.Win32]::Maximized($hwnd)
        placement_show_cmd = if ($null -ne $wpAfter) { $wpAfter[0] } else { $null }
    }
    Write-PanLog ("window state after restore iconic={0} zoomed={1} showCmd={2}" -f `
        $script:summary['window_state_after_restore']['is_iconic'], `
        $script:summary['window_state_after_restore']['is_zoomed'], `
        $script:summary['window_state_after_restore']['placement_show_cmd'])


    $mon = [VAPan.Win32]::MonitorRects($hwnd)
    if ($null -eq $mon) { Throw-PanFailure 4 'GetMonitorInfo failed' }
    $monW = $mon[2] - $mon[0]; $monH = $mon[3] - $mon[1]
    $script:summary['monitor'] = [ordered]@{
        left = $mon[0]; top = $mon[1]; right = $mon[2]; bottom = $mon[3]
        work_left = $mon[4]; work_top = $mon[5]; work_right = $mon[6]; work_bottom = $mon[7]
        width = $monW; height = $monH
    }
    $script:summary['client_before'] = [VAPan.Win32]::ClientSize($hwnd)
    $script:summary['bounds_before'] = [VAPan.Win32]::WindowBounds($hwnd)

    $style   = [uint32][VAPan.Win32]::GetWindowLong($hwnd, [VAPan.Win32]::GWL_STYLE)
    $exstyle = [uint32][VAPan.Win32]::GetWindowLong($hwnd, [VAPan.Win32]::GWL_EXSTYLE)
    $outer = [VAPan.Win32]::ComputeOuter($ClientWidth, $ClientHeight, $style, $exstyle)
    $outerW = $outer[2] - $outer[0]; $outerH = $outer[3] - $outer[1]
    $frameW = $outerW - $ClientWidth; $frameH = $outerH - $ClientHeight
    if ($outerW -gt $monW -or $outerH -gt $monH) {
        Throw-PanFailure 4 "requested client ${ClientWidth}x${ClientHeight} plus frame ${outerW}x${outerH} does not fit monitor ${monW}x${monH}"
    }
    $posX = $mon[0] + [int](($monW - $outerW) / 2)
    $posY = $mon[1] + [int](($monH - $outerH) / 2)
    if ($posY -lt $mon[1]) { $posY = $mon[1] }
    Write-PanLog "resize outer=${outerW}x${outerH} at ($posX,$posY) frameDelta=${frameW}x${frameH}"

    $moveReturn = [System.Collections.ArrayList]@()
    $moveOk = [VAPan.Win32]::MoveWindowTo($hwnd, $posX, $posY, $outerW, $outerH)
    [void]$moveReturn.Add($moveOk)
    if (-not $moveOk) { Throw-PanFailure 4 'SetWindowPos (initial resize) returned false' }
    $clientOk = [VAPan.Win32]::WaitClientSize($hwnd, $ClientWidth, $ClientHeight, 5000)
    if (-not $clientOk) {
        # one corrective resize using the measured frame delta
        $wb = [VAPan.Win32]::WindowBounds($hwnd)
        $cs = [VAPan.Win32]::ClientSize($hwnd)
        $dW = ($wb[2] - $wb[0]) - $cs[0]
        $dH = ($wb[3] - $wb[1]) - $cs[1]
        $moveOk2 = [VAPan.Win32]::MoveWindowTo($hwnd, $wb[0], $wb[1], $ClientWidth + $dW, $ClientHeight + $dH)
        [void]$moveReturn.Add($moveOk2)
        if (-not $moveOk2) { Throw-PanFailure 4 'SetWindowPos (corrective resize) returned false' }
        $clientOk = [VAPan.Win32]::WaitClientSize($hwnd, $ClientWidth, $ClientHeight, 5000)
    }
    $script:summary['move_window_return'] = @($moveReturn)
    $script:summary['client_after'] = [VAPan.Win32]::ClientSize($hwnd)
    $script:summary['bounds_after'] = [VAPan.Win32]::WindowBounds($hwnd)
    if (-not $clientOk) {
        Throw-PanFailure 4 "client area did not reach ${ClientWidth}x${ClientHeight} (actual $($script:summary['client_after'] -join 'x'))"
    }
    $wb = [VAPan.Win32]::WindowBounds($hwnd)
    if ($wb[0] -lt $mon[0] -or $wb[1] -lt $mon[1] -or $wb[2] -gt $mon[2] -or $wb[3] -gt $mon[3]) {
        Throw-PanFailure 4 "window bounds $($wb -join ',') are outside monitor $($mon[0..3] -join ',')"
    }
    Write-PanLog "client verified $($script:summary['client_after'] -join 'x')"

    # --- 2 s settle after the final successful resize, before start PNG/input ---
    # Best-effort visual settle only; it is not proof of drawing completion.
    $script:summary['post_resize_settle_seconds'] = 2
    $script:summary['post_resize_settle_note'] = 'best-effort visual settle after the final successful resize; not proof of drawing completion'
    $script:summary['post_resize_settle_start_qpc'] = [VAPan.Win32]::Qpc()
    Start-Sleep -Seconds 2
    $script:summary['post_resize_settle_end_qpc'] = [VAPan.Win32]::Qpc()
    $script:summary['post_resize_settle_end_utc'] = [DateTime]::UtcNow.ToString('o')

    # --- post-settle FAIL-CLOSED recheck (before foreground/capture/input) ---
    # No relaxed dimensions: the client must still be exactly the requested size,
    # the bounds must still be inside the physical monitor, and the window must
    # still be in the normal state. Any deviation fails exit 4 here.
    $wpSettle = [VAPan.Win32]::WindowPlacementInfo($hwnd)
    $csSettle = [VAPan.Win32]::ClientSize($hwnd)
    $wbSettle = [VAPan.Win32]::WindowBounds($hwnd)
    $monSettle = [VAPan.Win32]::MonitorRects($hwnd)
    $iconicSettle = [VAPan.Win32]::Minimized($hwnd)
    $zoomedSettle = [VAPan.Win32]::Maximized($hwnd)
    $inMonitorSettle = $false
    if ($null -ne $monSettle) {
        $inMonitorSettle = ($wbSettle[0] -ge $monSettle[0]) -and ($wbSettle[1] -ge $monSettle[1]) -and `
            ($wbSettle[2] -le $monSettle[2]) -and ($wbSettle[3] -le $monSettle[3])
    }
    $exactClientSettle = (($csSettle[0] -eq $ClientWidth) -and ($csSettle[1] -eq $ClientHeight))
    $script:summary['post_settle_state'] = [ordered]@{
        is_iconic = $iconicSettle
        is_zoomed = $zoomedSettle
        placement_show_cmd = if ($null -ne $wpSettle) { $wpSettle[0] } else { $null }
        client = $csSettle
        bounds = $wbSettle
        in_monitor = $inMonitorSettle
        exact_client = $exactClientSettle
    }
    Write-PanLog ("post-settle state iconic={0} zoomed={1} client={2}x{3} bounds={4} inMonitor={5} exact={6}" -f `
        $iconicSettle, $zoomedSettle, $csSettle[0], $csSettle[1], ($wbSettle -join ','), $inMonitorSettle, $exactClientSettle)
    if ($iconicSettle -or $zoomedSettle) {
        Throw-PanFailure 4 'post-settle recheck: window is iconic/zoomed after resize'
    }
    if (-not $exactClientSettle) {
        Throw-PanFailure 4 "post-settle recheck: client area changed from ${ClientWidth}x${ClientHeight} to $($csSettle -join 'x')"
    }
    if (-not $inMonitorSettle) {
        Throw-PanFailure 4 "post-settle recheck: bounds $($wbSettle -join ',') are outside monitor"
    }
    $script:summary['observed_mode'] = if ($zoomedSettle) { 'maximized' } elseif ($iconicSettle) { 'minimized' } else { 'normal-client' }
    }

    if ($script:maximizedMode) {
        # Same best-effort settle, then a strict maximized post-settle recheck.
        $script:summary['post_resize_settle_seconds'] = 2
        $script:summary['post_resize_settle_note'] = 'best-effort visual settle after the final successful maximize; not proof of drawing completion'
        $script:summary['post_resize_settle_start_qpc'] = [VAPan.Win32]::Qpc()
        Start-Sleep -Seconds 2
        $script:summary['post_resize_settle_end_qpc'] = [VAPan.Win32]::Qpc()
        $script:summary['post_resize_settle_end_utc'] = [DateTime]::UtcNow.ToString('o')
        $postProof = Test-MaximizedGeometryState -WindowHandle $hwnd -Calibration $script:maximizedCalibration
        $script:summary['post_settle_maximized_proof'] = $postProof
        $script:summary['post_settle_state'] = [ordered]@{
            is_iconic = $postProof['is_iconic']
            is_zoomed = $postProof['is_zoomed']
            placement_show_cmd = $postProof['placement_show_cmd']
            client = $postProof['client']
            bounds = $postProof['bounds']
            in_monitor = $postProof['coverage_ok']
            exact_client = $postProof['exact_client']
        }
        Write-PanLog ("post-settle maximized state iconic={0} zoomed={1} client={2} bounds={3} coverage={4} exact={5}" -f `
            $postProof['is_iconic'], $postProof['is_zoomed'], (@($postProof['client']) -join 'x'), (@($postProof['bounds']) -join ','), $postProof['coverage_ok'], $postProof['exact_client'])
        if (-not [bool]$postProof['ok']) {
            Throw-PanFailure 4 ('maximized-workarea post-settle proof failed: ' + [string]$postProof['reason'])
        }
    }

    # --- foreground only the owned HWND ---
    # Guarded: only acts when the window is iconic/zoomed, so it cannot silently
    # change the verified geometry. The bounded SetForegroundWindow attempt is
    # retained; only when Windows denies it does ONE verified preparatory caption
    # click run (outside the measured gesture, separately logged). In
    # maximized-workarea mode RestoreNormal is never called: the maximized state
    # is the verified target.
    if (-not $script:maximizedMode) { [VAPan.Win32]::RestoreNormal($hwnd) }
    [VAPan.Win32]::Foreground($hwnd)
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    $fgOk = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ([VAPan.Win32]::ForegroundIs($hwnd)) { $fgOk = $true; break }
        [VAPan.Win32]::Foreground($hwnd)
        Start-Sleep -Milliseconds 50
    }
    if (-not $fgOk) {
        Write-PanLog 'SetForegroundWindow denied; attempting ONE verified preparatory focus click'
        $prepClientW = if ($script:maximizedMode) { [int](Get-CalValue $script:maximizedCalibration 'client_width') } else { $ClientWidth }
        $prepClientH = if ($script:maximizedMode) { [int](Get-CalValue $script:maximizedCalibration 'client_height') } else { $ClientHeight }
        $prep = Invoke-PreparatoryCaptionFocus -WindowHandle $hwnd -TargetPid $TargetProcessId -ClientW $prepClientW -ClientH $prepClientH -AllowVerifiedCsdCaption:$AllowVerifiedCsdCaption -MaximizedMode:$script:maximizedMode -Calibration $script:maximizedCalibration
        $script:summary['preparatory_focus'] = $prep
        Write-PanLog ("preparatory focus method=" + [string]$prep['method'] + " optin=" + [string]$prep['csd_optin'] + " client_point=" + (@($prep['csd_client_point']) -join ',') + " point_screen=" + (@($prep['point_screen']) -join ',') + " attempts=" + [string]$prep['attempts'] + " move_confirmed=" + [string]$prep['move_confirmed'] + " click_delivered=" + [string]$prep['click_delivered'] + " click_count=" + [string]$prep['click_count'] + " move_rc=" + [string]$prep['move_sendinput_return'] + " down_rc=" + [string]$prep['left_down_return'] + " up_rc=" + [string]$prep['left_up_return'] + " topmost_attempted=" + [string]$prep['topmost_raise_attempted'] + " topmost_used=" + [string]$prep['topmost_used'] + " topmost_set_ok=" + [string]$prep['topmost_set_call_ok'] + " topmost_orig=" + [string]$prep['topmost_orig_exstyle_topmost'] + " topmost_restore_ok=" + [string]$prep['topmost_restore_call_ok'] + " topmost_after=" + [string]$prep['topmost_after_restore'] + " topmost_restore_verified=" + [string]$prep['topmost_restore_verified'] + " fg_after_restore=" + [string]$prep['foreground_after_restore'] + " point_owned_after_restore=" + [string]$prep['point_owned_after_restore'])
        if ($prep['foreground_after_wait']) {
            $fgOk = $true
            Write-PanLog 'preparatory focus click foregrounded the owned HWND'
        } else {
            Write-PanLog ("preparatory focus click did not foreground the owned HWND: " + $prep['reason'])
        }
    }
    $script:summary['foreground_at_start'] = $fgOk
    if (-not $fgOk) { Throw-PanFailure 4 'cannot foreground the owned HWND' }
    Write-PanLog 'owned HWND is foreground'

    # --- start PNG (outside the input interval) ---
    function Capture-ClientPng([string]$Name) {
        Add-Type -AssemblyName System.Drawing
        $capQpc = [VAPan.Win32]::Qpc()
        $capUtc = [DateTime]::UtcNow.ToString('o')
        $sz = [VAPan.Win32]::ClientSize($hwnd)
        $org = [VAPan.Win32]::ClientScreenOrigin($hwnd)
        $w = $sz[0]; $h = $sz[1]
        if ($w -le 0 -or $h -le 0) { throw "bad client size for PNG $w x $h" }
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        try {
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g.CopyFromScreen($org[0], $org[1], 0, 0, (New-Object System.Drawing.Size($w, $h)), [System.Drawing.CopyPixelOperation]::SourceCopy)
            } finally { $g.Dispose() }
            $path = Join-Path -Path $OutputDirectory -ChildPath $Name
            $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $bmp.Dispose() }
        return [ordered]@{
            path       = (Join-Path -Path $OutputDirectory -ChildPath $Name)
            utc        = $capUtc
            qpc        = $capQpc
            client     = [VAPan.Win32]::ClientSize($hwnd)
            bounds     = [VAPan.Win32]::WindowBounds($hwnd)
            monitor    = [VAPan.Win32]::MonitorRects($hwnd)
            foreground = [VAPan.Win32]::ForegroundIs($hwnd)
            session    = ([System.Diagnostics.Process]::GetCurrentProcess()).SessionId
        }
    }

    # --- object-drag preparation, outside the measured window ---
    # Baseline native Ctrl+S runs BEFORE selection and BEFORE timing, exactly as
    # the design requires. Only the native selector (left click) and native
    # Ctrl+S are used; no app commands. The select click is outside timing too.
    if ($Workload -eq 'object-drag') {
        # Calibrated physical select point only in maximized-workarea mode; the
        # legacy (250,580) trajectory is preserved unchanged otherwise.
        $selectX = 250
        $selectY = 580
        if ($script:maximizedMode) {
            $selectX = [int](Get-CalValue $script:maximizedCalibration 'select_client_x')
            $selectY = [int](Get-CalValue $script:maximizedCalibration 'select_client_y')
        }
        # Shared outside-timing arm press offset: legacy 4, maximized
        # ceil(4*physical_pixels_per_logical_pixel)+2 from the independent
        # calibration (8 at the pinned 1.491279). The press must exceed the
        # native 4-logical-pixel drag threshold even after mixed-DPI absolute
        # quantization, so BOTH arms use this one derived offset. An invalid
        # scale fails closed before any input.
        $armOffset = Get-ArmPressOffset -MaximizedMode:$script:maximizedMode -Calibration $script:maximizedCalibration
        if ($null -eq $armOffset) {
            Throw-PanFailure 2 'arm press offset could not be derived from the independent physical_pixels_per_logical_pixel calibration'
        }
        $armPressX = $selectX - $armOffset
        Invoke-SaveDocumentSnapshot -WindowHandle $hwnd -DocumentPath $DocumentPath -SnapshotName 'baseline.svg' -SnapshotDirectory $OutputDirectory -Summary $script:summary['object_drag'] -Slot 'baseline_save' | Out-Null
        Write-PanLog 'baseline.svg saved'
        Invoke-SelectClick -WindowHandle $hwnd -ClientX $selectX -ClientY $selectY -Summary $script:summary['object_drag'] -Slot 'selection' -MaximizedMode:$script:maximizedMode | Out-Null
        Write-PanLog ("native selector selection click delivered select=" + $selectX + ',' + $selectY)
        # Explicit pre-measurement click spacing: the native selector switches to
        # the rectangle tool on a double-click, so the arm press must be safely
        # past the system double-click interval. Runs outside the measured
        # QPC/CPU window; the bounded max(1000, dct+100) wait is inside the helper.
        Invoke-ClickSpacingWait -WindowHandle $hwnd -ClientW $ClientWidth -ClientH $ClientHeight -Summary $script:summary['object_drag'] -Slot 'click_spacing' -MaximizedMode:$script:maximizedMode -Calibration $script:maximizedCalibration | Out-Null
        $csWait = $script:summary['object_drag']['click_spacing']
        Write-PanLog ("pre-measurement click-spacing wait done dct_ms=" + [string]$csWait['system_double_click_time_ms'] + " required_wait_ms=" + [string]$csWait['required_wait_ms'] + " observed_wait_ms=" + [string]$csWait['observed_wait_ms'] + " foreground_ok=" + [string]$csWait['foreground_ok_after_wait'])
        # Outside timing: arm ONE continuous native left-button drag. The native
        # selector anchors on the FIRST accepted button1 motion, so press at
        # select-4 and send exactly ONE motion to select; the bounded 1000 ms wait
        # is only a settling assumption. The button stays held through the
        # measured window and is released exactly once at the end (or by finally).
        $arm = Invoke-DragArm -WindowHandle $hwnd -PressX $armPressX -AnchorX $selectX -Y $selectY -WaitMs 1000 -ClientW $ClientWidth -ClientH $ClientHeight -QpcFrequency $script:qpcFreq -Summary $script:summary['object_drag'] -Slot 'arm' -MaximizedMode:$script:maximizedMode -Calibration $script:maximizedCalibration
        Write-PanLog ("drag arm done press=" + (@($arm['press_client']) -join ',') + " anchor=" + (@($arm['anchor_client']) -join ',') + " requested_wait_ms=" + [string]$arm['requested_wait_ms'] + " observed_wait_ms=" + [string]$arm['observed_wait_ms'] + " foreground_ok=" + [string]$arm['foreground_ok_after_wait'] + " anchor_immediate_match=" + [string]$arm['anchor_cursor_match'] + " anchor_after_wait_match=" + [string]$arm['anchor_after_wait_match'] + " cursor_after_wait=" + (@($arm['cursor_after_wait']) -join ','))
    }

    $script:summary['capture_start'] = Capture-ClientPng 'client-start.png'
    $script:startPng = $true
    Write-PanLog 'start PNG captured'

    # --- pacing + injection ---
    $freq = $script:qpcFreq
    function Wait-Qpc([long]$Target) {
        while ($true) {
            $now = [VAPan.Win32]::Qpc()
            if ($now -ge $Target) { return }
            $remainMs = (($Target - $now) * 1000.0) / $freq
            if ($remainMs -gt 2.0) { [System.Threading.Thread]::Sleep(1) }
            else { [System.Threading.Thread]::SpinWait(50) }
        }
    }

    function Emit-Event {
        param(
            [string]$Phase, [int]$Loop, [int]$ClientX, [int]$ClientY,
            [bool]$IsButton, [uint32]$Flags
        )
        if ($script:process.HasExited) {
            Add-PanEvent -Loop $Loop -Phase "$Phase-lost" -DesiredX $ClientX -DesiredY $ClientY `
                -ActualX -1 -ActualY -1 -ActualScreenX -1 -ActualScreenY -1 `
                -Return -1 -ForegroundOk $false -Qpc ([VAPan.Win32]::Qpc()) -Utc ([DateTime]::UtcNow.ToString('o')) | Out-Null
            Throw-PanFailure 5 "target process exited before event (phase=$Phase loop=$Loop)"
        }
        if (-not [VAPan.Win32]::ForegroundIs($script:hwnd) -or -not [VAPan.Win32]::Visible($script:hwnd)) {
            Add-PanEvent -Loop $Loop -Phase "$Phase-lost" -DesiredX $ClientX -DesiredY $ClientY `
                -ActualX -1 -ActualY -1 -ActualScreenX -1 -ActualScreenY -1 `
                -Return -1 -ForegroundOk $false -Qpc ([VAPan.Win32]::Qpc()) -Utc ([DateTime]::UtcNow.ToString('o')) | Out-Null
            Throw-PanFailure 5 "foreground/visibility lost before event (phase=$Phase loop=$Loop)"
        }
        $qpc = [VAPan.Win32]::Qpc()
        $utc = [DateTime]::UtcNow.ToString('o')
        $rc = 0
        if ($IsButton) {
            $rc = [int][VAPan.Win32]::SendMouse($Flags, 0, 0)
        } else {
            $scr = [VAPan.Win32]::ClientToScreenPoint($script:hwnd, $ClientX, $ClientY)
            $pixelCenter = ((Get-Variable -Name 'maximizedMode' -Scope Script -ValueOnly -ErrorAction SilentlyContinue) -eq $true)
            $abs = [VAPan.Win32]::ScreenToAbsoluteForMode($scr[0], $scr[1], $pixelCenter)
            if ($null -eq $abs) { Throw-PanFailure 6 "point $($scr[0]),$($scr[1]) cannot be represented in the virtual desktop at phase=$Phase loop=$Loop" }
            $rc = [int][VAPan.Win32]::SendMouse($Flags, $abs[0], $abs[1])
        }
        $cur = [VAPan.Win32]::CursorClient($script:hwnd)
        Add-PanEvent -Loop $Loop -Phase $Phase -DesiredX $ClientX -DesiredY $ClientY `
            -ActualX $cur[0] -ActualY $cur[1] -ActualScreenX $cur[2] -ActualScreenY $cur[3] `
            -Return $rc -ForegroundOk $true -Qpc $qpc -Utc $utc | Out-Null
        if ($rc -ne 1) {
            Throw-PanFailure 6 "SendInput returned $rc (expected 1) at phase=$Phase loop=$Loop"
        }
    }

    if ($Workload -eq 'object-drag') {
        # continuous-v2: (Loops-1) out/back cycles + one final out-leg + exactly
        # one release. The press and the first-motion anchor are armed OUTSIDE
        # the measured window, so there is no measured initial position or down.
        $expectedEvents = (($Loops - 1) * (2 * $StepsPerLeg)) + $StepsPerLeg + 1
    } else {
        $expectedEvents = 1 + ($Loops * ((2 * $StepsPerLeg) + 2))
    }
    $script:summary['expected_events'] = $expectedEvents
    Write-PanLog "workload=$Workload start expectedEvents=$expectedEvents"

    # --- optional screen-observer hook: valid baseline strictly before input ---
    # Started only after the out-of-timing arm/settle and the start PNG, and
    # before input_start_qpc. It is not part of the measured event protocol; it
    # observes the already-running target during the measured drag. A ready file
    # (first valid baseline) is required before any measured input; otherwise the
    # helper fails closed and injects nothing.
    if ($script:observerRequested) {
        $script:observerOutDir = Join-Path $OutputDirectory 'observer'
        $script:observerStopFile = Join-Path $script:observerOutDir 'observer-stop'
        $obsStart = Start-ScreenObserver -ExePath $script:observerResolvedExePath -ExpectedExeSha256 $ExpectedObserverExeSha256 -Roi $script:observerRoi -TargetPid $TargetProcessId -Hwnd ($hwnd.ToInt64()) -OutDir $script:observerOutDir
        $script:observerStartResult = $obsStart
        if (-not [bool]$obsStart['ready_seen']) {
            Set-ObserverSummary -Start $obsStart -Stop $null -Meta $script:observerRoiMeta -ExePath $script:observerResolvedExePath -ExpectedExeSha256 $ExpectedObserverExeSha256 -OutDir $script:observerOutDir -StopFile $script:observerStopFile
            Throw-PanFailure 7 ('screen observer did not reach a valid baseline before input: ' + [string]$obsStart['error'])
        }
        Write-PanLog ('screen observer ready pid=' + [string]$obsStart['process_id'] + ' ready_wait_ms=' + [string]$obsStart['ready_wait_ms'] + ' out=' + $script:observerOutDir)
    }

    $script:workStartQpc = [VAPan.Win32]::Qpc()
    $script:summary['input_start_qpc'] = $script:workStartQpc
    $script:summary['input_start_utc'] = [DateTime]::UtcNow.ToString('o')
    $script:summary['workload_start_qpc'] = $script:workStartQpc
    $script:summary['workload_start_utc'] = $script:summary['input_start_utc']
    $script:summary['cpu_delta_scope'] = 'input sequence only (before the first injected event through the last injected event); not all render CPU'
    $cpuBefore = $null
    try {
        $script:process.Refresh()
        if (-not $script:process.HasExited) { $cpuBefore = $script:process.TotalProcessorTime.TotalSeconds }
    } catch { $cpuBefore = $null }
    $script:summary['cpu_seconds_before'] = $cpuBefore

    if ($Workload -eq 'object-drag') {
        # --- continuous-v2 native left-button drag, measured window only ---
        # The out-of-timing arm already pressed and anchored at 250; the button
        # stays held, so this window is ONE continuous drag: nine out/back cycles
        # then one final outbound leg to +120, then exactly ONE release.
        $dragStartX = 250
        $dragEndX   = 370
        $dragY      = 580
        # Calibrated physical offsets only in maximized-workarea mode. The
        # Get-Variable guard keeps the legacy extracted block executable when no
        # maximized mode exists (default empty calibration).
        if ((Get-Variable -Name 'maximizedMode' -Scope Script -ValueOnly -ErrorAction SilentlyContinue) -eq $true) {
            $dragStartX = [int](Get-CalValue $script:maximizedCalibration 'select_client_x')
            $dragEndX   = $dragStartX + 120
            $dragY      = [int](Get-CalValue $script:maximizedCalibration 'select_client_y')
        }
        # Leg-boundary maximized geometry guard (no-op in the legacy path and
        # never resolved by the legacy extracted block).
        $odGuardEnabled = ((Get-Variable -Name 'maximizedMode' -Scope Script -ValueOnly -ErrorAction SilentlyContinue) -eq $true)
        $script:summary['object_drag']['expected_events'] = $expectedEvents
        $script:summary['owned_focus']['actual_foreground_hwnd'] = ('0x{0:X}' -f [VAPan.Win32]::GetForegroundWindow().ToInt64())
        $script:summary['owned_focus']['owned_hwnd'] = ('0x{0:X}' -f $hwnd.ToInt64())
        $script:summary['owned_focus']['matches'] = [bool][VAPan.Win32]::ForegroundIs($hwnd)
        $script:summary['owned_focus']['at_measured_start'] = [bool][VAPan.Win32]::ForegroundIs($hwnd)
        $script:summary['owned_focus']['at_measured_start_utc'] = [DateTime]::UtcNow.ToString('o')

        for ($l = 1; $l -le ($Loops - 1); $l++) {
            $legStart = [VAPan.Win32]::Qpc()
            for ($s = 1; $s -le $StepsPerLeg; $s++) {
                Wait-Qpc ([long]($legStart + [long]($freq * ($StepIntervalMs * $s) / 1000.0)))
                $cx = [VAPan.Win32]::StepCoord($dragStartX, $dragEndX, $s, $StepsPerLeg)
                Emit-Event -Phase 'out' -Loop $l -ClientX $cx -ClientY $dragY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
                $script:lastClientX = $cx; $script:lastClientY = $dragY
            }
            if ($odGuardEnabled) { Test-MeasuredGeometryGuard ('cycle-out-' + $l) | Out-Null }

            $legStart = [VAPan.Win32]::Qpc()
            for ($s = 1; $s -le $StepsPerLeg; $s++) {
                Wait-Qpc ([long]($legStart + [long]($freq * ($StepIntervalMs * $s) / 1000.0)))
                $cx = [VAPan.Win32]::StepCoord($dragEndX, $dragStartX, $s, $StepsPerLeg)
                Emit-Event -Phase 'back' -Loop $l -ClientX $cx -ClientY $dragY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
                $script:lastClientX = $cx; $script:lastClientY = $dragY
            }
            if ($odGuardEnabled) { Test-MeasuredGeometryGuard ('cycle-back-' + $l) | Out-Null }
        }

        # One final outbound leg; the single release at +120 ends the measured
        # window and is the only measured up event.
        $legStart = [VAPan.Win32]::Qpc()
        for ($s = 1; $s -le $StepsPerLeg; $s++) {
            Wait-Qpc ([long]($legStart + [long]($freq * ($StepIntervalMs * $s) / 1000.0)))
            $cx = [VAPan.Win32]::StepCoord($dragStartX, $dragEndX, $s, $StepsPerLeg)
            Emit-Event -Phase 'out' -Loop $Loops -ClientX $cx -ClientY $dragY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
            $script:lastClientX = $cx; $script:lastClientY = $dragY
        }
        if ($odGuardEnabled) { Test-MeasuredGeometryGuard 'final-out' | Out-Null }
        Emit-Event -Phase 'up' -Loop $Loops -ClientX $dragEndX -ClientY $dragY -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP)
        $script:leftDown = $false
        if ($odGuardEnabled) { Test-MeasuredGeometryGuard 'post-release' | Out-Null }
    } else {
        # --- original middle-button canvas pan (unchanged) ---
        $startX = 350
        $endX   = 470
        $panY   = 400

        # position at the pan start once; the return leg ends exactly there
        Emit-Event -Phase 'position' -Loop 0 -ClientX $startX -ClientY $panY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
        Wait-Qpc ([long]($script:workStartQpc + [long]($freq * $StepIntervalMs / 1000.0)))

        for ($l = 1; $l -le $Loops; $l++) {
            Emit-Event -Phase 'down' -Loop $l -ClientX $startX -ClientY $panY -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_MIDDLEDOWN)
            $script:midDown = $true

            $legStart = [VAPan.Win32]::Qpc()
            for ($s = 1; $s -le $StepsPerLeg; $s++) {
                Wait-Qpc ([long]($legStart + [long]($freq * ($StepIntervalMs * $s) / 1000.0)))
                $cx = [VAPan.Win32]::StepCoord($startX, $endX, $s, $StepsPerLeg)
                Emit-Event -Phase 'out' -Loop $l -ClientX $cx -ClientY $panY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
                $script:lastClientX = $cx; $script:lastClientY = $panY
            }

            $legStart = [VAPan.Win32]::Qpc()
            for ($s = 1; $s -le $StepsPerLeg; $s++) {
                Wait-Qpc ([long]($legStart + [long]($freq * ($StepIntervalMs * $s) / 1000.0)))
                $cx = [VAPan.Win32]::StepCoord($endX, $startX, $s, $StepsPerLeg)
                Emit-Event -Phase 'back' -Loop $l -ClientX $cx -ClientY $panY -IsButton $false -Flags ([uint32][VAPan.Win32]::MOVE_FLAGS)
                $script:lastClientX = $cx; $script:lastClientY = $panY
            }

            Emit-Event -Phase 'up' -Loop $l -ClientX $startX -ClientY $panY -IsButton $true -Flags ([uint32][VAPan.Win32]::MOUSEEVENTF_MIDDLEUP)
            $script:midDown = $false
        }
    }

    $script:workEndQpc = [VAPan.Win32]::Qpc()
    $script:summary['input_end_qpc'] = $script:workEndQpc
    $script:summary['input_end_utc'] = [DateTime]::UtcNow.ToString('o')
    $script:summary['workload_end_qpc'] = $script:workEndQpc
    $script:summary['workload_end_utc'] = $script:summary['input_end_utc']
    $script:summary['window_duration_seconds'] = ($script:workEndQpc - $script:workStartQpc) / [double]$freq

    # Close the measured application-CPU interval immediately after the last
    # injected event, BEFORE the optional observer-stop block. Stop-ScreenObserver
    # performs bounded waits, stream drain and hashing for the observer process,
    # so sampling the app CPU after it would fold observer teardown work into the
    # measured interval and make observer-on trials incomparable with controls.
    # cpuBefore is sampled just after input_start_qpc in the same scope.
    $cpuAfter = $null
    try {
        $script:process.Refresh()
        if (-not $script:process.HasExited) { $cpuAfter = $script:process.TotalProcessorTime.TotalSeconds }
    } catch { $cpuAfter = $null }
    $script:summary['cpu_seconds_after'] = $cpuAfter
    if ($null -ne $cpuBefore -and $null -ne $cpuAfter) {
        $script:summary['cpu_seconds_delta'] = $cpuAfter - $cpuBefore
    }

    # --- optional screen-observer hook: stop strictly after input_end ---------
    # Normal stop via the stop file with a bounded wait; force-kill only the
    # owned observer process if it failed to exit. Nonzero exit, timeout, a
    # forced kill or no captured frames invalidate the trial (exit 7). The
    # observer block is recorded before any throw so the partial evidence is
    # preserved. No latency association is computed here.
    if ($script:observerRequested) {
        $obsStop = Stop-ScreenObserver -InputStartQpc $script:summary['input_start_qpc'] -InputEndQpc $script:summary['input_end_qpc']
        Set-ObserverSummary -Start $script:observerStartResult -Stop $obsStop -Meta $script:observerRoiMeta -ExePath $script:observerResolvedExePath -ExpectedExeSha256 $ExpectedObserverExeSha256 -OutDir $script:observerOutDir -StopFile $script:observerStopFile
        $obsStopErrors = @($obsStop['errors'])
        if ($obsStopErrors.Count -gt 0) {
            Throw-PanFailure 7 ('screen observer hook trial invalid: ' + ($obsStopErrors -join ' | '))
        }
        Write-PanLog ('screen observer stopped exit=' + [string]$obsStop['exit_code'] + ' frames=' + [string]$obsStop['frames_written'] + ' content_changes=' + [string]$obsStop['content_changes'] + ' cpu_delta_s=' + [string]$obsStop['cpu_seconds_delta'])
    }

    if ($script:eventIndex -ne $expectedEvents) {
        Throw-PanFailure 6 "injected event count $script:eventIndex != expected $expectedEvents"
    }

    if ($Workload -eq 'object-drag') {
        # Record the actual owned-focus state and the accepted (SendInput==1)
        # measured events at the close of the measured window.
        $script:summary['accepted_event_count'] = @($script:events | Where-Object { $_.sendinput_return -eq 1 }).Count
        $script:summary['owned_focus']['at_measured_end'] = [bool][VAPan.Win32]::ForegroundIs($hwnd)
        $script:summary['owned_focus']['at_measured_end_utc'] = [DateTime]::UtcNow.ToString('o')
        $script:summary['owned_focus']['accepted_event_count'] = $script:summary['accepted_event_count']

        # Endpoint save happens BEFORE the return leg. Both are outside the
        # measured window; their events never enter $script:events or the count.
        Invoke-SaveDocumentSnapshot -WindowHandle $hwnd -DocumentPath $DocumentPath -SnapshotName 'endpoint.svg' -SnapshotDirectory $OutputDirectory -Summary $script:summary['object_drag'] -Slot 'endpoint_save' | Out-Null
        Write-PanLog 'endpoint.svg saved'

        # Visually reviewable actual moved rect: ONE client-drag-endpoint.png
        # captured after the endpoint native save and before the return leg,
        # outside the measured window. It complements the SVG oracle. The
        # capture time is best-effort and is not proof of drawing completion.
        $script:summary['object_drag']['endpoint_capture'] = Capture-ClientPng 'client-drag-endpoint.png'
        $script:summary['object_drag']['endpoint_capture_note'] = 'best-effort capture time outside the measured window after endpoint.svg and before the return leg; not proof of drawing completion'
        Write-PanLog 'client-drag-endpoint.png captured (best-effort capture time, not proof of drawing completion)'

        # Outside timing: analogous arm for the return drag. Legacy position
        # 250-4=246 down, ONE anchor motion to 250, bounded 1000 ms settle; in
        # maximized mode the arm uses the SAME derived offset around the fixed
        # anchors (select, select+120). The button stays held for the leg, which
        # sends 60 motions back to select and ONE release. The return auxiliary
        # count is 3 arm events + 61 leg events = 64.
        $retArm = Invoke-DragArm -WindowHandle $hwnd -PressX ($selectX + 120 + $armOffset) -AnchorX ($selectX + 120) -Y $selectY -WaitMs 1000 -ClientW $ClientWidth -ClientH $ClientHeight -QpcFrequency $freq -Summary $script:summary['object_drag'] -Slot 'return_arm' -MaximizedMode:$script:maximizedMode -Calibration $script:maximizedCalibration
        if ($script:maximizedMode) { Test-MeasuredGeometryGuard 'return-arm' | Out-Null }
        Write-PanLog ("return arm done press=" + (@($retArm['press_client']) -join ',') + " anchor=" + (@($retArm['anchor_client']) -join ',') + " requested_wait_ms=" + [string]$retArm['requested_wait_ms'] + " observed_wait_ms=" + [string]$retArm['observed_wait_ms'] + " foreground_ok=" + [string]$retArm['foreground_ok_after_wait'] + " anchor_after_wait_match=" + [string]$retArm['anchor_after_wait_match'])

        Invoke-ReturnLeg -WindowHandle $hwnd -FromX ($selectX + 120) -FromY $selectY -ToX $selectX -ToY $selectY -Legs $StepsPerLeg -StepIntervalMs $StepIntervalMs -QpcFrequency $freq -Summary $script:summary['object_drag'] -Slot 'return_gesture' | Out-Null
        if ($script:maximizedMode) { Test-MeasuredGeometryGuard 'return-leg' | Out-Null }
        Write-PanLog 'native return leg delivered'

        Invoke-SaveDocumentSnapshot -WindowHandle $hwnd -DocumentPath $DocumentPath -SnapshotName 'returned.svg' -SnapshotDirectory $OutputDirectory -Summary $script:summary['object_drag'] -Slot 'returned_save' | Out-Null
        if ($script:maximizedMode) { Test-MeasuredGeometryGuard 'returned' | Out-Null }
        Write-PanLog 'returned.svg saved'
    }

    $script:status = 'ok'
    $script:exitCode = 0
    Write-PanLog "workload complete injected=$script:eventIndex duration=$($script:summary['window_duration_seconds'])s"
}
catch {
    if ($script:status -ne 'ok') { $script:status = 'failed' }
    # Input has stopped at this point (the outer catch runs before finally). If
    # input began but never reached the normal end, close the window honestly
    # with the observed QPC so the summary stays partial rather than fabricated.
    if ($script:summary.Contains('input_start_qpc') -and $null -ne $script:summary['input_start_qpc'] -and
        (-not $script:summary.Contains('input_end_qpc') -or $null -eq $script:summary['input_end_qpc'])) {
        $script:summary['input_end_qpc'] = [VAPan.Win32]::Qpc()
        $script:summary['input_end_utc'] = [DateTime]::UtcNow.ToString('o')
        $script:summary['input_end_note'] = 'input stopped by failure/release path; QPC observed in catch'
    }
    $script:summary['failure'] = "$_"
    Write-PanLog "ERROR: $_"
}
finally {
    # always release an outstanding S key from a Ctrl+S save
    if ($script:sDown -and $script:hwnd -ne [IntPtr]::Zero) {
        try {
            $rcS = [int][VAPan.Win32]::SendKey([uint16][VAPan.Win32]::VK_S, $true)
            Write-PanLog "released S key in finally rc=$rcS"
        } catch {
            Write-PanLog "WARNING: final S release failed: $_"
        }
        $script:sDown = $false
    }

    # always release an outstanding Ctrl from a Ctrl+S save
    if ($script:ctrlDown -and $script:hwnd -ne [IntPtr]::Zero) {
        try {
            $rcCtrl = [int][VAPan.Win32]::SendKey([uint16][VAPan.Win32]::VK_CONTROL, $true)
            Write-PanLog "released Ctrl key in finally rc=$rcCtrl"
        } catch {
            Write-PanLog "WARNING: final Ctrl release failed: $_"
        }
        $script:ctrlDown = $false
    }

    # always release an outstanding left button (preparatory caption click,
    # selection click, or an arm that errored/failed after its press)
    if ($script:leftDown -and $script:hwnd -ne [IntPtr]::Zero) {
        try {
            $rcLeft = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP, 0, 0)
            Write-PanLog "released left button in finally rc=$rcLeft"
        } catch {
            Write-PanLog "WARNING: final left-button release failed: $_"
        }
        $script:leftDown = $false
    }

    # always release an outstanding middle button
    if ($script:midDown -and $script:hwnd -ne [IntPtr]::Zero) {
        try {
            $rc = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_MIDDLEUP, 0, 0)
            $cur = [VAPan.Win32]::CursorClient($script:hwnd)
            Add-PanEvent -Loop -1 -Phase 'release-finally' -DesiredX $script:lastClientX -DesiredY $script:lastClientY `
                -ActualX $cur[0] -ActualY $cur[1] -ActualScreenX $cur[2] -ActualScreenY $cur[3] `
                -Return $rc -ForegroundOk ([VAPan.Win32]::ForegroundIs($script:hwnd)) `
                -Qpc ([VAPan.Win32]::Qpc()) -Utc ([DateTime]::UtcNow.ToString('o')) | Out-Null
            Write-PanLog "released middle button in finally rc=$rc"
        } catch {
            Write-PanLog "WARNING: final middle-button release failed: $_"
        }
        $script:midDown = $false
    }

    # Safety net: if the observer was requested but the normal stop path did not
    # run (a failure before input_end, or ready never appeared), stop the owned
    # process now and preserve whatever evidence exists. The normal path has
    # already written the observer summary, so this only fills a gap.
    if ($script:observerRequested) {
        try {
            $obsRunning = $false
            if ($null -ne $script:observerProcess) {
                try { $script:observerProcess.Refresh(); $obsRunning = -not $script:observerProcess.HasExited } catch { $obsRunning = $false }
            }
            if (-not $script:summary.Contains('observer')) {
                $lateStop = if ($obsRunning) { Stop-ScreenObserver -InputStartQpc $script:summary['input_start_qpc'] -InputEndQpc $script:summary['input_end_qpc'] } else { $null }
                Set-ObserverSummary -Start $script:observerStartResult -Stop $lateStop -Meta $script:observerRoiMeta -ExePath $script:observerResolvedExePath -ExpectedExeSha256 $ExpectedObserverExeSha256 -OutDir $script:observerOutDir -StopFile $script:observerStopFile
            } elseif ($obsRunning) {
                Write-PanLog 'WARNING: observer process still alive after the recorded stop; stopping it owned'
                [void](Stop-ScreenObserver -InputStartQpc $script:summary['input_start_qpc'] -InputEndQpc $script:summary['input_end_qpc'])
            }
        } catch {
            Write-PanLog "WARNING: late observer stop failed: $_"
        }
    }

    # End PNG outside the input interval. The 2 s settle is a best-effort visual
    # settle, NOT proof that the renderer finished drawing.
    if ($script:startPng -and -not $script:endPng -and $script:hwnd -ne [IntPtr]::Zero) {
        try {
            $script:summary['capture_end_settle_seconds'] = 2
            $script:summary['capture_end_settle_note'] = 'best-effort visual settle outside the input window; not proof of drawing completion'
            Start-Sleep -Seconds 2
            $script:summary['capture_end'] = Capture-ClientPng 'client-end.png'
            $script:endPng = $true
            Write-PanLog 'end PNG captured'
        } catch {
            Write-PanLog "WARNING: end PNG capture failed: $_"
        }
    }

    # Restore the previous thread DPI-awareness context (opt-in mode only).
    if ($script:maximizedMode -and $script:pmv2Ok) {
        try {
            $restored = [bool][VAPan.Win32]::RestoreThreadContext([long]$script:pmv2Prev)
            if ($script:summary.Contains('dpi_awareness')) {
                $script:summary['dpi_awareness']['restored'] = $restored
            }
            Write-PanLog "PER_MONITOR_AWARE_V2 thread context restored=$restored"
        } catch {
            Write-PanLog "WARNING: thread DPI-awareness restore failed: $_"
        }
    }

    try {
        $script:summary['injected_event_count'] = $script:eventIndex
        $script:summary['utc_finished'] = [DateTime]::UtcNow.ToString('o')
        Save-PanSummary
        Write-PanLog "summary written exit=$script:exitCode status=$script:status"
    } catch {
        Write-Warning "failed to write input-summary.json: $_"
    }
}

exit $script:exitCode
