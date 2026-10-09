#include "popup_notice.h"

#include <lvgl.h>
#include <string.h>
#include "app_theme.h"
#include "theme_style.h"
#include "theme_select.h"
#include "radar_view.h"

#if defined(ESP_PLATFORM)
#include <Arduino.h>
#include "audio.h"
#include "display.h"
#endif

namespace {

static lv_obj_t   *s_panel = nullptr;
static lv_obj_t   *s_card  = nullptr;
static lv_obj_t   *s_title = nullptr;
static lv_obj_t   *s_body  = nullptr;
static lv_obj_t   *s_hint  = nullptr;
static lv_timer_t *s_timer = nullptr;

static void timer_cb(lv_timer_t *t) {
    s_timer = nullptr;
    popup_notice::dismiss();
}

}  // namespace

bool popup_notice::showing() {
    return s_panel != nullptr;
}

void popup_notice::show(const char *msg, const char *title, uint32_t timeoutMs) {
    if (!msg || !*msg) return;

    // Clear any existing active popup first
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    if (s_panel) {
        lv_obj_del(s_panel);
        s_panel = nullptr;
        s_card = s_title = s_body = s_hint = nullptr;
    }

    // Wake display from sleep or dimming
#if defined(ESP_PLATFORM)
    display::noteActivity();
#endif

    // Resolve palette based on the active theme
    lv_color_t cardBg;
    lv_color_t cardBorder;
    lv_color_t titleColor;
    lv_color_t bodyColor;
    lv_color_t hintColor;
    int cardRadius = 22;

    if (app_theme::get() == APP_THEME_OFFICE) {
        const AppPalette &p = app_theme::palette();
        cardBg     = p.panel;                     // crisp light panel (#F4F5F7)
        cardBorder = p.accent;                    // theme blue accent (#3B5BFF)
        titleColor = p.accent;                    // title in theme accent
        bodyColor  = p.ink;                       // dark charcoal ink
        hintColor  = p.soft;                      // secondary soft text
        cardRadius = 18;
    } else {
        const char *slug = theme_select::activeSlug();
        const int rTheme = radar::theme();
        bool isAviator = (rTheme == THEME_AVIATOR) || (slug && !strcasecmp(slug, "aviator"));

        if (isAviator) {
            // Aviator: warm brass/bronze chrome with ivory and cream typography
            cardBg     = lv_color_hex(0x1B160F); // warm deep walnut/leather tone
            cardBorder = lv_color_hex(0x6B5A3A); // Aviator brass / antique bronze
            titleColor = lv_color_hex(0xDACFA6); // warm ivory / gold
            bodyColor  = lv_color_hex(0xEDE3CC); // cream ivory text
            hintColor  = lv_color_hex(0x9C8F73); // warm soft secondary
            cardRadius = 20;
        } else if (rTheme == THEME_MILITARY) {
            // Military: tactical dark green cockpit
            cardBg     = lv_color_hex(0x0C140E);
            cardBorder = lv_color_hex(0x1E3A2E);
            titleColor = lv_color_hex(0x39FF8A); // phosphor green
            bodyColor  = lv_color_hex(0xEAFFF3);
            hintColor  = lv_color_hex(0x5F7A6C);
            cardRadius = 16;
        } else {
            // Custom SD theme from Launch Kit or Default Orb
            const auto &set = theme_style::settings();
            if (set.hlShow && set.hlColor != 0 && set.hlColor != 0x232A36) {
                cardBg     = lv_color_hex(0x12151C);
                cardBorder = lv_color_hex(set.hlColor);
                titleColor = lv_color_hex(set.selColor ? set.selColor : 0xF39C12);
                bodyColor  = lv_color_white();
                hintColor  = lv_color_hex(set.itemColor ? set.itemColor : 0x8A94A6);
                cardRadius = (set.hlRadius > 0) ? set.hlRadius * 2 : 20;
            } else {
                // Default Orb style (classic slate & warm amber)
                cardBg     = lv_color_hex(0x161922);
                cardBorder = lv_color_hex(0x3A4254);
                titleColor = lv_color_hex(0xF39C12);
                bodyColor  = lv_color_white();
                hintColor  = lv_color_hex(0x6C757D);
                cardRadius = 22;
            }
        }
    }

    // Full-screen backdrop on top layer with translucent dimming
    s_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    // Centered rounded notification card themed to the active visual skin
    s_card = lv_obj_create(s_panel);
    lv_obj_set_width(s_card, 350);
    lv_obj_set_height(s_card, 250);
    lv_obj_set_style_bg_color(s_card, cardBg, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_card, cardBorder, 0);
    lv_obj_set_style_border_width(s_card, 2, 0);
    lv_obj_set_style_radius(s_card, cardRadius, 0);
    lv_obj_set_style_pad_all(s_card, 16, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(s_card);

    // Title label
    const char *titleText = (title && *title) ? title : "Notice";
    s_title = lv_label_create(s_card);
    lv_label_set_text(s_title, titleText);
    lv_obj_set_style_text_color(s_title, titleColor, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 8);

    // Message body label
    s_body = lv_label_create(s_card);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, 310);
    lv_label_set_text(s_body, msg);
    lv_obj_set_style_text_color(s_body, bodyColor, 0);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, 4);

    // Bottom dismiss hint
    s_hint = lv_label_create(s_card);
    lv_label_set_text(s_hint, "tap, turn or push to dismiss");
    lv_obj_set_style_text_color(s_hint, hintColor, 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    // Force an immediate refresh
    lv_refr_now(NULL);

    // Play pleasant notification chirp
#if defined(ESP_PLATFORM)
    if (audio_present()) {
        audio_play(AUDIO_NEW);
    }
    Serial.printf("[popup] \"%s\": %s\n", titleText, msg);
#endif

    // Setup auto-dismiss timer
    if (timeoutMs > 0) {
        s_timer = lv_timer_create(timer_cb, timeoutMs, nullptr);
        if (s_timer) lv_timer_set_repeat_count(s_timer, 1);
    }
}

void popup_notice::dismiss() {
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = nullptr;
    }
    if (!s_panel) return;

    lv_obj_del(s_panel);
    s_panel = nullptr;
    s_card = s_title = s_body = s_hint = nullptr;

    // Repaint underlying screen cleanly (same technique as knob_help and update_ui)
    if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(NULL);

    lv_timer_t *again = lv_timer_create([](lv_timer_t *t) {
        if (lv_obj_t *scr = lv_scr_act()) lv_obj_invalidate(scr);
        lv_obj_invalidate(lv_layer_top());
        lv_timer_del(t);
    }, 150, nullptr);
    if (again) lv_timer_set_repeat_count(again, 1);
}
