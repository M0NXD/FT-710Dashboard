# Third-party sources for FT-710 Dashboard 2.0.5

This source package is provided alongside the Windows application ZIP on the same GitHub release page. It contains:

| File | Component |
| --- | --- |
| `hamlib-4.7.2.tar.gz` | Complete corresponding source for the unmodified Hamlib 4.7.2 runtime DLL, including its upstream Windows build scripts |
| `libusb-1.0.24.tar.bz2` | Complete corresponding source for libusb 1.0.24 (DLL file version 1.0.24.11584), including upstream Windows/MinGW build files |
| `mingw-w64-v13.0.0.tar.gz` | Upstream MinGW-w64 13.0.0 source, including winpthreads |
| `portaudio-v19.7.0.zip` | Unmodified PortAudio 19.7.0 source |
| `dashboard-portaudio_wdmks.c` | The WDM-KS implementation adapted by this dashboard from PortAudio |
| `THIRD_PARTY_NOTICES.md` | Component provenance, licences, copyright references and LGPL replacement permissions |
| `PORTAUDIO_SOURCE_NOTICES.txt` | Original per-file attribution and permission notices for the PortAudio Windows/common code |
| `GCC_SOURCE_NOTICES.txt` | Original GNU, Hewlett-Packard, Silicon Graphics and other attribution/permission notices from the pinned C++ header tree |
| `GCC_COPYING3.txt`, `GCC_COPYING.RUNTIME.txt`, `BOOST_LICENSE_1_0.txt` | Licence texts accompanying the C++ runtime/header notices |
| `DEPENDENCIES.json` | Original download URLs, component versions and SHA-256 checksums |

The original upstream source archives are preserved as downloaded. Extract them using `tar`, Windows ZIP extraction or an archive utility. The archives include the original licences and per-file copyright notices. For building Hamlib, consult its `README.build-Windows` and `scripts/build-w64.sh`; for libusb, consult its `INSTALL` and `README` files and the included MinGW build files. You may use and modify each component under its included upstream licence.

The dashboard source corresponding to the 2.0.5 executable is available at [the application release tag](https://github.com/M0NXD/FT-710Dashboard/tree/v2.0.5). The application uses replaceable external Hamlib and libusb DLLs; the LGPL permissions are explained in `THIRD_PARTY_NOTICES.md`.

This ZIP contains no FTDI libraries or SDK files. FTDI's download locations are documented in the application README and notices.
