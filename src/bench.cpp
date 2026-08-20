// MCTS throughput benchmark. Runs repeated searches from a fixed mid-game
// position and reports iterations/sec, rollouts/sec (rollout mode) and the
// average iterations per call, for a given budget, thread count and optional
// neural network.
//
//   bench [--ms N] [--budget ms] [--threads N] [--players N] [--dice N]
//         [--net weights.bin]

#include "ai.h"
#include "game.h"
#include "nn.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace barricade;

int main(int argc, char* argv[]) {
    int ms = 4000;
    int budgetMs = 200;
    int threads = 0;
    int players = 2;
    int dice = 3;
    std::string netFile;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--ms" && i + 1 < argc) {
            ms = std::atoi(argv[++i]);
        } else if (a == "--budget" && i + 1 < argc) {
            budgetMs = std::atoi(argv[++i]);
        } else if (a == "--threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
        } else if (a == "--players" && i + 1 < argc) {
            players = std::atoi(argv[++i]);
        } else if (a == "--dice" && i + 1 < argc) {
            dice = std::atoi(argv[++i]);
        } else if (a == "--net" && i + 1 < argc) {
            netFile = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: bench [--ms N] [--budget ms] [--threads N] [--players N] "
                         "[--dice N] [--net weights.bin]\n");
            return 1;
        }
    }
    if (players < 2 || players > 4 || dice < 1 || dice > 6) {
        std::fprintf(stderr, "players must be 2..4 and dice 1..6\n");
        return 1;
    }

    nn::NeuralNet net;
    if (!netFile.empty()) {
        if (!net.load(netFile)) {
            std::fprintf(stderr, "cannot load %s\n", netFile.c_str());
            return 1;
        }
        setMctsNetwork(&net);
    }

    Game g(players);
    g.startTurn();
    g.forceDice(dice);

    mctsActionStats(g, 0, 50, threads);  // warm up caches and worker pools

    long long totalIter = 0;
    long long totalRoll = 0;
    long long calls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        const long long roll0 = mctsRolloutCount();
        mctsActionStats(g, 0, budgetMs, threads);
        totalIter += mctsIterationCount();
        totalRoll += mctsRolloutCount() - roll0;
        ++calls;
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() >= ms) break;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (secs <= 0.0) return 0;

    const double iterPerSec = static_cast<double>(totalIter) / secs;
    std::printf("players=%d threads=%d budget=%dms calls=%lld net=%s\n", players,
                threads > 0 ? threads : (int)std::thread::hardware_concurrency(), budgetMs, calls,
                netFile.empty() ? "rollout" : netFile.c_str());
    std::printf("iterations/s: %.0f\n", iterPerSec);
    if (netFile.empty()) std::printf("rollouts/s:   %.0f\n", static_cast<double>(totalRoll) / secs);
    std::printf("avg iters/call: %.0f\n", static_cast<double>(totalIter) / static_cast<double>(calls));
    return 0;
}