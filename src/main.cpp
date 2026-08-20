#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <SDL_ttf.h>

#include "ai.h"
#include "board.h"
#include "game.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCell = 40;
constexpr int kStatusH = 56;
constexpr int kFrame = 22;                                  // wooden frame around the board
constexpr int kBoardW = barricade::kCols * kCell;           // 680
constexpr int kBoardH = barricade::kRows * kCell;           // 760
constexpr int kBoardX = kFrame;
constexpr int kBoardY = kStatusH + kFrame;
constexpr int kPanelW = 452;
constexpr int kPanelX = kBoardX + kBoardW + kFrame;         // 724
constexpr int kWinW = kPanelX + kPanelW;                    // 1176
constexpr int kWinH = kStatusH + kBoardH + 2 * kFrame;      // 852
constexpr int kMenuCX = kBoardX + kBoardW / 2;
constexpr int kPanelPad = 14;

const SDL_Color kGoalColor = {255, 250, 235, 255};
const SDL_Color kGridColor = {120, 100, 72, 255};
const SDL_Color kHighlightColor = {90, 220, 90, 255};
const SDL_Color kPlacementColor = {90, 160, 255, 255};
const SDL_Color kSelectColor = {255, 255, 255, 255};
const SDL_Color kStatusBg = {52, 40, 28, 255};
const SDL_Color kLeatherDark = {38, 29, 20, 255};
const SDL_Color kTextColor = {235, 230, 220, 255};
const SDL_Color kTextDim = {168, 156, 138, 255};
const SDL_Color kGold = {255, 220, 120, 255};

const SDL_Color kPlayerColors[barricade::kMaxPlayers] = {
    {205, 55, 55, 255},    // red
    {55, 130, 205, 255},   // blue
    {225, 200, 45, 255},   // yellow
    {60, 170, 75, 255},    // green
};

const char* kPlayerNames[barricade::kMaxPlayers] = {"Rouge", "Bleu", "Jaune", "Vert"};
const char* kPlayerShorts[barricade::kMaxPlayers] = {"R", "B", "J", "V"};

// Distinct colors for the numbered advice arrows (matches the panel legend).
const SDL_Color kAdvicePalette[8] = {
    {90, 220, 90, 255},    {70, 200, 220, 255}, {235, 160, 60, 255}, {190, 90, 230, 255},
    {240, 120, 130, 255},  {180, 140, 90, 255}, {120, 220, 160, 255}, {200, 200, 90, 255},
};

SDL_Color lighten(SDL_Color c, int d) {
    const auto clamp = [](int v) {
        return static_cast<Uint8>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return {clamp(c.r + d), clamp(c.g + d), clamp(c.b + d), c.a};
}

SDL_Color lerpColor(SDL_Color a, SDL_Color b, double t) {
    const auto cl = [](int v) { return static_cast<Uint8>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    return {cl(a.r + static_cast<int>((b.r - a.r) * t)),
            cl(a.g + static_cast<int>((b.g - a.g) * t)),
            cl(a.b + static_cast<int>((b.b - a.b) * t)), a.a};
}

void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
}

void fillCircle(SDL_Renderer* r, int cx, int cy, int rad) {
    for (int dy = -rad; dy <= rad; ++dy) {
        for (int dx = -rad; dx <= rad; ++dx) {
            if (dx * dx + dy * dy <= rad * rad) {
                SDL_RenderDrawPoint(r, cx + dx, cy + dy);
            }
        }
    }
}

void drawRing(SDL_Renderer* r, int cx, int cy, int rad, SDL_Color c) {
    setColor(r, c);
    for (int dy = -rad; dy <= rad; ++dy) {
        for (int dx = -rad; dx <= rad; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 >= (rad - 3) * (rad - 3) && d2 <= rad * rad) {
                SDL_RenderDrawPoint(r, cx + dx, cy + dy);
            }
        }
    }
}

void drawLine(SDL_Renderer* r, int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        SDL_RenderDrawPoint(r, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void renderText(SDL_Renderer* r, TTF_Font* f, const std::string& text, int x, int y, SDL_Color c) {
    if (!f || text.empty()) return;
    SDL_Surface* s = TTF_RenderText_Blended(f, text.c_str(), c);
    if (!s) return;
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_Rect dst{x, y, s->w, s->h};
    SDL_RenderCopy(r, t, nullptr, &dst);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(s);
}

void renderCentered(SDL_Renderer* r, TTF_Font* f, const std::string& text, int cx, int cy,
                    SDL_Color c) {
    if (!f || text.empty()) return;
    int tw = 0, th = 0;
    TTF_SizeUTF8(f, text.c_str(), &tw, &th);
    renderText(r, f, text, cx - tw / 2, cy - th / 2, c);
}

void fillBevel(SDL_Renderer* r, const SDL_Rect& rc, SDL_Color base, SDL_Color light,
               SDL_Color dark) {
    setColor(r, base);
    SDL_RenderFillRect(r, &rc);
    SDL_Rect b = {rc.x + 1, rc.y + 1, rc.w - 2, 1};
    setColor(r, light);
    SDL_RenderFillRect(r, &b);
    b = {rc.x + 1, rc.y + 1, 1, rc.h - 2};
    SDL_RenderFillRect(r, &b);
    b = {rc.x + 1, rc.y + rc.h - 2, rc.w - 2, 1};
    setColor(r, dark);
    SDL_RenderFillRect(r, &b);
    b = {rc.x + rc.w - 2, rc.y + 1, 1, rc.h - 2};
    SDL_RenderFillRect(r, &b);
}

void drawPips(SDL_Renderer* r, int cx, int cy, int value) {
    const int o = 7;
    const int s = 5;
    setColor(r, {30, 25, 20, 255});
    switch (value) {
        case 1: fillCircle(r, cx, cy, s); break;
        case 2: fillCircle(r, cx - o, cy - o, s); fillCircle(r, cx + o, cy + o, s); break;
        case 3: fillCircle(r, cx - o, cy - o, s); fillCircle(r, cx, cy, s); fillCircle(r, cx + o, cy + o, s); break;
        case 4: fillCircle(r, cx - o, cy - o, s); fillCircle(r, cx + o, cy - o, s); fillCircle(r, cx - o, cy + o, s); fillCircle(r, cx + o, cy + o, s); break;
        case 5: fillCircle(r, cx - o, cy - o, s); fillCircle(r, cx + o, cy - o, s); fillCircle(r, cx, cy, s); fillCircle(r, cx - o, cy + o, s); fillCircle(r, cx + o, cy + o, s); break;
        default: fillCircle(r, cx - o, cy - o, s); fillCircle(r, cx + o, cy - o, s); fillCircle(r, cx - o, cy, s); fillCircle(r, cx + o, cy, s); fillCircle(r, cx - o, cy + o, s); fillCircle(r, cx + o, cy + o, s); break;
    }
}

// Procedural vertical-grain wood texture covering the frame and the panel.
SDL_Texture* makeWoodTexture(SDL_Renderer* r, int w, int h) {
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return nullptr;
    Uint32* px = static_cast<Uint32*>(s->pixels);
    const int pw = s->pitch / 4;
    std::mt19937 rng(20260819);
    std::vector<double> ph(w), amp(w);
    for (int x = 0; x < w; ++x) {
        ph[x] = (rng() % 1000) / 200.0;
        amp[x] = 8.0 + (rng() % 140) / 12.0;
    }
    const auto cl = [](double v) {
        return static_cast<Uint32>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double f = std::sin(y * 0.045 + ph[x]) * amp[x] +
                             std::sin(x * 0.09 + y * 0.02) * 5.0;
            const Uint32 r_ = cl(152.0 + f);
            const Uint32 g_ = cl(118.0 + f * 0.9);
            const Uint32 b_ = cl(84.0 + f * 0.55);
            px[y * pw + x] = 0xFF000000 | (r_ << 16) | (g_ << 8) | b_;
        }
    }
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    return t;
}

void drawTexture(SDL_Renderer* r, SDL_Texture* t, int x, int y, int w, int h) {
    const SDL_Rect dst{x, y, w, h};
    SDL_RenderCopy(r, t, nullptr, &dst);
}

SDL_Rect cellRect(barricade::Point p) {
    return {kBoardX + p.x * kCell, kBoardY + p.y * kCell, kCell, kCell};
}

// Animated pawn movement: the last applied move is interpolated.
struct Anim {
    bool active = false;
    int player = -1;
    int pawn = -1;
    barricade::Point from;
    barricade::Point to;
    Uint32 start = 0;
    Uint32 dur = 280;
};

barricade::Point pawnPixel(const barricade::Game& game, const Anim& anim, int p, int m) {
    const barricade::Point pos = game.pawnPos(p, m);
    int px = kBoardX + pos.x * kCell;
    int py = kBoardY + pos.y * kCell;
    if (anim.active && anim.player == p && anim.pawn == m) {
        const Uint32 now = SDL_GetTicks();
        double t = static_cast<double>(now - anim.start) / static_cast<double>(anim.dur);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const double ease = t * t * (3.0 - 2.0 * t);
        px = kBoardX + static_cast<int>(std::lround((anim.from.x + (anim.to.x - anim.from.x) * ease) * kCell));
        py = kBoardY + static_cast<int>(std::lround((anim.from.y + (anim.to.y - anim.from.y) * ease) * kCell));
    }
    return {px, py};
}

// FNV-1a signature of the whole position: pawns, barricades, current player,
// die and pending-barricade flag. Used to detect when a stored analysis no
// longer matches the board.
uint64_t posSig(const barricade::Game& g) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    for (int p = 0; p < g.playerCount(); ++p) {
        for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
            const barricade::Point pos = g.pawnPos(p, m);
            mix(static_cast<uint64_t>(pos.x) * 31u + static_cast<uint64_t>(pos.y));
        }
    }
    for (const barricade::Point& b : g.barricades()) {
        if (b.x >= 0) mix(static_cast<uint64_t>(b.x) * 31u + static_cast<uint64_t>(b.y) + 1u);
    }
    mix(static_cast<uint64_t>(g.currentPlayer()));
    mix(static_cast<uint64_t>(g.dice()));
    mix(g.pendingBarricade() ? 1u : 0u);
    return h;
}

