/**
 * @file ota_app.c
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

#include "core_api.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "ota_app.h"

typedef enum {
    OTA_STATE_INIT,
    OTA_STATE_BEGIN,
    OTA_STATE_IN_PROGRESS,
    OTA_STATE_SUCCESS,
} OTA_STATE_T;

typedef struct {
    OTA_STATE_T  state;
    esp_ota_handle_t handle;
    const esp_partition_t *partition;
    unsigned int     image_size;
    unsigned int     offset_image_size;
} OTA_HANDLE_S;

#define DBG_TAG "OTA_APP"

#define OTA_HANDLE_INIT(h) do { \
     (h).state = OTA_STATE_INIT; \
     (h).handle = 0; \
     (h).partition = NULL; \
     (h).image_size = 0; \
     (h).offset_image_size = 0; \
} while (0)

static OTA_HANDLE_S s_ota_handle = {
    .state = OTA_STATE_INIT,
    .handle = 0,
    .partition = NULL,
    .image_size = 0,
    .offset_image_size = 0
};

/**
 * @brief Failure cleanup: discard an unfinished upgrade (abort releases the
 * handle and the partition entry) and return the state to INIT
 *
 * There are two call sites: the write-failure branch in data_process (core does
 * not check the return value of core_ota_data_process -- tuya_ota.c:81 -- so the
 * implementation must clean up after itself; otherwise the state stays
 * IN_PROGRESS and start_notify is permanently rejected, leaving the device
 * without OTA until it reboots), and the error paths of ota_end_notify.
 * Idempotent: abort is skipped when the handle has already been released.
 */
static void __ota_fail_cleanup(void)
{
    if (s_ota_handle.handle) {
        (void)esp_ota_abort(s_ota_handle.handle); /* failure can be ignored (may already be ended/aborted) */
    }
    OTA_HANDLE_INIT(s_ota_handle);
}

/**
 * @brief ota start notify (registered callback: CORE_OTA_CBS_T.start_notify)
 *
 * @param[in] image_size:  image size
 * @param[in] type:        ota type
 * @param[in] path:        ota path
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET ota_start_notify(uint32_t image_size, TUYA_OTA_TYPE_E type, TUYA_OTA_PATH_E path)
{
    (void)type;
    (void)path;

    if (0 == image_size) {
        ESP_LOGE(DBG_TAG, "%s: invalid ota image size %u", __func__, (unsigned)image_size);
        return OPRT_INVALID_PARM;
    }

    /* Entry self-healing: both failure endpoints (a write failure stops in
     * IN_PROGRESS, an hmac failure stops in SUCCESS) are ultimately recovered
     * here. Core's OTA flow is single-threaded and serial (DL_EVENT_* handled
     * in order), so there are no concurrent sessions; clearing a stale session
     * does not interrupt an upgrade in progress. */
    if (OTA_STATE_INIT != s_ota_handle.state) {
        ESP_LOGW(DBG_TAG, "%s: stale ota session (state=%d), cleanup and restart", __func__,
                 s_ota_handle.state);
        __ota_fail_cleanup();
        /* On self-healing, erase the whole target partition: esp_ota_begin
         * with a non-zero image_size only erases aligned_erase_size
         * (esp_ota_ops.c:147-158), so if this image_size is smaller than the
         * range left over from the previous failure, the unerased tail makes
         * esp_ota_write fail (NOR cannot write 0 back to 1). Self-healing is a
         * rare exception path; one full erase (the whole update partition,
         * hundreds of milliseconds) buys unconditional correctness. */
        const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
        if (p) {
            (void)esp_partition_erase_range(p, 0, p->size);
        }
    }

    OTA_HANDLE_INIT(s_ota_handle);

    s_ota_handle.partition = esp_ota_get_next_update_partition(NULL);
    if (NULL == s_ota_handle.partition) {
        ESP_LOGE(DBG_TAG, "%s: get next update partition failed (partitions.csv needs ota_0/ota_1?)", __func__);
        return OPRT_COM_ERROR;
    }

    if (ESP_OK != esp_ota_begin(s_ota_handle.partition, image_size, &s_ota_handle.handle)) {
        s_ota_handle.handle = 0;
        s_ota_handle.partition = NULL;
        ESP_LOGE(DBG_TAG, "%s: call esp_ota_begin failed", __func__);
        return OPRT_COM_ERROR;
    }

    s_ota_handle.image_size = image_size;
    s_ota_handle.state = OTA_STATE_BEGIN;

    return OPRT_OK;
}

