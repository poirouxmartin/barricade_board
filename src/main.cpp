#define SDL_MAIN_HANDLED

#include <SDL.h>

#include "game.h"

#include <cstdlib>

namespace {

constexpr int kCellSize = 80;
constexpr int kWindowSize = barricade::kBoardSize * kCellSize;

const SDL_Color kPlayerColors[barricade::kMaxPlayers] = {
    {230, 60, 60, 255},
    {60, 90, 230, 255},
    {240, 200, 40, 255},
    {60, 180, 70, 255},
};

const SDL_Color kGoalColor = {220, 220, 220, 255};
const SDL_Color kStartTint = {0, 0, 0, 255};
const SDL_Color kGridColor = {60, 60, 60, 255};
const SDL_Color kBoardColor = {215, 195, 150, 255};

bool pointInStartZone(barricade::Point p) {
    for (int player = 0; player < barricade::kMaxPlayers; ++player) {
        for (int m = 0; m < barricade::kMarblesPerPlayer; ++m) {
            const barricade::Point s = barricade::kStartZones[player][m];
            if (s.x == p.x && s.y == p.y) {
                return true;
            }
        }
    }
    return false;
}

void fillCircle(SDL_Renderer* renderer, int cx, int cy, int radius) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) {
                SDL_RenderDrawPoint(renderer, cx + dx, cy + dy);
            }
        }
    }
}

void drawBoard(SDL_Renderer* renderer, const barricade::Game& game) {
    SDL_SetRenderDrawColor(renderer, kBoardColor.r, kBoardColor.g, kBoardColor.b, 255);
    SDL_RenderClear(renderer);

    for (int y = 0; y < barricade::kBoardSize; ++y) {
        for (int x = 0; x < barricade::kBoardSize; ++x) {
            const barricade::Point p{x, y};
            const SDL_Rect cell{x * kCellSize, y * kCellSize, kCellSize, kCellSize};

            if (p.x == barricade::kGoal.x && p.y == barricade::kGoal.y) {
                SDL_SetRenderDrawColor(renderer, kGoalColor.r, kGoalColor.g, kGoalColor.b, 255);
                SDL_RenderFillRect(renderer, &cell);
            } else if (pointInStartZone(p)) {
                SDL_SetRenderDrawColor(renderer, kStartTint.r, kStartTint.g, kStartTint.b, 255);
                SDL_RenderDrawRect(renderer, &cell);
            }
        }
    }

    for (int i = 1; i < barricade::kBoardSize; ++i) {
        const int line = i * kCellSize;
        SDL_SetRenderDrawColor(renderer, kGridColor.r, kGridColor.g, kGridColor.b, 255);
        SDL_RenderDrawLine(renderer, line, 0, line, kWindowSize);
        SDL_RenderDrawLine(renderer, 0, line, kWindowSize, line);
    }

    for (int player = 0; player < game.playerCount(); ++player) {
        const SDL_Color& c = kPlayerColors[player];
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 255);
        for (int m = 0; m < barricade::kMarblesPerPlayer; ++m) {
            const barricade::Point pos = game.marblePos(player, m);
            const int cx = pos.x * kCellSize + kCellSize / 2;
            const int cy = pos.y * kCellSize + kCellSize / 2;
            fillCircle(renderer, cx, cy, kCellSize / 3);
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return EXIT_FAILURE;
    }

    SDL_Window* window = SDL_CreateWindow(
        "Barricade", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kWindowSize, kWindowSize, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    int playerCount = 4;
    if (argc > 1) {
        playerCount = std::atoi(argv[1]);
        if (playerCount < 2 || playerCount > 4) {
            playerCount = 4;
        }
    }

    barricade::Game game(playerCount);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                running = false;
            }
        }

        drawBoard(renderer, game);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return EXIT_SUCCESS;
}