// Own pawn (on track or in base) at `cell` for player `cur`, or -1.
int pawnAtCell(const barricade::Game& game, int cur, barricade::Point cell) {
    const int id = game.pawnAt(cell);
    if (id >= 0 && id / barricade::kPawnsPerPlayer == cur) {
        return id % barricade::kPawnsPerPlayer;
    }
    for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
        if (game.pawnInBase(cur, m) && cell == barricade::baseCell(cur, m)) return m;
    }
    return -1;
}

void drawReach(SDL_Renderer* r, barricade::Point p, SDL_Color fill, int alpha) {
    const SDL_Rect rc = cellRect(p);
    setColor(r, {fill.r, fill.g, fill.b, static_cast<Uint8>(alpha)});
    const SDL_Rect inner{rc.x + 7, rc.y + 7, kCell - 14, kCell - 14};
    SDL_RenderFillRect(r, &inner);
    setColor(r, {fill.r, fill.g, fill.b, 255});
    SDL_RenderDrawRect(r, &inner);
}

void drawFadeRing(SDL_Renderer* r, barricade::Point cell, Uint32 at, Uint32 dur, SDL_Color c) {
    const Uint32 now = SDL_GetTicks();
    if (now - at >= dur) return;
    const double t = static_cast<double>(now - at) / static_cast<double>(dur);
    const SDL_Rect rc = cellRect(cell);
    const int cx = rc.x + kCell / 2;
    const int cy = rc.y + kCell / 2;
    const int rad = kCell / 2 - 6 + static_cast<int>(10.0 * t);
    setColor(r, {c.r, c.g, c.b, static_cast<Uint8>(255 * (1.0 - t))});
    for (int dy = -rad; dy <= rad; ++dy) {
        for (int dx = -rad; dx <= rad; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 >= (rad - 2) * (rad - 2) && d2 <= rad * rad) {
                SDL_RenderDrawPoint(r, cx + dx, cy + dy);
            }
        }
    }
}

// Arrow with a numbered badge at its midpoint (badge number = advice rank).
// The shaft is drawn with a dark outline and a filled arrowhead so it stays
// readable over the wooden board.
void drawArrow(SDL_Renderer* r, TTF_Font* font, barricade::Point from, barricade::Point to,
               SDL_Color c, int num) {
    const SDL_Rect a = cellRect(from), b = cellRect(to);
    const int x0 = a.x + kCell / 2, y0 = a.y + kCell / 2;
    const int x1 = b.x + kCell / 2, y1 = b.y + kCell / 2;
    const double ang = std::atan2(static_cast<double>(y1 - y0), static_cast<double>(x1 - x0));
    const double ux = std::cos(ang), uy = std::sin(ang);
    const auto thickLine = [&](SDL_Color col, int w) {
        setColor(r, col);
        for (int k = -w; k <= w; ++k) {
            const double ox = -uy * k, oy = ux * k;
            drawLine(r, static_cast<int>(x0 + ox), static_cast<int>(y0 + oy),
                     static_cast<int>(x1 + ox - ux * 9), static_cast<int>(y1 + oy - uy * 9));
        }
    };
    thickLine({30, 24, 16, 230}, 2);  // dark outline
    thickLine(c, 1);                  // colored shaft
    // filled arrowhead (outlined, then bright)
    const int hx = static_cast<int>(x1 - ux * 3), hy = static_cast<int>(y1 - uy * 3);
    for (int k = -1; k <= 1; k += 2) {
        const double a2 = ang + k * 0.5;
        setColor(r, {30, 24, 16, 230});
        drawLine(r, hx, hy, hx - static_cast<int>(17 * std::cos(a2)),
                 hy - static_cast<int>(17 * std::sin(a2)));
    }
    for (int k = -1; k <= 1; k += 2) {
        const double a2 = ang + k * 0.44;
        setColor(r, c);
        drawLine(r, hx, hy, hx - static_cast<int>(14 * std::cos(a2)),
                 hy - static_cast<int>(14 * std::sin(a2)));
    }
    // number badge on a dark disc with a colored ring
    const double len = std::hypot(static_cast<double>(x1 - x0), static_cast<double>(y1 - y0));
    if (len > 20.0) {
        const double px = -uy, py = ux;
        const int mx = static_cast<int>(x0 + (x1 - x0) * 0.45 + px * 16);
        const int my = static_cast<int>(y0 + (y1 - y0) * 0.45 + py * 16);
        setColor(r, {18, 14, 9, 255});
        fillCircle(r, mx, my, 13);
        drawRing(r, mx, my, 13, {40, 32, 22, 255});
        drawRing(r, mx, my, 11, c);
        char buf[4];
        std::snprintf(buf, sizeof buf, "%d", num);
        renderCentered(r, font, buf, mx, my, {255, 255, 255, 255});
    }
}

// One candidate action with its end-game scenario: the MCTS recommendation
// (move or barricade placement) plus each player's simulated win share after
// playing it.
struct ScenarioView {
    barricade::AIMove move;          // pawn/dest (mode 1)
    barricade::Point cell{-1, -1};   // barricade placement (mode 2)
    bool isPlacement = false;
    double value = 0.0;              // MCTS value or barricade score
    long long visits = 0;            // MCTS visits (mode 1)
    std::vector<double> shares;      // per-player end-game win shares
    long long sims = 0;              // end-game games played for the shares
};

// Computer advice shown during a human turn. An analysis runs automatically
// in the background for the current position; the overlay (arrows / placement
// badges + panel list) is only revealed when the user toggles it with C and is
// driven by the live analysis engine.
struct AdviceView {
    bool show = false;  // overlay visible (user toggled)
    bool busy = false;  // auto-analysis in progress
    int player = -1;    // player the analysis was computed for
    std::vector<barricade::MctsRecommendation> moves;    // auto mode
    std::vector<barricade::BarricadeRecommendation> placements;  // auto mode
    std::vector<ScenarioView> scenarios;                 // engine mode
    double bestGain = -1.0;  // current player's win% of the best move (-1 unknown)
    long long bestSims = 0;  // games played for bestGain
    bool engineActive = false;
    double engineStepMs = 0.0;
    double engineBaseMs = 500.0;
    long long engineStep = 0;
};

// Live analysis engine: one persistent thread that waits for a work request
// and then keeps deepening (doubling the step budget) until stopped or the
// position changes, so the UI thread is never blocked (no join while running).
struct AnalysisEngine {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::vector<ScenarioView> scenarios;   // last completed step (guarded by m)
    std::atomic<bool> exit{false};
    std::atomic<bool> want{false};         // work requested
    std::atomic<bool> running{false};      // a session is deepening
    std::atomic<bool> requestStop{true};   // stop the current session
    std::atomic<bool> restart{false};      // reset the budget after this step
    std::atomic<double> baseMs{500.0};     // user-set base budget
    std::atomic<double> stepMs{0.0};       // budget of the last step
    std::atomic<long long> step{0};        // steps completed in the session
    uint64_t sig = 0;                      // position signature (guarded by m)
    barricade::Game snapshot{4};           // guarded by m
    int mode = 1;                          // guarded by m
};

void engineLoop(AnalysisEngine& E) {
    std::unique_lock<std::mutex> lk(E.m);
    while (true) {
        E.cv.wait(lk, [&] { return E.exit.load() || E.want.load(); });
        if (E.exit.load()) return;
        E.want.store(false);
        const barricade::Game snapshot = E.snapshot;
        const int mode = E.mode;
        lk.unlock();
        E.running.store(true);
        E.requestStop.store(false);
        double budget = E.baseMs.load();
        E.step.store(0);
        while (!E.exit.load() && !E.requestStop.load() && !E.want.load()) {
            // Both modes search with the MCTS: a move node (mode 1) returns the
            // best pawn moves, a placement node (mode 2, a captured barricade
            // pending) returns the best placements. Each is then scored by a
            // 2000-game position simulation.
            std::vector<ScenarioView> sc;
            const auto dist = barricade::dynamicGoalDist(snapshot);
            const auto recs = barricade::mctsRecommendations(
                snapshot, snapshot.currentPlayer(), static_cast<int>(budget));
            for (const auto& rec : recs) {
                barricade::Game child = snapshot;
                if (mode == 2) {
                    if (rec.move.pawn >= 0 || rec.move.dest.x < 0) continue;
                    if (!child.placeBarricadeFast(rec.move.dest)) continue;
                    ScenarioView sv;
                    sv.cell = rec.move.dest;
                    sv.isPlacement = true;
                    sv.value = rec.value;
                    sv.visits = rec.visits;
                    sv.shares = barricade::simulateWinChances(child, 2000);
                    sv.sims = 2000;
                    sc.push_back(std::move(sv));
                } else {
                    if (rec.move.pawn < 0) continue;
                    if (!child.movePawn(snapshot.currentPlayer(), rec.move.pawn,
                                        rec.move.dest)) {
                        continue;
                    }
                    // A move that captured a barricade brings a placement with
                    // it; estimate its value with the same dynamic distances
                    // the search uses.
                    if (child.pendingBarricade()) {
                        child.placeBarricadeFast(barricade::cheapBarricadePlacement(child, dist));
                    }
                    ScenarioView sv;
                    sv.move = rec.move;
                    sv.value = rec.value;
                    sv.visits = rec.visits;
                    sv.shares = barricade::simulateWinChances(child, 2000);
                    sv.sims = 2000;
                    sc.push_back(std::move(sv));
                }
            }
            if (mode == 2) {
                // Also offer the exact BFS scorer's best cells: the MCTS
                // placement branch is capped at kBarricadeBranch cells, and a
                // good manual block could rank lower in that shortlist while
                // still scoring well when simulated.
                const auto exact = barricade::barricadeRecommendations(snapshot, 6);
                for (const auto& br : exact) {
                    bool dup = false;
                    for (const ScenarioView& sv : sc) {
                        if (sv.isPlacement && sv.cell == br.cell) {
                            dup = true;
                            break;
                        }
                    }
                    if (dup) continue;
                    barricade::Game child = snapshot;
                    if (!child.placeBarricadeFast(br.cell)) continue;
                    ScenarioView sv;
                    sv.cell = br.cell;
                    sv.isPlacement = true;
                    sv.value = 0.0;
                    sv.visits = 0;
                    sv.shares = barricade::simulateWinChances(child, 2000);
                    sv.sims = 2000;
                    sc.push_back(std::move(sv));
                }
            }
            // Rank the scenarios by the current player's simulated win share:
            // the MCTS visits and the 2000-game position simulation are two
            // different estimators, and the share is the one the UI displays.
            {
                const int cp = snapshot.currentPlayer();
                std::sort(sc.begin(), sc.end(), [cp](const ScenarioView& a, const ScenarioView& b) {
                    const double as = !a.shares.empty() ? a.shares[cp] : -1.0;
                    const double bs = !b.shares.empty() ? b.shares[cp] : -1.0;
                    return as > bs;
                });
            }
            {
                std::lock_guard<std::mutex> guard(E.m);
                E.scenarios = std::move(sc);
                E.stepMs.store(budget);
            }
            E.step.fetch_add(1);
            if (E.exit.load() || E.requestStop.load() || E.want.load()) break;
            if (E.restart.load()) {
                E.restart.store(false);
                budget = E.baseMs.load();
            } else {
                budget = std::min(budget * 2.0, 2000.0);
            }
        }
        E.running.store(false);
        E.requestStop.store(false);
        lk.lock();
    }
}

