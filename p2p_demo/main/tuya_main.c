/**
 * @file tuya_main.c
 * @brief Tuya device application flow for iot_demo (ESP32): callbacks, OTA
 *        registration, network hookup and the application main thread
 *
 * user_main() initializes the services used by the demo (cJSON allocator
 * hooks, time service, log output, KV storage, software timers, work queue),
 * initializes the WiFi station without credentials and reconnects with the
 * credentials saved by a previous BLE provisioning, registers the OTA
 * callbacks with core_ota_register_cbs(), initializes tuya_iot and the BLE
 * stack, registers the BLE provisioning token-get port and then runs the
 * tuya_iot_yield() loop. An unbound device receives its WiFi credentials and
 * the binding token from the Tuya APP over BLE; there is no compiled-in
 * SSID/password. The link status reported to core is maintained by
 * __user_link_status_cb(), which port_netif invokes on registration and on
 * every WiFi link event; the callback also keeps the active IP (used for
 * socket source binding and LAN broadcasts) in sync.
 *
 * Event handling covers OTA notification logging, reset handling (which also
 * drops the provisioned WiFi credentials) and DP receive/report for both
 * object and raw DPs. On ESP32 tuya_app_main() creates the dedicated thread
 * that runs user_main(); on a Linux build main() calls user_main() directly.
 *
 * @copyright Copyright (c) 2021-2025 Tuya Inc. All Rights Reserved.
 */

#include "cJSON.h"
#include "sdkconfig.h"
#include "core_api.h"
#include "core_event.h"
#include "core_event_info.h"
#include "core_network.h"
#include "core_netif.h"
#include "core_api_ops.h"
#include "tuya_config.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "tuya_lan.h"
#include "ble_mgr.h"
#include "ble_netcfg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "reset_netcfg.h"
#include "wifi_sta.h"
#include "ota_app.h"

/* This demo provisions WiFi over BLE only (no compiled-in SSID/password), so
 * the BLE stack is a hard requirement. */
#if !defined(ENABLE_BLUETOOTH) || (ENABLE_BLUETOOTH == 0)
#error "iot_demo provisions WiFi over BLE: enable CONFIG_ENABLE_BLUETOOTH (NimBLE) in menuconfig"
#endif

#ifndef PROJECT_VERSION
#define PROJECT_VERSION "1.0.0"
#endif

/* Tuya device handle */
tuya_iot_client_t client;

/* Tuya license information (uuid authkey) */
tuya_iot_license_t license;

/**
 * @brief user defined log output api, in this demo, it will use uart0 as log-tx
 *
 * @param str log string
 * @return void
 */
void user_log_output_cb(const char *str)
{
    CORE_API_OPS_T *ops = core_api_get_ops();
    if (NULL != ops && ops->log_output) {
        ops->log_output(str);
    }
}

/**
 * @brief user defined upgrade notify callback, it will notify device a OTA
 * request received
 *
 * @param client device info
 * @param upgrade the upgrade request info
 * @return void
 */
void user_upgrade_notify_on(tuya_iot_client_t *client, cJSON *upgrade)
{
    PR_INFO("----- Upgrade information -----");
    if (!upgrade) {
        PR_WARN("upgrade JSON is NULL");
        return;
    }

    cJSON *type_item    = cJSON_GetObjectItem(upgrade, "type");
    cJSON *version_item = cJSON_GetObjectItem(upgrade, "version");
    cJSON *size_item    = cJSON_GetObjectItem(upgrade, "size");
    cJSON *md5_item     = cJSON_GetObjectItem(upgrade, "md5");
    cJSON *hmac_item    = cJSON_GetObjectItem(upgrade, "hmac");
    cJSON *url_item     = cJSON_GetObjectItem(upgrade, "url");
    cJSON *https_item   = cJSON_GetObjectItem(upgrade, "httpsUrl");

    PR_INFO("OTA Channel: %d", cJSON_IsNumber(type_item) ? type_item->valueint : -1);
    PR_INFO("Version: %s", cJSON_IsString(version_item) ? version_item->valuestring : "N/A");
    PR_INFO("Size: %s", cJSON_IsString(size_item) ? size_item->valuestring : "N/A");
    PR_INFO("MD5: %s", cJSON_IsString(md5_item) ? md5_item->valuestring : "N/A");
    PR_INFO("HMAC: %s", cJSON_IsString(hmac_item) ? hmac_item->valuestring : "N/A");
    PR_INFO("URL: %s", cJSON_IsString(url_item) ? url_item->valuestring : "N/A");
    PR_INFO("HTTPS URL: %s", cJSON_IsString(https_item) ? https_item->valuestring : "N/A");
}

