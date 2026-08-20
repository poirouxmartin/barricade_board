#include "nn.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace barricade {
namespace nn {

namespace {
// Flat parameter layout offsets (see kParams in the header).
constexpr size_t kW1 = 0;                                    // [kPlanes][kCh][3][3]
constexpr size_t kB1 = kW1 + kPlanes * kCh * 9;              // [kCh]
constexpr size_t kW2 = kB1 + kCh;                            // [kCh][kCh][3][3]
constexpr size_t kB2 = kW2 + kCh * kCh * 9;                  // [kCh]
constexpr size_t kW3 = kB2 + kCh;                            // [kCh][kCh][3][3]
constexpr size_t kB3 = kW3 + kCh * kCh * 9;                  // [kCh]
constexpr size_t kWP = kB3 + kCh;                            // [kCh][kPolicySlots]
constexpr size_t kBP = kWP + kCh * kPolicySlots;             // [kPolicySlots]
constexpr size_t kWV1 = kBP + kPolicySlots;                  // [kCh][kValHid]
constexpr size_t kBV1 = kWV1 + kCh * kValHid;                // [kValHid]
constexpr size_t kWV2 = kBV1 + kValHid;                      // [kValHid][kMaxPlayers]
constexpr size_t kBV2 = kWV2 + kValHid * kMaxPlayers;        // [kMaxPlayers]
constexpr size_t kEnd = kBV2 + kMaxPlayers;

inline void reluInPlace(float* a, size_t n) {
    for (size_t i = 0; i < n; ++i) a[i] = a[i] > 0.f ? a[i] : 0.f;
}

inline void reluMask(const float* a, float* g, size_t n) {
    for (size_t i = 0; i < n; ++i) g[i] = a[i] > 0.f ? g[i] : 0.f;
}

// 3x3 convolution with zero padding, stride 1. `in` has `cin` planes of kHW,
// `out` has kCh planes. W layout is [cout][cin][3][3].
// The border cells are computed with bounds checks; the interior (all 9 taps
// in-bounds) runs as contiguous per-tap FMA loops that MSVC vectorizes, which
// is the hot path of the MCTS leaf evaluations.
void conv3(const float* in, int cin, const float* W, const float* B, float* out) {
    for (int c = 0; c < kCh; ++c) {
        const float* wc = W + c * (cin * 9);
        const float bias = B[c];
        float* oc = out + c * kHW;
        for (int y = 0; y < kRowsN; ++y) {
            for (int x = 0; x < kColsN; ++x) {
                if (y != 0 && y != kRowsN - 1 && x != 0 && x != kColsN - 1) continue;
                float acc = bias;
                for (int ci = 0; ci < cin; ++ci) {
                    const float* ip = in + ci * kHW;
                    const float* wp = wc + ci * 9;
                    for (int dy = 0; dy < 3; ++dy) {
                        const int yy = y + dy - 1;
                        if (yy < 0 || yy >= kRowsN) continue;
                        for (int dx = 0; dx < 3; ++dx) {
                            const int xx = x + dx - 1;
                            if (xx < 0 || xx >= kColsN) continue;
                            acc += ip[yy * kColsN + xx] * wp[dy * 3 + dx];
                        }
                    }
                }
                oc[y * kColsN + x] = acc;
            }
        }
        for (int y = 1; y < kRowsN - 1; ++y) {
            float* orow = oc + y * kColsN;
            for (int x = 1; x < kColsN - 1; ++x) orow[x] = bias;
        }
        for (int ci = 0; ci < cin; ++ci) {
            const float* ip = in + ci * kHW;
            const float* wp = wc + ci * 9;
            for (int dy = 0; dy < 3; ++dy) {
                const float* irow = ip + (dy - 1) * kColsN;
                for (int dx = 0; dx < 3; ++dx) {
                    const float wv = wp[dy * 3 + dx];
                    if (wv == 0.0f) continue;
                    const float* s = irow + (dx - 1);
                    for (int y = 1; y < kRowsN - 1; ++y) {
                        float* o = oc + y * kColsN;
                        const float* row = s + y * kColsN;
                        for (int x = 1; x < kColsN - 1; ++x) o[x] += row[x] * wv;
                    }
                }
            }
        }
    }
}

// Backward through conv3. dOut is [kCh][kHW]. Gradients accumulate into dW
// ([cout][cin][3][3]) and dB; the input gradient is added into dIn (may be
// nullptr). Caller zeroes dW/dB/dIn before use.
// For a given kernel tap (dy,dx) the valid output cells form a rectangle (the
// full grid minus the edge that falls out of bounds), so both the dW and dIn
// passes run as contiguous vectorized loops.
void conv3Backward(const float* in, int cin, const float* W, const float* dOut, float* dW,
                   float* dB, float* dIn) {
    for (int c = 0; c < kCh; ++c) {
        const float* wc = W + c * (cin * 9);
        float* dwc = dW + c * (cin * 9);
        float db = 0.0f;
        for (int idx = 0; idx < kHW; ++idx) db += dOut[c * kHW + idx];
        dB[c] += db;
        for (int ci = 0; ci < cin; ++ci) {
            const float* ip = in + ci * kHW;
            const float* wp = wc + ci * 9;
            float* dwp = dwc + ci * 9;
            for (int dy = 0; dy < 3; ++dy) {
                const int y0 = dy == 0 ? 1 : 0;
                const int y1 = dy == 2 ? kRowsN - 2 : kRowsN - 1;
                const int dyOff = dy - 1;
                for (int dx = 0; dx < 3; ++dx) {
                    const int x0 = dx == 0 ? 1 : 0;
                    const int x1 = dx == 2 ? kColsN - 2 : kColsN - 1;
                    float acc = 0.0f;
                    for (int y = y0; y <= y1; ++y) {
                        const float* o = dOut + c * kHW + y * kColsN;
                        const float* s = ip + (y + dyOff) * kColsN + (dx - 1);
                        for (int x = x0; x <= x1; ++x) acc += o[x] * s[x];
                    }
                    dwp[dy * 3 + dx] += acc;
                }
            }
            if (!dIn) continue;
            for (int dy = 0; dy < 3; ++dy) {
                const int y0 = dy == 0 ? 1 : 0;
                const int y1 = dy == 2 ? kRowsN - 2 : kRowsN - 1;
                const int dyOff = dy - 1;
                for (int dx = 0; dx < 3; ++dx) {
                    const int x0 = dx == 0 ? 1 : 0;
                    const int x1 = dx == 2 ? kColsN - 2 : kColsN - 1;
                    const float wv = wp[dy * 3 + dx];
                    if (wv == 0.0f) continue;
                    for (int y = y0; y <= y1; ++y) {
                        float* di = dIn + ci * kHW + (y + dyOff) * kColsN + (dx - 1);
                        const float* o = dOut + c * kHW + y * kColsN;
                        for (int x = x0; x <= x1; ++x) di[x] += o[x] * wv;
                    }
                }
            }
        }
    }
}

// Per-thread scratch for the forward pass and featurization: the conv stages
// are small, so avoiding a heap allocation per call dominates speed in the
// MCTS hot path (thread_local keeps the shared net thread-safe).
struct ForwardScratch {
    std::vector<float> a1, a2, a3;
    std::vector<float> planes;
};

ForwardScratch& fwdScratch() {
    static thread_local ForwardScratch s;
    return s;
}

}  // namespace

void featurize(const Game& g, float* out) {
    std::fill(out, out + kPlanes * kHW, 0.0f);
    const int n = g.playerCount();
    for (int p = 0; p < n; ++p) {
        float* plane = out + p * kHW;
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            const Point pos = g.pawnPos(p, m);
            plane[pos.y * kColsN + pos.x] = 1.0f;
        }
    }
    float* bar = out + 4 * kHW;
    for (const Point& b : g.barricades()) {
        if (b.x < 0) continue;
        bar[b.y * kColsN + b.x] = 1.0f;
    }
    float* goal = out + 5 * kHW;
    goal[0 * kColsN + 8] = 1.0f;
    float* base = out + 6 * kHW;
    for (int p = 0; p < kMaxPlayers; ++p) {
        for (int m = 0; m < kPawnsPerPlayer; ++m) {
            const Point c = baseCell(p, m);
            base[c.y * kColsN + c.x] = 1.0f;
        }
    }
    float* turn = out + 7 * kHW;
    std::fill(turn, turn + 4 * kHW, 0.0f);
    std::fill(turn + g.currentPlayer() * kHW, turn + (g.currentPlayer() + 1) * kHW, 1.0f);
    float* dice = out + 11 * kHW;
    std::fill(dice, dice + kHW, g.dice() / 6.0f);
}

