#pragma once
#include <stdint.h>

namespace popup_notice {
    // True while a popup notice is visible on the display.
    bool showing();

    // Show a popup notification for 5 seconds (or custom timeoutMs if specified).
    // msg: text body of the notification.
    // title: optional title (defaults to "Notice" if nullptr or empty).
    // timeoutMs: display duration in ms (defaults to 5000 ms).
    void show(const char *msg, const char *title = nullptr, uint32_t timeoutMs = 5000);

    // Dismiss the popup immediately and restore the active screen.
    void dismiss();
}
