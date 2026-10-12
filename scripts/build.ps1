param([string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskVendor = Join-Path $taskRoot 'vendor'
$taskFtdiDirectory = Join-Path $taskRoot 'runtime/ftdi'
$taskMissingFtdi = @('FTD2XX.dll', 'LibFT4222.dll') | Where-Object { !(Test-Path -LiteralPath (Join-Path $taskFtdiDirectory $_)) }
if ($taskMissingFtdi) {
    Write-Output ('RF capture runtime not staged: ' + ($taskMissingFtdi -join ', ') + '. The app can be built; obtain the two optional DLLs separately to use RF capture.')
    Write-Output 'FTD2XX.dll: https://ftdichip.com/drivers/d2xx-drivers/ (Windows Desktop x64 driver package)'
    Write-Output 'LibFT4222.dll: https://ftdichip.com/software-examples/ft4222h-software-examples/ (Windows x64 library; rename LibFT4222-64.dll)'
}
$taskSdk = Join-Path $taskVendor 'hamlib-w64-4.7.2'
if (!(Test-Path -LiteralPath (Join-Path $taskSdk 'include/hamlib/rig.h'))) {
    New-Item -ItemType Directory -Force -Path $taskVendor | Out-Null
    $taskArchive = Join-Path $taskVendor 'hamlib-w64-4.7.2.zip'
    Invoke-WebRequest 'https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-w64-4.7.2.zip' -OutFile $taskArchive
    $taskExpected = '8553bc6c5c6032e8debf99c017e98f58fed7e07e7c25d04815dc3e8bbe3304c7'
    if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $taskExpected) {
        throw 'Downloaded Hamlib archive failed SHA256 verification.'
    }
    Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskVendor -Force
}
$taskBuild = Join-Path $taskRoot 'build'
$audioArchive = Join-Path $taskVendor 'portaudio-v19.7.0.zip'
$audioSource = Join-Path $taskVendor 'portaudio-19.7.0'
if (!(Test-Path -LiteralPath (Join-Path $audioSource 'include/portaudio.h'))) {
    Invoke-WebRequest 'https://codeload.github.com/PortAudio/portaudio/zip/refs/tags/v19.7.0' -OutFile $audioArchive
    $audioExpected = 'ce1e7b27ad362cb9745de7da3d266a996b085ca75a12ac62c881f88ab6894acf'
    if ((Get-FileHash -LiteralPath $audioArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $audioExpected) {
        throw 'Downloaded PortAudio source failed SHA256 verification.'
    }
    Expand-Archive -LiteralPath $audioArchive -DestinationPath $taskVendor -Force
}
cmake -S $taskRoot -B $taskBuild -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $taskBuild
if ($LASTEXITCODE -ne 0) { throw 'C++ compilation failed.' }
cmake --install $taskBuild --prefix (Join-Path $taskRoot 'dist')
if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
Write-Output ('Application (dashboard, audio scopes, RF scope, and web interface): ' + (Join-Path $taskRoot 'dist/FT710Dashboard.exe'))
