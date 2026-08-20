#include "ai.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace barricade {

namespace {

constexpr int kInf = 1000000;
constexpr double kUctC = 1.414;
// Rollouts run to the end of the game (like the win-chance simulation), so the
// MCTS value is the actual end-game win rate, not the short-horizon heuristic:
// a move that lowers the simulated win chances is then rarely picked.
constexpr int kRolloutSteps = 5000;
constexpr int kMaxIterations = 2000000;
// How many candidate cells a placement node branches over. The cheap scorer
// scans every free track cell, so a good mid-board block of an opponent is
// never missed (the old 24-cell prefix of the ranking only saw cells near the
// goal, which made every bot dump its barricade at the top and block itself).
constexpr int kBarricadeBranch = 8;
// How many non-capturing pawn moves a node keeps per pawn, ranked by the
// pawn's distance to its goal at the destination. Captures and winning moves
// are always kept. Pruning the clearly-wasteful moves shrinks the root from
// ~35 actions to ~10, so each action gets several times more rollouts and the
// MCTS selection is much sharper.
constexpr int kKeepMovesPerPawn = 2;
}  // namespace

// Defined below (with the placement scoring); the search and the win-chance
// estimates call it so they see the barricades currently on the board.
std::vector<Point> currentBarricades(const Game& game);

// Exact BFS-based placement score (defined below with the placement helpers);
// the cheap candidate scorer uses it to refine its shortlist.
int scoreBarricade(const Game& game, const std::vector<int>& baseDist, Point c);

// Gain-only placement used by the Monte-Carlo loops (rollouts and the
// win-chance simulation), defined below: no exact BFS refinement.
Point fastBarricadePlacement(const Game& game, const std::vector<int>& dist);

// Goal BFS base distances for the placement scores (defined below): current
// barricades except the goal-adjacent (8,1), which would make them all kInf.
const std::vector<int> baseGoalDist(const Game& game);

namespace {

int indexOf(Point p) {
    return p.y * kCols + p.x;
}

int distAt(const std::vector<int>& dist, Point p) {
    if (p.x < 0 || p.x >= kCols || p.y < 0 || p.y >= kRows) return kInf;
    return dist[indexOf(p)];
}

// BFS from the goal over the track graph, avoiding `blocked` cells.
std::vector<int> bfsFromGoal(const std::vector<Point>& blocked) {
    std::vector<int> dist(kCols * kRows, kInf);
    std::vector<char> isBlocked(kCols * kRows, 0);
    for (const Point& b : blocked) isBlocked[indexOf(b)] = 1;

    const Point goal{8, 0};
    std::vector<Point> q;
    dist[indexOf(goal)] = 0;
    q.push_back(goal);

    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (size_t i = 0; i < q.size(); ++i) {
        const Point cur = q[i];
        for (int k = 0; k < 4; ++k) {
            const Point np{cur.x + dx[k], cur.y + dy[k]};
            if (!isTrackCell(np.x, np.y)) continue;
            const int ni = indexOf(np);
            if (dist[ni] != kInf || isBlocked[ni]) continue;
            dist[ni] = dist[indexOf(cur)] + 1;
            q.push_back(np);
        }
    }
    return dist;
}

// BFS from a pawn position. A pawn in base starts one step beyond its front cell.
std::vector<int> bfsFrom(Point src) {
    if (baseOwner(src.x, src.y) >= 0) {
        src = baseFrontCell(baseOwner(src.x, src.y));
        std::vector<int> d = bfsFrom(src);
        for (int& v : d) {
            if (v != kInf) ++v;
        }
        return d;
    }
    std::vector<int> dist(kCols * kRows, kInf);
    std::vector<Point> q;
    dist[indexOf(src)] = 0;
    q.push_back(src);

    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (size_t i = 0; i < q.size(); ++i) {
        const Point cur = q[i];
        for (int k = 0; k < 4; ++k) {
            const Point np{cur.x + dx[k], cur.y + dy[k]};
            if (!isTrackCell(np.x, np.y)) continue;
            const int ni = indexOf(np);
            if (dist[ni] != kInf) continue;
            dist[ni] = dist[indexOf(cur)] + 1;
            q.push_back(np);
        }
    }
    return dist;
}

// Weighted distance to the goal. Stepping onto a fixed barricade cell costs
// kBarricadePenalty extra (landing on it to capture it takes an extra turn),
// so the AI naturally prefers barricade-light routes instead of the center
// column full of barricades.
constexpr int kBarricadePenalty = 8;

std::vector<int> buildGoalDist() {
    std::vector<int> dist(kCols * kRows, kInf);
    std::vector<char> done(kCols * kRows, 0);
    const Point goal{8, 0};
    dist[indexOf(goal)] = 0;

    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (;;) {
        int best = -1;
        int bestDist = kInf;
        for (int i = 0; i < kCols * kRows; ++i) {
            if (!done[i] && dist[i] < bestDist) {
                bestDist = dist[i];
                best = i;
            }
        }
        if (best < 0) break;
        done[best] = 1;
        const Point cur{best % kCols, best / kCols};
        for (int k = 0; k < 4; ++k) {
            const Point np{cur.x + dx[k], cur.y + dy[k]};
            if (!isTrackCell(np.x, np.y)) continue;
            const int cost = 1 + (isInitialBarricadeCell(np.x, np.y) ? kBarricadePenalty : 0);
            const int nd = bestDist + cost;
            const int ni = indexOf(np);
            if (nd < dist[ni]) dist[ni] = nd;
        }
    }
    return dist;
}

const std::vector<int>& goalDist() {
    static const std::vector<int> d = buildGoalDist();
    return d;
}

// Same weighted goal distance but accounting for the barricades currently on
// the board: a barricade cell costs kBarricadePenalty extra (it must be
// captured to pass), so the bots stop routing through freshly placed walls.
// The static `goalDist()` above only penalizes the fixed initial barricades,
// which is why pawns used to walk straight into the walls the players build.
std::vector<int> buildGoalDistDynamic(const std::vector<Point>& blocked) {
    std::vector<char> isBlocked(kCols * kRows, 0);
    for (const Point& b : blocked) isBlocked[indexOf(b)] = 1;

    std::vector<int> dist(kCols * kRows, kInf);
    std::vector<char> done(kCols * kRows, 0);
    const Point goal{8, 0};
    dist[indexOf(goal)] = 0;

    const int dx[4] = {1, -1, 0, 0};
    const int dy[4] = {0, 0, 1, -1};
    for (;;) {
        int best = -1;
        int bestDist = kInf;
        for (int i = 0; i < kCols * kRows; ++i) {
            if (!done[i] && dist[i] < bestDist) {
                bestDist = dist[i];
                best = i;
            }
        }
        if (best < 0) break;
        done[best] = 1;
        const Point cur{best % kCols, best / kCols};
        for (int k = 0; k < 4; ++k) {
            const Point np{cur.x + dx[k], cur.y + dy[k]};
            if (!isTrackCell(np.x, np.y)) continue;
            const int ni = indexOf(np);
            const int cost = 1 + ((isInitialBarricadeCell(np.x, np.y) || isBlocked[ni])
                                      ? kBarricadePenalty
                                      : 0);
            const int nd = bestDist + cost;
            if (nd < dist[ni]) dist[ni] = nd;
        }
    }
    return dist;
}

// Unweighted BFS distance to the goal (no barricade penalty): the exact number
// of steps needed to reach the goal, used to detect a pawn that can win this
// turn.
const std::vector<int>& rawGoalDist() {
    static const std::vector<int> d = bfsFromGoal({});
    return d;
}

int pawnDistToGoal(const std::vector<int>& dist, const Game& g, int player, int pawn) {
    if (g.pawnInBase(player, pawn)) {
        const int d = distAt(dist, baseFrontCell(player));
        return d == kInf ? kInf : d + 1;
    }
    return distAt(dist, g.pawnPos(player, pawn));
}

int progress(const Game& g, int player, const std::vector<int>& dist) {
    int best = kInf;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        best = std::min(best, pawnDistToGoal(dist, g, player, m));
    }
    return best;
}

