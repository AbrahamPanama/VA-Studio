# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
  Focused ACTUAL-APP visual regression for the Windows default Cairo/GDI buffer
  candidate: same candidate app, buffer ON (unset) vs buffer OFF (=0), paired
  settled-state client captures and an exact pixel oracle.

.DESCRIPTION
  Dispatched through the EXISTING reviewed normal-user console chain
  (Invoke-RdpScript.ps1 -> explorer-limited-probe\Invoke-ExplorerScript.ps1).
  No elevation, no scheduler, no ExecutionPolicy argument, no RDP session, no OS
  display change, no installed-app replacement.

  Reuses the reviewed Invoke-AppTrial.ps1 launch/profile/environment/module-check
  methods (copied, not dot-sourced): physical-console gate, isolated fresh profile
  + preferences.xml, the exact process/child environment contract, owned
  VACards-Test.exe --app-id-tag launch with async stream drain, child discovery by
  parent PID + unique tag, runtime loaded-module path/hash verification, and
  normal WM_CLOSE / CloseMainWindow teardown.

  The embedded VAPan C# interop block is extracted byte-exactly from the pinned
  packaging/windows/vacards/Invoke-AppPan.ps1 here-string. Only a small separate
  interop class is added: RedrawWindow (optional diagnostic forced repaint) and
  CreateWindowExW/DestroyWindow/UpdateWindow for the controlled temporary opaque
  occluder, plus an exact 32bpp ARGB image diff.

  Two arms run sequentially from the SAME document path with separate fresh
  profiles: arm 'default' (GDK_WIN32_CAIRO_GDI_BUFFER cleared) and arm 'disabled'
  (GDK_WIN32_CAIRO_GDI_BUFFER=0). Each arm captures the same deterministic settled
  states; corresponding states are compared with exact geometry preconditions and
  an exact per-pixel oracle. show_diagnostics=0 in both arms (recorded divergence)
  to remove the dynamic on-canvas rendering-stats overlay.

  No PASS is issued by this script; it reports MATCH/MISMATCH/INVALID and writes
  visual-regression-result.json. The root owns acceptance.

.PARAMETER SelfCheck
  Parse/compile/identity self-check only; no GUI launch.

.PARAMETER Root
  Existing staged root (wrapper + fixture + helpers). Receives SELFCHECK.json and
  PILOT-COMPLETE.json.

.PARAMETER SelfPath
  Path to this script for the SelfCheck AST parse (required when dispatched as a
  scriptblock, where $PSCommandPath is empty).
#>
[CmdletBinding()]
param(
    [switch]$SelfCheck,
    [string]$SelfPath = '',
    [string]$Root = '',
    [string]$AppRoot = '',
    [string]$FixtureSource = '',
    [string]$PanInteropSourcePath = '',
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedFixtureSha256 = '',
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedExeSha256 = '',
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedGtkSha256 = '',
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedCairoSha256 = '',
    [ValidatePattern('^([0-9a-fA-F]{64})?$')][string]$ExpectedGlibSha256 = '',
    [ValidateRange(10, 600)][int]$LaunchTimeoutSeconds = 90,
    [ValidateRange(5, 120)][int]$CloseTimeoutSeconds = 20,
    [ValidateRange(200, 20000)][int]$SettleMs = 2500,
    [ValidateRange(100, 20000)][int]$StateWaitMs = 1500,
    [ValidateRange(1, 80)][int]$OccluderInsetXPercent = 15,
    [ValidateRange(1, 80)][int]$OccluderInsetYPercent = 20,
    [ValidateRange(10, 99)][int]$OccluderWidthPercent = 70,
    [ValidateRange(10, 99)][int]$OccluderHeightPercent = 60,
    [ValidateRange(64, 16384)][int]$RestoredSmallWidth = 1024,
    [ValidateRange(64, 16384)][int]$RestoredSmallHeight = 640,
    [ValidateRange(64, 16384)][int]$RestoredLargeWidth = 1600,
    [ValidateRange(64, 16384)][int]$RestoredLargeHeight = 1000,
    [string]$ExpectedMonitorDevice = '\\.\DISPLAY2',
    [int]$ExpectedMonitorLeft = 1920,
    [int]$ExpectedMonitorTop = 0,
    [int]$ExpectedMonitorRight = 5760,
    [int]$ExpectedMonitorBottom = 2160,
    [int]$ExpectedWorkRight = 5760,
    [int]$ExpectedWorkBottom = 2088,
    [int]$ExpectedDpi = 144,
    # --- explicit DIAGNOSTIC mode (dual-geometry) ---------------------------------
    # Collects actual geometry and BOTH buffer arms without claiming valid/full4K.
    # Preserves every hard identity/session gate; records mismatches as errors.
    [switch]$Diagnostic,
    [string]$ExpectedSecondaryDevice = '\\.\DISPLAY1',
    [int]$ExpectedSecondaryLeft = 0,
    [int]$ExpectedSecondaryTop = 0,
    [int]$ExpectedSecondaryRight = 1920,
    [int]$ExpectedSecondaryBottom = 1080,
    [int]$ExpectedSecondaryWorkRight = 1920,
    [int]$ExpectedSecondaryWorkBottom = 1032,
    [int]$ExpectedSecondaryDpi = 96,
    [ValidateRange(64, 16384)][int]$DiagnosticRestoredWidth = 1280,
    [ValidateRange(64, 16384)][int]$DiagnosticRestoredHeight = 800,
    # --- normal-test harness corrections (no app/GTK change) --------------------
    # CSD focus opt-in: the beta5 GTK client-side decoration exposes no Win32
    # HTCAPTION, so the reviewed fixed client point (ClientW/2, 30) is used after
    # the native caption scan fails. Off by default; the pilot passes it.
    [switch]$AllowVerifiedCsdCaption,
    # Optional cursor park before each capture. Root-suggested, but the v2 pilot
    # showed it WORSENED the ruler-marker mismatch (all states failed), so it is
    # default OFF and never required for the CSD focus policy.
    [switch]$ParkCursorOnCaption,
    # Minimize specifically identified stale WindowsTerminal / DisplaySettings
    # windows for the duration of the run (record + restore; never close/kill).
    [switch]$MinimizeStaleOccluders,
    # SYSTEM_AWARE app window DPI (96) is distinct from the 144 monitor DPI.
    [ValidateRange(48, 960)][int]$ExpectedWindowDpi = 96,
    [ValidateRange(48, 960)][int]$ExpectedMonitorDpi = 144,
    # PREREGISTERED oracle: exact artwork/canvas, <=1/channel in <=32 UI pixels.
    [ValidateRange(0, 4096)][int]$UiTolerancePixels = 32,
    [ValidateRange(0, 255)][int]$UiToleranceChannel = 1,
    # Rectangle calibration collected from the maximized state (opt-in).
    [switch]$CalibrateRectangle,
    [string]$CalibrationOutputPath = '',
    [ValidateRange(1, 16)][int]$NativeSaveTimeoutSeconds = 6,
    # --- focused STATIC-ARTWORK mode (bounded; no full 13-state campaign) ------
    # Two corresponding full-4K states per arm. Before EVERY capture a move-only
    # pointer sequence traverses the SAME verified blank pasteboard client point
    # and then the verified CSD caption centre, so the prior ruler-hover state is
    # constrained identically in both arms (caption-only parking did not do this).
    [switch]$StaticArtworkOnly,
    # Rectangle fixture only: add one physical 4K -> 1080 -> 4K transition state.
    [switch]$StaticIncludeMonitorTransition,
    [ValidateRange(0, 4096)][int]$StaticCanvasClientX = 150,
    [ValidateRange(0, 4096)][int]$StaticCanvasClientY = 320,
    [ValidateRange(0, 4096)][int]$StaticCaptionClientY = 30,
    [ValidateRange(0, 5000)][int]$StaticPointerMoveSettleMs = 150,
    [ValidateRange(0, 5000)][int]$StaticPointerFinalSettleMs = 400,
    # --- separate TELEMETRY calibration mode (show_diagnostics=1) --------------
    # Isolated profile with rendering diagnostics ON so VACARDS_RENDER_STATS_LOG
    # actually opens the CSV (src/ui/widget/canvas.cpp gates on show_diagnostics).
    # Never used for paired static PNGs; no pixel-comparison claim.
    [switch]$TelemetryOnly
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$RequiredConsoleSession = 2
$ManagedEnvNames = @(
    'GSK_RENDERER', 'GDK_DISABLE', 'GDK_BACKEND', 'GDK_DEBUG',
    'GDK_WIN32_FORCE_DCOMP', 'GDK_WIN32_CAIRO_GDI_BUFFER',
    'VACARDS_REDRAW_DEADLINE', 'VACARDS_DOCUMENT_DEADLINE',
    'VACARDS_TEST_PROFILE_DIR', 'VACARDS_RENDER_STATS_LOG',
    'VACARDS_FRAME_TIMING', 'VACARDS_INPUT_LATENCY'
)
$StateOrder = @(
    '00-initial', '01-restored-small', '02-restored-large', '03-native-max-4k',
    '04-minimized', '05-minimize-restored', '06-native-max-4k-again',
    '07-occluded', '08-revealed', '09-restored-1080', '10-native-max-1080',
    '11-restored-4k', '12-native-max-4k-return'
)
$PrimaryStates = @(
    '00-initial', '01-restored-small', '02-restored-large', '03-native-max-4k',
    '04-minimized', '05-minimize-restored', '06-native-max-4k-again',
    '07-occluded', '08-revealed', '09-restored-1080', '10-native-max-1080',
    '11-restored-4k', '12-native-max-4k-return'
)
$SW_MINIMIZE = 6

# ---------------------------------------------------------------------------
# Embedded VAPan Win32 interop — extracted byte-exactly from the pinned
# packaging/windows/vacards/Invoke-AppPan.ps1 here-string (verified by -SelfCheck).
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

# ---------------------------------------------------------------------------
# Minimal additional interop (separate class; the VAPan block above is unmodified).
# ---------------------------------------------------------------------------
$VAVisualDiffSource = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class VAVisualNative
{
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr CreateWindowExW(uint dwExStyle, string lpClassName, string lpWindowName, uint dwStyle, int X, int Y, int nWidth, int nHeight, IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool RedrawWindow(IntPtr hWnd, IntPtr lprcUpdate, IntPtr hrgnUpdate, uint flags);
    [DllImport("user32.dll", SetLastError=true)] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    // --- actual DPI-awareness / DWM / monitor-DPI probes (diagnostic truth) ---
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetAwarenessFromDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] public static extern IntPtr GetThreadDpiAwarenessContext();
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr a, IntPtr b);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr hWnd, uint flags);
    [DllImport("shcore.dll")] public static extern int GetDpiForMonitor(IntPtr hmonitor, int dpiType, out uint dpiX, out uint dpiY);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);
    public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
    public const int MDT_EFFECTIVE_DPI = 0;
    public const uint MONITOR_DEFAULTTONEAREST = 2;
    public const int DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4;
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern int GetClassNameW(IntPtr hWnd, System.Text.StringBuilder lpClassName, int nMaxCount);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern int GetWindowTextW(IntPtr hWnd, System.Text.StringBuilder lpWindowName, int nMaxCount);
    public const uint WS_EX_TOPMOST = 0x00000008;
    public const uint WS_EX_TOOLWINDOW = 0x00000080;
    public const uint WS_EX_NOACTIVATE = 0x08000000;
    public const uint WS_POPUP = 0x80000000;
    public const uint WS_VISIBLE = 0x10000000;
    public const uint SS_BLACKRECT = 0x00000004;
    public const uint RDW_INVALIDATE = 0x0001;
    public const uint RDW_UPDATENOW = 0x0100;
    public const uint RDW_ALLCHILDREN = 0x0080;

    public static IntPtr CreateOccluder(int x, int y, int w, int h)
    {
        return CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, "STATIC", "",
            WS_POPUP | WS_VISIBLE | SS_BLACKRECT, x, y, w, h, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
    }
    public static bool ForceRepaint(IntPtr h) { return RedrawWindow(h, IntPtr.Zero, IntPtr.Zero, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN); }
    public static int[] GetRect(IntPtr h) { RECT r; if (!GetWindowRect(h, out r)) return null; return new int[] { r.Left, r.Top, r.Right, r.Bottom }; }
    public static string ClassName(IntPtr h) { System.Text.StringBuilder sb = new System.Text.StringBuilder(256); GetClassNameW(h, sb, sb.Capacity); return sb.ToString(); }
    public static string WindowText(IntPtr h) { System.Text.StringBuilder sb = new System.Text.StringBuilder(512); GetWindowTextW(h, sb, sb.Capacity); return sb.ToString(); }

    // --- diagnostic probes -------------------------------------------------
    public static long GetWindowDpiContextRaw(IntPtr h) { try { return GetWindowDpiAwarenessContext(h).ToInt64(); } catch { return -1; } }
    public static int DecodeWindowAwareness(IntPtr h) { try { return GetAwarenessFromDpiAwarenessContext(GetWindowDpiAwarenessContext(h)); } catch { return -99; } }
    public static int DecodeThreadAwareness() { try { return GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()); } catch { return -99; } }
    public static bool ThreadIsPmv2() { try { return AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), new IntPtr(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)); } catch { return false; } }
    public static int[] DirectClientRect(IntPtr h) { RECT r; if (!GetClientRect(h, out r)) return null; return new int[] { r.Left, r.Top, r.Right, r.Bottom }; }
    // [hr, left, top, right, bottom]; hr != 0 (e.g. E_HANDLE) means unavailable.
    public static int[] DwmExtendedFrameBounds(IntPtr h)
    {
        RECT r; r.Left = 0; r.Top = 0; r.Right = 0; r.Bottom = 0;
        int hr = DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, out r, Marshal.SizeOf(typeof(RECT)));
        return new int[] { hr, r.Left, r.Top, r.Right, r.Bottom };
    }
    // [hr, dpiX, dpiY] effective DPI of the monitor nearest the window.
    public static int[] MonitorDpi(IntPtr h)
    {
        IntPtr mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
        uint x = 0, y = 0;
        int hr = GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, out x, out y);
        return new int[] { hr, (int)x, (int)y };
    }

    // [count, minX, minY, maxX, maxY] of pixels whose RGB is within +/- tol of
    // (r,g,b), alpha ignored. count 0 => minX/minY = -1. Read-only image probe.
    public static long[] FindColorBBox(string path, int r, int g, int b, int tol)
    {
        using (Bitmap a = new Bitmap(path))
        {
            int w = a.Width, h = a.Height;
            Rectangle rect = new Rectangle(0, 0, w, h);
            BitmapData da = a.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            long count = 0;
            int minX = int.MaxValue, minY = int.MaxValue, maxX = -1, maxY = -1;
            try
            {
                int stride = da.Stride;
                byte[] ba = new byte[stride * h];
                Marshal.Copy(da.Scan0, ba, 0, ba.Length);
                for (int y = 0; y < h; y++)
                {
                    int row = y * stride;
                    for (int x = 0; x < w; x++)
                    {
                        int o = row + x * 4;
                        int d0 = Math.Abs(ba[o] - b);     // B
                        int d1 = Math.Abs(ba[o + 1] - g); // G
                        int d2 = Math.Abs(ba[o + 2] - r); // R
                        if (d0 <= tol && d1 <= tol && d2 <= tol)
                        {
                            count++;
                            if (x < minX) minX = x;
                            if (y < minY) minY = y;
                            if (x > maxX) maxX = x;
                            if (y > maxY) maxY = y;
                        }
                    }
                }
            }
            finally { a.UnlockBits(da); }
            if (count == 0) { minX = -1; minY = -1; maxX = -1; maxY = -1; }
            return new long[] { count, minX, minY, maxX, maxY };
        }
    }

    // [r, g, b, a] at (x, y); null when the point is outside the image. Read-only.
    public static int[] GetPixel(string path, int x, int y)
    {
        using (Bitmap a = new Bitmap(path))
        {
            if (x < 0 || y < 0 || x >= a.Width || y >= a.Height) return null;
            Color c = a.GetPixel(x, y);
            return new int[] { c.R, c.G, c.B, c.A };
        }
    }

    // [diff, w, h, 0, 0, maxDelta, minX, minY, maxX, maxY, 0, largeCount, smallCount]
    // largeCount = pixels with a channel delta > uiChannel (hard failures);
    // smallCount = pixels with a channel delta in 1..uiChannel (UI tolerance).
    public static long[] ImageDiff(string pathA, string pathB, int[] exclusions, int uiChannel = 1)
    {
        if (exclusions == null) exclusions = new int[0];
        using (Bitmap a = new Bitmap(pathA))
        using (Bitmap b = new Bitmap(pathB))
        {
            if (a.Width != b.Width || a.Height != b.Height)
                return new long[] { -1, a.Width, a.Height, b.Width, b.Height, 0, -1, -1, -1, -1, 0, -1, -1 };
            int w = a.Width, h = a.Height;
            Rectangle rect = new Rectangle(0, 0, w, h);
            BitmapData da = a.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            BitmapData db = b.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            long diff = 0, maxDelta = 0, largeCount = 0, smallCount = 0;
            int minX = int.MaxValue, minY = int.MaxValue, maxX = -1, maxY = -1;
            try
            {
                int stride = da.Stride;
                byte[] ba = new byte[stride * h];
                byte[] bb = new byte[stride * h];
                Marshal.Copy(da.Scan0, ba, 0, ba.Length);
                Marshal.Copy(db.Scan0, bb, 0, bb.Length);
                for (int y = 0; y < h; y++)
                {
                    int row = y * stride;
                    for (int x = 0; x < w; x++)
                    {
                        bool skip = false;
                        for (int i = 0; i + 3 < exclusions.Length; i += 4)
                        {
                            if (x >= exclusions[i] && x < exclusions[i] + exclusions[i + 2] &&
                                y >= exclusions[i + 1] && y < exclusions[i + 1] + exclusions[i + 3]) { skip = true; break; }
                        }
                        if (skip) continue;
                        int o = row + x * 4;
                        int d0 = Math.Abs(ba[o] - bb[o]);
                        int d1 = Math.Abs(ba[o + 1] - bb[o + 1]);
                        int d2 = Math.Abs(ba[o + 2] - bb[o + 2]);
                        int d3 = Math.Abs(ba[o + 3] - bb[o + 3]);
                        if (d0 != 0 || d1 != 0 || d2 != 0 || d3 != 0)
                        {
                            diff++;
                            int md = Math.Max(Math.Max(d0, d1), Math.Max(d2, d3));
                            if (md > maxDelta) maxDelta = md;
                            if (md > uiChannel) largeCount++; else smallCount++;
                            if (x < minX) minX = x;
                            if (y < minY) minY = y;
                            if (x > maxX) maxX = x;
                            if (y > maxY) maxY = y;
                        }
                    }
                }
            }
            finally { a.UnlockBits(da); b.UnlockBits(db); }
            if (diff == 0) { minX = -1; minY = -1; maxX = -1; maxY = -1; }
            return new long[] { diff, w, h, 0, 0, maxDelta, minX, minY, maxX, maxY, 0, largeCount, smallCount };
        }
    }
}
'@

$vapanCompileError = $null
try {
    if (-not ('VAPan.Win32' -as [type])) { Add-Type -TypeDefinition $VAPanInteropSource -Language CSharp }
} catch { $vapanCompileError = $_.Exception.Message }

$visualCompileError = $null
try {
    Add-Type -AssemblyName System.Drawing -ErrorAction Stop | Out-Null
    if (-not ('VAVisualNative' -as [type])) {
        Add-Type -TypeDefinition $VAVisualDiffSource -Language CSharp -ReferencedAssemblies 'System.Drawing'
    }
} catch { $visualCompileError = $_.Exception.Message }

# Toolbar band rule (root band rule): tolerance applies only to the top
# y < 160 physical client pixels; every pixel outside that band must be exactly
# equal. The paired captures are the full windowValid client region, so image y
# equals client/screenshot y. Fixed, not caller-tunable.
$script:toolbarBandHeightPx = 160

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
$script:occluderHwnd = [IntPtr]::Zero
$script:logPath = $null

