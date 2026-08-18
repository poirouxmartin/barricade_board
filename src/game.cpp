#include "game.h"

#include <algorithm>
#include <random>

namespace barricade {

int rollDie() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    return 1 + rng() % 6;
}

}  // namespace

namespace barricade {

// Static orthogonal adjacency table over the track graph.
const std::array<std::array<Neighbors, kRows>, kCols>& neighbors() {
    static const auto t = [] {
        std::array<std::array<Neighbors, kRows>, kCols> t{};
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (int y = 0; y < 14; ++y) {
            for (int x = 0; x < kCols; ++x) {
                if (!isTrackCell(x, y)) continue;
                for (int k = 0; k < 4; ++k) {
                    const Point np{x + dx[k], y + dy[k]};
                    if (isTrackCell(np.x, np.y)) {
                        t[x][y].cells[t[x][y].count++] = np;
                    }
                }
            }
        }
        return t;
    }();
    return t;
}

Game::Game(int playerCount) : player_count_(playerCount) {
    reset();
}

void Game::reset() {
    current_ = 0;
    dice_ = 0;
    over_ = false;
    winner_ = -1;
    pending_barricade_ = false;
    captured_barricade_ = -1;
    for (int p = 0; p < kMaxPlayers; ++p) {
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            pawns_[p][m] = {-1, -1};  // in base
        }
    }
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            barricade_grid_[x][y] = 0;
            pawn_grid_[x][y] = 255;
        }
    }
    int b = 0;
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < kCols; ++x) {
            if (isInitialBarricadeCell(x, y)) {
                barricades_[b++] = {x, y};
                barricade_grid_[x][y] = 1;
            }
        }
    }
}

void Game::startTurn() {
    dice_ = rollDie();
}

void Game::nextTurn() {
    if (over_) return;
    current_ = (current_ + 1) % player_count_;
    dice_ = rollDie();
}

bool Game::ownPawnAt(Point p, int player) const {
    const uint8_t id = pawn_grid_[p.x][p.y];
    return id != 255 && id / kPawnsPerPlayer == player;
}

// Depth-first walk collecting legal landing cells. Returns true when `out` is
// full so callers can short-circuit (used to find just one destination).
bool Game::explore(Point cur, Point prev, int steps, Point* out, int& count, int maxOut,
                   int player, char* seen) const {
    if (steps == 0) {
        if (baseOwner(cur.x, cur.y) != -1) return false;  // never land on a base
        if (ownPawnAt(cur, player)) return false;
        const int idx = cur.y * kCols + cur.x;
        if (seen[idx]) return false;  // already collected this cell
        if (count >= maxOut) return true;
        seen[idx] = 1;
        out[count++] = cur;
        return count >= maxOut;
    }

    int n = 0;
    Point cands[4];
    if (baseOwner(cur.x, cur.y) == player) {
        cands[n++] = baseFrontCell(player);
    } else {
        const Neighbors& nb = neighbors()[cur.x][cur.y];
        n = nb.count;
        for (int i = 0; i < n; ++i) cands[i] = nb.cells[i];
    }
    for (int i = 0; i < n; ++i) {
        const Point np = cands[i];
        if (np == prev) continue;                                   // no double-back
        if (steps > 1 && barricade_grid_[np.x][np.y]) continue;     // cannot pass over a barricade
        if (explore(np, cur, steps - 1, out, count, maxOut, player, seen)) return true;
    }
    return false;
}

int Game::legalDestinationsTo(int player, int pawn, Point* out, int maxOut, char* seen) const {
    if (over_ || dice_ <= 0) return 0;
    int count = 0;
    Point start = pawns_[player][pawn];
    if (start.x < 0) start = baseCell(player, pawn);
    explore(start, start, dice_, out, count, maxOut, player, seen);
    return count;
}

std::vector<Point> Game::legalDestinations(int player, int pawn) const {
    std::vector<Point> out;
    if (over_ || dice_ <= 0) return out;
    Point buf[512];
    std::array<char, kCols * kRows> seen{};
    const int n = legalDestinationsTo(player, pawn, buf, 512, seen.data());
    out.assign(buf, buf + n);
    return out;
}

bool Game::hasLegalMove(int player) const {
    if (over_ || dice_ <= 0) return false;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        if (!legalDestinations(player, m).empty()) return true;
    }
    return false;
}

bool Game::movePawnFast(int player, int pawn, Point dest) {
    return applyMove(player, pawn, dest);
}

bool Game::movePawn(int player, int pawn, Point dest) {
    if (over_ || player != current_) return false;
    const auto legal = legalDestinations(player, pawn);
    if (std::find(legal.begin(), legal.end(), dest) == legal.end()) return false;
    return applyMove(player, pawn, dest);
}

bool Game::applyMove(int player, int pawn, Point dest) {
    if (over_ || player != current_) return false;

    const Point old = pawns_[player][pawn];
    if (old.x >= 0) pawn_grid_[old.x][old.y] = 255;
    for (int p = 0; p < player_count_; ++p) {
        if (p == player) continue;
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            if (pawns_[p][m] == dest) pawns_[p][m] = {-1, -1};  // captured -> base
        }
    }
    for (int i = 0; i < kBarricadeCount; ++i) {
        if (barricades_[i] == dest) {
            pending_barricade_ = true;
            captured_barricade_ = i;
            barricades_[i] = {-1, -1};
            barricade_grid_[dest.x][dest.y] = 0;
            break;
        }
    }

    pawns_[player][pawn] = dest;
    pawn_grid_[dest.x][dest.y] = static_cast<uint8_t>(player * kPawnsPerPlayer + pawn);
    if (isGoalCell(dest.x, dest.y)) {
        over_ = true;
        winner_ = player;
    }
    if (!pending_barricade_ && !over_) nextTurn();
    return true;
}

std::vector<Point> Game::barricadePlacements() const {
    std::vector<Point> out;
    if (!pending_barricade_) return out;
    for (int y = 0; y <= kBottomRowY; ++y) {
        for (int x = 0; x < kCols; ++x) {
            if (!isTrackCell(x, y) || y == kBottomRowY || isGoalCell(x, y)) continue;
            const Point p{x, y};
            if (barricadeAt(p) || pawnAt(p) != -1) continue;
            out.push_back(p);
        }
    }
    return out;
}

bool Game::placeBarricade(Point dest) {
    if (!pending_barricade_) return false;
    const auto cells = barricadePlacements();
    if (std::find(cells.begin(), cells.end(), dest) == cells.end()) return false;
    barricades_[captured_barricade_] = dest;
    barricade_grid_[dest.x][dest.y] = 1;
    pending_barricade_ = false;
    if (!over_) nextTurn();
    return true;
}

bool Game::placeBarricadeFast(Point dest) {
    if (!pending_barricade_) return false;
    barricades_[captured_barricade_] = dest;
    barricade_grid_[dest.x][dest.y] = 1;
    pending_barricade_ = false;
    if (!over_) nextTurn();
    return true;
}

Point Game::pawnPos(int player, int pawn) const {
    const Point p = pawns_[player][pawn];
    if (p.x < 0) return baseCell(player, pawn);
    return p;
}

bool Game::pawnInBase(int player, int pawn) const {
    return pawns_[player][pawn].x < 0;
}

int Game::pawnAt(Point p) const {
    const uint8_t v = pawn_grid_[p.x][p.y];
    return v == 255 ? -1 : static_cast<int>(v);
}

bool Game::barricadeAt(Point p) const {
    return barricade_grid_[p.x][p.y] != 0;
}

}  // namespace barricade