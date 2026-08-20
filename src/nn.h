#pragma once

#include "board.h"
#include "game.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace barricade {
namespace nn {

constexpr int kRowsN = kRows;             // 19
constexpr int kColsN = kCols;             // 17
constexpr int kHW = kRowsN * kColsN;      // 323
constexpr int kPlanes = 12;               // 4 pawns + barricades + goal + bases + 4 turn + dice
constexpr int kCh = 16;                   // conv channels
constexpr int kValHid = 16;               // value hidden units
constexpr int kPolicySlots = 6;           // pawn slots 0..4, placement slot 5
constexpr int kPolicySize = kPolicySlots * kHW;  // 1938

inline int policyIndex(int slot, Point p) {
    return slot * kHW + p.y * kColsN + p.x;
}

// Featurizes a position into kPlanes*kHW floats: per-player pawn occupancy (4
// planes), barricades (1), goal cell (1), base cells (1), current-player one-hot
// (4), and the dice value scaled to [0,1] (1). Pawns in base sit on their base
// cell. A cell outside the board is left 0 (the conv layers pad with zeros).
void featurize(const Game& g, float* out);

struct NetOut {
    float policy[kPolicySize];  // raw logits (softmaxed over the legal subset by the caller)
    float value[kMaxPlayers];   // win distribution over the players (softmax)
};

// Small convolutional net (3 conv layers + 1x1 policy head + pooled value head).
// Weights live in one flat array; save/load round-trips them so training
// results persist. All methods are single-threaded.
class NeuralNet {
public:
    NeuralNet() { w_.assign(paramCount(), 0.0f); }

    bool load(const std::string& path);
    bool save(const std::string& path) const;
    bool loaded() const { return loaded_; }

    NetOut forward(const float* planes) const;
    NetOut evaluate(const Game& g) const;

    // Mini-batch training step. `planes[b]`, `polTarget[b]` (kPolicySize
    // floats, visit distribution sparse over recorded actions; may be nullptr
    // to skip the policy loss), `winners[b]` (0..kMaxPlayers-1, or -1 to skip
    // the value loss). Applies one Adam update over the batch.
    float trainBatch(const float* const* planes, const float* const* polTarget,
                     const int* winners, int batch);

    float* params() { return w_.data(); }
    const float* params() const { return w_.data(); }
    size_t paramCount() const { return kParams; }

    void initRandom(uint64_t seed);  // small Kaiming-like init for the trainer
    void setLearningRate(float v) { lr_ = v; }
    float learningRate() const { return lr_; }

    static constexpr size_t kParams = 6826;

private:
    std::vector<float> w_;    // full parameter blob
    std::vector<float> m_;    // Adam first moment (lazy)
    std::vector<float> v_;    // Adam second moment (lazy)
    uint64_t t_ = 0;
    float lr_ = 0.01f;
    bool loaded_ = false;
};

}  // namespace nn
}  // namespace barricade