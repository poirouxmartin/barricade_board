#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <SDL_ttf.h>

#include "ai.h"
#include "board.h"
#include "game.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kCell = 40;
constexpr int kStatusH = 48;
constexpr int kWinW = barricade::kCols * kCell;
constexpr int kWinH = kStatusH + barricade::kRows * kCell;

const SDL_Color kBoardColor = {210, 190, 150, 255};
const SDL_Color kGoalColor = {255, 250, 235, 255};
const SDL_Color kGridColor = {150, 130, 100, 255};
const SDL_Color kBarricadeColor = {70, 60, 50, 255};
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

void setColor(SDL_Renderer* r, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, 255);
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

void drawBoard(SDL_Renderer* r, TTF_Font* font, const barricade::Game& game, int selectedPawn,
               const std::string& msg) {
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

    // highlight reachable cells of the selected pawn
    if (selectedPawn >= 0 && !game.pendingBarricade()) {
        const auto dests = game.legalDestinations(cur, selectedPawn);
        for (const barricade::Point p : dests) {
            const SDL_Rect rc = cellRect(p);
            setColor(r, kHighlightColor);
            const SDL_Rect inner{rc.x + 8, rc.y + 8, kCell - 16, kCell - 16};
            SDL_RenderFillRect(r, &inner);
        }
    }

    // barricade placement cells
    if (game.pendingBarricade()) {
        const auto cells = game.barricadePlacements();
        for (const barricade::Point p : cells) {
            const SDL_Rect rc = cellRect(p);
            setColor(r, kPlacementColor);
            SDL_RenderDrawRect(r, &rc);
            const SDL_Rect inner{rc.x + 6, rc.y + 6, kCell - 12, kCell - 12};
            SDL_RenderDrawRect(r, &inner);
        }
    }

    // barricades
    for (const barricade::Point b : game.barricades()) {
        if (b.x < 0) continue;
        const SDL_Rect rc = cellRect(b);
        setColor(r, kBarricadeColor);
        const SDL_Rect inner{rc.x + 10, rc.y + 10, kCell - 20, kCell - 20};
        SDL_RenderFillRect(r, &inner);
        setColor(r, {120, 105, 90, 255});
        SDL_RenderDrawRect(r, &inner);
    }

    // pawns
    for (int p = 0; p < game.playerCount(); ++p) {
        for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
            const barricade::Point pos = game.pawnPos(p, m);
            const SDL_Rect rc = cellRect(pos);
            const int cx = rc.x + kCell / 2, cy = rc.y + kCell / 2;
            setColor(r, {40, 30, 20, 255});
            fillCircle(r, cx, cy, kCell / 2 - 3);
            setColor(r, kPlayerColors[p]);
            fillCircle(r, cx, cy, kCell / 2 - 5);
            if (p == cur && selectedPawn == m) {
                drawRing(r, cx, cy, kCell / 2 - 2, kSelectColor);
            }
        }
    }

    // status bar
    SDL_Rect sb{0, 0, kWinW, kStatusH};
    setColor(r, kStatusBg);
    SDL_RenderFillRect(r, &sb);

    if (!game.isOver()) {
        setColor(r, kPlayerColors[cur]);
        SDL_Rect dot{12, 12, 24, 24};
        SDL_RenderFillRect(r, &dot);
        std::string turnText = "Joueur ";
        turnText += kPlayerNames[cur];
        renderText(r, font, turnText, 46, 13, kTextColor);

        SDL_Rect die{kWinW - 58, 8, 40, 40};
        setColor(r, {240, 240, 240, 255});
        SDL_RenderFillRect(r, &die);
        setColor(r, {90, 90, 90, 255});
        SDL_RenderDrawRect(r, &die);
        if (game.dice() > 0) drawPips(r, kWinW - 38, 28, game.dice());
    }

    if (!msg.empty()) {
        renderText(r, font, msg, 46, 24, {255, 200, 90, 255});
    }
}

void setMessage(std::string& msg, const std::string& text) {
    msg = text;
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
    const auto isAi = [humanCount](int p) { return p >= humanCount; };
    constexpr Uint32 kAiDelay = 700;
    Uint32 aiTick = 0;

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

    TTF_Font* font = TTF_OpenFont("C:/Windows/Fonts/arial.ttf", 20);
    if (!font) SDL_Log("TTF_OpenFont failed: %s", TTF_GetError());

    barricade::Game game(playerCount);
    game.startTurn();

    int selectedPawn = -1;
    std::string msg;
    bool skipPending = false;

    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (e.key.keysym.sym == SDLK_r) {
                    game = barricade::Game(playerCount);
                    game.startTurn();
                    selectedPawn = -1;
                    msg.clear();
                    skipPending = false;
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
                if (cx < 0 || cx >= barricade::kCols || cy < 0 || cy >= barricade::kRows) continue;

                if (game.pendingBarricade()) {
                    const auto cells = game.barricadePlacements();
                    if (std::find(cells.begin(), cells.end(), cell) != cells.end()) {
                        game.placeBarricade(cell);
                        selectedPawn = -1;
                        msg.clear();
                        skipPending = false;
                    } else {
                        setMessage(msg, "Case invalide pour la barricade");
                    }
                    continue;
                }

                const int pawnId = game.pawnAt(cell);
                bool clicked = false;
                if (pawnId >= 0 && pawnId / barricade::kPawnsPerPlayer == cur) {
                    const int pawn = pawnId % barricade::kPawnsPerPlayer;
                    if (!game.legalDestinations(cur, pawn).empty()) {
                        selectedPawn = pawn;
                        msg.clear();
                        clicked = true;
                    }
                } else {
                    for (int m = 0; m < barricade::kPawnsPerPlayer; ++m) {
                        if (game.pawnInBase(cur, m) && cell == barricade::baseCell(cur, m) &&
                            !game.legalDestinations(cur, m).empty()) {
                            selectedPawn = m;
                            msg.clear();
                            clicked = true;
                            break;
                        }
                    }
                }
                if (clicked) continue;

                if (selectedPawn >= 0) {
                    const auto dests = game.legalDestinations(cur, selectedPawn);
                    if (std::find(dests.begin(), dests.end(), cell) != dests.end()) {
                        if (game.movePawn(cur, selectedPawn, cell)) {
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

        // AI players play automatically, one action every kAiDelay ms.
        if (!game.isOver() && isAi(game.currentPlayer())) {
            if (game.dice() == 0) {
                game.startTurn();
                aiTick = SDL_GetTicks() + kAiDelay;
            } else if (SDL_GetTicks() >= aiTick) {
                selectedPawn = -1;
                if (game.pendingBarricade()) {
                    game.placeBarricade(barricade::naiveBarricadePlacement(game));
                } else {
                    const auto mv = barricade::mctsMove(game, game.currentPlayer(), 600);
                    if (mv.pawn >= 0) {
                        game.movePawn(game.currentPlayer(), mv.pawn, mv.dest);
                    }
                }
                aiTick = SDL_GetTicks() + kAiDelay;
            }
        }

        // auto-skip a player who has no legal move
        if (!game.isOver() && !game.pendingBarricade() && game.dice() > 0 &&
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

        drawBoard(renderer, font, game, selectedPawn, msg);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (font) TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    return EXIT_SUCCESS;
}