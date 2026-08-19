# barricade_board - Project Conventions

## Architecture
- C++17, CMake, vcpkg for dependencies
- SDL2 for rendering
- Structure: `src/` (game, board, ai), `tests/`

## Build & Run
```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
./build/x64/Release/barricade_board.exe
```

## Test
```bash
cmake --build build --target smoke_test --config Release
./build/x64/Release/smoke_test.exe
```

## Code Style
- Follow existing naming (snake_case for functions, PascalCase for classes)
- Headers in `src/*.h`, implementations in `src/*.cpp`
- No raw pointers, prefer `std::unique_ptr`/`std::shared_ptr`

## Conventions
- One responsibility per class
- Early returns preferred
- Comments only for non-obvious "why"