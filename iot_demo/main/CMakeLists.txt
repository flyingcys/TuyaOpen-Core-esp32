# demo sources
set(_demo_srcs
    "main.c"
    "tuya_main.c"
    "wifi_sta.c"
    "reset_netcfg.c"
    "ota_app.c"
)

idf_component_register(
    SRCS ${_demo_srcs}
    INCLUDE_DIRS "."
    REQUIRES nvs_flash tuyaopen-core esp-idf esp_wifi esp_netif esp_event esp_timer
)
unset(_demo_srcs)

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
