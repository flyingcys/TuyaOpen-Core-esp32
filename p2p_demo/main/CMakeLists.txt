# demo sources
set(_demo_srcs
    "main.c"
    "tuya_main.c"
    "tuya_ipc_demo.c"
    "demo_vcam.c"
    "demo_media_event.c"
    "wifi_sta.c"
    "reset_netcfg.c"
    "ota_app.c"
)

idf_component_register(
    SRCS ${_demo_srcs}
    INCLUDE_DIRS "."
    EMBED_FILES "demo_video.264" "demo_video_150.264" "demo_video_100.264"
    REQUIRES nvs_flash tuyaopen-core esp-idf esp_wifi esp_netif esp_event esp_timer
)
unset(_demo_srcs)

# Virtual camera / P2P AV parameters. The embedded ladder is 320x240@10,
# 250/150/100 kbps, GOP 10; the 250 kbps entry is the base.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    P2P_MEM_USE_PSRAM=1
    P2P_DEMO_WIDTH=320
    P2P_DEMO_HEIGHT=240
    P2P_DEMO_FPS=10
    P2P_DEMO_KBPS=250
    P2P_DEMO_GOP=10
)

if(DEFINED DEMO_VCAM_FORCE_SWITCH_TEST AND DEMO_VCAM_FORCE_SWITCH_TEST)
    # Bring-up only: force 250 -> 150 (10 s) -> 100 (20 s) to verify that the
    # embedded ladder switches without a decoder reset or black screen.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEMO_VCAM_FORCE_SWITCH_TEST=1)
endif()

if(DEFINED CONFIG_PROJECT_VERSION)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        PROJECT_VERSION="${CONFIG_PROJECT_VERSION}")
endif()

# Switch macros and TKL headers (injected via tuyaopen-headers; the app's own
# sources need them too)
target_link_libraries(${COMPONENT_LIB} PRIVATE tuyaopen-headers)

# Under CONFIG_MBEDTLS_THREADING_ALT, esp-idf's own mbedtls component also needs
# threading_alt.h when it compiles -- use the ROM version shipped with esp-idf
# (FreeRTOS mapping, self-contained).
# The Tuya side (tuya_tls.c) uses the core_mutex version through core's
# tuya_transport/config/threading_alt.h (the two are structurally identical and
# each belongs to its own allocator; mbedtls only operates the mutex through the
# set_alt callback, so they do not conflict).
foreach(_mbed_tgt mbedtls mbedcrypto mbedx509)
    if(TARGET ${_mbed_tgt})
        target_include_directories(${_mbed_tgt} PUBLIC
            $ENV{IDF_PATH}/components/mbedtls/port/mbedtls_rom)
    endif()
endforeach()
unset(_mbed_tgt)
