cmake_minimum_required(VERSION 3.16)

# The ROM manager app: a second image, built separately from the emulator and
# flashed into its own partition. See main/manager_main.c for why it is separate.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gameboy_manager)
