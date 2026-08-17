#pragma once

namespace barricade {

struct Point {
    int x = 0;
    int y = 0;
};

inline bool operator==(const Point& a, const Point& b) { return a.x == b.x && a.y == b.y; }
inline bool operator!=(const Point& a, const Point& b) { return !(a == b); }

constexpr int kCols = 17;
constexpr int kRows = 19;  // y=0 top, y=13 bottom track row, y=14..18 bases
constexpr int kMaxPlayers = 4;
constexpr int kPawnsPerPlayer = 5;
constexpr int kBarricadeCount = 11;
constexpr int kBottomRowY = 13;
constexpr int kBaseTopY = 14;

constexpr int kBaseCol[kMaxPlayers] = {2, 6, 10, 14};

bool isTrackCell(int x, int y);
bool isGoalCell(int x, int y);
int baseOwner(int x, int y);  // player index or -1
bool isInitialBarricadeCell(int x, int y);
Point baseFrontCell(int player);
Point baseCell(int player, int index);

}  // namespace barricade