function Write-VLog([string]$Message) {
    $line = ([DateTime]::UtcNow.ToString('o') + ' ' + $Message)
    # Write-Host, never Write-Output: Invoke-Arm returns a hashtable and any
    # success-stream output from nested helpers would corrupt that return value.
    try { Write-Host $line } catch { }
    if ($script:logPath) { try { Add-Content -LiteralPath $script:logPath -Value $line -Encoding UTF8 } catch { } }
}
function Resolve-FullPath([string]$Path) { return [IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($Path)) }
function Get-FileSha256([string]$Path) {
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { return ([BitConverter]::ToString($sha.ComputeHash($stream)) -replace '-', '').ToLowerInvariant() }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
}
function Get-TextSha256([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}
function Get-ObjProp($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}
function Compare-BandAwareImage {
    <#
      Band-aware capture comparison in screenshot/client physical pixel
      coordinates (paired captures are the full windowValid client region, so
      image y == client y). The top toolbar band (y < BandHeightPx) may differ
      by at most UiToleranceChannel per channel in at most UiTolerancePixels
      pixels. EVERY pixel at y >= BandHeightPx must be EXACTLY equal. No region
      is discarded: call 1 EXCLUDES the band and measures only the outside
      region (requires diff == 0); call 2 EXCLUDES the outside region and
      measures only the band for the bounded tolerance. Any channel delta
      greater than UiToleranceChannel fails anywhere (outside band this is
      already caught by the exact-diff requirement). Returns an ordered map.
    #>
    param(
        [string]$PathA,
        [string]$PathB,
        [int]$BandHeightPx = 160,
        [int]$UiToleranceChannel = 1,
        [int]$UiTolerancePixels = 32
    )
    $res = [ordered]@{
        ok = $false; status = 'DIFF_ERROR'
        outsideBandDiffPixels = $null; insideBandDiffPixels = $null
        insideBandLargeDiffPixels = $null; insideBandSmallDiffPixels = $null
        maxChannelDelta = $null; diffBox = $null
        bandHeightPx = $BandHeightPx; width = $null; height = $null; error = $null
    }
    try {
        # Call 1: exclude y in [0, BandHeightPx) -> measures only outside the band.
        $rOut = [VAVisualNative]::ImageDiff($PathA, $PathB, [int[]]@(0, 0, 100000, $BandHeightPx), $UiToleranceChannel)
        if ($null -eq $rOut -or $rOut.Length -lt 13) { throw 'outside-band image diff returned no data' }
        if ($rOut[0] -lt 0) {
            $res['status'] = 'SIZE_MISMATCH'; $res['outsideBandDiffPixels'] = -1
            return $res
        }
        $w = [int]$rOut[1]; $h = [int]$rOut[2]
        $res['width'] = $w; $res['height'] = $h
        $res['outsideBandDiffPixels'] = [int]$rOut[0]
        $bandH = [Math]::Min($BandHeightPx, $h)
        if ($bandH -lt 0) { $bandH = 0 }
        $res['bandHeightPx'] = $bandH
        # Call 2: exclude y in [bandH, h) -> measures only the band.
        $rIn = [VAVisualNative]::ImageDiff($PathA, $PathB, [int[]]@(0, $bandH, 100000, [Math]::Max(0, $h - $bandH)), $UiToleranceChannel)
        if ($null -eq $rIn -or $rIn.Length -lt 13) { throw 'inside-band image diff returned no data' }
        $res['insideBandDiffPixels'] = [int]$rIn[0]
        $res['insideBandLargeDiffPixels'] = [int]$rIn[11]
        $res['insideBandSmallDiffPixels'] = [int]$rIn[12]
        $res['maxChannelDelta'] = [Math]::Max([int]$rOut[5], [int]$rIn[5])
        if ([int]$rOut[0] -gt 0) {
            $res['status'] = 'PIXEL_MISMATCH'
            $res['diffBox'] = [ordered]@{ minX = [int]$rOut[6]; minY = [int]$rOut[7]; maxX = [int]$rOut[8]; maxY = [int]$rOut[9] }
        } elseif ([int]$rIn[11] -gt 0) {
            $res['status'] = 'PIXEL_MISMATCH'
            if ([int]$rIn[0] -gt 0) { $res['diffBox'] = [ordered]@{ minX = [int]$rIn[6]; minY = [int]$rIn[7]; maxX = [int]$rIn[8]; maxY = [int]$rIn[9] } }
        } elseif ([int]$rIn[12] -le $UiTolerancePixels) {
            if ([int]$rIn[12] -gt 0) { $res['status'] = 'MATCH_UI_TOLERANCE' } else { $res['status'] = 'MATCH' }
        } else {
            $res['status'] = 'PIXEL_MISMATCH'
            if ([int]$rIn[0] -gt 0) { $res['diffBox'] = [ordered]@{ minX = [int]$rIn[6]; minY = [int]$rIn[7]; maxX = [int]$rIn[8]; maxY = [int]$rIn[9] } }
        }
        $res['ok'] = (@('MATCH', 'MATCH_UI_TOLERANCE') -contains [string]$res['status'])
    } catch {
        $res['status'] = 'DIFF_ERROR'; $res['error'] = $_.Exception.Message
    }
    return $res
}
function Test-ProcessAlive($Process) {
    if ($null -eq $Process) { return $false }
    try { return -not $Process.HasExited } catch { return $true }
}
function Get-ProcessExitInfo($Process) {
    $info = [ordered]@{ processId = $null; hasExited = $null; exitCode = $null }
    if ($null -eq $Process) { return $info }
    try { $info.processId = [int]$Process.Id } catch { return $info }
    try { $info.hasExited = [bool]$Process.HasExited } catch { $info.hasExited = 'unavailable'; return $info }
    if ($info.hasExited) { try { $info.exitCode = [int]$Process.ExitCode } catch { $info.exitCode = 'unavailable' } }
    return $info
}
function Test-PathInside([string]$Path, [string]$RootPath) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullRoot = [IO.Path]::GetFullPath($RootPath)
    if (-not $fullRoot.EndsWith([IO.Path]::DirectorySeparatorChar)) { $fullRoot += [IO.Path]::DirectorySeparatorChar }
    return $fullPath.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)
}
function Set-ManagedEnvironment($Values) {
    foreach ($name in $Values.Keys) {
        $value = $Values[$name]
        if ($null -eq $value -or [string]$value -eq '<cleared>') { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
        else { [Environment]::SetEnvironmentVariable($name, [string]$value, 'Process') }
    }
}
function Restore-ManagedEnvironment([string[]]$Names, $Saved) {
    foreach ($name in $Names) {
        $previous = $Saved[$name]
        if ($null -eq $previous) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
        else { [Environment]::SetEnvironmentVariable($name, [string]$previous, 'Process') }
    }
}
function Get-PhysicalConsoleInfo {
    $info = [ordered]@{
        userInteractive = [Environment]::UserInteractive
        sessionId = $null; activeConsoleSessionId = $null
        protocol = $null; protocolName = 'unknown'; isRemote = $null
        physicalConsole = $false; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    try { $info.sessionId = [int](Get-Process -Id $PID).SessionId } catch { $errs.Add('session id unavailable') }
    try { $info.activeConsoleSessionId = [int][VAPan.Win32]::ActiveConsoleSession() } catch { $errs.Add('WTSGetActiveConsoleSessionId failed') }
    try {
        $w = [VAPan.Win32]::WtsSession([uint32]$info.sessionId)
        if ($null -ne $w -and $w.Length -ge 3) {
            $info.protocol = [int]$w[1]
            $info.isRemote = [bool]($w[2] -eq 1)
            if ($info.isRemote) { $info.protocolName = 'rdp' } elseif ($info.protocol -eq 0) { $info.protocolName = 'console' } else { $info.protocolName = 'unknown' }
            if ($w[0] -ne 1) { $errs.Add('WTS session query incomplete') }
        } else { $errs.Add('WTS session query returned no data') }
    } catch { $errs.Add('WTS session query threw') }
    $info.errors = @($errs)
    $info.physicalConsole = [bool]($info.userInteractive -and ($null -ne $info.sessionId) -and
        ([int]$info.sessionId -eq $RequiredConsoleSession) -and ($null -ne $info.activeConsoleSessionId) -and
        ([int]$info.activeConsoleSessionId -eq $RequiredConsoleSession) -and ($info.protocol -eq 0) -and ($info.isRemote -eq $false))
    return $info
}
function Get-AppAliveInventory {
    $found = @()
    foreach ($n in @('inkscape', 'VACards-Test', 'gtk-cairo-buffer-test', 'bench-runner', 'bench-dispatch')) {
        $ps = @(Get-Process -Name $n -ErrorAction SilentlyContinue)
        if ($ps.Count -gt 0) { $found += ($n + '=' + (($ps | ForEach-Object { $_.Id }) -join ',')) }
    }
    return $found
}
function Get-RectObject($r) {
    return [ordered]@{ left = $r[0]; top = $r[1]; right = $r[2]; bottom = $r[3]; width = ($r[2] - $r[0]); height = ($r[3] - $r[1]) }
}
function Get-WindowIdentity([IntPtr]$Hwnd) {
    # Full identity of one top-level window: class/text/visible/root/PID/
    # foreground/bounds/client. Used to prove the chosen HWND is the owned
    # document window, not a transient GTK startup/splash window.
    $id = [ordered]@{
        hwnd = $Hwnd.ToInt64(); valid = ($Hwnd -ne [IntPtr]::Zero)
        className = $null; windowText = $null; visible = $null; root = $null
        pid = $null; foreground = $null; bounds = $null; client = $null
    }
    if ($Hwnd -eq [IntPtr]::Zero) { return $id }
    try { $id.className = [VAVisualNative]::ClassName($Hwnd) } catch { }
    try { $id.windowText = [VAVisualNative]::WindowText($Hwnd) } catch { }
    try { $id.visible = [bool][VAPan.Win32]::Visible($Hwnd) } catch { }
    try { $r = [VAPan.Win32]::GetAncestor($Hwnd, 2); $id.root = $r.ToInt64() } catch { }
    try { $id.pid = [int][VAPan.Win32]::WindowPid($Hwnd) } catch { }
    try { $id.foreground = [bool][VAPan.Win32]::ForegroundIs($Hwnd) } catch { }
    try { $b = [VAPan.Win32]::WindowBounds($Hwnd); if ($null -ne $b) { $id.bounds = @($b) } } catch { }
    try { $c = [VAPan.Win32]::ClientSize($Hwnd); if ($null -ne $c) { $id.client = @($c) } } catch { }
    return $id
}
function Get-RectFromLtrb([int]$L, [int]$T, [int]$R, [int]$B) {
    return [ordered]@{ left = $L; top = $T; right = $R; bottom = $B; width = ($R - $L); height = ($B - $T) }
}
function Get-RectIntersect($A, $B) {
    if ($null -eq $A -or $null -eq $B) { return $null }
    $l = [Math]::Max([int]$A.left, [int]$B.left)
    $t = [Math]::Max([int]$A.top, [int]$B.top)
    $r = [Math]::Min([int]$A.right, [int]$B.right)
    $b = [Math]::Min([int]$A.bottom, [int]$B.bottom)
    if ($r -le $l -or $b -le $t) { return $null }
    return (Get-RectFromLtrb $l $t $r $b)
}
function Get-DpiAwarenessName([int]$Value) {
    if ($Value -eq -1) { return 'context-per-monitor-aware-v2' }
    if ($Value -eq 0) { return 'unaware' }
    if ($Value -eq 1) { return 'system-aware' }
    if ($Value -eq 2) { return 'per-monitor-aware' }
    return ('unknown(' + [string]$Value + ')')
}
function Get-CaptureRegions($M) {
    # workareaClipped = monitor work rect INTERSECT physical virtual screen.
    # windowValid     = owned client physical rect INTERSECT workareaClipped.
    # Captures are taken only from these, so CopyFromScreen is never offscreen.
    $regions = [ordered]@{ screen = $null; workareaClipped = $null; clientPhysical = $null; windowValid = $null }
    if ($null -eq $M -or $null -eq $M.workRect -or $null -eq $M.clientRect -or $null -eq $M.physicalClientOrigin) { return $regions }
    try {
        $v = [VAPan.Win32]::VirtualScreen()
        if ($null -ne $v -and $v.Length -ge 4) {
            $regions.screen = Get-RectFromLtrb $v[0] $v[1] ($v[0] + $v[2]) ($v[1] + $v[3])
        }
    } catch { }
    $regions.workareaClipped = if ($null -ne $regions.screen) { Get-RectIntersect $M.workRect $regions.screen } else { $M.workRect }
    $cx = [int]$M.physicalClientOrigin.x; $cy = [int]$M.physicalClientOrigin.y
    $cw = [int]$M.clientRect.width; $ch = [int]$M.clientRect.height
    if ($cw -gt 0 -and $ch -gt 0) {
        $regions.clientPhysical = Get-RectFromLtrb $cx $cy ($cx + $cw) ($cy + $ch)
        $regions.windowValid = Get-RectIntersect $regions.clientPhysical $regions.workareaClipped
    }
    return $regions
}
function Measure-WindowState([IntPtr]$Hwnd, [string]$Label) {
    $m = [ordered]@{
        label = $Label; utc = [DateTime]::UtcNow.ToString('o'); hwnd = $Hwnd.ToInt64()
        validHwnd = ($Hwnd -ne [IntPtr]::Zero)
        isZoomed = $null; isIconic = $null; showCmd = $null; outerRect = $null; clientRect = $null
        physicalClientOrigin = $null; dpiForWindow = $null; monitorDevice = $null
        monitorRect = $null; workRect = $null; foregroundIsOwned = $null; ownerIsApp = $null
        windowClass = $null; windowText = $null; visible = $null; pid = $null
        windowDpiAwarenessRaw = $null; windowDpiAwareness = $null; windowDpiAwarenessName = $null
        threadDpiAwareness = $null; threadDpiAwarenessName = $null; threadIsPmv2 = $null
        dwmHr = $null; dwmExtendedFrameBounds = $null
        clientRectDirect = $null; windowRectDirect = $null
        monitorDpiHr = $null; monitorDpiX = $null; monitorDpiY = $null
        containment = $null; captureRegions = $null
    }
    if ($Hwnd -eq [IntPtr]::Zero) { return $m }
    try { $m.ownerIsApp = ([VAPan.Win32]::WindowPid($Hwnd) -eq [uint32]$script:currentAppPid) } catch { }
    try { $m.isZoomed = [bool][VAPan.Win32]::Maximized($Hwnd) } catch { }
    try { $m.isIconic = [bool][VAPan.Win32]::Minimized($Hwnd) } catch { }
    try {
        $wp = [VAPan.Win32]::WindowPlacementInfo($Hwnd)
        if ($null -ne $wp -and $wp.Length -ge 6) { $m.showCmd = [int]$wp[0] }
    } catch { }
    try { $wr = [VAPan.Win32]::WindowBounds($Hwnd); if ($null -ne $wr -and $wr.Length -ge 4) { $m.outerRect = Get-RectObject $wr } } catch { }
    try { $cs = [VAPan.Win32]::ClientSize($Hwnd); if ($null -ne $cs -and $cs.Length -ge 2) { $m.clientRect = [ordered]@{ width = $cs[0]; height = $cs[1] } } } catch { }
    try { $o = [VAPan.Win32]::ClientScreenOrigin($Hwnd); if ($null -ne $o -and $o.Length -ge 2) { $m.physicalClientOrigin = [ordered]@{ x = $o[0]; y = $o[1] } } } catch { }
    try { $m.dpiForWindow = [int][VAPan.Win32]::DpiForWindow($Hwnd) } catch { }
    try { $m.monitorDevice = [string][VAPan.Win32]::MonitorDevice($Hwnd) } catch { }
    try {
        $mr = [VAPan.Win32]::MonitorRects($Hwnd)
        if ($null -ne $mr -and $mr.Length -ge 8) {
            $m.monitorRect = Get-RectFromLtrb $mr[0] $mr[1] $mr[2] $mr[3]
            $m.workRect = Get-RectFromLtrb $mr[4] $mr[5] $mr[6] $mr[7]
        }
    } catch { }
    try { $m.foregroundIsOwned = [bool][VAPan.Win32]::ForegroundIs($Hwnd) } catch { }
    # --- actual window identity (title/class/PID) at the measured instant -----
    try { $m.windowClass = [VAVisualNative]::ClassName($Hwnd) } catch { }
    try { $m.windowText = [VAVisualNative]::WindowText($Hwnd) } catch { }
    try { $m.visible = [bool][VAPan.Win32]::Visible($Hwnd) } catch { }
    try { $m.pid = [int][VAPan.Win32]::WindowPid($Hwnd) } catch { }
    # --- actual DPI-awareness APIs (never assumed) ----------------------------
    try { $m.windowDpiAwarenessRaw = [long][VAVisualNative]::GetWindowDpiContextRaw($Hwnd) } catch { }
    try {
        $m.windowDpiAwareness = [int][VAVisualNative]::DecodeWindowAwareness($Hwnd)
        $m.windowDpiAwarenessName = Get-DpiAwarenessName $m.windowDpiAwareness
    } catch { }
    try {
        $m.threadDpiAwareness = [int][VAVisualNative]::DecodeThreadAwareness()
        $m.threadDpiAwarenessName = Get-DpiAwarenessName $m.threadDpiAwareness
    } catch { }
    try { $m.threadIsPmv2 = [bool][VAVisualNative]::ThreadIsPmv2() } catch { }
    # --- DWM extended frame bounds + direct GetClientRect/GetWindowRect -------
    try {
        $db = [VAVisualNative]::DwmExtendedFrameBounds($Hwnd)
        if ($null -ne $db -and $db.Length -ge 5) {
            $m.dwmHr = [int]$db[0]
            if ($db[0] -eq 0) { $m.dwmExtendedFrameBounds = Get-RectFromLtrb $db[1] $db[2] $db[3] $db[4] }
        }
    } catch { }
    try { $cc = [VAVisualNative]::DirectClientRect($Hwnd); if ($null -ne $cc -and $cc.Length -ge 4) { $m.clientRectDirect = Get-RectFromLtrb $cc[0] $cc[1] $cc[2] $cc[3] } } catch { }
    try { $ww = [VAVisualNative]::GetRect($Hwnd); if ($null -ne $ww -and $ww.Length -ge 4) { $m.windowRectDirect = Get-RectFromLtrb $ww[0] $ww[1] $ww[2] $ww[3] } } catch { }
    # --- monitor DPI separately from window DPI -------------------------------
    try {
        $md = [VAVisualNative]::MonitorDpi($Hwnd)
        if ($null -ne $md -and $md.Length -ge 3) { $m.monitorDpiHr = [int]$md[0]; $m.monitorDpiX = [int]$md[1]; $m.monitorDpiY = [int]$md[2] }
    } catch { }
    # --- containment proof (existing VAPan helper + client-vs-work) -----------
    $insideMonitor = $false
    try { $insideMonitor = [bool][VAPan.Win32]::BoundsInsideMonitor($Hwnd) } catch { }
    $clientInsideWork = $false
    if ($null -ne $m.clientRect -and $null -ne $m.physicalClientOrigin -and $null -ne $m.workRect) {
        $cx = [int]$m.physicalClientOrigin.x; $cy = [int]$m.physicalClientOrigin.y
        $clientInsideWork = [bool](($cx -ge $m.workRect.left) -and ($cy -ge $m.workRect.top) -and
            (($cx + [int]$m.clientRect.width) -le $m.workRect.right) -and (($cy + [int]$m.clientRect.height) -le $m.workRect.bottom))
    }
    $m.containment = [ordered]@{ windowBoundsInsideMonitor = $insideMonitor; clientInsideWorkArea = $clientInsideWork }
    $m.captureRegions = Get-CaptureRegions $m
    return $m
}
function Save-Screenshot([int]$X, [int]$Y, [int]$Width, [int]$Height, [string]$Path) {
    if ($Width -le 0 -or $Height -le 0) { return $false }
    try {
        $bmp = New-Object System.Drawing.Bitmap($Width, $Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try { $g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size($Width, $Height)), [System.Drawing.CopyPixelOperation]::SourceCopy) }
            finally { $g.Dispose() }
            $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally { $bmp.Dispose() }
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { Write-VLog ('SCREENSHOT_MISSING ' + $Path); return $false }
        # Re-open the saved PNG to prove it exists with the requested valid
        # dimensions; a zero-byte/truncated file is a hard capture failure.
        try {
            $check = New-Object System.Drawing.Bitmap($Path)
            try {
                if ([int]$check.Width -ne $Width -or [int]$check.Height -ne $Height) {
                    Write-VLog ('SCREENSHOT_BADDIM ' + $Path + ' got=' + [string]$check.Width + 'x' + [string]$check.Height + ' want=' + [string]$Width + 'x' + [string]$Height)
                    return $false
                }
            } finally { $check.Dispose() }
        } catch {
            Write-VLog ('SCREENSHOT_DECODE_FAIL ' + $Path + ' :: ' + $_.Exception.Message)
            return $false
        }
        return $true
    } catch {
        Write-VLog ('SCREENSHOT_FAIL ' + $Path + ' :: ' + $_.Exception.Message)
        return $false
    }
}
function Get-VapanBlockText([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    $m = [regex]::Match($text, '(?s)\$VAPanInteropSource\s*=\s*@''\r?\n(.*?)\r?\n''@')
    if (-not $m.Success) { return $null }
    return ($m.Groups[1].Value -replace "`r`n", "`n")
}
function Get-PreferencesXml {
    param([int]$ShowDiagnostics = 0)
    return @"
<inkscape version="1">
  <group id="options">
    <group id="rendering" show_diagnostics="$ShowDiagnostics" request_opengl="0" windows_accelerated="0"/>
    <group id="savewindowgeometry" value="2"/>
  </group>
  <group id="desktop">
    <group id="geometry" width="1280" height="800" maximized="0" fullscreen="0"/>
  </group>
</inkscape>
"@
}
function Get-CenteredPosition([int[]]$WorkRect, [int]$W, [int]$H) {
    $x = [int]($WorkRect[4] + (($WorkRect[6] - $WorkRect[4]) - $W) / 2)
    $y = [int]($WorkRect[5] + (($WorkRect[7] - $WorkRect[5]) - $H) / 2)
    return @($x, $y)
}
function Get-GeometryCanon($g) {
    $c = [ordered]@{}
    foreach ($k in @('isZoomed', 'isIconic', 'showCmd', 'dpiForWindow', 'monitorDevice', 'foregroundIsOwned', 'ownerIsApp')) {
        if ($g.Contains($k)) { $c[$k] = $g[$k] } else { $c[$k] = $null }
    }
    foreach ($r in @('outerRect', 'clientRect', 'monitorRect', 'workRect')) {
        $v = $g[$r]
        # clientRect carries only width/height; Get-ObjProp keeps this robust for
        # every rect shape instead of throwing under StrictMode on a missing edge.
        if ($null -eq $v) { $c[$r] = $null } else {
            $c[$r] = @(
                (Get-ObjProp $v 'left'), (Get-ObjProp $v 'top'), (Get-ObjProp $v 'right'), (Get-ObjProp $v 'bottom'),
                (Get-ObjProp $v 'width'), (Get-ObjProp $v 'height'))
        }
    }
    $o = $g['physicalClientOrigin']
    if ($null -eq $o) { $c['physicalClientOrigin'] = $null } else { $c['physicalClientOrigin'] = @($o.x, $o.y) }
    return $c
}
function Add-State([IntPtr]$Hwnd, [string]$Label, $Arm, [string]$ArmDir, [bool]$DoCapture, [switch]$StaticPointerSequence) {
    # Re-assert foreground on the owned window immediately before measuring and
    # capturing, so the settled frame is the owned client, not an overlapping
    # window. When SetForegroundWindow is denied (GTK CSD exposes no Win32
    # HTCAPTION), the reviewed opt-in one-click CSD caption policy is applied.
    # Foreground errors are recorded by Measure-WindowState, not hidden.
    $focusRecord = $null
    $cursorPark = $null
    if ($DoCapture -and $Hwnd -ne [IntPtr]::Zero) {
        try { [void][VAPan.Win32]::Foreground($Hwnd) } catch { }
        Start-Sleep -Milliseconds 200
        if (-not [VAPan.Win32]::ForegroundIs($Hwnd)) {
            try {
                $focusRecord = Invoke-VisualCaptionFocus -WindowHandle $Hwnd -AllowVerifiedCsdCaption:$AllowVerifiedCsdCaption
            } catch {
                $focusRecord = [ordered]@{ schema = 'vacards-app-visual-preparatory-focus/1'; attempted = $true; reason = ('exception: ' + $_.Exception.Message); foreground_after_wait = $false }
            }
            Start-Sleep -Milliseconds 200
        } else {
            $focusRecord = [ordered]@{ schema = 'vacards-app-visual-preparatory-focus/1'; attempted = $false; reason = 'already-foreground'; foreground_after_wait = $true }
        }
        # Optional (default OFF) cursor park at the SAME verified safe caption
        # point in BOTH arms. The v2 pilot showed this WORSENED the ruler-marker
        # mismatch, so it is not part of the default policy.
        if ($ParkCursorOnCaption -and $AllowVerifiedCsdCaption -and [VAPan.Win32]::ForegroundIs($Hwnd)) {
            try { $cursorPark = Move-VisualCursorToCaption -WindowHandle $Hwnd } catch { }
            Start-Sleep -Milliseconds 250
        }
        # Static-artwork policy: move-only through the verified blank canvas point
        # and then the verified caption centre BEFORE measuring/capturing, so the
        # prior ruler cursor-marker state is constrained identically in both arms.
        if ($StaticPointerSequence -and [VAPan.Win32]::ForegroundIs($Hwnd)) {
            try { $cursorPark = Move-VisualCursorThroughCanvasThenCaption -WindowHandle $Hwnd } catch {
                $cursorPark = [ordered]@{ schema = 'vacards-app-visual-cursor-sequence/1'; attempted = $true; moved = $false; reason = ('exception: ' + $_.Exception.Message) }
            }
        }
    }
    $m = Measure-WindowState $Hwnd $Label
    $captureRecord = $null
    $workareaRecord = $null
    if ($DoCapture -and $Hwnd -ne [IntPtr]::Zero) {
        # In-bounds owned-window intersection only: client physical rect INTERSECT
        # (monitor work rect INTERSECT physical virtual screen). Never offscreen.
        $regions = $m.captureRegions
        $winReg = if ($null -ne $regions) { $regions.windowValid } else { $null }
        if ($null -eq $winReg -or [int]$winReg.width -le 0 -or [int]$winReg.height -le 0) {
            Write-VLog ('CAPTURE_REGION_INVALID ' + $Arm.mode + ' ' + $Label + ' (no in-bounds owned intersection)')
        } else {
            $pngPath = Join-Path $ArmDir ($Label + '.png')
            if (Save-Screenshot -X ([int]$winReg.left) -Y ([int]$winReg.top) -Width ([int]$winReg.width) -Height ([int]$winReg.height) -Path $pngPath) {
                $canvasPix = $null; $captionPix = $null
                $captionClientX = [int][math]::Floor([int]$winReg.width / 2)
                try { $canvasPix = [VAVisualNative]::GetPixel($pngPath, [int]$StaticCanvasClientX, [int]$StaticCanvasClientY) } catch { }
                try { $captionPix = [VAVisualNative]::GetPixel($pngPath, $captionClientX, [int]$StaticCaptionClientY) } catch { }
                $captureRecord = [ordered]@{
                    path = $pngPath; region = 'windowValid'
                    x = [int]$winReg.left; y = [int]$winReg.top; width = [int]$winReg.width; height = [int]$winReg.height
                    windowValidRect = @($winReg.left, $winReg.top, $winReg.right, $winReg.bottom)
                    clientPhysicalRect = if ($null -ne $regions.clientPhysical) { @($regions.clientPhysical.left, $regions.clientPhysical.top, $regions.clientPhysical.right, $regions.clientPhysical.bottom) } else { $null }
                    workareaClippedRect = if ($null -ne $regions.workareaClipped) { @($regions.workareaClipped.left, $regions.workareaClipped.top, $regions.workareaClipped.right, $regions.workareaClipped.bottom) } else { $null }
                    sha256 = (Get-FileSha256 $pngPath); sizeBytes = (Get-Item -LiteralPath $pngPath).Length
                    canvasPointClient = @([int]$StaticCanvasClientX, [int]$StaticCanvasClientY)
                    canvasPointPixel = $canvasPix
                    captionPointClient = @($captionClientX, [int]$StaticCaptionClientY)
                    captionPointPixel = $captionPix
                }
            }
        }
    }
    # Diagnostic only: additionally capture the monitor work area clipped to the
    # physical screen (the full target geometry), so the actual window can be
    # located inside the whole output even when the client is DPI-virtualized.
    if ($Diagnostic -and $DoCapture -and $Hwnd -ne [IntPtr]::Zero) {
        $regions = $m.captureRegions
        $wa = if ($null -ne $regions) { $regions.workareaClipped } else { $null }
        if ($null -ne $wa -and [int]$wa.width -gt 0 -and [int]$wa.height -gt 0) {
            $waPath = Join-Path $ArmDir ($Label + '-workarea.png')
            if (Save-Screenshot -X ([int]$wa.left) -Y ([int]$wa.top) -Width ([int]$wa.width) -Height ([int]$wa.height) -Path $waPath) {
                $workareaRecord = [ordered]@{
                    path = $waPath; region = 'workareaClipped'
                    x = [int]$wa.left; y = [int]$wa.top; width = [int]$wa.width; height = [int]$wa.height
                    sha256 = (Get-FileSha256 $waPath); sizeBytes = (Get-Item -LiteralPath $waPath).Length
                }
            }
        }
    }
    $Arm.states[$Label] = [ordered]@{ label = $Label; geometry = $m; capture = $captureRecord; workarea = $workareaRecord; focus = $focusRecord; cursorPark = $cursorPark }
    $clientText = '?'
    if ($null -ne $m.clientRect) { $clientText = [string]$m.clientRect.width + 'x' + [string]$m.clientRect.height }
    $originText = '?'
    if ($null -ne $m.physicalClientOrigin) { $originText = [string]$m.physicalClientOrigin.x + ',' + [string]$m.physicalClientOrigin.y }
    Write-VLog ('STATE ' + $Arm.mode + ' ' + $Label + ' captured=' + [bool]($null -ne $captureRecord) +
        ' wa=' + [bool]($null -ne $workareaRecord) +
        ' client=' + $clientText + ' origin=' + $originText +
        ' zoomed=' + [string]$m.isZoomed + ' iconic=' + [string]$m.isIconic + ' showCmd=' + [string]$m.showCmd +
        ' dpi=' + [string]$m.dpiForWindow + ' monDpi=' + [string]$m.monitorDpiX +
        ' aware=' + [string]$m.windowDpiAwarenessName + ' fg=' + [string]$m.foregroundIsOwned)
}
function Close-Occluder {
    if ($script:occluderHwnd -ne [IntPtr]::Zero) {
        try { [void][VAVisualNative]::DestroyWindow($script:occluderHwnd) } catch { }
        $script:occluderHwnd = [IntPtr]::Zero
    }
}

function Invoke-VisualCaptionFocus {
    <#
      Wrapper-local port of the reviewed Invoke-AppPan.ps1
      Invoke-PreparatoryCaptionFocus POLICY (same checks, same opt-in). ONE
      user-like click on a TEMPORARILY TOPMOST-verified point of ONLY the owned
      HWND: a point whose own WM_NCHITTEST is HTCAPTION, or - only when the
      opt-in is set - the ONE fixed client-side-titlebar point
      ClientScreenOrigin + (ClientW/2, 30) with DPI 96 and its own hit-test
      HTCLIENT. Ownership/PID/HTCLIENT are re-verified before the move and at the
      actual cursor point before the press; the temporary WS_EX_TOPMOST is always
      restored and verified. No close/maximize button is ever clicked.
    #>
    param(
        [IntPtr]$WindowHandle,
        [switch]$AllowVerifiedCsdCaption
    )
    $res = [ordered]@{
        schema = 'vacards-app-visual-preparatory-focus/1'
        attempted = $true; precondition_ok = $false; reason = $null; method = $null
        csd_optin = [bool]$AllowVerifiedCsdCaption; csd_dpi = $null; csd_client_point = $null
        brought_to_top = $null; point_screen = $null; hittest = $null; hittest_ok = $null
        click_delivered = $false; left_down_return = $null; left_up_return = $null
        foreground_before = $null; foreground_after = $null; foreground_after_wait = $false
        topmost_used = $false; topmost_orig_exstyle_topmost = $null
        topmost_restore_call_ok = $null; topmost_after_restore = $null
        topmost_restore_verified = $null; foreground_after_restore = $null
        point_owned_after_restore = $null
    }
    $topmostAttempted = $false; $origTopmost = $false; $px = $null; $py = $null; $targetPid = 0
    try {
        if ($WindowHandle -eq [IntPtr]::Zero) { $res.reason = 'zero hwnd'; return $res }
        $targetPid = [int][VAPan.Win32]::WindowPid($WindowHandle)
        if ($targetPid -le 0 -or -not [VAPan.Win32]::Visible($WindowHandle)) { $res.reason = 'not owned/visible'; return $res }
        $res.foreground_before = [bool][VAPan.Win32]::ForegroundIs($WindowHandle)
        $cs = [VAPan.Win32]::ClientSize($WindowHandle)
        if ($null -eq $cs -or $cs[0] -le 0 -or $cs[1] -le 0) { $res.reason = 'no client size'; return $res }
        $clientW = [int]$cs[0]; $clientH = [int]$cs[1]
        $res.precondition_ok = $true
        $res.brought_to_top = [bool][VAPan.Win32]::BringToTopNoActivate($WindowHandle)
        $requiredHt = 0
        $find = [VAPan.Win32]::FindCaptionPoint($WindowHandle, [uint32]$targetPid, 250, 100, 16, 4)
        if ($find[4] -eq 1) {
            $res.method = 'native_htcaption'; $requiredHt = 2; $px = $find[0]; $py = $find[1]
        } elseif ($AllowVerifiedCsdCaption) {
            $dpi = [int][VAPan.Win32]::DpiForWindow($WindowHandle)
            $res.csd_dpi = $dpi
            if ($dpi -ne [int]$ExpectedWindowDpi) { $res.reason = ('CSD opt-in requires window dpi ' + $ExpectedWindowDpi + ' (observed ' + $dpi + ')'); return $res }
            $cx = [int][math]::Floor($clientW / 2); $cy = 30
            if ($cx -lt 0 -or $cy -lt 0 -or $cx -ge $clientW -or $cy -ge $clientH) { $res.reason = 'CSD fixed client point outside the client'; return $res }
            $org = [VAPan.Win32]::ClientScreenOrigin($WindowHandle)
            $px = [int]$org[0] + $cx; $py = [int]$org[1] + $cy
            $res.csd_client_point = @($cx, $cy)
            $res.method = 'visually_verified_beta5_csd_titlebar'
            $rootPre = [VAPan.Win32]::RootFromPoint($px, $py)
            $res.root_check_before_move = ($rootPre -eq $WindowHandle)
            $res.root_pid_before_move = [int][VAPan.Win32]::WindowPid($rootPre)
            if (-not $res.root_check_before_move -or $res.root_pid_before_move -ne $targetPid) {
                $topmostAttempted = $true
                $origTopmost = [bool][VAPan.Win32]::IsTopmost($WindowHandle)
                $res.topmost_orig_exstyle_topmost = $origTopmost
                $setTop = [bool][VAPan.Win32]::SetTopmostNoActivate($WindowHandle)
                $res.topmost_used = $setTop
                if (-not $setTop) { $res.reason = 'temporary topmost SetWindowPos call failed'; return $res }
                $rootRaise = [VAPan.Win32]::RootFromPoint($px, $py)
                $htRaiseOk = 0
                $htRaise = [VAPan.Win32]::HitTestBounded($WindowHandle, $px, $py, 250, [ref]$htRaiseOk)
                $res.point_owned_after_raise = (($rootRaise -eq $WindowHandle) -and ([int][VAPan.Win32]::WindowPid($rootRaise) -eq $targetPid))
                if (-not $res.point_owned_after_raise -or $htRaiseOk -ne 1 -or $htRaise -ne 1) { $res.reason = 'CSD point not owned/HTCLIENT after topmost raise'; return $res }
            }
            $requiredHt = 1
        } else {
            $res.reason = 'no verified HTCAPTION point and CSD opt-in is disabled'; return $res
        }
        $res.point_screen = @($px, $py)
        $rootNow = [VAPan.Win32]::RootFromPoint($px, $py)
        $res.root_is_target = ($rootNow -eq $WindowHandle)
        $res.root_pid = [int][VAPan.Win32]::WindowPid($rootNow)
        $htOk = 0
        $htNow = [VAPan.Win32]::HitTestBounded($WindowHandle, $px, $py, 250, [ref]$htOk)
        $res.hittest = $htNow; $res.hittest_ok = $htOk
        if (-not $res.root_is_target -or $res.root_pid -ne $targetPid -or $htOk -ne 1 -or $htNow -ne $requiredHt) { $res.reason = ('verified point not root-owned/expected hittest ' + $requiredHt + ' before move'); return $res }
        $cur = [VAPan.Win32]::CursorClient($WindowHandle)
        $abs = [VAPan.Win32]::ScreenToAbsoluteForMode($px, $py, $false)
        if ($null -eq $abs) { $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3]); $res.reason = 'point not representable on the virtual desktop'; return $res }
        $res.move_sendinput_return = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOVE_FLAGS, $abs[0], $abs[1])
        $curAt = [VAPan.Win32]::CursorClient($WindowHandle)
        $res.move_confirmed = (($res.move_sendinput_return -eq 1) -and ($curAt[2] -eq $px) -and ($curAt[3] -eq $py))
        if (-not $res.move_confirmed) { $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3]); $res.reason = 'SendInput move not confirmed at the intended point'; return $res }
        $rootPress = [VAPan.Win32]::RootFromPoint($curAt[2], $curAt[3])
        $htPressOk = 0
        $htPress = [VAPan.Win32]::HitTestBounded($WindowHandle, $curAt[2], $curAt[3], 250, [ref]$htPressOk)
        if (($rootPress -ne $WindowHandle) -or ([int][VAPan.Win32]::WindowPid($rootPress) -ne $targetPid) -or $htPressOk -ne 1 -or $htPress -ne $requiredHt) {
            $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3]); $res.reason = 'actual cursor point not owned/expected hittest before press'; return $res
        }
        $res.left_down_return = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTDOWN, 0, 0)
        $res.left_up_return = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP, 0, 0)
        $res.click_delivered = (($res.left_down_return -eq 1) -and ($res.left_up_return -eq 1))
        $null = [VAPan.Win32]::SetCursorPos($cur[2], $cur[3])
        $fgDeadline = [DateTime]::UtcNow.AddSeconds(3)
        while ([DateTime]::UtcNow -lt $fgDeadline) { if ([VAPan.Win32]::ForegroundIs($WindowHandle)) { break }; Start-Sleep -Milliseconds 50 }
        $res.foreground_after = [bool][VAPan.Win32]::ForegroundIs($WindowHandle)
        $res.foreground_after_wait = ($res.click_delivered -and $res.foreground_after)
        if (-not $res.click_delivered) { $res.reason = 'preparatory click not delivered' } elseif (-not $res.foreground_after_wait) { $res.reason = 'owned HWND still not foreground after the preparatory click' }
        return $res
    } catch {
        $res.reason = ('preparatory-focus exception: ' + $_.Exception.Message)
        return $res
    } finally {
        if ($topmostAttempted) {
            try {
                $res.topmost_restore_call_ok = [bool][VAPan.Win32]::RestoreTopmostNoActivate($WindowHandle, $origTopmost)
                $res.topmost_after_restore = [bool][VAPan.Win32]::IsTopmost($WindowHandle)
                $res.topmost_restore_verified = ($res.topmost_after_restore -eq $origTopmost)
                $rootAfter = [IntPtr]::Zero
                if ($null -ne $px -and $null -ne $py) { $rootAfter = [VAPan.Win32]::RootFromPoint([int]$px, [int]$py) }
                $res.point_owned_after_restore = (($rootAfter -eq $WindowHandle) -and ([int][VAPan.Win32]::WindowPid($rootAfter) -eq $targetPid))
                $res.foreground_after_restore = [bool][VAPan.Win32]::ForegroundIs($WindowHandle)
                $res.foreground_after_wait = ([bool]$res.click_delivered -and [bool]$res.foreground_after -and [bool]$res.topmost_restore_verified -and [bool]$res.foreground_after_restore -and [bool]$res.point_owned_after_restore)
                if (-not $res.topmost_restore_verified) { $res.reason = 'temporary topmost restoration not verified; not a success' }
                elseif (-not $res.foreground_after_restore) { $res.reason = 'owned HWND not foreground after topmost restore; not a success' }
                elseif (-not $res.point_owned_after_restore) { $res.reason = 'planned point not owned after topmost restore; not a success' }
            } catch {
                $res.topmost_restore_verified = $false; $res.foreground_after_wait = $false
                $res.reason = ('topmost restore exception: ' + $_.Exception.Message)
            }
        }
    }
}

