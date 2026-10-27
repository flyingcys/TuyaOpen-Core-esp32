/**
 * @file main.c
 * @brief iot_demo application entry (ESP32 / esp-idf)
 *
 * app_main initializes NVS (erasing and retrying when NVS has no free pages
 * or a new version is found) and then calls tuya_app_main() (tuya_main.c),
 * which creates a dedicated thread running user_main().
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "esp_err.h"
#include "nvs_flash.h"

/* Tuya application main thread (defined in tuya_main.c) */
extern void tuya_app_main(void);

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    tuya_app_main();
}
