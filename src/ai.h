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

// Per-action statistics of the root-parallel MCTS search (merged across workers).
struct ActionStats {
    AIMove move;
    long long visits = 0;
    double score = 0.0;
};
std::vector<ActionStats> mctsActionStats(const Game& game, int player, int budgetMs,
                                         int nThreads = 0);

// Diagnostics from the last MCTS search: number of iterations and tree nodes.
long long mctsIterationCount();
long long mctsNodeCount();

// Diagnostics of the last completed MCTS search (AI move or advice request).
struct SearchInfo {
    long long iterations = 0;
    long long nodes = 0;
    double elapsedMs = 0.0;
    double avgDepth = 0.0;
    double winProb = 0.0;  // current player's win probability for the best move
};
SearchInfo mctsInfo();

// Static heuristic: each player's minimum distance to the goal (kInf if blocked).
std::vector<int> playerProgress(const Game& game);

// Per-pawn weighted distance to the goal for `player` (kInf if no path).
std::vector<int> playerArmyDistances(const Game& game, int player);

// Estimated win probabilities per player (normalized so they sum to 1),
// based on the whole army: the sum of per-pawn weights 1/(dist+3).
std::vector<double> winChances(const Game& game);

// Greedy heuristic: picks a barricade placement that slows opponents
// down the most while hurting the current player the least.
Point naiveBarricadePlacement(const Game& game);

// Fast variant of `naiveBarricadePlacement` for the MCTS search (rollouts and
// tree descent): no BFS, uses a static cell ranking instead.
Point cheapBarricadePlacement(const Game& game);

// Ranked barricade placements (best first) for the advice UI. `score` is the
// raw gain-lose value; the caller normalizes it to a percentage.
struct BarricadeRecommendation {
    Point cell;
    double score = 0.0;
};
std::vector<BarricadeRecommendation> barricadeRecommendations(const Game& game, int topN);

}  // namespace barricade