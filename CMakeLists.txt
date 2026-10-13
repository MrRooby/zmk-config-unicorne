# Sources go into ZMK's `app` target so they can include <zmk/...> headers.
if(CONFIG_ZMK_KEYSTATS)
  target_sources(app PRIVATE src/key_stats.c)
  target_sources_ifdef(CONFIG_ZMK_KEYSTATS_BLE app PRIVATE src/key_stats_ble.c)
endif()
