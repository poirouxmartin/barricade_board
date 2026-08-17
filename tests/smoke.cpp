#include "game.h"

#include <cstdio>

using namespace barricade;

int failures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::printf("FAIL: %s\n", msg);
        ++failures;
    }
}

bool has(const std::vector<Point>& v, Point p) {
    for (const Point& q : v) {
        if (q == p) return true;
    }
    return false;
}

int main() {
    // A pawn in base can always leave the base.
    {
        Game g(4);
        g.startTurn();
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            check(!g.legalDestinations(0, m).empty(), "base pawn has a move");
        }
    }

    // Straight movement from the bottom row; no immediate double-back.
    {
        Game g(4);
        g.forceDice(1);
        g.movePawn(0, 0, {2, 13});  // red front cell
        g.forceDice(2);
        const auto d = g.legalDestinations(0, 0);
        check(has(d, {4, 13}), "roll 2 reaches x+2 on bottom row");
        check(!has(d, {2, 13}), "no double-back to start cell");
    }

    // Capturing: landing on an opponent pawn sends it back to its base.
    {
        Game g(2);
        g.forceDice(1);
        g.movePawn(0, 0, {2, 13});
        g.forceDice(1);
        g.movePawn(1, 0, {6, 13});  // blue pawn steps out in front of its base
        g.forceDice(4);
        check(g.movePawn(0, 0, {6, 13}), "capture move legal");
        check(g.pawnInBase(1, 0), "captured pawn returns to base");
        check(g.pawnPos(0, 0) == Point{6, 13}, "capturing pawn occupies the cell");
    }

    // Passing over own pawn is allowed, landing on it is not.
    {
        Game g(4);
        g.forceDice(1);
        g.movePawn(0, 0, {2, 13});
        g.forceDice(2);  // from base: step 1 = (2,13), step 2 = (3,13)
        const auto d = g.legalDestinations(0, 1);
        check(has(d, {3, 13}), "passes over own pawn to reach x+1");
        check(!has(d, {2, 13}), "cannot land on own pawn");
    }

// Landing on a barricade captures it and requires placement.
    {
        Game g(2);
        g.forceDice(1);
        g.movePawn(0, 0, {2, 13});  // red out
        g.forceDice(1);
        g.movePawn(1, 0, {6, 13});  // blue out, red to play
        g.forceDice(4);             // (3,13),(4,13),(4,12),(4,11): barricade at (4,11)
        check(g.movePawn(0, 0, {4, 11}), "exact landing on barricade cell");
        check(g.pendingBarricade(), "barricade capture pending placement");
        const auto cells = g.barricadePlacements();
        for (const Point& c : cells) {
            check(c.y != kBottomRowY, "barricade never placed on bottom row");
            check(!isGoalCell(c.x, c.y), "barricade never placed on goal");
            check(isTrackCell(c.x, c.y), "barricade placed on a track cell");
        }
        check(!cells.empty(), "at least one placement cell");
        check(g.placeBarricade(cells[0]), "barricade placed");
        check(!g.pendingBarricade(), "placement resolved");
        check(g.barricadeAt(cells[0]), "barricade now at chosen cell");
    }

    if (failures == 0) {
        std::printf("All smoke tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", failures);
    return 1;
}



