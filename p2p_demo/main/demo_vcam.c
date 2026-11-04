/**
 * @file demo_vcam.c
 * @brief ESP32 virtual camera: embedded Annex-B H.264 ladder -> AU index ->
 *        cyclic frame feed at the configured frame rate.
 *
 * The ESP32 board has no camera in this demo. Three 320x240@10 samples
 * (250/150/100 kbps) are embedded with IDF EMBED_FILES and indexed at open.
 * demo_vcam_set_bitrate() switches the feed to the highest compatible variant
 * not above the P2P rate-control request; all variants share one
 * resolution/profile/fps/GOP and SPS/PPS, and the switch resumes on an IDR.
 *
 * @copyright Copyright (c) 2021-2026 Tuya Inc. All Rights Reserved.
 */

#include <string.h>

#include "demo_vcam.h"
#include "core_log.h"
#include "core_memory.h"
#include "core_system.h"
#include "core_thread.h"

#define DEMO_VCAM_THREAD_STACK   4096
#define DEMO_VCAM_WAIT_SLICE_MS  10
#define DEMO_VCAM_EXIT_WAIT_MS   2000
#define DEMO_VCAM_VARIANT_MAX    3
#define DEMO_VCAM_SWITCH_HOLD_MS 2000

#ifndef DEMO_VCAM_FORCE_SWITCH_TEST
#define DEMO_VCAM_FORCE_SWITCH_TEST 0
#endif

typedef struct {
    uint32_t offset;
    uint32_t len;
    BOOL_T   is_i;
} DEMO_VCAM_AU_T;

typedef struct {
    const uint8_t  *buf;
    uint32_t        buf_len;
    DEMO_VCAM_AU_T *aus;
    uint32_t        au_num;
    uint32_t        i_num;
    uint32_t        kbps;
    BOOL_T          compatible;
} DEMO_VCAM_VARIANT_T;

extern const uint8_t demo_video_264_start[] asm("_binary_demo_video_264_start");
extern const uint8_t demo_video_264_end[] asm("_binary_demo_video_264_end");
extern const uint8_t demo_video_150_264_start[] asm("_binary_demo_video_150_264_start");
extern const uint8_t demo_video_150_264_end[] asm("_binary_demo_video_150_264_end");
extern const uint8_t demo_video_100_264_start[] asm("_binary_demo_video_100_264_start");
extern const uint8_t demo_video_100_264_end[] asm("_binary_demo_video_100_264_end");

static struct {
    DEMO_VCAM_VARIANT_T variants[DEMO_VCAM_VARIANT_MAX];
    uint32_t        variant_num;
    volatile int    active;
    volatile int    pending; /* -1 = none */
    uint64_t        last_switch_ms;
    uint32_t        fps;
    uint32_t        cur_kbps;
    uint32_t        late_logs;
    BOOL_T          force_150;
    BOOL_T          force_100;
    DEMO_VCAM_FRAME_CB cb;
    THREAD_HANDLE   thread;
    volatile BOOL_T running;
    volatile BOOL_T stop;
    volatile BOOL_T started;
    volatile BOOL_T exited;
    volatile BOOL_T want_i_frame;
    volatile BOOL_T poisoned;
} s_vcam;

static BOOL_T __demo_vcam_find_sc(const uint8_t *buf, uint32_t len, uint32_t from, uint32_t *off, uint32_t *sc_len)
{
    uint32_t i;

    if (buf == NULL || off == NULL || sc_len == NULL || from >= len) {
        return FALSE;
    }
    for (i = from; i + 3 < len; i++) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            *off    = i;
            *sc_len = 3;
            return TRUE;
        }
        if (i + 4 < len && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1) {
            *off    = i;
            *sc_len = 4;
            return TRUE;
        }
    }
    return FALSE;
}

static void __demo_vcam_emit(DEMO_VCAM_AU_T *aus, uint32_t cap, uint32_t idx, uint32_t off, uint32_t len, BOOL_T is_i)
{
    if (aus != NULL && idx < cap) {
        aus[idx].offset = off;
        aus[idx].len    = len;
        aus[idx].is_i   = is_i;
    }
}

