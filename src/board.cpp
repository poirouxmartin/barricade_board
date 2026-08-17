#include "board.h"

namespace barricade {

namespace {
const char* kLayout[14] = {
    "........G........",  // y=0: goal
    "########B########",
    "#..............#",
    "########B########",
    "........B........",
    "......##B##......",
    "......#..#......",
    "....##B###B##....",
    "....#.......#....",
    "..#############..",
    "..#...#...#...#..",
    "B###B###B###B###B",
    "#...#...#...#...#",
    "#################",  // y=13: bottom row
};
}  // namespace

bool isTrackCell(int x, int y) {
    if (x < 0 || x >= kCols || y < 0 || y >= 14) return false;
    const char c = kLayout[y][x];
    return c == '#' || c == 'B' || c == 'G';
}

bool isGoalCell(int x, int y) {
    return x == 8 && y == 0;
}

int baseOwner(int x, int y) {
    if (y < kBaseTopY || y > kBaseTopY + kPawnsPerPlayer - 1) return -1;
    for (int p = 0; p < kMaxPlayers; ++p) {
        if (x == kBaseCol[p]) return p;
    }
    return -1;
}

bool isInitialBarricadeCell(int x, int y) {
    if (x < 0 || x >= kCols || y < 0 || y >= 14) return false;
    return kLayout[y][x] == 'B';
}

Point baseFrontCell(int player) {
    return {kBaseCol[player], kBottomRowY};
}

Point baseCell(int player, int index) {
    return {kBaseCol[player], kBaseTopY + index};
}

}  // namespace barricade