NetOut NeuralNet::forward(const float* x) const {
    const float* w = w_.data();
    ForwardScratch& scr = fwdScratch();
    scr.a1.resize(kCh * kHW);
    scr.a2.resize(kCh * kHW);
    scr.a3.resize(kCh * kHW);
    conv3(x, kPlanes, w + kW1, w + kB1, scr.a1.data());
    reluInPlace(scr.a1.data(), scr.a1.size());
    conv3(scr.a1.data(), kCh, w + kW2, w + kB2, scr.a2.data());
    reluInPlace(scr.a2.data(), scr.a2.size());
    conv3(scr.a2.data(), kCh, w + kW3, w + kB3, scr.a3.data());
    reluInPlace(scr.a3.data(), scr.a3.size());

    NetOut out;
    for (int s = 0; s < kPolicySlots; ++s) {
        float* po = out.policy + s * kHW;
        for (int idx = 0; idx < kHW; ++idx) po[idx] = w[kBP + s];
        for (int ci = 0; ci < kCh; ++ci) {
            const float wv = w[kWP + ci * kPolicySlots + s];
            if (wv == 0.0f) continue;
            const float* a = scr.a3.data() + ci * kHW;
            for (int idx = 0; idx < kHW; ++idx) po[idx] += a[idx] * wv;
        }
    }

    float v16[kCh];
    for (int ci = 0; ci < kCh; ++ci) {
        float ssum = 0.0f;
        for (int idx = 0; idx < kHW; ++idx) ssum += scr.a3[ci * kHW + idx];
        v16[ci] = ssum / static_cast<float>(kHW);
    }
    float raw[kMaxPlayers];
    float maxRaw = -1e30f;
    // dense layers with ReLU on the hidden layer
    float h[kValHid];
    for (int j = 0; j < kValHid; ++j) {
        float acc = w[kBV1 + j];
        for (int ci = 0; ci < kCh; ++ci) acc += v16[ci] * w[kWV1 + ci * kValHid + j];
        h[j] = acc > 0.0f ? acc : 0.0f;
    }
    for (int k = 0; k < kMaxPlayers; ++k) {
        float acc = w[kBV2 + k];
        for (int j = 0; j < kValHid; ++j) acc += h[j] * w[kWV2 + j * kMaxPlayers + k];
        raw[k] = acc;
        if (acc > maxRaw) maxRaw = acc;
    }
    float sum = 0.0f;
    for (int k = 0; k < kMaxPlayers; ++k) {
        out.value[k] = std::exp(raw[k] - maxRaw);
        sum += out.value[k];
    }
    for (int k = 0; k < kMaxPlayers; ++k) out.value[k] /= sum;
    return out;
}

