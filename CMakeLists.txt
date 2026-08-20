cmake_minimum_required(VERSION 3.20)
project(barricade_board LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files are UTF-8 (accented French UI strings); without this MSVC
# reinterprets non-ASCII literals in the system codepage (mojibake in the UI).
if(MSVC)
    add_compile_options(/utf-8)
endif()

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_ttf CONFIG REQUIRED)

add_executable(barricade_board
    src/main.cpp
    src/game.h
    src/game.cpp
    src/board.h
    src/board.cpp
    src/ai.h
    src/ai.cpp
    src/nn.h
    src/nn.cpp
)

target_link_libraries(barricade_board PRIVATE SDL2::SDL2main SDL2::SDL2 SDL2_ttf::SDL2_ttf)

add_executable(smoke_test
    tests/smoke.cpp
    src/game.h
    src/game.cpp
    src/board.h
    src/board.cpp
    src/ai.h
    src/ai.cpp
    src/nn.h
    src/nn.cpp
)
target_include_directories(smoke_test PRIVATE src)
target_link_libraries(smoke_test PRIVATE)

# Neural-network training pipeline (console tools, no SDL).
add_executable(selfplay
    src/selfplay.cpp
    src/game.h
    src/game.cpp
    src/board.h
    src/board.cpp
    src/ai.h
    src/ai.cpp
    src/nn.h
    src/nn.cpp
)
target_include_directories(selfplay PRIVATE src)

add_executable(train
    src/train.cpp
    src/game.h
    src/game.cpp
    src/board.h
    src/board.cpp
    src/ai.h
    src/ai.cpp
    src/nn.h
    src/nn.cpp
)
target_include_directories(train PRIVATE src)

add_executable(bench
    src/bench.cpp
    src/game.h
    src/game.cpp
    src/board.h
    src/board.cpp
    src/ai.h
    src/ai.cpp
    src/nn.h
    src/nn.cpp
)
target_include_directories(bench PRIVATE src)