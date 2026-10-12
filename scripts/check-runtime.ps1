param([string]$ApplicationDirectory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$ApplicationDirectory = (Resolve-Path -LiteralPath $ApplicationDirectory).Path
$env:PATH = $ApplicationDirectory + ';' + $env:PATH
# This script runs independently of the EXE so missing loader DLLs can be diagnosed.
function Find-StationRuntime([string]$Name) {
    foreach ($runtimeFolder in (@($ApplicationDirectory) + ($env:PATH -split ';'))) {
        if (!$runtimeFolder) { continue }
        $runtimeCandidate = Join-Path $runtimeFolder.Trim('"') $Name
        if (Test-Path -LiteralPath $runtimeCandidate -PathType Leaf) { return (Resolve-Path -LiteralPath $runtimeCandidate).Path }
    }
    return $null
}
function Test-X64Runtime([string]$Path) {
    $runtimeBytes = [IO.File]::ReadAllBytes($Path)
    if ($runtimeBytes.Length -lt 64 -or [BitConverter]::ToUInt16($runtimeBytes,0) -ne 0x5a4d) { return $false }
    $runtimePe = [BitConverter]::ToInt32($runtimeBytes,0x3c)
    return $runtimePe -ge 0 -and $runtimePe + 6 -le $runtimeBytes.Length -and [BitConverter]::ToUInt32($runtimeBytes,$runtimePe) -eq 0x4550 -and [BitConverter]::ToUInt16($runtimeBytes,$runtimePe+4) -eq 0x8664
}
$runtimeMissing = $false
$runtimeNames = @('libhamlib-4.dll','libusb-1.0.dll','libwinpthread-1.dll')
foreach ($runtimeName in $runtimeNames) {
    $runtimePath = Find-StationRuntime $runtimeName
    if (!$runtimePath) { Write-Output ('MISSING: ' + $runtimeName);$runtimeMissing=$true;continue }
    if (!(Test-X64Runtime $runtimePath)) { Write-Output ('WRONG ARCHITECTURE: ' + $runtimePath);$runtimeMissing=$true;continue }
    Write-Output ('FOUND x64: ' + $runtimePath)
}
if (!$runtimeMissing) {
    # Load by the same search path as the application and check required APIs.
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class StationRuntimeProbe {
  [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibraryW(string path);
  [DllImport("kernel32", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr module,string name);
  [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr module);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate IntPtr Version();
}
'@
    $runtimeThread = [StationRuntimeProbe]::LoadLibraryW((Find-StationRuntime 'libwinpthread-1.dll'))
    try { if ($runtimeThread -eq [IntPtr]::Zero -or [StationRuntimeProbe]::GetProcAddress($runtimeThread,'clock_gettime64') -eq [IntPtr]::Zero) { Write-Output 'INCOMPATIBLE: libwinpthread-1.dll must export clock_gettime64.';$runtimeMissing=$true } }
    finally { if ($runtimeThread -ne [IntPtr]::Zero) { [void][StationRuntimeProbe]::FreeLibrary($runtimeThread) } }
    $runtimeHamlib = [StationRuntimeProbe]::LoadLibraryW((Find-StationRuntime 'libhamlib-4.dll'))
    try {
        if ($runtimeHamlib -eq [IntPtr]::Zero) { Write-Output ('Hamlib failed to load; Windows error ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error());$runtimeMissing=$true }
        else {
            $runtimeVersionAddress = [StationRuntimeProbe]::GetProcAddress($runtimeHamlib,'rig_version')
            if ($runtimeVersionAddress -eq [IntPtr]::Zero) { Write-Output 'INCOMPATIBLE: missing rig_version export.';$runtimeMissing=$true }
            else { $runtimeVersionFunction = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($runtimeVersionAddress,[type][StationRuntimeProbe+Version]);$runtimeVersion = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($runtimeVersionFunction.Invoke());Write-Output ('Hamlib: ' + $runtimeVersion);if ($runtimeVersion -notmatch '^Hamlib 4\.7\.2\b|^4\.7\.2\b') { Write-Output 'Install the matching Hamlib 4.7.2 x64 runtime.';$runtimeMissing=$true } }
        }
    } finally { if ($runtimeHamlib -ne [IntPtr]::Zero) { [void][StationRuntimeProbe]::FreeLibrary($runtimeHamlib) } }
}
foreach ($runtimeName in @('FTD2XX.dll','LibFT4222.dll')) {
    $runtimePath=Find-StationRuntime $runtimeName
    if (!$runtimePath) { Write-Output ('OPTIONAL RF CAPTURE MISSING: ' + $runtimeName) }
    elseif (!(Test-X64Runtime $runtimePath)) { Write-Output ('OPTIONAL RF CAPTURE WRONG ARCHITECTURE: ' + $runtimePath) }
    else { Write-Output ('OPTIONAL RF CAPTURE FOUND x64: ' + $runtimePath) }
}
Write-Output 'Hamlib x64 ZIP: https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-w64-4.7.2.zip'
Write-Output 'WinLibs/MinGW x64 ZIP: https://github.com/brechtsanders/winlibs_mingw/releases/download/15.2.0posix-13.0.0-ucrt-r4/winlibs-x86_64-posix-seh-gcc-15.2.0-mingw-w64ucrt-13.0.0-r4.zip'
Write-Output 'Copy libhamlib-4.dll and libusb-1.0.dll from the Hamlib bin folder; use libwinpthread-1.dll from the WinLibs mingw64/bin folder beside the dashboard EXE.'
Write-Output 'FTD2XX.dll: https://ftdichip.com/drivers/d2xx-drivers/ (Windows Desktop x64 driver package)'
Write-Output 'LibFT4222.dll: https://ftdichip.com/software-examples/ft4222h-software-examples/ (Windows x64 library; rename LibFT4222-64.dll)'
Write-Output 'RF capture requires SCU-LAN10 = ON on the FT-710.'
if ($runtimeMissing) { exit 1 }
Write-Output 'Required runtime checks passed.'
