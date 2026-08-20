#pragma once

#include "board.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace barricade {

struct Neighbors {
    Point cells[4];
    int count = 0;
};

// Static orthogonal adjacency table over the track graph (public for the AI).
const std::array<std::array<Neighbors, kRows>, kCols>& neighbors();

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
    // Ceiling on the number of actions (moves + placements). A game that hits
    // it without a goal win is resolved by proximity (the player whose pawn is
    // closest to the goal wins), so a mutual blockade can never stall forever.
    static constexpr int kMaxActions = 500;
    bool deadlockEnded() const { return deadlock_; }

    void startTurn();  // rolls the die for the current player
    void nextTurn();   // advances to the next player and rolls
    void forceDice(int value) { dice_ = value; }  // test helper

    std::vector<Point> legalDestinations(int player, int pawn) const;
    // Hot-path variant: fills `out` (capacity maxOut) with distinct destinations.
    // `seen` must be a zero-initialized kCols*kRows buffer. Returns the count.
    int legalDestinationsTo(int player, int pawn, Point* out, int maxOut, char* seen) const;
    bool hasLegalMove(int player) const;
    bool movePawn(int player, int pawn, Point dest);
    // Same as movePawn but skips the legality check; caller must ensure `dest` is legal.
    bool movePawnFast(int player, int pawn, Point dest);
    std::vector<Point> barricadePlacements() const;
    bool placeBarricade(Point dest);
    // Same as placeBarricade but skips the legality check; caller must ensure
    // `dest` is legal (used by the AI search where the cell was already vetted).
    bool placeBarricadeFast(Point dest);

    Point pawnPos(int player, int pawn) const;  // base cell when in base
    bool pawnInBase(int player, int pawn) const;
    int pawnAt(Point p) const;  // player * kPawnsPerPlayer + pawn, or -1
    bool barricadeAt(Point p) const;
    const std::array<Point, kBarricadeCount>& barricades() const { return barricades_; }

    // Position notation (FEN-like): full game state on a single line so tests
    // can round-trip positions. Fields are ';'-separated key=value pairs:
    //   barricade;N=4;turn=0;dice=3;hand=0;over=0;winner=-1;P0=2,13|-|-|-|-;...
    //   ;P1=...;bars=8,1|8,3|...;act=3
    // Each player has kPawnsPerPlayer slots ('-' = in base, else x,y). `hand`
    // is 1 while a barricade is in hand (its cell is absent from `bars`).
    // `over`/`winner` let a finished game (goal or deadlock) round-trip too.
    std::string savePosition() const;
    bool loadPosition(const std::string& text);  // resets the game first; false on parse error

private:
    bool explore(Point cur, Point prev, int steps, Point* out, int& count, int maxOut,
                 int player, char* seen) const;
    bool ownPawnAt(Point p, int player) const;
    bool applyMove(int player, int pawn, Point dest);
    void resolveDeadlock();
    int deadlockWinner() const;
    void setPawn(Point p, int id);  // id player*5+pawn, or 255 = empty
    void setBarricade(Point p);
    void clearBarricade(Point p);

    int player_count_;
    int current_ = 0;
    int dice_ = 0;
    bool over_ = false;
    int winner_ = -1;
    bool pending_barricade_ = false;
    int captured_barricade_ = -1;
    int actions_ = 0;
    bool deadlock_ = false;

    std::array<std::array<Point, kPawnsPerPlayer>, kMaxPlayers> pawns_;
    std::array<Point, kBarricadeCount> barricades_;
    // Bit-packed barricade grid (1-bit bitmap) to shrink Game for the MCTS
    // tree; the pawn grid stays a direct byte array because 5-bit packing
    // slowed the hot `pawnAt` path.
    std::array<uint64_t, (kCols * kRows + 63) / 64> barricade_grid_{};
    std::array<std::array<uint8_t, kRows>, kCols> pawn_grid_{};  // player*5+pawn or 255
};

}  // namespace barricade