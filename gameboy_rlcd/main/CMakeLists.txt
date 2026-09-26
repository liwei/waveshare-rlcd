idf_component_register(
    SRCS
        "main.c"
        "st7305.c"
        "fb.c"
        "ui.c"
        "input.c"
        "input_bt.c"
        "hid_gamepad.c"
        "esp_hid_gap.c"
        "buttons.c"
        "battery.c"
        "storage_sd.c"
        "gbemu.c"
        "audio.c"
        "audio_i2s.c"
        "profiler.c"
        "console.c"
        "minigb_apu/minigb_apu.c"
    INCLUDE_DIRS "." "crankboy_core" "minigb_apu"
    REQUIRES esp_driver_gpio esp_driver_spi esp_driver_sdmmc esp_driver_usb_serial_jtag
             esp_timer esp_adc sdmmc fatfs driver nvs_flash codec_board esp_codec_dev
             esp_hid bt app_update esp_partition
    # 16x16 CJK glyph subset for non-ASCII ROM filenames; mapped from flash, so
    # it costs no RAM. Regenerate with tools/mk_cjkfont.py.
    EMBED_FILES "cjkfont.bin"
)

# 16-bit signed samples, so minigb_apu matches what the codec is fed.
target_compile_definitions(${COMPONENT_LIB} PRIVATE MINIGB_APU_AUDIO_FORMAT_S16SYS)

# The CrankBoy core is embedded through gbemu.c as a single translation unit;
# keep it heavily optimised, as PaperBoy does upstream.
set_source_files_properties(gbemu.c PROPERTIES COMPILE_FLAGS "-O3 -Wno-error=unused-label")
