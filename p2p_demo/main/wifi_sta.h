/**
 * @file wifi_sta.h
 * @brief Demo WiFi station connection (esp-idf native API)
 *
 * The station is initialized without credentials: the demo has no compiled-in
 * SSID/password. The credentials are provisioned by the APP over BLE
 * (ble_netcfg) and persisted in KV, so the next boot reconnects directly.
 *
 * This module initializes the esp-idf station: default event loop, default
 * station netif, WiFi driver and station configuration. wifi_sta_connect()
 * applies the credentials and starts/joins the network. On
 * WIFI_EVENT_STA_DISCONNECTED it schedules reconnection from a one-shot
 * esp_timer using a backoff array (1s, 3s, 5s, 10s, 15s, 20s) whose index
 * advances per attempt and resets on IP_EVENT_STA_GOT_IP; the event task
 * itself never blocks. Once the IP is ready the port's port_netif reports the
 * link status to core, and core_network.c calls the socket API directly (lwIP).
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */
#ifndef __WIFI_STA_H__
#define __WIFI_STA_H__

#include <stddef.h>

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds of the provisioned credentials (match the BLE netcfg contract) */
#define WIFI_STA_SSID_MAX   32
#define WIFI_STA_PASSWD_MAX 64

/**
 * @brief Initialize the WiFi station (event loop, netif, driver, handlers)
 *
 * No credentials are applied and no connection is started here; call
 * wifi_sta_connect() once the credentials are available (from BLE provisioning
 * or from the KV store). Idempotent.
 *
 * @return OPRT_OK on success. Others on error
 */
OPERATE_RET wifi_sta_init(void);

/**
 * @brief Apply credentials and join the network
 *
 * On the first call this sets the station configuration and starts the driver
 * (WIFI_EVENT_STA_START triggers the first connect). When the driver is
 * already running the station is disconnected, which makes the event handler
 * reconnect with the new configuration. Returning from this function does not
 * mean the connection is established (it completes asynchronously).
 *
 * @param ssid     WiFi SSID (non-empty)
 * @param password WiFi password (empty string for open networks)
 * @return OPRT_OK on success. Others on error
 */
OPERATE_RET wifi_sta_connect(const char *ssid, const char *password);

/**
 * @brief Persist the provisioned credentials in KV
 *
 * @param ssid     WiFi SSID (non-empty)
 * @param password WiFi password (empty string for open networks)
 * @return OPRT_OK on success. Others on error
 */
OPERATE_RET wifi_sta_save_cfg(const char *ssid, const char *password);

/**
 * @brief Load the persisted credentials
 *
 * @param[out] ssid        buffer for the SSID
 * @param[in]  ssid_size   buffer size (>= WIFI_STA_SSID_MAX + 1)
 * @param[out] password    buffer for the password
 * @param[in]  passwd_size buffer size (>= WIFI_STA_PASSWD_MAX + 1)
 * @return OPRT_OK when stored credentials were found, OPRT_NOT_FOUND otherwise
 */
OPERATE_RET wifi_sta_load_cfg(char *ssid, size_t ssid_size, char *password, size_t passwd_size);

/**
 * @brief Remove the persisted credentials (factory reset)
 *
 * @return OPRT_OK
 */
OPERATE_RET wifi_sta_clear_cfg(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_STA_H__ */
