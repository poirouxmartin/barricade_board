#include "ai.h"
#include "game.h"
#include "nn.h"

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

    // A "clean block" regression guard: red leads via the LEFT border, blue
    // via the RIGHT, red holds a barricade. The placement must slow blue's
    // pawn down while leaving red's own route untouched (the exact BFS scorer
    // keeps the goal-adjacent barricade (8,1) out of its blocked set, which is
    // what makes the distances finite in the first place).
    {
        Game g(2);
        g.movePawnFast(0, 0, {1, 3});
        g.movePawnFast(1, 0, {15, 3});
        g.movePawnFast(0, 0, {0, 3});
        g.movePawnFast(0, 0, {0, 2});
        g.movePawnFast(0, 0, {0, 1});
        g.movePawnFast(0, 0, {1, 1});  // red leader on the left corridor
        g.movePawnFast(1, 0, {16, 3});
        g.movePawnFast(1, 0, {16, 2});
        g.movePawnFast(1, 0, {16, 1});
        g.movePawnFast(1, 0, {15, 1});  // blue leader on the right corridor
        g.movePawnFast(0, 1, {4, 11});  // capture barricade -> pending
        check(g.pendingBarricade(), "clean block setup: barricade pending");
        check(has(g.barricadePlacements(), cheapBarricadePlacement(g, dynamicGoalDist(g))),
              "cheap placement is a legal cell");
        const auto recs = barricadeRecommendations(g, 6);
        check(!recs.empty() && recs[0].cell.x >= 9, "exact scorer prefers the opponent's side");
        const auto before = dynamicGoalDist(g);
        Game placed = g;
        const Point c = cheapBarricadePlacement(g, before);
        check(placed.placeBarricadeFast(c), "cheap placement applies");
        const auto redBefore = playerArmyDistances(g, 0)[0];
        const auto redAfter = playerArmyDistances(placed, 0)[0];
        const auto blueBefore = playerArmyDistances(g, 1)[0];
        const auto blueAfter = playerArmyDistances(placed, 1)[0];
        constexpr int kInf = 1000000;
        check(redBefore < kInf && redBefore == redAfter, "clean block does not slow our own leader");
        check(blueBefore < kInf && blueAfter > blueBefore, "clean block slows the opponent leader");
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

    // With a barricade pending, the MCTS branches on placements: it must
    // return a barricade cell (pawn == -1) that is a legal placement.
    {
        Game g(2);
        g.movePawnFast(0, 0, {4, 12});  // red on track
        g.movePawnFast(1, 0, {6, 13});  // blue out
        g.movePawnFast(0, 0, {4, 11});  // capture barricade (4,11) -> pending
        check(g.pendingBarricade(), "placement node setup: barricade pending");
        const auto mv = mctsMove(g, g.currentPlayer(), 400);
        check(mv.pawn < 0 && mv.dest.x >= 0, "mcts returns a placement with a pending barricade");
        if (mv.pawn < 0 && mv.dest.x >= 0) {
            check(has(g.barricadePlacements(), mv.dest), "mcts placement cell is legal");
            check(g.placeBarricade(mv.dest), "mcts placement applies");
        }
    }

    // Advice on a placement root must return placement recommendations: the
    // freeze bug filtered every placement out of mctsRecommendations because
    // placement actions carry pawn == -1 (regression guard for the advice UI).
    {
        Game g(2);
        g.movePawnFast(0, 0, {4, 12});
        g.movePawnFast(1, 0, {6, 13});
        g.movePawnFast(0, 0, {4, 11});  // capture barricade -> pending
        check(g.pendingBarricade(), "placement advice setup: barricade pending");
        const auto recs = mctsRecommendations(g, g.currentPlayer(), 300);
        bool anyPlacement = false;
        for (const auto& r : recs) {
            if (r.move.pawn < 0 && r.move.dest.x >= 0) {
                check(has(g.barricadePlacements(), r.move.dest), "placement advice cell is legal");
                anyPlacement = true;
            }
        }
        check(anyPlacement, "advice returns at least one placement recommendation");
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

    // Turn cap: a mutual blockade that never reaches the goal must resolve by
    // proximity (closest pawn wins) instead of stalling forever.
    {
        Game g(2);
        g.forceDice(1);
        // Two safe track cells per player, far from the goal and from any
        // barricade: pawns shuffle back and forth, the goal is never reached.
        const Point cells[2][2] = {{{6, 5}, {6, 6}}, {{10, 5}, {10, 6}}};
        int step = 0;
        while (!g.isOver() && step < barricade::Game::kMaxActions + 50) {
            const int p = g.currentPlayer();
            if (!g.movePawnFast(p, 0, cells[p][step % 2])) break;
            ++step;
        }
        check(g.isOver(), "stalled game hits the action cap");
        check(g.deadlockEnded(), "cap end is flagged as a deadlock");
        check(g.winner() >= 0 && g.winner() < g.playerCount(),
              "deadlock resolution picks a real player");
    }

// Position notation: round-trip a fresh game (identical re-save).
    {
        Game g(4);
        g.startTurn();
        g.forceDice(3);
        const std::string s = g.savePosition();
        Game g2(4);
        check(g2.loadPosition(s), "load fresh position");
        check(g2.playerCount() == 4 && g2.currentPlayer() == g.currentPlayer(),
              "fresh round-trip keeps player/turn");
        check(g2.dice() == g.dice(), "fresh round-trip keeps dice");
        check(g2.savePosition() == s, "fresh position re-saves identically");
    }

    // Mid-game: pawns out and a captured barricade in hand.
    {
        Game g(2);
        g.forceDice(1); g.movePawnFast(0, 0, {2, 13});  // red leaves the base
        g.forceDice(1); g.movePawnFast(1, 0, {6, 13});  // green leaves the base
        g.forceDice(4); g.movePawnFast(0, 0, {4, 11});  // red captures (4,11)
        check(g.pendingBarricade(), "setup: barricade in hand");
        const std::string s = g.savePosition();
        Game g2(2);
        check(g2.loadPosition(s), "load mid-game position");
        check(g2.pendingBarricade(), "mid-game keeps barricade in hand");
        check(g2.currentPlayer() == 0 && g2.dice() == 4, "mid-game keeps turn/dice");
        check(g2.pawnPos(0, 0) == Point{4, 11}, "mid-game keeps capturing pawn");
        check(!g2.barricadeAt({4, 11}), "captured barricade cell stays empty");
        check(g2.savePosition() == s, "mid-game position re-saves identically");
    }

    // Finished game: goal reached, winner recorded.
    {
        const std::string s =
            "barricade;N=2;turn=0;dice=3;hand=0;over=1;winner=0;"
            "P0=8,0|-|-|-|-;P1=6,13|-|-|-|-;"
            "bars=8,1|8,3|8,4|8,5|6,7|10,7|0,11|4,11|8,11|12,11|16,11;act=41";
        Game g(2);
        check(g.loadPosition(s), "load finished position");
        check(g.isOver() && g.winner() == 0, "finished position keeps winner");
        check(g.savePosition() == s, "finished position re-saves identically");
    }

    // Malformed positions are rejected without corrupting the game.
    {
        Game g(2);
        const std::string all = "8,1|8,3|8,4|8,5|6,7|10,7|0,11|4,11|8,11|12,11|16,11";
        check(!g.loadPosition("garbage"), "reject bad header");
        check(!g.loadPosition("barricade;N=2;turn=0;dice=7;hand=0;over=0;winner=-1;"
                              "P0=-|-|-|-|-;P1=-|-|-|-|-;bars=" + all + ";act=0"),
              "reject bad dice");
        check(!g.loadPosition("barricade;N=2;turn=0;dice=3;hand=0;over=0;winner=-1;"
                              "P0=-|-|-|-|-;P1=-|-|-|-|-;bars=8,1|8,3|8,4|8,5|6,7|10,7|"
                              "0,11|4,11|8,11|12,11;act=0"),
              "reject wrong barricade count");
        check(!g.loadPosition("barricade;N=2;turn=0;dice=3;hand=0;over=0;winner=-1;"
                              "P0=8,1|-|-|-|-;P1=-|-|-|-|-;bars=" + all + ";act=0"),
              "reject pawn standing on a barricade");
        check(!g.loadPosition("barricade;N=2;turn=0;dice=3;hand=0;over=0;winner=-1;"
                              "P0=6,13|-|-|-|-;P1=6,13|-|-|-|-;bars=" + all + ";act=0"),
              "reject two pawns on the same cell");
        check(g.playerCount() == 2 && !g.isOver(), "failed loads leave a valid reset game");
    }

    // Neural network: forward determinism, sane zero-net output and featurize.
    {
        nn::NeuralNet net;
        Game g(2);
        g.startTurn();
        g.forceDice(3);
        const nn::NetOut a = net.evaluate(g);
        const nn::NetOut b = net.evaluate(g);
        check(a.value[0] > 0.1f && a.value[0] < 0.4f, "zero net value near uniform");
        check(a.value[0] == b.value[0] && a.policy[0] == b.policy[0],
              "forward is deterministic");
        float vs = 0.0f;
        for (int k = 0; k < kMaxPlayers; ++k) vs += a.value[k];
        check(vs > 0.99f && vs < 1.01f, "value head sums to one");
        std::vector<float> pl(static_cast<size_t>(nn::kPlanes) * nn::kHW);
        nn::featurize(g, pl.data());
        float sum = 0.0f;
        for (float v : pl) sum += v;
        check(sum > 0.0f, "featurize produces a non-empty input");
        check(pl[5 * nn::kHW + 0 * nn::kColsN + 8] == 1.0f, "goal plane set");
        check(pl[11 * nn::kHW] == g.dice() / 6.0f, "dice plane set");
        check(pl[7 * nn::kHW + 0] == 1.0f && pl[8 * nn::kHW + 0] == 0.0f,
              "current-player plane set");
    }

    // Neural network: save/load round-trips the parameters.
    {
        nn::NeuralNet a, b;
        a.initRandom(42);
        const std::string tmp = "nn_rt_test.bin";
        check(a.save(tmp), "nn save");
        check(b.load(tmp), "nn load");
        bool same = true;
        const float* pa = a.params();
        const float* pb = b.params();
        for (size_t i = 0; i < a.paramCount(); ++i) {
            if (pa[i] != pb[i]) {
                same = false;
                break;
            }
        }
        check(same, "nn params round-trip");
        std::remove(tmp.c_str());
    }

    // MCTS runs with a (random) network installed; without a network the
    // rollout path is untouched (all the tests above already ran it).
    {
        nn::NeuralNet net;
        net.initRandom(1);
        setMctsNetwork(&net);
        Game g(2);
        g.startTurn();
        g.forceDice(3);
        const auto stats = mctsActionStats(g, 0, 100);
        setMctsNetwork(nullptr);
        check(!stats.empty(), "neural MCTS returns actions");
        bool anyVisits = false;
        for (const ActionStats& s : stats) {
            if (s.visits > 0) anyVisits = true;
        }
        check(anyVisits, "neural MCTS actions have visits");
    }

    // Training step: finite loss and a changed weight vector.
    {
        nn::NeuralNet net;
        net.initRandom(2);
        net.setLearningRate(0.1f);
        std::vector<float> pl(static_cast<size_t>(nn::kPlanes) * nn::kHW, 0.0f);
        std::vector<float> pol(nn::kPolicySize, 0.0f);
        pol[nn::policyIndex(0, Point{2, 13})] = 1.0f;
        const float* planes[1] = {pl.data()};
        const float* pols[1] = {pol.data()};
        int wins[1] = {0};
        const float loss = net.trainBatch(planes, pols, wins, 1);
        check(loss == loss && loss < 10.0f, "trainBatch finite loss");
        float diff = 0.0f;
        const float* p = net.params();
        for (size_t i = 0; i < net.paramCount(); ++i) diff += p[i] * p[i];
        check(diff > 0.0f, "trainBatch leaves non-zero params");
    }

if (failures == 0) {
        std::printf("All smoke tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) failed.\n", failures);
    return 1;
}