double heuristicEval(const Game& g, int rootPlayer, const std::vector<int>& dist) {
    if (g.isOver()) return g.winner() == rootPlayer ? 1.0 : -1.0;

    const auto& raw = rawGoalDist();
    int myD[kPawnsPerPlayer];
    int oppD[kMaxPlayers * kPawnsPerPlayer];
    int nOpp = 0;
    int myMin = kInf;
    int oppMin = kInf;
    int myOne = kInf;  // exact number of steps to the goal (unweighted BFS)
    int oppOne = kInf;
    double mySum = 0.0;
    double oppSum = 0.0;
    int myOut = 0;
    int oppOut = 0;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        const int d = pawnDistToGoal(dist, g, rootPlayer, m);
        myD[m] = d;
        if (d != kInf) {
            mySum += 1.0 / (d + 1.0);
            myMin = std::min(myMin, d);
        }
        myOne = std::min(myOne, pawnDistToGoal(raw, g, rootPlayer, m));
        if (!g.pawnInBase(rootPlayer, m)) ++myOut;
    }
    for (int p = 0; p < g.playerCount(); ++p) {
        if (p == rootPlayer) continue;
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            const int d = pawnDistToGoal(dist, g, p, m);
            oppD[nOpp] = d;
            ++nOpp;
            if (d != kInf) {
                oppSum += 1.0 / (d + 1.0);
                oppMin = std::min(oppMin, d);
            }
            oppOne = std::min(oppOne, pawnDistToGoal(raw, g, p, m));
            if (!g.pawnInBase(p, m)) ++oppOut;
        }
    }
    if (myMin == kInf) myMin = 30;
    if (oppMin == kInf) oppMin = 30;

    // A pawn within a single die roll of the goal (raw distance: the goal can
    // never be barricaded) is a near-win; the linear terms alone would only
    // score it as a small lead. Give it a large bonus that grows as it gets
    // closer. Only an actual win (isOver above) is worth exactly 100%.
    const int kWinRange = 6;
    if (myOne <= kWinRange && myOne < oppOne) {
        return 0.98 - 0.04 * myOne;  // 1 step -> 0.94, 6 steps -> 0.74
    }
    if (oppOne <= kWinRange && oppOne < myOne) {
        return -(0.98 - 0.04 * oppOne);
    }

    // Capture adjacency: pawns racing within ~1 step of goal-distance of each
    // other can eat one another (all move towards the same goal). Weighing it
    // slightly rewards threatening the opponent and penalizes being exposed.
    int myThreat = 0;
    int myRisk = 0;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        if (myD[m] == kInf) continue;
        for (int i = 0; i < nOpp; ++i) {
            if (oppD[i] == kInf) continue;
            if (std::abs(myD[m] - oppD[i]) <= 1) {
                ++myThreat;
                ++myRisk;
            }
        }
    }

    // Closest-pawn race dominates; the whole army (sum of 1/(dist+1)) and the
    // pawns out of base add refinement, and capture adjacency a small nudge.
    const double oppAvg = (g.playerCount() > 1) ? oppSum / (g.playerCount() - 1) : 0.0;
    double v = (oppMin - myMin) / 10.0 + (mySum - oppAvg) * 1.2 +
               (myOut - oppOut) * 0.02 + (myThreat - myRisk) * 0.01;
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    return v;
}

