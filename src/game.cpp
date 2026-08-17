#include "game.h"

namespace barricade {

Game::Game(int playerCount) : player_count_(playerCount) {
    reset();
}

void Game::reset() {
    for (int p = 0; p < kMaxPlayers; ++p) {
        for (int m = 0; m < kMarblesPerPlayer; ++m) {
            marbles_[p][m] = kStartZones[p][m];
        }
    }
}

}  // namespace barricade