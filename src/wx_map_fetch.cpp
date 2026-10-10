#include "wx_map_fetch.h"
#include "wx_radar.h"
#include "theme_style.h"
#include "theme_sd.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <new>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "sdcard.h"
#include "net_fetch.h"
#include "config.h"
#else
#include <cstdarg>
static struct {
    void println(const char *s) { printf("%s\n", s); }
    void printf(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
} Serial;
static uint32_t millis() { return 0; }
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif

#include <PNGdec.h>

namespace {

struct WeatherMapHeader {
    char     magic[8];       // "ORBMAP1\0"
    float    lat;            // Latitude
    float    lon;            // Longitude
    uint16_t width;          // 360
    uint16_t height;         // 360
    uint32_t timestamp;      // e.g. millis() or boot timestamp
    uint8_t  reserved[12];   // Zero
};

static const char *MAP_SD_PATH = "/weather_map.raw";

static double s_loadedLat = 999.0;
static double s_loadedLon = 999.0;
static bool   s_haveLoaded = false;

static inline uint16_t rgb565(uint32_t c) {
    return (uint16_t)((((c >> 19) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) | ((c >> 3) & 0x1F));
}

// Global PNG decoder pointer in PSRAM
static PNG *s_mapPng = nullptr;
static bool ensure_png_decoder() {
    if (s_mapPng) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    s_mapPng = new (mem) PNG();
    return true;
}

// Temporary tile decoding row/line buffer
static uint16_t *s_tilePixels = nullptr;

static int tile_png_line(PNGDRAW *draw) {
    if (!s_tilePixels || draw->y < 0 || draw->y >= 256) return 1;
    const uint8_t *src = draw->pPixels;
    uint16_t *dst = s_tilePixels + (size_t)draw->y * 256;
    const int w = draw->iWidth < 256 ? draw->iWidth : 256;

    if (draw->iPixelType == PNG_PIXEL_INDEXED) {
        auto get_color = [&](uint8_t idx) -> uint16_t {
            if (draw->pFastPalette) return draw->pFastPalette[idx];
            if (draw->pPalette) {
                const uint8_t *p = draw->pPalette + (size_t)idx * 3;
                return (uint16_t)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
            }
            return 0;
        };
        if (draw->iBpp == 4) {
            for (int x = 0; x < w; x += 2) {
                const uint8_t byte = *src++;
                dst[x] = get_color((byte >> 4) & 0x0F);
                if (x + 1 < w) {
                    dst[x + 1] = get_color(byte & 0x0F);
                }
            }
        } else if (draw->iBpp == 8) {
            for (int x = 0; x < w; ++x) {
                dst[x] = get_color(*src++);
            }
        } else if (draw->iBpp == 1) {
            for (int x = 0; x < w; x += 8) {
                const uint8_t byte = *src++;
                for (int b = 0; b < 8 && x + b < w; ++b) {
                    dst[x + b] = get_color((byte >> (7 - b)) & 1);
                }
            }
        }
    } else if (draw->iPixelType == PNG_PIXEL_TRUECOLOR) {
        for (int x = 0; x < w; ++x, src += 3) {
            dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
        }
    } else if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA) {
        for (int x = 0; x < w; ++x, src += 4) {
            dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
        }
    } else if (draw->iPixelType == PNG_PIXEL_GRAYSCALE) {
        for (int x = 0; x < w; ++x, ++src) {
            const uint8_t g = *src;
            dst[x] = (uint16_t)(((g & 0xF8) << 8) | ((g & 0xFC) << 3) | (g >> 3));
        }
    }
    return 1;
}

} // namespace

bool wx_map_fetch::is_cached(double lat, double lon) {
    if (lat < -90.0 || lat > 90.0 || (lat == 0.0 && lon == 0.0)) return false;
#ifdef ARDUINO
    if (!sdcard::mounted()) return false;
    theme_sd::lock();
    File f = SD.open(MAP_SD_PATH, "r");
    if (!f || f.isDirectory()) {
        if (f) f.close();
        theme_sd::unlock();
        return false;
    }
    WeatherMapHeader hdr;
    const size_t rd = f.read((uint8_t *)&hdr, sizeof(hdr));
    const size_t totalSz = f.size();
    f.close();
    theme_sd::unlock();

    if (rd != sizeof(hdr)) return false;
    if (memcmp(hdr.magic, "ORBMAP1", 8) != 0) return false;
    if (hdr.width != WX_RADAR_SIZE || hdr.height != WX_RADAR_SIZE) return false;
    if (totalSz < sizeof(hdr) + (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t)) return false;
    if (fabs(hdr.lat - (float)lat) > 0.005f || fabs(hdr.lon - (float)lon) > 0.005f) return false;
    return true;
#else
    return false;
#endif
}

bool wx_map_fetch::load_cached(double lat, double lon, uint16_t *dst360) {
    if (!dst360) return false;
    if (lat < -90.0 || lat > 90.0 || (lat == 0.0 && lon == 0.0)) return false;
#ifdef ARDUINO
    if (!sdcard::mounted()) return false;
    theme_sd::lock();
    File f = SD.open(MAP_SD_PATH, "r");
    if (!f || f.isDirectory()) {
        if (f) f.close();
        theme_sd::unlock();
        return false;
    }
    WeatherMapHeader hdr;
    if (f.read((uint8_t *)&hdr, sizeof(hdr)) != sizeof(hdr)) {
        f.close();
        theme_sd::unlock();
        return false;
    }
    if (memcmp(hdr.magic, "ORBMAP1", 8) != 0 ||
        hdr.width != WX_RADAR_SIZE || hdr.height != WX_RADAR_SIZE ||
        fabs(hdr.lat - (float)lat) > 0.005f || fabs(hdr.lon - (float)lon) > 0.005f) {
        f.close();
        theme_sd::unlock();
        return false;
    }
    const size_t payloadBytes = (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
    const size_t got = f.read((uint8_t *)dst360, payloadBytes);
    f.close();
    theme_sd::unlock();

    if (got != payloadBytes) {
        Serial.printf("[wxmap] incomplete read from %s: %u of %u bytes\n", MAP_SD_PATH, (unsigned)got, (unsigned)payloadBytes);
        return false;
    }
    Serial.printf("[wxmap] loaded cached map for (%.4f, %.4f) from %s\n", lat, lon, MAP_SD_PATH);
    s_loadedLat = lat;
    s_loadedLon = lon;
    s_haveLoaded = true;
    return true;
#else
    return false;
#endif
}

bool wx_map_fetch::download_and_cache(double lat, double lon, uint16_t *dst360) {
    if (lat < -85.0 || lat > 85.0 || (lat == 0.0 && lon == 0.0)) return false;
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[wxmap] cannot download map: WiFi not connected");
        return false;
    }
#endif

    uint16_t *target = dst360;
    if (!target) {
        target = wx_plate_crop_buffer();
        if (!target) {
            Serial.println("[wxmap] failed to get crop buffer");
            return false;
        }
    }

    if (!ensure_png_decoder()) {
        Serial.println("[wxmap] failed to allocate PNG decoder");
        return false;
    }

    // Allocate temporary 256x256 tile buffer in PSRAM (128 KB)
    s_tilePixels = (uint16_t *)heap_caps_malloc(256 * 256 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_tilePixels) {
        Serial.println("[wxmap] failed to allocate 128KB tile buffer in PSRAM");
        return false;
    }

    // Default fill: background color from theme
    const uint16_t bg = rgb565(theme_style::weather().bg);
    for (size_t i = 0; i < (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE; ++i) {
        target[i] = bg;
    }

    const int zoom = 8;
    const double W = 256.0 * (1 << zoom);  // 65536.0

    const double lat_rad = lat * M_PI / 180.0;
    const double Xc = (lon + 180.0) / 360.0 * W;
    const double Yc = (1.0 - log(tan(lat_rad) + 1.0 / cos(lat_rad)) / M_PI) / 2.0 * W;

    // Scale to fit 50 miles (80.4672 km) into 178 px radius
    const double km_per_screen_px = 80.4672 / 178.0;
    const double km_per_tile_px = (40075.016686 * cos(lat_rad)) / W;
    const double scale = km_per_screen_px / km_per_tile_px;

    const double w_rad = 180.0 * scale;
    int min_tx = (int)floor((Xc - w_rad) / 256.0);
    int max_tx = (int)floor((Xc + w_rad) / 256.0);
    int min_ty = (int)floor((Yc - w_rad) / 256.0);
    int max_ty = (int)floor((Yc + w_rad) / 256.0);

    const int max_tile_idx = (1 << zoom) - 1;
    if (min_ty < 0) min_ty = 0;
    if (max_ty > max_tile_idx) max_ty = max_tile_idx;

    Serial.printf("[wxmap] fetching map tiles for (%.4f, %.4f): tx=[%d..%d] ty=[%d..%d] scale=%.4f\n",
                  lat, lon, min_tx, max_tx, min_ty, max_ty, scale);

    const int c = WX_RADAR_SIZE / 2;
    const int r2 = (c - 2) * (c - 2);   // 178 * 178

    bool anyTileOk = false;
    for (int ty = min_ty; ty <= max_ty; ++ty) {
        for (int tx = min_tx; tx <= max_tx; ++tx) {
            int wrapped_tx = ((tx % (1 << zoom)) + (1 << zoom)) % (1 << zoom);
            char url[160];
            snprintf(url, sizeof(url), "http://basemaps.cartocdn.com/dark_all/%d/%d/%d.png",
                     zoom, wrapped_tx, ty);

            uint8_t *pngData = nullptr;
            size_t pngLen = 0;
#ifdef ARDUINO
            const bool fetched = net_fetch_psram(url, ADSB_USER_AGENT, &pngData, &pngLen, 65536, 3500, 7000);
#else
            const bool fetched = false;
#endif
            if (!fetched || !pngData || pngLen == 0) {
                Serial.printf("[wxmap] tile fetch failed: %s\n", url);
                continue;
            }

            memset(s_tilePixels, 0, 256 * 256 * sizeof(uint16_t));
            if (s_mapPng->openRAM(pngData, (int)pngLen, tile_png_line) == PNG_SUCCESS) {
                s_mapPng->decode(nullptr, PNG_FAST_PALETTE);
                s_mapPng->close();
                anyTileOk = true;

                // Stitch this tile into the 360x360 target
                for (int sy = 0; sy < WX_RADAR_SIZE; ++sy) {
                    const int dy = sy - c;
                    const double wy = Yc + (double)dy * scale;
                    const int tileY = (int)floor(wy / 256.0);
                    if (tileY != ty) continue;
                    const int py = (int)floor(wy - (double)tileY * 256.0);
                    if (py < 0 || py >= 256) continue;

                    for (int sx = 0; sx < WX_RADAR_SIZE; ++sx) {
                        const int dx = sx - c;
                        if (dx * dx + dy * dy > r2) continue;  // outside radar circle

                        const double wx = Xc + (double)dx * scale;
                        const int tileX = (int)floor(wx / 256.0);
                        if (tileX != tx) continue;
                        const int px = (int)floor(wx - (double)tileX * 256.0);
                        if (px < 0 || px >= 256) continue;

                        target[sy * WX_RADAR_SIZE + sx] = s_tilePixels[py * 256 + px];
                    }
                }
            } else {
                Serial.printf("[wxmap] PNG decode failed for tile (%d, %d)\n", tx, ty);
            }
            heap_caps_free(pngData);

#ifdef ARDUINO
            vTaskDelay(pdMS_TO_TICKS(10));
#endif
        }
    }

    heap_caps_free(s_tilePixels);
    s_tilePixels = nullptr;

    if (!anyTileOk) {
        Serial.println("[wxmap] failed to fetch any map tiles");
        return false;
    }

    // Save to SD card cache
#ifdef ARDUINO
    if (sdcard::mounted()) {
        theme_sd::lock();
        if (SD.exists(MAP_SD_PATH)) SD.remove(MAP_SD_PATH);
        File f = SD.open(MAP_SD_PATH, FILE_WRITE);
        if (f) {
            WeatherMapHeader hdr;
            memset(&hdr, 0, sizeof(hdr));
            memcpy(hdr.magic, "ORBMAP1", 8);
            hdr.lat = (float)lat;
            hdr.lon = (float)lon;
            hdr.width = WX_RADAR_SIZE;
            hdr.height = WX_RADAR_SIZE;
            hdr.timestamp = millis();

            f.write((const uint8_t *)&hdr, sizeof(hdr));
            f.write((const uint8_t *)target, (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
            f.close();
            Serial.printf("[wxmap] saved %u KB map to %s on SD card\n",
                          (unsigned)((sizeof(hdr) + (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t)) / 1024),
                          MAP_SD_PATH);
        } else {
            Serial.printf("[wxmap] could not open %s for write\n", MAP_SD_PATH);
        }
        theme_sd::unlock();
    }
#endif

    s_loadedLat = lat;
    s_loadedLon = lon;
    s_haveLoaded = true;
    return true;
}

bool wx_map_fetch::ensure(double lat, double lon) {
    if (lat < -85.0 || lat > 85.0 || (lat == 0.0 && lon == 0.0)) {
        wx_plate_set_have(false);
        return false;
    }

    // 1. If already loaded into s_plateCrop and coordinates match, we're done
    if (s_haveLoaded && fabs(s_loadedLat - lat) < 0.005 && fabs(s_loadedLon - lon) < 0.005 && wx_plate_have()) {
        return true;
    }

    uint16_t *buf = wx_plate_crop_buffer();
    if (!buf) {
        wx_plate_set_have(false);
        return false;
    }

    // 2. Try loading from SD card cache
    if (load_cached(lat, lon, buf)) {
        wx_plate_set_have(true);
        return true;
    }

    // 3. Try downloading and caching over Wi-Fi
#ifdef ARDUINO
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wxmap] downloading new map for (%.4f, %.4f)...\n", lat, lon);
        if (download_and_cache(lat, lon, buf)) {
            wx_plate_set_have(true);
            return true;
        }
    }
#endif

    wx_plate_set_have(false);
    return false;
}

void wx_map_fetch::invalidate() {
    s_haveLoaded = false;
    s_loadedLat = 999.0;
    s_loadedLon = 999.0;
    wx_plate_set_have(false);
}
