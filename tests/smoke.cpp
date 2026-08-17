#include "ai.h"
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
    // Board geometry matches the knauzi/Malefiz reference (Board.java).
    {
        const std::vector<std::vector<int>> refTrack{
            {8},                                                         // y=0 goal
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},  // y=1
            {0, 16},                                                     // y=2
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},  // y=3
            {8},                                                         // y=4
            {6, 7, 8, 9, 10},                                            // y=5
            {6, 10},                                                     // y=6
            {4, 5, 6, 7, 8, 9, 10, 11, 12},                              // y=7
            {4, 12},                                                     // y=8
            {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14},                // y=9
            {2, 6, 10, 14},                                              // y=10
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},  // y=11
            {0, 4, 8, 12, 16},                                           // y=12
            {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},  // y=13
        };
        const std::vector<Point> refBarricades{
            {8, 1}, {8, 3}, {8, 4}, {8, 5}, {6, 7}, {10, 7},
            {0, 11}, {4, 11}, {8, 11}, {12, 11}, {16, 11},
        };
        for (int y = 0; y < 14; ++y) {
            for (int x = 0; x < 17; ++x) {
                bool inRef = false;
                for (int rx : refTrack[y]) {
                    if (rx == x) inRef = true;
                }
                if (isTrackCell(x, y) != inRef) {
                    std::printf("FAIL track (%d,%d): got=%d ref=%d\n", x, y, isTrackCell(x, y), inRef);
                    ++failures;
                }
            }
        }
        for (const Point& p : refBarricades) {
            if (!isInitialBarricadeCell(p.x, p.y)) {
                std::printf("FAIL barricade missing (%d,%d)\n", p.x, p.y);
                ++failures;
            }
        }
        for (int y = 0; y < 14; ++y) {
            for (int x = 0; x < 17; ++x) {
                bool inRef = false;
                for (const Point& b : refBarricades) {
                    if (b.x == x && b.y == y) inRef = true;
                }
                if (isInitialBarricadeCell(x, y) != inRef) {
                    std::printf("FAIL stray barricade (%d,%d)\n", x, y);
                    ++failures;
                }
            }
        }
        check(isGoalCell(8, 0), "goal at (8,0)");
    }

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

    // Naive AI returns a legal move for the current player.
    {
        Game g(2);
        g.forceDice(3);
        const AIMove mv = naiveMove(g, g.currentPlayer());
        check(mv.pawn >= 0, "ai picks a pawn");
        if (mv.pawn >= 0) {
            check(has(g.legalDestinations(g.currentPlayer(), mv.pawn), mv.dest), "ai dest is legal");
        }
    }

    // Naive AI barricade placement is a legal placement.
    {
        Game g(2);
        g.forceDice(1);
        g.movePawn(0, 0, {2, 13});
        g.forceDice(1);
        g.movePawn(1, 0, {6, 13});
        g.forceDice(4);
        check(g.movePawn(0, 0, {4, 11}), "capture barricade to test ai placement");
        check(g.pendingBarricade(), "pending barricade");
        const auto cells = g.barricadePlacements();
        const Point c = naiveBarricadePlacement(g);
        check(has(cells, c), "ai barricade placement is legal");
        check(g.placeBarricade(c), "ai placement applied");
    }

    // MCTS returns a legal, applicable move for the current player.
    {
        Game g(2);
        g.forceDice(3);
        const AIMove mv = mctsMove(g, g.currentPlayer(), 200);
        check(mv.pawn >= 0, "mcts picks a pawn");
        if (mv.pawn >= 0) {
            check(has(g.legalDestinations(g.currentPlayer(), mv.pawn), mv.dest), "mcts dest is legal");
            check(g.movePawn(g.currentPlayer(), mv.pawn, mv.dest), "mcts move applies");
        }
    }

    // MCTS is legal and applicable when a capture is available.
    {
        Game g(2);
        g.forceDice(1);
        g.movePawn(0, 0, {2, 13});
        g.forceDice(1);
        g.movePawn(1, 0, {6, 13});
        g.forceDice(4);
        const auto mv = mctsMove(g, g.currentPlayer(), 300);
        check(mv.pawn >= 0, "mcts picks a pawn with capture available");
        if (mv.pawn >= 0) {
            check(has(g.legalDestinations(g.currentPlayer(), mv.pawn), mv.dest), "mcts dest is legal");
            check(g.movePawn(g.currentPlayer(), mv.pawn, mv.dest), "mcts move applies");
        }
    }

    if (failures == 0) {
        std::printf("All smoke tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", failures);
    return 1;
}



