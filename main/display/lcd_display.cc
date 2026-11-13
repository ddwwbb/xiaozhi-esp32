#include "lcd_display.h"
#include "application.h"
#include "assets/lang_config.h"
#include "gif/lvgl_gif.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <material_symbols.h>
#include <noto_emoji.h>
#include <src/misc/cache/lv_cache.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "board.h"

namespace {

size_t NextUtf8Boundary(const std::string& s, size_t pos, int codepoints) {
    const size_t n = s.size();
    for (int c = 0; c < codepoints && pos < n; ++c) {
        ++pos;
        while (pos < n && (static_cast<uint8_t>(s[pos]) & 0xC0u) == 0x80u) {
            ++pos;
        }
    }
    return pos;
}

bool HasPositiveEmotion(const std::string& e) {
    return e == "happy" || e == "laughing" || e == "funny" || e == "loving" || e == "wink" ||
           e == "cool" || e == "delicious" || e == "kissy" || e == "confident";
}

bool HasAlertEmotion(const std::string& e) {
    return e == "angry" || e == "warning" || e == "cancel" || e == "shocked" || e == "surprised" ||
           e == "embarrassed";
}

bool HasCalmEmotion(const std::string& e) {
    return e == "sad" || e == "crying" || e == "sleepy" || e == "relaxed" || e == "confused";
}

}  // namespace

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                       int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback =
            [](void* arg) {
                LcdDisplay* display = static_cast<LcdDisplay*>(arg);
                display->SetPreviewImage(nullptr);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
#if CONFIG_LV_USE_BUILTIN_MALLOC
        // Base TLSF pool is CONFIG_LV_MEM_SIZE_KILOBYTES in PSRAM; grow it so
        // the 2MB decoded-image cache does not exhaust a 2MB-PSRAM-safe pool.
        // TLSF rejects any single pool larger than LV_MEM_SIZE when
        // LV_MEM_POOL_EXPAND_SIZE is 0 (block_size_max = 1 << ceil(log2(LV_MEM_SIZE))).
        const size_t extra_total = 2560 * 1024;
        const size_t chunk_size = LV_MEM_SIZE;
        size_t added = 0;
        while (added < extra_total) {
            size_t this_chunk = extra_total - added;
            if (this_chunk > chunk_size) {
                this_chunk = chunk_size;
            }
            void* pool = heap_caps_malloc(this_chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (pool == nullptr || lv_mem_add_pool(pool, this_chunk) == nullptr) {
                if (pool != nullptr) {
                    heap_caps_free(pool);
                }
                ESP_LOGE(TAG, "Failed to add %uKB LVGL PSRAM pool (added %uKB)",
                         (unsigned)(this_chunk / 1024), (unsigned)(added / 1024));
                break;
            }
            added += this_chunk;
        }
        if (added == extra_total) {
            ESP_LOGI(TAG, "Added %uKB LVGL pool in PSRAM", (unsigned)(added / 1024));
        }
#endif
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    // SPI LCD still needs an internal DMA bounce buffer for PSRAM sources.
    // A full-frame transfer (~150KB) cannot allocate that bounce buffer, so
    // keep a small DMA strip in internal SRAM and partial refresh.
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .swap_bytes = 1,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                             int width, int height, int offset_x, int offset_y, bool mirror_x,
                             bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
                .full_refresh = 1,
                .direct_mode = 1,
            },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {.flags = {
                                                     .bb_mode = true,
                                                     .avoid_tearing = true,
                                                 }};

    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                               int width, int height, int offset_x, int offset_y, bool mirror_x,
                               bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation =
            {
                .swap_xy = swap_xy,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = true,
                .buff_spiram = false,
                .sw_rotate = true,
            },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {.flags = {
                                                     .avoid_tearing = false,
                                                 }};
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);

    if (anim_timer_ != nullptr) {
        lv_timer_delete(anim_timer_);
        anim_timer_ = nullptr;
    }

    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }

    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void LcdDisplay::Unlock() { lvgl_port_unlock(); }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(),
                              0);  // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0);  // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0,
                 text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define MAX_MESSAGES 40