NetOut NeuralNet::evaluate(const Game& g) const {
    ForwardScratch& scr = fwdScratch();
    scr.planes.resize(static_cast<size_t>(kPlanes) * kHW);
    featurize(g, scr.planes.data());
    return forward(scr.planes.data());
}

float NeuralNet::trainBatch(const float* const* planes, const float* const* polTarget,
                            const int* winners, int batch) {
    if (m_.size() != kParams) {
        m_.assign(kParams, 0.0f);
        v_.assign(kParams, 0.0f);
        t_ = 0;
    }
    const float* w = w_.data();
    std::vector<float> grad(kParams, 0.0f);
    std::vector<float> a1(kCh * kHW), a2(kCh * kHW), a3(kCh * kHW), pol(kPolicySize);
    std::vector<float> dA1(kCh * kHW), dA2(kCh * kHW), dA3(kCh * kHW), dPol(kPolicySize);
    std::vector<float> dX(static_cast<size_t>(kPlanes) * kHW);
    std::vector<int> rec;
    rec.reserve(64);

    float totalLoss = 0.0f;
    for (int b = 0; b < batch; ++b) {
        const float* x = planes[b];
        conv3(x, kPlanes, w + kW1, w + kB1, a1.data());
        reluInPlace(a1.data(), a1.size());
        conv3(a1.data(), kCh, w + kW2, w + kB2, a2.data());
        reluInPlace(a2.data(), a2.size());
        conv3(a2.data(), kCh, w + kW3, w + kB3, a3.data());
        reluInPlace(a3.data(), a3.size());

        for (int s = 0; s < kPolicySlots; ++s) {
            float* ps = pol.data() + s * kHW;
            for (int idx = 0; idx < kHW; ++idx) ps[idx] = w[kBP + s];
            for (int ci = 0; ci < kCh; ++ci) {
                const float wv = w[kWP + ci * kPolicySlots + s];
                if (wv == 0.0f) continue;
                const float* a = a3.data() + ci * kHW;
                for (int idx = 0; idx < kHW; ++idx) ps[idx] += a[idx] * wv;
            }
        }
        float v16[kCh];
        for (int ci = 0; ci < kCh; ++ci) {
            float s = 0.0f;
            for (int idx = 0; idx < kHW; ++idx) s += a3[ci * kHW + idx];
            v16[ci] = s / static_cast<float>(kHW);
        }
        float h[kValHid];
        for (int j = 0; j < kValHid; ++j) {
            float acc = w[kBV1 + j];
            for (int ci = 0; ci < kCh; ++ci) acc += v16[ci] * w[kWV1 + ci * kValHid + j];
            h[j] = acc > 0.0f ? acc : 0.0f;
        }
        float raw[kMaxPlayers];
        float maxRaw = -1e30f;
        for (int k = 0; k < kMaxPlayers; ++k) {
            float acc = w[kBV2 + k];
            for (int j = 0; j < kValHid; ++j) acc += h[j] * w[kWV2 + j * kMaxPlayers + k];
            raw[k] = acc;
            if (acc > maxRaw) maxRaw = acc;
        }
        float pv[kMaxPlayers];
        float vsum = 0.0f;
        for (int k = 0; k < kMaxPlayers; ++k) {
            pv[k] = std::exp(raw[k] - maxRaw);
            vsum += pv[k];
        }
        for (int k = 0; k < kMaxPlayers; ++k) pv[k] /= vsum;

        std::fill(dA3.begin(), dA3.end(), 0.0f);
        std::fill(dPol.begin(), dPol.end(), 0.0f);
        float lossB = 0.0f;

        if (winners[b] >= 0) {
            lossB += -std::log(pv[winners[b]] + 1e-9f);
            for (int k = 0; k < kMaxPlayers; ++k) {
                float dRaw = pv[k] - (k == winners[b] ? 1.0f : 0.0f);
                for (int j = 0; j < kValHid; ++j) grad[kWV2 + j * kMaxPlayers + k] += dRaw * h[j];
                grad[kBV2 + k] += dRaw;
            }
            float dH[kValHid];
            for (int j = 0; j < kValHid; ++j) {
                float acc = 0.0f;
                for (int k = 0; k < kMaxPlayers; ++k) {
                    acc += (pv[k] - (k == winners[b] ? 1.0f : 0.0f)) *
                           w[kWV2 + j * kMaxPlayers + k];
                }
                dH[j] = h[j] > 0.0f ? acc : 0.0f;
                grad[kBV1 + j] += dH[j];
                for (int ci = 0; ci < kCh; ++ci) {
                    grad[kWV1 + ci * kValHid + j] += dH[j] * v16[ci];
                }
            }
            float dV16[kCh];
            for (int ci = 0; ci < kCh; ++ci) {
                float acc = 0.0f;
                for (int j = 0; j < kValHid; ++j) {
                    acc += dH[j] * w[kWV1 + ci * kValHid + j];
                }
                dV16[ci] = acc;
            }
            for (int ci = 0; ci < kCh; ++ci) {
                const float g = dV16[ci] / static_cast<float>(kHW);
                for (int idx = 0; idx < kHW; ++idx) dA3[ci * kHW + idx] += g;
            }
        }
        if (polTarget[b]) {
            rec.clear();
            const float* tgt = polTarget[b];
            for (int i = 0; i < kPolicySize; ++i) {
                if (tgt[i] != 0.0f) rec.push_back(i);
            }
            if (!rec.empty()) {
                float maxLg = -1e30f;
                for (int i : rec) maxLg = std::max(maxLg, pol[i]);
                float zsum = 0.0f;
                for (int i : rec) zsum += std::exp(pol[i] - maxLg);
                for (int i : rec) {
                    const float p = std::exp(pol[i] - maxLg) / zsum;
                    lossB += -tgt[i] * std::log(p + 1e-9f);
                    dPol[i] = p - tgt[i];
                }
                for (int i : rec) {
                    const int s = i / kHW;
                    const int idx = i % kHW;
                    grad[kBP + s] += dPol[i];
                    for (int ci = 0; ci < kCh; ++ci) {
                        grad[kWP + ci * kPolicySlots + s] += dPol[i] * a3[ci * kHW + idx];
                        dA3[ci * kHW + idx] += dPol[i] * w[kWP + ci * kPolicySlots + s];
                    }
                }
            }
        }
        totalLoss += lossB;

        reluMask(a3.data(), dA3.data(), dA3.size());
        std::fill(dA2.begin(), dA2.end(), 0.0f);
        conv3Backward(a2.data(), kCh, w + kW3, dA3.data(), grad.data() + kW3,
                      grad.data() + kB3, dA2.data());
        reluMask(a2.data(), dA2.data(), dA2.size());
        std::fill(dA1.begin(), dA1.end(), 0.0f);
        conv3Backward(a1.data(), kCh, w + kW2, dA2.data(), grad.data() + kW2,
                      grad.data() + kB2, dA1.data());
        reluMask(a1.data(), dA1.data(), dA1.size());
        std::fill(dX.begin(), dX.end(), 0.0f);
        conv3Backward(x, kPlanes, w + kW1, dA1.data(), grad.data() + kW1,
                      grad.data() + kB1, dX.data());
    }

    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    ++t_;
    const float b1t = 1.0f - std::pow(b1, static_cast<float>(t_));
    const float b2t = 1.0f - std::pow(b2, static_cast<float>(t_));
    for (size_t i = 0; i < kParams; ++i) {
        const float g = grad[i];
        m_[i] = b1 * m_[i] + (1.0f - b1) * g;
        v_[i] = b2 * v_[i] + (1.0f - b2) * g * g;
        const float mhat = m_[i] / b1t;
        const float vhat = v_[i] / b2t;
        w_[i] -= lr_ * mhat / (std::sqrt(vhat) + eps);
    }
    return totalLoss / static_cast<float>(batch);
}