// Greedy walk used by the rollout: moves a pawn `g.dice()` steps towards the
// goal, respecting the move rules (barricades block intermediate steps, the
// final landing may capture). Returns {-1,-1} if the pawn cannot complete all
// steps greedily. Equal scores are broken randomly so the rollout does not
// systematically favor one side of the board (the static neighbor table is
// ordered right-first, which would bias a fixed tie-break).
Point greedyWalkDest(const Game& g, int player, int pawn, std::mt19937& rng,
                     const std::vector<int>& dist) {
    const int dice = g.dice();
    Point cur = g.pawnPos(player, pawn);
    Point prev = cur;
    bool inBase = g.pawnInBase(player, pawn);
    const auto& nb = neighbors();
    for (int step = 0; step < dice; ++step) {
        const bool last = step == dice - 1;
        int n = 0;
        Point cands[4];
        if (inBase) {
            const Point front = baseFrontCell(player);
            if (last) {
                const int pid = g.pawnAt(front);
                if (pid >= 0 && pid / kPawnsPerPlayer == player) return {-1, -1};
            }
            cands[n++] = front;
            inBase = false;
        } else {
            const Neighbors& c = nb[cur.x][cur.y];
            for (int i = 0; i < c.count; ++i) {
                const Point np = c.cells[i];
                if (np.x == prev.x && np.y == prev.y) continue;
                if (!last && g.barricadeAt(np)) continue;
                if (last) {
                    if (baseOwner(np.x, np.y) != -1) continue;
                    const int pid = g.pawnAt(np);
                    if (pid >= 0 && pid / kPawnsPerPlayer == player) continue;
                }
                cands[n++] = np;
            }
        }
        if (n == 0) return {-1, -1};
        int best = 0;
        int bestScore = kInf;
        int ties[4];
        int tieCount = 0;
        for (int i = 0; i < n; ++i) {
            const Point np = cands[i];
            int score = distAt(dist, np);
            if (last) {
                if (g.pawnAt(np) >= 0) score -= 10000;     // capture an opponent pawn
                else if (g.barricadeAt(np)) score -= 5000;  // capture a barricade
            }
            if (score < bestScore) {
                bestScore = score;
                best = i;
                tieCount = 0;
                ties[tieCount++] = i;
            } else if (score == bestScore) {
                ties[tieCount++] = i;
            }
        }
        if (tieCount > 1) best = ties[rng() % tieCount];
        prev = cur;
        cur = cands[best];
    }
    return cur;
}

// Minimal exact fallback of `cheapMove` (one DFS per pawn, no scoring): used
// when the greedy walk finds no legal move for any pawn even though one exists.
AIMove cheapMoveFallback(const Game& g, int player, std::mt19937& rng) {
    int order[5] = {0, 1, 2, 3, 4};
    for (int i = 4; i > 0; --i) {
        std::swap(order[i], order[rng() % (i + 1)]);
    }
    Point dests[512];
    char seen[kCols * kRows];
    for (int k = 0; k < kPawnsPerPlayer; ++k) {
        const int m = order[k];
        std::memset(seen, 0, sizeof seen);
        // maxOut=1 stops the DFS at the first legal landing cell.
        const int n = g.legalDestinationsTo(player, m, dests, 1, seen);
        if (n > 0) return {m, dests[0]};
    }
    return AIMove{};
}

// Cheap rollout move: picks a random pawn and walks it greedily to the goal.
// Falls back to an exact search when the walk finds no move for any pawn.
AIMove cheapMove(const Game& g, int player, std::mt19937& rng,
                 const std::vector<int>& dist) {
    if (g.dice() <= 0) return {};
    // A pawn that can land exactly on the goal this turn must always take the
    // win; otherwise the random pawn order would waste the turn on another pawn
    // and hand the game back to the opponents.
    const auto& gd = rawGoalDist();
    bool nearGoal = false;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        const int d = pawnDistToGoal(gd, g, player, m);
        if (d != kInf && d <= g.dice()) {
            nearGoal = true;
            break;
        }
    }
    if (nearGoal) {
        Point dests[512];
        char seen[kCols * kRows];
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            const int d = pawnDistToGoal(gd, g, player, m);
            if (d == kInf || d > g.dice()) continue;
            std::memset(seen, 0, sizeof seen);
            const int n = g.legalDestinationsTo(player, m, dests, 512, seen);
            for (int i = 0; i < n; ++i) {
                if (isGoalCell(dests[i].x, dests[i].y)) return {m, dests[i]};
            }
        }
    }
    int order[5] = {0, 1, 2, 3, 4};
    for (int i = 4; i > 0; --i) {
        std::swap(order[i], order[rng() % (i + 1)]);
    }
    for (int k = 0; k < kPawnsPerPlayer; ++k) {
        const Point d = greedyWalkDest(g, player, order[k], rng, dist);
        if (d.x >= 0) return {order[k], d};
    }
    return cheapMoveFallback(g, player, rng);
}

// Simulates a game from `g0` to the end (or horizon) using the heuristic.
double rollout(const Game& g0, int rootPlayer, std::mt19937& rng,
               const std::vector<int>& dist) {
    Game g = g0;
    for (int step = 0; step < kRolloutSteps && !g.isOver(); ++step) {
        if (g.pendingBarricade()) {
            g.placeBarricadeFast(fastBarricadePlacement(g, dist));
        } else if (g.dice() == 0) {
            g.startTurn();
        } else {
            const AIMove mv = cheapMove(g, g.currentPlayer(), rng, dist);
            if (mv.pawn >= 0) {
                g.movePawnFast(g.currentPlayer(), mv.pawn, mv.dest);
            } else {
                g.nextTurn();  // no legal move: skipped
            }
        }
    }
    if (g.isOver()) return g.winner() == rootPlayer ? 1.0 : -1.0;
    return heuristicEval(g, rootPlayer, dist);
}

// ---------------------------------------------------------------------------
// Root-parallel MCTS. Each worker builds its OWN tree (no shared memory, no
// virtual loss, no atomics), so workers scale almost linearly; the root
// children statistics are merged once the threads have joined.
// ---------------------------------------------------------------------------

struct TreeNode;
struct TreeLink;

// Defined below with the placement scoring; used by the tree descent.
std::vector<Point> cheapBarricadeCandidates(const Game& game, int K,
                                            const std::vector<int>& dist);

struct TreeNode {
    explicit TreeNode(Game&& g) : game(std::move(g)) {}
    Game game;
    int player = 0;
    long long visits = 0;
    double score = 0.0;
    bool expanded = false;
    size_t nextAction = 0;
    std::vector<AIMove> actions;
    TreeLink* children = nullptr;
};

