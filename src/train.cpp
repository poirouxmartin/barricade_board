// Offline trainer for the policy/value network. Reads the self-play data
// produced by selfplay (one position per line: <pos>\t<actions>\t<winner>),
// runs Adam mini-batches over the whole set for a few epochs, and writes the
// trained weights for the MCTS to consume.

#include "game.h"
#include "nn.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace barricade;

namespace {

struct Rec {
    std::vector<float> planes;  // kPlanes*kHW
    std::vector<float> pol;     // kPolicySize visit distribution (sparse)
    int winner = -1;
    int players = 0;
};

// The board is mirror-symmetric about the vertical axis (x -> kColsN-1-x).
// Under the mirror the base columns pair up as 2<->14 and 6<->10, i.e. player
// p maps to player 3-p, so the pawn and turn planes swap accordingly and the
// winner label is mirrored. This only produces a reachable position when all
// four players exist, so it is applied to 4-player samples only. The policy
// target mirrors spatially with the pawn slot unchanged (the pawn indices are
// the current player's, and the mirrored game just has the mirrored current
// player).
Rec mirror(const Rec& in) {
    Rec out;
    out.players = in.players;
    out.winner = in.winner < 0 ? -1 : 3 - in.winner;
    out.planes.assign(static_cast<size_t>(nn::kPlanes) * nn::kHW, 0.0f);
    const int mx = nn::kColsN - 1;
    for (int y = 0; y < nn::kRowsN; ++y) {
        for (int x = 0; x < nn::kColsN; ++x) {
            const int mi = y * nn::kColsN + mx - x;
            const int oi = y * nn::kColsN + x;
            for (int p = 0; p < 4; ++p) {
                out.planes[(3 - p) * nn::kHW + oi] = in.planes[p * nn::kHW + mi];
                out.planes[(7 + (3 - p)) * nn::kHW + oi] = in.planes[(7 + p) * nn::kHW + mi];
            }
            for (int c : {4, 5, 6, 11}) {
                out.planes[c * nn::kHW + oi] = in.planes[c * nn::kHW + mi];
            }
        }
    }
    if (!in.pol.empty()) {
        out.pol.assign(nn::kPolicySize, 0.0f);
        for (int s = 0; s < nn::kPolicySlots; ++s) {
            for (int y = 0; y < nn::kRowsN; ++y) {
                for (int x = 0; x < nn::kColsN; ++x) {
                    out.pol[s * nn::kHW + y * nn::kColsN + x] =
                        in.pol[s * nn::kHW + y * nn::kColsN + mx - x];
                }
            }
        }
    }
    return out;
}

bool parsePolicyTarget(const std::string& acts, float* out) {
    std::fill(out, out + nn::kPolicySize, 0.0f);
    if (acts.empty()) return false;
    std::vector<std::pair<int, long long>> items;
    long long total = 0;
    size_t pos = 0;
    while (pos < acts.size()) {
        const size_t semi = acts.find(';', pos);
        const std::string item = acts.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        if (item.empty()) break;
        const size_t at = item.find('@');
        const size_t colon = item.find(':');
        if (at == std::string::npos || colon == std::string::npos) return false;
        const int slot = std::atoi(item.substr(0, at).c_str());
        const std::string cell = item.substr(at + 1, colon - at - 1);
        const long long visits = std::atoll(item.substr(colon + 1).c_str());
        const size_t comma = cell.find(',');
        if (comma == std::string::npos) return false;
        Point p{std::atoi(cell.substr(0, comma).c_str()), std::atoi(cell.substr(comma + 1).c_str())};
        if (slot < 0 || slot >= nn::kPolicySlots || p.x < 0 || p.x >= nn::kColsN || p.y < 0 || p.y >= nn::kRowsN) {
            return false;
        }
        items.push_back({nn::policyIndex(slot, p), visits});
        total += visits;
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    if (total <= 0) return false;
    for (const auto& it : items) out[it.first] = static_cast<float>(it.second) / static_cast<float>(total);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* dataFile = "data.txt";
    const char* weightsFile = "weights.bin";
    bool dataFileSet = false;
    int epochs = 5;
    float lr = 0.01f;
    float lrDecay = 0.9f;
    int batchSize = 32;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--epochs" && i + 1 < argc) {
            epochs = std::atoi(argv[++i]);
        } else if (a == "--lr" && i + 1 < argc) {
            lr = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--lr-decay" && i + 1 < argc) {
            lrDecay = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--batch" && i + 1 < argc) {
            batchSize = std::atoi(argv[++i]);
        } else if (!a.empty() && a[0] != '-') {
            if (dataFileSet) {
                weightsFile = argv[i];
                dataFileSet = false;  // consumed
            } else {
                dataFile = argv[i];
                dataFileSet = true;
            }
        } else {
            std::fprintf(stderr,
                         "usage: train [data.txt] [weights.bin] [--epochs N] [--lr x] "
                         "[--lr-decay x] [--batch N]\n");
            return 1;
        }
    }

    std::vector<Rec> recs;
    {
        std::ifstream in(dataFile);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", dataFile);
            return 1;
        }
        std::string line;
        while (std::getline(in, line)) {
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            Rec r;
            r.winner = std::atoi(line.c_str() + t2 + 1);
            if (r.winner < 0 || r.winner >= kMaxPlayers) r.winner = -1;
            Game g(4);
            if (!g.loadPosition(line.substr(0, t1))) {
                std::fprintf(stderr, "skip unparseable position line\n");
                continue;
            }
            r.planes.assign(static_cast<size_t>(nn::kPlanes) * nn::kHW, 0.0f);
            nn::featurize(g, r.planes.data());
            r.pol.assign(nn::kPolicySize, 0.0f);
            if (!parsePolicyTarget(line.substr(t1 + 1, t2 - t1 - 1), r.pol.data())) {
                r.pol.clear();  // value-only sample
            }
            r.players = g.playerCount();
            if (r.players == 4) recs.push_back(mirror(r));
            recs.push_back(std::move(r));
        }
    }
    if (recs.empty()) {
        std::fprintf(stderr, "no usable samples in %s\n", dataFile);
        return 1;
    }
    std::printf("loaded %zu samples\n", recs.size());

    nn::NeuralNet net;
    net.initRandom(12345);
    net.setLearningRate(lr);
    std::mt19937 rng(777);
    std::vector<size_t> order(recs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;

    std::vector<const float*> planes(batchSize);
    std::vector<const float*> pols(batchSize);
    std::vector<int> wins(batchSize);
    for (int e = 0; e < epochs; ++e) {
        std::shuffle(order.begin(), order.end(), rng);
        float sumLoss = 0.0f;
        int nb = 0;
        for (size_t i = 0; i < order.size(); i += static_cast<size_t>(batchSize)) {
            const int n = static_cast<int>(std::min<size_t>(batchSize, order.size() - i));
            for (int b = 0; b < n; ++b) {
                const Rec& r = recs[order[i + b]];
                planes[b] = r.planes.data();
                pols[b] = r.pol.empty() ? nullptr : r.pol.data();
                wins[b] = r.winner;
            }
            sumLoss += net.trainBatch(planes.data(), pols.data(), wins.data(), n);
            ++nb;
        }
        std::printf("epoch %d loss %.4f\n", e + 1, sumLoss / nb);
        lr *= lrDecay;
        net.setLearningRate(lr);
    }

    if (!net.save(weightsFile)) {
        std::fprintf(stderr, "cannot write %s\n", weightsFile);
        return 1;
    }
    std::printf("saved %s\n", weightsFile);
    return 0;
}