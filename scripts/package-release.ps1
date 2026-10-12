param([string]$Executable = '', [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$releaseRoot = Split-Path -Parent $PSScriptRoot
if (!$Executable) { $Executable = Join-Path $releaseRoot 'dist/FT710Dashboard.exe' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $releaseRoot 'artifacts/releases' }
if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'Build FT710Dashboard.exe first.' }
$releaseVersion = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $releaseRoot 'CMakeLists.txt')), 'project\(FT710Dashboard VERSION ([0-9.]+)').Groups[1].Value
if (!$releaseVersion) { throw 'Cannot determine application version.' }
$releaseDependencies = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'release-dependencies.json') | ConvertFrom-Json
$releaseRuntimeDirectory = Split-Path -Parent (Resolve-Path -LiteralPath $Executable).Path
# These are the only third-party DLLs approved for the distributable package.
$releaseRuntimeNames = @('libhamlib-4.dll','libusb-1.0.dll','libwinpthread-1.dll')
if (Compare-Object ($releaseRuntimeNames | Sort-Object) ($releaseDependencies.runtimeLibraries.file | Sort-Object)) { throw 'Dependency lock must list exactly the three approved runtime DLLs.' }
foreach ($releaseLibrary in $releaseDependencies.runtimeLibraries) {
    $releaseLibraryPath = Join-Path $releaseRuntimeDirectory $releaseLibrary.file
    if (!(Test-Path -LiteralPath $releaseLibraryPath -PathType Leaf)) { throw ('Required release DLL missing: ' + $releaseLibraryPath) }
    if ((Get-FileHash -LiteralPath $releaseLibraryPath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $releaseLibrary.sha256) {
        throw ('Runtime differs from its approved source/version: ' + $releaseLibrary.file + '. Use the pinned Hamlib/WinLibs downloads in scripts/release-dependencies.json.')
    }
}

function Get-ReleaseDependency($Dependency) {
    $releaseCachePath = Join-Path $releaseRoot $Dependency.cache
    if (!(Test-Path -LiteralPath $releaseCachePath -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $releaseCachePath) | Out-Null
        Invoke-WebRequest -Uri $Dependency.url -OutFile $releaseCachePath -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $releaseCachePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Dependency.sha256) { throw ('Dependency checksum mismatch: ' + $Dependency.file) }
    return $releaseCachePath
}

$releaseCache = @{}
foreach ($releaseDependency in $releaseDependencies.downloads) { $releaseCache[$releaseDependency.id] = Get-ReleaseDependency $releaseDependency }

$releaseStage = Join-Path $OutputDirectory ('FT710Dashboard-' + $releaseVersion)
$releaseSourceStage = Join-Path $OutputDirectory ('FT710Dashboard-' + $releaseVersion + '-ThirdPartySources')
foreach ($releaseDirectory in @($releaseStage,$releaseSourceStage)) {
    New-Item -ItemType Directory -Force -Path $releaseDirectory | Out-Null
    if (Get-ChildItem -LiteralPath $releaseDirectory -Force) { throw ('Release staging directory must be empty: ' + $releaseDirectory) }
}
# Never copy dist/ recursively: optional FTDI and unrelated runtime DLLs stay out.
Copy-Item -LiteralPath $Executable -Destination (Join-Path $releaseStage 'FT710Dashboard.exe')
Copy-Item -LiteralPath (Join-Path $releaseRoot 'README.md') -Destination $releaseStage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-runtime.ps1') -Destination $releaseStage
Copy-Item -LiteralPath (Join-Path $releaseRoot 'THIRD_PARTY_NOTICES.md') -Destination $releaseStage
foreach ($releaseLibraryName in $releaseRuntimeNames) { Copy-Item -LiteralPath (Join-Path $releaseRuntimeDirectory $releaseLibraryName) -Destination $releaseStage }

$releaseLicences = @(
    @{dependency='hamlib';member='hamlib-4.7.2/LICENSE';target='licenses/hamlib/LICENSE.txt'},
    @{dependency='hamlib';member='hamlib-4.7.2/COPYING.LIB';target='licenses/hamlib/COPYING.LIB.txt'},
    @{dependency='hamlib';member='hamlib-4.7.2/AUTHORS';target='licenses/hamlib/AUTHORS.txt'},
    @{dependency='libusb';member='libusb-1.0.24/COPYING';target='licenses/libusb/COPYING.txt'},
    @{dependency='libusb';member='libusb-1.0.24/AUTHORS';target='licenses/libusb/AUTHORS.txt'},
    @{dependency='mingw-w64';member='mingw-w64-13.0.0/mingw-w64-libraries/winpthreads/COPYING';target='licenses/winpthreads/COPYING.txt'},
    @{dependency='mingw-w64';member='mingw-w64-13.0.0/COPYING';target='licenses/mingw-w64/COPYING.txt'},
    @{dependency='mingw-w64';member='mingw-w64-13.0.0/AUTHORS';target='licenses/mingw-w64/AUTHORS.txt'},
    @{dependency='mingw-w64';member='mingw-w64-13.0.0/COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt';target='licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt'},
    @{dependency='mingw-w64';member='mingw-w64-13.0.0/COPYING.MinGW-w64/COPYING.MinGW-w64.txt';target='licenses/mingw-w64/COPYING.MinGW-w64.txt'},
    @{dependency='portaudio';member='portaudio-19.7.0/LICENSE.txt';target='licenses/portaudio/LICENSE.txt'},
    @{dependency='gcc-license';target='licenses/gcc/COPYING3.txt'},
    @{dependency='gcc-exception';target='licenses/gcc/COPYING.RUNTIME.txt'}
)
$releaseExtracted = @{}
foreach ($releaseGroup in ($releaseLicences | Where-Object { $_.member } | Group-Object dependency)) {
    $releaseExtractDirectory = Join-Path $releaseRoot ('vendor/release-dependencies/license-extract/' + $releaseGroup.Name)
    New-Item -ItemType Directory -Force -Path $releaseExtractDirectory | Out-Null
    $releaseMembers = @($releaseGroup.Group | ForEach-Object { $_.member })
    & tar -xf $releaseCache[$releaseGroup.Name] -C $releaseExtractDirectory @releaseMembers
    if ($LASTEXITCODE) { throw ('Cannot extract dependency notices: ' + $releaseGroup.Name) }
    $releaseExtracted[$releaseGroup.Name] = $releaseExtractDirectory
}
foreach ($releaseLicence in $releaseLicences) {
    $releaseLicenceSource = if ($releaseLicence.member) { Join-Path $releaseExtracted[$releaseLicence.dependency] $releaseLicence.member } else { $releaseCache[$releaseLicence.dependency] }
    if (!(Test-Path -LiteralPath $releaseLicenceSource -PathType Leaf) -or (Get-Item -LiteralPath $releaseLicenceSource).Length -eq 0) { throw ('Missing dependency notice: ' + $releaseLicence.target) }
    $releaseLicenceTarget = Join-Path $releaseStage $releaseLicence.target
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $releaseLicenceTarget) | Out-Null
    Copy-Item -LiteralPath $releaseLicenceSource -Destination $releaseLicenceTarget
}

