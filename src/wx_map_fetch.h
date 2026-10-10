#pragma once

#include <stdint.h>
#include <stdbool.h>

namespace wx_map_fetch {

// Checks whether /weather_map.raw exists on the SD card and matches (lat, lon).
bool is_cached(double lat, double lon);

// Loads the cached 360x360 RGB565 map from SD card into dst360.
// Returns true on success, false if missing or mismatched.
bool load_cached(double lat, double lon, uint16_t *dst360);

// Fetches the map tiles (Carto Dark Matter) over HTTP, stitches them to 360x360 at 50-mile scale,
// writes the result to dst360 (if non-null) and caches to /weather_map.raw on SD card.
// Returns true on success.
bool download_and_cache(double lat, double lon, uint16_t *dst360);

// Ensures the map plate in wx_radar_client is populated for (lat, lon):
// - If already loaded in memory for (lat, lon), returns true.
// - If cached on SD, loads it into s_plateCrop and returns true.
// - If not cached and Wi-Fi is connected, downloads, caches to SD, loads into s_plateCrop.
bool ensure(double lat, double lon);

// Invalidates in-memory tag so the next call to ensure() checks the cache or re-fetches.
void invalidate();

}  // namespace wx_map_fetch
