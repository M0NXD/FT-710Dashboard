# Third-party components in FT-710 Dashboard

The Windows x64 release ZIP includes three external runtime DLLs: `libhamlib-4.dll`, `libusb-1.0.dll` and `libwinpthread-1.dll`. Keep this notice and the `licenses/` directory with the application when redistributing the package. The individual components retain their upstream licences and copyrights. No endorsement by their authors is implied.

The separate **FT710Dashboard-2.0.5-ThirdPartySources.zip** on the [same GitHub release page](https://github.com/M0NXD/FT-710Dashboard/releases/tag/v2.0.5) provides the complete, unmodified Hamlib 4.7.2 and libusb 1.0.24 source archives, including their upstream build files. It also provides the MinGW-w64 13.0.0 and PortAudio 19.7.0 source archives. `DEPENDENCIES.json` in both ZIPs records upstream download URLs, component versions and SHA-256 checksums. These source archives are available without charge and without requiring a binary download.

## Hamlib 4.7.2

- File: `libhamlib-4.dll`, unmodified from the official [Hamlib Windows x64 package](https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-w64-4.7.2.zip).
- Copyright: the Hamlib Group and contributors, including Frank Singleton and Stephane Fillod. The complete upstream contributor list is in `licenses/hamlib/AUTHORS.txt`.
- Licence: **GNU Lesser General Public License, version 2.1 or later**.
- Included notices: `licenses/hamlib/LICENSE.txt`, `licenses/hamlib/COPYING.LIB.txt`, `licenses/hamlib/AUTHORS.txt` and `licenses/hamlib/COPYING.txt`. The runtime library is LGPL; the extra GPL text accompanies the upstream AUTHORS document, which references it.
- Corresponding source: `hamlib-4.7.2.tar.gz` in the source ZIP, also available [directly from Hamlib](https://github.com/Hamlib/Hamlib/releases/download/4.7.2/hamlib-4.7.2.tar.gz). The upstream Windows build instructions and scripts are included in that archive. The dashboard does not modify the supplied Hamlib DLL.

## libusb 1.0.24

- File: `libusb-1.0.dll` (file version **1.0.24.11584**), unmodified from the same Hamlib Windows x64 package.
- Copyright: libusb authors and contributors; see `licenses/libusb/AUTHORS.txt` and the per-file notices in the source archive.
- Licence: **GNU Lesser General Public License, version 2.1 or later**.
- Included notices: `licenses/libusb/COPYING.txt` and `licenses/libusb/AUTHORS.txt`.
- Corresponding source: `libusb-1.0.24.tar.bz2` in the source ZIP, also available [directly from libusb](https://github.com/libusb/libusb/releases/download/v1.0.24/libusb-1.0.24.tar.bz2). Its upstream Windows/MinGW build files are included. The dashboard does not modify the supplied libusb DLL.

### Use and replacement of the LGPL libraries

Hamlib and libusb are dynamically linked as separate DLLs. You may replace them with interface-compatible modified builds. For compliance with LGPL 2.1 section 6, permission is granted to modify this application for your own use and to reverse engineer it for debugging such library modifications. This permission does not restrict the rights granted by the LGPL to the libraries themselves. The application imposes no DLL signing requirement or runtime checksum lock; the release packager's checks identify the original files being distributed.

Keep the LGPL notices and corresponding sources available when redistributing these DLLs. The full licence terms and warranty disclaimers are supplied in the files listed above.

## MinGW-w64 winpthreads 13.0.0

- File: `libwinpthread-1.dll`, from [WinLibs GCC 15.2.0, x64 POSIX UCRT, release 4](https://github.com/brechtsanders/winlibs_mingw/releases/tag/15.2.0posix-13.0.0-ucrt-r4).
- Copyright: (c) 2011 mingw-w64 project; portions (C) 2010 Lockless Inc.
- Licence: the **MIT permission notice and BSD redistribution conditions** reproduced in full in `licenses/winpthreads/COPYING.txt`.
- The MinGW-w64 root and aggregate runtime notices are also included under `licenses/mingw-w64/` to cover MinGW runtime/header code used by the build.
- Source: `mingw-w64-v13.0.0.tar.gz` in the source ZIP, from the official [MinGW-w64 v13.0.0 tag](https://github.com/mingw-w64/mingw-w64/tree/v13.0.0). The threading library is under `mingw-w64-libraries/winpthreads/`.

## GCC runtime code

The executable uses GCC 15.2.0 runtime code, including statically linked libstdc++. Credit belongs to the **Free Software Foundation and GNU GCC contributors**; individual source files retain their respective copyright notices. It was compiled by GCC through an eligible compilation process. These components are covered by **GPL version 3 with the GCC Runtime Library Exception version 3.1**. The complete GPL and exception are included as `licenses/gcc/COPYING3.txt` and `licenses/gcc/COPYING.RUNTIME.txt`.

See the [GCC runtime licence explanation](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html) and the [GCC 15.2.0 source tree](https://github.com/gcc-mirror/gcc/tree/releases/gcc-15.2.0). The runtime exception permits distribution of the application together with this runtime code under terms consistent with the independent application. No separate GCC runtime DLL or compiler is included in the ZIP.

The standard-library headers also retain original STL notices credited to **Hewlett-Packard Company** and **Silicon Graphics Computer Systems, Inc.** Its shared-pointer implementation acknowledges Boost-derived work by **Peter Dimov and Multi Media Ltd.**, **Greg Colvin** and **Beman Dawes**. Their original copyright/permission comments, together with the GNU contributor notices, are preserved in `licenses/gcc/SOURCE_NOTICES.txt`. This catalogue covers the pinned toolchain's complete C++ header tree and also includes notices for headers unused by this application. The original **Boost Software License 1.0** is supplied in `licenses/gcc/BOOST_LICENSE_1_0.txt`. Both notice catalogues and the GCC/Boost licence texts are also included in the source ZIP.

### WinLibs compiler distribution

**Brecht Sanders** provides the [WinLibs GCC 15.2.0 / MinGW-w64 13.0.0 x64 POSIX UCRT release 4 build](https://github.com/brechtsanders/winlibs_mingw/releases/tag/15.2.0posix-13.0.0-ucrt-r4) used to compile the application and obtain its matching threading DLL. WinLibs packages the upstream toolchain components; their own licences and copyrights remain applicable. The application ZIP contains the threading runtime, not the compiler/tool collection.

## PortAudio 19.7.0

PortAudio is statically linked into `FT710Dashboard.exe`; no PortAudio DLL is required. Copyright (c) 1999-2006 Ross Bencina and Phil Burk, with additional per-file copyright notices for the implementation contributors. Its complete permission notice and disclaimer are in `licenses/portaudio/LICENSE.txt`.

The Windows **WDM-KS** implementation credits **Andrew Baldwin** and **Robert Bielik (WaveRT)**, based on the API proposed by Ross Bencina; its copyright notice credits Andrew Baldwin, Ross Bencina and Phil Burk. The **WASAPI** implementation credits **David Viens** and **Dmitry Kostjuchenko**, alongside Ross Bencina and Phil Burk. Common and Windows support code carries its original per-file author/copyright notices.

`licenses/portaudio/SOURCE_NOTICES.txt` preserves the original copyright, author, permission and disclaimer comment blocks from the pinned common core, Windows support, skeleton, WASAPI and WDM-KS sources and their public headers. This includes the non-binding requests attached to the upstream permission notices. The same catalogue is included in the source ZIP as `PORTAUDIO_SOURCE_NOTICES.txt`.

The source ZIP contains the unmodified upstream `portaudio-v19.7.0.zip`, from the [PortAudio v19.7.0 tag](https://github.com/PortAudio/portaudio/tree/v19.7.0), plus the dashboard's adapted WDM-KS implementation as `dashboard-portaudio_wdmks.c`. That adaptation is also in [the application source](https://github.com/M0NXD/FT-710Dashboard/blob/v2.0.5/src/portaudio_wdmks.c) and retains its upstream notices.

## FTDI libraries are obtained separately

The optional D2XX and LibFT4222 libraries are the work of **Future Technology Devices International Limited (FTDI)** and are supplied under FTDI's own terms. No FTDI DLLs, drivers or SDK files are included in the application ZIP, source ZIP or source repository. For RF scope capture, obtain the native x64 files separately:

- **FTD2XX.dll:** [FTDI D2XX Windows desktop drivers](https://ftdichip.com/drivers/d2xx-drivers/).
- **LibFT4222.dll:** [FTDI FT4222H Windows library and examples](https://ftdichip.com/software-examples/ft4222h-software-examples/).

Follow the README's file locations/naming instructions and enable **SCU-LAN10 = ON** on the FT-710.