static uint32_t __demo_vcam_index(const uint8_t *buf, uint32_t len, DEMO_VCAM_AU_T *aus, uint32_t cap)
{
    uint32_t count      = 0;
    uint32_t cur_off    = 0;
    uint32_t cur_end    = 0;
    BOOL_T   cur_open   = FALSE;
    BOOL_T   cur_has_vcl = FALSE;
    BOOL_T   cur_is_i   = FALSE;
    uint32_t sc_off     = 0;
    uint32_t sc_len     = 0;
    BOOL_T   has        = __demo_vcam_find_sc(buf, len, 0, &sc_off, &sc_len);

    while (has) {
        uint32_t nal_off = sc_off;
        uint32_t nal_end = len;
        uint32_t next_off = 0;
        uint32_t next_len = 0;
        BOOL_T   has_next = FALSE;
        uint8_t  nal_type = 0;
        BOOL_T   is_vcl   = FALSE;
        BOOL_T   new_au   = FALSE;

        if (nal_off + sc_len >= len) {
            break;
        }
        nal_type = buf[nal_off + sc_len] & 0x1F;
        is_vcl   = (nal_type >= 1 && nal_type <= 5);

        has_next = __demo_vcam_find_sc(buf, len, nal_off + sc_len, &next_off, &next_len);
        if (has_next) {
            nal_end = next_off;
        }

        if (cur_open) {
            if (9 == nal_type) {
                new_au = TRUE;
            } else if (is_vcl && cur_has_vcl) {
                new_au = TRUE;
            }
        }
        if (new_au) {
            __demo_vcam_emit(aus, cap, count, cur_off, cur_end - cur_off, cur_is_i);
            count++;
            cur_open = FALSE;
        }
        if (!cur_open) {
            cur_off     = nal_off;
            cur_has_vcl = FALSE;
            cur_is_i    = FALSE;
            cur_open    = TRUE;
        }
        cur_end = nal_end;
        if (is_vcl) {
            cur_has_vcl = TRUE;
            if (5 == nal_type) {
                cur_is_i = TRUE;
            }
        }

        if (has_next) {
            sc_off = next_off;
            sc_len = next_len;
        }
        has = has_next;
    }

    if (cur_open && cur_end > cur_off) {
        __demo_vcam_emit(aus, cap, count, cur_off, cur_end - cur_off, cur_is_i);
        count++;
    }

    return count;
}

/**
 * @brief Locate the first SPS and PPS in an Annex-B sample
 *
 * Variants may only be switched mid-stream when their SPS/PPS match; the
 * decoder is not reset by the demo.
 */
static BOOL_T __demo_vcam_get_sps_pps(const uint8_t *buf, uint32_t len, const uint8_t **sps,
                                      uint32_t *sps_len, const uint8_t **pps, uint32_t *pps_len)
{
    uint32_t sc_off = 0;
    uint32_t sc_len = 0;

    *sps     = NULL;
    *sps_len = 0;
    *pps     = NULL;
    *pps_len = 0;
    if (buf == NULL || len == 0 || !__demo_vcam_find_sc(buf, len, 0, &sc_off, &sc_len)) {
        return FALSE;
    }

    while (1) {
        uint32_t nal_off = sc_off + sc_len;
        uint32_t next_off = 0;
        uint32_t next_len = 0;
        BOOL_T   has_next = __demo_vcam_find_sc(buf, len, nal_off, &next_off, &next_len);
        uint32_t nal_end = has_next ? next_off : len;
        uint8_t  nal_type = (nal_off < len) ? (buf[nal_off] & 0x1F) : 0;
        uint32_t nal_len = (nal_end > nal_off) ? (nal_end - nal_off) : 0;

        if (nal_type == 7 && *sps == NULL) {
            *sps     = buf + nal_off;
            *sps_len = nal_len;
        } else if (nal_type == 8 && *pps == NULL) {
            *pps     = buf + nal_off;
            *pps_len = nal_len;
        }
        if (*sps != NULL && *pps != NULL) {
            return TRUE;
        }
        if (!has_next) {
            break;
        }
        sc_off = next_off;
        sc_len = next_len;
    }
    return FALSE;
}

