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
#include <vector>

namespace barricade {

namespace {

constexpr int kInf = 1000000;
constexpr double kUctC = 1.414;
constexpr int kRolloutSteps = 10;
constexpr int kMaxIterations = 2000000;
}  // namespace

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

int pawnDistToGoal(const std::vector<int>& dist, const Game& g, int player, int pawn) {
    if (g.pawnInBase(player, pawn)) {
        const int d = distAt(dist, baseFrontCell(player));
        return d == kInf ? kInf : d + 1;
    }
    return distAt(dist, g.pawnPos(player, pawn));
}

int progress(const Game& g, int player) {
    int best = kInf;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        best = std::min(best, pawnDistToGoal(goalDist(), g, player, m));
    }
    return best;
}

double heuristicEval(const Game& g, int rootPlayer) {
    int myMin = progress(g, rootPlayer);
    int oppMin = kInf;
    for (int op = 0; op < g.playerCount(); ++op) {
        if (op != rootPlayer) oppMin = std::min(oppMin, progress(g, op));
    }
    if (myMin == kInf) myMin = 30;
    if (oppMin == kInf) oppMin = 30;
    double v = (oppMin - myMin) / 10.0;
    if (v > 1.0) v = 1.0;
    if (v < -1.0) v = -1.0;
    return v;
}

// Greedy walk used by the rollout: moves a pawn `g.dice()` steps towards the
// goal, respecting the move rules (barricades block intermediate steps, the
// final landing may capture). Returns {-1,-1} if the pawn cannot complete all
// steps greedily.
Point greedyWalkDest(const Game& g, int player, int pawn) {
    const int dice = g.dice();
    Point cur = g.pawnPos(player, pawn);
    Point prev = cur;
    bool inBase = g.pawnInBase(player, pawn);
    const auto& dist = goalDist();
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
            }
        }
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
AIMove cheapMove(const Game& g, int player, std::mt19937& rng) {
    if (g.dice() <= 0) return {};
    int order[5] = {0, 1, 2, 3, 4};
    for (int i = 4; i > 0; --i) {
        std::swap(order[i], order[rng() % (i + 1)]);
    }
    for (int k = 0; k < kPawnsPerPlayer; ++k) {
        const Point d = greedyWalkDest(g, player, order[k]);
        if (d.x >= 0) return {order[k], d};
    }
    return cheapMoveFallback(g, player, rng);
}

// Simulates a game from `g0` to the end (or horizon) using the heuristic.
double rollout(const Game& g0, int rootPlayer, std::mt19937& rng) {
    Game g = g0;
    for (int step = 0; step < kRolloutSteps && !g.isOver(); ++step) {
        if (g.pendingBarricade()) {
            g.placeBarricadeFast(cheapBarricadePlacement(g));
        } else if (g.dice() == 0) {
            g.startTurn();
        } else {
            const AIMove mv = cheapMove(g, g.currentPlayer(), rng);
            if (mv.pawn >= 0) {
                g.movePawnFast(g.currentPlayer(), mv.pawn, mv.dest);
            } else {
                g.nextTurn();  // no legal move: skipped
            }
        }
    }
    if (g.isOver()) return g.winner() == rootPlayer ? 1.0 : -1.0;
    return heuristicEval(g, rootPlayer);
}

// ---------------------------------------------------------------------------
// Root-parallel MCTS. Each worker builds its OWN tree (no shared memory, no
// virtual loss, no atomics), so workers scale almost linearly; the root
// children statistics are merged once the threads have joined.
// ---------------------------------------------------------------------------

struct TreeNode;
struct TreeLink;

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
};

std::atomic<long long> g_nodeCount{0};
std::atomic<long long> g_iterations{0};

// Builds the node's action list exactly once (the tree is private, no lock).
void expandNode(TreeNode* n) {
    Point dests[512];
    char seen[kCols * kRows];
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        std::memset(seen, 0, sizeof seen);
        const int cnt = n->game.legalDestinationsTo(n->player, m, dests, 512, seen);
        for (int i = 0; i < cnt; ++i) n->actions.push_back({m, dests[i]});
    }
    if (n->actions.empty()) n->actions.push_back(AIMove{});  // skip pseudo-action
    n->expanded = true;
}

