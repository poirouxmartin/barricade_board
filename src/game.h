#pragma once

#include <array>

namespace barricade {

inline constexpr int kBoardSize = 9;
inline constexpr int kMarblesPerPlayer = 4;
inline constexpr int kMaxPlayers = 4;

struct Point {
    int x = 0;
    int y = 0;
};

inline constexpr Point kGoal = {4, 4};

// Starting zones: the 2x2 block in each corner of the board.
inline constexpr Point kStartZones[kMaxPlayers][kMarblesPerPlayer] = {
    {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
    {{7, 0}, {8, 0}, {7, 1}, {8, 1}},
    {{0, 7}, {1, 7}, {0, 8}, {1, 8}},
    {{7, 7}, {8, 7}, {7, 8}, {8, 8}},
};

class Game {
public:
    explicit Game(int playerCount);

    int playerCount() const { return player_count_; }
    const Point& marblePos(int player, int index) const {
        return marbles_[player][index];
    }

    // Resets every marble to its starting zone.
    void reset();

private:
    int player_count_;
    std::array<std::array<Point, kMarblesPerPlayer>, kMaxPlayers> marbles_;
};

}  // namespace barricade