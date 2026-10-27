/**
 * @file wifi_sta.c
 * @brief Demo WiFi station connection (esp-idf native API)
 *
 * The station is initialized without credentials and joins the network only
 * after wifi_sta_connect() is called: the credentials come from BLE
 * provisioning (ble_netcfg) on the first boot and from KV on later boots.
 *
 * On WIFI_EVENT_STA_DISCONNECTED reconnection is scheduled from a one-shot
 * esp_timer using a backoff array (1s, 3s, 5s, 10s, 15s, 20s) whose index
 * advances per attempt and resets on IP_EVENT_STA_GOT_IP; the event task
 * itself never blocks. Once the IP is ready the port's port_netif reports the
 * link status to core, and core_network.c calls the socket API directly (lwIP).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "wifi_sta.h"

#include "core_api.h"
#include "core_kv.h"
#include "sdkconfig.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <string.h>

#define WIFI_TAG "wifi_sta"

/* KV key of the provisioned credentials (NVS key limit: 15 characters) */
#define WIFI_STA_CFG_KEY "wifi_sta_cfg"

typedef struct {
    char ssid[WIFI_STA_SSID_MAX + 1];
    char passwd[WIFI_STA_PASSWD_MAX + 1];
} wifi_sta_cfg_t;

/* Reconnect backoff (ms): reset once connected; increases step by step within
 * one connection cycle, capped at 20s */
static const uint32_t s_reconnect_delay_ms[] = {1000, 3000, 5000, 10000, 15000, 20000};
#define WIFI_RECONNECT_DELAY_NUM (sizeof(s_reconnect_delay_ms) / sizeof(s_reconnect_delay_ms[0]))

static int s_reconnect_idx = 0;
static esp_timer_handle_t s_reconnect_timer = NULL;
static BOOL_T s_wifi_inited = FALSE;
static BOOL_T s_wifi_started = FALSE;

static void __wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void __wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Event task context: do not block waiting; use a one-shot timer for
         * backoff reconnection */
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        uint32_t delay_ms = s_reconnect_delay_ms[s_reconnect_idx];
        if (s_reconnect_idx < (int)WIFI_RECONNECT_DELAY_NUM - 1) {
            s_reconnect_idx++;
        }
        ESP_LOGW(WIFI_TAG, "disconnected (reason %d), reconnect in %ums", disc ? disc->reason : -1,
                 (unsigned)delay_ms);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(WIFI_TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_reconnect_idx = 0;
    }
}

OPERATE_RET wifi_sta_init(void)
{
    if (TRUE == s_wifi_inited) {
        return OPRT_OK; /* idempotent */
    }

    /* Backoff reconnect timer (one-shot, restarted on every disconnect) */
    esp_timer_create_args_t timer_args = {
        .callback = __wifi_reconnect_timer_cb,
        .name = "wifi_reconn",
    };
    if (ESP_OK != esp_timer_create(&timer_args, &s_reconnect_timer)) {
        ESP_LOGE(WIFI_TAG, "reconnect timer create failed");
        return OPRT_COM_ERROR;
    }

    /* Network interface and event loop (the port's port_netif registers its link
     * listener on top of them later).
     * Neither is idempotent: if already initialized (created earlier by another
     * component) they return ESP_ERR_INVALID_STATE, which is treated as success
     * and execution continues */
    esp_err_t err = esp_netif_init();
    if (ESP_OK != err && ESP_ERR_INVALID_STATE != err) {
        ESP_LOGE(WIFI_TAG, "esp_netif_init failed: %d", err);
        return OPRT_COM_ERROR;
    }
    err = esp_event_loop_create_default();
    if (ESP_OK != err && ESP_ERR_INVALID_STATE != err) {
        ESP_LOGE(WIFI_TAG, "event loop create failed: %d", err);
        return OPRT_COM_ERROR;
    }
    if (NULL == esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(WIFI_TAG, "create default wifi sta failed");
        return OPRT_COM_ERROR;
    }

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (ESP_OK != esp_wifi_init(&wifi_init_cfg)) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_init failed");
        return OPRT_COM_ERROR;
    }

    if (ESP_OK != esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, __wifi_event_handler, NULL) ||
        ESP_OK != esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, __wifi_event_handler, NULL)) {
        ESP_LOGE(WIFI_TAG, "event handler register failed");
        return OPRT_COM_ERROR;
    }

    if (ESP_OK != esp_wifi_set_mode(WIFI_MODE_STA)) {
        ESP_LOGE(WIFI_TAG, "esp_wifi set mode failed");
        return OPRT_COM_ERROR;
    }

    s_wifi_inited = TRUE;
    ESP_LOGI(WIFI_TAG, "station initialized (waiting for credentials)");

    return OPRT_OK;
}