$releaseSourceArchives = @($releaseDependencies.downloads | Where-Object { $_.sourceArchive })
$releaseManifest = [ordered]@{
    applicationVersion = $releaseVersion
    executableSha256 = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash.ToLowerInvariant()
    toolchain = $releaseDependencies.toolchain
    runtimeLibraries = $releaseDependencies.runtimeLibraries
    sourceArchives = @($releaseSourceArchives | Select-Object id,file,url,sha256)
    excludedRuntimeFiles = @('FTD2XX.dll','LibFT4222.dll')
}
$releaseManifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $releaseStage 'DEPENDENCIES.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $releaseStage 'DEPENDENCIES.json') -Destination $releaseSourceStage
Copy-Item -LiteralPath (Join-Path $releaseRoot 'THIRD_PARTY_NOTICES.md') -Destination $releaseSourceStage
Copy-Item -LiteralPath (Join-Path $releaseRoot 'docs/THIRD_PARTY_SOURCES.md') -Destination (Join-Path $releaseSourceStage 'README.md')
Copy-Item -LiteralPath (Join-Path $releaseRoot 'src/portaudio_wdmks.c') -Destination (Join-Path $releaseSourceStage 'dashboard-portaudio_wdmks.c')
foreach ($releaseSourceArchive in $releaseSourceArchives) { Copy-Item -LiteralPath $releaseCache[$releaseSourceArchive.id] -Destination (Join-Path $releaseSourceStage $releaseSourceArchive.file) }

