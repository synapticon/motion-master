# MU_3SL 3.4.2

MU_3SL is the iC-Haus library that calibrates the iC-MU, a magnetic encoder chip with two
tracks. Motion Master uses it to compute the track corrections in the iC-MU calibration.

This directory holds the headers and the prebuilt binaries as iC-Haus supplied them. There is no
source. The library reports its version as `3.4.2.1`.

| Path | Platform |
| --- | --- |
| `include/` | All platforms |
| `bin/linux/x86_64/libMU_3SL_interface.so.3.4.2` | Linux x86_64 |
| `bin/windows/x86-64/MU_3SL_interface_64.dll` and `.lib` | Windows x64 |

iC-Haus supplies no build for Linux arm64 or macOS. On those platforms, Motion Master builds
without MU_3SL and cannot calibrate an iC-MU encoder.

## Local change

`include/MU_3SL_interface.h` differs from the iC-Haus file in one line. The line
`#include <cstdbool>` is removed. C++17 deprecated that header and C++20 removed it, so GCC
warns on it and the build treats every warning as an error. `bool` is a C++ keyword, so the
header needs nothing from it. Apply the same change when you replace the library with a newer
version.

## Build

`cmake/mu_3sl.cmake` defines the `mm::mu_3sl` target and sets `MM_HAVE_MU_3SL` where a build
exists.

## Licence

MU_3SL is subject to the iC-Haus terms stated at the top of each header:

> Software and its documentation is provided by iC-Haus GmbH or contributors "AS IS" and is
> subject to the ZVEI General Conditions for the Supply of Products and Services with iC-Haus
> amendments and the ZVEI Software clause with iC-Haus amendments (http://www.ichaus.de/EULA).
