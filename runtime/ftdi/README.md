# FTDI FT4222 runtime files

The RF scope loads these **64-bit Windows** runtime libraries dynamically:

- `FTD2XX.dll`
- `LibFT4222.dll`

These DLLs come from **two separate packages**:

| Required file | Official download | Where to get the x64 DLL |
| --- | --- | --- |
| `FTD2XX.dll` | [FTDI D2XX drivers](https://ftdichip.com/drivers/d2xx-drivers/) — Windows Desktop x64 package or driver installer | After installing the driver, copy `%WINDIR%\System32\FTD2XX.dll`. If the driver ZIP supplies `amd64\ftd2xx64.dll`, rename that copy to `FTD2XX.dll`. |
| `LibFT4222.dll` | [FTDI FT4222H Windows library and examples](https://ftdichip.com/software-examples/ft4222h-software-examples/) — v1.4.8 | Copy `imports\LibFT4222\dll\amd64\LibFT4222-64.dll` and rename the copy to `LibFT4222.dll`. |

The FT4222H library package does not include the native `FTD2XX.dll`. Use amd64/x64 files; `i386`/`SysWOW64` DLLs, ARM64 files and the .NET wrapper `FTD2XX_NET.dll` are not suitable for this dashboard.

Place the two named DLLs in this directory before configuring/building if you want them staged for RF capture. CMake and `scripts/build.ps1` report missing files as optional dependencies; the dashboard still builds and CAT controls remain available. When present, the build copies the DLLs beside the dashboard executable. For a downloaded release, place them directly beside `FT710Dashboard.exe`.

The DLLs are ignored by Git and excluded from the release ZIP. Install the matching FTDI Windows USB driver from the [D2XX driver page](https://ftdichip.com/drivers/d2xx-drivers/) on the PC connected to the radio. Enable **SCU-LAN10 = ON** on the FT-710 for RF capture.