// Controls of the live-analysis overlay header.
struct EngineControls {
    SDL_Rect minus, plus, relancer, stop;
};
EngineControls engineControlRects(const SDL_Rect& box) {
    const int y = box.y + 28;
    EngineControls c;
    c.minus = {box.x + 10, y, 24, 22};
    c.plus = {box.x + 98, y, 24, 22};
    c.relancer = {box.x + 130, y, 72, 22};
    c.stop = {box.x + 208, y, 88, 22};
    return c;
}

void drawSmallButton(SDL_Renderer* r, TTF_Font* small, int x, int y, int w, int h,
                     const std::string& label, bool armed) {
    fillBevel(r, {x, y, w, h}, armed ? SDL_Color{90, 140, 60, 255} : SDL_Color{62, 52, 40, 255},
              {130, 100, 80, 255}, {40, 34, 26, 255});
    renderCentered(r, small, label, x + w / 2, y + h / 2 + 1, kTextColor);
}

void drawAdviceOverlay(SDL_Renderer* r, TTF_Font* font, const barricade::Game& game,
                       const AdviceView& advice, bool match) {
    if (!advice.show || advice.busy || !match) return;
    const size_t n = std::min<size_t>(advice.scenarios.size(), 4);
    const bool placementMode = !advice.scenarios.empty() && advice.scenarios[0].isPlacement;
    if (placementMode) {
        // numbered placement badges on the recommended cells
        for (size_t i = 0; i < n; ++i) {
            const auto& sv = advice.scenarios[i];
            if (!sv.isPlacement) continue;
            const SDL_Rect rc = cellRect(sv.cell);
            const int cx = rc.x + kCell / 2, cy = rc.y + kCell / 2;
            const SDL_Color c = kAdvicePalette[i % 8];
            drawRing(r, cx, cy, kCell / 2 - 4, c);
            if (i == 0) {
                setColor(r, {255, 255, 255, 200});
                SDL_RenderDrawRect(r, &rc);
            }
            setColor(r, {24, 20, 14, 230});
            fillCircle(r, cx, cy, 11);
            drawRing(r, cx, cy, 11, c);
            char buf[4];
            std::snprintf(buf, sizeof buf, "%d", static_cast<int>(i) + 1);
            renderCentered(r, font, buf, cx, cy, {255, 255, 255, 255});
        }
        return;
    }
    // move advice: numbered arrows
    const int cur = game.currentPlayer();
    for (size_t i = 0; i < n; ++i) {
        const auto& sv = advice.scenarios[i];
        if (sv.isPlacement) continue;
        drawArrow(r, font, game.pawnPos(cur, sv.move.pawn), sv.move.dest, kAdvicePalette[i % 8],
                  static_cast<int>(i) + 1);
        if (i == 0) {
            const SDL_Rect rc = cellRect(sv.move.dest);
            setColor(r, {255, 255, 255, 200});
            SDL_RenderDrawRect(r, &rc);
        }
    }
}

// Right-panel layout, shared between drawing and hit-testing.
struct PanelLayout {
    SDL_Rect adviseBtn;
    SDL_Rect adviceBox;
    SDL_Rect adviceClose;
    SDL_Rect b1, b2, b3, b4;   // recherche, gains, heuristiques, strategie
    SDL_Rect simBtn;           // Pause / Lancer in the gains box
};

PanelLayout panelLayout(bool expandedOverlay) {
    PanelLayout L;
    const int cx = kPanelX + kPanelPad;
    const int cw = kPanelW - 2 * kPanelPad;
    L.adviseBtn = {cx, kStatusH + 50, cw, 34};
    const int y1 = kStatusH + 96;
    const int y2 = y1 + 140 + 12;
    const int y3 = y2 + 220 + 12;
    const int y4 = y3 + 120 + 12;
    L.b1 = {cx, y1, cw, 140};
    L.b2 = {cx, y2, cw, 220};
    L.b3 = {cx, y3, cw, 120};
    L.b4 = {cx, y4, cw, 70};
    L.simBtn = {L.b2.x + cw - 66, L.b2.y + 120, 58, 20};
    if (expandedOverlay) {
        // the conseils box grows over the params/strategy boxes (the gains box
        // with its pause button stays visible)
        L.adviceBox = {cx, y3, cw, kWinH - 40 - y3};
    } else {
        // anchored to the bottom so it never collides with the strategy box
        L.adviceBox = {cx, kWinH - 40 - 150, cw, 150};
    }
    L.adviceClose = {L.adviceBox.x + L.adviceBox.w - 26, L.adviceBox.y + 7, 20, 20};
    return L;
}

void drawBox(SDL_Renderer* r, const SDL_Rect& box, const std::string& title, TTF_Font* font) {
    setColor(r, {24, 19, 13, 205});
    SDL_RenderFillRect(r, &box);
    setColor(r, {120, 104, 82, 255});
    SDL_RenderDrawRect(r, &box);
    renderText(r, font, title, box.x + 10, box.y + 5, kGold);
}

std::string groupThousands(long long v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(i, 1, ' ');
    return s;
}

std::string fmt1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    std::string s = b;
    for (char& ch : s) if (ch == '.') ch = ',';
    return s;
}

std::string fmtMs(double ms) {
    char b[32];
    if (ms >= 1000.0) {
        std::snprintf(b, sizeof b, "%.0f s", ms / 1000.0);
    } else {
        std::snprintf(b, sizeof b, "%.1f s", ms / 1000.0);
        for (char& ch : b) if (ch == '.') ch = ',';
    }
    return b;
}

// MCTS value -> displayed "chance of winning" calibrated for the player count:
// baseline 1/N at value 0, 100% at value +1. (For 2 players this is exactly
// (1+v)/2; for 4 players a +0,54 lead reads ~66% instead of the 77% the
// duelist formula gave.)
int movePct(const barricade::Game& game, double value) {
    const double n = game.playerCount();
    double p = 1.0 / n + value * (1.0 - 1.0 / n);
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;
    return static_cast<int>(std::lround(p * 100.0));
}

// Same mapping applied to the last-search win probability (which is (1+v)/2).
int deepPct(const barricade::Game& game, const barricade::SearchInfo& info) {
    return movePct(game, info.winProb * 2.0 - 1.0);
}