OPERATE_RET wifi_sta_connect(const char *ssid, const char *password)
{
    if (NULL == ssid || '\0' == ssid[0]) {
        ESP_LOGE(WIFI_TAG, "ssid empty");
        return OPRT_INVALID_PARM;
    }
    if (TRUE != s_wifi_inited) {
        ESP_LOGE(WIFI_TAG, "wifi_sta_init() must be called first");
        return OPRT_COM_ERROR;
    }

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", ssid);
    if (password) {
        snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", password);
    }

    if (ESP_OK != esp_wifi_set_config(WIFI_IF_STA, &wifi_config)) {
        ESP_LOGE(WIFI_TAG, "esp_wifi set config failed");
        return OPRT_COM_ERROR;
    }

    if (TRUE != s_wifi_started) {
        /* After start: WIFI_EVENT_STA_START -> esp_wifi_connect() (see the event handler) */
        if (ESP_OK != esp_wifi_start()) {
            ESP_LOGE(WIFI_TAG, "esp_wifi_start failed");
            return OPRT_COM_ERROR;
        }
        s_wifi_started = TRUE;
    } else {
        /* Re-provisioned: drop the current connection; the DISCONNECTED event
         * schedules a reconnect with the configuration set above */
        esp_wifi_disconnect();
    }

    ESP_LOGI(WIFI_TAG, "connecting to \"%s\"...", ssid);

    return OPRT_OK;
}

OPERATE_RET wifi_sta_save_cfg(const char *ssid, const char *password)
{
    wifi_sta_cfg_t cfg = {0};

    if (NULL == ssid || '\0' == ssid[0]) {
        return OPRT_INVALID_PARM;
    }

    snprintf(cfg.ssid, sizeof(cfg.ssid), "%s", ssid);
    if (password) {
        snprintf(cfg.passwd, sizeof(cfg.passwd), "%s", password);
    }

    return core_kv_set(WIFI_STA_CFG_KEY, (const uint8_t *)&cfg, sizeof(cfg));
}

OPERATE_RET wifi_sta_load_cfg(char *ssid, size_t ssid_size, char *password, size_t passwd_size)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    wifi_sta_cfg_t cfg;
    int rt;

    if (NULL == ssid || NULL == password || 0 == ssid_size || 0 == passwd_size) {
        return OPRT_INVALID_PARM;
    }

    rt = core_kv_get(WIFI_STA_CFG_KEY, &buf, &len);
    if (OPRT_OK != rt) {
        ESP_LOGW(WIFI_TAG, "no saved WiFi credentials (kv get: %d)", rt);
        return OPRT_NOT_FOUND;
    }
    if (NULL == buf || len != sizeof(cfg)) {
        ESP_LOGE(WIFI_TAG, "saved WiFi credentials invalid (len %u, expected %u)", (unsigned)len,
                 (unsigned)sizeof(cfg));
        if (buf) {
            core_free(buf);
        }
        return OPRT_COM_ERROR;
    }

    memcpy(&cfg, buf, sizeof(cfg));
    core_free(buf);

    cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    cfg.passwd[sizeof(cfg.passwd) - 1] = '\0';
    if ('\0' == cfg.ssid[0]) {
        return OPRT_NOT_FOUND;
    }

    snprintf(ssid, ssid_size, "%s", cfg.ssid);
    snprintf(password, passwd_size, "%s", cfg.passwd);

    return OPRT_OK;
}

OPERATE_RET wifi_sta_clear_cfg(void)
{
    core_kv_del(WIFI_STA_CFG_KEY);
    return OPRT_OK;
}
