// Self-play data generator for the policy/value network. Plays whole games
// with the MCTS (rollout-based or neural, depending on --net) and writes one
// line per position:
//   <position (savePosition)>\t<slot>@x,y:visits;...\t<winner>
// `slot` is the pawn index (0..4) or 5 for a barricade placement; the visits
// are the MCTS root statistics used as the policy training target.

#include "ai.h"
#include "game.h"
#include "nn.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace barricade;

namespace {
std::mt19937& rng() {
    static std::mt19937 g(std::random_device{}());
    return g;
}

// Returns the index into `stats` of the move to play: argmax visits, or a
// sample from the visit distribution with probability `eps`.
int pickAction(const std::vector<ActionStats>& stats, double eps) {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    if (uni(rng()) >= eps) {
        long long best = -1;
        int pick = 0;
        for (size_t i = 0; i < stats.size(); ++i) {
            if (stats[i].visits > best) {
                best = stats[i].visits;
                pick = static_cast<int>(i);
            }
        }
        return pick;
    }
    long long total = 0;
    for (const ActionStats& s : stats) total += s.visits;
    if (total <= 0) return 0;
    long long r = static_cast<long long>(rng()() % static_cast<unsigned long long>(total));
    long long acc = 0;
    for (size_t i = 0; i < stats.size(); ++i) {
        acc += stats[i].visits;
        if (r < acc) return static_cast<int>(i);
    }
    return 0;
}
}  // namespace

int main(int argc, char* argv[]) {
    int games = 50;
    int budgetMs = 400;
    int players = 2;
    const char* outFile = "data.txt";
    const char* netFile = nullptr;
    double eps = 0.25;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--net" && i + 1 < argc) {
            netFile = argv[++i];
        } else if (a == "--eps" && i + 1 < argc) {
            eps = std::atof(argv[++i]);
        } else if (a == "--games" && i + 1 < argc) {
            games = std::atoi(argv[++i]);
        } else if (a == "--budget" && i + 1 < argc) {
            budgetMs = std::atoi(argv[++i]);
        } else if (a == "--players" && i + 1 < argc) {
            players = std::atoi(argv[++i]);
        } else if (a == "--out" && i + 1 < argc) {
            outFile = argv[++i];
        } else {
            std::fprintf(stderr,
                         "usage: selfplay [--games N] [--budget ms] [--players N] [--eps x] "
                         "[--net weights.bin] [--out data.txt]\n");
            return 1;
        }
    }

    nn::NeuralNet net;
    if (netFile) {
        if (!net.load(netFile)) {
            std::fprintf(stderr, "cannot load %s\n", netFile);
            return 1;
        }
        setMctsNetwork(&net);
    }

    std::ofstream out(outFile, std::ios::app);
    if (!out) {
        std::fprintf(stderr, "cannot open %s for append\n", outFile);
        return 1;
    }

    struct Sample {
        std::string pos;
        std::vector<int> ids;  // index into the stats of the searched position
        std::vector<int> slots;
        std::vector<Point> cells;
        std::vector<long long> visits;
    };

    long long positions = 0;
    for (int gi = 0; gi < games; ++gi) {
        Game g(players);
        g.startTurn();
        std::vector<Sample> samples;
        while (!g.isOver()) {
            if (g.dice() <= 0) g.startTurn();
            const int cp = g.currentPlayer();
            const auto stats = mctsActionStats(g, cp, budgetMs);
            if (stats.empty()) {  // no legal move: pass to the next player
                g.nextTurn();
                continue;
            }

            Sample s;
            s.pos = g.savePosition();
            for (size_t i = 0; i < stats.size(); ++i) {
                const ActionStats& a = stats[i];
                if (a.visits <= 0) continue;
                if (a.move.pawn < 0 && a.move.dest.x < 0) continue;  // skip pseudo-action
                s.ids.push_back(static_cast<int>(i));
                s.slots.push_back(a.move.pawn < 0 ? 5 : a.move.pawn);
                s.cells.push_back(a.move.dest);
                s.visits.push_back(a.visits);
            }
            if (s.visits.empty()) {
                g.nextTurn();
                continue;
            }

            const int pick = pickAction(stats, eps);
            const AIMove mv = stats[pick].move;
            if (mv.pawn < 0) {
                g.placeBarricade(mv.dest);
            } else {
                g.movePawn(cp, mv.pawn, mv.dest);
            }
            samples.push_back(std::move(s));
        }

        const int winner = g.winner();
        for (const Sample& s : samples) {
            out << s.pos << '\t';
            for (size_t i = 0; i < s.visits.size(); ++i) {
                if (i) out << ';';
                out << s.slots[i] << '@' << s.cells[i].x << ',' << s.cells[i].y << ':' << s.visits[i];
            }
            out << '\t' << winner << '\n';
            ++positions;
        }
    }
    out.close();
    std::printf("wrote %lld positions across %d games\n", positions, games);
    return 0;
}