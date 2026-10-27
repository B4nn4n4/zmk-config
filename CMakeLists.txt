zephyr_include_directories(include)

add_subdirectory(drivers)

target_sources_ifdef(CONFIG_ZMK_BACKLIGHT_MAP app PRIVATE src/backlight.c)
target_sources_ifdef(CONFIG_ZMK_BACKLIGHT_MAP app PRIVATE src/behaviors/behavior_backlight.c)
target_sources(app PRIVATE src/behaviors/behavior_bootloader.c)
target_sources_ifdef(CONFIG_ZMK_BATTERY_INDICATOR app PRIVATE src/battery_indicator.c)
target_sources_ifdef(CONFIG_ZMK_BATTERY_INDICATOR app PRIVATE src/behaviors/behavior_battery_indicator.c)
target_sources_ifdef(CONFIG_ZMK_BATTERY_INDICATOR app PRIVATE src/behaviors/behavior_layer_color.c)
target_sources_ifdef(CONFIG_ZMK_BOOT_MAGIC_KEY app PRIVATE src/boot_magic_key.c)
target_sources_ifdef(CONFIG_ZMK_CHARGE_STATE_REPORTING app PRIVATE src/charge_state.c)
# Input-split references this virtual device even when BLE telemetry is disabled.
target_sources_ifdef(CONFIG_DT_HAS_ZMK_SPLIT_BATTERY_SOURCE_ENABLED app PRIVATE src/split_battery.c)
target_sources_ifdef(CONFIG_ZMK_SPLIT_BATTERY_TELEMETRY app PRIVATE src/behaviors/behavior_split_battery.c)
target_sources_ifdef(CONFIG_ZMK_USB_BATTERY_REPORTING app PRIVATE src/usb_battery.c)
