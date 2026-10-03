# SPDX-License-Identifier: GPL-2.0-or-later
# Default user scope: execute in an ordinary (non-admin) interactive Windows
# session. Machine scope (-Scope machine): execute once from an already elevated
# interactive 64-bit session; this helper never elevates itself or changes
# UAC/token/security state.
param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$Payload,
    [Parameter(Mandatory=$true)][string]$Evidence,
    [Parameter(Mandatory=$true)][string]$RuntimeCheck,
    [string]$HarnessPython = 'C:\msys64\ucrt64\bin\python.exe',
    [ValidateSet('user','machine')][string]$Scope = 'user'
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (Test-Path -LiteralPath $Evidence) { throw 'Use a fresh evidence directory.' }
$null = New-Item -ItemType Directory -Path $Evidence
Start-Transcript -LiteralPath (Join-Path $Evidence 'install-test.log') | Out-Null
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $token = @{user=$identity.Name; session=(Get-Process -Id $PID).SessionId;
               elevated=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)}
    $token | ConvertTo-Json | Set-Content (Join-Path $Evidence 'token.json')
    if ($Scope -eq 'machine') {
        if (-not $token.elevated -or $token.session -le 0 -or
            -not [Environment]::Is64BitProcess -or -not [Environment]::Is64BitOperatingSystem) {
            throw 'Machine scope requires an elevated interactive 64-bit token.'
        }
    } elseif ($token.elevated -or $token.session -le 0) {
        throw 'Use an ordinary interactive user token.'
    }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $Payload 'share\vacards-test\WINDOWS-INTERNAL-TEST.json') | ConvertFrom-Json
    $version = $manifest.release_version
    if ($Scope -eq 'machine') {
        # Machine identity is derived from the delivered payload manifest and the
        # tracked NSI script, not from copied per-user literals. TEST_VERSION is
        # the manifest release_version; DISPLAY_VERSION is the manifest's
        # display_version with underscores normalized the way pack_defines does.
        $nsiPath = Join-Path $PSScriptRoot 'internal-test-installer.nsi'
        if (-not (Test-Path -LiteralPath $nsiPath)) { throw 'Cannot find the NSI script beside this helper.' }
        $nsiText = Get-Content -Raw -LiteralPath $nsiPath
        if (-not ($nsiText.Contains('!ifdef SCOPE_MACHINE') -and $nsiText.Contains('!define REG_ROOT HKLM'))) {
            throw 'NSI script has no machine registry branch.'
        }
        $productMatch = [regex]::Match($nsiText, '(?m)^\s*!define\s+PRODUCT\s+"([^"]+)"')
        $keyMatch = [regex]::Match($nsiText, '(?m)^\s*!define\s+KEY\s+"([^"]+)"')
        if (-not $productMatch.Success -or -not $keyMatch.Success) { throw 'Cannot derive machine identity from the NSI script.' }
        $product = $productMatch.Groups[1].Value
        $nsiKey = $keyMatch.Groups[1].Value -replace '\$\{TEST_VERSION\}', $version
        if ($nsiKey -notlike '*\Uninstall\VACards.Inkscape.InternalTest.*') { throw 'Unexpected NSI uninstall key identity.' }
        $key = 'HKLM:\' + $nsiKey
        $displayVersion = $manifest.version_fields.display_version
        if (-not $displayVersion) { $displayVersion = $version }
        $displayName = $displayVersion -replace '_',' '
        foreach ($template in @(
            'CreateDirectory "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}"',
            'CreateShortcut "$SMPROGRAMS\${PRODUCT} ${DISPLAY_VERSION}\${PRODUCT} ${DISPLAY_VERSION}.lnk"',
            'CreateShortcut "$DESKTOP\${PRODUCT} ${DISPLAY_VERSION}.lnk"')) {
            if (-not $nsiText.Contains($template)) { throw ('NSI script lacks an expected machine shortcut: '+$template) }
        }
        $commonDesktop = [Environment]::GetFolderPath('CommonDesktopDirectory')
        $commonPrograms = [Environment]::GetFolderPath('CommonPrograms')
        if (-not $commonDesktop -or -not $commonPrograms) { throw 'Cannot resolve the all-users desktop or Start Menu.' }
        $desktopLink = Join-Path $commonDesktop ($product+' '+$displayName+'.lnk')
        $startMenuFolder = Join-Path $commonPrograms ($product+' '+$displayName)
        $startMenuLink = Join-Path $startMenuFolder ($product+' '+$displayName+'.lnk')
        if (Test-Path $key) { throw 'This version is already installed; leave it untouched.' }
        foreach ($shortcut in @($desktopLink,$startMenuFolder,$startMenuLink)) {
            if (Test-Path -LiteralPath $shortcut) { throw ('Existing machine shortcut collision; leave it untouched: '+$shortcut) }
        }
    } else {
        $key = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\VACards.Inkscape.InternalTest.'+$version
        if (Test-Path $key) { throw 'This version is already installed; leave it untouched.' }
    }
    function Snapshot-Registrations {
        $values = @{}
        foreach ($path in @('HKCU:\Environment',
            'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
            'HKCU:\Software\Classes\.svg','HKCU:\Software\Classes\.svgz',
            'HKCU:\Software\Classes\Inkscape.SVG','HKCU:\Software\Classes\Inkscape.SVGZ',
            'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\inkscape.exe',
            'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\inkscape.exe')) {
            $values[$path] = if (Test-Path $path) {
                $reg = Get-Item $path
                @($reg.GetValueNames() | Sort-Object | ForEach-Object { $_+'='+$reg.GetValue($_) }) -join "`n"
            } else { '<absent>' }
        }
        $values.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Name+'='+$_.Value }
    }
    $before = @(Snapshot-Registrations)
    $before | Set-Content (Join-Path $Evidence 'registrations-before.txt')
    if ($Scope -eq 'machine') {
        # The positive destination is a NEW DIRECT child of the 64-bit Program
        # Files root, so root/nested/existing destinations can be exercised as
        # separate uniquely owned children below.
        $programFiles64 = [System.IO.Path]::GetFullPath($env:ProgramFiles).TrimEnd('\')
        if ($env:ProgramW6432 -and -not [String]::Equals($programFiles64,
                [System.IO.Path]::GetFullPath($env:ProgramW6432).TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Machine scope must run in the 64-bit Program Files view.'
        }
        $qaRoot = Join-Path $programFiles64 ('VA Studio QA '+[guid]::NewGuid().ToString('N'))
        if (-not [String]::Equals((Split-Path -Parent $qaRoot), $programFiles64, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Machine QA destination is not a direct child of Program Files (x64).'
        }
        if (Test-Path -LiteralPath $qaRoot) { throw 'Machine QA destination already exists.' }
        $missingParent = $qaRoot
        $install = $qaRoot
        $existing = Join-Path $programFiles64 ('VA Studio QA existing '+[guid]::NewGuid().ToString('N'))
    } else {
        $qaRoot = Join-Path $env:LOCALAPPDATA ('VACards Test QA\'+[guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $qaRoot
        $missingParent = Join-Path $qaRoot 'new missing parent\Programs\VACards Test'
        if (Test-Path $missingParent) { throw 'Expected missing destination parents.' }
        $install = Join-Path $missingParent ('Prueba '+[char]0xE1+' '+[char]0x6587)
        $existing = Join-Path $qaRoot 'already exists'
    }
    $null = New-Item -ItemType Directory -Path $existing
    'preserve' | Set-Content -LiteralPath (Join-Path $existing 'sentinel.txt')
    function Execute-Installer([string]$Destination) {
        $process = Start-Process -FilePath $Installer -ArgumentList ('/S /D='+$Destination) -PassThru
        $null = $process.Handle
        if (-not $process.WaitForExit(180000)) { throw 'Installer timed out; inspect its PID.' }
        return $process.ExitCode
    }
    function Assert-OwnedSentinelOnly([string]$Directory, [string]$SentinelName) {
        # A rejected installer may have left a partial payload, so refusal
        # evidence is never recursively deleted. Require exactly the owned
        # sentinel regular file and nothing else, hidden entries included.
        if (-not (Test-Path -LiteralPath $Directory -PathType Container)) {
            throw ('Owned refusal directory is missing: '+$Directory)
        }
        $children = @(Get-ChildItem -LiteralPath $Directory -Force)
        $owned = @($children | Where-Object {
            $_.Name -eq $SentinelName -and -not $_.PSIsContainer -and
            -not ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) })
        if ($children.Count -ne 1 -or $owned.Count -ne 1) {
            throw ('Refused installer left unexpected artifacts in '+$Directory+': '+
                (@($children | ForEach-Object { $_.Name }) -join ', '))
        }
    }
    if ($Scope -eq 'machine') {
        # Program Files root and any nested path inside our own uniquely owned
        # parent must be refused before the installer writes. Owned refusal
        # directories are retained as evidence and never recursively deleted.
        if ((Execute-Installer $programFiles64) -eq 0) { throw 'Machine root destination accepted.' }
        if (Test-Path $key) { throw 'Rejected root install created registry state.' }
        $nestedParent = Join-Path $programFiles64 ('VA Studio QA parent '+[guid]::NewGuid().ToString('N'))
        $null = New-Item -ItemType Directory -Path $nestedParent
        $nestedSentinel = Join-Path $nestedParent 'sentinel.txt'
        'preserve' | Set-Content -LiteralPath $nestedSentinel
        $nestedChild = Join-Path $nestedParent 'child'
        if (Test-Path -LiteralPath $nestedChild) { throw 'Expected missing nested destination.' }
        if ((Execute-Installer $nestedChild) -eq 0) { throw 'Machine nested destination accepted.' }
        if (Test-Path -LiteralPath $nestedChild) { throw 'Rejected machine nested install created its destination.' }
        if ((Get-Content -LiteralPath $nestedSentinel) -ne 'preserve') { throw 'Nested refusal changed owned sentinel.' }
        if (Test-Path $key) { throw 'Rejected nested install created registry state.' }
        # Retain the owned refusal parent as evidence; a rejected installer may
        # have written unexpected artifacts that must not be erased.
        Assert-OwnedSentinelOnly $nestedParent 'sentinel.txt'
    }
    if ((Execute-Installer $existing) -eq 0) { throw 'Existing destination accepted.' }
    if ((Get-Content (Join-Path $existing 'sentinel.txt')) -ne 'preserve') { throw 'Existing data changed.' }
    if (Test-Path $key) { throw 'Rejected install created registry state.' }
    if ($Scope -eq 'machine') {
        # Retained as evidence exactly like the nested refusal parent.
        Assert-OwnedSentinelOnly $existing 'sentinel.txt'
    }
    if ((Execute-Installer $install) -ne 0) { throw 'Internal-test installation failed.' }
    if ((Get-ItemProperty $key).InstallLocation -ne $install) { throw 'Wrong uninstall identity.' }
    if ($Scope -eq 'machine') {
        foreach ($shortcut in @($desktopLink,$startMenuFolder,$startMenuLink)) {
            if (-not (Test-Path -LiteralPath $shortcut)) { throw ('Missing machine all-users shortcut: '+$shortcut) }
        }
    }
    $install | Set-Content (Join-Path $Evidence 'installed-directory.txt')
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    foreach ($name in @('INKSCAPE_DATADIR','INKSCAPE_LOCALEDIR','INKSCAPE_PROFILE_DIR',
                        'PYTHONHOME','PYTHONPATH','GSETTINGS_SCHEMA_DIR','GDK_PIXBUF_MODULE_FILE')) {
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    $env:PYTHONDONTWRITEBYTECODE = '1'
    & $HarnessPython -B $RuntimeCheck --payload $install --evidence (Join-Path $Evidence 'runtime')
    if ($LASTEXITCODE -ne 0) { throw 'Installed payload smoke failed.' }
    # Open a real GUI window from the delivered launcher in the ordinary desktop.
    $env:VACARDS_TEST_PROFILE_DIR = Join-Path $Evidence 'gui-profile'
    $launcher = Start-Process -FilePath (Join-Path $install 'VACards-Test.exe') `
        -ArgumentList ('"'+(Join-Path $Evidence 'runtime\saved.svg')+'"') -PassThru
    $null = $launcher.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    $app = $null
    do {
        Start-Sleep -Milliseconds 200
        $children = @(Get-CimInstance Win32_Process -Filter ('ParentProcessId='+$launcher.Id))
        foreach ($child in $children) {
            if ($child.Name -eq 'inkscape.exe') {
                $app = Get-Process -Id $child.ProcessId
                $app.Refresh()
            }
        }
    } while (($null -eq $app -or $app.MainWindowHandle -eq 0) -and [DateTime]::UtcNow -lt $deadline -and -not $launcher.HasExited)
    if ($null -eq $app -or $app.MainWindowHandle -eq 0) { throw 'Delivered GUI did not open.' }
    # Verify the actual HWND icons, not just the PE file's resource inventory.
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
public static class VAStudioWindowIcons {
    [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr LoadImageW(IntPtr instance, string name, uint type, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern bool DestroyIcon(IntPtr icon);
    public static void Verify(IntPtr hwnd, string officialIcon, string evidence) {
        foreach (int size in new int[] {0, 1}) {
            IntPtr actual = SendMessageW(hwnd, 0x007F, new IntPtr(size), IntPtr.Zero);
            if (actual == IntPtr.Zero) throw new Exception("Window icon is missing: " + size);
            using (Icon icon = Icon.FromHandle(actual))
            using (Bitmap bitmap = icon.ToBitmap()) {
                IntPtr expected = LoadImageW(IntPtr.Zero, officialIcon, 1, bitmap.Width, bitmap.Height, 0x0010);
                if (expected == IntPtr.Zero) throw new Exception("Cannot load official VA icon");
                try {
                    using (Icon expectedIcon = Icon.FromHandle(expected))
                    using (Bitmap reference = expectedIcon.ToBitmap()) {
                        bitmap.Save(Path.Combine(evidence, "window-icon-" + size + ".png"), ImageFormat.Png);
                        reference.Save(Path.Combine(evidence, "expected-icon-" + size + ".png"), ImageFormat.Png);
                        for (int y = 0; y < bitmap.Height; ++y)
                            for (int x = 0; x < bitmap.Width; ++x)
                                if (bitmap.GetPixel(x, y).ToArgb() != reference.GetPixel(x, y).ToArgb())
                                    throw new Exception("Window icon differs from VA artwork: " + size);
                    }
                } finally { DestroyIcon(expected); }
            }
        }
    }
}
'@
    [VAStudioWindowIcons]::Verify($app.MainWindowHandle,
        (Join-Path $install 'share\vacards-test\VACards-AppIcon.ico'), $Evidence)
    if ($app.MainWindowTitle -notmatch 'VA Studio') { throw 'Window title is not VA Studio.' }
    $app.MainWindowTitle | Set-Content (Join-Path $Evidence 'window-title.txt')
    $loaded = @($app.Modules | Select-Object -ExpandProperty FileName)
    $loaded | Set-Content (Join-Path $Evidence 'gui-loaded-modules.txt')
    foreach ($module in $loaded) {
        if (-not ($module.StartsWith($install+'\',[StringComparison]::OrdinalIgnoreCase) -or
                  $module.StartsWith($env:SystemRoot+'\',[StringComparison]::OrdinalIgnoreCase))) {
            throw ('GUI loaded external dependency: '+$module)
        }
    }
    if (-not ($loaded -match 'libcairo-2.dll')) { throw 'Patched Cairo was not loaded.' }
    if (-not ($loaded -match 'libcdr-0.1.dll')) { throw 'Enhanced libcdr was not loaded.' }
    $app.CloseMainWindow() | Out-Null
    if (-not $app.WaitForExit(30000)) { throw 'GUI did not close normally.' }
    if (-not $launcher.WaitForExit(30000) -or $launcher.ExitCode -ne 0) { throw 'Launcher shutdown failed.' }
    'keep user data' | Set-Content (Join-Path $install 'unowned-sentinel.txt')
    $uninstaller = Join-Path $install 'Uninstall-VACards-Test.exe'
    $remove = Start-Process -FilePath $uninstaller -ArgumentList '/S' -PassThru
    $null = $remove.Handle
    if (-not $remove.WaitForExit(90000) -or $remove.ExitCode -ne 0) { throw 'Uninstall failed.' }
    # NSIS hands off to its temporary uninstaller copy so it can delete itself.
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    while (((Test-Path $key) -or (Test-Path -LiteralPath $uninstaller)) -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    if (Test-Path $key) { throw 'Test registry key remains.' }
    if (Test-Path -LiteralPath $uninstaller) { throw 'Uninstaller remains.' }
    if ($Scope -eq 'machine') {
        foreach ($shortcut in @($desktopLink,$startMenuFolder,$startMenuLink)) {
            if (Test-Path -LiteralPath $shortcut) { throw ('Uninstall left machine all-users shortcut: '+$shortcut) }
        }
    }
    # No payload whitelist: enumerate every remaining regular (including hidden)
    # file and allow only the deliberately preserved unknown sentinel. The
    # runtime smoke already proved the installed payload writes no own files
    # into the install tree, so there is no legitimate own-runtime leftover.
    $retained = @()
    if (Test-Path -LiteralPath $install) {
        $retained = @(Get-ChildItem -LiteralPath $install -Recurse -File -Force |
            ForEach-Object { $_.FullName.Substring($install.Length).TrimStart('\','/') })
    }
    $allowed = @('unowned-sentinel.txt')
    $extra = @($retained | Where-Object { $allowed -notcontains $_ })
    $missing = @($allowed | Where-Object { $retained -notcontains $_ })
    if ($extra.Count -ne 0) { throw ('Uninstall left owned/unknown files: ' + ($extra -join ', ')) }
    if ($missing.Count -ne 0) { throw ('Uninstall removed deliberately preserved files: ' + ($missing -join ', ')) }
    if ((Get-Content (Join-Path $install 'unowned-sentinel.txt')) -ne 'keep user data') { throw 'Uninstall changed unknown user data.' }
    $after = @(Snapshot-Registrations)
    $after | Set-Content (Join-Path $Evidence 'registrations-after.txt')
    if (Compare-Object $before $after) { throw 'Stock registration or PATH changed.' }
    $checks = @('existing-folder-rejected','per-user-install','absent-destination-parents','unicode-install-path',
                'runtime-smoke','real-GUI-open-close','bundled-GUI-dependencies',
                'own-uninstall','uninstall-tree-clean','unknown-file-preserved','registry-and-PATH-unchanged')
    if ($Scope -eq 'machine') {
        $checks = @('existing-folder-rejected','machine-install','absent-destination-parents',
                    'runtime-smoke','real-GUI-open-close','bundled-GUI-dependencies',
                    'own-uninstall','uninstall-tree-clean','unknown-file-preserved','registry-and-PATH-unchanged',
                    'machine-root-rejected','machine-nested-rejected','machine-key-and-shortcuts-absent',
                    'machine-hklm-identity','machine-allusers-shortcuts-created-and-removed')
    }
    $pass = @{
        passed=$true; scope=$Scope; elevated=$token.elevated; installer=(Get-FileHash $Installer).Hash;
        source_commit=$manifest.source_commit; version=$version;
        checks=$checks
        retained_test_directory=$install; retained_profile=(Join-Path $Evidence 'gui-profile')
    }
    if ($Scope -eq 'machine') {
        # Owned refusal directories are intentionally retained as evidence.
        $pass.retained_refusal_directories = @($nestedParent, $existing)
    }
    $pass | ConvertTo-Json | Set-Content (Join-Path $Evidence 'PASS.json')
} catch {
    $_ | Out-String | Set-Content (Join-Path $Evidence 'FAIL.txt')
    throw
} finally {
    Stop-Transcript | Out-Null
}
