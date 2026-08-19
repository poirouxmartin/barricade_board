# Barricade Board

Board game implementation in C++ with SDL2 rendering and AI opponent.

## Quick Start
```bash
# Requires: vcpkg with SDL2, CMake 3.20+
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
./build/x64/Release/barricade_board.exe
```

## Test
```bash
cmake --build build --target smoke_test --config Release
./build/x64/Release/smoke_test.exe
```

## Structure
- `src/game.*` - Game loop, state machine
- `src/board.*` - Board representation, rules
- `src/ai.*` - AI opponent (minimax)
- `tests/smoke.cpp` - Basic smoke tests

## Dependencies
- SDL2 (via vcpkg)
- C++17 compiler