function Move-VisualCursorToCaption {
    <#
      MOVE-ONLY cursor park at the verified safe CSD caption point
      ClientScreenOrigin + (ClientW/2, 30). No click, so it cannot toggle
      foreground or a button; used to make the ruler/pointer history identical in
      both arms before a capture.
    #>
    param([IntPtr]$WindowHandle)
    $res = [ordered]@{ schema = 'vacards-app-visual-cursor-park/1'; attempted = $false; moved = $false; point_screen = $null; method = $null; reason = $null }
    try {
        if (-not [VAPan.Win32]::ForegroundIs($WindowHandle)) { $res.reason = 'not foreground'; return $res }
        $cs = [VAPan.Win32]::ClientSize($WindowHandle)
        if ($null -eq $cs -or $cs[0] -le 0) { $res.reason = 'no client size'; return $res }
        $org = [VAPan.Win32]::ClientScreenOrigin($WindowHandle)
        $px = [int]$org[0] + [int][math]::Floor($cs[0] / 2)
        $py = [int]$org[1] + 30
        $abs = [VAPan.Win32]::ScreenToAbsoluteForMode($px, $py, $false)
        if ($null -eq $abs) { $res.reason = 'point not representable'; return $res }
        $res.attempted = $true
        $r = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOVE_FLAGS, $abs[0], $abs[1])
        Start-Sleep -Milliseconds 120
        $cur = [VAPan.Win32]::CursorClient($WindowHandle)
        $res.moved = (($r -eq 1) -and ($cur[2] -eq $px) -and ($cur[3] -eq $py))
        $res.point_screen = @($px, $py)
        $res.method = 'caption-park-move-only'
        if (-not $res.moved) { $res.reason = 'SendInput move not confirmed at the caption point' }
    } catch { $res.reason = ('cursor park exception: ' + $_.Exception.Message) }
    return $res
}

