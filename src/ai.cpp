#include "ai.h"

#include <algorithm>
#include <array>
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
constexpr int kMaxIterations = 100000;
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

const std::vector<int>& goalDist() {
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

// Cheap rollout move: picks a random pawn that can move, then its best
// destination by greedy score. Much cheaper than fastMove (1 DFS vs 5).
AIMove cheapMove(const Game& g, int player, std::mt19937& rng) {
    int order[5] = {0, 1, 2, 3, 4};
    for (int i = 4; i > 0; --i) {
        std::swap(order[i], order[rng() % (i + 1)]);
    }
    Point dests[512];
    char seen[kCols * kRows];
    for (int k = 0; k < kPawnsPerPlayer; ++k) {
        const int m = order[k];
        std::memset(seen, 0, sizeof seen);
        const int n = g.legalDestinationsTo(player, m, dests, 512, seen);
        if (n == 0) continue;
        const int curD = pawnDistToGoal(goalDist(), g, player, m);
        if (curD == kInf) continue;
        int bestScore = -kInf;
        AIMove best{m, dests[0]};
        for (int i = 0; i < n; ++i) {
            const Point dest = dests[i];
            int score = (curD - distAt(goalDist(), dest)) * 20;
            if (isGoalCell(dest.x, dest.y)) score += 1000000;
            if (g.pawnAt(dest) >= 0) score += 1500;
            if (g.barricadeAt(dest)) score += 600;
            if (score > bestScore) {
                bestScore = score;
                best = {m, dest};
            }
        }
        return best;
    }
    return AIMove{};
}

// Simulates a game from `g0` to the end (or horizon) using the heuristic.
double rollout(const Game& g0, int rootPlayer, std::mt19937& rng) {
    Game g = g0;
    for (int step = 0; step < kRolloutSteps && !g.isOver(); ++step) {
        if (g.pendingBarricade()) {
            g.placeBarricade(naiveBarricadePlacement(g));
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

struct MctsNode {
    explicit MctsNode(Game g) : game(std::move(g)) {}
    Game game;
    int player = 0;
    int visits = 0;
    double score = 0.0;
    std::vector<AIMove> actions;
    std::vector<size_t> unexpanded;
    std::vector<MctsNode*> children;
    MctsNode* parent = nullptr;
};

void fillActions(MctsNode* n) {
    if (!n->actions.empty()) return;
    Point dests[512];
    char seen[kCols * kRows];
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        std::memset(seen, 0, sizeof seen);
        const int cnt = n->game.legalDestinationsTo(n->player, m, dests, 512, seen);
        for (int i = 0; i < cnt; ++i) n->actions.push_back({m, dests[i]});
    }
    if (n->actions.empty()) n->actions.push_back(AIMove{});  // skip pseudo-action
    n->children.assign(n->actions.size(), nullptr);
    for (size_t i = 0; i < n->actions.size(); ++i) n->unexpanded.push_back(i);
}

MctsNode* createChild(MctsNode* n, size_t idx, std::mt19937& rng,
                      std::vector<std::unique_ptr<MctsNode>>& arena) {
    Game g = n->game;
    const AIMove& mv = n->actions[idx];
    if (mv.pawn >= 0) {
        if (!g.movePawn(n->player, mv.pawn, mv.dest)) return nullptr;
        if (g.pendingBarricade()) g.placeBarricade(naiveBarricadePlacement(g));
    } else {
        g.nextTurn();
    }
    g.forceDice(1 + rng() % 6);

    arena.emplace_back(new MctsNode(std::move(g)));
    MctsNode* child = arena.back().get();
    child->game = std::move(g);
    child->player = child->game.currentPlayer();
    child->parent = n;
    n->children[idx] = child;
    return child;
}

MctsNode* uctSelect(MctsNode* n, std::mt19937& rng) {
    MctsNode* best = nullptr;
    double bestUct = -1.0;
    for (MctsNode* c : n->children) {
        if (!c) continue;
        if (c->visits == 0) return c;
        const double uct = c->score / c->visits +
                           kUctC * std::sqrt(std::log(n->visits + 1) / c->visits);
        if (uct > bestUct) {
            bestUct = uct;
            best = c;
        }
    }
    return best;
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
            if (game.barricadeAt(dest)) score += 600;
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

struct RootStats {
    std::vector<AIMove> actions;
    std::vector<long long> visits;
    std::vector<double> scores;
};

// Runs a single-threaded MCTS search for `budgetMs` and returns root statistics.
RootStats runSearch(const Game& game, int player, int budgetMs) {
    RootStats stats;
    if (game.dice() <= 0 || game.isOver()) return stats;

    std::mt19937 rng(std::random_device{}());
    const auto start = std::chrono::steady_clock::now();

    MctsNode root(game);
    root.player = game.currentPlayer();

    std::vector<std::unique_ptr<MctsNode>> arena;

    for (int iter = 0; iter < kMaxIterations; ++iter) {
        if ((iter & 31) == 0) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= budgetMs) {
                break;
            }
        }

        MctsNode* n = &root;
        while (!n->game.isOver()) {
            fillActions(n);
            if (!n->unexpanded.empty()) break;
            n = uctSelect(n, rng);
            if (!n) break;
        }

        if (n->game.isOver()) {
            const double v = n->game.winner() == player ? 1.0 : -1.0;
            for (MctsNode* p = n; p; p = p->parent) {
                p->visits++;
                p->score += v;
            }
            continue;
        }

        fillActions(n);
        if (n->unexpanded.empty()) continue;
        const size_t pos = rng() % n->unexpanded.size();
        const size_t idx = n->unexpanded[pos];
        n->unexpanded.erase(n->unexpanded.begin() + pos);

        MctsNode* child = createChild(n, idx, rng, arena);
        if (!child) continue;

        const double v = rollout(child->game, player, rng);
        for (MctsNode* p = child; p; p = p->parent) {
            p->visits++;
            p->score += v;
        }
    }

    stats.actions = root.actions;
    stats.visits.assign(root.actions.size(), 0);
    stats.scores.assign(root.actions.size(), 0.0);
    for (size_t i = 0; i < root.actions.size(); ++i) {
        if (root.children[i]) {
            stats.visits[i] = root.children[i]->visits;
            stats.scores[i] = root.children[i]->score;
        }
    }
    return stats;
}

AIMove pickBest(const std::vector<RootStats>& results, const Game& game, int player) {
    if (results.empty()) return naiveMove(game, player);
    long long bestVisits = -1;
    double bestAvg = -2.0;
    int bestIdx = -1;
    const auto& actions = results[0].actions;
    for (size_t i = 0; i < actions.size(); ++i) {
        long long v = 0;
        double sc = 0.0;
        for (const RootStats& r : results) {
            if (i < r.visits.size()) {
                v += r.visits[i];
                sc += r.scores[i];
            }
        }
        const double avg = v > 0 ? sc / v : -2.0;
        if (v > bestVisits || (v == bestVisits && avg > bestAvg)) {
            bestVisits = v;
            bestAvg = avg;
            bestIdx = static_cast<int>(i);
        }
    }
    if (bestIdx < 0) return naiveMove(game, player);
    return actions[bestIdx];
}

}  // namespace

AIMove mctsMove(const Game& game, int player, int budgetMs) {
    if (game.dice() <= 0 || game.isOver()) return AIMove{};

    int nThreads = std::thread::hardware_concurrency();
    if (nThreads < 1) nThreads = 1;
    if (nThreads > 16) nThreads = 16;
    if (nThreads == 1 || budgetMs < 40) {  // thread spawn overhead not worth it
        const std::vector<RootStats> one{runSearch(game, player, budgetMs)};
        return pickBest(one, game, player);
    }

    std::vector<RootStats> results(nThreads);
    std::vector<std::thread> threads;
    threads.reserve(nThreads);
    for (int t = 0; t < nThreads; ++t) {
        threads.emplace_back([&, t] { results[t] = runSearch(game, player, budgetMs); });
    }
    for (std::thread& th : threads) th.join();

    const std::vector<RootStats> all(results.begin(), results.end());
    return pickBest(all, game, player);
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