TreeNode* createChild(TreeNode* n, const AIMove& mv, std::mt19937& rng, Worker& w) {
    Game g = n->game;
    if (mv.pawn >= 0) {
        if (!g.movePawn(n->player, mv.pawn, mv.dest)) return nullptr;
        if (g.pendingBarricade()) {
            g.placeBarricadeFast(cheapBarricadePlacement(g));
        }
    } else {
        g.nextTurn();
    }
    g.forceDice(1 + rng() % 6);

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
                std::chrono::steady_clock::time_point start, int budgetMs, Worker& w) {
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
        if (!n->expanded) expandNode(n);

        double v;
        if (n->nextAction < n->actions.size()) {
            const size_t idx = n->nextAction++;
            TreeNode* child = createChild(n, n->actions[idx], rng, w);
            if (!child) {  // movePawn rejected a supposedly legal move
                ++iter;
                continue;
            }
            publish(n, child, n->actions[idx], w);
            v = rollout(child->game, player, rng);
            child->visits = 1;
            child->score = v;
            for (TreeNode* p : path) {
                p->visits++;
                p->score += v;
            }
        } else {
            v = heuristicEval(n->game, player);
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
        treeSearch(roots[0], player, start, budgetMs, *workers[0]);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(nThreads);
        for (int t = 0; t < nThreads; ++t) {
            threads.emplace_back(
                [&, t] { treeSearch(roots[t], player, start, budgetMs, *workers[t]); });
        }
        for (std::thread& th : threads) th.join();
    }

    long long totalIter = 0;
    long long totalNodes = 0;
    for (int t = 0; t < nThreads; ++t) {
        totalIter += workers[t]->iterations;
        totalNodes += workers[t]->nodeConstructed;
    }
    g_iterations.store(totalIter, std::memory_order_relaxed);
    g_nodeCount.store(totalNodes + nThreads, std::memory_order_relaxed);  // + roots

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
    return out;
}

AIMove mctsMove(const Game& game, int player, int budgetMs) {
    return pickBest(mctsActionStats(game, player, budgetMs), game, player);
}

// Diagnostics for tuning: iterations and tree nodes of the last
// `mctsActionStats` call.
long long mctsIterationCount() { return g_iterations.load(std::memory_order_relaxed); }
long long mctsNodeCount() { return g_nodeCount.load(std::memory_order_relaxed); }

std::vector<MctsRecommendation> mctsRecommendations(const Game& game, int player, int budgetMs) {
    std::vector<MctsRecommendation> out;
    for (const ActionStats& s : mctsActionStats(game, player, budgetMs)) {
        if (s.move.pawn < 0 || s.visits == 0) continue;
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
// pawns detour (gain/loss 2).
Point cheapBarricadePlacement(const Game& game) {
    const PlacementTables& pt = placementTables();
    const auto& dist = goalDist();

    int myDist[kPawnsPerPlayer];
    for (int om = 0; om < kPawnsPerPlayer; ++om) {
        myDist[om] = pawnDistToGoal(dist, game, game.currentPlayer(), om);
    }
    int opDist[kMaxPlayers * kPawnsPerPlayer];
    int nOp = 0;
    for (int op = 0; op < game.playerCount(); ++op) {
        if (op == game.currentPlayer()) continue;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            opDist[nOp++] = pawnDistToGoal(dist, game, op, om);
        }
    }

    Point best{0, 0};
    int bestScore = -kInf;
    int scored = 0;
    for (const Point& c : pt.rank) {
        if (game.barricadeAt(c) || game.pawnAt(c) != -1) continue;
        if (++scored > 5) break;

        const int cg = distAt(dist, c);
        const int fwd = pt.fwd[indexOf(c)];
        const int oppImpact = (fwd == 1) ? 40 : 2;
        const int myImpact = (fwd == 1) ? 1000 : 2;

        int gain = 0;
        for (int i = 0; i < nOp; ++i) {
            if (opDist[i] != kInf && cg < opDist[i]) gain += oppImpact;
        }
        int lose = 0;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            if (myDist[om] != kInf && cg < myDist[om]) lose += myImpact;
        }
        const int score = gain - lose;
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }
    if (bestScore == -kInf) {
        // Very crowded board: nothing scored, return the first legal cell.
        for (const Point& c : pt.rank) {
            if (!game.barricadeAt(c) && game.pawnAt(c) == -1) return c;
        }
    }
    return best;
}

Point naiveBarricadePlacement(const Game& game) {
    std::vector<Point> blocks;
    for (const Point& b : game.barricades()) {
        if (b.x >= 0) blocks.push_back(b);
    }
    const auto baseDist = bfsFromGoal(blocks);

    const auto cells = game.barricadePlacements();
    Point best{0, 0};
    int bestScore = -kInf;
    for (const Point& c : cells) {
        std::vector<Point> blocked = blocks;
        blocked.push_back(c);
        const auto bd = bfsFromGoal(blocked);

        int gain = 0;
        for (int op = 0; op < game.playerCount(); ++op) {
            if (op == game.currentPlayer()) continue;
            for (int om = 0; om < kPawnsPerPlayer; ++om) {
                const int a = pawnDistToGoal(baseDist, game, op, om);
                const int b = pawnDistToGoal(bd, game, op, om);
                if (a == kInf) continue;
                gain += (b == kInf) ? 40 : (b - a);
            }
        }
        int lose = 0;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            const int a = pawnDistToGoal(baseDist, game, game.currentPlayer(), om);
            const int b = pawnDistToGoal(bd, game, game.currentPlayer(), om);
            if (a == kInf) continue;
            lose += (b == kInf) ? 1000 : (b - a);
        }

        const int score = gain - lose;
        if (score > bestScore || (score == bestScore && c.y < best.y)) {
            bestScore = score;
            best = c;
        }
    }
    return best;
}

}  // namespace barricade