function Move-VisualCursorThroughCanvasThenCaption {
    <#
      MOVE-ONLY pointer sequence for the bounded static-artwork comparison. It
      first moves (no click) to the verified blank pasteboard canvas client point
      (StaticCanvasClientX/Y), then to the verified CSD caption centre
      (ClientW/2, StaticCaptionClientY), settling between and after. Both points
      are re-verified as root-owned HTCLIENT before and after the move; no press
      is ever delivered, so no UI command/button can fire. This constrains the
      prior ruler cursor-marker position identically in both arms, which
      caption-only parking did NOT do (v2 made every state differ).
    #>
    param([IntPtr]$WindowHandle)
    $res = [ordered]@{
        schema = 'vacards-app-visual-cursor-sequence/1'; attempted = $false; moved = $false
        method = 'move-only-canvas-then-caption'; reason = $null
        canvas_client_point = $null; caption_client_point = $null
        canvas_screen_point = $null; caption_screen_point = $null
        canvas_hit_ok = $null; caption_hit_ok = $null
        canvas_owned = $null; caption_owned = $null
        canvas_move_return = $null; caption_move_return = $null
        canvas_move_confirmed = $null; caption_move_confirmed = $null
        foreground_after = $null
    }
    try {
        if (-not [VAPan.Win32]::ForegroundIs($WindowHandle)) { $res.reason = 'not foreground'; return $res }
        $targetPid = [int][VAPan.Win32]::WindowPid($WindowHandle)
        if ($targetPid -le 0) { $res.reason = 'zero window pid'; return $res }
        $cs = [VAPan.Win32]::ClientSize($WindowHandle)
        if ($null -eq $cs -or $cs[0] -le 0 -or $cs[1] -le 0) { $res.reason = 'no client size'; return $res }
        $cw = [int]$cs[0]; $ch = [int]$cs[1]
        $canvasX = [int]$StaticCanvasClientX; $canvasY = [int]$StaticCanvasClientY
        $captionX = [int][math]::Floor($cw / 2); $captionY = [int]$StaticCaptionClientY
        if ($canvasX -ge $cw -or $canvasY -ge $ch) { $res.reason = 'canvas client point outside the client'; return $res }
        if ($captionY -ge $ch) { $res.reason = 'caption client point outside the client'; return $res }
        $org = [VAPan.Win32]::ClientScreenOrigin($WindowHandle)
        $res.canvas_client_point = @($canvasX, $canvasY)
        $res.caption_client_point = @($captionX, $captionY)
        $res.attempted = $true
        $first = $true
        foreach ($pt in @(@($canvasX, $canvasY), @($captionX, $captionY))) {
            $px = [int]$org[0] + [int]$pt[0]; $py = [int]$org[1] + [int]$pt[1]
            # pixel-center mode: exact-pixel round-trip so the confirmation below
            # is meaningful (legacy 65535/(extent-1) maps y=30 -> 29 and would
            # refuse an otherwise valid move; see NormalizeAbsolutePixelCenter).
            $abs = [VAPan.Win32]::ScreenToAbsoluteForMode($px, $py, $true)
            if ($null -eq $abs) { $res.reason = 'point not representable on the virtual desktop'; return $res }
            $r = [int][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOVE_FLAGS, $abs[0], $abs[1])
            Start-Sleep -Milliseconds $StaticPointerMoveSettleMs
            $cur = [VAPan.Win32]::CursorClient($WindowHandle)
            $confirmed = (($r -eq 1) -and ($cur[2] -eq $px) -and ($cur[3] -eq $py))
            $rootNow = [VAPan.Win32]::RootFromPoint($px, $py)
            $htOk = 0
            $ht = [VAPan.Win32]::HitTestBounded($WindowHandle, $px, $py, 250, [ref]$htOk)
            $owned = (($rootNow -eq $WindowHandle) -and ([int][VAPan.Win32]::WindowPid($rootNow) -eq $targetPid))
            if ($first) {
                $res.canvas_move_return = $r; $res.canvas_move_confirmed = $confirmed
                $res.canvas_owned = $owned; $res.canvas_hit_ok = $htOk; $res.canvas_hittest = $ht
                $res.canvas_screen_point = @($px, $py)
                if (-not $confirmed) { $res.reason = 'canvas SendInput move not confirmed at the intended point'; return $res }
                if (-not ($owned -and $htOk -eq 1 -and $ht -eq 1)) { $res.reason = 'canvas point not owned/HTCLIENT'; return $res }
                $first = $false
            } else {
                $res.caption_move_return = $r; $res.caption_move_confirmed = $confirmed
                $res.caption_owned = $owned; $res.caption_hit_ok = $htOk; $res.caption_hittest = $ht
                $res.caption_screen_point = @($px, $py)
                if (-not $confirmed) { $res.reason = 'caption SendInput move not confirmed at the intended point'; return $res }
                if (-not ($owned -and $htOk -eq 1)) { $res.reason = 'caption point not owned HTCLIENT'; return $res }
            }
        }
        Start-Sleep -Milliseconds $StaticPointerFinalSettleMs
        $res.moved = $true
        $res.foreground_after = [bool][VAPan.Win32]::ForegroundIs($WindowHandle)
    } catch { $res.reason = ('cursor sequence exception: ' + $_.Exception.Message) }
    return $res
}

function Invoke-MinimizeStaleOccluders {
    <#
      Minimize ONLY specifically identified stale test/DisplaySettings windows in
      the active physical console session (record title/class/PID/session/
      show-state). Never closes or kills anything. Restoration is the caller's
      responsibility in finally.
    #>
    $records = @()
    foreach ($name in @('WindowsTerminal', 'SystemSettings', 'ApplicationFrameHost')) {
        foreach ($p in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            try {
                if ([int]$p.SessionId -ne $RequiredConsoleSession) { continue }
                $h = [IntPtr]$p.MainWindowHandle
                if ($h -eq [IntPtr]::Zero) { continue }
                $rec = [ordered]@{
                    processName = $name; processId = [int]$p.Id; sessionId = [int]$p.SessionId
                    hwnd = $h.ToInt64(); windowClass = $null; windowTitle = $null
                    originalShowCmd = $null; originalIconic = $null; minimizedByUs = $false
                }
                try { $rec.windowClass = [VAVisualNative]::ClassName($h) } catch { }
                try { $rec.windowTitle = [VAVisualNative]::WindowText($h) } catch { }
                try { $rec.originalIconic = [bool][VAPan.Win32]::Minimized($h) } catch { }
                try { $wp = [VAPan.Win32]::WindowPlacementInfo($h); if ($null -ne $wp -and $wp.Length -ge 1) { $rec.originalShowCmd = [int]$wp[0] } } catch { }
                if ($rec.originalIconic -ne $true) {
                    [void][VAPan.Win32]::ShowWindow($h, 6)   # SW_MINIMIZE
                    Start-Sleep -Milliseconds 200
                    $rec.minimizedByUs = [bool][VAPan.Win32]::Minimized($h)
                }
                $records += $rec
            } catch { }
        }
    }
    return $records
}

function Restore-StaleOccluders($Records) {
    foreach ($rec in @($Records)) {
        try {
            if ($null -eq $rec -or $rec.minimizedByUs -ne $true) { continue }
            $h = [IntPtr][int64]$rec.hwnd
            if ($h -eq [IntPtr]::Zero) { continue }
            [void][VAPan.Win32]::ShowWindow($h, 9)   # SW_RESTORE
            Start-Sleep -Milliseconds 150
        } catch { }
    }
}

function Get-NamedviewZoom([string]$SvgPath) {
    try {
        if (-not (Test-Path -LiteralPath $SvgPath -PathType Leaf)) { return $null }
        $text = [IO.File]::ReadAllText($SvgPath)
        $m = [regex]::Match($text, 'inkscape:zoom\s*=\s*"([0-9]+(?:\.[0-9]+)?)"')
        if (-not $m.Success) { return $null }
        return [double]$m.Groups[1].Value
    } catch { return $null }
}

function Get-CanvasStats([string]$CsvPath) {
    $stats = [ordered]@{ scale = $null; logicalWidth = $null; logicalHeight = $null; gtkRenderer = $null; canvasBackend = $null; rows = 0 }
    try {
        if (-not (Test-Path -LiteralPath $CsvPath -PathType Leaf)) { return $stats }
        $rows = @(Import-Csv -LiteralPath $CsvPath)
        $stats.rows = $rows.Count
        $last = $null
        foreach ($r in $rows) {
            if ($null -ne $r.scale -and [string]$r.scale -ne '' -and -not [string]::IsNullOrWhiteSpace([string]$r.logical_width) -and [int]$r.logical_width -gt 0) { $last = $r }
        }
        if ($null -ne $last) {
            $stats.scale = [double]$last.scale
            $stats.logicalWidth = [int]$last.logical_width
            $stats.logicalHeight = [int]$last.logical_height
            $stats.gtkRenderer = [string]$last.gtk_renderer
            $stats.canvasBackend = [string]$last.canvas_backend
        }
    } catch { }
    return $stats
}

function Invoke-NativeCtrlS([IntPtr]$WindowHandle, [string]$DocumentPath, [int]$TimeoutSeconds) {
    $res = [ordered]@{ schema = 'vacards-app-visual-native-save/1'; attempted = $true; foreground = $null; wrote = $false; reason = $null; sha256 = $null; mtimeUtc = $null }
    try {
        if (-not [VAPan.Win32]::ForegroundIs($WindowHandle)) { $res.reason = 'owned HWND is not foreground before Ctrl+S'; return $res }
        $res.foreground = $true
        $before = if (Test-Path -LiteralPath $DocumentPath) { (Get-Item -LiteralPath $DocumentPath).LastWriteTimeUtc } else { [DateTime]::MinValue }
        $null = [VAPan.Win32]::SendKey([uint16][VAPan.Win32]::VK_CONTROL, $false)
        Start-Sleep -Milliseconds 40
        $null = [VAPan.Win32]::SendKey([uint16][VAPan.Win32]::VK_S, $false)
        Start-Sleep -Milliseconds 40
        $null = [VAPan.Win32]::SendKey([uint16][VAPan.Win32]::VK_S, $true)
        Start-Sleep -Milliseconds 40
        $null = [VAPan.Win32]::SendKey([uint16][VAPan.Win32]::VK_CONTROL, $true)
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while ([DateTime]::UtcNow -lt $deadline) {
            Start-Sleep -Milliseconds 200
            if (Test-Path -LiteralPath $DocumentPath -PathType Leaf) {
                $after = (Get-Item -LiteralPath $DocumentPath).LastWriteTimeUtc
                if ($after -gt $before) { $res.wrote = $true; $res.mtimeUtc = $after.ToString('o'); break }
            }
        }
        if (-not $res.wrote) { $res.reason = 'no fresh native save observed within the bound'; return $res }
        $res.sha256 = Get-FileSha256 $DocumentPath
        return $res
    } catch {
        $res.reason = ('native save exception: ' + $_.Exception.Message); return $res
    }
}

function Get-RectangleCalibration {
    param([string]$ClientPng, [string]$SavedSvg, [string]$CsvPath, $Geometry)
    $cal = [ordered]@{
        schema = 'vacards-maximized-drag-calibration/1'
        calibration_id = 'maximized-drag-4k-display2-next4-20260922'
        monitor_device = $null; monitor_bounds = @(); work_bounds = @()
        dpi = $null; monitor_dpi = $null
        window_dpi = $null; client_width = $null; client_height = $null
        canvas_scale = $null; canvas_logical_width = $null; canvas_logical_height = $null
        namedview_zoom = $null; blue_exact_bbox_client = $null; blue_exact_count = $null
        blue_near_bbox_client = $null; blue_exact_width = $null; blue_exact_height = $null
        select_client_x = $null; select_client_y = $null
        select_screen_x = $null; select_screen_y = $null
        physical_pixels_per_logical_pixel = $null
        factor_formula = 'observed_blue_width / (400 * namedview_zoom)'
        notes = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    try {
        if ($null -ne $Geometry) {
            if ($null -ne $Geometry.monitorDevice) { $cal.monitor_device = [string]$Geometry.monitorDevice }
            if ($null -ne $Geometry.monitorRect) { $cal.monitor_bounds = @([int]$Geometry.monitorRect.left, [int]$Geometry.monitorRect.top, [int]$Geometry.monitorRect.right, [int]$Geometry.monitorRect.bottom) }
            if ($null -ne $Geometry.workRect) { $cal.work_bounds = @([int]$Geometry.workRect.left, [int]$Geometry.workRect.top, [int]$Geometry.workRect.right, [int]$Geometry.workRect.bottom) }
            # vacards-maximized-drag-calibration/1 `dpi` is the APP WINDOW DPI
            # (Read-MaximizedCalibration / Test-MaximizedGeometryState). The
            # monitor DPI is preserved separately and used only for the observer.
            if ($null -ne $Geometry.dpiForWindow) { $cal.dpi = [int]$Geometry.dpiForWindow; $cal.window_dpi = [int]$Geometry.dpiForWindow }
            if ($null -ne $Geometry.monitorDpiX) { $cal.monitor_dpi = [int]$Geometry.monitorDpiX }
            if ($null -ne $Geometry.clientRect) { $cal.client_width = [int]$Geometry.clientRect.width; $cal.client_height = [int]$Geometry.clientRect.height }
        }
        $stats = Get-CanvasStats $CsvPath
        $cal.canvas_scale = $stats.scale; $cal.canvas_logical_width = $stats.logicalWidth; $cal.canvas_logical_height = $stats.logicalHeight
        $cal.namedview_zoom = Get-NamedviewZoom $SavedSvg
        if (Test-Path -LiteralPath $ClientPng -PathType Leaf) {
            $exact = [VAVisualNative]::FindColorBBox($ClientPng, 32, 64, 128, 0)
            if ($null -ne $exact -and $exact[0] -gt 0) {
                $cal.blue_exact_count = [long]$exact[0]
                $cal.blue_exact_bbox_client = @([int]$exact[1], [int]$exact[2], [int]$exact[3], [int]$exact[4])
                $cal.blue_exact_width = [int]$exact[3] - [int]$exact[1] + 1
                $cal.blue_exact_height = [int]$exact[4] - [int]$exact[2] + 1
                $cal.select_client_x = [int]$exact[1] + [int][math]::Floor(($cal.blue_exact_width) / 2)
                $cal.select_client_y = [int]$exact[2] + [int][math]::Floor(($cal.blue_exact_height) / 2)
                if ($null -ne $Geometry -and $null -ne $Geometry.physicalClientOrigin) {
                    $cal.select_screen_x = [int]$Geometry.physicalClientOrigin.x + [int]$cal.select_client_x
                    $cal.select_screen_y = [int]$Geometry.physicalClientOrigin.y + [int]$cal.select_client_y
                }
            } else { [void]$errs.Add('exact #204080 fill not found in the maximized client PNG') }
            $near = [VAVisualNative]::FindColorBBox($ClientPng, 32, 64, 128, 8)
            if ($null -ne $near -and $near[0] -gt 0) { $cal.blue_near_bbox_client = @([int]$near[1], [int]$near[2], [int]$near[3], [int]$near[4]) }
        } else { [void]$errs.Add('maximized client PNG is missing') }
        if ($null -ne $cal.namedview_zoom -and $cal.namedview_zoom -gt 0 -and $null -ne $cal.blue_exact_width) {
            $cal.physical_pixels_per_logical_pixel = [math]::Round(([double]$cal.blue_exact_width / (400.0 * [double]$cal.namedview_zoom)), 6)
        } else {
            [void]$errs.Add('namedview zoom or blue width unavailable; factor NOT estimated')
        }
        $cal.notes = @('factor derived from the observed exact-colour blue fill vs 400 document units times the saved namedview zoom; never assumed 1 or 1.5')
    } catch { [void]$errs.Add('calibration exception: ' + $_.Exception.Message) }
    $cal.calibration_errors = @($errs)
    $cal.ok = ($errs.Count -eq 0)
    return $cal
}

function Get-ObserverRoiConfig {
    param($Calibration, [int]$VerticalInset = 12, [int]$MarginPx = 100, [int]$RoiHeight = 8, [int]$TravelPx = 120)
    $roi = [ordered]@{
        schema = 'vacards-screen-observer-roi/1'; roi_id = 'next4-maximized-drag-display2'
        monitor_device = $Calibration.monitor_device; monitor_bounds = $Calibration.monitor_bounds
        monitor_dpi = $Calibration.monitor_dpi
        desktop_x = $null; desktop_y = $null; desktop_width = $null; desktop_height = $null
        margin_px = $MarginPx; travel_px = $TravelPx
        expected_blue_width = $null
        select_client_x = $Calibration.select_client_x; select_client_y = $Calibration.select_client_y
        select_screen_x = $Calibration.select_screen_x; select_screen_y = $Calibration.select_screen_y
        ok = $false; reason = $null
        notes = @('ROI includes a background margin before the left blue edge and the FULL +120px travel budget; expected_blue_width is the full measured target run width (observer-hook equality validator is under repair)')
    }
    try {
        $bb = $Calibration.blue_exact_bbox_client
        $mon = $Calibration.monitor_bounds
        $work = $Calibration.work_bounds
        if ($null -eq $bb -or $null -eq $mon -or $bb.Count -ne 4 -or $mon.Count -ne 4) { $roi.reason = 'missing blue bbox or monitor bounds'; return $roi }
        $blueW = [int]$Calibration.blue_exact_width
        $blueH = [int]$Calibration.blue_exact_height
        $roiW = $blueW + $TravelPx + (2 * $MarginPx)
        if ($roiW -gt 1024) { $roi.reason = ('ROI width ' + $roiW + ' exceeds the 1024 budget; refusing to crop the trajectory'); return $roi }
        if (($roiW * $RoiHeight) -gt 65536) { $roi.reason = 'ROI pixel budget exceeded'; return $roi }
        $roi.desktop_x = [int]$mon[0] + [int]$bb[0] - $MarginPx
        $roi.desktop_y = [int]$mon[1] + [int]$bb[1] + $VerticalInset
        $roi.desktop_width = $roiW
        $roi.desktop_height = $RoiHeight
        $roi.expected_blue_width = $blueW
        # Strip must be vertically inside the blue fill, above the cursor row.
        $cursorClientY = [int]$Calibration.select_client_y
        $stripClientY = $roi.desktop_y - [int]$mon[1]
        if ($stripClientY -lt [int]$bb[1] -or ($stripClientY + $RoiHeight) -gt ([int]$bb[1] + $blueH)) { $roi.reason = 'ROI strip is not inside the blue fill vertically'; return $roi }
        if ($cursorClientY -lt ($stripClientY + $RoiHeight)) { $roi.reason = 'cursor row is not below the ROI strip'; return $roi }
        if ($null -ne $work -and $work.Count -eq 4) {
            if ($roi.desktop_x -lt [int]$work[0] -or ($roi.desktop_x + $roiW) -gt [int]$work[2] -or
                $roi.desktop_y -lt [int]$work[1] -or ($roi.desktop_y + $RoiHeight) -gt [int]$work[3]) { $roi.reason = 'ROI lies outside the work area'; return $roi }
        }
        if ($null -ne $Calibration.select_screen_x) {
            $cx = [int]$Calibration.select_screen_x; $cy = [int]$Calibration.select_screen_y
            if (($cx -ge $roi.desktop_x) -and ($cx -lt ($roi.desktop_x + $roiW)) -and ($cy -ge $roi.desktop_y) -and ($cy -lt ($roi.desktop_y + $RoiHeight))) {
                $roi.reason = 'cursor point lies inside the ROI strip'; return $roi
            }
        }
        $roi.ok = $true
    } catch { $roi.reason = ('ROI exception: ' + $_.Exception.Message) }
    return $roi
}

function Stop-OwnedProcesses($AppProcess, $AppPid, $LauncherProcess, $LauncherPid) {
    $killed = @()
    foreach ($item in @(
        [ordered]@{ name = 'app'; process = $AppProcess; pid = $AppPid },
        [ordered]@{ name = 'launcher'; process = $LauncherProcess; pid = $LauncherPid }
    )) {
        if ($null -eq $item.process) { continue }
        if (-not (Test-ProcessAlive $item.process)) { continue }
        try {
            if ([int]$item.process.Id -ne [int]$item.pid) { continue }
            $item.process.Kill(); $killed += ($item.name + ':' + $item.pid)
        } catch { }
    }
    return $killed
}

# ---------------------------------------------------------------------------
# Self-check (no GUI)
# ---------------------------------------------------------------------------
if ($SelfCheck) {
    $selfPath = $SelfPath
    if ([string]::IsNullOrWhiteSpace($selfPath)) { $selfPath = $PSCommandPath }
    $checks = [ordered]@{
        schema = 'vacards.app-visual-selfcheck/1'
        utc = [DateTime]::UtcNow.ToString('o')
        selfPath = $selfPath; root = $Root; appRoot = $AppRoot
        parseOk = $false; parseErrors = @()
        vapanCompileOk = $false; vapanCompileError = $vapanCompileError
        visualCompileOk = $false; visualCompileError = $visualCompileError
        embeddedBlockSha256 = $null; stagedBlockSha256 = $null; blockMatches = $null
        pins = [ordered]@{}; fixture = [ordered]@{}; rootExists = $false; runDirExists = $null
        ok = $false; errors = @()
    }
    $errs = New-Object System.Collections.Generic.List[string]
    try {
        if ([string]::IsNullOrWhiteSpace($selfPath) -or -not (Test-Path -LiteralPath $selfPath -PathType Leaf)) {
            $errs.Add('SelfPath is missing or not a file')
        } else {
            $tokens = $null; $perrors = $null
            [void][System.Management.Automation.Language.Parser]::ParseFile($selfPath, [ref]$tokens, [ref]$perrors)
            if ($perrors.Count -eq 0) { $checks.parseOk = $true }
            else { foreach ($e in $perrors) { $errs.Add('parse: ' + $e.Message) }; $checks.parseErrors = @($perrors | ForEach-Object { $_.Message }) }
        }
        $checks.vapanCompileOk = ($null -eq $vapanCompileError)
        if (-not $checks.vapanCompileOk) { $errs.Add('VAPan compile failed: ' + [string]$vapanCompileError) }
        $checks.visualCompileOk = ($null -eq $visualCompileError)
        if (-not $checks.visualCompileOk) { $errs.Add('visual interop compile failed: ' + [string]$visualCompileError) }

        if ($selfPath -and (Test-Path -LiteralPath $selfPath -PathType Leaf)) {
            $embedded = Get-VapanBlockText $selfPath
            if ($null -eq $embedded) { $errs.Add('embedded VAPan block not found in self') }
            else { $checks.embeddedBlockSha256 = Get-TextSha256 $embedded }
        }
        if ([string]::IsNullOrWhiteSpace($PanInteropSourcePath) -or -not (Test-Path -LiteralPath $PanInteropSourcePath -PathType Leaf)) {
            $errs.Add('PanInteropSourcePath is missing or not a file')
        } else {
            $staged = Get-VapanBlockText $PanInteropSourcePath
            if ($null -eq $staged) { $errs.Add('VAPan block not found in staged Invoke-AppPan.ps1') }
            else { $checks.stagedBlockSha256 = Get-TextSha256 $staged }
        }
        if ($null -ne $checks.embeddedBlockSha256 -and $null -ne $checks.stagedBlockSha256) {
            $checks.blockMatches = ($checks.embeddedBlockSha256 -eq $checks.stagedBlockSha256)
            if (-not $checks.blockMatches) { $errs.Add('embedded VAPan block does not match the staged Invoke-AppPan.ps1 block') }
        }

        if ([string]::IsNullOrWhiteSpace($AppRoot) -or -not (Test-Path -LiteralPath $AppRoot -PathType Container)) {
            $errs.Add('AppRoot is missing or not a directory')
        } else {
            foreach ($pair in @(
                @('VACards-Test.exe', 'VACards-Test.exe', ''),
                @('bin/inkscape.exe', 'bin\inkscape.exe', $ExpectedExeSha256),
                @('bin/libgtk-4-1.dll', 'bin\libgtk-4-1.dll', $ExpectedGtkSha256),
                @('bin/libcairo-2.dll', 'bin\libcairo-2.dll', $ExpectedCairoSha256),
                @('bin/libglib-2.0-0.dll', 'bin\libglib-2.0-0.dll', $ExpectedGlibSha256))) {
                $p = Join-Path $AppRoot $pair[1]
                if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $errs.Add('missing AppRoot file ' + $pair[1]); continue }
                $h = Get-FileSha256 $p
                $exp = [string]$pair[2]
                $checks.pins[$pair[0]] = [ordered]@{ actual = $h; expected = if ($exp) { $exp.ToLowerInvariant() } else { $null }; matches = (($exp -eq '') -or ($h -eq $exp.ToLowerInvariant())) }
                if ($exp -ne '' -and $h -ne $exp.ToLowerInvariant()) { $errs.Add('pin mismatch for ' + $pair[1]) }
            }
        }
        if ([string]::IsNullOrWhiteSpace($FixtureSource) -or -not (Test-Path -LiteralPath $FixtureSource -PathType Leaf)) {
            $errs.Add('FixtureSource is missing or not a file')
        } else {
            $fh = Get-FileSha256 $FixtureSource
            $checks.fixture = [ordered]@{ path = $FixtureSource; actual = $fh; expected = $ExpectedFixtureSha256.ToLowerInvariant(); matches = ($fh -eq $ExpectedFixtureSha256.ToLowerInvariant()) }
            if (-not $checks.fixture.matches) { $errs.Add('fixture hash mismatch') }
        }
        if ([string]::IsNullOrWhiteSpace($Root) -or -not (Test-Path -LiteralPath $Root -PathType Container)) {
            $errs.Add('Root is missing or not a directory')
        } else {
            $checks.rootExists = $true
            $checks.runDirExists = (Test-Path -LiteralPath (Join-Path $Root 'visual-output'))
        }
    } catch { $errs.Add('selfcheck threw: ' + $_.Exception.Message) }
    $checks.errors = @($errs)
    $checks.ok = ($errs.Count -eq 0)
    if ($Root -and (Test-Path -LiteralPath $Root -PathType Container)) {
        try { [IO.File]::WriteAllText((Join-Path $Root 'SELFCHECK.json'), ($checks | ConvertTo-Json -Depth 10), [System.Text.UTF8Encoding]::new($false)) } catch { }
    }
    ($checks | ConvertTo-Json -Depth 10) | Write-Output
    if ($checks.ok) { exit 0 }
    exit 1
}

