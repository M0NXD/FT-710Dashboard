param([string]$Executable = '', [string]$OutputDirectory = '')
$ErrorActionPreference = 'Stop'
$releaseRoot = Split-Path -Parent $PSScriptRoot
if (!$Executable) { $Executable = Join-Path $releaseRoot 'dist/FT710Dashboard.exe' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $releaseRoot 'artifacts/releases' }
if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw 'Build FT710Dashboard.exe first.' }
$releaseVersion = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $releaseRoot 'CMakeLists.txt')), 'project\(FT710Dashboard VERSION ([0-9.]+)').Groups[1].Value
if (!$releaseVersion) { throw 'Cannot determine application version.' }
$releaseStage = Join-Path $OutputDirectory ('FT710Dashboard-' + $releaseVersion)
New-Item -ItemType Directory -Force -Path $releaseStage | Out-Null
if (Get-ChildItem -LiteralPath $releaseStage -Force) { throw ('Release staging directory must be empty: ' + $releaseStage) }
# Explicit allowlist: never copy dist/ recursively or publish third-party DLLs.
Copy-Item -LiteralPath $Executable -Destination (Join-Path $releaseStage 'FT710Dashboard.exe')
Copy-Item -LiteralPath (Join-Path $releaseRoot 'README.md') -Destination $releaseStage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'check-runtime.ps1') -Destination $releaseStage
$releaseAllowed = @('FT710Dashboard.exe','README.md','check-runtime.ps1')
$releaseFiles = @(Get-ChildItem -LiteralPath $releaseStage -File)
if ($releaseFiles.Count -ne $releaseAllowed.Count -or ($releaseFiles | Where-Object { $_.Name -notin $releaseAllowed })) { throw 'Release allowlist validation failed.' }
$releaseZip = Join-Path $OutputDirectory ('FT710Dashboard-' + $releaseVersion + '-Windows-x64.zip')
if (Test-Path -LiteralPath $releaseZip) { throw ('Release archive already exists: ' + $releaseZip) }
Compress-Archive -LiteralPath $releaseFiles.FullName -DestinationPath $releaseZip
# Confirm the archive contents, not just the staging directory.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$releaseArchive = [IO.Compression.ZipFile]::OpenRead($releaseZip)
try { if ($releaseArchive.Entries.Count -ne 3 -or ($releaseArchive.Entries | Where-Object { $_.FullName -notin $releaseAllowed })) { throw 'Unexpected release archive content.' } }
finally { $releaseArchive.Dispose() }
$releaseHash = Get-FileHash -LiteralPath $releaseZip -Algorithm SHA256
Set-Content -LiteralPath ($releaseZip + '.sha256') -Value ($releaseHash.Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($releaseZip)) -Encoding ascii
Write-Output ('Application-only release: ' + $releaseZip)
