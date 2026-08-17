#include "game.h"

#include <algorithm>
#include <random>

namespace barricade {

namespace {

int rollDie() {
    static std::random_device rd;
    static std::mt19937 rng(rd());
    return 1 + rng() % 6;
}

}  // namespace

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
    int b = 0;
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < kCols; ++x) {
            if (isInitialBarricadeCell(x, y)) {
                barricades_[b++] = {x, y};
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
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        if (pawns_[player][m] == p) return true;
    }
    return false;
}

void Game::explore(Point cur, Point prev, int steps, std::vector<Point>& out, int player) const {
    if (steps == 0) {
        if (baseOwner(cur.x, cur.y) != -1) return;  // never land on a base
        if (ownPawnAt(cur, player)) return;
        out.push_back(cur);
        return;
    }

    Point cands[4];
    int n = 0;
    if (baseOwner(cur.x, cur.y) == player) {
        cands[n++] = baseFrontCell(player);
    } else {
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (int i = 0; i < 4; ++i) {
            const Point np{cur.x + dx[i], cur.y + dy[i]};
            if (isTrackCell(np.x, np.y)) cands[n++] = np;
        }
    }
    for (int i = 0; i < n; ++i) {
        const Point np = cands[i];
        if (np == prev) continue;                          // no double-back
        if (steps > 1 && barricadeAt(np)) continue;        // cannot pass over a barricade
        explore(np, cur, steps - 1, out, player);
    }
}

std::vector<Point> Game::legalDestinations(int player, int pawn) const {
    std::vector<Point> out;
    if (over_ || dice_ <= 0) return out;
    Point start = pawns_[player][pawn];
    if (start.x < 0) start = baseCell(player, pawn);
    explore(start, start, dice_, out, player);
    return out;
}

bool Game::hasLegalMove(int player) const {
    if (over_ || dice_ <= 0) return false;
    for (int m = 0; m < kPawnsPerPlayer; ++m) {
        if (!legalDestinations(player, m).empty()) return true;
    }
    return false;
}

bool Game::movePawn(int player, int pawn, Point dest) {
    if (over_ || player != current_) return false;
    const auto legal = legalDestinations(player, pawn);
    if (std::find(legal.begin(), legal.end(), dest) == legal.end()) return false;

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
            break;
        }
    }

    pawns_[player][pawn] = dest;
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
    for (int pl = 0; pl < kMaxPlayers; ++pl) {
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            if (pawns_[pl][m] == p) return pl * kPawnsPerPlayer + m;
        }
    }
    return -1;
}

bool Game::barricadeAt(Point p) const {
    for (int i = 0; i < kBarricadeCount; ++i) {
        if (barricades_[i] == p) return true;
    }
    return false;
}

}  // namespace barricade