# Validate exact file lists in both staging trees and finished ZIPs.
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Write-ReleaseZip([string]$Stage, [string]$Name, [string[]]$Allowed) {
    $releaseAbsoluteStage = (Resolve-Path -LiteralPath $Stage).Path
    $releaseActual = @(Get-ChildItem -LiteralPath $Stage -File -Recurse | ForEach-Object { $_.FullName.Substring($releaseAbsoluteStage.Length + 1).Replace('\','/') })
    if ($releaseActual.Count -ne $Allowed.Count -or (Compare-Object ($releaseActual | Sort-Object) ($Allowed | Sort-Object))) { throw ('Release allowlist validation failed: ' + $Name) }
    $releaseZipPath = Join-Path $OutputDirectory $Name
    if (Test-Path -LiteralPath $releaseZipPath) { throw ('Release archive already exists: ' + $releaseZipPath) }
    [IO.Compression.ZipFile]::CreateFromDirectory($releaseAbsoluteStage,$releaseZipPath,[IO.Compression.CompressionLevel]::Optimal,$false)
    $releaseArchive = [IO.Compression.ZipFile]::OpenRead($releaseZipPath)
    try {
        $releaseEntryNames = @($releaseArchive.Entries | Where-Object { !$_.FullName.EndsWith('/') } | ForEach-Object { $_.FullName })
        if ($releaseEntryNames.Count -ne $Allowed.Count -or (Compare-Object ($releaseEntryNames | Sort-Object) ($Allowed | Sort-Object))) { throw ('Unexpected release archive content: ' + $Name) }
        foreach ($releaseDirectoryEntry in ($releaseArchive.Entries | Where-Object { $_.FullName.EndsWith('/') })) {
            if (!($Allowed | Where-Object { $_.StartsWith($releaseDirectoryEntry.FullName,[StringComparison]::Ordinal) })) { throw 'Unexpected archive directory.' }
        }
    } finally { $releaseArchive.Dispose() }
    $releaseZipHash = (Get-FileHash -LiteralPath $releaseZipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath ($releaseZipPath + '.sha256') -Value ($releaseZipHash + '  ' + [IO.Path]::GetFileName($releaseZipPath)) -Encoding ascii
    Write-Output ($Name + ' SHA256: ' + $releaseZipHash)
}
$releaseAllowed = @('FT710Dashboard.exe','README.md','check-runtime.ps1','THIRD_PARTY_NOTICES.md','DEPENDENCIES.json') + $releaseRuntimeNames + @($releaseLicences | ForEach-Object { $_.target })
$releaseSourceAllowed = @('README.md','THIRD_PARTY_NOTICES.md','DEPENDENCIES.json','dashboard-portaudio_wdmks.c') + @($releaseSourceArchives | ForEach-Object { $_.file })
Write-ReleaseZip $releaseStage ('FT710Dashboard-' + $releaseVersion + '-Windows-x64.zip') $releaseAllowed
Write-ReleaseZip $releaseSourceStage ('FT710Dashboard-' + $releaseVersion + '-ThirdPartySources.zip') $releaseSourceAllowed
