/**
 * @file reset_netcfg.h
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

#ifndef __RESET_NETCFG_H__
#define __RESET_NETCFG_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/***********************************************************
***********************typedef define***********************
***********************************************************/

/***********************************************************
********************function declaration********************
***********************************************************/
/**
 * @brief Starts the network configuration reset process: increments the reset
 *        counter and starts a 5-second one-shot timer that clears the counter.
 *
 * On a normal boot (no further reset within 5 seconds) the counter is cleared;
 * after 3 consecutive resets the next reset_netconfig_check() clears the
 * network configuration data.
 *
 * @return int Returns 0 on success, or a negative value on failure.
 */
int reset_netconfig_start(void);

/**
 * @brief Checks the reset counter against the threshold and clears the network
 *        configuration once it is reached.
 *
 * When the counter is >= 3, subscribes to EVENT_RESET (which clears the counter
 * after the reset event) and calls tuya_iot_reset() to erase the network
 * configuration data.
 *
 * @return int Returns 0 on success, or a negative value on failure.
 */
int reset_netconfig_check(void);

#ifdef __cplusplus
}
#endif

#endif /* __RESET_NETCFG_H__ */
