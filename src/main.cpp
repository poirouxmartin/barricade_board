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
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kCell = 40;
constexpr int kStatusH = 48;
constexpr int kWinW = barricade::kCols * kCell;
constexpr int kWinH = kStatusH + barricade::kRows * kCell;

const SDL_Color kBoardColor = {210, 190, 150, 255};
const SDL_Color kGoalColor = {255, 250, 235, 255};
const SDL_Color kGridColor = {150, 130, 100, 255};
const SDL_Color kBarricadeColor = {80, 68, 56, 255};
const SDL_Color kHighlightColor = {90, 220, 90, 255};
const SDL_Color kPlacementColor = {90, 160, 255, 255};
const SDL_Color kSelectColor = {255, 255, 255, 255};
const SDL_Color kStatusBg = {30, 25, 20, 255};
const SDL_Color kTextColor = {235, 230, 220, 255};

const SDL_Color kPlayerColors[barricade::kMaxPlayers] = {
    {205, 55, 55, 255},    // red
    {55, 130, 205, 255},   // blue
    {225, 200, 45, 255},   // yellow
    {60, 170, 75, 255},    // green
};

const char* kPlayerNames[barricade::kMaxPlayers] = {"Rouge", "Bleu", "Jaune", "Vert"};

SDL_Color lighten(SDL_Color c, int d) {
    const auto clamp = [](int v) {
        return static_cast<Uint8>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    return {clamp(c.r + d), clamp(c.g + d), clamp(c.b + d), c.a};
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

SDL_Rect cellRect(barricade::Point p) {
    return {p.x * kCell, kStatusH + p.y * kCell, kCell, kCell};
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
    int px = pos.x * kCell;
    int py = kStatusH + pos.y * kCell;
    if (anim.active && anim.player == p && anim.pawn == m) {
        const Uint32 now = SDL_GetTicks();
        double t = static_cast<double>(now - anim.start) / static_cast<double>(anim.dur);
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        const double ease = t * t * (3.0 - 2.0 * t);
        px = static_cast<int>(std::lround((anim.from.x + (anim.to.x - anim.from.x) * ease) * kCell));
        py = kStatusH + static_cast<int>(std::lround((anim.from.y + (anim.to.y - anim.from.y) * ease) * kCell));
    }
    return {px, py};
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

void drawArrow(SDL_Renderer* r, barricade::Point from, barricade::Point to, SDL_Color c) {
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
}

// Computer advice shown during a human turn: arrows on the board + a panel
// listing the MCTS root evaluations.
struct AdviceView {
    bool active = false;  // overlay visible
    bool busy = false;    // computation in progress
    int player = -1;      // player the advice was computed for
    std::vector<barricade::MctsRecommendation> moves;
};

void drawAdvice(SDL_Renderer* r, TTF_Font* font, const barricade::Game& game,
                const AdviceView& advice) {
    if (!advice.active || advice.busy || advice.moves.empty()) return;
    const int cur = game.currentPlayer();
    const size_t n = std::min<size_t>(advice.moves.size(), 8);

    for (size_t i = 0; i < n; ++i) {
        const auto& rec = advice.moves[i];
        SDL_Color c;
        if (rec.value > 0.05) {
            c = {90, 220, 90, 255};
        } else if (rec.value < -0.05) {
            c = {235, 90, 90, 255};
        } else {
            c = {235, 200, 90, 255};
        }
        drawArrow(r, game.pawnPos(cur, rec.move.pawn), rec.move.dest, c);
        if (i == 0) {
            const SDL_Rect rc = cellRect(rec.move.dest);
            setColor(r, {255, 255, 255, 200});
            SDL_RenderDrawRect(r, &rc);
        }
    }

    const int pw = 292;
    const int px = kWinW - pw - 10;
    const int py = kStatusH + 12;
    const int ph = 26 + static_cast<int>(n) * 22;
    setColor(r, {20, 16, 12, 210});
    const SDL_Rect panel{px, py, pw, ph};
    SDL_RenderFillRect(r, &panel);
    setColor(r, {120, 105, 90, 255});
    SDL_RenderDrawRect(r, &panel);
    renderText(r, font, "Conseils de l'IA", px + 10, py + 4, {255, 220, 120, 255});

    for (size_t i = 0; i < n; ++i) {
        const auto& rec = advice.moves[i];
        const int rowY = py + 26 + static_cast<int>(i) * 22;
        setColor(r, kPlayerColors[cur]);
        fillCircle(r, px + 15, rowY + 9, 5);
        char buf[40];
        int pct = static_cast<int>(50 + rec.value * 50);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        std::snprintf(buf, sizeof buf, "P%d -> (%d,%d)  %2d%%", rec.move.pawn + 1,
                      rec.move.dest.x, rec.move.dest.y, pct);
        SDL_Color tc;
        if (rec.value > 0.05) {
            tc = {140, 235, 140, 255};
        } else if (rec.value < -0.05) {
            tc = {240, 130, 130, 255};
        } else {
            tc = {235, 210, 130, 255};
        }
        renderText(r, font, buf, px + 26, rowY, tc);
    }
}

constexpr int kAdviceBtnX = kWinW - 232;
constexpr int kAdviceBtnY = 8;
constexpr int kAdviceBtnW = 88;
constexpr int kAdviceBtnH = 32;

void drawBoard(SDL_Renderer* r, TTF_Font* font, const barricade::Game& game, int selectedPawn,
               int hoverPawn, bool showHints, const std::string& msg, const Anim& anim,
               barricade::Point lastMove, Uint32 lastMoveAt, const AdviceView& advice,
               bool canAdvise) {
    setColor(r, {45, 35, 25, 255});
    SDL_RenderClear(r);

    const int cur = game.currentPlayer();

    // cells
    for (int y = 0; y < barricade::kRows; ++y) {
        for (int x = 0; x < barricade::kCols; ++x) {
            const SDL_Rect rc = cellRect({x, y});
            if (barricade::isTrackCell(x, y)) {
                setColor(r, barricade::isGoalCell(x, y) ? kGoalColor : kBoardColor);
                SDL_RenderFillRect(r, &rc);
                SDL_Rect bevel = {rc.x + 1, rc.y + 1, rc.w - 2, 1};
                setColor(r, {230, 214, 178, 255});
                SDL_RenderFillRect(r, &bevel);
                bevel = {rc.x + 1, rc.y + 1, 1, rc.h - 2};
                SDL_RenderFillRect(r, &bevel);
                bevel = {rc.x + 1, rc.y + rc.h - 2, rc.w - 2, 1};
                setColor(r, {176, 150, 116, 255});
                SDL_RenderFillRect(r, &bevel);
                bevel = {rc.x + rc.w - 2, rc.y + 1, 1, rc.h - 2};
                SDL_RenderFillRect(r, &bevel);
            } else if (barricade::baseOwner(x, y) >= 0) {
                const SDL_Color c = kPlayerColors[barricade::baseOwner(x, y)];
                setColor(r, {static_cast<Uint8>(c.r / 2 + 70), static_cast<Uint8>(c.g / 2 + 70),
                             static_cast<Uint8>(c.b / 2 + 70), 255});
                SDL_RenderFillRect(r, &rc);
            }
            setColor(r, kGridColor);
            SDL_RenderDrawRect(r, &rc);
        }
    }

    // goal marker
    const barricade::Point goal{8, 0};
    const SDL_Rect gr = cellRect(goal);
    const int gcx = gr.x + kCell / 2, gcy = gr.y + kCell / 2;
    setColor(r, {205, 165, 60, 255});
    fillCircle(r, gcx, gcy, kCell / 2 - 6);
    setColor(r, {255, 250, 235, 255});
    fillCircle(r, gcx, gcy, kCell / 2 - 14);
    setColor(r, {205, 165, 60, 255});
    drawLine(r, gcx, gcy - 7, gcx, gcy + 7);
    drawLine(r, gcx - 7, gcy, gcx + 7, gcy);

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

    // barricades
    for (const barricade::Point b : game.barricades()) {
        if (b.x < 0) continue;
        const SDL_Rect rc = cellRect(b);
        const SDL_Rect inner{rc.x + 9, rc.y + 9, kCell - 18, kCell - 18};
        setColor(r, kBarricadeColor);
        SDL_RenderFillRect(r, &inner);
        setColor(r, {120, 105, 90, 255});
        SDL_RenderDrawRect(r, &inner);
        SDL_Rect bevel = {inner.x + 1, inner.y + 1, inner.w - 2, 1};
        setColor(r, {150, 135, 118, 255});
        SDL_RenderFillRect(r, &bevel);
        bevel = {inner.x + 1, inner.y + 1, 1, inner.h - 2};
        SDL_RenderFillRect(r, &bevel);
        const int bcx = rc.x + kCell / 2, bcy = rc.y + kCell / 2;
        setColor(r, {150, 135, 118, 255});
        drawLine(r, bcx - 8, bcy - 8, bcx + 8, bcy + 8);
        drawLine(r, bcx - 8, bcy + 8, bcx + 8, bcy - 8);
    }

    // last-move marker
    if (lastMove.x >= 0) drawFadeRing(r, lastMove, lastMoveAt, 700, {255, 220, 120, 255});

    // computer advice overlay
    if (!game.pendingBarricade() && !game.isOver()) drawAdvice(r, font, game, advice);

    // pawns
    for (int p = 0; p < game.playerCount(); ++p) {
        for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
            const barricade::Point px = pawnPixel(game, anim, p, m);
            const int cx = px.x + kCell / 2, cy = px.y + kCell / 2;
            setColor(r, {35, 28, 20, 255});
            fillCircle(r, cx, cy, kCell / 2 - 3);
            setColor(r, kPlayerColors[p]);
            fillCircle(r, cx, cy, kCell / 2 - 5);
            setColor(r, {255, 255, 255, 70});
            fillCircle(r, cx - 4, cy - 4, 3);
            if (p == cur && selectedPawn == m) {
                const int rad = kCell / 2 - 2 + static_cast<int>(3.0 * std::sin(SDL_GetTicks() / 160.0));
                drawRing(r, cx, cy, rad, kSelectColor);
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
        const SDL_Rect boardArea{0, kStatusH, kWinW, kWinH - kStatusH};
        SDL_RenderFillRect(r, &boardArea);
    }

    // status bar
    const SDL_Rect sb{0, 0, kWinW, kStatusH};
    setColor(r, kStatusBg);
    SDL_RenderFillRect(r, &sb);
    SDL_Rect stripe{0, 0, 6, kStatusH};
    setColor(r, game.isOver() ? kPlayerColors[game.winner()] : kPlayerColors[cur]);
    SDL_RenderFillRect(r, &stripe);

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

        if (canAdvise) {
            const SDL_Rect btn{kAdviceBtnX, kAdviceBtnY, kAdviceBtnW, kAdviceBtnH};
            const bool active = advice.active || advice.busy;
            fillBevel(r, btn, active ? SDL_Color{90, 140, 60, 255} : SDL_Color{70, 60, 48, 255},
                      {130, 100, 80, 255}, {48, 40, 32, 255});
            renderCentered(r, font, "Conseil", btn.x + btn.w / 2, btn.y + btn.h / 2 + 1, kTextColor);
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

void drawMenu(SDL_Renderer* r, TTF_Font* titleFont, TTF_Font* font, int mouseX, int mouseY,
              int players, int humans) {
    setColor(r, {28, 22, 16, 255});
    SDL_RenderClear(r);

    // decorative board backdrop
    for (int y = 0; y < barricade::kRows; ++y) {
        for (int x = 0; x < barricade::kCols; ++x) {
            if (!barricade::isTrackCell(x, y)) continue;
            const SDL_Rect rc = cellRect({x, y});
            setColor(r, barricade::isGoalCell(x, y) ? kGoalColor : kBoardColor);
            SDL_RenderFillRect(r, &rc);
            setColor(r, kGridColor);
            SDL_RenderDrawRect(r, &rc);
        }
    }
    setColor(r, {28, 22, 16, 210});
    const SDL_Rect overlay{0, 0, kWinW, kWinH};
    SDL_RenderFillRect(r, &overlay);

    renderCentered(r, titleFont, "Barricade", kWinW / 2, 110, {240, 220, 170, 255});
    renderCentered(r, font, "Malefiz", kWinW / 2, 150, {160, 145, 120, 255});

    auto drawStepper = [&](int cx, int cy, const char* label, int value) {
        renderCentered(r, font, label, cx, cy - 34, kTextColor);
        const SDL_Rect minus{cx - 74, cy - 20, 48, 40};
        const SDL_Rect plus{cx + 26, cy - 20, 48, 40};
        fillBevel(r, minus, {70, 60, 48, 255}, {110, 96, 80, 255}, {48, 40, 32, 255});
        fillBevel(r, plus, {70, 60, 48, 255}, {110, 96, 80, 255}, {48, 40, 32, 255});
        renderCentered(r, font, "-", minus.x + minus.w / 2, minus.y + minus.h / 2 + 1, kTextColor);
        renderCentered(r, font, "+", plus.x + plus.w / 2, plus.y + plus.h / 2 + 1, kTextColor);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%d", value);
        renderCentered(r, titleFont, buf, cx, cy + 4, kTextColor);
    };

    drawStepper(kWinW / 2, 290, "Nombre de joueurs", players);
    drawStepper(kWinW / 2, 400, "Joueurs humains", humans);

    const SDL_Rect play{kWinW / 2 - 110, 520, 220, 60};
    const bool hover = mouseX >= play.x && mouseX < play.x + play.w && mouseY >= play.y &&
                       mouseY < play.y + play.h;
    fillBevel(r, play, hover ? SDL_Color{110, 150, 60, 255} : SDL_Color{90, 120, 50, 255},
              {150, 200, 90, 255}, {55, 75, 30, 255});
    renderCentered(r, titleFont, "Jouer", play.x + play.w / 2, play.y + play.h / 2 + 2, kTextColor);

    renderCentered(r, font, "Entree pour jouer - Echap pour quitter", kWinW / 2, 640,
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

    TTF_Font* font = TTF_OpenFont("C:/Windows/Fonts/arial.ttf", 20);
    if (!font) SDL_Log("TTF_OpenFont failed: %s", TTF_GetError());
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

    // Computer advice for a human turn (async, same budget as the AI).
    AdviceView advice;
    bool adviceBusy = false;
    std::atomic<bool> adviceHaveResult{false};
    std::thread adviceThread;
    std::vector<barricade::MctsRecommendation> adviceResult;
    const auto requestAdvice = [&] {
        if (adviceBusy || advice.active || game.isOver() || game.pendingBarricade() ||
            isAi(game.currentPlayer()) || aiBusy)
            return;
        advice.active = true;
        advice.busy = true;
        advice.player = game.currentPlayer();
        const barricade::Game snapshot = game;
        adviceBusy = true;
        adviceHaveResult = false;
        adviceThread = std::thread([snapshot, &adviceResult, &adviceHaveResult] {
            adviceResult =
                barricade::mctsRecommendations(snapshot, snapshot.currentPlayer(), kAiBudget);
            adviceHaveResult = true;
        });
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
                    const SDL_Rect mMinus{kWinW / 2 - 74, 270, 48, 40};
                    const SDL_Rect mPlus{kWinW / 2 + 26, 270, 48, 40};
                    const SDL_Rect hMinus{kWinW / 2 - 74, 380, 48, 40};
                    const SDL_Rect hPlus{kWinW / 2 + 26, 380, 48, 40};
                    const SDL_Rect play{kWinW / 2 - 110, 520, 220, 60};
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
                    skipPending = false;
                    anim = Anim{};
                    lastMove = {-1, -1};
                    advice.active = false;
                    advice.busy = false;
                    advice.moves.clear();
                } else if (e.key.keysym.sym == SDLK_c) {
                    requestAdvice();
                } else if (e.key.keysym.sym == SDLK_m) {
                    state = AppState::Menu;
                    advice.active = false;
                    advice.busy = false;
                    advice.moves.clear();
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                const int mx = e.button.x, my = e.button.y;
                const int cx = mx / kCell;
                const int cy = (my - kStatusH) / kCell;
                const barricade::Point cell{cx, cy};
                const int cur = game.currentPlayer();

                if (e.button.button == SDL_BUTTON_RIGHT) {
                    selectedPawn = -1;
                    continue;
                }
                if (e.button.button != SDL_BUTTON_LEFT || game.isOver()) continue;
                if (mx >= kAdviceBtnX && mx < kAdviceBtnX + kAdviceBtnW && my >= kAdviceBtnY &&
                    my < kAdviceBtnY + kAdviceBtnH) {
                    requestAdvice();
                    continue;
                }
                if (cx < 0 || cx >= barricade::kCols || cy < 0 || cy >= barricade::kRows) continue;

                if (game.pendingBarricade()) {
                    const auto cells = game.barricadePlacements();
                    if (std::find(cells.begin(), cells.end(), cell) != cells.end()) {
                        game.placeBarricade(cell);
                        lastMove = cell;
                        lastMoveAt = SDL_GetTicks();
                        selectedPawn = -1;
                        msg.clear();
                        skipPending = false;
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
            // collect finished advice computation
            if (adviceBusy && adviceHaveResult) {
                adviceThread.join();
                adviceBusy = false;
                advice.moves = std::move(adviceResult);
                advice.busy = false;
            }

            // hide advice when it no longer matches the current situation
            if (game.isOver() || game.pendingBarricade() || isAi(game.currentPlayer()) ||
                game.currentPlayer() != advice.player) {
                advice.active = false;
                advice.moves.clear();
            }

            // hover hints: reachable cells under the mouse (human turn only)
            int hoverPawn = -1;
            if (!game.isOver() && !game.pendingBarricade() && !isAi(game.currentPlayer())) {
                const int hx = mouseX / kCell;
                const int hy = (mouseY - kStatusH) / kCell;
                if (hx >= 0 && hx < barricade::kCols && hy >= 0 && hy < barricade::kRows) {
                    hoverPawn = pawnAtCell(game, game.currentPlayer(), {hx, hy});
                }
            }

            const bool showHints =
                !game.isOver() && !game.pendingBarricade() && !isAi(game.currentPlayer());
            drawBoard(renderer, font, game, selectedPawn, hoverPawn, showHints, msg, anim,
                      lastMove, lastMoveAt, advice, showHints);
        } else {
            drawMenu(renderer, titleFont, font, mouseX, mouseY, menuPlayers, menuHumans);
        }
SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (aiBusy) aiThread.join();
    if (adviceBusy) adviceThread.join();

    if (titleFont && titleFont != font) TTF_CloseFont(titleFont);
    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return EXIT_SUCCESS;
}