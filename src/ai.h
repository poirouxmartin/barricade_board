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

// Recommendation of the MCTS root: one entry per candidate action, sorted by
// estimated gain (`value` in [-1, 1], +1 = root player wins the rollout).
// Used to show the computer's advice in the UI.
struct MctsRecommendation {
    AIMove move;
    long long visits = 0;
    double value = 0.0;
};
std::vector<MctsRecommendation> mctsRecommendations(const Game& game, int player, int budgetMs);

// Per-action statistics of the shared-tree search root.
struct ActionStats {
    AIMove move;
    long long visits = 0;
    double score = 0.0;
};
std::vector<ActionStats> mctsActionStats(const Game& game, int player, int budgetMs);

// Diagnostics from the last MCTS search: number of iterations and tree nodes.
long long mctsIterationCount();
long long mctsNodeCount();

// Greedy heuristic: picks a barricade placement that slows opponents
// down the most while hurting the current player the least.
Point naiveBarricadePlacement(const Game& game);

// Fast variant of `naiveBarricadePlacement` for the MCTS search (rollouts and
// tree descent): no BFS, uses a static cell ranking instead.
Point cheapBarricadePlacement(const Game& game);

}  // namespace barricade