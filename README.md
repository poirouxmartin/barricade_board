# barricade_board

Malefiz (the barricade board game) in C++20 with SDL2: four players, five pawns each, eleven
barricades, a die. The interesting part is the opponent, and the die changes the problem: there
is no best move, only a best expectation.

Project page: [martinpoiroux.com/en/projects/malefiz](https://martinpoiroux.com/en/projects/malefiz/)

## What is in it

- **Rules and state** (`src/board.*`, `src/game.*`) kept apart from rendering, so games can run
  headless for measurement.
- **Three opponents** (`src/ai.*`):
  - a direct heuristic that picks the best pawn/destination pair without searching;
  - a Monte Carlo tree search that plays games to the end and keeps what wins most often;
  - the same search guided by a small network (`src/nn.*`) that supplies move priors and a
    position value instead of random playouts.
- **Network**: the board is read as twelve planes (pawns of each player, barricades, goal
  squares, bases, side to move, die value). It trains by self-play (`src/selfplay.cpp`,
  `src/train.cpp`).
- **Search telemetry in the UI**: simulations, speed, mean depth, win chance per colour, and the
  gap between the static estimate and the simulated one. Built mainly to see when the evaluation
  is wrong.

## Build

Requires CMake 3.20+, a C++20 compiler and [vcpkg](https://vcpkg.io) (SDL2 is declared in
`vcpkg.json`).

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Targets: `barricade_board` (the game), `smoke_test`, `selfplay`, `train`, `bench`.

## Test

```bash
cmake --build build --config Release --target smoke_test
./build/Release/smoke_test
```

## License

MIT
