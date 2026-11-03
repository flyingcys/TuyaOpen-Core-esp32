cmake_minimum_required(VERSION 3.16)

# Local development: components/tuyaopen-core and components/esp-idf are
# relative symlinks to the TuyaOpen-Core checkout next to this repository.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

# ESP32 uses ESP-IDF's lwIP and pthread implementation. Select the libre
# platform port explicitly; p2p-core/port/re/re.cmake has no platform branch.
set(RE_PLATFORM_CMAKE
    "${CMAKE_CURRENT_LIST_DIR}/components/tuyaopen-core/p2p-core/port/re/platform/esp-idf.cmake"
    CACHE FILEPATH "libre ICE platform port" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p2p_demo)
