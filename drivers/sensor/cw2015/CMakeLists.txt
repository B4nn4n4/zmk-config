zephyr_library()
zephyr_include_directories(.)
zephyr_library_sources_ifdef(CONFIG_CW2015 cw2015.c)
