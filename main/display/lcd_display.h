#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "fluid_orb.h"
#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>
#include <string>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    static constexpr int kOrbCanvasSize = 152;

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* subtitle_viewport_ = nullptr;
    lv_obj_t* subtitle_accent_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* orb_canvas_ = nullptr;
    std::unique_ptr<FluidOrbRenderer> orb_renderer_;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_timer_t* anim_timer_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    bool power_save_mode_ = false;
    bool thinking_hint_active_ = false;
    std::string current_emotion_ = "neutral";

    // Fluid Voice Orb animation state
    float orb_phase_ = 0.0f;
    float orb_flow_time_ = 0.0f;
    float orb_scale_ = 1.0f;
    float orb_glow_ = 0.35f;
    float orb_warp_ = 1.0f;
    float smooth_level_ = 0.0f;
    float smooth_bands_[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    float orb_offset_y_ = -4.0f;
    uint32_t orb_render_us_acc_ = 0;
    uint32_t orb_render_frames_ = 0;
    uint32_t orb_frame_counter_ = 0;
    bool orb_half_rate_ = false;
    lv_color_t cur_color_top_ = lv_color_hex(0x0284C7);
    lv_color_t cur_color_bottom_ = lv_color_hex(0x38BDF8);
    lv_color_t cur_color_halo_ = lv_color_hex(0x0EA5E9);
    lv_color_t orb_base_ = lv_color_hex(0x1E3A8A);
    lv_color_t orb_c1_ = lv_color_hex(0x3B82F6);
    lv_color_t orb_c2_ = lv_color_hex(0x93C5FD);
    lv_color_t orb_c3_ = lv_color_hex(0xE0F2FE);

    // Smooth multiline subtitle state
    std::string target_subtitle_text_;
    std::string displayed_subtitle_text_;
    std::string subtitle_role_;
    size_t revealed_bytes_ = 0;
    uint32_t reveal_tick_div_ = 0;
    float smooth_scroll_y_ = 0.0f;
    float target_scroll_y_ = 0.0f;

    void InitializeLcdThemes();
    void OnAnimationTimer();
    void UpdateSubtitleTargetScroll();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual bool SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void RemoveChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    virtual void SetPowerSaveMode(bool on) override;

    // Set whether to hide chat messages/subtitles
    virtual void SetHideSubtitle(bool hide) override;
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
