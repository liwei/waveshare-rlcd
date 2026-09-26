# The manager reuses the board drivers, the storage layer and the page from the
# emulator's tree rather than duplicating them: single files, referenced by path.
# What it does not take is anything to do with emulation - no core, no audio, no
# Bluetooth - which is the whole point of it being a separate application.
idf_component_register(
    SRCS
        "manager_main.c"
        "web.c"
        "rom_files.c"
        "../../main/storage_sd.c"
        "../../main/st7305.c"
        "../../main/fb.c"
        "../../main/buttons.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_sdmmc esp_driver_usb_serial_jtag
             esp_timer sdmmc fatfs nvs_flash app_update esp_partition esp_wifi esp_netif esp_http_server esp_event
    # The page, and the same CJK glyph subset the emulator embeds, since fb.c
    # renders non-ASCII ROM names with it.
    EMBED_FILES "rommanager.html" "../../main/cjkfont.bin"
)