/**
 * @brief user defined event handler
 *
 * @param client device info
 * @param event the event info
 * @return void
 */
void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));
    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_INFO("Device Bind Start! Waiting for the Tuya APP to provision over BLE...");
        break;

    /* Cloud link is up: provisioning is done, drop the BLE provisioning
     * channel (the BLE stack stays up for BLE DP control) */
    case TUYA_EVENT_MQTT_CONNECTED:
        ble_netcfg_stop();
        break;
    /* RECV upgrade request */
    case TUYA_EVENT_UPGRADE_NOTIFY:
        user_upgrade_notify_on(client, event->value.asJSON);
        break;
    case TUYA_EVENT_RESET: {
        tuya_reset_type_t reset_type = (tuya_reset_type_t)event->value.asInteger;
        PR_INFO("Device Reset:%d", reset_type);

        /* Factory reset: forget the provisioned credentials; the next boot
         * starts BLE provisioning again */
        wifi_sta_clear_cfg();

        // TUYA_RESET_TYPE_FACTORY, TUYA_RESET_TYPE_REMOTE_FACTORY, TUYA_RESET_TYPE_DATA_FACTORY
        // Need remove the device application data from the kv store
    } break;
    case TUYA_EVENT_RESET_COMPLETE: {
        PR_INFO("Device Reset Complete!");

        // Restart the device
        core_system_reset();
    } break;
    /* RECV OBJ DP */
    case TUYA_EVENT_DP_RECEIVE_OBJ: {
        dp_obj_recv_t *dpobj = event->value.dpobj;
        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d CNT:%u", dpobj->cmd_tp, dpobj->dtt_tp, dpobj->dpscnt);
        if (dpobj->devid != NULL) {
            PR_DEBUG("devid.%s", dpobj->devid);
        }

        uint32_t index = 0;
        for (index = 0; index < dpobj->dpscnt; index++) {
            dp_obj_t *dp = dpobj->dps + index;
            PR_DEBUG("idx:%d dpid:%d type:%d ts:%u", index, dp->id, dp->type, dp->time_stamp);
            switch (dp->type) {
            case PROP_BOOL: {
                PR_DEBUG("bool value:%d", dp->value.dp_bool);
                break;
            }
            case PROP_VALUE: {
                PR_DEBUG("int value:%d", dp->value.dp_value);
                break;
            }
            case PROP_STR: {
                PR_DEBUG("str value:%s", dp->value.dp_str);
                break;
            }
            case PROP_ENUM: {
                PR_DEBUG("enum value:%u", dp->value.dp_enum);
                break;
            }
            case PROP_BITMAP: {
                PR_DEBUG("bits value:0x%X", dp->value.dp_bitmap);
                break;
            }
            default: {
                PR_ERR("idx:%d dpid:%d type:%d ts:%u is invalid", index, dp->id, dp->type, dp->time_stamp);
                break;
            }
            } // end of switch
        }

        tuya_iot_dp_obj_report(client, dpobj->devid, dpobj->dps, dpobj->dpscnt, 0);

    } break;

    /* RECV RAW DP */
    case TUYA_EVENT_DP_RECEIVE_RAW: {
        dp_raw_recv_t *dpraw = event->value.dpraw;
        PR_DEBUG("SOC Rev DP Cmd t1:%d t2:%d", dpraw->cmd_tp, dpraw->dtt_tp);
        if (dpraw->devid != NULL) {
            PR_DEBUG("devid.%s", dpraw->devid);
        }

        uint32_t  index = 0;
        dp_raw_t *dp    = &dpraw->dp;
        PR_DEBUG("dpid:%d type:RAW len:%d data:", dp->id, dp->len);
        for (index = 0; index < dp->len; index++) {
            PR_DEBUG_RAW("%02x", dp->data[index]);
        }

        tuya_iot_dp_raw_report(client, dpraw->devid, &dpraw->dp, 3);

    } break;

        /* TBD.. add other event if necessary */

    default:
        break;
    }
}

/* Link status: maintained by the port layer's (port_netif) status callback and
 * polled by core through user_network_check */
static volatile bool s_link_up = false;

/**
 * @brief port link status callback: port_netif reports the current status
 * immediately after registration, and calls back again whenever it changes,
 * driven by WiFi events (GOT_IP/DISCONNECTED)
 *
 * @param stat current link status
 * @return void
 */
