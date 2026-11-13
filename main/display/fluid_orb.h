#ifndef FLUID_ORB_H
#define FLUID_ORB_H

#include <cstdint>

// Per-frame inputs for the fluid orb. Colors are 8-bit RGB triplets.
struct FluidOrbFrame {
    float time = 0.0f;          // Monotonic phase (radians-ish), drives flow and edge motion
    float level = 0.0f;         // Smoothed voice energy 0..1
    float bands[5] = {0};       // Smoothed band energies 0..1 (low -> high)
    float radius_scale = 1.0f;  // Breathing / mode scale around nominal radius
    float glow = 0.4f;          // Outer glow strength 0..1
    float warp = 1.0f;          // Interior turbulence multiplier
    uint8_t base[3] = {0};      // Deep body color
    uint8_t c1[3] = {0};        // Flowing blob colors
    uint8_t c2[3] = {0};
    uint8_t c3[3] = {0};        // Highlight / rim color
    uint8_t glow_color[3] = {0};
    uint8_t bg[3] = {0};        // Screen background (canvas is opaque)
};

// Software renderer for a ChatGPT-Voice / Gemini-Live style liquid orb.
// Interior color flow is computed at half resolution (domain-warped metaball
// color blobs); the silhouette, anti-aliasing, sphere shading and outer glow
// are composed at full resolution into an opaque RGB565 buffer.
class FluidOrbRenderer {
public:
    explicit FluidOrbRenderer(int size);
    ~FluidOrbRenderer();

    FluidOrbRenderer(const FluidOrbRenderer&) = delete;
    FluidOrbRenderer& operator=(const FluidOrbRenderer&) = delete;

    bool ok() const { return pixels_ != nullptr; }
    int size() const { return size_; }
    uint16_t* pixels() const { return pixels_; }

    // Fill whole canvas with background (used on theme change / init).
    void Clear(const uint8_t bg[3]);
    void Render(const FluidOrbFrame& frame);

    float nominal_radius() const { return nominal_r_; }
    uint32_t last_field_us() const { return last_field_us_; }
    uint32_t last_compose_us() const { return last_compose_us_; }

private:
    static constexpr int kAngles = 128;
    static constexpr int kSinSize = 256;
    static constexpr int kGlowPx = 11;
    static constexpr int kMaxBlobs = 4;

    int size_ = 0;
    int half_ = 0;
    float nominal_r_ = 0.0f;

    uint16_t* pixels_ = nullptr;  // size_*size_ RGB565 (PSRAM ok, read by LVGL)
    // Quadrant-symmetric lookup tables (half_*half_), kept in internal RAM when possible
    uint16_t* qdist4_ = nullptr;  // distance from center in 1/4 px
    uint8_t* qangle_ = nullptr;   // first-quadrant angle index 0..kAngles/4
    uint8_t* spec_box_ = nullptr; // specular highlight patch (spec_side_^2)
    int16_t* row_shade_ = nullptr;   // per-row vertical shading term
    int16_t* span_x0_ = nullptr;     // last written span per row (for incremental bg fill)
    int16_t* span_x1_ = nullptr;
    uint8_t* field_ = nullptr;     // quarter_*quarter_*3 RGB888 interior color flow
    uint16_t* shaded_ = nullptr;   // half_*half_ RGB565 fully shaded interior
    int quarter_ = 0;
    int spec_x0_ = 0, spec_y0_ = 0, spec_side_ = 0;
    uint8_t last_bg_[3] = {0, 0, 0};
    bool bg_valid_ = false;
    uint32_t last_field_us_ = 0;
    uint32_t last_compose_us_ = 0;
    float sin_lut_[kSinSize] = {0};

    float FastSin(float x) const;
    void RenderField(const FluidOrbFrame& f, float body_r);
    void RenderShaded(const FluidOrbFrame& f, const int* rad4, int max_r4, float body_r);
    inline int AngleIndex(int x, int y, int qx_val) const;
};

#endif  // FLUID_ORB_H
