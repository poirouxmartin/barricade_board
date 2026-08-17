#pragma once

#include "board.h"

#include <array>
#include <vector>

namespace barricade {

class Game {
public:
    explicit Game(int playerCount);

    void reset();
    int playerCount() const { return player_count_; }
    int currentPlayer() const { return current_; }
    int dice() const { return dice_; }
    bool isOver() const { return over_; }
    int winner() const { return winner_; }
    bool pendingBarricade() const { return pending_barricade_; }

    void startTurn();  // rolls the die for the current player
    void nextTurn();   // advances to the next player and rolls
    void forceDice(int value) { dice_ = value; }  // test helper

    std::vector<Point> legalDestinations(int player, int pawn) const;
    bool hasLegalMove(int player) const;
    bool movePawn(int player, int pawn, Point dest);
    std::vector<Point> barricadePlacements() const;
    bool placeBarricade(Point dest);

    Point pawnPos(int player, int pawn) const;  // base cell when in base
    bool pawnInBase(int player, int pawn) const;
    int pawnAt(Point p) const;  // player * kPawnsPerPlayer + pawn, or -1
    bool barricadeAt(Point p) const;
    const std::array<Point, kBarricadeCount>& barricades() const { return barricades_; }

private:
    void explore(Point cur, Point prev, int steps, std::vector<Point>& out, int player) const;
    bool ownPawnAt(Point p, int player) const;

    int player_count_;
    int current_ = 0;
    int dice_ = 0;
    bool over_ = false;
    int winner_ = -1;
    bool pending_barricade_ = false;
    int captured_barricade_ = -1;

    std::array<std::array<Point, kPawnsPerPlayer>, kMaxPlayers> pawns_;
    std::array<Point, kBarricadeCount> barricades_;
};

}  // namespace barricade