struct TreeLink {
    AIMove action;
    TreeNode* node;
    TreeLink* next;
};

// Nodes/links a worker allocated; kept alive until the worker is joined.
// They come from per-thread arenas (big aligned blocks) so workers don't
// contend on malloc.
struct Worker {
    static constexpr size_t kNodeBlock = 1024;
    struct alignas(TreeNode) NodeBlock {
        char data[kNodeBlock * sizeof(TreeNode)];
    };
    static constexpr size_t kLinkBlock = 8192;
    struct alignas(TreeLink) LinkBlock {
        char data[kLinkBlock * sizeof(TreeLink)];
    };

    TreeNode* allocNode(Game&& g) {
        if (nodesLeft == 0) {
            nodeBlocks.emplace_back(std::make_unique<NodeBlock>());
            nodeCursor = reinterpret_cast<TreeNode*>(nodeBlocks.back().get());
            nodesLeft = kNodeBlock;
        }
        TreeNode* p = nodeCursor++;
        --nodesLeft;
        ++nodeConstructed;
        new (p) TreeNode(std::move(g));
        return p;
    }

    TreeLink* allocLink(AIMove mv, TreeNode* node, TreeLink* next) {
        if (linksLeft == 0) {
            linkBlocks.emplace_back(std::make_unique<LinkBlock>());
            linkCursor = reinterpret_cast<TreeLink*>(linkBlocks.back().get());
            linksLeft = kLinkBlock;
        }
        TreeLink* p = linkCursor++;
        --linksLeft;
        ++linkConstructed;
        p->action = mv;
        p->node = node;
        p->next = next;
        return p;
    }

    ~Worker() {
        size_t full = nodeConstructed / kNodeBlock;
        size_t rem = nodeConstructed % kNodeBlock;
        for (size_t i = 0; i < full; ++i) {
            TreeNode* base = reinterpret_cast<TreeNode*>(nodeBlocks[i].get());
            for (size_t j = 0; j < kNodeBlock; ++j) base[j].~TreeNode();
        }
        if (rem != 0) {
            TreeNode* base = reinterpret_cast<TreeNode*>(nodeBlocks[full].get());
            for (size_t j = 0; j < rem; ++j) base[j].~TreeNode();
        }
    }

    std::vector<std::unique_ptr<NodeBlock>> nodeBlocks;
    std::vector<std::unique_ptr<LinkBlock>> linkBlocks;
    TreeNode* nodeCursor = nullptr;
    TreeLink* linkCursor = nullptr;
    size_t nodesLeft = 0;
    size_t linksLeft = 0;
    size_t nodeConstructed = 0;
    size_t linkConstructed = 0;
    long long iterations = 0;
    long long depthSum = 0;
};

std::atomic<long long> g_nodeCount{0};
std::atomic<long long> g_iterations{0};
std::atomic<long long> g_elapsedUs{0};
std::atomic<double> g_avgDepth{0.0};
std::atomic<double> g_winProb{0.0};

// Builds the node's action list exactly once (the tree is private, no lock).
// A node with a pending barricade is a placement node: its actions are the
// best candidate cells for the captured barricade (pawn == -1 marks a
// placement). Otherwise the actions are the pawn moves.
void expandNode(TreeNode* n, const std::vector<int>& dist) {
    if (n->game.pendingBarricade()) {
        for (const Point& c : cheapBarricadeCandidates(n->game, kBarricadeBranch, dist)) {
            n->actions.push_back({-1, c});
        }
    } else {
        Point dests[512];
        char seen[kCols * kRows];
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            std::memset(seen, 0, sizeof seen);
            const int cnt = n->game.legalDestinationsTo(n->player, m, dests, 512, seen);
            if (cnt == 0) continue;
            // Captures and wins are always worth exploring. The other moves are
            // ranked by the distance to the goal at their destination and only
            // the best few are kept, so a wasted side step cannot eat rollouts
            // that should go to the plausible moves.
            std::vector<Point> kept;
            kept.reserve(cnt);
            std::vector<std::pair<int, Point>> ordinary;
            ordinary.reserve(cnt);
            for (int i = 0; i < cnt; ++i) {
                if (isGoalCell(dests[i].x, dests[i].y) || n->game.barricadeAt(dests[i])) {
                    kept.push_back(dests[i]);
                } else {
                    ordinary.push_back({distAt(dist, dests[i]), dests[i]});
                }
            }
            std::stable_sort(ordinary.begin(), ordinary.end(),
                             [](const std::pair<int, Point>& a, const std::pair<int, Point>& b) {
                                 return a.first < b.first;
                             });
            const size_t take = std::min(ordinary.size(), static_cast<size_t>(kKeepMovesPerPawn));
            for (size_t i = 0; i < take; ++i) kept.push_back(ordinary[i].second);
            for (const Point& d : kept) n->actions.push_back({m, d});
        }
    }
    if (n->actions.empty()) n->actions.push_back(AIMove{});  // skip pseudo-action
    n->expanded = true;
}

TreeNode* createChild(TreeNode* n, const AIMove& mv, std::mt19937& rng, Worker& w) {
    Game g = n->game;
    if (mv.pawn >= 0) {
        // Pawn move. A captured barricade is left pending on purpose: the child
        // becomes a placement node so the search chooses where to put it.
        if (!g.movePawn(n->player, mv.pawn, mv.dest)) return nullptr;
    } else if (mv.dest.x >= 0) {
        // Placement action (pawn == -1, a real cell): put the barricade down.
        if (!g.placeBarricadeFast(mv.dest)) return nullptr;
    } else {
        g.nextTurn();  // skip pseudo-action: no legal move
    }
    if (!g.pendingBarricade()) g.forceDice(1 + rng() % 6);

    TreeNode* child = w.allocNode(std::move(g));
    child->player = child->game.currentPlayer();
    return child;
}

