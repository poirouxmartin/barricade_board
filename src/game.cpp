#include "game.h"

#include <algorithm>
#include <random>

namespace barricade {

namespace {
std::vector<std::string> splitTok(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t end = s.find(sep, pos);
        out.push_back(s.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return out;
}

bool parsePoint(const std::string& t, Point& p) {
    const size_t c = t.find(',');
    if (c == std::string::npos) return false;
    p.x = std::atoi(t.substr(0, c).c_str());
    p.y = std::atoi(t.substr(c + 1).c_str());
    return true;
}
}  // namespace

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

// Distances from the goal cell over the track graph (unweighted BFS), computed
// once. Used to resolve a turn-capped game by "closest to the goal wins".
const std::vector<int>& goalProximity() {
    static const std::vector<int> dist = [] {
        constexpr int kInf = 1'000'000;
        std::vector<int> d(kCols * kRows, kInf);
        std::vector<int> queue;
        queue.reserve(kCols * kRows);
        d[0 * kCols + 8] = 0;
        queue.push_back(0 * kCols + 8);
        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const int cur = queue[head];
            const int cx = cur % kCols;
            const int cy = cur / kCols;
            for (int k = 0; k < 4; ++k) {
                const int nx = cx + dx[k];
                const int ny = cy + dy[k];
                if (!isTrackCell(nx, ny)) continue;
                const int ni = ny * kCols + nx;
                if (d[cur] + 1 < d[ni]) {
                    d[ni] = d[cur] + 1;
                    queue.push_back(ni);
                }
            }
        }
        return d;
    }();
    return dist;
}

void Game::resolveDeadlock() {
    over_ = true;
    deadlock_ = true;
    winner_ = deadlockWinner();
}

int Game::deadlockWinner() const {
    const auto& prox = goalProximity();
    int bestDist = 1'000'000;
    int bestSum = 1'000'000;
    int winner = 0;
    for (int p = 0; p < player_count_; ++p) {
        int minDist = 1'000'000;
        int sum = 0;
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            Point pos = pawnPos(p, m);
            if (pawnInBase(p, m)) pos = baseFrontCell(p);
            const int d = prox[pos.y * kCols + pos.x];
            if (d < minDist) minDist = d;
            sum += d;
        }
        // Closest pawn wins; a tie falls back to the tighter army.
        if (minDist < bestDist || (minDist == bestDist && sum < bestSum)) {
            bestDist = minDist;
            bestSum = sum;
            winner = p;
        }
    }
    return winner;
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
    actions_ = 0;
    deadlock_ = false;
    for (int p = 0; p < kMaxPlayers; ++p) {
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            pawns_[p][m] = {-1, -1};  // in base
        }
    }
    for (int i = 0; i < barricade_grid_.size(); ++i) barricade_grid_[i] = 0;
    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) pawn_grid_[x][y] = 255;
    }
    int b = 0;
    for (int y = 0; y < 14; ++y) {
        for (int x = 0; x < kCols; ++x) {
            if (isInitialBarricadeCell(x, y)) {
                barricades_[b++] = {x, y};
                setBarricade({x, y});
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
    const int id = pawnAt(p);
    return id >= 0 && id / kPawnsPerPlayer == player;
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
        if (steps > 1 && barricadeAt(np)) continue;                 // cannot pass over a barricade
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
    if (old.x >= 0) setPawn(old, 255);
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
            clearBarricade(dest);
            break;
        }
    }

    pawns_[player][pawn] = dest;
    setPawn(dest, player * kPawnsPerPlayer + pawn);
    if (isGoalCell(dest.x, dest.y)) {
        over_ = true;
        winner_ = player;
    }
    if (!over_ && ++actions_ >= kMaxActions) resolveDeadlock();
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
    setBarricade(dest);
    pending_barricade_ = false;
    if (!over_ && ++actions_ >= kMaxActions) resolveDeadlock();
    if (!over_) nextTurn();
    return true;
}

bool Game::placeBarricadeFast(Point dest) {
    if (!pending_barricade_) return false;
    barricades_[captured_barricade_] = dest;
    setBarricade(dest);
    pending_barricade_ = false;
    if (!over_ && ++actions_ >= kMaxActions) resolveDeadlock();
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
    const int i = p.y * kCols + p.x;
    return (barricade_grid_[i >> 6] >> (i & 63)) & 1ULL;
}

void Game::setPawn(Point p, int id) {
    pawn_grid_[p.x][p.y] = static_cast<uint8_t>(id);
}

void Game::setBarricade(Point p) {
    const int i = p.y * kCols + p.x;
    barricade_grid_[i >> 6] |= 1ULL << (i & 63);
}

void Game::clearBarricade(Point p) {
    const int i = p.y * kCols + p.x;
    barricade_grid_[i >> 6] &= ~(1ULL << (i & 63));
}

