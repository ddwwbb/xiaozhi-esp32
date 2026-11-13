#include "fluid_orb.h"

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr const char* TAG = "FluidOrb";
constexpr float kTwoPi = 6.28318530718f;

void* AllocPsramFirst(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

// Keep hot lookup and intermediate buffers (~34KB total) in internal SRAM.
void* AllocInternalFirst(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p != nullptr ? p : heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

inline uint16_t Pack565(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline int Mix8(int a, int b, int t /*0..256*/) { return a + (((b - a) * t) >> 8); }

inline void Fill16(uint16_t* dst, int count, uint16_t v) {
    for (int i = 0; i < count; ++i) {
        dst[i] = v;
    }
}

}  // namespace

FluidOrbRenderer::FluidOrbRenderer(int size)
    : size_(size & ~3), half_((size & ~3) / 2), quarter_((size & ~3) / 4) {
    const size_t n = static_cast<size_t>(size_) * size_;
    const size_t q = static_cast<size_t>(half_) * half_;
    const size_t qq = static_cast<size_t>(quarter_ + 1) * (quarter_ + 1);

    nominal_r_ = (size_ * 0.5f - kGlowPx) * 0.80f;
    const float R = nominal_r_;
    const float hr = 0.48f * R;
    spec_side_ = static_cast<int>(hr * 2.0f) + 2;

    pixels_ = static_cast<uint16_t*>(AllocPsramFirst(n * sizeof(uint16_t)));
    qdist4_ = static_cast<uint16_t*>(AllocInternalFirst(q * sizeof(uint16_t)));
    qangle_ = static_cast<uint8_t*>(AllocInternalFirst(q));
    spec_box_ = static_cast<uint8_t*>(AllocInternalFirst(spec_side_ * spec_side_));
    row_shade_ = static_cast<int16_t*>(AllocInternalFirst(size_ * sizeof(int16_t)));
    span_x0_ = static_cast<int16_t*>(AllocInternalFirst(size_ * sizeof(int16_t)));
    span_x1_ = static_cast<int16_t*>(AllocInternalFirst(size_ * sizeof(int16_t)));
    field_ = static_cast<uint8_t*>(AllocInternalFirst(qq * 3));
    shaded_ = static_cast<uint16_t*>(AllocInternalFirst(q * sizeof(uint16_t)));

    if (!pixels_ || !qdist4_ || !qangle_ || !spec_box_ || !row_shade_ || !span_x0_ ||
        !span_x1_ || !field_ || !shaded_) {
        ESP_LOGE(TAG, "Allocation failed for %dx%d orb", size_, size_);
        heap_caps_free(pixels_);
        pixels_ = nullptr;
        return;
    }

    std::memset(field_, 0, qq * 3);
    std::memset(shaded_, 0, q * sizeof(uint16_t));
    for (int i = 0; i < kSinSize; ++i) {
        sin_lut_[i] = std::sin(kTwoPi * i / kSinSize);
    }

    // First-quadrant tables; pixel centers are at +0.5 from the canvas center.
    for (int y = 0; y < half_; ++y) {
        for (int x = 0; x < half_; ++x) {
            const float dx = x + 0.5f;
            const float dy = y + 0.5f;
            const float d = std::sqrt(dx * dx + dy * dy);
            qdist4_[y * half_ + x] = static_cast<uint16_t>(std::min(65535.0f, d * 4.0f + 0.5f));
            const float a = std::atan2(dy, dx);  // 0..pi/2
            qangle_[y * half_ + x] = static_cast<uint8_t>(a / kTwoPi * kAngles + 0.5f);
        }
    }

    // Vertical shading: lower hemisphere slightly darker (light from top-left).
    const float c = (size_ - 1) * 0.5f;
    for (int y = 0; y < size_; ++y) {
        const float ny = std::max(0.0f, (y - c) / R);
        row_shade_[y] = static_cast<int16_t>(std::min(1.2f, ny) * 34.0f);
    }

    // Specular highlight patch at upper-left of the sphere.
    const float hx = c - 0.30f * R;
    const float hy = c - 0.36f * R;
    spec_x0_ = static_cast<int>(hx - spec_side_ * 0.5f);
    spec_y0_ = static_cast<int>(hy - spec_side_ * 0.5f);
    for (int y = 0; y < spec_side_; ++y) {
        for (int x = 0; x < spec_side_; ++x) {
            const float sx = (spec_x0_ + x) - hx;
            const float sy = (spec_y0_ + y) - hy;
            const float sd = std::sqrt(sx * sx + sy * sy) / hr;
            const float s = std::max(0.0f, 1.0f - sd);
            spec_box_[y * spec_side_ + x] =
                static_cast<uint8_t>(std::min(255.0f, s * s * 135.0f));
        }
    }

    for (int y = 0; y < size_; ++y) {
        span_x0_[y] = 0;
        span_x1_[y] = static_cast<int16_t>(size_ - 1);
    }

    ESP_LOGI(TAG, "Fluid orb renderer ready: %dx%d, nominal radius %.1f, LUT internal=%d",
             size_, size_, nominal_r_,
             (esp_ptr_internal(qdist4_) && esp_ptr_internal(shaded_)) ? 1 : 0);
}

FluidOrbRenderer::~FluidOrbRenderer() {
    heap_caps_free(pixels_);
    heap_caps_free(qdist4_);
    heap_caps_free(qangle_);
    heap_caps_free(spec_box_);
    heap_caps_free(row_shade_);
    heap_caps_free(span_x0_);
    heap_caps_free(span_x1_);
    heap_caps_free(field_);
    heap_caps_free(shaded_);
}

float FluidOrbRenderer::FastSin(float x) const {
    int idx = static_cast<int>(x * (kSinSize / kTwoPi));
    return sin_lut_[idx & (kSinSize - 1)];
}

inline int FluidOrbRenderer::AngleIndex(int x, int y, int a0) const {
    const bool lower = y >= half_;
    const bool right = x >= half_;
    if (lower) {
        return (right ? a0 : (kAngles / 2 - a0)) & (kAngles - 1);
    }
    return (right ? (kAngles - a0) : (kAngles / 2 + a0)) & (kAngles - 1);
}

void FluidOrbRenderer::Clear(const uint8_t bg[3]) {
    if (!ok()) {
        return;
    }
    Fill16(pixels_, size_ * size_, Pack565(bg[0], bg[1], bg[2]));
    for (int y = 0; y < size_; ++y) {
        span_x0_[y] = static_cast<int16_t>(size_);
        span_x1_[y] = -1;
    }
    std::memcpy(last_bg_, bg, 3);
    bg_valid_ = true;
}

void FluidOrbRenderer::RenderField(const FluidOrbFrame& f, float body_r) {
    const int q = quarter_ + 1;
    const float cq = (quarter_ - 1) * 0.5f;
    const float rb = body_r * 0.25f;
    const float t = f.time;

    struct Blob {
        float x, y, inv_rr, rr, strength;
        float r, g, b;
    } blobs[kMaxBlobs];

    const float orbit[kMaxBlobs] = {0.48f, 0.54f, 0.34f, 0.58f};
    const float speed[kMaxBlobs] = {0.62f, -0.81f, 1.15f, -0.49f};
    const float phase[kMaxBlobs] = {0.0f, 1.9f, 3.8f, 0.8f};
    const float size_k[kMaxBlobs] = {1.05f + 0.45f * f.bands[0], 0.98f + 0.42f * f.bands[1],
                                     0.72f + 0.48f * f.bands[2], 0.80f + 0.40f * f.bands[3]};
    const float strength[kMaxBlobs] = {0.96f, 0.92f, 0.82f, 0.75f};
    const uint8_t* cols[kMaxBlobs] = {f.c1, f.c2, f.c3, f.c1};

    for (int b = 0; b < kMaxBlobs; ++b) {
        const float orb = rb * orbit[b] * (1.0f + 0.28f * f.level);
        const float a = t * speed[b] + phase[b];
        blobs[b].x = cq + orb * FastSin(a + kTwoPi * 0.25f);
        blobs[b].y = cq + orb * 0.86f * FastSin(a * 1.27f + phase[b] * 0.7f);
        const float r = rb * size_k[b];
        blobs[b].rr = r * r;
        blobs[b].inv_rr = 1.0f / blobs[b].rr;
        blobs[b].strength = strength[b];
        blobs[b].r = cols[b][0];
        blobs[b].g = cols[b][1];
        blobs[b].b = cols[b][2];
    }

    // Separable domain warp + silky wave interference at 1/4 resolution.
    float warp_x[48];
    float warp_y[48];
    float wave_u[48];
    float wave_v[48];
    const float w_amp = (1.1f + 2.4f * f.level) * f.warp;
    const int qq = std::min(q, 48);
    for (int i = 0; i < qq; ++i) {
        warp_x[i] = w_amp * FastSin(i * 0.36f + t * 1.7f) +
                    0.6f * w_amp * FastSin(i * 0.78f - t * 1.15f);
        warp_y[i] = w_amp * FastSin(i * 0.42f - t * 1.35f) +
                    0.6f * w_amp * FastSin(i * 0.71f + t * 0.95f);
        wave_u[i] = FastSin(i * 0.34f + t * 1.25f);
        wave_v[i] = FastSin(i * 0.39f - t * 1.05f);
    }

    const float br = f.base[0], bg = f.base[1], bb = f.base[2];
    const float c2r = f.c2[0], c2g = f.c2[1], c2b = f.c2[2];
    const float c3r = f.c3[0], c3g = f.c3[1], c3b = f.c3[2];
    const float ribbon_gain = 0.22f + 0.25f * f.level;

    for (int y = 0; y < qq; ++y) {
        uint8_t* row = field_ + (y * q) * 3;
        const float wy = warp_x[y];
        const float wv = wave_v[y];
        for (int x = 0; x < qq; ++x) {
            const float px = x + wy;
            const float py = y + warp_y[x];
            float r = br, g = bg, b = bb;
            for (int k = 0; k < kMaxBlobs; ++k) {
                const Blob& bl = blobs[k];
                const float dx = px - bl.x;
                const float dy = py - bl.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < bl.rr) {
                    const float u = 1.0f - d2 * bl.inv_rr;
                    const float w = u * u * bl.strength;
                    r += (bl.r - r) * w;
                    g += (bl.g - g) * w;
                    b += (bl.b - b) * w;
                }
            }
            // Flowing aurora/cloud ribbon interference (ChatGPT/Gemini signature swirl)
            const float rib = (wave_u[x] + wv) * 0.5f;
            if (rib > 0.0f) {
                const float rw = rib * rib * ribbon_gain;
                r += (c3r - r) * rw;
                g += (c3g - g) * rw;
                b += (c3b - b) * rw;
            } else {
                const float rw = rib * rib * (ribbon_gain * 0.7f);
                r += (c2r - r) * rw;
                g += (c2g - g) * rw;
                b += (c2b - b) * rw;
            }
            uint8_t* p = row + x * 3;
            p[0] = static_cast<uint8_t>(r);
            p[1] = static_cast<uint8_t>(g);
            p[2] = static_cast<uint8_t>(b);
        }
    }
}

void FluidOrbRenderer::RenderShaded(const FluidOrbFrame& f, const int* rad4, int max_r4,
                                    float body_r) {
    const int qstride = (quarter_ + 1) * 3;
    const int cen_k = static_cast<int>(256.0f * 255.0f / std::max(1.0f, body_r * 4.0f));
    const int core_amt = static_cast<int>(std::clamp(f.level, 0.0f, 1.0f) * 145.0f);
    const int shade_k = static_cast<int>(64.0f * 65536.0f / (16.0f * nominal_r_ * nominal_r_));
    const int shade_cap4 = static_cast<int>(nominal_r_ * 4.0f * 1.25f);
    constexpr int kRim4 = 22;
    const int c3r = f.c3[0], c3g = f.c3[1], c3b = f.c3[2];

    const int reach_half = static_cast<int>((max_r4 + 8) * 0.125f) + 2;
    const int ch = half_ / 2;
    const int hy0 = std::max(0, ch - reach_half);
    const int hy1 = std::min(half_ - 1, ch + reach_half);
    const int hx0 = std::max(0, ch - reach_half);
    const int hx1 = std::min(half_ - 1, ch + reach_half);

    for (int hy = hy0; hy <= hy1; ++hy) {
        const int y = (hy << 1);
        const bool lower = y >= half_;
        const int qy = lower ? (y - half_) : (half_ - 1 - y);
        const uint16_t* qd_row = qdist4_ + qy * half_;
        const uint8_t* qa_row = qangle_ + qy * half_;
        const int row_dark = row_shade_[y];
        const int sy = y - spec_y0_;
        const uint8_t* spec_row =
            (sy >= 0 && sy < spec_side_) ? spec_box_ + sy * spec_side_ : nullptr;

        const int fy = hy >> 1;
        const bool odd_y = (hy & 1) != 0;
        const uint8_t* frow0 = field_ + fy * qstride;
        const uint8_t* frow1 = frow0 + (odd_y ? qstride : 0);
        uint16_t* srow = shaded_ + hy * half_;

        for (int hx = hx0; hx <= hx1; ++hx) {
            const int x = (hx << 1);
            const bool right = x >= half_;
            const int qx = right ? (x - half_) : (half_ - 1 - x);
            const int d4 = qd_row[qx];
            if (d4 > max_r4 + 8) {
                continue;
            }
            const int a = AngleIndex(x, y, qa_row[qx]);
            const int diff = rad4[a] - d4;
            if (diff <= -8) {
                continue;
            }

            // Bilinear interpolation from 1/4-res field_ to 1/2-res shaded_
            const int fx = hx >> 1;
            const uint8_t* p00 = frow0 + fx * 3;
            const uint8_t* p10 = frow1 + fx * 3;
            int r, g, b;
            if ((hx & 1) == 0) {
                r = (p00[0] + p10[0]) >> 1;
                g = (p00[1] + p10[1]) >> 1;
                b = (p00[2] + p10[2]) >> 1;
            } else {
                r = (p00[0] + p00[3] + p10[0] + p10[3]) >> 2;
                g = (p00[1] + p00[4] + p10[1] + p10[4]) >> 2;
                b = (p00[2] + p00[5] + p10[2] + p10[5]) >> 2;
            }

            if (core_amt > 0 && diff > 0) {
                const int cen = std::min(255, (diff * cen_k) >> 8);
                const int m = (cen * core_amt) >> 8;
                r = Mix8(r, c3r, m);
                g = Mix8(g, c3g, m);
                b = Mix8(b, c3b, m);
            }
            if (diff < kRim4) {
                const int m = ((kRim4 - std::max(diff, 0)) * 135) / kRim4;
                r = Mix8(r, c3r, m);
                g = Mix8(g, c3g, m);
                b = Mix8(b, c3b, m);
            }
            const int dc = std::min(d4, shade_cap4);
            int lum = 255 - ((dc * dc * shade_k) >> 16) - row_dark;
            lum = std::max(lum, 148);
            r = (r * lum) >> 8;
            g = (g * lum) >> 8;
            b = (b * lum) >> 8;
            if (spec_row != nullptr) {
                const int sx = x - spec_x0_;
                if (sx >= 0 && sx < spec_side_) {
                    const int sp = spec_row[sx];
                    r += ((255 - r) * sp) >> 8;
                    g += ((255 - g) * sp) >> 8;
                    b += ((255 - b) * sp) >> 8;
                }
            }
            srow[hx] = Pack565(r, g, b);
        }
    }
}

void FluidOrbRenderer::Render(const FluidOrbFrame& f) {
    if (!ok()) {
        return;
    }
    if (!bg_valid_ || std::memcmp(last_bg_, f.bg, 3) != 0) {
        Clear(f.bg);
    }

    const float max_r = size_ * 0.5f - kGlowPx - 1.0f;
    const float body_r = std::min(nominal_r_ * f.radius_scale * (1.0f + 0.16f * f.level),
                                  max_r - 6.0f);

    // 1) Silhouette: organic liquid droplet contour from 4 harmonics.
    const float amp[4] = {1.1f + 5.8f * f.bands[0], 0.8f + 4.8f * f.bands[1],
                          0.5f + 3.8f * f.bands[2], 0.3f + 3.0f * f.bands[3]};
    const float pspd[4] = {0.65f, -0.95f, 1.35f, -1.75f};
    int pidx[4];
    for (int k = 0; k < 4; ++k) {
        pidx[k] = static_cast<int>(f.time * pspd[k] * (kSinSize / kTwoPi)) + k * 37;
    }
    int rad4[kAngles];
    int max_r4 = 0;
    for (int j = 0; j < kAngles; ++j) {
        float r = body_r;
        for (int k = 0; k < 4; ++k) {
            r += amp[k] * sin_lut_[((k + 2) * j * 2 + pidx[k]) & (kSinSize - 1)];
        }
        r = std::clamp(r, 8.0f, max_r);
        rad4[j] = static_cast<int>(r * 4.0f);
        max_r4 = std::max(max_r4, rad4[j]);
    }

    // 2) Quarter-res fluid field + half-res shaded sphere interior.
    const int64_t t_field = esp_timer_get_time();
    RenderField(f, body_r);
    RenderShaded(f, rad4, max_r4, body_r);
    const int64_t t_compose = esp_timer_get_time();
    last_field_us_ = static_cast<uint32_t>(t_compose - t_field);

    // 3) Pre-packed outer glow table (eliminates per-pixel RGB math in outer halo).
    constexpr int kGlow4 = kGlowPx * 4;
    uint16_t glow565_lut[kGlow4 + 1];
    const int bgr = f.bg[0], bgg = f.bg[1], bgb = f.bg[2];
    const uint16_t bg565 = Pack565(bgr, bgg, bgb);
    const int gr = f.glow_color[0], gg = f.glow_color[1], gb = f.glow_color[2];
    const float gmax = std::clamp(f.glow, 0.0f, 1.0f) * 215.0f;
    for (int g = 0; g <= kGlow4; ++g) {
        const float u = 1.0f - static_cast<float>(g) / kGlow4;
        const int ga = static_cast<int>(gmax * u * u);
        glow565_lut[g] = Pack565(Mix8(bgr, gr, ga), Mix8(bgg, gg, ga), Mix8(bgb, gb, ga));
    }
    const int edge_ga = static_cast<int>(gmax);
    const int edge_r = Mix8(bgr, gr, edge_ga);
    const int edge_g = Mix8(bgg, gg, edge_ga);
    const int edge_b = Mix8(bgb, gb, edge_ga);

    const int reach4 = max_r4 + kGlow4 + 4;
    const float reach = reach4 * 0.25f;
    const float cy = (size_ - 1) * 0.5f;

    // 4) Full-resolution composition: fast table/copy for interior & halo, 2px AA on rim.
    for (int y = 0; y < size_; ++y) {
        uint16_t* prow = pixels_ + y * size_;
        const float dyf = std::fabs(y - cy);
        int x0 = size_, x1 = -1;
        if (dyf < reach) {
            const float w = std::sqrt(reach * reach - dyf * dyf);
            x0 = std::max(0, static_cast<int>(cy - w));
            x1 = std::min(size_ - 1, static_cast<int>(cy + w + 1.0f));
        }
        if (span_x0_[y] < x0) {
            const int e = std::min<int>(x0, span_x1_[y] + 1);
            if (e > span_x0_[y]) {
                Fill16(prow + span_x0_[y], e - span_x0_[y], bg565);
            }
        }
        if (span_x1_[y] > x1) {
            const int s = std::max<int>(x1 + 1, span_x0_[y]);
            if (span_x1_[y] >= s) {
                Fill16(prow + s, span_x1_[y] - s + 1, bg565);
            }
        }
        span_x0_[y] = static_cast<int16_t>(x0);
        span_x1_[y] = static_cast<int16_t>(x1);
        if (x0 > x1) {
            continue;
        }

        const bool lower = y >= half_;
        const int qy = lower ? (y - half_) : (half_ - 1 - y);
        const uint16_t* qd_row = qdist4_ + qy * half_;
        const uint8_t* qa_row = qangle_ + qy * half_;
        const uint16_t* srow = shaded_ + (y >> 1) * half_;

        for (int x = x0; x <= x1; ++x) {
            const bool right = x >= half_;
            const int qx = right ? (x - half_) : (half_ - 1 - x);
            const int d4 = qd_row[qx];
            if (d4 >= reach4) {
                prow[x] = bg565;
                continue;
            }
            const int a0 = qa_row[qx];
            int a;
            if (lower) {
                a = right ? a0 : (kAngles / 2 - a0);
            } else {
                a = right ? (kAngles - a0) : (kAngles / 2 + a0);
            }
            const int diff = rad4[a & (kAngles - 1)] - d4;

            if (diff >= 4) {
                prow[x] = srow[x >> 1];
            } else if (diff <= -4) {
                const int g = -diff;
                prow[x] = (g <= kGlow4) ? glow565_lut[g] : bg565;
            } else {
                const uint16_t c565 = srow[x >> 1];
                const int ir = (c565 >> 8) & 0xF8;
                const int ig = (c565 >> 3) & 0xFC;
                const int ib = (c565 << 3) & 0xF8;
                const int cov = (diff + 4) * 32;
                prow[x] = Pack565(Mix8(edge_r, ir, cov), Mix8(edge_g, ig, cov),
                                  Mix8(edge_b, ib, cov));
            }
        }
    }
    last_compose_us_ = static_cast<uint32_t>(esp_timer_get_time() - t_compose);
}