static uint32_t __demo_vcam_next_i(const DEMO_VCAM_VARIANT_T *v, uint32_t cur)
{
    uint32_t i;

    if (v == NULL || v->aus == NULL || v->au_num == 0) {
        return 0;
    }
    if (cur >= v->au_num) {
        cur = v->au_num - 1;
    }
    for (i = cur + 1; i < v->au_num; i++) {
        if (v->aus[i].is_i) {
            return i;
        }
    }
    for (i = 0; i < cur; i++) {
        if (v->aus[i].is_i) {
            return i;
        }
    }
    return cur;
}

static void __demo_vcam_thread(void *args)
{
    uint32_t period_ms;
    uint32_t idx = 0;
    uint64_t tick = 0;
    uint64_t start_ms;
    static uint32_t s_iframe_jumps = 0;

    (void)args;
    s_vcam.started = TRUE;

    period_ms = 1000 / s_vcam.fps;
    if (period_ms == 0) {
        period_ms = 1;
    }
    start_ms = core_system_get_millisecond();

    while (!s_vcam.stop) {
        uint64_t due_ms;
        DEMO_VCAM_FRAME_T frame;
        DEMO_VCAM_VARIANT_T *v;

        if (s_vcam.pending >= 0) {
            int p = s_vcam.pending;

            s_vcam.pending = -1;
            if (p >= 0 && p < (int)s_vcam.variant_num && s_vcam.variants[p].compatible &&
                s_vcam.variants[p].aus != NULL && s_vcam.variants[p].au_num > 0) {
                s_vcam.active   = p;
                s_vcam.cur_kbps = s_vcam.variants[p].kbps;
                idx = s_vcam.variants[p].aus[0].is_i ? 0 : __demo_vcam_next_i(&s_vcam.variants[p], 0);
                PR_NOTICE("vcam switch: %ukbps, %u AU", s_vcam.variants[p].kbps, s_vcam.variants[p].au_num);
            }
        }

        v = &s_vcam.variants[s_vcam.active];
        if (s_vcam.want_i_frame) {
            s_vcam.want_i_frame = FALSE;
            idx = __demo_vcam_next_i(v, idx);
            if (s_iframe_jumps < 8) {
                s_iframe_jumps++;
                PR_NOTICE("vcam I-frame jump: au=%u", idx);
            }
        }

        due_ms = start_ms + tick * period_ms;
        while (!s_vcam.stop) {
            uint64_t now_ms = core_system_get_millisecond();
            uint32_t slice;

            if (now_ms >= due_ms) {
                break;
            }
            slice = (due_ms - now_ms) > DEMO_VCAM_WAIT_SLICE_MS ? DEMO_VCAM_WAIT_SLICE_MS
                                                               : (uint32_t)(due_ms - now_ms);
            core_system_sleep(slice ? slice : 1);
        }
        if (s_vcam.stop) {
            break;
        }

        {
            uint64_t late = core_system_get_millisecond() - due_ms;
            if (late > 200 && s_vcam.late_logs < 5) {
                s_vcam.late_logs++;
                PR_WARN("vcam tick late: %ums (tick=%u)", (unsigned)late, (unsigned)tick);
            }
        }

        frame.data       = v->buf + v->aus[idx].offset;
        frame.len        = v->aus[idx].len;
        frame.ts_ms      = tick * 1000 / s_vcam.fps;
        frame.is_i_frame = v->aus[idx].is_i;
        if (s_vcam.cb) {
            s_vcam.cb(&frame);
        }

        idx = (idx + 1) % v->au_num;
        tick++;

#if DEMO_VCAM_FORCE_SWITCH_TEST
        {
            uint64_t elapsed_ms = core_system_get_millisecond() - start_ms;

            if (!s_vcam.force_150 && elapsed_ms >= 10000) {
                s_vcam.force_150 = TRUE;
                PR_NOTICE("vcam force switch: 150kbps ret=%d", demo_vcam_set_bitrate(150));
            } else if (!s_vcam.force_100 && elapsed_ms >= 20000) {
                s_vcam.force_100 = TRUE;
                PR_NOTICE("vcam force switch: 100kbps ret=%d", demo_vcam_set_bitrate(100));
            }
        }
#endif
    }

    s_vcam.exited = TRUE;
}

typedef struct {
    uint32_t        kbps;
    const uint8_t  *start;
    const uint8_t  *end;
} DEMO_VCAM_EMBEDDED_T;

/* Base first, then the fallbacks. The selection scan is order-independent,
 * but the 250 kbps entry must stay first: it is the SPS/PPS reference and the
 * rate the P2P AV info advertises. */
