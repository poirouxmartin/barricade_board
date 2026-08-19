#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <SDL_ttf.h>

#include "ai.h"
#include "board.h"
#include "game.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCell = 40;
constexpr int kStatusH = 48;
constexpr int kFrame = 22;                                  // wooden frame around the board
constexpr int kBoardW = barricade::kCols * kCell;           // 680
constexpr int kBoardH = barricade::kRows * kCell;           // 760
constexpr int kBoardX = kFrame;
constexpr int kBoardY = kStatusH + kFrame;
constexpr int kPanelW = 372;
constexpr int kPanelX = kBoardX + kBoardW + kFrame;         // 724
constexpr int kWinW = kPanelX + kPanelW;                    // 1096
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
void drawArrow(SDL_Renderer* r, TTF_Font* font, barricade::Point from, barricade::Point to,
               SDL_Color c, int num) {
    const SDL_Rect a = cellRect(from), b = cellRect(to);
    const int x0 = a.x + kCell / 2, y0 = a.y + kCell / 2;
    const int x1 = b.x + kCell / 2, y1 = b.y + kCell / 2;
    setColor(r, c);
    drawLine(r, x0, y0, x1, y1);
    const double ang = std::atan2(static_cast<double>(y1 - y0), static_cast<double>(x1 - x0));
    for (int k = -1; k <= 1; k += 2) {
        const double a2 = ang + k * 0.55;
        drawLine(r, x1, y1, x1 - static_cast<int>(11 * std::cos(a2)),
                 y1 - static_cast<int>(11 * std::sin(a2)));
    }
    // number badge, offset perpendicular to the arrow to avoid overlaps
    const double len = std::hypot(static_cast<double>(x1 - x0), static_cast<double>(y1 - y0));
    if (len > 0.0) {
        const double px = -(y1 - y0) / len, py = (x1 - x0) / len;
        const int mx = static_cast<int>((x0 + x1) / 2.0 + px * 14);
        const int my = static_cast<int>((y0 + y1) / 2.0 + py * 14);
        setColor(r, {24, 20, 14, 230});
        fillCircle(r, mx, my, 11);
        drawRing(r, mx, my, 11, c);
        char buf[4];
        std::snprintf(buf, sizeof buf, "%d", num);
        renderCentered(r, font, buf, mx, my, {255, 255, 255, 255});
    }
}

// Computer advice shown during a human turn. An analysis runs automatically
// in the background for the current position; the overlay (arrows / placement
// badges + panel list) is only revealed when the user toggles it with C.
struct AdviceView {
    bool show = false;  // overlay visible (user toggled)
    bool busy = false;  // computation in progress
    int player = -1;    // player the analysis was computed for
    std::vector<barricade::MctsRecommendation> moves;
    std::vector<barricade::BarricadeRecommendation> placements;
};