# ===========================================================================
# GUI run
# ===========================================================================
$state = [ordered]@{
    schema = 'vacards.app-visual-regression/1'
    status = 'initializing'
    verdict = 'INVALID'
    failure = $null
    errors = @()
    warnings = @()
    root = $null; outputRoot = $null
    session = [ordered]@{}
    pinned = [ordered]@{}
    display = [ordered]@{}
    environment = [ordered]@{}
    arms = [ordered]@{}
    compare = [ordered]@{}
    unavailable = @(
        'menus/dialog states are not supported by this wrapper and are Unexecuted in this packet',
        'static render/resize/pan checks for the three editable/bitmap/PDF fixtures are separate bounded cases and may be Unexecuted here',
        'no complex native object drag / Undo / save-reopen coverage is claimed from these static checks'
    )
    diagnostic = [bool]$Diagnostic
    capturedUtc = $null
}
$validationErrors = New-Object System.Collections.Generic.List[string]
$summaryPath = $null
$markerPath = $null
if (-not [string]::IsNullOrWhiteSpace($Root)) { $markerPath = Join-Path $Root 'PILOT-COMPLETE.json' }
$outputCreated = $false
$savedEnv = $null
$threadPrevRaw = $null
$threadPromoted = $false

function Add-ValidationError([string]$Message) {
    [void]$validationErrors.Add($Message)
    $state.warnings += $Message
}

