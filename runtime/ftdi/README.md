# FTDI FT4222 runtime files

The RF scope loads these **64-bit Windows** runtime libraries dynamically:

- `FTD2XX.dll`
- `LibFT4222.dll`

Download the libraries from [FTDI's FT4222H software examples and downloads](https://ftdichip.com/software-examples/ft4222h-software-examples/). Place the DLLs in this directory before configuring/building. CMake and `scripts/build.ps1` report an error when either file is missing. The build copies them beside the dashboard executable.

The DLLs are ignored by Git. This directory contains only this note in the source repository; install the matching FTDI USB driver from [FTDI's driver page](https://ftdichip.com/drivers/) on the Windows PC connected to the radio.
