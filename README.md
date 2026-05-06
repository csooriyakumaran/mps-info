# MPS-INFO

Prints scan information from an MPS output binary file to the console. 

Uses the [Scanivalve-MPS-Protocol](https://github.com/csooriyakumaran/scanivalve-mps-protocol), as defined in the [Scanivavle Hardware, Software, and User Manual](https://scanivalve.com/wp-content/uploads/2026/03/MPS4200_v401_260304.pdf).



# USAGE

```powershell
mps-info.exe <path/to/datafile>

```

***OUTPUT***

```text
-----------------------------------------------------------
 -- File Path --
           .\data\test-4264-eu-1000Hz-sim0x40-1M-Frames.dat
-----------------------------------------------------------
 -- Scanner Type                                   MPS-4264
 -- File Size                               348000000 bytes
 -- Packet Type                                        0x0A
 -- Start Time                06-04-2026 10:58:29.704137300
 -- Scan Duration                               1000.000  s
 -- Scan Rate                                   1000.000 Hz
 -- Units                                                PA
 -- Unit Conversion                                6894.760
 -- T-Channels                                            8
 -- P-Channels                                           64
-----------------------------------------------------------
 -- Total Frames                                    1000000
 -- Missing Frames                                        0
 -- Repeated Frames                                       0
-----------------------------------------------------------
```

# BUILD FROM SOURCE

```powershelll
cmake -S . -B build -G "Ninja" -D CMAKE_C_COMPIlER=clang -D CMAKE_BUILD_TYPE=Release
cmake --build build

# install with docs
cmake --install build --config Release --prefix <path/to/install>

# install executable only to <prefix>/bin/mps-info.exe
cmake --install build --config Release --prefix <path/to/install> --component Runtime

```
