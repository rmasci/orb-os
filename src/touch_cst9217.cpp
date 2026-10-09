// CST9217 capacitive touch over I2C (Arduino). Ported from Waveshare's
// esp_lcd_touch_cst9217: read 10 bytes from reg 0xD000, validate ACK 0xAB,
// unpack the 12-bit X/Y of the first touch point. Single-touch is enough here.
#include "touch_cst9217.h"
#include "config.h"
#include "display.h"
#include "input_router.h"
#include "app_shell.h"
#include "popup_notice.h"
#include "diag_log.h"
#include <Arduino.h>
#include <Wire.h>

extern void host_wake_display();
extern bool host_is_idle();

#define CST9217_REG_DATA 0xD000
#define CST9217_ACK      0xAB
#define CST9217_DATA_LEN 10        // 1 point * 5 + 5

// Read `len` bytes from a 16-bit register (address sent big-endian, then STOP).
static bool cst_read_reg(uint16_t reg, uint8_t *data, uint8_t len) {
    Wire.beginTransmission((uint8_t)I2C_ADDR_TOUCH);
    Wire.write((uint8_t)(reg >> 8));
    Wire.write((uint8_t)(reg & 0xFF));
    if (Wire.endTransmission(true) != 0) return false;
    delayMicroseconds(500);                       // chip needs a moment before the read
    if (Wire.requestFrom((uint8_t)I2C_ADDR_TOUCH, len) < len) return false;
    for (uint8_t i = 0; i < len; ++i) data[i] = Wire.read();
    return true;
}

bool touch_begin() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);

    // hardware reset (low 10 ms, high 50 ms)
    pinMode(PIN_TP_RST, OUTPUT);
    digitalWrite(PIN_TP_RST, LOW);
    delay(10);
    digitalWrite(PIN_TP_RST, HIGH);
    delay(50);
    pinMode(PIN_TP_INT, INPUT);

    // comms check: a valid (even untouched) data read carries ACK 0xAB at byte 6
    uint8_t d[CST9217_DATA_LEN] = {0};
    if (cst_read_reg(CST9217_REG_DATA, d, CST9217_DATA_LEN) && d[6] == CST9217_ACK) {
        Serial.println("[touch] CST9217 responding (ACK ok)");
    } else {
        Serial.println("[touch] CST9217 not responding yet (will keep polling)");
    }
    return true;
}

bool touch_read(uint16_t *ox, uint16_t *oy) {
    uint8_t d[CST9217_DATA_LEN];
    if (!cst_read_reg(CST9217_REG_DATA, d, CST9217_DATA_LEN)) return false;
    if (d[6] != CST9217_ACK) return false;

    const uint8_t points = d[5] & 0x7F;
    if (points == 0) return false;
    if ((d[0] & 0x0F) != 0x06) return false;       // status of point 0 must be "down"

    uint16_t x = ((uint16_t)d[1] << 4) | (d[3] >> 4);
    uint16_t y = ((uint16_t)d[2] << 4) | (d[3] & 0x0F);

    if (x > SCREEN_W - 1) x = SCREEN_W - 1;
    if (y > SCREEN_H - 1) y = SCREEN_H - 1;
    if (TP_MIRROR_X) x = (SCREEN_W - 1) - x;
    if (TP_MIRROR_Y) y = (SCREEN_H - 1) - y;

    *ox = x;
    *oy = y;
    return true;
}

void touch_poll() {
    static uint32_t s_lastPollMs = 0;
    const uint32_t now = millis();
    if ((now - s_lastPollMs) < 20) return;
    s_lastPollMs = now;

    uint16_t rx = 0, ry = 0;
    bool down = touch_read(&rx, &ry);

    int lx = 0, ly = 0;
    if (down) {
        display::mapTouchCoords(rx, ry, lx, ly);
    }

    static bool     s_touching = false;
    static int      s_startX = 0, s_startY = 0;
    static int      s_lastY = 0;
    static int      s_lastLogicalX = 0, s_lastLogicalY = 0;
    static uint32_t s_touchStartTime = 0;
    static bool     s_didScroll = false;
    static bool     s_wokeFromIdle = false;
    static int      s_scrollAccum = 0;

    if (!s_touching && down) {
        // Touch down
        s_touching = true;
        s_startX = lx;
        s_startY = ly;
        s_lastY = ly;
        s_lastLogicalX = lx;
        s_lastLogicalY = ly;
        s_scrollAccum = 0;
        s_didScroll = false;
        s_touchStartTime = now;

        if (host_is_idle()) {
            s_wokeFromIdle = true;
            host_wake_display();
        } else {
            s_wokeFromIdle = false;
            display::noteActivity();
        }
    } else if (s_touching && down) {
        // Finger moving / held
        s_lastLogicalX = lx;
        s_lastLogicalY = ly;
        display::noteActivity();

        int dx = lx - s_startX;
        int dy = ly - s_startY;
        bool isHorizontal = (abs(dx) > 30) && (abs(dx) > (abs(dy) * 3 / 2));

        if (!isHorizontal) {
            int stepDy = ly - s_lastY;
            s_lastY = ly;
            s_scrollAccum += stepDy;

            const int SCROLL_STEP_PX = 24;
            while (s_scrollAccum <= -SCROLL_STEP_PX) {
                s_scrollAccum += SCROLL_STEP_PX;
                s_didScroll = true;
                // Drag UP -> advance list (CW / delta = +1)
                input_router::dispatch(1, false);
            }
            while (s_scrollAccum >= SCROLL_STEP_PX) {
                s_scrollAccum -= SCROLL_STEP_PX;
                s_didScroll = true;
                // Drag DOWN -> retreat list (CCW / delta = -1)
                input_router::dispatch(-1, false);
            }
        }
    } else if (s_touching && !down) {
        // Touch released
        s_touching = false;
        uint32_t duration = now - s_touchStartTime;
        int totalDx = s_lastLogicalX - s_startX;
        int totalDy = s_lastLogicalY - s_startY;

        if (s_wokeFromIdle) {
            return;
        }

        if (s_didScroll) {
            return;
        }

        const int SWIPE_MIN_PX = 50;

        // Swipe Left to Right -> Settings
        if (totalDx >= SWIPE_MIN_PX && totalDx > (abs(totalDy) * 4 / 3)) {
            Serial.println("[touch] swipe left-to-right -> Settings");
            diag::log("touch swipe R -> Settings");
            if (popup_notice::showing()) popup_notice::dismiss();
            app_shell::selectApp(app_shell::APP_SETTINGS);
            return;
        }

        // Swipe Right to Left -> Clock
        if (totalDx <= -SWIPE_MIN_PX && (-totalDx) > (abs(totalDy) * 4 / 3)) {
            Serial.println("[touch] swipe right-to-left -> Clock");
            diag::log("touch swipe L -> Clock");
            if (popup_notice::showing()) popup_notice::dismiss();
            app_shell::selectApp(app_shell::APP_CLOCK);
            return;
        }

        // Tap (< 25px move, < 600ms) -> Knob push
        if (abs(totalDx) < 25 && abs(totalDy) < 25 && duration < 600) {
            Serial.println("[touch] tap -> button press");
            diag::log("touch tap -> push (app %s)", app_shell::name());
            input_router::dispatch(0, true);
            return;
        }
    }
}