void publish(TreeNode* n, TreeNode* child, const AIMove& mv, Worker& w) {
    TreeLink* link = w.allocLink(mv, child, n->children);
    n->children = link;
}

TreeNode* uctSelect(TreeNode* n) {
    const long long nv = n->visits;
    const double base = kUctC * std::sqrt(std::log(static_cast<double>(nv) + 1.0));
    TreeNode* best = nullptr;
    double bestUct = -1.0;
    for (TreeLink* l = n->children; l; l = l->next) {
        TreeNode* c = l->node;
        if (!c) continue;
        const long long cv = c->visits;
        if (cv == 0) return c;  // visit every child once before exploiting
        const double uct = c->score / cv + base / std::sqrt(static_cast<double>(cv));
        if (uct > bestUct) {
            bestUct = uct;
            best = c;
        }
    }
    return best;
}

// One worker's search on its private tree, until the shared deadline.
void treeSearch(TreeNode* root, int player,
                std::chrono::steady_clock::time_point start, int budgetMs, Worker& w,
                const std::vector<int>& dist) {
    std::mt19937 rng(std::random_device{}());
    const auto deadline = start + std::chrono::milliseconds(budgetMs);
    std::vector<TreeNode*> path;
    long long iter = 0;
    while (iter < kMaxIterations) {
        if ((iter & 63) == 0 && std::chrono::steady_clock::now() > deadline) break;
        path.clear();

        TreeNode* n = root;
        while (n->expanded && n->nextAction >= n->actions.size()) {
            TreeNode* c = uctSelect(n);
            if (!c) break;
            n = c;
            path.push_back(n);
        }
        if (!n->expanded) expandNode(n, dist);

        double v;
        if (n->nextAction < n->actions.size()) {
            const size_t idx = n->nextAction++;
            TreeNode* child = createChild(n, n->actions[idx], rng, w);
            if (!child) {  // movePawn rejected a supposedly legal move
                ++iter;
                continue;
            }
            publish(n, child, n->actions[idx], w);
            v = rollout(child->game, player, rng, dist);
            child->visits = 1;
            child->score = v;
            for (TreeNode* p : path) {
                p->visits++;
                p->score += v;
            }
            w.depthSum += path.size() + 1;  // depth of the new leaf
        } else {
            v = heuristicEval(n->game, player, dist);
            for (TreeNode* p : path) {
                p->visits++;
                p->score += v;
            }
        }
        root->visits++;
        root->score += v;
        ++iter;
    }
    w.iterations = iter;
}

}  // namespace

AIMove naiveMove(const Game& game, int player) {
    std::vector<std::vector<int>> opDists;
    for (int op = 0; op < game.playerCount(); ++op) {
        if (op == player) continue;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            opDists.push_back(bfsFrom(game.pawnPos(op, om)));
        }
    }

    AIMove best;
    int bestScore = -kInf;
    Point dests[512];
    char seen[kCols * kRows];
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        std::memset(seen, 0, sizeof seen);
        const int n = game.legalDestinationsTo(player, m, dests, 512, seen);
        const int curD = pawnDistToGoal(goalDist(), game, player, m);
        if (curD == kInf) continue;
        for (int i = 0; i < n; ++i) {
            const Point dest = dests[i];
            int score = (curD - distAt(goalDist(), dest)) * 20;
            if (isGoalCell(dest.x, dest.y)) score += 1000000;
            const int victim = game.pawnAt(dest);
            if (victim >= 0) score += 1500;  // captures an opponent pawn
            if (game.barricadeAt(dest)) score += 200;
            int risk = 0;
            for (size_t j = 0; j < opDists.size(); ++j) {
                const int dd = opDists[j][indexOf(dest)];
                if (dd > 0 && dd <= 6) ++risk;
            }
            score -= risk * 250;
            if (score > bestScore) {
                bestScore = score;
                best = {m, dest};
            }
        }
    }
    return best;
}

namespace {

AIMove pickBest(const std::vector<ActionStats>& stats, const Game& game, int player) {
    if (stats.empty()) return naiveMove(game, player);
    long long bestVisits = -1;
    double bestAvg = -2.0;
    AIMove best;
    for (const ActionStats& s : stats) {
        const double avg = s.visits > 0 ? s.score / s.visits : -2.0;
        if (s.visits > bestVisits || (s.visits == bestVisits && avg > bestAvg)) {
            bestVisits = s.visits;
            bestAvg = avg;
            best = s.move;
        }
    }
    return best;
}

}  // namespace

