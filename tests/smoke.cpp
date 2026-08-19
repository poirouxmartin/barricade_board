#include "ai.h"
#include "game.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <thread>
#include <vector>

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

    // The cell the capturing pawn just left is a valid placement target.
    {
        Game g(2);
        g.movePawnFast(0, 0, {4, 12});  // red on track, above the bottom row
        g.movePawnFast(1, 0, {6, 13});  // blue out
        g.movePawnFast(0, 0, {4, 11});  // red lands on barricade (4,11) -> capture
        check(g.pendingBarricade(), "barricade capture pending placement");
        const auto cells = g.barricadePlacements();
        check(has(cells, {4, 12}), "departure cell is a valid placement target");
        check(g.placeBarricade({4, 12}), "barricade placed on the departure cell");
    }

    // The cell the capturing pawn landed on (now occupied) is forbidden.
    {
        Game g(2);
        g.movePawnFast(0, 0, {4, 12});
        g.movePawnFast(1, 0, {6, 13});
        g.movePawnFast(0, 0, {4, 11});
        check(g.pendingBarricade(), "pending placement");
        check(!has(g.barricadePlacements(), {4, 11}), "occupied arrival cell is forbidden");
    }

    // Ranked barricade recommendations are legal and sorted best-first.
    {
        Game g(2);
        g.movePawnFast(0, 0, {4, 12});
        g.movePawnFast(1, 0, {6, 13});
        g.movePawnFast(0, 0, {4, 11});  // capture barricade at (4,11)
        check(g.pendingBarricade(), "pending barricade");
        const auto recs = barricadeRecommendations(g, 6);
        check(!recs.empty(), "barricade recommendations non-empty");
        const auto cells = g.barricadePlacements();
        for (size_t i = 0; i < recs.size(); ++i) {
            check(has(cells, recs[i].cell), "recommended cell is legal");
            if (i > 0) check(recs[i - 1].score >= recs[i].score, "recommendations sorted");
        }
        check(recs.size() <= 6, "recommendations capped at topN");
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

    // End-game simulation always produces a full probability distribution.
    {
        Game g(4);
        g.forceDice(3);
        const auto p = simulateWinChances(g, 4000);
        check(p.size() == 4, "simulation covers all 4 players");
        double sum = 0.0;
        for (double v : p) sum += v;
        check(sum > 0.99 && sum < 1.01, "simulation shares sum to 1");
    }

    // Simulation from a fresh 2-player game runs to completion (no stall).
    {
        Game g(2);
        g.forceDice(3);
        const auto p = simulateWinChances(g, 2000);
        double sum = 0.0;
        for (double v : p) sum += v;
        check(sum > 0.99 && sum < 1.01, "2-player simulation shares sum to 1");
    }

    // Simulation restarts on demand: the async worker counts games.
    {
        Game g(4);
        g.forceDice(3);
        std::atomic<bool> stop{false};
        std::vector<std::atomic<long long>> wins(4);
        std::atomic<long long> games{0};
        std::thread th([&] {
            simulateWinChancesAsync(g, 20000, &stop, wins.data(), g.playerCount(), &games, 2);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        stop.store(true);
        th.join();
        check(games.load() > 0, "async simulation ran games");
        check(games.load() <= 20000, "async simulation stops at target");
    }

    // A player one step from the goal wins the vast majority of simulated
    // end-games: the rollout policy must always take the immediate win instead
    // of moving a random pawn (regression: cheapMove wasted the turn and the
    // shares dropped to ~50% for the player about to win).
    {
        std::vector<int> dist(kCols * kRows, 1000000);
        std::queue<Point> q;
        dist[0 * kCols + 8] = 0;
        q.push({8, 0});
        while (!q.empty()) {
            const Point cur = q.front();
            q.pop();
            const int d = dist[cur.y * kCols + cur.x];
            const Neighbors& nb = neighbors()[cur.x][cur.y];
            for (int i = 0; i < nb.count; ++i) {
                const Point np = nb.cells[i];
                const int idx = np.y * kCols + np.x;
                if (dist[idx] == 1000000) {
                    dist[idx] = d + 1;
                    q.push(np);
                }
            }
        }

        Game g(4);
        g.startTurn();
        while (g.currentPlayer() != 3) g.nextTurn();
        for (int guard = 0; guard < 80; ++guard) {
            g.forceDice(1);
            const auto dests = g.legalDestinations(3, 0);
            if (dests.empty()) break;
            int bestD = 1000000;
            Point best = dests[0];
            for (const Point& d : dests) {
                const int sc = dist[d.y * kCols + d.x];
                if (sc < bestD) {
                    bestD = sc;
                    best = d;
                }
            }
            if (!g.movePawn(3, 0, best)) break;
            if (g.isOver()) break;
            if (g.pendingBarricade()) {
                // Place the captured barricade far from green so the corridor
                // stays open, like a human would.
                const auto cells = g.barricadePlacements();
                const Point gp = g.pawnPos(3, 0);
                Point far = cells[0];
                int farD = -1;
                for (const Point& c : cells) {
                    const int md = std::abs(c.x - gp.x) + std::abs(c.y - gp.y);
                    if (md > farD) {
                        farD = md;
                        far = c;
                    }
                }
                if (!g.placeBarricade(far)) break;
            }
            if (g.pawnPos(3, 0).x == 8 && g.pawnPos(3, 0).y == 1) break;
            while (g.currentPlayer() != 3) g.nextTurn();
        }
        const Point pp = g.pawnPos(3, 0);
        check(pp.x == 8 && pp.y == 1, "setup: green sits one step from the goal");
        const auto p = simulateWinChances(g, 8000);
        check(p[3] > 0.9, "green one step from the goal wins the simulated games");
        const auto st = winChances(g);
        check(st[3] > 0.9, "static heuristic also reads the one-step position as a sure win");
    }

    // A fresh symmetric game must not show a big starting-player bias: the
    // greedy rollout used to break distance ties toward the right, which made
    // the left side of the board win ~2/3 of the simulated games (red ~50%).
    {
        Game g(4);
        g.startTurn();
        const auto p = simulateWinChances(g, 12000);
        for (int i = 0; i < 4; ++i) {
            check(p[i] < 0.42, "no player gets a big starting advantage in the simulation");
        }
    }

    if (failures == 0) {
        std::printf("All smoke tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", failures);
    return 1;
}