/**
 * @brief ota data process (registered callback: CORE_OTA_CBS_T.data_process)
 *
 * @param[in] pack:       point to ota pack
 * @param[out] remain_len: ota pack remain len
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET ota_data_process(TUYA_OTA_DATA_T *pack, uint32_t *remain_len)
{
    if (NULL == pack || NULL == remain_len) {
        ESP_LOGE(DBG_TAG, "%s: input invalid params", __func__);
        return OPRT_INVALID_PARM;
    }

    if (OTA_STATE_INIT == s_ota_handle.state) {
        ESP_LOGE(DBG_TAG, "%s: ota upgrade is not init", __func__);
        return OPRT_COM_ERROR;
    }

    if (OTA_STATE_SUCCESS == s_ota_handle.state) {
        *remain_len = 0;
        ESP_LOGI(DBG_TAG, "%s: ota process is successful", __func__);
        return OPRT_OK;
    }

    if (ESP_OK != esp_ota_write(s_ota_handle.handle, (const void *)pack->data, pack->len)) {
        ESP_LOGE(DBG_TAG, "%s: call esp_ota_write failed", __func__);
        /* core ignores the return value of this interface (tuya_ota.c:81);
         * this self-cleanup has two benefits:
         * 1) Correctness: after the state returns to INIT, subsequent data is
         *    rejected by the INIT gate at the top of this function (reported as
         *    "not init"); if the state stayed in IN_PROGRESS, data that core
         *    keeps sending would reach esp_ota_write with an already-aborted
         *    handle, which is worse than deferring recovery to the entry
         *    self-healing;
         * 2) Timeliness: the stale handle is released earlier, instead of
         *    waiting for the entry self-healing in the next start_notify. */
        __ota_fail_cleanup();
        return OPRT_COM_ERROR;
    }

    s_ota_handle.offset_image_size += pack->len;
    if (OTA_STATE_BEGIN == s_ota_handle.state) {
        s_ota_handle.state = OTA_STATE_IN_PROGRESS;
    }

    *remain_len = 0;
    if ((OTA_STATE_IN_PROGRESS == s_ota_handle.state) &&
        (s_ota_handle.offset_image_size >= s_ota_handle.image_size)) {
        s_ota_handle.state = OTA_STATE_SUCCESS;
    }

    return OPRT_OK;
}

/**
 * @brief ota end notify (registered callback: CORE_OTA_CBS_T.end_notify)
 *
 * @param[in] reset:  ota reset
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
OPERATE_RET ota_end_notify(BOOL_T reset)
{
    /* Note: core's only call site is end_notify(TRUE) on the success path
     * (tuya_ota.c:118); the DL_EVENT_FAULT / hmac verification failure
     * branches send no notification. Failure recovery therefore relies on the
     * entry self-healing in start_notify plus the write-failure self-cleanup
     * in data_process.
     *
     * The reset parameter is unused: every call reaching this function is
     * TRUE, so the implementation always commits and reboots. */
    (void)reset;

    if (OTA_STATE_SUCCESS != s_ota_handle.state) {
        ESP_LOGE(DBG_TAG, "%s: ota upgrade state is not success", __func__);
        goto err_exit;
    }

    if (ESP_OK != esp_ota_end(s_ota_handle.handle)) {
        ESP_LOGE(DBG_TAG, "%s: call esp_ota_end failed", __func__);
        goto err_exit;
    }

    if (ESP_OK != esp_ota_set_boot_partition(s_ota_handle.partition)) {
        ESP_LOGE(DBG_TAG, "%s: call esp_ota_set_boot_partition failed", __func__);
        goto err_exit;
    }

    OTA_HANDLE_INIT(s_ota_handle);

    ESP_LOGI(DBG_TAG, "%s: ota upgrade successful, system reboot now...", __func__);
    esp_restart();
    return OPRT_OK;

err_exit:
    __ota_fail_cleanup();
    return OPRT_COM_ERROR;
}