void NeuralNet::initRandom(uint64_t seed) {
    std::mt19937 rng(static_cast<uint32_t>(seed));
    std::normal_distribution<float> dist(0.0f, 1.0f);
    auto sc = [&](size_t fanin) { return dist(rng) * std::sqrt(2.0f / static_cast<float>(fanin)); };
    float* w = w_.data();
    for (int ci = 0; ci < kPlanes * kCh; ++ci)
        for (int k = 0; k < 9; ++k) w[kW1 + ci * 9 + k] = sc(static_cast<size_t>(kPlanes) * 9);
    std::fill(w + kB1, w + kB1 + kCh, 0.0f);
    for (int ci = 0; ci < kCh * kCh; ++ci)
        for (int k = 0; k < 9; ++k) w[kW2 + ci * 9 + k] = sc(static_cast<size_t>(kCh) * 9);
    std::fill(w + kB2, w + kB2 + kCh, 0.0f);
    for (int ci = 0; ci < kCh * kCh; ++ci)
        for (int k = 0; k < 9; ++k) w[kW3 + ci * 9 + k] = sc(static_cast<size_t>(kCh) * 9);
    std::fill(w + kB3, w + kB3 + kCh, 0.0f);
    for (int ci = 0; ci < kCh * kPolicySlots; ++ci) w[kWP + ci] = sc(static_cast<size_t>(kCh) * 9);
    std::fill(w + kBP, w + kBP + kPolicySlots, 0.0f);
    for (int ci = 0; ci < kCh * kValHid; ++ci) w[kWV1 + ci] = sc(static_cast<size_t>(kCh));
    std::fill(w + kBV1, w + kBV1 + kValHid, 0.0f);
    for (int ci = 0; ci < kValHid * kMaxPlayers; ++ci) w[kWV2 + ci] = sc(static_cast<size_t>(kValHid));
    std::fill(w + kBV2, w + kBV2 + kMaxPlayers, 0.0f);
    loaded_ = false;
}

bool NeuralNet::load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t magic = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || magic != 0x4e4e4231u) {
        std::fclose(f);
        return false;
    }
    size_t n = 0;
    if (std::fread(&n, sizeof(n), 1, f) != 1 || n != kParams) {
        std::fclose(f);
        return false;
    }
    if (std::fread(w_.data(), sizeof(float), kParams, f) != kParams) {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    m_.clear();
    v_.clear();
    t_ = 0;
    loaded_ = true;
    return true;
}

bool NeuralNet::save(const std::string& path) const {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t magic = 0x4e4e4231u;
    const size_t n = kParams;
    const bool ok = std::fwrite(&magic, 4, 1, f) == 1 && std::fwrite(&n, sizeof(n), 1, f) == 1 &&
                    std::fwrite(w_.data(), sizeof(float), kParams, f) == kParams;
    std::fclose(f);
    return ok;
}

}  // namespace nn
}  // namespace barricade