// Root-parallel MCTS: every worker searches its own tree from the same root
// position; the root children statistics are summed across workers.
std::vector<ActionStats> mctsActionStats(const Game& game, int player, int budgetMs,
                                         int nThreads) {
    if (game.dice() <= 0 || game.isOver()) return {};

    // Goal distances that penalize the barricades currently on the board, so
    // the rollouts stop routing pawns through the walls the players build.
    const auto dist = buildGoalDistDynamic(currentBarricades(game));

    if (nThreads < 1) nThreads = std::thread::hardware_concurrency();
    if (nThreads < 1) nThreads = 1;
    if (nThreads > 16) nThreads = 16;

    g_iterations.store(0, std::memory_order_relaxed);
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::unique_ptr<Worker>> workers(nThreads);
    std::vector<TreeNode*> roots(nThreads);
    for (int t = 0; t < nThreads; ++t) {
        workers[t] = std::make_unique<Worker>();
        roots[t] = workers[t]->allocNode(Game(game));
        roots[t]->player = roots[t]->game.currentPlayer();
    }

    if (nThreads == 1) {
        treeSearch(roots[0], player, start, budgetMs, *workers[0], dist);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(nThreads);
        for (int t = 0; t < nThreads; ++t) {
            threads.emplace_back(
                [&, t] { treeSearch(roots[t], player, start, budgetMs, *workers[t], dist); });
        }
        for (std::thread& th : threads) th.join();
    }

    long long totalIter = 0;
    long long totalNodes = 0;
    long long depthSum = 0;
    for (int t = 0; t < nThreads; ++t) {
        totalIter += workers[t]->iterations;
        totalNodes += workers[t]->nodeConstructed;
        depthSum += workers[t]->depthSum;
    }
    g_iterations.store(totalIter, std::memory_order_relaxed);
    g_nodeCount.store(totalNodes + nThreads, std::memory_order_relaxed);  // + roots
    g_elapsedUs.store(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count(),
                      std::memory_order_relaxed);
    g_avgDepth.store(totalIter > 0 ? static_cast<double>(depthSum) / totalIter : 0.0,
                     std::memory_order_relaxed);

    // Merge the root children statistics across all workers.
    std::vector<ActionStats> out;
    for (int t = 0; t < nThreads; ++t) {
        for (TreeLink* l = roots[t]->children; l; l = l->next) {
            if (!l->node || l->node->visits <= 0) continue;
            ActionStats* slot = nullptr;
            for (ActionStats& s : out) {
                if (s.move.pawn == l->action.pawn && s.move.dest.x == l->action.dest.x &&
                    s.move.dest.y == l->action.dest.y) {
                    slot = &s;
                    break;
                }
            }
            if (!slot) {
                out.push_back({l->action, 0, 0.0});
                slot = &out.back();
            }
            slot->visits += l->node->visits;
            slot->score += l->node->score;
        }
    }

    // Best move = the one with the most visits; its value estimates the
    // current player's win probability.
    double bestProb = 0.5;
    long long bestVisits = 0;
    for (const ActionStats& s : out) {
        if (s.visits > bestVisits) {
            bestVisits = s.visits;
            bestProb = (1.0 + s.score / s.visits) / 2.0;
        }
    }
    if (bestProb < 0.0) bestProb = 0.0;
    if (bestProb > 1.0) bestProb = 1.0;
    g_winProb.store(bestProb, std::memory_order_relaxed);

    return out;
}

AIMove mctsMove(const Game& game, int player, int budgetMs) {
    return pickBest(mctsActionStats(game, player, budgetMs), game, player);
}

// Diagnostics for tuning: iterations and tree nodes of the last
// `mctsActionStats` call.
long long mctsIterationCount() { return g_iterations.load(std::memory_order_relaxed); }
long long mctsNodeCount() { return g_nodeCount.load(std::memory_order_relaxed); }

SearchInfo mctsInfo() {
    return {g_iterations.load(std::memory_order_relaxed),
            g_nodeCount.load(std::memory_order_relaxed),
            static_cast<double>(g_elapsedUs.load(std::memory_order_relaxed)) / 1000.0,
            g_avgDepth.load(std::memory_order_relaxed),
            g_winProb.load(std::memory_order_relaxed)};
}

// Each player's minimum distance to the goal; pawns with no reachable path
// report kInf.
std::vector<int> playerProgress(const Game& game) {
    const auto dist = buildGoalDistDynamic(currentBarricades(game));
    std::vector<int> out(game.playerCount());
    for (int p = 0; p < game.playerCount(); ++p) out[p] = progress(game, p, dist);
    return out;
}

// Per-pawn weighted distance to the goal for `player`.
std::vector<int> playerArmyDistances(const Game& game, int player) {
    const auto dist = buildGoalDistDynamic(currentBarricades(game));
    std::vector<int> out(kPawnsPerPlayer);
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        out[m] = pawnDistToGoal(dist, game, player, m);
    }
    return out;
}

// Estimated win probabilities per player from the whole army: each pawn on
// the track contributes 1/(dist+2), so advancing and having more pawns out of
// the base improves the estimate (the curve is slightly sharper than the old
// 1/(dist+3) to reward end-game proximity). Normalized so the shares sum to 1.
// A player one exact step from the goal (and nobody else that close) is
// resolved as a sure win, since the curve alone would only read ~40% for it.
std::vector<double> winChances(const Game& game) {
    const auto& raw = rawGoalDist();
    int leader = -1;
    for (int p = 0; p < game.playerCount(); ++p) {
        int best = kInf;
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            best = std::min(best, pawnDistToGoal(raw, game, p, m));
        }
        if (best == 1) {
            if (leader >= 0) leader = -2;  // two players one step away: fall through
            else leader = p;
        }
    }
    if (leader >= 0) {
        std::vector<double> out(game.playerCount(), 0.0);
        out[leader] = 0.95;  // big bonus for the near-win, not a sure 100%
        const double rest = 0.05 / (game.playerCount() - 1);
        for (int p = 0; p < game.playerCount(); ++p) {
            if (p != leader) out[p] = rest;
        }
        return out;
    }

    std::vector<double> out(game.playerCount());
    const auto dist = buildGoalDistDynamic(currentBarricades(game));
    for (int p = 0; p < game.playerCount(); ++p) {
        double s = 0.0;
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            int d = pawnDistToGoal(dist, game, p, m);
            if (d > 24) d = 24;
            s += 1.0 / (d + 2.0);
        }
        out[p] = s;
    }
    double sum = 0.0;
    for (double v : out) sum += v;
    for (double& v : out) v /= sum;
    return out;
}

std::vector<MctsRecommendation> mctsRecommendations(const Game& game, int player, int budgetMs) {
    std::vector<MctsRecommendation> out;
    for (const ActionStats& s : mctsActionStats(game, player, budgetMs)) {
        // Drop only the skip pseudo-action; a placement action (pawn == -1 with
        // a real cell) is a legitimate recommendation.
        if ((s.move.pawn < 0 && s.move.dest.x < 0) || s.visits == 0) continue;
        out.push_back({s.move, s.visits, s.score / s.visits});
    }
    std::sort(out.begin(), out.end(),
              [](const MctsRecommendation& a, const MctsRecommendation& b) {
                  if (a.value != b.value) return a.value > b.value;
                  return a.visits > b.visits;
              });
    return out;
}

// Static data for the cheap placement: forward-neighbor count and a ranking
// of candidate cells (choke points first, then by distance to the goal).
struct PlacementTables {
    std::array<char, kCols * kRows> fwd{};
    std::vector<Point> rank;
};

