# NBA Tracker / J2K Vision

Windows C++/Python vision stack for Helios/Titan Two integration.

## Main components

- `cpp/` — C++17 CMake project and `j2k_ch.dll`
- `python/` — Python Helios integration, UI, and validation harnesses
- `scripts/` — Windows build/sanity/release scripts
- `.github/workflows/` — CI validation

## Quick sanity check

```bat
scripts\j2k.bat sanity
