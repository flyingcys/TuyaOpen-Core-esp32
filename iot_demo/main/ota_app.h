/**
 * @file ota_app.h
 * @brief Application-layer OTA implementation (esp_ota): start/data/end
 *        callbacks registered with the core through CORE_OTA_CBS_T
 *
 * The upgrade image is written through esp_ota into the next update partition
 * (ota_0/ota_1 alternating); end_notify switches the boot partition and
 * reboots unconditionally (the reset parameter is ignored: every call reaching
 * it is TRUE). Note: the partition table must contain otadata and the
 * ota_0/ota_1 partitions, otherwise start_notify fails to get a partition (an
 * error code is returned; it does not crash).
 *
 * State machine: INIT -> BEGIN -> IN_PROGRESS -> SUCCESS. start_notify opens
 * a session on the next update partition; data_process appends data and moves
 * to SUCCESS once the received size reaches image_size; end_notify finalizes,
 * switches the boot partition and reboots. Failure endpoints (write failure,
 * non-success state at end_notify) run __ota_fail_cleanup(), and start_notify
 * clears any leftover session before opening a new one.
 *
 * The callbacks are installed by user_main() via core_ota_register_cbs(); when
 * no callbacks are registered, core's core_ota_* entry points return
 * OPRT_NOT_SUPPORTED.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __OTA_APP_H__
#define __OTA_APP_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief ota start notify (registered callback: CORE_OTA_CBS_T.start_notify)
*
* @param[in] image_size:  image size
* @param[in] type:        ota type
* @param[in] path:        ota path
*
* @note This API is used for ota start notify
*
* @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
*/
OPERATE_RET ota_start_notify(uint32_t image_size, TUYA_OTA_TYPE_E type, TUYA_OTA_PATH_E path);

/**
* @brief ota data process (registered callback: CORE_OTA_CBS_T.data_process)
*
* @param[in] pack:       point to ota pack
* @param[out] remain_len: ota pack remain len
*
* @note This API is used for ota data process
*
* @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
*/
OPERATE_RET ota_data_process(TUYA_OTA_DATA_T *pack, uint32_t *remain_len);

/**
* @brief ota end notify (registered callback: CORE_OTA_CBS_T.end_notify)
*
* @param[in] reset:  ota reset
*
* @note This API is used for ota end notify
*
* @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
*/
OPERATE_RET ota_end_notify(BOOL_T reset);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_APP_H__ */