void drawPanel(SDL_Renderer* r, TTF_Font* font, TTF_Font* small, const barricade::Game& game,
               const AdviceView& advice, const barricade::SearchInfo& info,
               const std::string& lastAiText, bool humanTurn, bool match, int analysisMode,
               long long simGames, const std::vector<double>& simShares, bool simPaused,
               const std::vector<std::vector<double>>& winHist) {
    const int cx = kPanelX + kPanelPad;
    const int cw = kPanelW - 2 * kPanelPad;
    const PanelLayout L = panelLayout(advice.show);

    // Panel title
    renderText(r, font, "Analyse IA", cx, kStatusH + 12, kTextColor);
    renderText(r, small, "MCTS root-parallel", cx, kStatusH + 32, kTextDim);

    // "Calculer un conseil" button (human turn only)
    if (humanTurn && !game.isOver()) {
        const bool armed = advice.show || advice.busy;
        fillBevel(r, L.adviseBtn, armed ? SDL_Color{90, 140, 60, 255} : SDL_Color{62, 52, 40, 255},
                  {130, 100, 80, 255}, {40, 34, 26, 255});
        renderCentered(r, font, "Calculer un conseil  (C)", L.adviseBtn.x + L.adviseBtn.w / 2,
                       L.adviseBtn.y + L.adviseBtn.h / 2 + 1, kTextColor);
    }

    // Recherche MCTS box
    const SDL_Rect b1 = L.b1;
    drawBox(r, b1, "Recherche (MCTS)", font);
    int yy = b1.y + 26;
    const auto row = [&](const std::string& label, const std::string& value, SDL_Color vc) {
        renderText(r, small, label, b1.x + 10, yy, kTextDim);
        renderText(r, small, value, b1.x + 130, yy, vc);
        yy += 18;
    };
    if (info.iterations > 0) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%d ms", 500);
        row("Budget", buf, kTextColor);
        row("Simulations", groupThousands(info.iterations), kTextColor);
        row("Noeuds", groupThousands(info.nodes), kTextColor);
        const double mits = info.elapsedMs > 0.0 ? info.iterations / info.elapsedMs / 1000.0 : 0.0;
        std::snprintf(buf, sizeof buf, "%s M/s", fmt1(mits).c_str());
        row("Vitesse", buf, kTextColor);
        std::snprintf(buf, sizeof buf, "%s", fmt1(info.avgDepth).c_str());
        row("Profondeur moy.", buf, kTextColor);
        // real end-game estimate (from the position simulation), coherent with
        // the bars below; falls back to the short-horizon MCTS value.
        if (simGames > 0 && !simShares.empty()) {
            std::snprintf(buf, sizeof buf, "%d%%",
                          static_cast<int>(std::lround(simShares[game.currentPlayer()] * 100.0)));
        } else {
            std::snprintf(buf, sizeof buf, "%d%%", deepPct(game, info));
        }
        row("Chance de gain", buf, kGold);
    } else {
        row("Budget", "500 ms", kTextColor);
        row("", "aucune recherche", kTextDim);
    }

    // Evaluation des gains box: static whole-army heuristic vs end-game simulation
    const SDL_Rect b2 = L.b2;
    drawBox(r, b2, "Gains : statique vs simule", font);
    const std::vector<double> chances = barricade::winChances(game);
    yy = b2.y + 26;
    char buf[64];
    for (int p = 0; p < game.playerCount(); ++p) {
        const int statBarX = b2.x + 76, statBarW = 44;
        const int simBarX = b2.x + 182, simBarW = 70;
        const int yb = yy + 1, barH = 12;
        renderText(r, small, kPlayerNames[p], b2.x + 8, yy, kPlayerColors[p]);
        renderText(r, small, "stat", b2.x + 52, yy, kTextDim);
        setColor(r, {15, 12, 8, 255});
        const SDL_Rect sbg{statBarX, yb, statBarW, barH};
        SDL_RenderFillRect(r, &sbg);
        const int sfill = static_cast<int>(statBarW * chances[p]);
        if (sfill > 0) {
            setColor(r, kPlayerColors[p]);
            const SDL_Rect sfg{statBarX + 1, yb + 1, sfill - 2, barH - 2};
            SDL_RenderFillRect(r, &sfg);
        }
        setColor(r, {255, 255, 255, 60});
        SDL_RenderDrawRect(r, &sbg);
        std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(chances[p] * 100.0)));
        renderText(r, small, buf, statBarX + statBarW + 4, yy, kTextColor);
        renderText(r, small, "sim", b2.x + 156, yy, kTextDim);
        const double sim = simShares.empty() ? 0.0 : simShares[p];
        setColor(r, {15, 12, 8, 255});
        const SDL_Rect ibg{simBarX, yb, simBarW, barH};
        SDL_RenderFillRect(r, &ibg);
        const int ifill = static_cast<int>(simBarW * sim);
        if (ifill > 0) {
            setColor(r, kPlayerColors[p]);
            const SDL_Rect ifg{simBarX + 1, yb + 1, ifill - 2, barH - 2};
            SDL_RenderFillRect(r, &ifg);
        }
        setColor(r, {255, 255, 255, 60});
        SDL_RenderDrawRect(r, &ibg);
std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(sim * 100.0)));
        renderText(r, small, buf, simBarX + simBarW + 4, yy, kTextColor);
        yy += 24;
    }
    // simulation counter + pause/resume toggle, on the same line
    if (simPaused) {
        std::snprintf(buf, sizeof buf, "Simulation : PAUSEE");
    } else if (simGames > 0) {
        std::snprintf(buf, sizeof buf, "Simulation : %s parties", groupThousands(simGames).c_str());
    } else {
        std::snprintf(buf, sizeof buf, "Simulation : en cours...");
    }
    renderText(r, small, buf, b2.x + 10, b2.y + 120, kGold);
    drawSmallButton(r, small, L.simBtn.x, L.simBtn.y, L.simBtn.w, L.simBtn.h,
                    simPaused ? "Lancer" : "Pause", false);

    // Evolution of the static win chances over the game (one point per move)
    renderText(r, small, "Evolution des chances", b2.x + 10, b2.y + 140, kTextDim);
    const SDL_Rect plot{b2.x + 10, b2.y + 154, cw - 20, 60};
    setColor(r, {15, 12, 8, 255});
    SDL_RenderFillRect(r, &plot);
    setColor(r, {255, 255, 255, 60});
    SDL_RenderDrawRect(r, &plot);
    const int histN = static_cast<int>(winHist.size());
    if (histN >= 2) {
        // Auto-scale the y axis to the observed range: a well-matched game
        // keeps the normalized chances near 50/50, and on a fixed [0,1] scale
        // the curve would be a barely visible flat band.
        double vmin = 1.0, vmax = 0.0;
        for (int i = 0; i < histN; ++i) {
            for (int p = 0; p < game.playerCount() && p < static_cast<int>(winHist[i].size()); ++p) {
                vmin = std::min(vmin, winHist[i][p]);
                vmax = std::max(vmax, winHist[i][p]);
            }
        }
        const double pad = std::max(0.01, (vmax - vmin) * 0.2);
        vmin = std::max(0.0, vmin - pad);
        vmax = std::min(1.0, vmax + pad);
        const double span = std::max(vmax - vmin, 1e-6);
        const auto yof = [&](double v) {
            return plot.y + plot.h - 1 - static_cast<int>((v - vmin) / span * (plot.h - 2));
        };
        // dashed 0.5 reference line when it falls inside the scaled range
        if (vmin < 0.5 && vmax > 0.5) {
            setColor(r, {255, 255, 255, 45});
            const int y05 = yof(0.5);
            for (int x = plot.x; x < plot.x + plot.w; x += 4) SDL_RenderDrawPoint(r, x, y05);
        }
        for (int p = 0; p < game.playerCount(); ++p) {
            setColor(r, kPlayerColors[p]);
            // 1px horizontal offset per player so coincident curves (the shares
            // are complementary and can sit exactly on each other) stay visible.
            const int pxo = p & 1;
            int px = -1, py = -1;
            for (int i = 0; i < histN; ++i) {
                const int x = std::min(plot.x + (i * (plot.w - 1)) / std::max(histN - 1, 1) + pxo,
                                       plot.x + plot.w - 1);
                const double v = (i < static_cast<int>(winHist[i].size())) ? winHist[i][p] : 0.0;
                const int y = yof(v);
                if (px >= 0) drawLine(r, px, py, x, y);
                px = x;
                py = y;
            }
            // sample dots: visible even when a single move jumps the curve
            for (int i = 0; i < histN; ++i) {
                const int x = std::min(plot.x + (i * (plot.w - 1)) / std::max(histN - 1, 1) + pxo,
                                       plot.x + plot.w - 1);
                const double v = (i < static_cast<int>(winHist[i].size())) ? winHist[i][p] : 0.0;
                const int y = yof(v);
                SDL_RenderDrawPoint(r, x, y);
                SDL_RenderDrawPoint(r, x + 1, y);
                SDL_RenderDrawPoint(r, x, y + 1);
            }
        }
    }

    // Heuristiques (parametres) box: live inputs + scoring constants
    const std::vector<std::vector<int>> army = [&]() {
        std::vector<std::vector<int>> v(game.playerCount());
        for (int p = 0; p < game.playerCount(); ++p) v[p] = barricade::playerArmyDistances(game, p);
        return v;
    }();
    const SDL_Rect b3 = L.b3;
    drawBox(r, b3, "Heuristiques (parametres)", font);
    yy = b3.y + 26;
    std::snprintf(buf, sizeof buf, "Dist. but :");
    for (int p = 0; p < game.playerCount(); ++p) {
        int sum = 0, cnt = 0;
        for (int d : army[p]) {
            if (d > 999) continue;
            sum += d;
            ++cnt;
        }
        if (cnt == 0) {
            std::snprintf(buf + std::strlen(buf), sizeof buf - std::strlen(buf), " %s:-",
                          kPlayerShorts[p]);
        } else {
            std::snprintf(buf + std::strlen(buf), sizeof buf - std::strlen(buf), " %s:%d",
                          kPlayerShorts[p], (sum + cnt / 2) / cnt);
        }
    }
    renderText(r, small, buf, b3.x + 10, yy, kTextColor);
    yy += 18;
    const auto param = [&](const std::string& text) {
        renderText(r, small, text, b3.x + 10, yy, kTextDim);
        yy += 18;
    };
    param("Eval = min/10 + armee x1,2 + sortis + capture");
    param("Bloc : adv +60 x2 | moi -1000 | detour");
    param("Capture +1500 | bloc +200 | risque -250");
    param("Rollout 10 | UCT 1,41 | BFS bloc +8 | cap/bloc");

    // Strategie box
    const SDL_Rect b4 = L.b4;
    drawBox(r, b4, "Strategie / fin de partie", font);
    std::string strategy = "-";
    if (match && !advice.busy) {
        char line[64];
        if (analysisMode == 2 && !advice.scenarios.empty()) {
            std::snprintf(line, sizeof line, "Bloc -> (%d,%d)",
                          advice.scenarios[0].cell.x, advice.scenarios[0].cell.y);
            strategy = line;
        } else if (!advice.scenarios.empty()) {
            const auto& sv = advice.scenarios[0];
            const int pct = !sv.shares.empty()
                                ? static_cast<int>(std::lround(sv.shares[game.currentPlayer()] * 100.0))
                                : -1;
            if (pct >= 0) {
                std::snprintf(line, sizeof line, "P%d -> (%d,%d) : %d%%", sv.move.pawn + 1,
                              sv.move.dest.x, sv.move.dest.y, pct);
            } else {
                std::snprintf(line, sizeof line, "P%d -> (%d,%d)", sv.move.pawn + 1,
                              sv.move.dest.x, sv.move.dest.y);
            }
            strategy = line;
        } else if (analysisMode == 2 && !advice.placements.empty()) {
            std::snprintf(line, sizeof line, "Bloc -> (%d,%d)",
                          advice.placements[0].cell.x, advice.placements[0].cell.y);
            strategy = line;
        } else if (!advice.moves.empty()) {
            const auto& rec = advice.moves[0];
            const int pct = advice.bestGain >= 0.0
                                ? static_cast<int>(std::lround(advice.bestGain * 100.0))
                                : -1;
            if (pct >= 0) {
                std::snprintf(line, sizeof line, "P%d -> (%d,%d) : %d%%", rec.move.pawn + 1,
                              rec.move.dest.x, rec.move.dest.y, pct);
            } else {
                std::snprintf(line, sizeof line, "P%d -> (%d,%d)", rec.move.pawn + 1,
                              rec.move.dest.x, rec.move.dest.y);
            }
            strategy = line;
        }
    } else if (!lastAiText.empty()) {
        strategy = lastAiText;
    }
    renderText(r, small, strategy, b4.x + 10, b4.y + 28, kTextColor);
    const std::vector<int> prog = barricade::playerProgress(game);
    int minP = -1, minD = 1000000;
    for (int p = 0; p < game.playerCount(); ++p) {
        if (prog[p] < minD) {
            minD = prog[p];
            minP = p;
        }
    }
    if (minD <= 3) {
        char line[64];
        std::snprintf(line, sizeof line, "%s proche de la victoire !", kPlayerNames[minP]);
        renderText(r, small, line, b4.x + 10, b4.y + 52, kGold);
    } else {
        renderText(r, small, "Course vers le but", b4.x + 10, b4.y + 52, kTextDim);
    }

    // Conseils box (opaque: it covers the params/strategy boxes while shown)
    if (advice.show) {
        const SDL_Rect box = L.adviceBox;
        setColor(r, {24, 19, 13, 255});
        SDL_RenderFillRect(r, &box);
        setColor(r, {120, 104, 82, 255});
        SDL_RenderDrawRect(r, &box);
        renderText(r, font, "Conseils de l'IA", box.x + 10, box.y + 5, kGold);
        // close button (X)
        setColor(r, {140, 60, 60, 255});
        SDL_RenderFillRect(r, &L.adviceClose);
        setColor(r, {60, 20, 20, 255});
        SDL_RenderDrawRect(r, &L.adviceClose);
        drawLine(r, L.adviceClose.x + 5, L.adviceClose.y + 5, L.adviceClose.x + L.adviceClose.w - 5,
                 L.adviceClose.y + L.adviceClose.h - 5);
        drawLine(r, L.adviceClose.x + L.adviceClose.w - 5, L.adviceClose.y + 5, L.adviceClose.x + 5,
                 L.adviceClose.y + L.adviceClose.h - 5);
        // engine controls: budget - / value / + / Relancer / Arreter|Reprendre
        const EngineControls c = engineControlRects(box);
        drawSmallButton(r, small, c.minus.x, c.minus.y, c.minus.w, c.minus.h, "-", false);
        drawSmallButton(r, small, c.plus.x, c.plus.y, c.plus.w, c.plus.h, "+", false);
        renderCentered(r, small, fmtMs(advice.engineBaseMs), box.x + 66, c.minus.y + 11, kGold);
        drawSmallButton(r, small, c.relancer.x, c.relancer.y, c.relancer.w, c.relancer.h,
                        "Relancer", false);
        drawSmallButton(r, small, c.stop.x, c.stop.y, c.stop.w, c.stop.h,
                        advice.engineActive ? "Arreter" : "Reprendre", advice.engineActive);
        // status line
        char status[96];
        if (advice.engineActive) {
            std::snprintf(status, sizeof status, "Analyse en direct : etape %lld (%s)",
                          static_cast<long long>(advice.engineStep), fmtMs(advice.engineStepMs).c_str());
        } else {
            std::snprintf(status, sizeof status, "Analyse arretee - Relancer pour reprendre");
        }
        renderText(r, small, status, box.x + 10, box.y + 58, kTextDim);
        // scenarios: action + sim count, then per-player win shares
        const size_t n = std::min<size_t>(advice.scenarios.size(), 4);
        yy = box.y + 82;
        for (size_t i = 0; i < n; ++i) {
            const auto& sv = advice.scenarios[i];
            char line[64];
            if (sv.isPlacement) {
                std::snprintf(line, sizeof line, "#%d  Bloc -> (%d,%d)", static_cast<int>(i) + 1,
                              sv.cell.x, sv.cell.y);
            } else {
                std::snprintf(line, sizeof line, "#%d  P%d -> (%d,%d)", static_cast<int>(i) + 1,
                              sv.move.pawn + 1, sv.move.dest.x, sv.move.dest.y);
            }
            setColor(r, kAdvicePalette[i % 8]);
            fillCircle(r, box.x + 18, yy + 8, 6);
            renderText(r, small, line, box.x + 30, yy, kTextColor);
            // Coherent with the shares shown below: both come from the 2000-game
            // position simulation (the MCTS visits are a different estimator).
            const long long sims = sv.sims;
            std::snprintf(line, sizeof line, "%s sims", groupThousands(sims).c_str());
            renderText(r, small, line, box.x + box.w - 118, yy, kTextDim);
            // per-player simulated win shares for this move
            yy += 19;
            int x = box.x + 30;
            for (int p = 0; p < game.playerCount(); ++p) {
                const int sh = !sv.shares.empty()
                                   ? static_cast<int>(std::lround(sv.shares[p] * 100.0))
                                   : -1;
                char t[24];
                if (sh < 0) {
                    std::snprintf(t, sizeof t, "%s ?", kPlayerShorts[p]);
                } else {
                    std::snprintf(t, sizeof t, "%s %d%%", kPlayerShorts[p], sh);
                }
                renderText(r, small, t, x, yy, p == game.currentPlayer() ? kGold : kPlayerColors[p]);
                x += 46;
            }
            yy += 17;
        }
        if (advice.scenarios.empty()) {
            renderText(r, small, "Recherche en cours...", box.x + 10, yy, kTextDim);
        }
    }

    // controls hint
    renderText(r, small, "C : conseil   M : menu   R : rejouer   Echap : quitter", cx,
               kWinH - 26, kTextDim);
}

