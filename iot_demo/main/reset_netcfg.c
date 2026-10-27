/**
 * @file reset_netcfg.c
 * @brief Reset-count based network configuration clearing: reset counter,
 *        reset event handling, and network configuration clearing
 *
 * The reset counter is a single byte stored in the KV store under "rst_cnt".
 * reset_netconfig_start() increments it and arms a 5-second one-shot software
 * timer that writes the counter back to 0; reset_netconfig_check() compares
 * the counter with RESET_NETCNT_MAX (3) and, once reached, subscribes
 * __reset_netconfig_clear to EVENT_RESET and calls tuya_iot_reset(). The
 * subscription clears the counter when the reset completes.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include "core_api.h"
#include "tuya_iot.h"

/***********************************************************
************************macro define************************
***********************************************************/
#define RESET_NETCNT_NAME "rst_cnt"
#define RESET_NETCNT_MAX  3

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/

/***********************************************************
***********************variable define**********************
***********************************************************/

/***********************************************************
***********************function define**********************
***********************************************************/

static int reset_count_read(uint8_t *count)
{
    int rt = OPRT_OK;

    uint8_t *read_buf = NULL;
    size_t read_len;

    TUYA_CALL_ERR_RETURN(core_kv_get(RESET_NETCNT_NAME, &read_buf, &read_len));
    *count = read_buf[0];

    PR_DEBUG("reset count is %d", *count);

    if (NULL != read_buf) {
        core_free(read_buf);
        read_buf = NULL;
    }

    return rt;
}

static int reset_count_write(uint8_t count)
{
    PR_DEBUG("reset count write %d", count);
    return core_kv_set(RESET_NETCNT_NAME, &count, 1);
}

static void reset_netconfig_timer(TIMER_ID timer_id, void *arg)
{
    reset_count_write(0);
    PR_DEBUG("reset cnt clear!");
}

static OPERATE_RET __reset_netconfig_clear(void *data)
{
    reset_count_write(0);
    PR_DEBUG("reset cnt clear by reset event!");
    return OPRT_OK;
}

/**
 * @brief Read the reset counter and clear the network configuration once it
 *        reached RESET_NETCNT_MAX
 *
 * When the counter is < 3 the function returns OPRT_OK immediately. Otherwise
 * it subscribes __reset_netconfig_clear to EVENT_RESET (the subscriber clears
 * the counter when the reset event is delivered) and calls tuya_iot_reset()
 * to erase the network configuration data.
 *
 * @return int Returns 0 on success, or a negative value on failure.
 */
int reset_netconfig_check(void)
{
    int rt = OPRT_OK;
    uint8_t rst_cnt = 0;

    TUYA_CALL_ERR_LOG(reset_count_read(&rst_cnt));
    if (rst_cnt < RESET_NETCNT_MAX) {
        return OPRT_OK;
    }

    core_event_subscribe(EVENT_RESET, "reset_netconfig", __reset_netconfig_clear, SUBSCRIBE_TYPE_NORMAL);

    PR_DEBUG("Reset ctrl data!");
    tuya_iot_reset(tuya_iot_client_get());

    return rt;
}

/**
 * @brief Increment the reset counter and arm the 5-second clear timer
 *
 * Reads the counter, writes it back incremented by one, creates a one-shot
 * software timer and starts it with a 5000 ms timeout; when it fires,
 * reset_netconfig_timer() writes the counter back to 0. A normal boot (no
 * further reset within 5 seconds) therefore clears the counter, while 3
 * consecutive resets keep it at RESET_NETCNT_MAX for reset_netconfig_check().
 *
 * @return int Returns 0 on success, or a negative value on failure.
 */
int reset_netconfig_start(void)
{
    int rt = OPRT_OK;
    uint8_t rst_cnt = 0;

    TUYA_CALL_ERR_LOG(reset_count_read(&rst_cnt));
    TUYA_CALL_ERR_LOG(reset_count_write(++rst_cnt));

    PR_DEBUG("start reset cnt clear timer!!!!!");
    TIMER_ID rst_config_timer;
    core_sw_timer_create(reset_netconfig_timer, NULL, &rst_config_timer);
    core_sw_timer_start(rst_config_timer, 5000, CORE_TIMER_ONCE);

    return OPRT_OK;
}
