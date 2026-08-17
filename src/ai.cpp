#include "ai.h"

#include <array>
#include <vector>

namespace barricade {

namespace {

constexpr int kInf = 1000000;

int indexOf(Point p) {
    return p.y * kCols + p.x;
}

int distAt(const std::vector<int>& dist, Point p) {
    if (p.x < 0 || p.x >= kCols || p.y < 0 || p.y >= kRows) return kInf;
    return dist[indexOf(p)];
}

// BFS from the goal over the track graph, avoiding `blocked` cells.
// Returns the number of steps to reach the goal from every cell (kInf if blocked).
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

int distToGoal(Point p) {
    static const std::vector<int> d = bfsFromGoal({});
    return distAt(d, p);
}

int pawnDistToGoal(const std::vector<int>& goalDist, const Game& g, int player, int pawn) {
    if (g.pawnInBase(player, pawn)) {
        const int d = distAt(goalDist, baseFrontCell(player));
        return d == kInf ? kInf : d + 1;
    }
    return distAt(goalDist, g.pawnPos(player, pawn));
}

}  // namespace

AIMove naiveMove(const Game& game, int player) {
    const auto goalDist = bfsFromGoal({});
    std::vector<std::vector<int>> opDists;
    for (int op = 0; op < game.playerCount(); ++op) {
        if (op == player) continue;
        for (int om = 0; om < kPawnsPerPlayer; ++om) {
            opDists.push_back(bfsFrom(game.pawnPos(op, om)));
        }
    }

    AIMove best;
    int bestScore = -kInf;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        const auto dests = game.legalDestinations(player, m);
        const int curD = pawnDistToGoal(goalDist, game, player, m);
        if (curD == kInf) continue;
        for (const Point& dest : dests) {
            int score = (curD - distAt(goalDist, dest)) * 20;
            if (isGoalCell(dest.x, dest.y)) score += 1000000;
            const int victim = game.pawnAt(dest);
            if (victim >= 0) score += 1500;  // captures an opponent pawn
            if (game.barricadeAt(dest)) score += 600;
            int risk = 0;
            for (size_t i = 0; i < opDists.size(); ++i) {
                const int dd = opDists[i][indexOf(dest)];
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