static const DEMO_VCAM_EMBEDDED_T s_embedded[] = {
    {250, demo_video_264_start, demo_video_264_end},
    {150, demo_video_150_264_start, demo_video_150_264_end},
    {100, demo_video_100_264_start, demo_video_100_264_end},
};
_Static_assert(sizeof(s_embedded) / sizeof(s_embedded[0]) <= DEMO_VCAM_VARIANT_MAX,
               "DEMO_VCAM_VARIANT_MAX is smaller than the embedded ladder");

OPERATE_RET demo_vcam_open_embedded(uint32_t fps, uint32_t kbps, DEMO_VCAM_FRAME_CB cb)
{
    const uint8_t *base_sps = NULL;
    const uint8_t *base_pps = NULL;
    uint32_t       base_sps_len = 0;
    uint32_t       base_pps_len = 0;
    uint32_t       n;
    int            active = -1;
    OPERATE_RET    ret = OPRT_MALLOC_FAILED;
    THREAD_CFG_T   cfg;

    if (fps == 0 || cb == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (s_vcam.running) {
        return OPRT_COM_ERROR;
    }
    if (s_vcam.poisoned) {
        PR_ERR("vcam: unusable after a timed-out close");
        return OPRT_COM_ERROR;
    }

    memset(s_vcam.variants, 0, sizeof(s_vcam.variants));
    s_vcam.variant_num    = 0;
    s_vcam.pending        = -1;
    s_vcam.last_switch_ms = 0;

    for (n = 0; n < sizeof(s_embedded) / sizeof(s_embedded[0]) && n < DEMO_VCAM_VARIANT_MAX; n++) {
        DEMO_VCAM_VARIANT_T *v = &s_vcam.variants[s_vcam.variant_num];
        uint32_t             au_num;
        uint32_t             i_num = 0;
        uint32_t             i;

        v->kbps    = s_embedded[n].kbps;
        v->buf     = s_embedded[n].start;
        v->buf_len = (uint32_t)(s_embedded[n].end - s_embedded[n].start);
        au_num     = __demo_vcam_index(v->buf, v->buf_len, NULL, 0);
        if (au_num == 0) {
            PR_ERR("vcam: embedded %ukbps sample has no AU", v->kbps);
            ret = OPRT_INVALID_PARM;
            goto fail;
        }
        v->aus = (DEMO_VCAM_AU_T *)core_malloc(au_num * sizeof(DEMO_VCAM_AU_T));
        if (v->aus == NULL) {
            goto fail;
        }
        (void)__demo_vcam_index(v->buf, v->buf_len, v->aus, au_num);
        for (i = 0; i < au_num; i++) {
            if (v->aus[i].is_i) {
                i_num++;
            }
        }
        v->au_num     = au_num;
        v->i_num      = i_num;
        v->compatible = TRUE;
        s_vcam.variant_num++;
        PR_DEBUG("vcam variant: %ukbps, %u bytes, %u AU, %u I", v->kbps, v->buf_len, au_num, i_num);
    }
    if (s_vcam.variant_num == 0) {
        PR_ERR("vcam: embedded ladder is empty");
        goto fail;
    }

    /* All variants must share the base SPS/PPS or the decoder sees a new
     * stream at every switch. */
    if (__demo_vcam_get_sps_pps(s_vcam.variants[0].buf, s_vcam.variants[0].buf_len, &base_sps,
                                &base_sps_len, &base_pps, &base_pps_len)) {
        for (n = 1; n < s_vcam.variant_num; n++) {
            const uint8_t *sps = NULL;
            const uint8_t *pps = NULL;
            uint32_t       sps_len = 0;
            uint32_t       pps_len = 0;

            if (!__demo_vcam_get_sps_pps(s_vcam.variants[n].buf, s_vcam.variants[n].buf_len, &sps, &sps_len,
                                         &pps, &pps_len) ||
                sps_len != base_sps_len || pps_len != base_pps_len ||
                memcmp(sps, base_sps, sps_len) != 0 || memcmp(pps, base_pps, pps_len) != 0) {
                s_vcam.variants[n].compatible = FALSE;
                PR_ERR("vcam variant %ukbps SPS/PPS differs; disabled", s_vcam.variants[n].kbps);
            }
        }
    } else {
        PR_ERR("vcam base sample has no SPS/PPS; only the base variant is usable");
        for (n = 1; n < s_vcam.variant_num; n++) {
            s_vcam.variants[n].compatible = FALSE;
        }
    }

    for (n = 0; n < s_vcam.variant_num; n++) {
        if (!s_vcam.variants[n].compatible) {
            continue;
        }
        if (s_vcam.variants[n].kbps <= kbps &&
            (active < 0 || s_vcam.variants[n].kbps > s_vcam.variants[active].kbps)) {
            active = (int)n;
        }
    }
    if (active < 0) {
        for (n = 0; n < s_vcam.variant_num; n++) {
            if (!s_vcam.variants[n].compatible) {
                continue;
            }
            if (active < 0 || s_vcam.variants[n].kbps < s_vcam.variants[active].kbps) {
                active = (int)n;
            }
        }
    }
    if (active < 0) {
        PR_ERR("vcam: no compatible variant");
        ret = OPRT_NOT_SUPPORTED;
        goto fail;
    }

    s_vcam.active       = active;
    s_vcam.cur_kbps     = s_vcam.variants[active].kbps;
    s_vcam.fps          = fps;
    s_vcam.late_logs    = 0;
    s_vcam.force_150    = FALSE;
    s_vcam.force_100    = FALSE;
    s_vcam.cb           = cb;
    s_vcam.stop         = FALSE;
    s_vcam.started      = FALSE;
    s_vcam.exited       = FALSE;
    s_vcam.want_i_frame = FALSE;
    s_vcam.running      = TRUE;

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = DEMO_VCAM_THREAD_STACK;
    cfg.priority   = THREAD_PRIO_3;
    cfg.thrdname   = "demo_vcam";
    /* Reads the embedded AU table and feeds P2P; no flash operation. */
    cfg.psram_mode = 1;
    if (core_thread_create_and_start(&s_vcam.thread, NULL, NULL, __demo_vcam_thread, NULL, &cfg) != OPRT_OK) {
        s_vcam.running = FALSE;
        ret = OPRT_COM_ERROR;
        goto fail;
    }

    PR_NOTICE("vcam open: %u variants, active %ukbps, %ufps", s_vcam.variant_num, s_vcam.cur_kbps, fps);
    return OPRT_OK;

fail:
    for (n = 0; n < DEMO_VCAM_VARIANT_MAX; n++) {
        if (s_vcam.variants[n].aus != NULL) {
            core_free(s_vcam.variants[n].aus);
            s_vcam.variants[n].aus = NULL;
        }
    }
    s_vcam.variant_num = 0;
    s_vcam.active      = 0;
    s_vcam.cur_kbps    = 0;
    return ret;
}

OPERATE_RET demo_vcam_open(const char *path, uint32_t fps, uint32_t kbps, DEMO_VCAM_FRAME_CB cb)
{
    (void)path; /* ESP32 uses the embedded sample */
    return demo_vcam_open_embedded(fps, kbps, cb);
}

OPERATE_RET demo_vcam_close(void)
{
    uint32_t waited_ms = 0;

    if (!s_vcam.running) {
        return OPRT_OK;
    }
    s_vcam.running = FALSE;
    s_vcam.stop    = TRUE;

    if (s_vcam.thread != NULL) {
        while (!s_vcam.started && waited_ms < DEMO_VCAM_EXIT_WAIT_MS) {
            core_system_sleep(5);
            waited_ms += 5;
        }
        if (!s_vcam.started) {
            s_vcam.poisoned = TRUE;
            PR_ERR("vcam close: thread not started in %ums", waited_ms);
            return OPRT_TIMEOUT;
        }
        core_thread_delete(s_vcam.thread);
        s_vcam.thread = NULL;
        while (!s_vcam.exited && waited_ms < DEMO_VCAM_EXIT_WAIT_MS) {
            core_system_sleep(5);
            waited_ms += 5;
        }
        if (!s_vcam.exited) {
            s_vcam.poisoned = TRUE;
            PR_ERR("vcam close: thread not exited in %ums", waited_ms);
            return OPRT_TIMEOUT;
        }
    }

    for (uint32_t i = 0; i < DEMO_VCAM_VARIANT_MAX; i++) {
        core_free(s_vcam.variants[i].aus);
        memset(&s_vcam.variants[i], 0, sizeof(s_vcam.variants[i]));
    }
    s_vcam.variant_num    = 0;
    s_vcam.active         = 0;
    s_vcam.pending        = -1;
    s_vcam.last_switch_ms = 0;
    s_vcam.cb             = NULL;
    s_vcam.fps            = 0;
    s_vcam.cur_kbps       = 0;
    PR_NOTICE("vcam close");
    return OPRT_OK;
}

OPERATE_RET demo_vcam_request_i_frame(void)
{
    if (!s_vcam.running) {
        return OPRT_COM_ERROR;
    }
    s_vcam.want_i_frame = TRUE;
    return OPRT_OK;
}

OPERATE_RET demo_vcam_add_variant(uint32_t kbps, const char *path)
{
    uint32_t i;

    (void)path; /* embedded ladder: the path only documents the asset */
    if (kbps == 0) {
        return OPRT_INVALID_PARM;
    }
    for (i = 0; i < sizeof(s_embedded) / sizeof(s_embedded[0]); i++) {
        if (s_embedded[i].kbps == kbps) {
            return OPRT_OK;
        }
    }
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET demo_vcam_set_bitrate(uint32_t kbps)
{
    int      best = -1;
    uint32_t i;
    uint64_t now;

    if (!s_vcam.running) {
        return OPRT_COM_ERROR;
    }
    if (s_vcam.variant_num == 0) {
        return OPRT_NOT_SUPPORTED;
    }
    /* One switch at a time. While a switch or its hold-down is pending the
     * core keeps its current rate and retries next window. */
    if (s_vcam.pending >= 0) {
        return OPRT_RESOURCE_NOT_READY;
    }
    now = core_system_get_millisecond();
    if (s_vcam.last_switch_ms != 0 && now - s_vcam.last_switch_ms < DEMO_VCAM_SWITCH_HOLD_MS) {
        return OPRT_RESOURCE_NOT_READY;
    }

    for (i = 0; i < s_vcam.variant_num; i++) {
        if (!s_vcam.variants[i].compatible) {
            continue;
        }
        if (s_vcam.variants[i].kbps <= kbps &&
            (best < 0 || s_vcam.variants[i].kbps > s_vcam.variants[best].kbps)) {
            best = (int)i;
        }
    }
    if (best < 0) {
        for (i = 0; i < s_vcam.variant_num; i++) {
            if (!s_vcam.variants[i].compatible) {
                continue;
            }
            if (best < 0 || s_vcam.variants[i].kbps < s_vcam.variants[best].kbps) {
                best = (int)i;
            }
        }
    }
    if (best < 0 || s_vcam.variants[best].kbps > kbps) {
        PR_DEBUG("vcam set bitrate: %ukbps below lowest usable variant", kbps);
        return OPRT_NOT_SUPPORTED;
    }
    if (best == s_vcam.active) {
        return OPRT_OK;
    }

    s_vcam.pending        = best;
    s_vcam.last_switch_ms = now;
    PR_DEBUG("vcam set bitrate: request %ukbps -> %ukbps", kbps, s_vcam.variants[best].kbps);
    return OPRT_OK;
}

BOOL_T demo_vcam_is_open(void)
{
    return s_vcam.running;
}

OPERATE_RET demo_vcam_get_stats(uint32_t *au_num, uint32_t *i_num, uint32_t *max_len, BOOL_T *first_is_i)
{
    const DEMO_VCAM_VARIANT_T *v;
    uint32_t i;
    uint32_t maxl = 0;

    if (!s_vcam.running) {
        return OPRT_COM_ERROR;
    }
    v = &s_vcam.variants[s_vcam.active];
    if (v->aus == NULL || v->au_num == 0) {
        return OPRT_COM_ERROR;
    }
    for (i = 0; i < v->au_num; i++) {
        if (v->aus[i].len > maxl) {
            maxl = v->aus[i].len;
        }
    }
    if (au_num) {
        *au_num = v->au_num;
    }
    if (i_num) {
        *i_num = v->i_num;
    }
    if (max_len) {
        *max_len = maxl;
    }
    if (first_is_i) {
        *first_is_i = v->au_num ? v->aus[0].is_i : FALSE;
    }
    return OPRT_OK;
}
