#pragma once

#include "board.h"
#include "game.h"

namespace barricade {

struct AIMove {
    int pawn = -1;
    Point dest{-1, -1};
};

// Greedy heuristic: picks the best (pawn, destination) for `player`.
// The caller is responsible for making sure it is the player's turn.
AIMove naiveMove(const Game& game, int player);

// Monte-Carlo Tree Search: picks the best (pawn, destination) for the current
// player within `budgetMs` milliseconds, using the heuristic as rollout policy.
AIMove mctsMove(const Game& game, int player, int budgetMs);

// Greedy heuristic: picks a barricade placement that slows opponents
// down the most while hurting the current player the least.
Point naiveBarricadePlacement(const Game& game);

}  // namespace barricade