void drawBoard(SDL_Renderer* r, TTF_Font* font, TTF_Font* small, SDL_Texture* wood,
               const barricade::Game& game, int selectedPawn, int hoverPawn, bool showHints,
               const std::string& msg, const Anim& anim, barricade::Point lastMove, Uint32 lastMoveAt,
               const AdviceView& advice, bool adviceMatch) {
    // wood table covering everything below the status bar
    drawTexture(r, wood, 0, kStatusH, kWinW, kWinH - kStatusH);

    const int cur = game.currentPlayer();

    // board cells
    for (int y = 0; y < barricade::kRows; ++y) {
        for (int x = 0; x < barricade::kCols; ++x) {
            const SDL_Rect rc = cellRect({x, y});
            if (barricade::isTrackCell(x, y)) {
                if (barricade::isGoalCell(x, y)) {
                    setColor(r, {248, 240, 220, 255});
                    SDL_RenderFillRect(r, &rc);
                    setColor(r, kGridColor);
                    SDL_RenderDrawRect(r, &rc);
                } else {
                    // slightly varied wood tile
                    const int v = ((x * 31 + y * 17) % 9) - 4;
                    const SDL_Color base = lighten({196, 168, 124, 255}, v);
                    fillBevel(r, rc, base, lighten(base, 26), lighten(base, -22));
                }
            } else if (barricade::baseOwner(x, y) >= 0) {
                const SDL_Color c = kPlayerColors[barricade::baseOwner(x, y)];
                const SDL_Color base = {static_cast<Uint8>(c.r / 2 + 78),
                                        static_cast<Uint8>(c.g / 2 + 78),
                                        static_cast<Uint8>(c.b / 2 + 78), 255};
                fillBevel(r, rc, base, lighten(base, 26), lighten(base, -26));
                // dashed base outline
                setColor(r, {c.r, c.g, c.b, 170});
                SDL_RenderDrawRect(r, &rc);
            }
        }
    }

    // goal marker: golden target
    const barricade::Point goal{8, 0};
    const SDL_Rect gr = cellRect(goal);
    const int gcx = gr.x + kCell / 2, gcy = gr.y + kCell / 2;
    setColor(r, {190, 148, 48, 255});
    fillCircle(r, gcx, gcy, kCell / 2 - 5);
    setColor(r, {255, 250, 235, 255});
    fillCircle(r, gcx, gcy, kCell / 2 - 13);
    setColor(r, {190, 148, 48, 255});
    drawLine(r, gcx, gcy - 6, gcx, gcy + 6);
    drawLine(r, gcx - 6, gcy, gcx + 6, gcy);
    drawRing(r, gcx, gcy, kCell / 2 - 10, {190, 148, 48, 200});

    // highlight reachable cells of the selected pawn
    if (selectedPawn >= 0 && !game.pendingBarricade()) {
        const auto dests = game.legalDestinations(cur, selectedPawn);
        for (const barricade::Point p : dests) drawReach(r, p, kHighlightColor, 120);
    }

    // hover hint: reachable cells of the hovered own pawn
    if (selectedPawn < 0 && hoverPawn >= 0 && !game.pendingBarricade()) {
        const auto dests = game.legalDestinations(cur, hoverPawn);
        for (const barricade::Point p : dests) drawReach(r, p, kHighlightColor, 70);
    }

    // barricade placement cells
    if (game.pendingBarricade()) {
        const auto cells = game.barricadePlacements();
        for (const barricade::Point p : cells) {
            const SDL_Rect rc = cellRect(p);
            setColor(r, {90, 160, 255, 60});
            const SDL_Rect inner{rc.x + 6, rc.y + 6, kCell - 12, kCell - 12};
            SDL_RenderFillRect(r, &inner);
            setColor(r, kPlacementColor);
            SDL_RenderDrawRect(r, &rc);
            SDL_RenderDrawRect(r, &inner);
        }
    }

    // barricades: dark wood block with an X
    for (const barricade::Point b : game.barricades()) {
        if (b.x < 0) continue;
        const SDL_Rect rc = cellRect(b);
        const SDL_Rect inner{rc.x + 9, rc.y + 9, kCell - 18, kCell - 18};
        fillBevel(r, inner, {96, 78, 58, 255}, {140, 118, 92, 255}, {54, 44, 32, 255});
        const int bcx = rc.x + kCell / 2, bcy = rc.y + kCell / 2;
        setColor(r, {58, 46, 34, 255});
        drawLine(r, bcx - 8, bcy - 8, bcx + 8, bcy + 8);
        drawLine(r, bcx - 8, bcy + 8, bcx + 8, bcy - 8);
        setColor(r, {150, 128, 100, 255});
        drawLine(r, bcx - 6, bcy - 6, bcx + 6, bcy + 6);
        drawLine(r, bcx - 6, bcy + 6, bcx + 6, bcy - 6);
    }

    // last-move marker
    if (lastMove.x >= 0) drawFadeRing(r, lastMove, lastMoveAt, 700, {255, 220, 120, 255});

    // computer advice overlay
    if (!game.isOver()) drawAdviceOverlay(r, font, game, advice, adviceMatch);

    // pawns: polished tokens
    for (int p = 0; p < game.playerCount(); ++p) {
        for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
            const barricade::Point px = pawnPixel(game, anim, p, m);
            const int cx = px.x + kCell / 2, cy = px.y + kCell / 2;
            const int rad = kCell / 2 - 3;
            const SDL_Color c = kPlayerColors[p];
            setColor(r, {20, 15, 10, 90});
            fillCircle(r, cx + 2, cy + 3, rad);
            setColor(r, lighten(c, -70));
            fillCircle(r, cx, cy, rad);
            for (int i = 0; i < 5; ++i) {
                const double t = static_cast<double>(i) / 4.0;
                setColor(r, lerpColor(lighten(c, 45), c, t));
                fillCircle(r, cx, cy, rad - 1 - i * (rad / 5));
            }
            setColor(r, {255, 255, 255, 110});
            fillCircle(r, cx - rad / 3, cy - rad / 3, std::max(2, rad / 5));
            drawRing(r, cx, cy, rad - 1, {20, 15, 10, 90});
            if (p == cur && selectedPawn == m) {
                const int srad = rad + 2 + static_cast<int>(3.0 * std::sin(SDL_GetTicks() / 160.0));
                drawRing(r, cx, cy, srad, kSelectColor);
            }
        }
    }

    // hint rings on own movable pawns
    if (showHints && selectedPawn < 0 && !game.pendingBarricade()) {
        for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
            if (game.legalDestinations(cur, m).empty()) continue;
            const barricade::Point px = pawnPixel(game, anim, cur, m);
            const int cx = px.x + kCell / 2, cy = px.y + kCell / 2;
            SDL_Color hint = lighten(kPlayerColors[cur], 80);
            hint.a = 150;
            drawRing(r, cx, cy, kCell / 2 - 2, hint);
        }
    }
    // hovered pawn ring
    if (selectedPawn < 0 && hoverPawn >= 0 && !game.pendingBarricade()) {
        const barricade::Point px = pawnPixel(game, anim, cur, hoverPawn);
        drawRing(r, px.x + kCell / 2, px.y + kCell / 2, kCell / 2 - 2, {255, 255, 255, 200});
    }

    // game over overlay
    if (game.isOver()) {
        setColor(r, {20, 15, 10, 110});
        const SDL_Rect boardArea{kBoardX, kBoardY, kBoardW, kBoardH};
        SDL_RenderFillRect(r, &boardArea);
    }

    // status bar: leather strip
    const SDL_Rect sb{0, 0, kWinW, kStatusH};
    setColor(r, kStatusBg);
    SDL_RenderFillRect(r, &sb);
    SDL_Rect stripe{0, 0, 6, kStatusH};
    setColor(r, game.isOver() ? kPlayerColors[game.winner()] : kPlayerColors[cur]);
    SDL_RenderFillRect(r, &stripe);
    setColor(r, {28, 21, 14, 255});
    const SDL_Rect sbBottom{0, kStatusH - 3, kWinW, 3};
    SDL_RenderFillRect(r, &sbBottom);

    if (game.isOver()) {
        const SDL_Color wc = kPlayerColors[game.winner()];
        std::string t = "Le joueur ";
        t += kPlayerNames[game.winner()];
        t += " a gagne ! (R pour rejouer)";
        renderText(r, font, t, 24, 14, wc);
    } else {
        setColor(r, kPlayerColors[cur]);
        fillCircle(r, 22, kStatusH / 2, 9);
        drawRing(r, 22, kStatusH / 2, 13, {255, 255, 255, 120});
        const std::string turnText = std::string("Tour : ") + kPlayerNames[cur];
        renderText(r, font, turnText, 40, 4, kTextColor);
        if (!msg.empty()) renderText(r, small, msg, 40, 32, {255, 200, 90, 255});

        const SDL_Rect die{kWinW - 56, 8, 36, 36};
        setColor(r, {245, 245, 245, 255});
        SDL_RenderFillRect(r, &die);
        setColor(r, {90, 90, 90, 255});
        SDL_RenderDrawRect(r, &die);
        if (game.dice() > 0) {
            drawPips(r, kWinW - 38, 24, game.dice());
            char buf[16];
            std::snprintf(buf, sizeof buf, "De : %d", game.dice());
            renderText(r, font, buf, kWinW - 138, 14, kTextColor);
        }
    }
}