#else
#define MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called "
                     "but container not created)",
                     role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a
    // system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) &&
                lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr &&
                        strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if (strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;   // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100;  // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld",
                 img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256)
        zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release();  // Release ownership of smart pointer
    lv_obj_add_event_cb(
        preview_image,
        [](lv_event_t* e) {
            LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
            if (img != nullptr) {
                delete img;  // Properly release memory by deleting LvglImage object
            }
        },
        LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr || role == nullptr || content == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(content_); ++i) {
        auto container = lv_obj_get_child(content_, i);
        auto bubble = container;
        auto bubble_role = static_cast<const char*>(lv_obj_get_user_data(bubble));
        if (bubble_role == nullptr && lv_obj_get_child_cnt(bubble) > 0) {
            bubble = lv_obj_get_child(bubble, 0);
            bubble_role = static_cast<const char*>(lv_obj_get_user_data(bubble));
        }
        if (bubble_role == nullptr || strcmp(bubble_role, role) != 0 ||
            lv_obj_get_child_cnt(bubble) == 0) {
            continue;
        }
        auto label = lv_obj_get_child(bubble, 0);
        if (strcmp(lv_label_get_text(label), content) != 0) {
            continue;
        }
        if (chat_message_label_ == label) {
            chat_message_label_ = nullptr;
        }
        lv_obj_del(container);
        if (lv_obj_get_child_cnt(content_) == 0 && emoji_label_ != nullptr) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - Fluid Voice Orb stage */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, 164, 164);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_set_scrollbar_mode(emoji_box_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, -4);

    // Fluid Voice Orb: software-rendered liquid sphere on an opaque RGB565 canvas
    orb_renderer_ = std::make_unique<FluidOrbRenderer>(kOrbCanvasSize);
    if (orb_renderer_->ok()) {
        const lv_color_t bg = lvgl_theme->background_color();
        const uint8_t bg_rgb[3] = {bg.red, bg.green, bg.blue};
        orb_renderer_->Clear(bg_rgb);
        orb_canvas_ = lv_canvas_create(emoji_box_);
        lv_canvas_set_buffer(orb_canvas_, orb_renderer_->pixels(), kOrbCanvasSize,
                             kOrbCanvasSize, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(orb_canvas_, LV_ALIGN_CENTER, 0, 0);
    } else {
        ESP_LOGE(TAG, "Fluid orb renderer unavailable, orb disabled");
        orb_renderer_.reset();
    }

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, MATERIAL_SYMBOLS_ROBOT_2);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);        // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.75);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Multiline smooth-scrolling subtitle card (3-4 visible lines) */
    const lv_coord_t card_w = LV_HOR_RES - 16;
    const lv_coord_t line_h = text_font->line_height > 0 ? text_font->line_height : 22;
    const lv_coord_t view_h = line_h * 4 + 4;
    const lv_coord_t card_h = view_h + 16;

    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, card_w, card_h);
    lv_obj_set_style_radius(bottom_bar_, 12, 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->chat_background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_border_width(bottom_bar_, 1, 0);
    lv_obj_set_style_border_color(bottom_bar_, cur_color_bottom_, 0);
    lv_obj_set_style_border_opa(bottom_bar_, LV_OPA_30, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(bottom_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -6);

    subtitle_accent_ = lv_obj_create(bottom_bar_);
    lv_obj_set_size(subtitle_accent_, 4, view_h - 6);
    lv_obj_set_style_radius(subtitle_accent_, 2, 0);
    lv_obj_set_style_bg_color(subtitle_accent_, cur_color_bottom_, 0);
    lv_obj_set_style_bg_opa(subtitle_accent_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(subtitle_accent_, 0, 0);
    lv_obj_set_scrollbar_mode(subtitle_accent_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(subtitle_accent_, LV_ALIGN_LEFT_MID, 7, 0);

    subtitle_viewport_ = lv_obj_create(bottom_bar_);
    lv_obj_set_size(subtitle_viewport_, card_w - 24, view_h);
    lv_obj_set_style_bg_opa(subtitle_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(subtitle_viewport_, 0, 0);
    lv_obj_set_style_pad_all(subtitle_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(subtitle_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(subtitle_viewport_, LV_DIR_VER);
    lv_obj_align(subtitle_viewport_, LV_ALIGN_LEFT_MID, 16, 0);

    chat_message_label_ = lv_label_create(subtitle_viewport_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, card_w - 24);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(chat_message_label_, 0, 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content

    if (anim_timer_ == nullptr) {
        anim_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<LcdDisplay*>(lv_timer_get_user_data(t));
                if (self != nullptr) {
                    self->OnAnimationTimer();
                }
            },
            20, this);
    }

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::UpdateSubtitleTargetScroll() {
    if (chat_message_label_ == nullptr || subtitle_viewport_ == nullptr) {
        return;
    }
    lv_obj_update_layout(subtitle_viewport_);
    lv_coord_t label_h = lv_obj_get_height(chat_message_label_);
    lv_coord_t view_h = lv_obj_get_height(subtitle_viewport_);
    target_scroll_y_ = (label_h > view_h) ? static_cast<float>(label_h - view_h) : 0.0f;
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!",
                 role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() "
                     "was called but label not created)",
                     role, content);
        }
        return;
    }

    const std::string new_role = (role != nullptr) ? role : "";
    const std::string new_text = (content != nullptr) ? content : "";

    if (new_text.empty()) {
        target_subtitle_text_.clear();
        displayed_subtitle_text_.clear();
        subtitle_role_.clear();
        revealed_bytes_ = 0;
        smooth_scroll_y_ = 0.0f;
        target_scroll_y_ = 0.0f;
        lv_label_set_text(chat_message_label_, "");
        if (subtitle_viewport_ != nullptr) {
            lv_obj_scroll_to_y(subtitle_viewport_, 0, LV_ANIM_OFF);
        }
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    // Accent bar color per role
    if (subtitle_accent_ != nullptr) {
        lv_color_t accent = lv_color_hex(0x3B82F6);
        if (new_role == "user") {
            accent = lv_color_hex(0x10B981);
        } else if (new_role == "system") {
            accent = lv_color_hex(0xF59E0B);
        }
        lv_obj_set_style_bg_color(subtitle_accent_, accent, 0);
        if (bottom_bar_ != nullptr) {
            lv_obj_set_style_border_color(bottom_bar_, accent, 0);
        }
    }

    // Center align short single-line system prompts, left-align conversational turns
    if (new_role == "system" && new_text.size() <= 36) {
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    } else {
        lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_LEFT, 0);
    }

    const bool same_role = (new_role == subtitle_role_);
    const bool is_prefix_extension =
        same_role && !target_subtitle_text_.empty() &&
        new_text.size() >= target_subtitle_text_.size() &&
        new_text.compare(0, target_subtitle_text_.size(), target_subtitle_text_) == 0;

    subtitle_role_ = new_role;
    target_subtitle_text_ = new_text;

    if (new_role == "user") {
        // Streaming ASR interim or final user speech: show immediately and damp-scroll to bottom
        revealed_bytes_ = target_subtitle_text_.size();
        if (displayed_subtitle_text_ != target_subtitle_text_) {
            displayed_subtitle_text_ = target_subtitle_text_;
            lv_label_set_text(chat_message_label_, displayed_subtitle_text_.c_str());
        }
        if (!is_prefix_extension) {
            smooth_scroll_y_ = 0.0f;
            if (subtitle_viewport_ != nullptr) {
                lv_obj_scroll_to_y(subtitle_viewport_, 0, LV_ANIM_OFF);
            }
        }
        UpdateSubtitleTargetScroll();
    } else if (new_role == "assistant") {
        thinking_hint_active_ = false;
        if (is_prefix_extension) {
            // Keep current typewriter progress and smooth scroll anchor
            UpdateSubtitleTargetScroll();
        } else {
            // New sentence: start progressive typewriter reveal from first 2 codepoints
            smooth_scroll_y_ = 0.0f;
            target_scroll_y_ = 0.0f;
            reveal_tick_div_ = 0;
            if (subtitle_viewport_ != nullptr) {
                lv_obj_scroll_to_y(subtitle_viewport_, 0, LV_ANIM_OFF);
            }
            revealed_bytes_ = NextUtf8Boundary(target_subtitle_text_, 0, 2);
            displayed_subtitle_text_ = target_subtitle_text_.substr(0, revealed_bytes_);
            lv_label_set_text(chat_message_label_, displayed_subtitle_text_.c_str());
            UpdateSubtitleTargetScroll();
        }
    } else {
        // System message
        revealed_bytes_ = target_subtitle_text_.size();
        displayed_subtitle_text_ = target_subtitle_text_;
        lv_label_set_text(chat_message_label_, displayed_subtitle_text_.c_str());
        smooth_scroll_y_ = 0.0f;
        if (subtitle_viewport_ != nullptr) {
            lv_obj_scroll_to_y(subtitle_viewport_, 0, LV_ANIM_OFF);
        }
        UpdateSubtitleTargetScroll();
    }

    if (bottom_bar_ != nullptr && !hide_subtitle_) {
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::RemoveChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || content == nullptr ||
        target_subtitle_text_ != content) {
        return;
    }
    target_subtitle_text_.clear();
    displayed_subtitle_text_.clear();
    subtitle_role_.clear();
    revealed_bytes_ = 0;
    smooth_scroll_y_ = 0.0f;
    target_scroll_y_ = 0.0f;
    lv_label_set_text(chat_message_label_, "");
    if (subtitle_viewport_ != nullptr) {
        lv_obj_scroll_to_y(subtitle_viewport_, 0, LV_ANIM_OFF);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    target_subtitle_text_.clear();
    displayed_subtitle_text_.clear();
    subtitle_role_.clear();
    revealed_bytes_ = 0;
    smooth_scroll_y_ = 0.0f;
    target_scroll_y_ = 0.0f;
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (subtitle_viewport_ != nullptr) {
        lv_obj_scroll_to_y(subtitle_viewport_, 0, LV_ANIM_OFF);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

bool LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!",
                 emotion);
    }
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (emoji_box_ != nullptr) {
        DisplayLockGuard lock(this);
        current_emotion_ = (emotion != nullptr && emotion[0] != '\0') ? emotion : "neutral";
        if (current_emotion_ == "thinking") {
            thinking_hint_active_ = true;
        } else if (current_emotion_ == "neutral") {
            thinking_hint_active_ = false;
        }
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        return true;
    }
#endif
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG,
                     "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but "
                     "emoji image not created)",
                     emotion);
        }
        return false;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        const char* utf8 = noto_emoji_get_utf8(emotion);
        const lv_font_t* emotion_font = lvgl_theme->emoji_font()->font();
        if (utf8 == nullptr) {
            utf8 = material_symbols_get_utf8(emotion);
            emotion_font = lvgl_theme->large_icon_font()->font();
        }
        if (utf8 == nullptr || emoji_label_ == nullptr) {
            // No image and no glyph for this name: nothing was shown for the request.
            return false;
        }
        DisplayLockGuard lock(this);
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        lv_obj_set_style_text_font(emoji_label_, emotion_font, 0);
        lv_label_set_text(emoji_label_, utf8);
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        // The glyph is this emotion's own, from noto_emoji or material_symbols, not a
        // stand-in - so the request was honoured.
        return true;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    bool shown = false;
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback(
                [this]() { lv_image_set_src(emoji_image_, gif_controller_->image_dsc()); });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            shown = true;
        } else {
            // The collection had an image for this emotion but it would not decode, so
            // nothing is on screen for the request.
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        shown = true;
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
    // The WeChat style deliberately hides a neutral emotion. That is a choice about
    // presentation, not a failure to honour the request, so `shown` stands as it was.
    return shown;
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr)
            continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr)
            continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // Update bottom bar background color with 80% opacity
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->chat_background_color(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetPowerSaveMode(bool on) {
    LvglDisplay::SetPowerSaveMode(on);
    DisplayLockGuard lock(this);
    power_save_mode_ = on;
    if (anim_timer_ != nullptr) {
        if (on) {
            lv_timer_pause(anim_timer_);
        } else {
            lv_timer_resume(anim_timer_);
        }
    }
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!target_subtitle_text_.empty()) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void LcdDisplay::OnAnimationTimer() {
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    if (power_save_mode_ || emoji_box_ == nullptr) {
        return;
    }

    auto& app = Application::GetInstance();
    const DeviceState dev_state = app.GetDeviceState();
    const AudioVisualSnapshot snap = app.GetAudioService().GetVisualSnapshot();

    enum class OrbMode {
        kIdle,
        kListening,
        kThinking,
        kSpeaking,
    };

    if (dev_state == kDeviceStateListening && snap.active && snap.level > 0.14f) {
        thinking_hint_active_ = false;
    }

    OrbMode mode = OrbMode::kIdle;
    if (dev_state == kDeviceStateSpeaking || dev_state == kDeviceStateNotifying) {
        mode = OrbMode::kSpeaking;
        thinking_hint_active_ = false;
    } else if (dev_state == kDeviceStateConnecting || thinking_hint_active_ ||
               current_emotion_ == "thinking") {
        mode = OrbMode::kThinking;
    } else if (dev_state == kDeviceStateListening || dev_state == kDeviceStateAudioTesting) {
        mode = OrbMode::kListening;
    } else {
        mode = OrbMode::kIdle;
    }

    // Palette per mode: base (deep body), c1/c2 (flowing blobs), c3 (highlight/rim), halo (glow)
    struct OrbPalette {
        uint32_t base, c1, c2, c3, halo, accent;
    };
    // ChatGPT Voice style: deep blue body with white-cyan clouds
    OrbPalette pal = {0x1E3A8A, 0x3B82F6, 0x60A5FA, 0xE0F2FE, 0x3B82F6, 0x38BDF8};
    if (mode == OrbMode::kListening) {
        // Gemini Live listening: blue -> cyan -> mint
        pal = {0x0B3B8C, 0x22D3EE, 0x34D399, 0xECFEFF, 0x22D3EE, 0x2DD4BF};
    } else if (mode == OrbMode::kThinking) {
        // Thinking: indigo/violet swirl with pink sparks
        pal = {0x312E81, 0x8B5CF6, 0x3B82F6, 0xF5D0FE, 0x8B5CF6, 0xA78BFA};
    } else if (mode == OrbMode::kSpeaking) {
        if (HasPositiveEmotion(current_emotion_)) {
            pal = {0x9A3412, 0xFB923C, 0xF472B6, 0xFEF3C7, 0xF97316, 0xFBBF24};
        } else if (HasAlertEmotion(current_emotion_)) {
            pal = {0x7F1D1D, 0xEF4444, 0xFB7185, 0xFFE4E6, 0xEF4444, 0xFB7185};
        } else if (HasCalmEmotion(current_emotion_)) {
            pal = {0x1E1B4B, 0x6366F1, 0x38BDF8, 0xE0E7FF, 0x6366F1, 0x818CF8};
        } else {
            // Gemini speaking gradient: blue -> violet -> pink
            pal = {0x1E40AF, 0x4F8BFF, 0xA78BFA, 0xFCE7F3, 0x6D7CFF, 0x60A5FA};
        }
    } else if (HasAlertEmotion(current_emotion_)) {
        pal = {0x7F1D1D, 0xEF4444, 0xFB7185, 0xFFE4E6, 0xEF4444, 0xFB7185};
    } else if (HasCalmEmotion(current_emotion_)) {
        pal = {0x1E1B4B, 0x4F46E5, 0x6366F1, 0xE0E7FF, 0x4F46E5, 0x6366F1};
    }

    // Smooth cross-fade palette (~15% per 20ms frame)
    constexpr uint8_t kMix = 38;
    orb_base_ = lv_color_mix(lv_color_hex(pal.base), orb_base_, kMix);
    orb_c1_ = lv_color_mix(lv_color_hex(pal.c1), orb_c1_, kMix);
    orb_c2_ = lv_color_mix(lv_color_hex(pal.c2), orb_c2_, kMix);
    orb_c3_ = lv_color_mix(lv_color_hex(pal.c3), orb_c3_, kMix);
    cur_color_halo_ = lv_color_mix(lv_color_hex(pal.halo), cur_color_halo_, kMix);
    cur_color_bottom_ = lv_color_mix(lv_color_hex(pal.accent), cur_color_bottom_, kMix);

    // Target envelope & 5-band sampling / synthesis
    float target_level = 0.0f;
    float target_bands[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    if (mode == OrbMode::kListening || mode == OrbMode::kSpeaking) {
        if (snap.active) {
            target_level = snap.level;
            for (int i = 0; i < 5; ++i) {
                target_bands[i] = snap.bands[i];
            }
        }
        if (mode == OrbMode::kSpeaking && target_level < 0.06f) {
            // Gentle living pulse between words during TTS
            target_level = 0.06f + 0.03f * std::sin(orb_phase_ * 1.4f);
            for (int i = 0; i < 5; ++i) {
                target_bands[i] = std::max(target_bands[i], target_level);
            }
        }
    } else if (mode == OrbMode::kThinking) {
        target_level = 0.10f + 0.06f * std::sin(orb_phase_ * 1.6f);
        for (int i = 0; i < 5; ++i) {
            target_bands[i] = 0.18f + 0.14f * std::sin(orb_phase_ * 2.0f + i * 0.9f);
        }
    } else {
        target_level = 0.0f;
        for (int i = 0; i < 5; ++i) {
            target_bands[i] = 0.05f + 0.04f * std::sin(orb_phase_ * 0.7f + i * 1.3f);
        }
    }

    // Asymmetric spring filter (fast attack, smooth release)
    const float alpha_lvl = (target_level > smooth_level_) ? 0.45f : 0.14f;
    smooth_level_ += (target_level - smooth_level_) * alpha_lvl;
    for (int i = 0; i < 5; ++i) {
        const float alpha_b = (target_bands[i] > smooth_bands_[i]) ? 0.42f : 0.12f;
        smooth_bands_[i] += (target_bands[i] - smooth_bands_[i]) * alpha_b;
    }

    // Mode-dependent body dynamics, eased so transitions never snap
    float target_scale = 1.0f;
    float target_glow = 0.35f;
    float target_warp = 1.0f;
    float flow_speed = 0.035f;
    if (mode == OrbMode::kIdle) {
        target_scale = 0.94f + 0.03f * std::sin(orb_phase_ * 0.8f);  // slow breathing
        target_glow = 0.28f;
        target_warp = 0.7f;
        flow_speed = 0.022f;
    } else if (mode == OrbMode::kListening) {
        target_scale = 1.0f;
        target_glow = 0.40f + smooth_level_ * 0.55f;
        target_warp = 1.0f;
        flow_speed = 0.040f + smooth_level_ * 0.09f;
    } else if (mode == OrbMode::kThinking) {
        target_scale = 0.88f + 0.05f * std::sin(orb_phase_ * 1.8f);
        target_glow = 0.45f;
        target_warp = 1.6f;
        flow_speed = 0.095f;  // fast swirl
    } else {  // speaking
        target_scale = 1.02f;
        target_glow = 0.45f + smooth_level_ * 0.55f;
        target_warp = 1.1f;
        flow_speed = 0.045f + smooth_level_ * 0.11f;
    }
    orb_scale_ += (target_scale - orb_scale_) * 0.10f;
    orb_glow_ += (target_glow - orb_glow_) * 0.15f;
    orb_warp_ += (target_warp - orb_warp_) * 0.08f;
    orb_flow_time_ += flow_speed;
    if (orb_flow_time_ > 20000.0f) {
        orb_flow_time_ -= 20000.0f;  // rare (hours); keeps float precision for sin phases
    }
    orb_phase_ += 0.05f;
    if (orb_phase_ > 628.3185f) {
        orb_phase_ -= 628.3185f;
    }

    // Smoothly glide Orb stage vertically depending on whether subtitle card is shown
    const bool subtitle_shown =
        (bottom_bar_ != nullptr) && !lv_obj_has_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    const float target_stage_y = subtitle_shown ? -46.0f : -6.0f;
    if (std::fabs(target_stage_y - orb_offset_y_) > 0.3f) {
        orb_offset_y_ += (target_stage_y - orb_offset_y_) * 0.18f;
        lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, static_cast<lv_coord_t>(orb_offset_y_ + 0.5f));
    }

    // Render liquid orb (adaptive: drop to 25fps if the frame budget is exceeded)
    ++orb_frame_counter_;
    const bool orb_visible = orb_canvas_ != nullptr && orb_renderer_ != nullptr &&
                             !lv_obj_has_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (orb_visible && (!orb_half_rate_ || (orb_frame_counter_ & 1u) == 0u)) {
        FluidOrbFrame frame;
        frame.time = orb_flow_time_;
        frame.level = smooth_level_;
        for (int i = 0; i < 5; ++i) {
            frame.bands[i] = smooth_bands_[i];
        }
        frame.radius_scale = orb_scale_;
        frame.glow = orb_glow_;
        frame.warp = orb_warp_;
        auto to_rgb = [](lv_color_t c, uint8_t out[3]) {
            out[0] = c.red;
            out[1] = c.green;
            out[2] = c.blue;
        };
        to_rgb(orb_base_, frame.base);
        to_rgb(orb_c1_, frame.c1);
        to_rgb(orb_c2_, frame.c2);
        to_rgb(orb_c3_, frame.c3);
        to_rgb(cur_color_halo_, frame.glow_color);
        to_rgb(static_cast<LvglTheme*>(current_theme_)->background_color(), frame.bg);

        const int64_t t0 = esp_timer_get_time();
        orb_renderer_->Render(frame);
        orb_render_us_acc_ += static_cast<uint32_t>(esp_timer_get_time() - t0);
        ++orb_render_frames_;
        lv_obj_invalidate(orb_canvas_);

        if (orb_render_frames_ >= 250) {
            const uint32_t avg_us = orb_render_us_acc_ / orb_render_frames_;
            const bool want_half = avg_us > 9000;
            if (want_half != orb_half_rate_) {
                orb_half_rate_ = want_half;
            }
            ESP_LOGI(TAG, "Fluid orb render avg %lu us/frame (last field %lu us, compose %lu us) %s",
                     static_cast<unsigned long>(avg_us),
                     static_cast<unsigned long>(orb_renderer_->last_field_us()),
                     static_cast<unsigned long>(orb_renderer_->last_compose_us()),
                     orb_half_rate_ ? "25fps" : "50fps");
            orb_render_us_acc_ = 0;
            orb_render_frames_ = 0;
        }
    }

    // Progressive typewriter reveal for assistant subtitles
    if (chat_message_label_ != nullptr && revealed_bytes_ < target_subtitle_text_.size()) {
        ++reveal_tick_div_;
        const size_t remaining = target_subtitle_text_.size() - revealed_bytes_;
        // ~22 chars/sec normal pace; speed up slightly if backlog is long
        const bool should_advance = (remaining > 96) || ((reveal_tick_div_ & 1u) == 0u);
        if (should_advance) {
            int step_chars = (remaining > 180) ? 2 : 1;
            revealed_bytes_ = NextUtf8Boundary(target_subtitle_text_, revealed_bytes_, step_chars);
            displayed_subtitle_text_ = target_subtitle_text_.substr(0, revealed_bytes_);
            lv_label_set_text(chat_message_label_, displayed_subtitle_text_.c_str());
            UpdateSubtitleTargetScroll();
        }
    }

    // Smooth exponential-damped vertical scrolling for multiline subtitle viewport
    if (subtitle_viewport_ != nullptr && std::fabs(target_scroll_y_ - smooth_scroll_y_) > 0.4f) {
        smooth_scroll_y_ += (target_scroll_y_ - smooth_scroll_y_) * 0.22f;
        if (std::fabs(target_scroll_y_ - smooth_scroll_y_) <= 0.6f) {
            smooth_scroll_y_ = target_scroll_y_;
        }
        lv_obj_scroll_to_y(subtitle_viewport_, static_cast<lv_coord_t>(smooth_scroll_y_ + 0.5f),
                           LV_ANIM_OFF);
    }
#endif
}