static void __user_link_status_cb(CORE_NETIF_LINK_STAT_E stat)
{
    s_link_up = (CORE_NETIF_LINK_UP == stat);

    /* Keep the active IP in sync: both the source-address binding of outbound
     * sockets and the local IP in LAN broadcast packets come from this value;
     * 0 means no binding */
    TUYA_IP_ADDR_T addr = 0;
    if (s_link_up) {
        NW_IP_S ip = {0};
        if (OPRT_OK == core_netif_get_ip(&ip) && ip.ip[0] != '\0') {
            addr = core_net_str2addr(ip.ip);
        }
    }
    core_network_set_active_ip(addr);

    PR_NOTICE("link status changed to %d, link %s", stat, s_link_up ? "UP" : "DOWN");
}

/**
 * @brief user defined network check callback, it will check the network every
 * 1sec by reading the link status maintained by the port layer
 *
 * @return true
 * @return false
 */
bool user_network_check(void)
{
    return s_link_up;
}

/* ---------------------------------------------------------------------------
 * OTA callback registration: the application layer (this demo) injects the
 * callbacks into the core via core_ota_register_cbs
 * --------------------------------------------------------------------------- */
static const CORE_OTA_CBS_T s_ota_cbs = {
    .start_notify = ota_start_notify,
    .data_process = ota_data_process,
    .end_notify = ota_end_notify,
};

/* ---------------------------------------------------------------------------
 * BLE provisioning (the only WiFi provisioning path of this demo)
 *
 * The Tuya APP connects over BLE and sends ssid/password plus the binding
 * token. The BLE netcfg finish callback persists the credentials, joins the
 * network and hands the token to core through EVENT_LINK_ACTIVATE, which is
 * what releases core's token-get semaphore. The BLE stack stays up after
 * provisioning so BLE DP control keeps working when the cloud is unreachable.
 * --------------------------------------------------------------------------- */

/**
 * @brief Bring up the Tuya BLE stack (idempotent: tuya_ble_init keeps the
 *        existing manager). It serves both BLE provisioning and BLE DP control.
 */
static void __ble_stack_start(void)
{
    tuya_ble_cfg_t cfg = {0};

    cfg.client = &client;
    snprintf(cfg.device_name, sizeof(cfg.device_name), "TYBLE");

    if (OPRT_OK != tuya_ble_init(&cfg)) {
        PR_ERR("BLE stack init failed; BLE provisioning unavailable");
    }
}

/**
 * @brief BLE netcfg finish callback (runs in the BLE work-queue thread)
 *
 * Persists the credentials, stops the provisioning channel, joins the network
 * and publishes the binding token. core_event_publish() dispatches
 * synchronously, so the on-stack binding is safe.
 *
 * @param info ssid/passwd/token delivered by the APP
 * @return OPRT_OK on success. Others on error
 */
static int __ble_netcfg_finish(ble_netcfg_info_t *info)
{
    tuya_binding_info_t binding;

    if (NULL == info || 0 == info->s_len ||
        info->t_len < (REGION_LEN + TOKEN_LEN + REGIST_KEY_LEN)) {
        PR_ERR("BLE netcfg: invalid payload");
        return OPRT_INVALID_PARM;
    }

    PR_NOTICE("BLE provisioning: ssid \"%s\"", (const char *)info->ssid);

    /* Remember the credentials: the next boot reconnects without BLE */
    if (OPRT_OK != wifi_sta_save_cfg((const char *)info->ssid, (const char *)info->passwd)) {
        PR_ERR("BLE netcfg: failed to save the WiFi credentials");
    }

    ble_netcfg_stop();

    /* Join the network; core waits for link-up before continuing */
    wifi_sta_connect((const char *)info->ssid, (const char *)info->passwd);

    /* Binding token layout matches the upstream netmgr: region | token | regist_key */
    memset(&binding, 0, sizeof(binding));
    memcpy(binding.region, info->token, REGION_LEN);
    memcpy(binding.token, info->token + REGION_LEN, TOKEN_LEN);
    memcpy(binding.regist_key, info->token + REGION_LEN + TOKEN_LEN, REGIST_KEY_LEN);

    return core_event_publish(EVENT_LINK_ACTIVATE, &binding);
}

/**
 * @brief Token-get port: an unbound device gets its token from the APP over BLE
 *
 * core calls this from STATE_TOKEN_PENDING and then blocks on its token
 * semaphore until __ble_netcfg_finish() publishes EVENT_LINK_ACTIVATE.
 *
 * @param config device configuration (unused)
 * @return OPRT_OK when BLE provisioning was started
 */