try {
    if ([string]::IsNullOrWhiteSpace($Root) -or -not (Test-Path -LiteralPath $Root -PathType Container)) { throw ('Root does not exist: ' + $Root) }
    if ($null -ne $vapanCompileError) { throw ('VAPan interop did not compile: ' + $vapanCompileError) }
    if ($null -ne $visualCompileError) { throw ('visual interop did not compile: ' + $visualCompileError) }
    $script:logPath = Join-Path $Root 'visual-regression.log'
    $markerPath = Join-Path $Root 'PILOT-COMPLETE.json'
    $state.root = (Resolve-FullPath $Root)
    $appRootFull = Resolve-FullPath $AppRoot
    $fixtureSourceFull = Resolve-FullPath $FixtureSource
    Write-VLog ('VISUAL_START root=' + $state.root + ' appRoot=' + $appRootFull)

    $state.session = Get-PhysicalConsoleInfo
    if (-not $state.session.physicalConsole) {
        throw ('Refusing a non-physical-console run: session=' + $state.session.sessionId +
               ' activeConsole=' + $state.session.activeConsoleSessionId + ' protocol=' + $state.session.protocolName +
               ' remote=' + $state.session.isRemote + '; require interactive physical console session 2 with no RDP.')
    }
    $alive = @(Get-AppAliveInventory)
    $state.preexistingAppProcesses = $alive
    if ($alive.Count -gt 0) { throw ('Refusing: pre-existing VA Studio/test process(es): ' + ($alive -join ', ')) }

    foreach ($pair in @(
        @('VACards-Test.exe', 'VACards-Test.exe'),
        @('bin/inkscape.exe', 'bin\inkscape.exe'),
        @('bin/libgtk-4-1.dll', 'bin\libgtk-4-1.dll'),
        @('bin/libcairo-2.dll', 'bin\libcairo-2.dll'),
        @('bin/libglib-2.0-0.dll', 'bin\libglib-2.0-0.dll'))) {
        if (-not (Test-Path -LiteralPath (Join-Path $appRootFull $pair[1]) -PathType Leaf)) { throw ('AppRoot is missing ' + $pair[1]) }
    }
    $actualPins = [ordered]@{
        exe = Get-FileSha256 (Join-Path $appRootFull 'bin\inkscape.exe')
        gtk = Get-FileSha256 (Join-Path $appRootFull 'bin\libgtk-4-1.dll')
        cairo = Get-FileSha256 (Join-Path $appRootFull 'bin\libcairo-2.dll')
        glib = Get-FileSha256 (Join-Path $appRootFull 'bin\libglib-2.0-0.dll')
        launcher = Get-FileSha256 (Join-Path $appRootFull 'VACards-Test.exe')
    }
    $expectedPins = [ordered]@{
        exe = $ExpectedExeSha256.ToLowerInvariant()
        gtk = $ExpectedGtkSha256.ToLowerInvariant()
        cairo = $ExpectedCairoSha256.ToLowerInvariant()
        glib = if ($ExpectedGlibSha256) { $ExpectedGlibSha256.ToLowerInvariant() } else { '' }
    }
    $state.pinned = [ordered]@{ expected = $expectedPins; actual = $actualPins
        exeMatches = ($actualPins.exe -eq $expectedPins.exe)
        gtkMatches = ($actualPins.gtk -eq $expectedPins.gtk)
        cairoMatches = ($actualPins.cairo -eq $expectedPins.cairo)
        glibMatches = (($expectedPins.glib -eq '') -or ($actualPins.glib -eq $expectedPins.glib))
    }
    if (-not ($state.pinned.exeMatches -and $state.pinned.gtkMatches -and $state.pinned.cairoMatches)) { throw 'Pinned exe/GTK/Cairo hash mismatch on disk; refusing to launch.' }
    if (-not $state.pinned.glibMatches) { Add-ValidationError 'libglib-2.0-0.dll on-disk hash differs from the supplied pin (runtime check still authoritative).' }

    if (-not (Test-Path -LiteralPath $fixtureSourceFull -PathType Leaf)) { throw ('Fixture source missing: ' + $fixtureSourceFull) }
    $fixtureActual = Get-FileSha256 $fixtureSourceFull
    if ($fixtureActual -ne $ExpectedFixtureSha256.ToLowerInvariant()) { throw ('Fixture source sha256 ' + $fixtureActual + ' != pinned ' + $ExpectedFixtureSha256) }

    $outputRoot = Join-Path $state.root 'visual-output'
    if (Test-Path -LiteralPath $outputRoot) { throw ('Output directory already exists; refusing to overwrite: ' + $outputRoot) }
    $null = New-Item -ItemType Directory -Path $outputRoot -ErrorAction Stop
    $outputCreated = $true
    $state.outputRoot = $outputRoot
    $summaryPath = Join-Path $outputRoot 'visual-regression-result.json'
    $sharedDir = Join-Path $outputRoot 'shared'
    $null = New-Item -ItemType Directory -Path $sharedDir
    $sharedFixture = Join-Path $sharedDir 'fixture-simple.svg'
    Copy-Item -LiteralPath $fixtureSourceFull -Destination $sharedFixture
    $state.fixture = [ordered]@{ source = $fixtureSourceFull; sharedCopy = $sharedFixture; sha256 = (Get-FileSha256 $sharedFixture); expectedSha256 = $ExpectedFixtureSha256.ToLowerInvariant() }
    Write-VLog ('SHARED_FIXTURE ' + $sharedFixture + ' sha=' + $state.fixture.sha256)

    # Promote the calling thread to PER_MONITOR_AWARE_V2 (restored in finally).
    $est = [VAPan.Win32]::EstablishPerMonitorV2()
    if ($null -ne $est -and $est.Length -ge 3 -and $est[0] -eq 1) {
        $threadPromoted = $true
        $threadPrevRaw = $est[2]
    }
    $state.display = [ordered]@{
        threadPmv2 = $threadPromoted
        threadPmv2Verified = [bool][VAPan.Win32]::CurrentThreadIsPmv2()
        virtualScreen = @([VAPan.Win32]::VirtualScreen())
        expectedMonitorDevice = $ExpectedMonitorDevice
        expectedMonitor = @($ExpectedMonitorLeft, $ExpectedMonitorTop, $ExpectedMonitorRight, $ExpectedMonitorBottom)
        expectedWork = @($ExpectedWorkRight, $ExpectedWorkBottom)
        expectedDpi = $ExpectedDpi
        expectedWindowDpi = $ExpectedWindowDpi
        expectedMonitorDpi = $ExpectedMonitorDpi
        allowVerifiedCsdCaption = [bool]$AllowVerifiedCsdCaption
        minimizeStaleOccluders = [bool]$MinimizeStaleOccluders
        uiTolerancePixels = $UiTolerancePixels
        uiToleranceChannel = $UiToleranceChannel
        toolbarBandHeightPx = $script:toolbarBandHeightPx
        bandTolerancePolicy = 'exact outside y<toolbarBandHeightPx; <=uiToleranceChannel/channel in <=uiTolerancePixels inside'
        calibrateRectangle = [bool]$CalibrateRectangle
        diagnostic = [bool]$Diagnostic
        expectedSecondaryDevice = $ExpectedSecondaryDevice
        expectedSecondaryMonitor = @($ExpectedSecondaryLeft, $ExpectedSecondaryTop, $ExpectedSecondaryRight, $ExpectedSecondaryBottom)
        expectedSecondaryWork = @($ExpectedSecondaryWorkRight, $ExpectedSecondaryWorkBottom)
        expectedSecondaryDpi = $ExpectedSecondaryDpi
    }
    if (-not $state.display.threadPmv2Verified) { Add-ValidationError 'calling thread is not verified PER_MONITOR_AWARE_V2; geometry may be DPI-virtualized.' }

    $savedEnv = [ordered]@{}
    foreach ($name in $ManagedEnvNames) { $savedEnv[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }

    # -----------------------------------------------------------------------
    # Arm runner
    # -----------------------------------------------------------------------
    function Invoke-Arm {
        param([string]$Mode, [string]$ArmDir)
        $arm = [ordered]@{
            mode = $Mode; armDir = $ArmDir; status = 'initializing'
            environment = [ordered]@{}; profile = [ordered]@{}; launch = [ordered]@{}
            modules = [ordered]@{}; states = [ordered]@{}; exit = [ordered]@{}
            errors = @(); launchMs = $null; occluder = $null
        }
        $armErrs = New-Object System.Collections.Generic.List[string]
        $profileDir = Join-Path $ArmDir 'profile'
        $null = New-Item -ItemType Directory -Path $profileDir
        $null = New-Item -ItemType Directory -Path (Join-Path $profileDir 'Cache')
        $preferencesPath = Join-Path $profileDir 'preferences.xml'
        # show_diagnostics controls whether VACARDS_RENDER_STATS_LOG actually opens
        # its CSV (src/ui/widget/canvas.cpp gates on set_rendering_stats_enabled).
        # It is ON only for the separate TELEMETRY run; paired static PNGs always
        # use 0 so the dynamic overlay is never part of the comparison.
        $showDiagnostics = if ($TelemetryOnly) { 1 } else { 0 }
        [IO.File]::WriteAllText($preferencesPath, (Get-PreferencesXml -ShowDiagnostics $showDiagnostics), [System.Text.UTF8Encoding]::new($false))
        $arm.profile = [ordered]@{ directory = $profileDir; preferencesPath = $preferencesPath; showDiagnostics = $showDiagnostics; isolated = $true }
        $csvPath = Join-Path $ArmDir 'rendering.csv'
        $stdoutPath = Join-Path $ArmDir 'app-stdout.log'
        $stderrPath = Join-Path $ArmDir 'app-stderr.log'
        $gdiLiteral = if ($Mode -eq 'disabled') { '0' } else { '<cleared>' }
        $appliedEnv = [ordered]@{
            GSK_RENDERER = 'cairo'
            GDK_DISABLE = 'dcomp'
            GDK_BACKEND = 'win32'
            GDK_DEBUG = '<cleared>'
            GDK_WIN32_FORCE_DCOMP = '<cleared>'
            GDK_WIN32_CAIRO_GDI_BUFFER = $gdiLiteral
            VACARDS_REDRAW_DEADLINE = '<cleared>'
            VACARDS_DOCUMENT_DEADLINE = '<cleared>'
            VACARDS_TEST_PROFILE_DIR = $profileDir
            VACARDS_RENDER_STATS_LOG = $csvPath
            VACARDS_FRAME_TIMING = '1'
            VACARDS_INPUT_LATENCY = '1'
        }
        $arm.environment = [ordered]@{
            requested = $appliedEnv
            gdiBufferControl = $gdiLiteral
            note = 'Process-scoped only; original values restored in finally. show_diagnostics=0 divergence documented in APPROACH.md.'
        }
        $tag = 'vavisual' + [guid]::NewGuid().ToString('N')
        Set-ManagedEnvironment $appliedEnv
        Write-VLog ('LAUNCH ' + $Mode + ' tag=' + $tag + ' fixture=' + $sharedFixture)
        $launchStart = [DateTime]::UtcNow
        $launcherProcess = $null; $appProcess = $null; $launcherPid = $null; $appPid = $null
        $stdoutStream = $null; $stderrStream = $null; $stdoutTask = $null; $stderrTask = $null
        try {
            $psi = New-Object System.Diagnostics.ProcessStartInfo
            $psi.FileName = Join-Path $appRootFull 'VACards-Test.exe'
            $psi.Arguments = '--app-id-tag=' + $tag + ' "' + $sharedFixture + '"'
            $psi.UseShellExecute = $false
            $psi.RedirectStandardOutput = $true
            $psi.RedirectStandardError = $true
            $psi.CreateNoWindow = $false
            $psi.WorkingDirectory = $appRootFull
            foreach ($key in $appliedEnv.Keys) {
                if ($appliedEnv[$key] -eq '<cleared>') { $null = $psi.EnvironmentVariables.Remove($key) }
                else { $psi.EnvironmentVariables[$key] = [string]$appliedEnv[$key] }
            }
            $launcherProcess = New-Object System.Diagnostics.Process
            $launcherProcess.StartInfo = $psi
            if (-not $launcherProcess.Start()) { throw 'Could not start VACards-Test.exe.' }
            $launcherPid = [int]$launcherProcess.Id
            $stdoutStream = [IO.File]::Create($stdoutPath)
            $stderrStream = [IO.File]::Create($stderrPath)
            $stdoutTask = $launcherProcess.StandardOutput.BaseStream.CopyToAsync($stdoutStream)
            $stderrTask = $launcherProcess.StandardError.BaseStream.CopyToAsync($stderrStream)
            $arm.launch = [ordered]@{ appIdTag = $tag; launcherPath = $psi.FileName; launcherProcessId = $launcherPid; appProcessId = $null; childCommandLine = $null }

            $launchDeadline = [DateTime]::UtcNow.AddSeconds($LaunchTimeoutSeconds)
            while ($null -eq $appProcess -and [DateTime]::UtcNow -lt $launchDeadline) {
                if ($launcherProcess.HasExited) { break }
                Start-Sleep -Milliseconds 250
                try { $children = @(Get-CimInstance -ClassName Win32_Process -Filter ('ParentProcessId=' + $launcherPid) -ErrorAction Stop) } catch { $children = @() }
                foreach ($child in $children) {
                    $childName = [string](Get-ObjProp $child 'Name')
                    $childCommand = [string](Get-ObjProp $child 'CommandLine')
                    if ($childName -ieq 'inkscape.exe' -and $childCommand -and $childCommand.Contains($tag)) {
                        $candidate = Get-Process -Id ([int](Get-ObjProp $child 'ProcessId')) -ErrorAction SilentlyContinue
                        if ($null -ne $candidate) {
                            $appProcess = $candidate; $appPid = [int]$candidate.Id
                            $arm.launch.childCommandLine = $childCommand
                            $arm.launch.childParentProcessId = [int](Get-ObjProp $child 'ParentProcessId')
                            break
                        }
                    }
                }
            }
            if ($null -eq $appProcess) { throw 'The owned inkscape.exe child did not appear (no fallback adoption).' }
            $script:currentAppPid = $appPid
            $arm.launch.appProcessId = $appPid
            if ([string]::IsNullOrEmpty($arm.launch.childCommandLine) -or -not $arm.launch.childCommandLine.Contains($tag)) {
                $armErrs.Add('owned child command line does not contain the unique --app-id-tag')
            }

            # Bounded wait for a STABLE visible owned document window whose title
            # is the expected document title, proven by class/text/visible/root/PID.
            # The first non-zero MainWindowHandle is never trusted: GTK may expose a
            # transient startup window. FindMainWindow (reviewed VAPan helper) is
            # consulted as well, and the candidate must keep the same bounds/client
            # across consecutive samples before it is accepted.
            $expectedDocumentTitle = [IO.Path]::GetFileName($sharedFixture) + ' - VA Studio'
            $windowDeadline = [DateTime]::UtcNow.AddSeconds($LaunchTimeoutSeconds)
            $chosen = [IntPtr]::Zero
            $lastKey = ''; $stableCount = 0
            $seen = New-Object System.Collections.Generic.List[object]
            while ([DateTime]::UtcNow -lt $windowDeadline) {
                if (-not (Test-ProcessAlive $appProcess)) { break }
                try { $appProcess.Refresh() } catch { }
                $candidates = New-Object System.Collections.Generic.List[IntPtr]
                try { $fw = [VAPan.Win32]::FindMainWindow([uint32]$appPid); if ($fw -ne [IntPtr]::Zero) { $candidates.Add($fw) } } catch { }
                if ($appProcess.MainWindowHandle -ne 0) { $candidates.Add($appProcess.MainWindowHandle) }
                foreach ($c in $candidates) {
                    $wi = Get-WindowIdentity $c
                    [void]$seen.Add($wi)
                    if ($wi.visible -eq $true -and [string]$wi.windowText -eq $expectedDocumentTitle -and [int]$wi.pid -eq $appPid) {
                        $key = (($wi.bounds -join ',') + '|' + ($wi.client -join ','))
                        if ($key -eq $lastKey) { $stableCount++ } else { $stableCount = 0; $lastKey = $key }
                        $chosen = $c
                        if ($stableCount -ge 2) { break }
                    }
                }
                if ($chosen -ne [IntPtr]::Zero -and $stableCount -ge 2) { break }
                Start-Sleep -Milliseconds 300
            }
            if (-not (Test-ProcessAlive $appProcess)) { throw 'The application exited before its document window was stable.' }
            if ($chosen -eq [IntPtr]::Zero) { throw ('No stable visible owned document window with title "' + $expectedDocumentTitle + '" appeared.') }
            $hwnd = $chosen
            $arm.launch.mainWindowHandle = $hwnd.ToInt64()
            $arm.launch.windowTitle = [string][VAVisualNative]::WindowText($hwnd)
            $arm.launchMs = [int]([DateTime]::UtcNow - $launchStart).TotalMilliseconds
            $findHwndNow = [IntPtr]::Zero
            try { $findHwndNow = [VAPan.Win32]::FindMainWindow([uint32]$appPid) } catch { }
            $seenTail = @()
            if ($seen.Count -gt 0) {
                $take = [Math]::Min(8, $seen.Count)
                for ($si = $seen.Count - $take; $si -lt $seen.Count; $si++) { $seenTail += $seen[$si] }
            }
            $arm.windowIdentity = [ordered]@{
                expectedTitle = $expectedDocumentTitle
                processMainWindowHandle = $appProcess.MainWindowHandle.ToInt64()
                findMainWindowHandle = $findHwndNow.ToInt64()
                chosen = Get-WindowIdentity $hwnd
                seenCount = $seen.Count
                seenTail = @($seenTail)
            }
            Write-VLog ('WINDOW ' + $Mode + ' hwnd=' + $hwnd.ToInt64() + ' class=' + [string]$arm.windowIdentity.chosen.className +
                ' title=' + [string]$arm.launch.windowTitle +
                ' visible=' + [string]$arm.windowIdentity.chosen.visible +
                ' root=' + [string]$arm.windowIdentity.chosen.root +
                ' pid=' + [string]$arm.windowIdentity.chosen.pid +
                ' fg=' + [string]$arm.windowIdentity.chosen.foreground)
            Start-Sleep -Seconds 2

            # Loaded-module identity (runtime authority, not on-disk pins).
            $moduleInfo = [ordered]@{ count = $null; mainModule = $null; gtk = $null; cairo = $null; glib = $null; errors = @() }
            $moduleErrors = New-Object System.Collections.Generic.List[string]
            try {
                $modules = @($appProcess.Modules)
                $moduleInfo.count = $modules.Count
                foreach ($module in $modules) {
                    $name = [string]$module.ModuleName
                    if ($name -ieq 'inkscape.exe' -or $name -ieq 'libgtk-4-1.dll' -or $name -ieq 'libcairo-2.dll' -or $name -ieq 'libglib-2.0-0.dll') {
                        $entry = [ordered]@{ name = $name; fileName = [string]$module.FileName; sha256 = $null; insideAppRoot = $false; matchesExpected = $null }
                        try { $entry.sha256 = Get-FileSha256 $entry.fileName } catch { $moduleErrors.Add('could not hash ' + $name) }
                        $entry.insideAppRoot = Test-PathInside $entry.fileName $appRootFull
                        if ($name -ieq 'inkscape.exe') { $entry.matchesExpected = ($entry.sha256 -eq $expectedPins.exe); $moduleInfo.mainModule = $entry }
                        elseif ($name -ieq 'libgtk-4-1.dll') { $entry.matchesExpected = ($entry.sha256 -eq $expectedPins.gtk); $moduleInfo.gtk = $entry }
                        elseif ($name -ieq 'libcairo-2.dll') { $entry.matchesExpected = ($entry.sha256 -eq $expectedPins.cairo); $moduleInfo.cairo = $entry }
                        else { if ($expectedPins.glib -ne '') { $entry.matchesExpected = ($entry.sha256 -eq $expectedPins.glib) }; $moduleInfo.glib = $entry }
                    }
                }
            } catch { $moduleErrors.Add('loaded module enumeration failed: ' + $_.Exception.Message) }
            foreach ($key in @('mainModule', 'gtk', 'cairo')) {
                $entry = $moduleInfo[$key]
                if ($null -eq $entry) { $moduleErrors.Add('loaded ' + $key + ' not found'); continue }
                if (-not $entry.insideAppRoot) { $moduleErrors.Add(($entry.name + ' loaded from outside AppRoot: ' + $entry.fileName)) }
                if (-not $entry.matchesExpected) { $moduleErrors.Add(($entry.name + ' loaded hash does not match the pinned hash')) }
            }
            if ($null -ne $moduleInfo.glib -and -not $moduleInfo.glib.insideAppRoot) { $moduleErrors.Add('libglib-2.0-0.dll loaded from outside AppRoot') }
            $moduleInfo.errors = @($moduleErrors)
            $arm.modules = $moduleInfo
            foreach ($e in $moduleErrors) { $armErrs.Add($e) }
            $gtkShaLog = $null
            if ($null -ne $moduleInfo.gtk) { $gtkShaLog = $moduleInfo.gtk.sha256 }
            Write-VLog ('MODULES ' + $Mode + ' count=' + [string]$moduleInfo.count + ' gtk=' + [string]$gtkShaLog)

            if ($moduleErrors.Count -gt 0) { throw ('runtime module identity failed for arm ' + $Mode) }

            # ---- State machine -------------------------------------------------
            # Actual-fixture identity consistency (title/PID/foreground) recorded
            # per captured state; never a missing-screenshot pass.
            function Test-ArmStateConsistency([string]$StateLabel) {
                $st = $arm.states[$StateLabel]
                if ($null -eq $st) { return }
                $g = $st.geometry
                if ($g.ownerIsApp -ne $true) { $armErrs.Add($Mode + ' ' + $StateLabel + ' window PID is not the owned app process') }
                if ([string]$g.windowText -ne $expectedDocumentTitle) {
                    $armErrs.Add($Mode + ' ' + $StateLabel + ' window title "' + [string]$g.windowText + '" != "' + $expectedDocumentTitle + '"')
                }
                if ($g.foregroundIsOwned -ne $true) { $armErrs.Add($Mode + ' ' + $StateLabel + ' owned window is not foreground at capture') }
                if ($null -ne $g.containment -and $g.containment.windowBoundsInsideMonitor -ne $true) {
                    $armErrs.Add($Mode + ' ' + $StateLabel + ' window bounds are not fully inside the monitor (containment proof failed)')
                }
            }

            $expectedRects = @(
                $ExpectedMonitorLeft, $ExpectedMonitorTop, $ExpectedMonitorRight, $ExpectedMonitorBottom,
                $ExpectedMonitorLeft, $ExpectedMonitorTop, $ExpectedWorkRight, $ExpectedWorkBottom)
            $secondaryRects = @(
                $ExpectedSecondaryLeft, $ExpectedSecondaryTop, $ExpectedSecondaryRight, $ExpectedSecondaryBottom,
                $ExpectedSecondaryLeft, $ExpectedSecondaryTop, $ExpectedSecondaryWorkRight, $ExpectedSecondaryWorkBottom)

            # Opt-in: minimize specifically identified stale WindowsTerminal /
            # DisplaySettings windows so step-2 captures are unoccluded. They are
            # recorded and restored; never closed or killed.
            $script:armStaleOccluders = @()
            if ($MinimizeStaleOccluders) {
                $staleRec = Invoke-MinimizeStaleOccluders
                $script:armStaleOccluders = @($staleRec)
                $arm.staleOccluders = @($staleRec)
                Write-VLog ('STALE_OCCLUDERS ' + $Mode + ' count=' + [string]@($staleRec).Count)
            }

            if ($TelemetryOnly) {
                # -----------------------------------------------------------------
                # SEPARATE short telemetry calibration. show_diagnostics=1 (set in
                # the isolated profile above) is what actually opens
                # VACARDS_RENDER_STATS_LOG; this state is never part of the paired
                # static pixel comparison and no MATCH is claimed from it.
                # -----------------------------------------------------------------
                $scMaximizeT = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF030, [IntPtr]::Zero)  # SC_MAXIMIZE
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::Maximized($H)) { return $true }
                    }
                    return $false
                }
                if ([VAPan.Win32]::Maximized($hwnd)) { [void][VAPan.Win32]::Restore($hwnd); Start-Sleep -Milliseconds $StateWaitMs }
                if ([VAPan.Win32]::Minimized($hwnd)) { [void][VAPan.Win32]::ShowWindow($hwnd, 9); Start-Sleep -Milliseconds $StateWaitMs }
                $posT = Get-CenteredPosition $expectedRects 1280 800
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $posT[0], $posT[1], 1280, 800)
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $SettleMs
                $mxT = & $scMaximizeT $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd 't0-native-max-4k' $arm $ArmDir $true
                $arm.states['t0-native-max-4k'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['t0-native-max-4k'].maximizeConfirmed = $mxT
                if ($mxT -ne $true) { $armErrs.Add($Mode + ' t0-native-max-4k SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                Test-ArmStateConsistency 't0-native-max-4k'
                $gT = $arm.states['t0-native-max-4k'].geometry
                if ($null -eq $gT.clientRect -or [int]$gT.clientRect.width -ne 3840 -or [int]$gT.clientRect.height -ne 2088) {
                    $armErrs.Add($Mode + ' telemetry client is not 3840x2088')
                }
                if ($gT.dpiForWindow -ne $ExpectedWindowDpi) { $armErrs.Add($Mode + ' telemetry window DPI is not ' + [string]$ExpectedWindowDpi) }
                if ([string]$gT.monitorDevice -ne $ExpectedMonitorDevice) { $armErrs.Add($Mode + ' telemetry monitor is not ' + $ExpectedMonitorDevice) }

                $baselineSvg = Join-Path $ArmDir 'telemetry-baseline.svg'
                if (Test-Path -LiteralPath $baselineSvg) { Remove-Item -LiteralPath $baselineSvg -Force }
                $saveRes = Invoke-NativeCtrlS -WindowHandle $hwnd -DocumentPath $sharedFixture -TimeoutSeconds $NativeSaveTimeoutSeconds
                $arm.nativeSave = $saveRes
                if ($saveRes.wrote) { Copy-Item -LiteralPath $sharedFixture -Destination $baselineSvg -Force }
                $arm.telemetry = [ordered]@{
                    schema = 'vacards-app-visual-telemetry/1'
                    csvPath = $csvPath; csvExistsAtCapture = (Test-Path -LiteralPath $csvPath -PathType Leaf)
                    clientPng = $arm.states['t0-native-max-4k'].capture.path
                    baselineSvg = $baselineSvg
                    baselineWritten = [bool]$saveRes.wrote
                    baselineSha256 = if (Test-Path -LiteralPath $baselineSvg) { Get-FileSha256 $baselineSvg } else { $null }
                    namedviewZoom = Get-NamedviewZoom $baselineSvg
                    monitorDevice = [string]$gT.monitorDevice
                    monitorRect = if ($null -ne $gT.monitorRect) { @([int]$gT.monitorRect.left, [int]$gT.monitorRect.top, [int]$gT.monitorRect.right, [int]$gT.monitorRect.bottom) } else { $null }
                    workRect = if ($null -ne $gT.workRect) { @([int]$gT.workRect.left, [int]$gT.workRect.top, [int]$gT.workRect.right, [int]$gT.workRect.bottom) } else { $null }
                    dpiForWindow = [int]$gT.dpiForWindow
                    monitorDpi = if ($null -ne $gT.monitorDpiX) { [int]$gT.monitorDpiX } else { $null }
                    clientWidth = if ($null -ne $gT.clientRect) { [int]$gT.clientRect.width } else { $null }
                    clientHeight = if ($null -ne $gT.clientRect) { [int]$gT.clientRect.height } else { $null }
                    physicalClientX = if ($null -ne $gT.physicalClientOrigin) { [int]$gT.physicalClientOrigin.x } else { $null }
                    physicalClientY = if ($null -ne $gT.physicalClientOrigin) { [int]$gT.physicalClientOrigin.y } else { $null }
                }
                $requiredPngStates = @('t0-native-max-4k')
            } elseif ($Diagnostic) {
                # DIAGNOSTIC dual-geometry sequence (both arms, same placement).
                # Raw ShowWindow(3) is kept as ONE explicit diagnostic state only,
                # because external/raw maximize bypasses GTK's WM_SYSCOMMAND
                # handling and can manufacture oversized geometry. The decisive
                # path is the real native/user SC_MAXIMIZE message.
                $maximizeViaSc = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF030, [IntPtr]::Zero)  # WM_SYSCOMMAND, SC_MAXIMIZE
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::Maximized($H)) { return $true }
                    }
                    return $false
                }
                $restoreViaSc = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF120, [IntPtr]::Zero)  # WM_SYSCOMMAND, SC_RESTORE
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::NormalState($H)) { return 'SC_RESTORE' }
                    }
                    $f = [VAPan.Win32]::ForceNormal($H, $StateWaitMs)
                    return ('ForceNormalFallback:' + [string]$f)
                }
                # Focus before capture when SetForegroundWindow did not take:
                # verified HTCAPTION point only, after owner/title/visible check;
                # FindCaptionPoint scans centre-outward and accepts only HTCAPTION,
                # so caption buttons are never clicked.
                $ensureForeground = {
                    param([IntPtr]$H)
                    if ([VAPan.Win32]::ForegroundIs($H)) { return 'already-foreground' }
                    $wi = Get-WindowIdentity $H
                    if ([int]$wi.pid -ne $appPid -or $wi.visible -ne $true) { return 'skipped-identity' }
                    [void][VAPan.Win32]::BringToTopNoActivate($H)
                    $find = [VAPan.Win32]::FindCaptionPoint($H, [uint32]$appPid, 250, 100, 16, 4)
                    if ($find[4] -ne 1) { return ('no-htcaption:' + [string]$find[3]) }
                    $abs = [VAPan.Win32]::ScreenToAbsolute($find[0], $find[1])
                    [void][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOVE_FLAGS, $abs[0], $abs[1])
                    Start-Sleep -Milliseconds 80
                    [void][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTDOWN, 0, 0)
                    Start-Sleep -Milliseconds 50
                    [void][VAPan.Win32]::SendMouse([uint32][VAPan.Win32]::MOUSEEVENTF_LEFTUP, 0, 0)
                    Start-Sleep -Milliseconds 300
                    return ('htcaption-click:' + [string][VAPan.Win32]::ForegroundIs($H))
                }

                # 00 initial (untouched launch state)
                Add-State $hwnd '00-initial' $arm $ArmDir $true
                Test-ArmStateConsistency '00-initial'

                # restore / move into the 4K work area
                $rm = & $restoreViaSc $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                $p4 = Get-CenteredPosition $expectedRects $DiagnosticRestoredWidth $DiagnosticRestoredHeight
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $p4[0], $p4[1], $DiagnosticRestoredWidth, $DiagnosticRestoredHeight)
                [void][VAPan.Win32]::Foreground($hwnd)
                $fm = & $ensureForeground $hwnd
                Start-Sleep -Milliseconds $SettleMs
                Add-State $hwnd '01-restored-4k' $arm $ArmDir $true
                $arm.states['01-restored-4k'].restoreMethod = $rm
                $arm.states['01-restored-4k'].foregroundMethod = $fm
                Test-ArmStateConsistency '01-restored-4k'

                # 02 raw ShowWindow(3) 4K -- EXPLICIT DIAGNOSTIC STATE ONLY
                [void][VAPan.Win32]::ShowWindow($hwnd, 3)
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '02-raw-showwindow-4k' $arm $ArmDir $true
                $arm.states['02-raw-showwindow-4k'].maximizeMethod = 'rawShowWindow(3)'
                Test-ArmStateConsistency '02-raw-showwindow-4k'

                # 03 native maximize 4K (SC_MAXIMIZE)
                [void](& $restoreViaSc $hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                $mx4 = & $maximizeViaSc $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '03-native-max-4k' $arm $ArmDir $true
                $arm.states['03-native-max-4k'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['03-native-max-4k'].maximizeConfirmed = $mx4
                Test-ArmStateConsistency '03-native-max-4k'
                if ($mx4 -ne $true) { $armErrs.Add($Mode + ' 03-native-max-4k SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }

                # restore / move into the 1080 work area
                [void](& $restoreViaSc $hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                $p1 = Get-CenteredPosition $secondaryRects $DiagnosticRestoredWidth $DiagnosticRestoredHeight
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $p1[0], $p1[1], $DiagnosticRestoredWidth, $DiagnosticRestoredHeight)
                [void][VAPan.Win32]::Foreground($hwnd)
                $fm = & $ensureForeground $hwnd
                Start-Sleep -Milliseconds $SettleMs
                Add-State $hwnd '04-restored-1080' $arm $ArmDir $true
                $arm.states['04-restored-1080'].foregroundMethod = $fm
                Test-ArmStateConsistency '04-restored-1080'

                # 05 native maximize 1080
                $mx1 = & $maximizeViaSc $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '05-native-max-1080' $arm $ArmDir $true
                $arm.states['05-native-max-1080'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['05-native-max-1080'].maximizeConfirmed = $mx1
                Test-ArmStateConsistency '05-native-max-1080'
                if ($mx1 -ne $true) { $armErrs.Add($Mode + ' 05-native-max-1080 SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }

                # restore / move back into the 4K work area
                [void](& $restoreViaSc $hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $p4[0], $p4[1], $DiagnosticRestoredWidth, $DiagnosticRestoredHeight)
                [void][VAPan.Win32]::Foreground($hwnd)
                $fm = & $ensureForeground $hwnd
                Start-Sleep -Milliseconds $SettleMs
                Add-State $hwnd '06-restored-4k' $arm $ArmDir $true
                $arm.states['06-restored-4k'].foregroundMethod = $fm
                Test-ArmStateConsistency '06-restored-4k'

                # 07 native maximize 4K (return)
                $mx4b = & $maximizeViaSc $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '07-native-max-4k' $arm $ArmDir $true
                $arm.states['07-native-max-4k'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['07-native-max-4k'].maximizeConfirmed = $mx4b
                Test-ArmStateConsistency '07-native-max-4k'
                if ($mx4b -ne $true) { $armErrs.Add($Mode + ' 07-native-max-4k SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }

                $requiredPngStates = @('00-initial', '01-restored-4k', '02-raw-showwindow-4k', '03-native-max-4k',
                    '04-restored-1080', '05-native-max-1080', '06-restored-4k', '07-native-max-4k')
            } elseif ($StaticArtworkOnly) {
                # -----------------------------------------------------------------
                # BOUNDED static-artwork sequence: two corresponding full-4K states
                # per arm (plus, for the rectangle fixture, one 4K->1080->4K
                # transition). Reuses the reviewed native SC_MAXIMIZE/SC_RESTORE
                # path, CSD focus and capture helpers. Native actions only; no
                # forced redraw. Every capture is preceded by the move-only
                # canvas->caption pointer sequence.
                # -----------------------------------------------------------------
                $scMaximize = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF030, [IntPtr]::Zero)  # SC_MAXIMIZE
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::Maximized($H)) { return $true }
                    }
                    return $false
                }
                $scRestore = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF120, [IntPtr]::Zero)  # SC_RESTORE
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::NormalState($H)) { return 'SC_RESTORE' }
                    }
                    $f = [VAPan.Win32]::ForceNormal($H, $StateWaitMs)
                    return ('ForceNormalFallback:' + [string]$f)
                }
                $assertStaticMax4K = {
                    param([string]$Label)
                    $g = $arm.states[$Label].geometry
                    if ($g.isZoomed -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' IsZoomed is not true') }
                    if ($g.showCmd -ne 3) { $armErrs.Add($Mode + ' ' + $Label + ' showCmd is not SW_SHOWMAXIMIZED(3): ' + [string]$g.showCmd) }
                    if ([string]$g.monitorDevice -ne $ExpectedMonitorDevice) { $armErrs.Add($Mode + ' ' + $Label + ' monitor device ' + [string]$g.monitorDevice + ' != ' + $ExpectedMonitorDevice) }
                    if ($null -eq $g.monitorRect -or $g.monitorRect.left -ne $ExpectedMonitorLeft -or $g.monitorRect.top -ne $ExpectedMonitorTop -or $g.monitorRect.right -ne $ExpectedMonitorRight -or $g.monitorRect.bottom -ne $ExpectedMonitorBottom) {
                        $armErrs.Add($Mode + ' ' + $Label + ' monitor rect does not match the expected 4K bounds')
                    }
                    if ($null -eq $g.workRect -or $g.workRect.right -ne $ExpectedWorkRight -or $g.workRect.bottom -ne $ExpectedWorkBottom) {
                        $armErrs.Add($Mode + ' ' + $Label + ' work area does not match the expected 4K work area')
                    }
                    if ($null -eq $g.clientRect -or [int]$g.clientRect.width -ne 3840 -or [int]$g.clientRect.height -ne 2088) {
                        $armErrs.Add($Mode + ' ' + $Label + ' physical client is not 3840x2088 (got ' + [string]$g.clientRect.width + 'x' + [string]$g.clientRect.height + ')')
                    }
                    if ($null -ne $g.physicalClientOrigin -and ([int]$g.physicalClientOrigin.x -ne $ExpectedMonitorLeft -or [int]$g.physicalClientOrigin.y -ne $ExpectedMonitorTop)) {
                        $armErrs.Add($Mode + ' ' + $Label + ' physical client origin is not the 4K work-area origin')
                    }
                    if ($g.dpiForWindow -ne $ExpectedWindowDpi) { $armErrs.Add($Mode + ' ' + $Label + ' window DPI ' + [string]$g.dpiForWindow + ' != ' + [string]$ExpectedWindowDpi) }
                    # ADMISSION: a StaticArtworkOnly capture is only valid when the
                    # move-only canvas->caption sequence was actually confirmed at
                    # BOTH owned HTCLIENT points; a failed move invalidates the run
                    # instead of silently producing a MATCH.
                    $cp = $arm.states[$Label].cursorPark
                    if ($null -eq $cp) {
                        $armErrs.Add($Mode + ' ' + $Label + ' static pointer-sequence receipt missing')
                    } else {
                        if ($cp.moved -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' static pointer sequence not confirmed (moved != true): ' + [string]$cp.reason) }
                        if ($cp.canvas_owned -ne $true -or $cp.canvas_hit_ok -ne 1 -or $cp.canvas_move_confirmed -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' canvas point not owned/HTCLIENT/move-confirmed') }
                        if ($cp.caption_owned -ne $true -or $cp.caption_hit_ok -ne 1 -or $cp.caption_move_confirmed -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' caption point not owned/HTCLIENT/move-confirmed') }
                    }
                    if ($g.foregroundIsOwned -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' owned window is not foreground at capture') }
                    if ($null -ne $g.containment) {
                        if ($g.containment.windowBoundsInsideMonitor -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' window bounds not fully inside the monitor') }
                        if ($g.containment.clientInsideWorkArea -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' client not fully inside the monitor work area') }
                    }
                }
                if ([VAPan.Win32]::Maximized($hwnd)) { [void](& $scRestore $hwnd); Start-Sleep -Milliseconds $StateWaitMs }
                if ([VAPan.Win32]::Minimized($hwnd)) { [void][VAPan.Win32]::ShowWindow($hwnd, 9); Start-Sleep -Milliseconds $StateWaitMs }
                $pos0 = Get-CenteredPosition $expectedRects 1280 800
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos0[0], $pos0[1], 1280, 800)
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $SettleMs

                # s0: full-4K initial (native maximize)
                $mx0 = & $scMaximize $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd 's0-full4k-initial' $arm $ArmDir $true -StaticPointerSequence
                $arm.states['s0-full4k-initial'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['s0-full4k-initial'].maximizeConfirmed = $mx0
                if ($mx0 -ne $true) { $armErrs.Add($Mode + ' s0-full4k-initial SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                & $assertStaticMax4K 's0-full4k-initial'
                Test-ArmStateConsistency 's0-full4k-initial'

                # s1: small resize, then native maximize (full-4K again)
                [void](& $scRestore $hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                $posS = Get-CenteredPosition $expectedRects $RestoredSmallWidth $RestoredSmallHeight
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $posS[0], $posS[1], $RestoredSmallWidth, $RestoredSmallHeight)
                Start-Sleep -Milliseconds $StateWaitMs
                $mx1 = & $scMaximize $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd 's1-small-resize-max' $arm $ArmDir $true -StaticPointerSequence
                $arm.states['s1-small-resize-max'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['s1-small-resize-max'].maximizeConfirmed = $mx1
                if ($mx1 -ne $true) { $armErrs.Add($Mode + ' s1-small-resize-max SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                & $assertStaticMax4K 's1-small-resize-max'
                Test-ArmStateConsistency 's1-small-resize-max'

                $requiredPngStates = @('s0-full4k-initial', 's1-small-resize-max')

                # s2 (rectangle only): physical 4K -> 1080 -> 4K, THEN maximize and
                # capture with the same canvas->caption pointer sequence.
                if ($StaticIncludeMonitorTransition) {
                    [void](& $scRestore $hwnd)
                    Start-Sleep -Milliseconds $StateWaitMs
                    $pos1 = Get-CenteredPosition $secondaryRects $RestoredSmallWidth $RestoredSmallHeight
                    [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos1[0], $pos1[1], $RestoredSmallWidth, $RestoredSmallHeight)
                    [void][VAPan.Win32]::Foreground($hwnd)
                    Start-Sleep -Milliseconds $SettleMs
                    $m1080 = Measure-WindowState $hwnd 's2-pre-transition-1080'
                    if ([string]$m1080.monitorDevice -ne $ExpectedSecondaryDevice) { $armErrs.Add($Mode + ' s2 pre-transition window is not on ' + $ExpectedSecondaryDevice) }
                    [void](& $scRestore $hwnd)
                    Start-Sleep -Milliseconds $StateWaitMs
                    [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos0[0], $pos0[1], 1280, 800)
                    [void][VAPan.Win32]::Foreground($hwnd)
                    Start-Sleep -Milliseconds $SettleMs
                    $mx2 = & $scMaximize $hwnd
                    Start-Sleep -Milliseconds $StateWaitMs
                    Add-State $hwnd 's2-4k-1080-back' $arm $ArmDir $true -StaticPointerSequence
                    $arm.states['s2-4k-1080-back'].maximizeMethod = 'SC_MAXIMIZE'
                    $arm.states['s2-4k-1080-back'].maximizeConfirmed = $mx2
                    if ($mx2 -ne $true) { $armErrs.Add($Mode + ' s2-4k-1080-back SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                    & $assertStaticMax4K 's2-4k-1080-back'
                    Test-ArmStateConsistency 's2-4k-1080-back'
                    $requiredPngStates = @('s0-full4k-initial', 's1-small-resize-max', 's2-4k-1080-back')
                }
            } else {
                # Strict paired sequence. Fresh profiles may open on the laptop
                # primary (DISPLAY1, DPI 96). Move the owned window explicitly into
                # the expected 4K work area BEFORE the first capture, then drive the
                # rectangle states through the REAL native/user maximize/restore
                # path (WM_SYSCOMMAND SC_MAXIMIZE / SC_RESTORE). Raw ShowWindow(3)
                # is never used in this normal path; the archived diagnostic state
                # remains in -Diagnostic only.
                $scMaximize = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF030, [IntPtr]::Zero)
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::Maximized($H)) { return $true }
                    }
                    return $false
                }
                $scRestore = {
                    param([IntPtr]$H)
                    [void][VAPan.Win32]::PostMessage($H, 0x0112, [IntPtr]0xF120, [IntPtr]::Zero)
                    $dl = [DateTime]::UtcNow.AddMilliseconds([Math]::Max(3000, $StateWaitMs * 4))
                    while ([DateTime]::UtcNow -lt $dl) {
                        Start-Sleep -Milliseconds 200
                        if ([VAPan.Win32]::NormalState($H)) { return 'SC_RESTORE' }
                    }
                    $f = [VAPan.Win32]::ForceNormal($H, $StateWaitMs)
                    return ('ForceNormalFallback:' + [string]$f)
                }
                $assertMax4K = {
                    param([string]$Label)
                    $g = $arm.states[$Label].geometry
                    if ($g.isZoomed -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' IsZoomed is not true') }
                    if ($g.showCmd -ne 3) { $armErrs.Add($Mode + ' ' + $Label + ' showCmd is not SW_SHOWMAXIMIZED(3): ' + [string]$g.showCmd) }
                    if ([string]$g.monitorDevice -ne $ExpectedMonitorDevice) { $armErrs.Add($Mode + ' ' + $Label + ' monitor device ' + [string]$g.monitorDevice + ' != ' + $ExpectedMonitorDevice) }
                    if ($null -eq $g.monitorRect -or $g.monitorRect.left -ne $ExpectedMonitorLeft -or $g.monitorRect.top -ne $ExpectedMonitorTop -or $g.monitorRect.right -ne $ExpectedMonitorRight -or $g.monitorRect.bottom -ne $ExpectedMonitorBottom) {
                        $armErrs.Add($Mode + ' ' + $Label + ' monitor rect does not match the expected 4K bounds')
                    }
                    if ($null -eq $g.workRect -or $g.workRect.right -ne $ExpectedWorkRight -or $g.workRect.bottom -ne $ExpectedWorkBottom) {
                        $armErrs.Add($Mode + ' ' + $Label + ' work area does not match the expected 4K work area')
                    }
                    if ($null -eq $g.clientRect -or [int]$g.clientRect.width -ne 3840 -or [int]$g.clientRect.height -ne 2088) {
                        $armErrs.Add($Mode + ' ' + $Label + ' physical client is not 3840x2088 (got ' + [string]$g.clientRect.width + 'x' + [string]$g.clientRect.height + ')')
                    }
                    if ($null -ne $g.physicalClientOrigin -and ([int]$g.physicalClientOrigin.x -ne $ExpectedMonitorLeft -or [int]$g.physicalClientOrigin.y -ne $ExpectedMonitorTop)) {
                        $armErrs.Add($Mode + ' ' + $Label + ' physical client origin is not the 4K work-area origin')
                    }
                    if ($g.dpiForWindow -ne $ExpectedWindowDpi) { $armErrs.Add($Mode + ' ' + $Label + ' window DPI ' + [string]$g.dpiForWindow + ' != ' + [string]$ExpectedWindowDpi + ' (SYSTEM_AWARE app window DPI)') }
                    if ($g.foregroundIsOwned -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' owned window is not foreground at capture') }
                    if ($null -ne $g.containment) {
                        if ($g.containment.windowBoundsInsideMonitor -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' window bounds not fully inside the monitor') }
                        if ($g.containment.clientInsideWorkArea -ne $true) { $armErrs.Add($Mode + ' ' + $Label + ' client not fully inside the monitor work area') }
                    }
                }
                if ([VAPan.Win32]::Maximized($hwnd)) { [void](& $scRestore $hwnd); Start-Sleep -Milliseconds $StateWaitMs }
                if ([VAPan.Win32]::Minimized($hwnd)) { [void][VAPan.Win32]::ShowWindow($hwnd, 9); Start-Sleep -Milliseconds $StateWaitMs }
                $pos0 = Get-CenteredPosition $expectedRects 1280 800
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos0[0], $pos0[1], 1280, 800)
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $SettleMs
                $preMove = Measure-WindowState $hwnd 'pre-position'
                if ([string]$preMove.monitorDevice -ne $ExpectedMonitorDevice) {
                    $armErrs.Add($Mode + ' pre-positioned window is not on ' + $ExpectedMonitorDevice + ' (device=' + [string]$preMove.monitorDevice + ')')
                }
                Add-State $hwnd '00-initial' $arm $ArmDir $true

                $pos = Get-CenteredPosition $expectedRects $RestoredSmallWidth $RestoredSmallHeight
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos[0], $pos[1], $RestoredSmallWidth, $RestoredSmallHeight)
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '01-restored-small' $arm $ArmDir $true

                $pos = Get-CenteredPosition $expectedRects $RestoredLargeWidth $RestoredLargeHeight
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos[0], $pos[1], $RestoredLargeWidth, $RestoredLargeHeight)
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '02-restored-large' $arm $ArmDir $true

                $mx4 = & $scMaximize $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '03-native-max-4k' $arm $ArmDir $true
                $arm.states['03-native-max-4k'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['03-native-max-4k'].maximizeConfirmed = $mx4
                if ($mx4 -ne $true) { $armErrs.Add($Mode + ' 03-native-max-4k SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                & $assertMax4K '03-native-max-4k'

                # PREREGISTERED rectangle calibration (default arm only): native
                # Ctrl+S baseline to copy, namedview zoom, exact #204080 bbox,
                # canvas scale from the diagnostic CSV, and the factor.
                if ($CalibrateRectangle -and $Mode -eq 'default') {
                    $calOut = if ([string]::IsNullOrWhiteSpace($CalibrationOutputPath)) { Join-Path $ArmDir 'calibration' } else { $CalibrationOutputPath }
                    if (-not (Test-Path -LiteralPath $calOut)) { $null = New-Item -ItemType Directory -Path $calOut -Force }
                    $baselineSvg = Join-Path $calOut 'baseline.svg'
                    if (Test-Path -LiteralPath $baselineSvg) { Remove-Item -LiteralPath $baselineSvg -Force }
                    $saveRes = Invoke-NativeCtrlS -WindowHandle $hwnd -DocumentPath $sharedFixture -TimeoutSeconds $NativeSaveTimeoutSeconds
                    $arm.nativeSave = $saveRes
                    if ($saveRes.wrote) { Copy-Item -LiteralPath $sharedFixture -Destination $baselineSvg -Force }
                    else { $armErrs.Add($Mode + ' native Ctrl+S baseline save did not write (' + [string]$saveRes.reason + ')') }
                    $cal = Get-RectangleCalibration -ClientPng $arm.states['03-native-max-4k'].capture.path -SavedSvg $baselineSvg -CsvPath $csvPath -Geometry $arm.states['03-native-max-4k'].geometry
                    $roi = Get-ObserverRoiConfig -Calibration $cal
                    $calPath = Join-Path $calOut 'maximized-rectangle-calibration.json'
                    $roiPath = Join-Path $calOut 'observer-roi.json'
                    try { [IO.File]::WriteAllText($calPath, ($cal | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false)) } catch { $armErrs.Add($Mode + ' calibration JSON write failed: ' + $_.Exception.Message) }
                    try { [IO.File]::WriteAllText($roiPath, ($roi | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false)) } catch { $armErrs.Add($Mode + ' ROI JSON write failed: ' + $_.Exception.Message) }
                    $arm.calibration = [ordered]@{
                        path = $calPath; roiPath = $roiPath; baselineSvg = $baselineSvg
                        calibrationSha256 = if (Test-Path -LiteralPath $calPath) { Get-FileSha256 $calPath } else { $null }
                        roiSha256 = if (Test-Path -LiteralPath $roiPath) { Get-FileSha256 $roiPath } else { $null }
                        baselineSha256 = if (Test-Path -LiteralPath $baselineSvg) { Get-FileSha256 $baselineSvg } else { $null }
                        ok = $cal.ok; errors = @($cal.calibration_errors)
                    }
                    foreach ($ce in @($cal.calibration_errors)) { $armErrs.Add($Mode + ' calibration: ' + $ce) }
                }

                [void][VAPan.Win32]::ShowWindow($hwnd, $SW_MINIMIZE)
                Start-Sleep -Milliseconds $StateWaitMs
                $mmin = Measure-WindowState $hwnd '04-minimized'
                if ($mmin.isIconic -ne $true) { throw 'window did not report IsIconic after SW_MINIMIZE' }
                $arm.states['04-minimized'] = [ordered]@{ label = '04-minimized'; geometry = $mmin; capture = $null; focus = $null }
                Write-VLog ('STATE ' + $Mode + ' 04-minimized verifiedIconic=' + [bool]$mmin.isIconic)

                $rm = & $scRestore $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '05-minimize-restored' $arm $ArmDir $true
                $arm.states['05-minimize-restored'].restoreMethod = $rm

                $mx4b = & $scMaximize $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '06-native-max-4k-again' $arm $ArmDir $true
                $arm.states['06-native-max-4k-again'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['06-native-max-4k-again'].maximizeConfirmed = $mx4b
                if ($mx4b -ne $true) { $armErrs.Add($Mode + ' 06-native-max-4k-again SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                & $assertMax4K '06-native-max-4k-again'

                $client = [VAPan.Win32]::ClientSize($hwnd)
                $origin = [VAPan.Win32]::ClientScreenOrigin($hwnd)
                if ($null -eq $client -or $null -eq $origin -or $client[0] -le 0 -or $client[1] -le 0) { throw 'no client size/origin for occluder' }
                $ox = $origin[0] + [int]($client[0] * $OccluderInsetXPercent / 100)
                $oy = $origin[1] + [int]($client[1] * $OccluderInsetYPercent / 100)
                $ow = [int]($client[0] * $OccluderWidthPercent / 100)
                $oh = [int]($client[1] * $OccluderHeightPercent / 100)
                $script:occluderHwnd = [VAVisualNative]::CreateOccluder($ox, $oy, $ow, $oh)
                $occRect = [VAVisualNative]::GetRect($script:occluderHwnd)
                $arm.occluder = [ordered]@{
                    hwnd = $script:occluderHwnd.ToInt64(); requested = [ordered]@{ x = $ox; y = $oy; width = $ow; height = $oh }
                    rect = if ($null -ne $occRect) { @($occRect[0], $occRect[1], $occRect[2], $occRect[3]) } else { $null }
                    visible = [bool][VAVisualNative]::IsWindowVisible($script:occluderHwnd)
                    clientSize = @($client[0], $client[1]); clientOrigin = @($origin[0], $origin[1])
                }
                if ($script:occluderHwnd -eq [IntPtr]::Zero) { throw 'occluder window creation failed' }
                [void][VAVisualNative]::UpdateWindow($script:occluderHwnd)
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '07-occluded' $arm $ArmDir $true

                Close-Occluder
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $SettleMs
                # 08-revealed: NO forced redraw before capture.
                Add-State $hwnd '08-revealed' $arm $ArmDir $true

                # 4K -> 1080 -> 4K
                [void](& $scRestore $hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                $pos1 = Get-CenteredPosition $secondaryRects 1280 800
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos1[0], $pos1[1], 1280, 800)
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $SettleMs
                Add-State $hwnd '09-restored-1080' $arm $ArmDir $true

                $mx1 = & $scMaximize $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '10-native-max-1080' $arm $ArmDir $true
                $arm.states['10-native-max-1080'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['10-native-max-1080'].maximizeConfirmed = $mx1
                if ($mx1 -ne $true) { $armErrs.Add($Mode + ' 10-native-max-1080 SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }

                [void](& $scRestore $hwnd)
                Start-Sleep -Milliseconds $StateWaitMs
                [void][VAPan.Win32]::MoveWindowTo($hwnd, $pos0[0], $pos0[1], 1280, 800)
                [void][VAPan.Win32]::Foreground($hwnd)
                Start-Sleep -Milliseconds $SettleMs
                Add-State $hwnd '11-restored-4k' $arm $ArmDir $true

                $mx4c = & $scMaximize $hwnd
                Start-Sleep -Milliseconds $StateWaitMs
                Add-State $hwnd '12-native-max-4k-return' $arm $ArmDir $true
                $arm.states['12-native-max-4k-return'].maximizeMethod = 'SC_MAXIMIZE'
                $arm.states['12-native-max-4k-return'].maximizeConfirmed = $mx4c
                if ($mx4c -ne $true) { $armErrs.Add($Mode + ' 12-native-max-4k-return SC_MAXIMIZE did not confirm IsZoomed within the bounded wait') }
                & $assertMax4K '12-native-max-4k-return'

                # Every required captured state must carry a real screenshot receipt.
                # A missing PNG is a hard arm error, never a silent geometry-only pass.
                $requiredPngStates = @('00-initial', '01-restored-small', '02-restored-large', '03-native-max-4k',
                    '05-minimize-restored', '06-native-max-4k-again', '07-occluded', '08-revealed',
                    '09-restored-1080', '10-native-max-1080', '11-restored-4k', '12-native-max-4k-return')
                foreach ($st in $requiredPngStates) { Test-ArmStateConsistency $st }
            }  # end default strict sequence
            foreach ($rs in $requiredPngStates) {
                if (-not $arm.states.Contains($rs)) { $armErrs.Add('required state missing: ' + $rs); continue }
                if ($null -eq $arm.states[$rs].capture) { $armErrs.Add('required state has no screenshot receipt: ' + $rs) }
                if ($Diagnostic -and $null -eq $arm.states[$rs].workarea) { $armErrs.Add('required state has no workarea screenshot receipt: ' + $rs) }
            }

            # ---- Normal close (owned process only) -----------------------------
            $closeRequestedUtc = [DateTime]::UtcNow
            $closeMethod = 'none'
            try { $appProcess.Refresh() } catch { }
            if ($appProcess.MainWindowHandle -ne 0) {
                try { if ([VAPan.Win32]::PostMessage($appProcess.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)) { $closeMethod = 'WM_CLOSE' } } catch { }
            }
            if ($closeMethod -eq 'none') { try { if ($appProcess.CloseMainWindow()) { $closeMethod = 'CloseMainWindow' } } catch { } }
            if ($closeMethod -eq 'none') { $armErrs.Add('could not request a normal close of the owned window') }
            $closeDeadline = [DateTime]::UtcNow.AddSeconds($CloseTimeoutSeconds)
            while ((Test-ProcessAlive $appProcess) -and [DateTime]::UtcNow -lt $closeDeadline) { Start-Sleep -Milliseconds 250 }
            $normalExit = -not (Test-ProcessAlive $appProcess)
            if ($normalExit -and (Test-ProcessAlive $launcherProcess)) {
                $ld = [DateTime]::UtcNow.AddSeconds(10)
                while ((Test-ProcessAlive $launcherProcess) -and [DateTime]::UtcNow -lt $ld) { Start-Sleep -Milliseconds 100 }
            }
            $arm.exit = [ordered]@{
                closeMethod = $closeMethod
                closeWaitSeconds = [Math]::Round(([DateTime]::UtcNow - $closeRequestedUtc).TotalSeconds, 3)
                normalExit = $normalExit
                app = Get-ProcessExitInfo $appProcess
                launcher = Get-ProcessExitInfo $launcherProcess
                killedOwnProcesses = @()
            }
            if (-not $normalExit) { $armErrs.Add('the application did not exit within the close timeout') }
        } catch {
            $armErrs.Add('FATAL: ' + $_.Exception.Message)
            Write-VLog ('ARM_FATAL ' + $Mode + ' ' + $_.Exception.Message)
        } finally {
            Close-Occluder
            # Restore (never close/kill) any stale occluder windows we minimized.
            if (@($script:armStaleOccluders).Count -gt 0) {
                try { Restore-StaleOccluders $script:armStaleOccluders } catch { }
                $script:armStaleOccluders = @()
            }
            try { [System.Threading.Tasks.Task]::WaitAll(@($stdoutTask, $stderrTask), 8000) | Out-Null } catch { }
            foreach ($stream in @($stdoutStream, $stderrStream)) { if ($null -ne $stream) { try { $stream.Flush(); $stream.Dispose() } catch { } } }
            # Only kill our own captured PIDs if they are still alive.
            $late = @(Stop-OwnedProcesses -AppProcess $appProcess -AppPid $appPid -LauncherProcess $launcherProcess -LauncherPid $launcherPid)
            if ($late.Count -gt 0) {
                $armErrs.Add('owned processes still alive and were killed: ' + ($late -join ','))
                $arm.exit.killedOwnProcesses = @($late)
            }
        }
        $arm.errors = @($armErrs)
        $arm.status = if ($armErrs.Count -eq 0) { 'valid' } else { 'invalid' }
        $armResultPath = Join-Path $ArmDir 'arm-result.json'
        try { [IO.File]::WriteAllText($armResultPath, ($arm | ConvertTo-Json -Depth 16), [System.Text.UTF8Encoding]::new($false)) } catch { Write-VLog ('ARM_RESULT_WRITE_FAIL ' + $Mode + ' ' + $_.Exception.Message) }
        return $arm
    }

    # -----------------------------------------------------------------------
    # Run both arms (default then disabled) sequentially.
    # -----------------------------------------------------------------------
    $armDefaultDir = Join-Path $outputRoot 'arm-default'
    $null = New-Item -ItemType Directory -Path $armDefaultDir
    $armDefault = Invoke-Arm -Mode 'default' -ArmDir $armDefaultDir
    $state.arms['default'] = [ordered]@{ status = $armDefault.status; errors = @($armDefault.errors); armDir = $armDefaultDir }
    if ($armDefault.status -ne 'valid') {
        Add-ValidationError ('arm default invalid: ' + (@($armDefault.errors) -join ' | '))
    }
    # Diagnostic mode runs BOTH arms even when the first arm's geometry is
    # unexpected, so the paired physical captures exist for both buffer settings.
    # TELEMETRY runs the default arm only (no paired pixel claim).
    if (-not $TelemetryOnly) {
        if ($armDefault.status -eq 'valid' -or $Diagnostic) {
            $armDisabledDir = Join-Path $outputRoot 'arm-disabled'
            $null = New-Item -ItemType Directory -Path $armDisabledDir
            $armDisabled = Invoke-Arm -Mode 'disabled' -ArmDir $armDisabledDir
            $state.arms['disabled'] = [ordered]@{ status = $armDisabled.status; errors = @($armDisabled.errors); armDir = $armDisabledDir }
            if ($armDisabled.status -ne 'valid') { Add-ValidationError ('arm disabled invalid: ' + (@($armDisabled.errors) -join ' | ')) }
        }
    } else {
        # -------------------------------------------------------------------
        # FINAL telemetry calibration (separate from paired pixel comparison).
        # The CSV is read AFTER the app closed so buffered rows are present.
        # -------------------------------------------------------------------
        $tel = $armDefault.telemetry
        $telResult = [ordered]@{
            schema = 'vacards-app-visual-telemetry-result/1'
            armStatus = $armDefault.status
            csvPath = $null; csvExists = $false; csvRows = $null
            canvasScale = $null; canvasLogicalWidth = $null; canvasLogicalHeight = $null
            gtkRenderer = $null; canvasBackend = $null
            calibrationPath = $null; calibrationSha256 = $null
            roiPath = $null; roiSha256 = $null
            baselineSvg = $null; baselineSha256 = $null; namedviewZoom = $null
            matchesV2 = [ordered]@{ namedviewZoom = $null; factor = $null; blueBbox = $null; monitorDevice = $null; roiOk = $null }
            ok = $false; errors = @()
        }
        $telErrs = New-Object System.Collections.Generic.List[string]
        try {
            if ($null -eq $tel) { throw 'telemetry arm did not record a snapshot' }
            $telResult.csvPath = $tel.csvPath
            $telResult.csvExists = [bool](Test-Path -LiteralPath $tel.csvPath -PathType Leaf)
            $telResult.baselineSvg = $tel.baselineSvg
            $telResult.baselineSha256 = $tel.baselineSha256
            $telResult.namedviewZoom = $tel.namedviewZoom
            $stats = Get-CanvasStats $tel.csvPath
            $telResult.csvRows = $stats.rows
            $telResult.canvasScale = $stats.scale
            $telResult.canvasLogicalWidth = $stats.logicalWidth
            $telResult.canvasLogicalHeight = $stats.logicalHeight
            $telResult.gtkRenderer = $stats.gtkRenderer
            $telResult.canvasBackend = $stats.canvasBackend
            if (-not $telResult.csvExists) { [void]$telErrs.Add('rendering.csv was not produced even with show_diagnostics=1') }
            if ($null -eq $stats.scale) { [void]$telErrs.Add('canvas scale is still unavailable from rendering.csv') }
            $gSnap = [pscustomobject]@{
                monitorDevice = $tel.monitorDevice
                monitorRect = [pscustomobject]@{ left = $tel.monitorRect[0]; top = $tel.monitorRect[1]; right = $tel.monitorRect[2]; bottom = $tel.monitorRect[3] }
                workRect = [pscustomobject]@{ left = $tel.workRect[0]; top = $tel.workRect[1]; right = $tel.workRect[2]; bottom = $tel.workRect[3] }
                dpiForWindow = $tel.dpiForWindow
                monitorDpiX = $tel.monitorDpi
                clientRect = [pscustomobject]@{ width = $tel.clientWidth; height = $tel.clientHeight }
                physicalClientOrigin = [pscustomobject]@{ x = $tel.physicalClientX; y = $tel.physicalClientY }
            }
            $cal = Get-RectangleCalibration -ClientPng $tel.clientPng -SavedSvg $tel.baselineSvg -CsvPath $tel.csvPath -Geometry $gSnap
            # Measured canvas fields pinned explicitly; factor stays the observed
            # blue-width formula (513/(400*0.86)) and is never derived from a drag.
            $cal.canvas_scale_measured = ($null -ne $cal.canvas_scale)
            $cal.telemetry_csv_rows = $stats.rows
            $cal.derived_from = 'next4-artwork-focused telemetry run (show_diagnostics=1, isolated profile)'
            $cal.factor_never_from_drag_endpoint = $true
            $roi = Get-ObserverRoiConfig -Calibration $cal
            $calOut = if ([string]::IsNullOrWhiteSpace($CalibrationOutputPath)) { Join-Path $armDefaultDir 'calibration' } else { $CalibrationOutputPath }
            if (-not (Test-Path -LiteralPath $calOut)) { $null = New-Item -ItemType Directory -Path $calOut -Force }
            $calPath = Join-Path $calOut 'final-maximized-rectangle-calibration.json'
            $roiPath = Join-Path $calOut 'final-observer-roi.json'
            [IO.File]::WriteAllText($calPath, ($cal | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false))
            [IO.File]::WriteAllText($roiPath, ($roi | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
            $telResult.calibrationPath = $calPath
            $telResult.calibrationSha256 = Get-FileSha256 $calPath
            $telResult.roiPath = $roiPath
            $telResult.roiSha256 = Get-FileSha256 $roiPath
            $telResult.matchesV2.namedviewZoom = ($null -ne $cal.namedview_zoom -and [math]::Abs([double]$cal.namedview_zoom - 0.86) -lt 1e-6)
            $telResult.matchesV2.factor = ($null -ne $cal.physical_pixels_per_logical_pixel -and [math]::Abs([double]$cal.physical_pixels_per_logical_pixel - 1.491279) -lt 5e-6)
            $telResult.matchesV2.blueBbox = ($null -ne $cal.blue_exact_bbox_client -and (($cal.blue_exact_bbox_client -join ',') -eq '1120,1229,1632,1405'))
            $telResult.matchesV2.monitorDevice = ([string]$cal.monitor_device -eq $ExpectedMonitorDevice)
            $telResult.matchesV2.roiOk = [bool]$roi.ok
            foreach ($ce in @($cal.calibration_errors)) { [void]$telErrs.Add('calibration: ' + $ce) }
            if ($roi.ok -ne $true) { [void]$telErrs.Add('ROI not ok: ' + [string]$roi.reason) }
        } catch { [void]$telErrs.Add('telemetry processing exception: ' + $_.Exception.Message) }
        $telResult.errors = @($telErrs)
        $telResult.ok = ($telErrs.Count -eq 0)
        $state.telemetry = $telResult
        foreach ($te in @($telErrs)) { Add-ValidationError $te }
    }

    # -----------------------------------------------------------------------
    # Compare corresponding settled states.
    # -----------------------------------------------------------------------
    if ($state.arms.Contains('disabled')) {
        $compare = [ordered]@{}
        $primaryAllMatch = $true
        $compareLabels = if ($Diagnostic) {
            @('00-initial', '01-restored-4k', '02-raw-showwindow-4k', '03-native-max-4k',
              '04-restored-1080', '05-native-max-1080', '06-restored-4k', '07-native-max-4k')
        } elseif ($StaticArtworkOnly) {
            if ($StaticIncludeMonitorTransition) { @('s0-full4k-initial', 's1-small-resize-max', 's2-4k-1080-back') }
            else { @('s0-full4k-initial', 's1-small-resize-max') }
        } else { $StateOrder }
        $primaryLabels = if ($Diagnostic -or $StaticArtworkOnly) { $compareLabels } else { $PrimaryStates }
        foreach ($label in $compareLabels) {
            $a = $armDefault.states[$label]
            $b = $armDisabled.states[$label]
            if ($null -eq $a -or $null -eq $b) {
                $compare[$label] = [ordered]@{ status = 'MISSING'; primary = ($primaryLabels -contains $label) }
                if ($primaryLabels -contains $label) { $primaryAllMatch = $false }
                continue
            }
            $ga = Get-GeometryCanon $a.geometry
            $gb = Get-GeometryCanon $b.geometry
            $geoEqual = ((ConvertTo-Json -Compress $ga) -eq (ConvertTo-Json -Compress $gb))
            $ca = $a.capture; $cb = $b.capture
            # Pixel comparison only where the corresponding physical capture
            # geometry matches exactly (region x/y/w/h), in addition to window
            # geometry. Otherwise no pixel-equality claim is made.
            $capGeoEqual = $true
            if ($null -ne $ca -and $null -ne $cb) {
                $capGeoEqual = (([int]$ca.x -eq [int]$cb.x) -and ([int]$ca.y -eq [int]$cb.y) -and
                    ([int]$ca.width -eq [int]$cb.width) -and ([int]$ca.height -eq [int]$cb.height))
            }
            $entry = [ordered]@{
                status = $null; geometryEqual = $geoEqual; captureGeometryEqual = $capGeoEqual; primary = ($primaryLabels -contains $label)
                defaultSha256 = $null; disabledSha256 = $null; defaultBytes = $null; disabledBytes = $null
                defaultCapture = if ($null -ne $ca) { @($ca.x, $ca.y, $ca.width, $ca.height) } else { $null }
                disabledCapture = if ($null -ne $cb) { @($cb.x, $cb.y, $cb.width, $cb.height) } else { $null }
                defaultWorkarea = if ($null -ne (Get-ObjProp $a 'workarea')) { $wa = Get-ObjProp $a 'workarea'; @($wa.x, $wa.y, $wa.width, $wa.height) } else { $null }
                disabledWorkarea = if ($null -ne (Get-ObjProp $b 'workarea')) { $wb = Get-ObjProp $b 'workarea'; @($wb.x, $wb.y, $wb.width, $wb.height) } else { $null }
                diffPixels = $null; maxChannelDelta = $null; diffBox = $null; sizeMismatch = $false
                largeDiffPixels = $null; smallDiffPixels = $null
                outsideBandDiffPixels = $null; insideBandSmallDiffPixels = $null
                bandHeightPx = $script:toolbarBandHeightPx; bandTolerancePixels = $UiTolerancePixels; bandToleranceChannel = $UiToleranceChannel
                uiTolerancePixels = $UiTolerancePixels; uiToleranceChannel = $UiToleranceChannel
                geometryDefault = $ga; geometryDisabled = $gb
            }
            if ($null -ne $ca) { $entry.defaultSha256 = $ca.sha256; $entry.defaultBytes = $ca.sizeBytes }
            if ($null -ne $cb) { $entry.disabledSha256 = $cb.sha256; $entry.disabledBytes = $cb.sizeBytes }
            if (-not $geoEqual -or -not $capGeoEqual) {
                $entry.status = 'GEOMETRY_MISMATCH'
                if ($entry.primary) { $primaryAllMatch = $false }
            } elseif ($null -eq $ca -or $null -eq $cb) {
                $entry.status = 'GEOMETRY_ONLY_MATCH'
            } elseif ($ca.sha256 -eq $cb.sha256) {
                $entry.status = 'MATCH'; $entry.diffPixels = 0
            } else {
                try {
                    # PREREGISTERED band oracle: outside the top y<160 toolbar
                    # band every pixel must be EXACTLY equal; only inside the band
                    # may <=UiToleranceChannel/channel differ in at most
                    # UiTolerancePixels pixels. Any region is still measured (no
                    # outside-band pixel is masked or discarded).
                    $bd = Compare-BandAwareImage -PathA $ca.path -PathB $cb.path -BandHeightPx $script:toolbarBandHeightPx -UiToleranceChannel $UiToleranceChannel -UiTolerancePixels $UiTolerancePixels
                    $entry.maxChannelDelta = $bd['maxChannelDelta']
                    $entry.outsideBandDiffPixels = $bd['outsideBandDiffPixels']
                    $entry.insideBandSmallDiffPixels = $bd['insideBandSmallDiffPixels']
                    $entry.bandHeightPx = $bd['bandHeightPx']
                    if ($bd['status'] -eq 'SIZE_MISMATCH') {
                        $entry.status = 'SIZE_MISMATCH'; $entry.sizeMismatch = $true; $entry.diffPixels = -1
                    } elseif ($bd['status'] -eq 'DIFF_ERROR') {
                        $entry.status = 'DIFF_ERROR'; $entry.diffError = $bd['error']
                    } else {
                        $entry.status = $bd['status']
                        if ($null -ne $bd['outsideBandDiffPixels'] -and $null -ne $bd['insideBandDiffPixels']) {
                            $entry.diffPixels = [int]$bd['outsideBandDiffPixels'] + [int]$bd['insideBandDiffPixels']
                        } else { $entry.diffPixels = $bd['outsideBandDiffPixels'] }
                        # Hard-failure count retained for existing consumers:
                        # every outside-band difference plus every inside-band
                        # delta greater than the channel tolerance.
                        if ($null -ne $bd['outsideBandDiffPixels'] -and $null -ne $bd['insideBandLargeDiffPixels']) {
                            $entry.largeDiffPixels = [int]$bd['outsideBandDiffPixels'] + [int]$bd['insideBandLargeDiffPixels']
                        } else { $entry.largeDiffPixels = $null }
                        $entry.smallDiffPixels = $bd['insideBandSmallDiffPixels']
                        if ($null -ne $bd['diffBox']) { $entry.diffBox = $bd['diffBox'] }
                    }
                } catch {
                    $entry.status = 'DIFF_ERROR'; $entry.diffError = $_.Exception.Message
                }
                if ($entry.primary -and (@('MATCH', 'MATCH_UI_TOLERANCE') -notcontains $entry.status)) { $primaryAllMatch = $false }
            }
            $compare[$label] = $entry
            Write-VLog ('COMPARE ' + $label + ' status=' + $entry.status + ' geometryEqual=' + [bool]$geoEqual + ' diff=' + [string]$entry.diffPixels)
        }
        $state.compare = $compare
        $state.primaryAllMatch = $primaryAllMatch
    } else {
        $state.primaryAllMatch = $false
    }

    if ($TelemetryOnly) {
        # Telemetry packet: a measured calibration artifact, never a MATCH claim.
        if ($null -ne $state.telemetry -and $state.telemetry.ok) { $state.status = 'telemetry' } else { $state.status = 'invalid' }
        $state.verdict = 'TELEMETRY'
    } elseif ($Diagnostic) {
        # Diagnostic packet: never a valid/MATCH or full4K PASS claim. Actual
        # geometry and any mismatch remain recorded as errors in `state.errors`.
        $state.status = 'diagnostic'
        $state.verdict = 'DIAGNOSTIC'
    } elseif ($validationErrors.Count -eq 0 -and $state.primaryAllMatch) {
        $state.status = 'valid'
        $state.verdict = 'MATCH'
    } elseif (($state.arms.Contains('default')) -and ($state.arms.Contains('disabled')) -and (-not $state.primaryAllMatch)) {
        $state.status = 'invalid'
        $state.verdict = 'MISMATCH'
    } else {
        $state.status = 'invalid'
        $state.verdict = 'INVALID'
    }
} catch {
    $state.status = 'failed'
    $state.verdict = 'INVALID'
    $state.failure = [string]$_.Exception.Message
    $state.warnings += ('FATAL: ' + [string]$_.Exception.Message)
    Write-VLog ('VISUAL_FATAL ' + [string]$_.Exception.Message)
} finally {
    if ($state.status -eq 'initializing') { $state.status = 'failed'; $state.verdict = 'INVALID'; $state.failure = 'aborted before a status was set' }
    Close-Occluder
    if ($null -ne $savedEnv) { try { Restore-ManagedEnvironment -Names $ManagedEnvNames -Saved $savedEnv } catch { } }
    if ($threadPromoted -and $null -ne $threadPrevRaw) { try { [void][VAPan.Win32]::RestoreThreadContext([int64]$threadPrevRaw) } catch { } }
    $state.errors = @($validationErrors)
    $state.capturedUtc = [DateTime]::UtcNow.ToString('o')
    if ($outputCreated -and $summaryPath) {
        try { [IO.File]::WriteAllText($summaryPath, ($state | ConvertTo-Json -Depth 20), [System.Text.UTF8Encoding]::new($false)) } catch { Write-VLog ('SUMMARY_WRITE_FAIL ' + $_.Exception.Message) }
    }
    $marker = [ordered]@{
        schema = 'vacards.app-visual-complete/1'
        root = $state.root
        outputRoot = $state.outputRoot
        status = $state.status
        verdict = $state.verdict
        failure = $state.failure
        validationErrorCount = $validationErrors.Count
        capturedUtc = $state.capturedUtc
        resultPath = $summaryPath
    }
    if ($markerPath) { try { [IO.File]::WriteAllText($markerPath, ($marker | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false)) } catch { } }
    Write-VLog ('VISUAL_END status=' + $state.status + ' verdict=' + $state.verdict + ' errors=' + $validationErrors.Count)
}

if ($Diagnostic) {
    # A completed diagnostic run exits 0 even when mismatches were recorded; a
    # fatal failure (outer catch) exits 1. No PASS is implied either way.
    if ($state.status -eq 'failed') { exit 1 }
    exit 0
}
if ($TelemetryOnly) {
    # Telemetry is a measurement artifact, not a pass/fail comparison.
    if ($state.status -eq 'failed') { exit 1 }
    if ($null -ne $state.telemetry -and $state.telemetry.ok) { exit 0 }
    exit 1
}
if ($state.status -ne 'valid') { exit 1 }
exit 0