std::string Game::savePosition() const {
    std::string s = "barricade";
    s += ";N=" + std::to_string(player_count_);
    s += ";turn=" + std::to_string(current_);
    s += ";dice=" + std::to_string(dice_);
    s += ";hand=" + std::to_string(pending_barricade_ ? 1 : 0);
    s += ";over=" + std::to_string(over_ ? 1 : 0);
    s += ";winner=" + std::to_string(winner_);
    for (int p = 0; p < player_count_; ++p) {
        s += ";P" + std::to_string(p) + "=";
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            if (m) s += '|';
            const Point c = pawns_[p][m];
            if (c.x < 0) {
                s += '-';
            } else {
                s += std::to_string(c.x) + "," + std::to_string(c.y);
            }
        }
    }
    s += ";bars=";
    bool first = true;
    for (const Point& b : barricades_) {
        if (b.x < 0) continue;
        if (!first) s += '|';
        s += std::to_string(b.x) + "," + std::to_string(b.y);
        first = false;
    }
    s += ";act=" + std::to_string(actions_);
    return s;
}

bool Game::loadPosition(const std::string& text) {
    reset();

    int players = -1;
    int turn = -1;
    int dice = -1;
    int hand = 0;
    int act = 0;
    bool over = false;
    int winner = -1;
    Point pawnsIn[kMaxPlayers][kPawnsPerPlayer];
    for (int p = 0; p < kMaxPlayers; ++p) {
        for (int m = 0; m < kPawnsPerPlayer; ++m) pawnsIn[p][m] = {-1, -1};
    }
    std::vector<Point> bars;

    bool ok = true;
    std::vector<std::string> toks = splitTok(text, ';');
    if (toks.empty() || toks[0] != "barricade") ok = false;
    for (size_t i = 1; ok && i < toks.size(); ++i) {
        const std::string& t = toks[i];
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = t.substr(0, eq);
        const std::string v = t.substr(eq + 1);
if (k == "N") {
            players = std::atoi(v.c_str());
            if (players < 2 || players > kMaxPlayers) ok = false;
        } else if (k == "turn") {
            turn = std::atoi(v.c_str());
        } else if (k == "dice") {
            dice = std::atoi(v.c_str());
            if (dice < 0 || dice > 6) ok = false;
        } else if (k == "hand") {
            hand = std::atoi(v.c_str());
            if (hand != 0 && hand != 1) ok = false;
        } else if (k == "act") {
            act = std::atoi(v.c_str());
        } else if (k == "over") {
            over = std::atoi(v.c_str()) != 0;
} else if (k == "winner") {
            winner = std::atoi(v.c_str());
        } else if (k.size() == 2 && k[0] == 'P') {
            const int p = k[1] - '0';
            if (p < 0 || p >= kMaxPlayers) {
                ok = false;
                break;
            }
            std::vector<std::string> cells = splitTok(v, '|');
            if (cells.size() != kPawnsPerPlayer) {
                ok = false;
                break;
            }
            for (int m = 0; m < kPawnsPerPlayer; ++m) {
                if (cells[m] == "-") continue;
                Point c;
                if (!parsePoint(cells[m], c) || c.x < 0 || c.y < 0 || !isTrackCell(c.x, c.y)) {
                    ok = false;
                    break;
                }
                pawnsIn[p][m] = c;
            }
        } else if (k == "bars") {
            std::vector<std::string> cells = splitTok(v, '|');
            for (const std::string& c : cells) {
                if (c.empty()) continue;
                Point b;
                if (!parsePoint(c, b) || !isTrackCell(b.x, b.y) || b.y == kBottomRowY || isGoalCell(b.x, b.y)) {
                    ok = false;
                    break;
                }
                bars.push_back(b);
            }
        }
    }
    if (ok) {
        if (players < 2 || turn < 0 || turn >= players) ok = false;
        if (hand != 0 && hand != 1) ok = false;
        if (bars.size() != static_cast<size_t>(kBarricadeCount - hand)) ok = false;
    }
    if (!ok) {
        reset();
        return false;
    }

    player_count_ = players;
    current_ = turn;
    dice_ = dice;
    over_ = over;
    winner_ = winner;
    pending_barricade_ = hand == 1;
    captured_barricade_ = -1;
    actions_ = act;
    deadlock_ = false;

// Place the barricades exactly as listed; when one is in hand its slot stays
    // empty and becomes the captured index (barricades are interchangeable).
    for (int i = 0; i < static_cast<int>(barricade_grid_.size()); ++i) barricade_grid_[i] = 0;
    for (int i = 0; i < static_cast<int>(barricades_.size()); ++i) barricades_[i] = {-1, -1};
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        barricades_[i] = bars[i];
        setBarricade(bars[i]);
    }
    if (hand == 1) {
        captured_barricade_ = static_cast<int>(bars.size());
        barricades_[bars.size()] = {-1, -1};
    }

    for (int p = 0; p < player_count_; ++p) {
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            pawns_[p][m] = pawnsIn[p][m];
            if (pawnsIn[p][m].x < 0) continue;
if (pawnAt(pawnsIn[p][m]) != -1) {
                reset();
                return false;  // two pawns on the same cell
            }
            if (barricadeAt(pawnsIn[p][m])) {
                reset();
                return false;  // pawn standing on a placed barricade
            }
            setPawn(pawnsIn[p][m], p * kPawnsPerPlayer + m);
        }
    }
    return true;
}

}  // namespace barricade