void drawAdviceOverlay(SDL_Renderer* r, TTF_Font* font, const barricade::Game& game,
                       const AdviceView& advice, bool match, int analysisMode) {
    if (!advice.show || advice.busy || !match) return;
    if (analysisMode == 2) {
        // numbered placement badges on the recommended cells
        const size_t n = std::min<size_t>(advice.placements.size(), 6);
        for (size_t i = 0; i < n; ++i) {
            const SDL_Rect rc = cellRect(advice.placements[i].cell);
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
    const size_t n = std::min<size_t>(advice.moves.size(), 6);
    for (size_t i = 0; i < n; ++i) {
        const auto& rec = advice.moves[i];
        drawArrow(r, font, game.pawnPos(cur, rec.move.pawn), rec.move.dest,
                  kAdvicePalette[i % 8], static_cast<int>(i) + 1);
        if (i == 0) {
            const SDL_Rect rc = cellRect(rec.move.dest);
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
};

PanelLayout panelLayout() {
    PanelLayout L;
    L.adviseBtn = {kPanelX + kPanelPad, kStatusH + 50, kPanelW - 2 * kPanelPad, 34};
    const int y1 = kStatusH + 96;
    const int y2 = y1 + 140 + 10;
    const int y3 = y2 + 138 + 10;
    const int y4 = y3 + 86 + 10;
    const int adviceY = y4 + 86 + 10;
    L.adviceBox = {kPanelX + kPanelPad, adviceY, kPanelW - 2 * kPanelPad, 150};
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

void drawPanel(SDL_Renderer* r, TTF_Font* font, TTF_Font* small, const barricade::Game& game,
               const AdviceView& advice, const barricade::SearchInfo& info,
               const std::string& lastAiText, bool humanTurn, bool match, int analysisMode) {
    const int cx = kPanelX + kPanelPad;
    const int cw = kPanelW - 2 * kPanelPad;
    const PanelLayout L = panelLayout();
    const int cur = game.currentPlayer();

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
    const SDL_Rect b1{cx, kStatusH + 96, cw, 140};
    drawBox(r, b1, "Recherche (MCTS)", font);
    int yy = b1.y + 30;
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
        std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(info.winProb * 100.0)));
        row("Gain estime", buf, kGold);
    } else {
        row("Budget", "500 ms", kTextColor);
        row("", "aucune recherche", kTextDim);
    }

    // Chances de gain box
    const SDL_Rect b2{cx, b1.y + b1.h + 10, cw, 138};
    drawBox(r, b2, "Chances de gain (heuristique)", font);
    const std::vector<double> chances = barricade::winChances(game);
    yy = b2.y + 30;
    for (int p = 0; p < game.playerCount(); ++p) {
        const int barX = b2.x + 84, barW = 176, barH = 16, barY = yy + 4;
        setColor(r, {15, 12, 8, 255});
        const SDL_Rect bg{barX, barY, barW, barH};
        SDL_RenderFillRect(r, &bg);
        const int fill = static_cast<int>(barW * chances[p]);
        if (fill > 0) {
            setColor(r, kPlayerColors[p]);
            const SDL_Rect fg{barX + 1, barY + 1, fill - 2, barH - 2};
            SDL_RenderFillRect(r, &fg);
        }
        setColor(r, {255, 255, 255, 60});
        SDL_RenderDrawRect(r, &bg);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%s", kPlayerNames[p]);
        renderText(r, small, buf, b2.x + 8, yy, kPlayerColors[p]);
        std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(chances[p] * 100.0)));
        renderText(r, small, buf, barX + barW + 8, yy, kTextColor);
        yy += 24;
    }

    // Strategie box
    const SDL_Rect b3{cx, b2.y + b2.h + 10, cw, 86};
    drawBox(r, b3, "Strategie / fin de partie", font);
    std::string strategy = "—";
    if (match && !advice.busy) {
        if (analysisMode == 2 && !advice.placements.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "Bloc -> (%d,%d)",
                          advice.placements[0].cell.x, advice.placements[0].cell.y);
            strategy = buf;
        } else if (!advice.moves.empty()) {
            const auto& rec = advice.moves[0];
            const int pct = static_cast<int>(std::lround(50 + rec.value * 50));
            char buf[64];
            std::snprintf(buf, sizeof buf, "P%d -> (%d,%d) : %d%%", rec.move.pawn + 1,
                          rec.move.dest.x, rec.move.dest.y, pct);
            strategy = buf;
        }
    } else if (!lastAiText.empty()) {
        strategy = lastAiText;
    }
    renderText(r, small, strategy, b3.x + 10, b3.y + 28, kTextColor);
    const std::vector<int> prog = barricade::playerProgress(game);
    int minP = -1, minD = 1000000;
    for (int p = 0; p < game.playerCount(); ++p) {
        if (prog[p] < minD) {
            minD = prog[p];
            minP = p;
        }
    }
    if (minD <= 3) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s proche de la victoire !", kPlayerNames[minP]);
        renderText(r, small, buf, b3.x + 10, b3.y + 52, kGold);
    } else {
        renderText(r, small, "Course vers le but", b3.x + 10, b3.y + 52, kTextDim);
    }

    // Heuristiques box
    const SDL_Rect b4{cx, b3.y + b3.h + 10, cw, 86};
    drawBox(r, b4, "Heuristiques", font);
    std::string staticLine = "Statique (dist. but) :";
    for (int p = 0; p < game.playerCount(); ++p) {
        char buf[16];
        if (prog[p] > 999) {
            std::snprintf(buf, sizeof buf, " %s:-", kPlayerNames[p]);
        } else {
            std::snprintf(buf, sizeof buf, " %s:%d", kPlayerNames[p], prog[p]);
        }
        staticLine += buf;
    }
    renderText(r, small, staticLine, b4.x + 10, b4.y + 28, kTextColor);
    char buf[64];
    if (info.iterations > 0) {
        std::snprintf(buf, sizeof buf, "Profond (MCTS) : %d%%",
                      static_cast<int>(std::lround(info.winProb * 100.0)));
    } else {
        std::snprintf(buf, sizeof buf, "Profond (MCTS) : —");
    }
    renderText(r, small, buf, b4.x + 10, b4.y + 52, kTextDim);

    // Conseils box
    if (advice.show) {
        const SDL_Rect box = L.adviceBox;
        drawBox(r, box, "Conseils de l'IA", font);
        // close button (X)
        setColor(r, {140, 60, 60, 255});
        SDL_RenderFillRect(r, &L.adviceClose);
        setColor(r, {60, 20, 20, 255});
        SDL_RenderDrawRect(r, &L.adviceClose);
        drawLine(r, L.adviceClose.x + 5, L.adviceClose.y + 5, L.adviceClose.x + L.adviceClose.w - 5,
                 L.adviceClose.y + L.adviceClose.h - 5);
        drawLine(r, L.adviceClose.x + L.adviceClose.w - 5, L.adviceClose.y + 5, L.adviceClose.x + 5,
                 L.adviceClose.y + L.adviceClose.h - 5);
        if (advice.busy) {
            renderText(r, small, "Calcul en cours...", box.x + 10, box.y + 30, kTextDim);
        } else if (analysisMode == 2) {
            const size_t n = std::min<size_t>(advice.placements.size(), 6);
            yy = box.y + 30;
            for (size_t i = 0; i < n; ++i) {
                const auto& rec = advice.placements[i];
                char line[48];
                std::snprintf(line, sizeof line, "#%d  Bloc -> (%d,%d)",
                              static_cast<int>(i) + 1, rec.cell.x, rec.cell.y);
                setColor(r, kAdvicePalette[i % 8]);
                fillCircle(r, box.x + 18, yy + 8, 6);
                renderText(r, small, line, box.x + 30, yy, kTextColor);
                yy += 19;
            }
        } else if (advice.moves.empty()) {
            renderText(r, small, "Calcul en cours...", box.x + 10, box.y + 30, kTextDim);
        } else {
            const size_t n = std::min<size_t>(advice.moves.size(), 6);
            yy = box.y + 30;
            for (size_t i = 0; i < n; ++i) {
                const auto& rec = advice.moves[i];
                const int pct = static_cast<int>(std::lround(50 + rec.value * 50));
                char line[48];
                std::snprintf(line, sizeof line, "#%d  P%d -> (%d,%d)  %d%%", static_cast<int>(i) + 1,
                              rec.move.pawn + 1, rec.move.dest.x, rec.move.dest.y, pct);
                // rank swatch
                setColor(r, kAdvicePalette[i % 8]);
                fillCircle(r, box.x + 18, yy + 8, 6);
                renderText(r, small, line, box.x + 30, yy, kTextColor);
                yy += 19;
            }
        }
    }

    // controls hint
    renderText(r, small, "C : conseil   M : menu   R : rejouer   Echap : quitter", cx,
               kWinH - 26, kTextDim);
}

