/**
 * @file demo_vcam.c
 * @brief ESP32 virtual camera: embedded Annex-B H.264 sample -> AU index ->
 *        cyclic frame feed at the configured frame rate.
 *
 * The ESP32 board has no camera in this demo. The sample is embedded with
 * IDF EMBED_FILES and indexed at open; the feed thread loops it forever.
 * There is no file I/O and no rate-variant ladder on this platform: the P2P
 * bitrate callback reports NOT_SUPPORTED and the core keeps its base rate.
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

typedef struct {
    uint32_t offset;
    uint32_t len;
    BOOL_T   is_i;
} DEMO_VCAM_AU_T;

extern const uint8_t demo_video_264_start[] asm("_binary_demo_video_264_start");
extern const uint8_t demo_video_264_end[] asm("_binary_demo_video_264_end");

static struct {
    const uint8_t  *buf;
    uint32_t        buf_len;
    DEMO_VCAM_AU_T *aus;
    uint32_t        au_num;
    uint32_t        i_num;
    uint32_t        fps;
    uint32_t        cur_kbps;
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

static uint32_t __demo_vcam_next_i(uint32_t cur)
{
    uint32_t i;

    if (s_vcam.aus == NULL || s_vcam.au_num == 0) {
        return 0;
    }
    if (cur >= s_vcam.au_num) {
        cur = s_vcam.au_num - 1;
    }
    for (i = cur + 1; i < s_vcam.au_num; i++) {
        if (s_vcam.aus[i].is_i) {
            return i;
        }
    }
    for (i = 0; i < cur; i++) {
        if (s_vcam.aus[i].is_i) {
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

        if (s_vcam.want_i_frame) {
            s_vcam.want_i_frame = FALSE;
            idx = __demo_vcam_next_i(idx);
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

        frame.data       = s_vcam.buf + s_vcam.aus[idx].offset;
        frame.len        = s_vcam.aus[idx].len;
        frame.ts_ms      = tick * 1000 / s_vcam.fps;
        frame.is_i_frame = s_vcam.aus[idx].is_i;
        if (s_vcam.cb) {
            s_vcam.cb(&frame);
        }

        idx = (idx + 1) % s_vcam.au_num;
        tick++;
    }

    s_vcam.exited = TRUE;
}

OPERATE_RET demo_vcam_open_embedded(uint32_t fps, uint32_t kbps, DEMO_VCAM_FRAME_CB cb)
{
    uint32_t au_num;
    uint32_t i_num = 0;
    uint32_t i;
    THREAD_CFG_T cfg;

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

    s_vcam.buf     = demo_video_264_start;
    s_vcam.buf_len = (uint32_t)(demo_video_264_end - demo_video_264_start);

    au_num = __demo_vcam_index(s_vcam.buf, s_vcam.buf_len, NULL, 0);
    if (au_num == 0) {
        PR_ERR("vcam: embedded sample has no AU");
        return OPRT_INVALID_PARM;
    }
    s_vcam.aus = (DEMO_VCAM_AU_T *)core_malloc(au_num * sizeof(DEMO_VCAM_AU_T));
    if (s_vcam.aus == NULL) {
        return OPRT_MALLOC_FAILED;
    }
    (void)__demo_vcam_index(s_vcam.buf, s_vcam.buf_len, s_vcam.aus, au_num);
    for (i = 0; i < au_num; i++) {
        if (s_vcam.aus[i].is_i) {
            i_num++;
        }
    }

    s_vcam.au_num      = au_num;
    s_vcam.i_num       = i_num;
    s_vcam.fps         = fps;
    s_vcam.cur_kbps    = kbps;
    s_vcam.cb          = cb;
    s_vcam.stop        = FALSE;
    s_vcam.started     = FALSE;
    s_vcam.exited      = FALSE;
    s_vcam.want_i_frame = FALSE;
    s_vcam.running     = TRUE;

    memset(&cfg, 0, sizeof(cfg));
    cfg.stackDepth = DEMO_VCAM_THREAD_STACK;
    cfg.priority   = THREAD_PRIO_3;
    cfg.thrdname   = "demo_vcam";
    /* Reads the embedded AU table and feeds P2P; no flash operation. */
    cfg.psram_mode = 1;
    if (core_thread_create_and_start(&s_vcam.thread, NULL, NULL, __demo_vcam_thread, NULL, &cfg) != OPRT_OK) {
        s_vcam.running = FALSE;
        core_free(s_vcam.aus);
        s_vcam.aus = NULL;
        return OPRT_COM_ERROR;
    }

    PR_NOTICE("vcam open: embedded %u bytes, %u AU, %u I-frame, %ufps, %ukbps",
              s_vcam.buf_len, au_num, i_num, fps, kbps);
    return OPRT_OK;
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

    core_free(s_vcam.aus);
    s_vcam.aus     = NULL;
    s_vcam.au_num  = 0;
    s_vcam.i_num   = 0;
    s_vcam.buf     = NULL;
    s_vcam.buf_len = 0;
    s_vcam.cb      = NULL;
    s_vcam.fps     = 0;
    s_vcam.cur_kbps = 0;
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
    (void)kbps;
    (void)path;
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET demo_vcam_set_bitrate(uint32_t kbps)
{
    (void)kbps;
    return OPRT_NOT_SUPPORTED;
}

BOOL_T demo_vcam_is_open(void)
{
    return s_vcam.running;
}

OPERATE_RET demo_vcam_get_stats(uint32_t *au_num, uint32_t *i_num, uint32_t *max_len, BOOL_T *first_is_i)
{
    uint32_t i;
    uint32_t maxl = 0;

    if (!s_vcam.running) {
        return OPRT_COM_ERROR;
    }
    for (i = 0; i < s_vcam.au_num; i++) {
        if (s_vcam.aus[i].len > maxl) {
            maxl = s_vcam.aus[i].len;
        }
    }
    if (au_num) {
        *au_num = s_vcam.au_num;
    }
    if (i_num) {
        *i_num = s_vcam.i_num;
    }
    if (max_len) {
        *max_len = maxl;
    }
    if (first_is_i) {
        *first_is_i = s_vcam.au_num ? s_vcam.aus[0].is_i : FALSE;
    }
    return OPRT_OK;
}