const PlacementTables& placementTables() {
    static const PlacementTables t = [] {
        PlacementTables t;
        for (int y = 0; y <= kBottomRowY; ++y) {
            for (int x = 0; x < kCols; ++x) {
                if (!isTrackCell(x, y) || y == kBottomRowY || isGoalCell(x, y)) continue;
                const Point p{x, y};
                const int cg = distAt(goalDist(), p);
                if (cg == kInf) continue;
                int f = 0;
                const Neighbors& nb = neighbors()[x][y];
                for (int i = 0; i < nb.count; ++i) {
                    if (distAt(goalDist(), nb.cells[i]) < cg) ++f;
                }
                t.fwd[indexOf(p)] = static_cast<char>(f);
                t.rank.push_back(p);
            }
        }
        std::stable_sort(t.rank.begin(), t.rank.end(), [&t](const Point& a, const Point& b) {
            const int fa = t.fwd[indexOf(a)];
            const int fb = t.fwd[indexOf(b)];
            if ((fa == 1) != (fb == 1)) return fa == 1;  // choke points first
            return distAt(goalDist(), a) < distAt(goalDist(), b);
        });
        return t;
    }();
    return t;
}

// Very cheap barricade placement for the search (rollouts and tree descent).
// Uses the static ranking and static goal distances, so it never runs a BFS.
// The score mirrors `naiveBarricadePlacement`: a choke point (`fwd == 1`)
// traps every pawn behind it (gain 40 / loss 1000), anything else just makes
// pawns detour (gain/loss 2). Internal helper (anonymous namespace) so the
// tree descent can call it through the forward declaration above.
namespace {

// Gain-only prefilter shared by the exact candidate list and the fast rollout
// placement: ranks every free track cell by how many opponents it slows down
// (choke points and cells near the goal weighted more). It deliberately ignores
// our own pawns: the distance model cannot tell a block on the opponent's side
// from a block on our side, so penalizing our own pawns here could drop the
// clean block out of the shortlist before the exact scorer sees it.
struct GainCand {
    Point c;
    int gain;
};

std::vector<GainCand> gainBarricadeCandidates(const Game& game,
                                              const std::vector<int>& dist) {
    const PlacementTables& pt = placementTables();

    int opDist[kMaxPlayers * kPawnsPerPlayer];
    int nOp = 0;
    for (int op = 0; op < game.playerCount(); ++op) {
        if (op == game.currentPlayer()) continue;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            opDist[nOp++] = pawnDistToGoal(dist, game, op, om);
        }
    }

    std::vector<GainCand> out;
    out.reserve(160);
    for (const Point& c : pt.rank) {
        if (game.barricadeAt(c) || game.pawnAt(c) != -1) continue;
        const int cg = distAt(dist, c);
        if (cg == kInf) continue;

        const int fwd = pt.fwd[indexOf(c)];
        const int oppImpact = (fwd == 1) ? 50 : 4;

        int gain = 0;
        for (int i = 0; i < nOp; ++i) {
            if (opDist[i] != kInf && cg < opDist[i]) {
                gain += oppImpact * (opDist[i] <= 8 ? 2 : 1);
            }
        }
        out.push_back({c, gain});
    }
    return out;
}

std::vector<Point> cheapBarricadeCandidates(const Game& game, int K,
                                            const std::vector<int>& dist) {
    std::vector<GainCand> cands = gainBarricadeCandidates(game, dist);
    std::sort(cands.begin(), cands.end(),
              [](const GainCand& a, const GainCand& b) { return a.gain > b.gain; });
    if (cands.size() > 16) cands.resize(16);
    // Refine the shortlist with the exact BFS scorer: it measures the real
    // detour (or trap) for every pawn, so a clean block of the opponents wins
    // without penalizing our own route.
    const auto baseDist = baseGoalDist(game);
    for (GainCand& cd : cands) cd.gain = scoreBarricade(game, baseDist, cd.c);
    std::sort(cands.begin(), cands.end(),
              [](const GainCand& a, const GainCand& b) { return a.gain > b.gain; });
    std::vector<Point> out;
    for (size_t i = 0; i < cands.size() && out.size() < static_cast<size_t>(K); ++i) {
        out.push_back(cands[i].c);
    }
    return out;
}

}  // namespace

// Fast placement for the Monte-Carlo loops: the gain prefilter alone, no exact
// BFS refinement. Every capture triggers a placement, so the 16 refinements of
// the exact path would dominate their cost, and a slightly cruder placement
// policy is fine for a sample.
Point fastBarricadePlacement(const Game& game, const std::vector<int>& dist) {
    std::vector<GainCand> cands = gainBarricadeCandidates(game, dist);
    Point best{0, 0};
    int bestGain = 0;
    for (const GainCand& cd : cands) {
        if (cd.gain > bestGain) {
            bestGain = cd.gain;
            best = cd.c;
        }
    }
    if (bestGain > 0) return best;
    // Very crowded board: nothing scored, return the first legal cell.
    const PlacementTables& pt = placementTables();
    for (const Point& c : pt.rank) {
        if (!game.barricadeAt(c) && game.pawnAt(c) == -1) return c;
    }
    return {0, 0};
}

Point cheapBarricadePlacement(const Game& game, const std::vector<int>& dist) {
    const std::vector<Point> cands = cheapBarricadeCandidates(game, 1, dist);
    if (!cands.empty()) return cands[0];
    // Very crowded board: nothing scored, return the first legal cell.
    const PlacementTables& pt = placementTables();
    for (const Point& c : pt.rank) {
        if (!game.barricadeAt(c) && game.pawnAt(c) == -1) return c;
    }
    return {0, 0};
}

std::vector<int> dynamicGoalDist(const Game& game) {
    return buildGoalDistDynamic(currentBarricades(game));
}

std::vector<Point> currentBarricades(const Game& game) {
    std::vector<Point> blocks;
    for (const Point& b : game.barricades()) {
        if (b.x >= 0) blocks.push_back(b);
    }
    return blocks;
}

