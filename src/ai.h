#pragma once

#include "board.h"
#include "game.h"

#include <atomic>
#include <vector>

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

// Estimated win probabilities per player (normalized so they sum to 1).
// The game is a first-pawn-to-goal race, so each player is scored by its
// closest pawn's expected turns (exact-dice), and the share is that pawn's
// race rate (1/turns) normalized over the players. A player whose closest
// pawn is within ~3 turns gets a graded bonus toward 0.95 scaled by the gap
// to the runner-up; only an actual win is exactly 1.0. Uses goal distances
// that account for the barricades currently on the board.
std::vector<double> winChances(const Game& game);

// Greedy heuristic: picks a barricade placement that slows opponents
// down the most while hurting the current player the least.
Point naiveBarricadePlacement(const Game& game);

// Fast variant of `naiveBarricadePlacement` for the MCTS search (rollouts and
// tree descent): no BFS, uses a static cell ranking instead.
Point cheapBarricadePlacement(const Game& game, const std::vector<int>& dist);

// Weighted goal-distance map that penalizes the barricades currently on the
// board (each costs kBarricadePenalty extra, as it must be captured to pass).
// The static `goalDist` used by `naiveMove` ignores the current barricades,
// which made pawns walk into freshly placed walls.
std::vector<int> dynamicGoalDist(const Game& game);

// Ranked barricade placements (best first) for the advice UI. `score` is the
// raw gain-lose value; the caller normalizes it to a percentage.
struct BarricadeRecommendation {
    Point cell;
    double score = 0.0;
};
std::vector<BarricadeRecommendation> barricadeRecommendations(const Game& game, int topN);

// Plays `nGames` full end-games from the current position with the fast greedy
// policies (cheapMove + cheapBarricadePlacement) and returns each player's
// fraction of wins among the resolved games (so the shares sum to 1; games
// that never end because the track is deadlocked are excluded).
std::vector<double> simulateWinChances(const Game& game, long long nGames, int nThreads = 0);

// Background variant: keeps playing end-games on `nThreads` workers until
// `*stop` is set or `targetGames` have been played, adding each finished game
// to `gamesOut` and its winner to `winsOut` (an array of playerCount() atomics).
// Returns when the workers have finished; call it from a background thread.
void simulateWinChancesAsync(const Game& game, long long targetGames,
                             std::atomic<bool>* stop,
                             std::atomic<long long>* winsOut, int players,
                             std::atomic<long long>* gamesOut,
                             int nThreads = 0);

}  // namespace barricade