static int __ble_token_get(tuya_iot_config_t *config)
{
    (void)config;

    /* Retry the stack bring-up in case the early init failed */
    __ble_stack_start();

    if (OPRT_OK != ble_netcfg_start(__ble_netcfg_finish)) {
        PR_ERR("BLE netcfg start failed");
        return OPRT_COM_ERROR;
    }

    return OPRT_OK;
}

void user_main(void)
{
    int rt = OPRT_OK;

    //! open iot development kit runtim init
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = core_malloc, .free_fn = core_free});
    /* Before the log: every log line timestamps itself through the time
     * service, whose mutex this creates. */
    core_time_service_init();
    core_log_init(CORE_LOG_LEVEL_DEBUG, 1024, user_log_output_cb);

    PR_NOTICE("Application information:");
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);

    core_kv_init(&(core_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    core_sw_timer_init();
    core_workq_init();

    /* WiFi: bring up the station (event loop + netif + driver) without
     * credentials. The SSID/password are never compiled in: they come from the
     * previous BLE provisioning (KV) or from the APP over BLE in this boot.
     * Failure does not block further initialization (while the link is down,
     * core's network_check keeps the cloud connection blocked) */
    if (OPRT_OK != wifi_sta_init()) {
        PR_ERR("wifi station init failed");
    } else {
        char ssid[WIFI_STA_SSID_MAX + 1] = {0};
        char passwd[WIFI_STA_PASSWD_MAX + 1] = {0};

        if (OPRT_OK == wifi_sta_load_cfg(ssid, sizeof(ssid), passwd, sizeof(passwd))) {
            /* Already provisioned: reconnect without waiting for the APP */
            wifi_sta_connect(ssid, passwd);
        } else {
            PR_NOTICE("no provisioned WiFi credentials, waiting for BLE provisioning");
        }
    }

    core_ota_register_cbs(&s_ota_cbs);

    reset_netconfig_start();

    /* Use the compile-time credentials directly (overridden by tuya_config.h or the secrets header) */
    license.uuid    = TUYA_OPENSDK_UUID;
    license.authkey = TUYA_OPENSDK_AUTHKEY;
    // PR_DEBUG("uuid %s, authkey %s", license.uuid, license.authkey);
    /* Initialize Tuya device configuration */
    rt = tuya_iot_init(&client, &(const tuya_iot_config_t){
                                    .software_ver  = PROJECT_VERSION,
                                    .productkey    = TUYA_PRODUCT_ID,
                                    .uuid          = license.uuid,
                                    .authkey       = license.authkey,
                                    .event_handler = user_event_handler_on,
                                    .network_check = user_network_check,
                                });
    assert(rt == OPRT_OK);

    /* BLE stack: serves provisioning (unbound device) and BLE DP control.
     * Must run after tuya_iot_init (it reads the client configuration). */
    __ble_stack_start();

    /* Link status hookup: the port (port_netif) reports its current status
     * immediately and updates it on WiFi events */
    core_netif_set_link_cb(__user_link_status_cb);

    /* Token acquisition hookup: the APP delivers the binding token over BLE;
     * core blocks in STATE_TOKEN_PENDING until __ble_netcfg_finish() publishes
     * EVENT_LINK_ACTIVATE. */
    tuya_iot_token_get_port_register(&client, __ble_token_get);

    PR_DEBUG("tuya_iot_init success");
    /* Start tuya iot task */
    tuya_iot_start(&client);

    reset_netconfig_check();

    for (;;) {
        /* Loop to receive packets, and handles client keepalive */
        tuya_iot_yield(&client);
    }
}

/**
 * @brief main
 *
 * @param argc
 * @param argv
 * @return void
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
static void tuya_app_thread(void *arg)
{
    user_main();

    core_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

/**
 * @brief ESP32 entry point (called by app_main in main.c): creates a dedicated
 * thread to run user_main, avoiding the main task's default stack
 * (CONFIG_ESP_MAIN_TASK_STACK_SIZE)
 *
 * The stack size is 8KB: user_main runs the tuya_iot_yield state machine on
 * the same thread, including the MQTT loop (tuya_mqtt_loop); the TLS handshake
 * and packet decryption during connection setup also run on this stack -- an
 * mbedtls handshake peaks at several KB, so 4KB is too tight (esp-idf's
 * xTaskCreate counts the stack in bytes; the port adds another 1KB for s3).
 */
void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth   = 1024 * 8;
    thrd_param.priority     = THREAD_PRIO_1;
    thrd_param.thrdname     = "tuya_app_main";
    core_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