// Goal BFS distances with the current barricades, except the goal-adjacent
// one (8,1): it sits on the only path to the goal, so as a wall it would make
// every distance kInf (see scoreBarricade). Used as the base for placement
// scoring.
const std::vector<int> baseGoalDist(const Game& game) {
    std::vector<Point> blocked;
    for (const Point& b : currentBarricades(game)) {
        if (b.x == 8 && b.y == 1) continue;
        blocked.push_back(b);
    }
    return bfsFromGoal(blocked);
}

// Scores a candidate barricade placement: how much it slows down every
// opponent pawn (weighted more when the pawn is close to the goal) minus how
// much it slows our own pawns. `baseDist` is the goal BFS over the board with
// the barricades currently on it (see baseGoalDist). The goal-adjacent
// barricade (8,1) sits on the only approach to the goal: treating it as an
// impassable wall would make every distance kInf and every score 0, so it is
// kept out of the blocked set (as if already captured, which is what must
// happen before anyone can win).
int scoreBarricade(const Game& game, const std::vector<int>& baseDist, Point c) {
    const std::vector<Point>& cur = currentBarricades(game);
    std::vector<Point> blocked;
    blocked.reserve(cur.size() + 1);
    for (const Point& b : cur) {
        if (b.x == 8 && b.y == 1) continue;
        blocked.push_back(b);
    }
    blocked.push_back(c);
    const auto bd = bfsFromGoal(blocked);

    int gain = 0;
    for (int op = 0; op < game.playerCount(); ++op) {
        if (op == game.currentPlayer()) continue;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            const int a = pawnDistToGoal(baseDist, game, op, om);
            if (a == kInf) continue;
            const int b = pawnDistToGoal(bd, game, op, om);
            gain += (b == kInf) ? 60 : (b - a) * (a <= 8 ? 2 : 1);
        }
    }
    int lose = 0;
    for (int om = 0; om < kPawnsPerPlayer; ++om) {
        const int a = pawnDistToGoal(baseDist, game, game.currentPlayer(), om);
        if (a == kInf) continue;
        const int b = pawnDistToGoal(bd, game, game.currentPlayer(), om);
        lose += (b == kInf) ? 1000 : (b - a) * 2;
    }
    return gain - lose;
}

Point naiveBarricadePlacement(const Game& game) {
    const auto baseDist = baseGoalDist(game);
    const auto cells = game.barricadePlacements();
    Point best{0, 0};
    int bestScore = -kInf;
    for (const Point& c : cells) {
        const int s = scoreBarricade(game, baseDist, c);
        if (s > bestScore || (s == bestScore && c.y < best.y)) {
            bestScore = s;
            best = c;
        }
    }
    return best;
}

std::vector<BarricadeRecommendation> barricadeRecommendations(const Game& game, int topN) {
    std::vector<BarricadeRecommendation> out;
    const auto baseDist = baseGoalDist(game);
    for (const Point& c : game.barricadePlacements()) {
        out.push_back({c, static_cast<double>(scoreBarricade(game, baseDist, c))});
    }
    std::sort(out.begin(), out.end(),
              [](const BarricadeRecommendation& a, const BarricadeRecommendation& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.cell.y < b.cell.y;
              });
    if (static_cast<int>(out.size()) > topN) out.resize(static_cast<size_t>(topN));
    return out;
}

// Plays one full end-game from `g` (which must be mid-turn: either dice rolled
// or a barricade pending) and returns the winner, or -1 if the turn cap was hit.
static int simulateOneGame(const Game& g, std::mt19937& rng) {
    Game sim = g;
    const auto dist = buildGoalDistDynamic(currentBarricades(g));
    for (int turns = 0; turns < 5000; ++turns) {
        if (sim.isOver()) return sim.winner();
        if (sim.pendingBarricade()) {
            sim.placeBarricadeFast(fastBarricadePlacement(sim, dist));
        } else if (sim.dice() == 0) {
            sim.startTurn();
        } else {
            const AIMove mv = cheapMove(sim, sim.currentPlayer(), rng, dist);
            if (mv.pawn >= 0) {
                sim.movePawnFast(sim.currentPlayer(), mv.pawn, mv.dest);
            } else {
                sim.nextTurn();  // no legal move: turn skipped
            }
        }
    }
    return -1;
}

std::vector<double> simulateWinChances(const Game& game, long long nGames, int nThreads) {
    const int players = game.playerCount();
    if (nGames <= 0) return std::vector<double>(players, 0.0);
    std::atomic<bool> stop{false};
    std::vector<std::atomic<long long>> wins(players);
    std::atomic<long long> games{0};
    simulateWinChancesAsync(game, nGames, &stop, wins.data(), players, &games, nThreads);
    std::vector<double> out(players, 0.0);
    long long total = 0;
    for (int p = 0; p < players; ++p) total += wins[p].load();
    if (total > 0) {
        for (int p = 0; p < players; ++p) {
            out[p] = static_cast<double>(wins[p].load()) / total;
        }
    }
    return out;
}

void simulateWinChancesAsync(const Game& game, long long targetGames,
                             std::atomic<bool>* stop,
                             std::atomic<long long>* winsOut, int players,
                             std::atomic<long long>* gamesOut,
                             int nThreads) {
    if (nThreads < 1) nThreads = std::thread::hardware_concurrency();
    if (nThreads < 1) nThreads = 1;
    if (nThreads > 16) nThreads = 16;

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(nThreads));
    for (int t = 0; t < nThreads; ++t) {
        threads.emplace_back([&, t]() {
            std::mt19937 rng(std::random_device{}() ^
                             (static_cast<unsigned>(t) * 2654435761u));
            while (!stop->load(std::memory_order_relaxed)) {
                if (gamesOut->load(std::memory_order_relaxed) >= targetGames) break;
                const int w = simulateOneGame(game, rng);
                if (w >= 0 && w < players) {
                    winsOut[w].fetch_add(1, std::memory_order_relaxed);
                }
                gamesOut->fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& th : threads) th.join();
}

}  // namespace barricade