void drawBoard(SDL_Renderer* r, TTF_Font* font, SDL_Texture* wood, const barricade::Game& game,
               int selectedPawn, int hoverPawn, bool showHints, const std::string& msg,
               const Anim& anim, barricade::Point lastMove, Uint32 lastMoveAt,
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
        renderText(r, font, turnText, 40, 6, kTextColor);
        if (!msg.empty()) renderText(r, font, msg, 40, 24, {255, 200, 90, 255});

        const SDL_Rect die{kWinW - 56, 6, 36, 36};
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
    constexpr int kAiBudget = 500;            // ms of MCTS search per AI move
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

    // Asynchronous AI: the search runs in a background thread so the UI keeps
    // animating; the result is applied on the main thread.
    bool aiBusy = false;
    std::atomic<bool> aiHaveResult{false};
    std::thread aiThread;
    barricade::AIMove aiMove;
    bool aiIsPlacement = false;
    barricade::Point aiPlace{0, 0};
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
    };
    const auto toggleAdvice = [&] {
        if (advice.show) {
            closeAdvice();
        } else if (!game.isOver() && !isAi(game.currentPlayer())) {
            advice.show = true;
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
                const PanelLayout L = panelLayout();
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
                    if (!isAi(cur) && !game.isOver() && inRect(L.adviseBtn)) {
                        toggleAdvice();
                    } else if (advice.show && !advice.busy && inRect(L.adviceClose)) {
                        closeAdvice();
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
                        setMessage(msg, "Case invalide pour la barricade");
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
                    aiThread = std::thread([snapshot, &aiMove, &aiPlace, &aiIsPlacement, &aiHaveResult] {
                        if (snapshot.pendingBarricade()) {
                            aiIsPlacement = true;
                            aiPlace = barricade::naiveBarricadePlacement(snapshot);
                        } else {
                            aiIsPlacement = false;
                            aiMove = barricade::mctsMove(snapshot, snapshot.currentPlayer(), kAiBudget);
                        }
                        aiHaveResult = true;
                    });
                }
            } else if (aiHaveResult) {
                aiThread.join();
                aiBusy = false;
                const int cur = game.currentPlayer();
                if (aiIsPlacement) {
                    game.placeBarricade(aiPlace);
                    lastMove = aiPlace;
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

            // auto-analyse the current position on a human turn
            const bool humanPhase =
                !game.isOver() && !isAi(game.currentPlayer()) && game.dice() > 0;
            if (humanPhase && !adviceBusy &&
                (adviceSig != sig || adviceMode != wantMode)) {
                launchAnalysis(wantMode);
            }

            // the overlay only shows when the analysis matches the board
            const bool adviceMatch = !adviceBusy && adviceSig == sig && adviceMode == wantMode;
            if (game.isOver() || isAi(game.currentPlayer())) advice.show = false;

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
            drawBoard(renderer, font, wood, game, selectedPawn, hoverPawn, showHints, msg, anim,
                      lastMove, lastMoveAt, advice, adviceMatch);
            drawPanel(renderer, font, small, game, advice, barricade::mctsInfo(), lastAiText,
                      humanTurn, adviceMatch, adviceMode);
        } else {
            drawMenu(renderer, titleFont, font, wood, mouseX, mouseY, menuPlayers, menuHumans);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (aiBusy) aiThread.join();
    if (adviceBusy) adviceThread.join();

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