void setMessage(std::string& msg, const std::string& text) {
    msg = text;
}

struct MenuButton {
    SDL_Rect rc;
    const char* label;
};

void drawMenu(SDL_Renderer* r, TTF_Font* titleFont, TTF_Font* font, SDL_Texture* wood, int mouseX,
              int mouseY, int players, int humans) {
    setColor(r, {28, 22, 16, 255});
    SDL_RenderClear(r);
    drawTexture(r, wood, 0, 0, kWinW, kWinH);

    // decorative board backdrop
    for (int y = 0; y < barricade::kRows; ++y) {
        for (int x = 0; x < barricade::kCols; ++x) {
            if (!barricade::isTrackCell(x, y)) continue;
            const SDL_Rect rc = cellRect({x, y});
            setColor(r, barricade::isGoalCell(x, y) ? kGoalColor : SDL_Color{196, 168, 124, 255});
            SDL_RenderFillRect(r, &rc);
            setColor(r, kGridColor);
            SDL_RenderDrawRect(r, &rc);
        }
    }
    setColor(r, {28, 22, 16, 205});
    const SDL_Rect overlay{kBoardX - 30, 0, kBoardW + 60, kWinH};
    SDL_RenderFillRect(r, &overlay);

    renderCentered(r, titleFont, "Barricade", kMenuCX, 110, {240, 220, 170, 255});
    renderCentered(r, font, "Malefiz", kMenuCX, 150, {160, 145, 120, 255});

    auto drawStepper = [&](int cy, const char* label, int value) {
        renderCentered(r, font, label, kMenuCX, cy - 34, kTextColor);
        const SDL_Rect minus{kMenuCX - 74, cy - 20, 48, 40};
        const SDL_Rect plus{kMenuCX + 26, cy - 20, 48, 40};
        fillBevel(r, minus, {70, 60, 48, 255}, {110, 96, 80, 255}, {48, 40, 32, 255});
        fillBevel(r, plus, {70, 60, 48, 255}, {110, 96, 80, 255}, {48, 40, 32, 255});
        renderCentered(r, font, "-", minus.x + minus.w / 2, minus.y + minus.h / 2 + 1, kTextColor);
        renderCentered(r, font, "+", plus.x + plus.w / 2, plus.y + plus.h / 2 + 1, kTextColor);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", value);
        renderCentered(r, titleFont, buf, kMenuCX, cy + 4, kTextColor);
    };

    drawStepper(290, "Nombre de joueurs", players);
    drawStepper(400, "Joueurs humains", humans);

    const SDL_Rect play{kMenuCX - 110, 520, 220, 60};
    const bool hover = mouseX >= play.x && mouseX < play.x + play.w && mouseY >= play.y &&
                       mouseY < play.y + play.h;
    fillBevel(r, play, hover ? SDL_Color{110, 150, 60, 255} : SDL_Color{90, 120, 50, 255},
              {150, 200, 90, 255}, {55, 75, 30, 255});
    renderCentered(r, titleFont, "Jouer", play.x + play.w / 2, play.y + play.h / 2 + 2, kTextColor);

    renderCentered(r, font, "Entree pour jouer - Echap pour quitter", kMenuCX, 640,
                   {120, 110, 95, 255});
}

}  // namespace

