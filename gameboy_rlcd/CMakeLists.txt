cmake_minimum_required(VERSION 3.16)

# components/ holds the vendored codec_board + esp_codec_dev ports of
# Waveshare's and Espressif's audio components; ESP-IDF picks that directory up
# automatically.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gameboy_rlcd)