int main(int argc, char* argv[]) {
    int playerCount = 4;
    if (argc > 1) {
        playerCount = std::atoi(argv[1]);
        if (playerCount < 2 || playerCount > 4) playerCount = 4;
    }
    int humanCount = 1;
    if (argc > 2) {
        humanCount = std::atoi(argv[2]);
        if (humanCount < 0 || humanCount > playerCount) humanCount = 1;
    }
    const auto isAi = [&humanCount](int p) { return p >= humanCount; };
    constexpr int kAiBudget = 1000;           // ms of MCTS search per AI move
    constexpr Uint32 kAiPace = 330;           // min ms between two AI actions

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return EXIT_FAILURE;
    }
    if (TTF_Init() != 0) {
        SDL_Log("TTF_Init failed: %s", TTF_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_Window* window = SDL_CreateWindow("Barricade", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          kWinW, kWinH, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        TTF_Quit();
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        TTF_Quit();
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Texture* wood = makeWoodTexture(renderer, kWinW, kWinH);

    TTF_Font* font = TTF_OpenFont("C:/Windows/Fonts/arial.ttf", 20);
    if (!font) SDL_Log("TTF_OpenFont failed: %s", TTF_GetError());
    TTF_Font* small = TTF_OpenFont("C:/Windows/Fonts/arial.ttf", 15);
    if (!small) small = font;
    TTF_Font* titleFont = TTF_OpenFont("C:/Windows/Fonts/arialbd.ttf", 44);
    if (!titleFont) titleFont = font;

    barricade::Game game(playerCount);
    game.startTurn();

    enum class AppState { Menu, Playing };
    AppState state = (argc > 1) ? AppState::Playing : AppState::Menu;
    int menuPlayers = playerCount;
    int menuHumans = humanCount;

    int selectedPawn = -1;
    std::string msg;
    std::string lastAiText;
    bool skipPending = false;

    Anim anim{};
    barricade::Point lastMove{-1, -1};
    Uint32 lastMoveAt = 0;

    // Static win-chance history for the evolution graph (one point per move,
    // tracked by the position signature so even a move that leaves the chances
    // unchanged records a point).
    std::vector<std::vector<double>> winHist;
    winHist.reserve(512);
    uint64_t winHistSig = 0;

    // Asynchronous AI: the search runs in a background thread so the UI keeps
    // animating; the result is applied on the main thread.
    bool aiBusy = false;
    std::atomic<bool> aiHaveResult{false};
    std::thread aiThread;
    barricade::AIMove aiMove;
    bool aiIsPlacement = false;
    Uint32 aiNextAt = 0;

    // Computer analysis for a human turn. A background search runs
    // automatically for the current position; the overlay is only revealed
    // when the user presses C (or the panel button). Results are tagged with
    // the position signature so stale data never shows.
    AdviceView advice;
    bool adviceBusy = false;
    std::atomic<bool> adviceHaveResult{false};
    std::thread adviceThread;
    std::vector<barricade::MctsRecommendation> adviceResult;
    std::vector<barricade::BarricadeRecommendation> advicePlaceResult;
    int adviceMode = 0;  // mode of the in-flight computation
    uint64_t adviceSig = 0;

    // Live analysis engine: one persistent thread. The overlay is driven by
    // its deepening sessions; the UI thread never joins it while it runs.
    AnalysisEngine engine;
    engine.snapshot = game;
    engine.th = std::thread(engineLoop, std::ref(engine));
    uint64_t engineSig = 0;
    int engineMode = 0;

    const auto engineStart = [&](int mode) {
        std::lock_guard<std::mutex> guard(engine.m);
        engine.snapshot = game;
        engine.mode = mode;
        engine.sig = posSig(game);
        engine.scenarios.clear();
        engine.restart.store(false);
        engine.requestStop.store(false);
        engine.want.store(true);
        engine.cv.notify_one();
    };
    const auto engineStop = [&] { engine.requestStop.store(true); };
    const auto engineSync = [&] {
        std::lock_guard<std::mutex> guard(engine.m);
        advice.scenarios = engine.scenarios;
        advice.engineStepMs = engine.stepMs.load();
        advice.engineStep = engine.step.load();
        advice.engineBaseMs = engine.baseMs.load();
        advice.engineActive = engine.running.load();
    };

    const auto launchAnalysis = [&](int mode) {
        if (adviceBusy) return;
        adviceMode = mode;
        advice.player = game.currentPlayer();
        advice.busy = true;
        adviceSig = posSig(game);
        advice.moves.clear();
        advice.placements.clear();
        const barricade::Game snapshot = game;
        adviceBusy = true;
        adviceHaveResult = false;
        adviceThread = std::thread([snapshot, mode, &adviceResult, &advicePlaceResult,
                                    &adviceHaveResult] {
            if (mode == 2) {
                advicePlaceResult = barricade::barricadeRecommendations(snapshot, 6);
            } else {
                adviceResult =
                    barricade::mctsRecommendations(snapshot, snapshot.currentPlayer(), kAiBudget);
            }
            adviceHaveResult = true;
        });
    };
    const auto closeAdvice = [&] {
        advice.show = false;
        advice.moves.clear();
        advice.placements.clear();
        advice.scenarios.clear();
        engineStop();
    };
    const auto toggleAdvice = [&] {
        if (advice.show) {
            closeAdvice();
        } else if (!game.isOver() && !isAi(game.currentPlayer())) {
            advice.show = true;
        }
    };

    // End-game simulation running in the background for the current position:
    // plays whole games with the fast greedy policies and counts real winners,
    // which gives a finer win estimate than the static race heuristic.
    struct SimRun {
        std::atomic<bool> stop{true};
        std::atomic<bool> paused{true};
        std::atomic<long long> games{0};
        std::array<std::atomic<long long>, barricade::kMaxPlayers> wins{};
        std::thread th;
        uint64_t sig = 0;
    };
    SimRun simRun;

    const auto startSim = [&](uint64_t sig) {
        simRun.stop.store(true);
        if (simRun.th.joinable()) simRun.th.join();
        simRun.sig = sig;
        simRun.games.store(0);
        for (int p = 0; p < game.playerCount(); ++p) simRun.wins[p].store(0);
        const barricade::Game snapshot = game;
        simRun.stop.store(false);
        simRun.th = std::thread([snapshot, &simRun] {
            barricade::simulateWinChancesAsync(snapshot, 1000000000LL, &simRun.stop,
                                               simRun.wins.data(), snapshot.playerCount(),
                                               &simRun.games, 0);
        });
    };
    const auto stopSim = [&] {
        simRun.stop.store(true);
        if (simRun.th.joinable()) simRun.th.join();
    };
    // Resume without resetting the accumulated games when the position is
    // unchanged (used by the Pause/Lancer toggle).
    const auto resumeSim = [&] {
        const barricade::Game snapshot = game;
        simRun.stop.store(false);
        simRun.th = std::thread([snapshot, &simRun] {
            barricade::simulateWinChancesAsync(snapshot, 1000000000LL, &simRun.stop,
                                               simRun.wins.data(), snapshot.playerCount(),
                                               &simRun.games, 0);
        });
    };
    const auto toggleSim = [&] {
        if (simRun.paused.load()) {
            simRun.paused.store(false);
            if (posSig(game) != simRun.sig) {
                startSim(posSig(game));  // position changed while paused: restart
            } else {
                resumeSim();             // same position: keep counting
            }
        } else {
            simRun.paused.store(true);
            stopSim();
        }
    };

    bool running = true;
    while (running) {
        int mouseX = 0, mouseY = 0;
        SDL_GetMouseState(&mouseX, &mouseY);

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (state == AppState::Menu) {
                if (e.type == SDL_KEYDOWN) {
                    if (e.key.keysym.sym == SDLK_ESCAPE) {
                        running = false;
                    } else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) {
                        playerCount = menuPlayers;
                        humanCount = menuHumans;
                        game = barricade::Game(playerCount);
                        game.startTurn();
                        selectedPawn = -1;
                        msg.clear();
                        skipPending = false;
                        anim = Anim{};
                        lastMove = {-1, -1};
                        winHist.clear();
                        winHistSig = 0;
                        state = AppState::Playing;
                    }
                } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                    const int mx = e.button.x, my = e.button.y;
                    const auto inRect = [&](const SDL_Rect& rc) {
                        return mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h;
                    };
                    const SDL_Rect mMinus{kMenuCX - 74, 270, 48, 40};
                    const SDL_Rect mPlus{kMenuCX + 26, 270, 48, 40};
                    const SDL_Rect hMinus{kMenuCX - 74, 380, 48, 40};
                    const SDL_Rect hPlus{kMenuCX + 26, 380, 48, 40};
                    const SDL_Rect play{kMenuCX - 110, 520, 220, 60};
                    if (inRect(mMinus)) {
                        if (menuPlayers > 2) {
                            --menuPlayers;
                            if (menuHumans > menuPlayers) menuHumans = menuPlayers;
                        }
                    } else if (inRect(mPlus)) {
                        if (menuPlayers < 4) ++menuPlayers;
                    } else if (inRect(hMinus)) {
                        if (menuHumans > 0) --menuHumans;
                    } else if (inRect(hPlus)) {
                        if (menuHumans < menuPlayers) ++menuHumans;
                    } else if (inRect(play)) {
                        playerCount = menuPlayers;
                        humanCount = menuHumans;
                        game = barricade::Game(playerCount);
                        game.startTurn();
                        selectedPawn = -1;
                        msg.clear();
                        skipPending = false;
                        anim = Anim{};
                        lastMove = {-1, -1};
                        winHist.clear();
                        winHistSig = 0;
                        state = AppState::Playing;
                    }
                }
            } else if (aiBusy) {
                // ignore input while the AI is thinking
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (e.key.keysym.sym == SDLK_r) {
                    game = barricade::Game(playerCount);
                    game.startTurn();
                    selectedPawn = -1;
                    msg.clear();
                    lastAiText.clear();
                    skipPending = false;
                    anim = Anim{};
                    lastMove = {-1, -1};
                    winHist.clear();
                    winHistSig = 0;
                    closeAdvice();
                } else if (e.key.keysym.sym == SDLK_c) {
                    toggleAdvice();
                } else if (e.key.keysym.sym == SDLK_m) {
                    state = AppState::Menu;
                    closeAdvice();
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                const int mx = e.button.x, my = e.button.y;
                const int cur = game.currentPlayer();
    const PanelLayout L = panelLayout(advice.show);
                const auto inRect = [&](const SDL_Rect& rc) {
                    return mx >= rc.x && mx < rc.x + rc.w && my >= rc.y && my < rc.y + rc.h;
                };

                if (e.button.button == SDL_BUTTON_RIGHT) {
                    selectedPawn = -1;
                    continue;
                }
                if (e.button.button != SDL_BUTTON_LEFT || game.isOver()) continue;

                // panel interactions
                if (mx >= kPanelX) {
                    // Pause / Lancer toggle for the end-game simulation (gains box)
                    if (!game.isOver() && inRect(L.simBtn)) {
                        toggleSim();
                    } else if (!isAi(cur) && !game.isOver() && inRect(L.adviseBtn)) {
                        toggleAdvice();
                    } else if (advice.show && inRect(L.adviceClose)) {
                        closeAdvice();
                    } else if (advice.show && !game.isOver()) {
                        // live-analysis controls: budget / relancer / pause
                        const EngineControls c = engineControlRects(L.adviceBox);
                        if (inRect(c.minus)) {
                            double b = engine.baseMs.load();
                            engine.baseMs.store(b > 250.0 ? b / 2.0 : 250.0);
                            engine.restart.store(true);
                        } else if (inRect(c.plus)) {
                            double b = engine.baseMs.load();
                            engine.baseMs.store(b < 4000.0 ? b * 2.0 : 4000.0);
                            engine.restart.store(true);
                        } else if (inRect(c.relancer)) {
                            engineStop();
                            engine.restart.store(false);
                            engine.want.store(true);
                            engine.cv.notify_one();
                        } else if (inRect(c.stop)) {
                            if (engine.running.load()) {
                                engineStop();
                            } else {
                                engine.restart.store(false);
                                engine.want.store(true);
                                engine.cv.notify_one();
                            }
                        }
                    }
                    continue;
                }

                const int cx = (mx - kBoardX) / kCell;
                const int cy = (my - kBoardY) / kCell;
                if (cx < 0 || cx >= barricade::kCols || cy < 0 || cy >= barricade::kRows) continue;
                const barricade::Point cell{cx, cy};

                if (game.pendingBarricade()) {
                    const auto cells = game.barricadePlacements();
                    if (std::find(cells.begin(), cells.end(), cell) != cells.end()) {
                        game.placeBarricade(cell);
                        lastMove = cell;
                        lastMoveAt = SDL_GetTicks();
                        selectedPawn = -1;
                        msg.clear();
                        skipPending = false;
                        closeAdvice();
                    } else {
                        setMessage(msg, "Bloc interdit : case occupee, rangee du bas ou but");
                    }
                    continue;
                }

                const int pawn = pawnAtCell(game, cur, cell);
                bool clicked = false;
                if (pawn >= 0 && !game.legalDestinations(cur, pawn).empty()) {
                    selectedPawn = pawn;
                    msg.clear();
                    clicked = true;
                }
                if (clicked) continue;

                if (selectedPawn >= 0) {
                    const auto dests = game.legalDestinations(cur, selectedPawn);
                    if (std::find(dests.begin(), dests.end(), cell) != dests.end()) {
                        const barricade::Point from = game.pawnPos(cur, selectedPawn);
                        if (game.movePawn(cur, selectedPawn, cell)) {
                            anim = Anim{true, cur, selectedPawn, from, cell, SDL_GetTicks(), 280};
                            lastMove = cell;
                            lastMoveAt = SDL_GetTicks();
                            selectedPawn = -1;
                            skipPending = false;
                            closeAdvice();
                            if (game.isOver()) {
                                setMessage(msg, std::string("Le joueur ") + kPlayerNames[game.winner()] + " a gagne ! (R pour rejouer)");
                            } else if (!game.pendingBarricade()) {
                                msg.clear();
                            }
                        }
                    }
                }
            }
        }

        // AI players play automatically, in a background thread.
        if (!game.isOver() && isAi(game.currentPlayer())) {
            if (!aiBusy) {
                if (game.dice() == 0) {
                    game.startTurn();
                    aiNextAt = SDL_GetTicks() + kAiPace;
                } else if (SDL_GetTicks() >= aiNextAt) {
                    selectedPawn = -1;
                    const barricade::Game snapshot = game;
                    aiBusy = true;
                    aiHaveResult = false;
                    aiThread = std::thread([snapshot, &aiMove, &aiIsPlacement, &aiHaveResult] {
                        aiIsPlacement = snapshot.pendingBarricade();
                        aiMove = barricade::mctsMove(snapshot, snapshot.currentPlayer(), kAiBudget);
                        if (aiIsPlacement && aiMove.dest.x < 0) {
                            // Fallback if the search found nothing (rare).
                            aiMove.dest = barricade::naiveBarricadePlacement(snapshot);
                        }
                        aiHaveResult = true;
                    });
                }
            } else if (aiHaveResult) {
                aiThread.join();
                aiBusy = false;
                const int cur = game.currentPlayer();
                if (aiIsPlacement) {
                    game.placeBarricade(aiMove.dest);
                    lastMove = aiMove.dest;
                    lastMoveAt = SDL_GetTicks();
                    msg.clear();
                } else if (aiMove.pawn >= 0) {
                    const barricade::Point from = game.pawnPos(cur, aiMove.pawn);
                    game.movePawn(cur, aiMove.pawn, aiMove.dest);
                    anim = Anim{true, cur, aiMove.pawn, from, aiMove.dest, SDL_GetTicks(), 280};
                    lastMove = aiMove.dest;
                    lastMoveAt = SDL_GetTicks();
                    char buf[80];
                    std::snprintf(buf, sizeof buf, "IA (%s) joue P%d -> (%d,%d)",
                                  kPlayerNames[cur], aiMove.pawn + 1, aiMove.dest.x, aiMove.dest.y);
                    lastAiText = buf;
                    if (game.isOver()) {
                        setMessage(msg, std::string("Le joueur ") + kPlayerNames[game.winner()] + " a gagne ! (R pour rejouer)");
                    } else if (!game.pendingBarricade()) {
                        msg.clear();
                    }
                } else {
                    // No legal move: the auto-skip block below advances the turn.
                    msg.clear();
                }
                aiNextAt = SDL_GetTicks() + kAiPace;
            }
        }

        // auto-skip a player who has no legal move
        if (!aiBusy && !game.isOver() && !game.pendingBarricade() && game.dice() > 0 &&
            !game.hasLegalMove(game.currentPlayer())) {
            if (!skipPending) {
                skipPending = true;
                setMessage(msg, std::string("Joueur ") + kPlayerNames[game.currentPlayer()] +
                                   " : aucun coup possible");
            } else if (msg == std::string("Joueur ") + kPlayerNames[game.currentPlayer()] +
                                " : aucun coup possible") {
                game.nextTurn();
                selectedPawn = -1;
                skipPending = false;
                msg.clear();
            }
        }

        if (state == AppState::Playing) {
            // collect finished analysis
            if (adviceBusy && adviceHaveResult) {
                adviceThread.join();
                adviceBusy = false;
                if (adviceMode == 2) {
                    advice.placements = std::move(advicePlaceResult);
                } else {
                    advice.moves = std::move(adviceResult);
                }
                advice.busy = false;
            }

            const uint64_t sig = posSig(game);
            const int wantMode = game.pendingBarricade() ? 2 : 1;

            // keep the end-game simulation fresh for the current position
            // (unless the user has paused it)
            if (game.isOver()) {
                stopSim();
            } else if (!simRun.paused.load() && sig != simRun.sig) {
                startSim(sig);
            }

            // the overlay is dismissed when it can no longer apply
            if (game.isOver() || isAi(game.currentPlayer())) advice.show = false;

            const bool humanPhase =
                !game.isOver() && !isAi(game.currentPlayer()) && game.dice() > 0;

            // The live engine drives the overlay; the auto-analysis only runs
            // when the overlay is closed. While the overlay stays open the
            // engine keeps deepening the same position (it only stops on
            // demand: X, C, or closing via the AI turn / game over).
            if (humanPhase && advice.show) {
                if (engineSig != sig || engineMode != wantMode) {
                    engineStart(wantMode);
                    engineSig = sig;
                    engineMode = wantMode;
                }
            } else if (!advice.show && (engine.want.load() || engine.running.load())) {
                engineStop();
            }
            engineSync();

            // auto-analyse the current position on a human turn
            if (humanPhase && !advice.show && !adviceBusy &&
                (adviceSig != sig || adviceMode != wantMode)) {
                launchAnalysis(wantMode);
            }

            // the overlay only shows when the analysis matches the board
            bool adviceMatch;
            if (advice.show) {
                adviceMatch = engineSig == sig && engineMode == wantMode;
            } else {
                adviceMatch = !adviceBusy && adviceSig == sig && adviceMode == wantMode;
            }

            // hover hints: reachable cells under the mouse (human turn only)
            int hoverPawn = -1;
            if (!game.isOver() && !game.pendingBarricade() && !isAi(game.currentPlayer())) {
                const int hx = (mouseX - kBoardX) / kCell;
                const int hy = (mouseY - kBoardY) / kCell;
                if (mouseX >= kBoardX && mouseX < kBoardX + kBoardW && mouseY >= kBoardY &&
                    mouseY < kBoardY + kBoardH && hx >= 0 && hx < barricade::kCols && hy >= 0 &&
                    hy < barricade::kRows) {
                    hoverPawn = pawnAtCell(game, game.currentPlayer(), {hx, hy});
                }
            }

            const bool showHints =
                !game.isOver() && !game.pendingBarricade() && !isAi(game.currentPlayer());
            const bool humanTurn = !isAi(game.currentPlayer());
            long long simGames = simRun.games.load(std::memory_order_relaxed);
            std::vector<double> simShares(game.playerCount(), 0.0);
            if (!simRun.wins.empty()) {
                long long sum = 0;
                for (const auto& w : simRun.wins) sum += w.load(std::memory_order_relaxed);
                if (sum > 0) {
                    for (int p = 0; p < game.playerCount(); ++p) {
                        simShares[p] = static_cast<double>(simRun.wins[p].load(std::memory_order_relaxed)) / sum;
                    }
                }
            }
            // win-chance history for the evolution graph: one point per
            // position change (a move), tracked by the signature, so the curve
            // keeps drawing even when the normalized estimate barely moves.
            const std::vector<double> chancesNow = barricade::winChances(game);
            if (winHist.empty() || sig != winHistSig) {
                if (winHist.size() >= 500) winHist.erase(winHist.begin());
                winHist.push_back(chancesNow);
                winHistSig = sig;
            }
            drawBoard(renderer, font, small, wood, game, selectedPawn, hoverPawn, showHints, msg, anim,
                      lastMove, lastMoveAt, advice, adviceMatch);
            drawPanel(renderer, font, small, game, advice, barricade::mctsInfo(), lastAiText,
                      humanTurn, adviceMatch, adviceMode, simGames, simShares,
                      simRun.paused.load(), winHist);
        } else {
            drawMenu(renderer, titleFont, font, wood, mouseX, mouseY, menuPlayers, menuHumans);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (aiBusy) aiThread.join();
    if (adviceBusy) adviceThread.join();
    stopSim();
    engineStop();
    engine.exit.store(true);
    engine.cv.notify_all();
    if (engine.th.joinable()) engine.th.join();

    if (wood) SDL_DestroyTexture(wood);
    if (titleFont && titleFont != font) TTF_CloseFont(titleFont);
    if (small && small != font) TTF_CloseFont(small);
    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return EXIT_SUCCESS;
}