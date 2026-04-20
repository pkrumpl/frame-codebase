/*
 * FOMO Hand Detection Experiment Implementation
 *
 * Object detection experiment using a FOMO model trained for hand detection.
 * Input: 64x64 RGB. Output: 8x8x2 (background, hand).
 * Compiled when ML_EXPERIMENT=FOMO_HAND_DETECTION is set.
 *
 * Memory pattern: temp + rgb buffers live in BSS (~48 KB total). Heap is
 * untouched on the hot path; the only per-call allocations are the small
 * BT chunk buffer and SPI sprite payloads.
 */

#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include "experiment_common.h"
#include "error_logging.h"
#include "tflm_wrapper.h"
#include "bluetooth.h"
#include "spi.h"
#include "watchdog.h"
#include "nrfx_systick.h"
#include "lauxlib.h"

/* For DWT cycle counter timing */
#include "nrf.h"

/* FPGA is an enum constant from spi.h, not an extern variable */

#ifdef DEV_KIT_BUILD
extern uint8_t test_jpeg_data[];
extern const size_t test_jpeg_size;
#endif

/* Image dimensions */
#define CAPTURE_SIZE      720
#define SCALED_SIZE       90    /* After TJpgDec scale=3 (720/8=90) */
#define OUT_DIM           64    /* ML input dimension (64x64) */
#define NUM_CHANNELS      3     /* RGB */

#define RGB_CAPTURE_BYTES (SCALED_SIZE * SCALED_SIZE * NUM_CHANNELS)  /* 24,300 */
#define RGB_OUTPUT_BYTES  (OUT_DIM * OUT_DIM * NUM_CHANNELS)          /* 12,288 */

/* The 64x64 RGB output is smaller than a typical JPEG, so s_rgb_buffer is
 * sized to hold the JPEG (the larger of the two uses) and only the first
 * RGB_OUTPUT_BYTES are valid after downscale. */
#define MAX_JPEG_SIZE     (24 * 1024)
#define BT_CHUNK_SIZE     200
#define READ_CHUNK_SIZE   512

/* Working buffers in BSS to eliminate heap fragmentation across streaming
 * calls. s_rgb_buffer is double-purposed: it holds the raw JPEG during
 * read+decode, then the final 64x64 RGB image after downscale. The JPEG
 * bytes are consumed by the decoder before downscale overwrites them. */
static uint8_t s_temp_buffer[RGB_CAPTURE_BYTES];  /* 24,300 B (decode dst) */
static uint8_t s_rgb_buffer[MAX_JPEG_SIZE];       /* 24,576 B (jpeg src,
                                                              then 12,288 B
                                                              RGB output) */

/* Format current heap stats into buf. Suitable for inclusion in error
 * messages so the failure mode is visible over Bluetooth. */
static void heap_stats(int *out_free, int *out_frags)
{
    struct mallinfo mi = mallinfo();
    if (out_free)  *out_free  = (int)mi.fordblks;
    if (out_frags) *out_frags = (int)mi.ordblks;
}

/*-----------------------------------------------*/
/* FOMO post-processing                          */
/*-----------------------------------------------*/

/* Detection threshold in float (post-softmax probability) space.
 * Edge Impulse default is 0.5; tinyML deployments often need 0.05-0.3
 * for usable recall. Settable from Lua via set_threshold(). */
static float s_threshold = 0.5f;

/* "Cube" = a connected component of above-threshold cells of the same
 * class on the FOMO heatmap. FOMO does NOT use IoU-style NMS; instead
 * adjacent positive cells are merged into one detection (Edge Impulse's
 * `ei_handle_cube` algorithm). */
typedef struct {
    int8_t x, y;          /* grid coords of top-left corner */
    int8_t width, height; /* in grid cells */
    float score;          /* max score across cells in this cube */
} cube_t;

/* Up to 16 simultaneous detections per frame is more than enough for an
 * 8x8 grid (64 cells max). */
#define MAX_CUBES 16
static cube_t s_cubes[MAX_CUBES];
static int s_cube_count = 0;

/* Returns true if (x, y) is within or directly adjacent to cube `c`. */
static bool cube_overlaps(const cube_t *c, int x, int y)
{
    int x0 = c->x - 1, x1 = c->x + c->width;       /* +/-1 cell margin */
    int y0 = c->y - 1, y1 = c->y + c->height;
    return (x >= x0 && x <= x1 && y >= y0 && y <= y1);
}

static void cube_expand(cube_t *c, int x, int y, float score)
{
    int x0 = c->x < x ? c->x : x;
    int y0 = c->y < y ? c->y : y;
    int x1 = (c->x + c->width  - 1) > x ? (c->x + c->width  - 1) : x;
    int y1 = (c->y + c->height - 1) > y ? (c->y + c->height - 1) : y;
    c->x = (int8_t)x0;
    c->y = (int8_t)y0;
    c->width  = (int8_t)(x1 - x0 + 1);
    c->height = (int8_t)(y1 - y0 + 1);
    if (score > c->score) c->score = score;
}

/* Build the cube list from the dequantized heatmap.  Caller can then
 * iterate s_cubes[0..s_cube_count) and emit one detection per cube. */
static void build_cubes(const int8_t *output_grid)
{
    s_cube_count = 0;

    float out_scale; int32_t out_zp;
    fomo_get_quant_params(NULL, NULL, &out_scale, &out_zp);

    /* Diagnostic: track max score across the whole grid so the user can
     * tell whether the model is producing high confidences (and
     * threshold is wrong) vs. uniformly low scores (model is the issue). */
    float max_score = 0.0f;
    int max_x = -1, max_y = -1;

    for (int gy = 0; gy < FOMO_GRID_SIZE; gy++) {
        for (int gx = 0; gx < FOMO_GRID_SIZE; gx++) {
            int idx = (gy * FOMO_GRID_SIZE + gx) * FOMO_NUM_CLASSES;
            /* Skip class 0 (background); iterate non-bg classes. For our
             * model FOMO_NUM_CLASSES = 2, so only class 1 (hand). */
            for (int c = 1; c < FOMO_NUM_CLASSES; c++) {
                int8_t q = output_grid[idx + c];
                float prob = (float)((int32_t)q - out_zp) * out_scale;

                if (prob > max_score) {
                    max_score = prob;
                    max_x = gx; max_y = gy;
                }

                if (prob < s_threshold) continue;

                /* Try to merge into an existing cube */
                bool merged = false;
                for (int i = 0; i < s_cube_count; i++) {
                    if (cube_overlaps(&s_cubes[i], gx, gy)) {
                        cube_expand(&s_cubes[i], gx, gy, prob);
                        merged = true;
                        break;
                    }
                }
                if (!merged && s_cube_count < MAX_CUBES) {
                    s_cubes[s_cube_count].x = (int8_t)gx;
                    s_cubes[s_cube_count].y = (int8_t)gy;
                    s_cubes[s_cube_count].width = 1;
                    s_cubes[s_cube_count].height = 1;
                    s_cubes[s_cube_count].score = prob;
                    s_cube_count++;
                }
            }
        }
    }

    LOG("FOMO heatmap: max=%.3f at (%d,%d), thr=%.3f, cubes=%d",
        (double)max_score, max_x, max_y, (double)s_threshold, s_cube_count);
}

/*-----------------------------------------------*/
/* Hand Detection Overlay                        */
/*-----------------------------------------------*/

/**
 * Draw detection overlay on Frame display after FOMO inference.
 * Receives the already-built cube list (one dot per cube, not per cell).
 * Edge indicators (left/right vertical bars) for cubes whose centroid
 * falls outside the display FOV.
 */
static void draw_hand_detection_overlay(const int8_t *output_grid)
{
    (void)output_grid;  /* unused - cubes already built by build_cubes() */

    const uint16_t DISPLAY_W = 640;
    const uint16_t DISPLAY_H = 400;

    const int CENTER_START = 2;
    const int CENTER_END = 5;
    const int CENTER_COLS = CENTER_END - CENTER_START + 1;

    const uint16_t CELL_HEIGHT = DISPLAY_H / FOMO_GRID_SIZE;

    const uint16_t SPRITE_SIZE = 24;

    const uint16_t LINE_WIDTH = 12;
    const uint16_t LINE_HEIGHT = 80;

    const uint8_t HAND_COLOR = 10;  /* GREEN */

    /* 24x24 filled circle sprite (2-color, 1-bit): 576 pixels / 8 = 72 bytes */
    static const uint8_t dot_sprite[72] = {
        0x00, 0x7E, 0x00,
        0x01, 0xFF, 0x80,
        0x07, 0xFF, 0xE0,
        0x0F, 0xFF, 0xF0,
        0x1F, 0xFF, 0xF8,
        0x3F, 0xFF, 0xFC,
        0x3F, 0xFF, 0xFC,
        0x7F, 0xFF, 0xFE,
        0x7F, 0xFF, 0xFE,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF,
        0x7F, 0xFF, 0xFE,
        0x7F, 0xFF, 0xFE,
        0x3F, 0xFF, 0xFC,
        0x3F, 0xFF, 0xFC,
        0x1F, 0xFF, 0xF8,
        0x0F, 0xFF, 0xF0,
        0x07, 0xFF, 0xE0,
        0x01, 0xFF, 0x80,
        0x00, 0x7E, 0x00,
    };
    const size_t DOT_SPRITE_SIZE = 72;

    bool left_edge = false;
    bool right_edge = false;
    int16_t left_edge_y = -1;
    int16_t right_edge_y = -1;

    /* Iterate cubes (one detection each), not raw cells. */
    for (int i = 0; i < s_cube_count; i++) {
        const cube_t *c = &s_cubes[i];
        /* Centroid in grid space (float to keep mid-cube precision) */
        float cx_f = c->x + (c->width  - 1) * 0.5f;
        float cy_f = c->y + (c->height - 1) * 0.5f;

        int16_t py = (int16_t)(cy_f * CELL_HEIGHT + (CELL_HEIGHT - SPRITE_SIZE) / 2);
        if (py < 0) py = 0;
        if (py > DISPLAY_H - SPRITE_SIZE) py = DISPLAY_H - SPRITE_SIZE;

        /* Use the centroid column to decide on-screen vs edge indicator */
        int gx_center = (int)(cx_f + 0.5f);

        if (gx_center >= CENTER_START && gx_center <= CENTER_END) {
            float rel_x = cx_f - CENTER_START;
            uint16_t cell_width = DISPLAY_W / CENTER_COLS;
            int16_t px = (int16_t)(rel_x * cell_width + (cell_width - SPRITE_SIZE) / 2);

            if (px < 0) px = 0;
            if (px > DISPLAY_W - SPRITE_SIZE) px = DISPLAY_W - SPRITE_SIZE;

            uint8_t meta[8] = {
                (uint8_t)(px >> 8), (uint8_t)(px & 0xFF),
                (uint8_t)(py >> 8), (uint8_t)(py & 0xFF),
                0, SPRITE_SIZE,
                2,
                HAND_COLOR
            };

            uint8_t *payload = malloc(8 + DOT_SPRITE_SIZE);
            if (payload) {
                memcpy(payload, meta, 8);
                memcpy(payload + 8, dot_sprite, DOT_SPRITE_SIZE);
                spi_write(FPGA, 0x12, payload, 8 + DOT_SPRITE_SIZE);
                free(payload);
            }
        } else if (gx_center < CENTER_START) {
            left_edge = true;
            left_edge_y = py;
        } else {
            right_edge = true;
            right_edge_y = py;
        }
    }

    if (left_edge) {
        uint16_t px = 0;
        uint16_t py = (left_edge_y >= 0) ? (uint16_t)left_edge_y : (DISPLAY_H - LINE_HEIGHT) / 2;
        if (py > DISPLAY_H - LINE_HEIGHT) py = DISPLAY_H - LINE_HEIGHT;

        size_t line_bytes = LINE_HEIGHT * 2;
        uint8_t *payload = malloc(8 + line_bytes);
        if (payload) {
            payload[0] = (uint8_t)(px >> 8);
            payload[1] = (uint8_t)(px & 0xFF);
            payload[2] = (uint8_t)(py >> 8);
            payload[3] = (uint8_t)(py & 0xFF);
            payload[4] = 0;
            payload[5] = LINE_WIDTH;
            payload[6] = 2;
            payload[7] = HAND_COLOR;
            memset(payload + 8, 0xFF, line_bytes);
            spi_write(FPGA, 0x12, payload, 8 + line_bytes);
            free(payload);
        }
    }

    if (right_edge) {
        uint16_t px = DISPLAY_W - LINE_WIDTH;
        uint16_t py = (right_edge_y >= 0) ? (uint16_t)right_edge_y : (DISPLAY_H - LINE_HEIGHT) / 2;
        if (py > DISPLAY_H - LINE_HEIGHT) py = DISPLAY_H - LINE_HEIGHT;

        size_t line_bytes = LINE_HEIGHT * 2;
        uint8_t *payload = malloc(8 + line_bytes);
        if (payload) {
            payload[0] = (uint8_t)(px >> 8);
            payload[1] = (uint8_t)(px & 0xFF);
            payload[2] = (uint8_t)(py >> 8);
            payload[3] = (uint8_t)(py & 0xFF);
            payload[4] = 0;
            payload[5] = LINE_WIDTH;
            payload[6] = 2;
            payload[7] = HAND_COLOR;
            memset(payload + 8, 0xFF, line_bytes);
            spi_write(FPGA, 0x12, payload, 8 + line_bytes);
            free(payload);
        }
    }

    /* Swap frame buffers */
    spi_write(FPGA, 0x14, NULL, 0);
}

/*-----------------------------------------------*/
/* Camera helpers                                */
/*-----------------------------------------------*/

#ifndef DEV_KIT_BUILD

static void wake_camera(lua_State *L)
{
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "power_save");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, 0);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 3);
            return;
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

static int autoexpose(lua_State *L, int iterations)
{
    for (int i = 0; i < iterations; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            return -1;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            return -2;
        }
        lua_pop(L, 3);
        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }
    return 0;
}

static int trigger_capture(lua_State *L)
{
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        return -1;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "MEDIUM");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        lua_pop(L, 3);
        return -2;
    }
    lua_pop(L, 2);
    return 0;
}

static bool wait_image_ready(lua_State *L)
{
    uint32_t timeout = 1000000;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            return false;
        }
        ready = lua_toboolean(L, -1);
        lua_pop(L, 3);
        if (!ready) {
            nrfx_systick_delay_us(10);
        }
        reload_watchdog(NULL, NULL);
    }
    return ready;
}

/* Read the captured JPEG into jpeg_buffer (size MAX_JPEG_SIZE).
 * Returns the number of bytes read, or 0 on error / empty / overflow.
 * Diagnostic LOG lines distinguish the failure modes. */
static size_t read_jpeg_into(lua_State *L, uint8_t *jpeg_buffer)
{
    size_t jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            LOG("JPEG read: camera.read pcall failed at offset %u", (unsigned)jpeg_size);
            lua_pop(L, 3);
            return 0;
        }

        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
            break;
        }

        size_t chunk_len;
        const char *chunk = lua_tolstring(L, -1, &chunk_len);
        if (jpeg_size + chunk_len > MAX_JPEG_SIZE) {
            /* JPEG larger than buffer. Drain remaining bytes so the next
             * capture starts clean, then report. */
            LOG("JPEG read: oversize. read %u bytes, next chunk %u, MAX_JPEG_SIZE=%u",
                (unsigned)jpeg_size, (unsigned)chunk_len, (unsigned)MAX_JPEG_SIZE);
            lua_pop(L, 3);
            /* Drain */
            while (true) {
                lua_getglobal(L, "frame");
                lua_getfield(L, -1, "camera");
                lua_getfield(L, -1, "read");
                lua_pushinteger(L, READ_CHUNK_SIZE);
                if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                    lua_pop(L, 3);
                    break;
                }
                bool done = lua_isnil(L, -1);
                lua_pop(L, 3);
                if (done) break;
                reload_watchdog(NULL, NULL);
            }
            return 0;
        }
        memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
        jpeg_size += chunk_len;
        lua_pop(L, 3);
        reload_watchdog(NULL, NULL);
    }
    return jpeg_size;
}

#endif /* !DEV_KIT_BUILD */

/*-----------------------------------------------*/
/* Bluetooth send                                */
/*-----------------------------------------------*/

/* Block until the BT TX queue accepts the packet.
 *
 * bluetooth_send_data returns false on success and true on failure
 * (typically NRF_ERROR_RESOURCES = TX queue full). Without retry the
 * packet is silently dropped, which corrupts streaming transfers as soon
 * as the queue fills - the symptom is "image short by N bytes" with N
 * varying per frame. */
static void bt_send_blocking(const uint8_t *data, size_t length)
{
    /* Cap at ~500 ms total wait so a disconnected/stuck link doesn't
     * deadlock the experiment thread. */
    for (int retries = 0; retries < 250; retries++) {
        if (!bluetooth_send_data(data, length)) {
            return;  /* success */
        }
        nrfx_systick_delay_ms(2);
        reload_watchdog(NULL, NULL);
    }
    /* Drop and continue rather than block forever. */
}

/* Send the 64x64 RGB image (chunked) + separator + 8x8x2 prediction grid
 * (chunked) + end marker. Mirrors the FOMO_BEER_CAN protocol. */
static int send_image_and_predictions(const uint8_t *rgb_buffer,
                                       const int8_t *output_grid)
{
    uint8_t *chunk_buffer = malloc(BT_CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        return -1;
    }
    chunk_buffer[0] = 0x01;

    size_t offset = 0;
    while (offset < RGB_OUTPUT_BYTES) {
        size_t chunk = (RGB_OUTPUT_BYTES - offset > BT_CHUNK_SIZE)
                           ? BT_CHUNK_SIZE
                           : (RGB_OUTPUT_BYTES - offset);
        memcpy(chunk_buffer + 1, rgb_buffer + offset, chunk);
        bt_send_blocking(chunk_buffer, chunk + 1);
        offset += chunk;
        /* Throttle: NRF_SUCCESS only means "queued"; the radio still
         * needs time to actually transmit. Without this delay packets
         * pile up faster than the radio can drain them and get dropped
         * at the link layer, even though sd_ble_gatts_hvx accepted them.
         * Matches VWW_RGB's empirically-tuned cadence. */
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    nrfx_systick_delay_ms(50);
    uint8_t separator[3] = {0x01, 0xFE, 0xFE};
    bt_send_blocking(separator, 3);

    nrfx_systick_delay_ms(50);
    offset = 0;
    while (offset < FOMO_OUTPUT_SIZE) {
        size_t chunk = (FOMO_OUTPUT_SIZE - offset > BT_CHUNK_SIZE)
                           ? BT_CHUNK_SIZE
                           : (FOMO_OUTPUT_SIZE - offset);
        chunk_buffer[0] = 0x01;
        memcpy(chunk_buffer + 1, output_grid + offset, chunk);
        bt_send_blocking(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bt_send_blocking(end_marker, 5);

    free(chunk_buffer);
    return 0;
}

/*-----------------------------------------------*/
/* Acquire one RGB frame                         */
/*-----------------------------------------------*/

/* Capture one frame from the camera (or use DEV_KIT test data), decode it
 * to RGB and downscale into the static s_rgb_buffer.
 *
 * Memory pattern: only the JPEG buffer touches the heap (transient).
 * temp + rgb are static (BSS) so no fragmentation, no peak heap pressure
 * beyond MAX_JPEG_SIZE.
 *
 * Returns 0 on success, -1 on error (luaL_error already raised).
 * Result is in s_rgb_buffer (no caller-owned pointer).
 * skip_camera_setup=true is the "fast" path.
 */
static int acquire_rgb_frame(lua_State *L, bool skip_camera_setup)
{
    uint16_t actual_width, actual_height;
    int result;

#ifdef DEV_KIT_BUILD
    (void)skip_camera_setup;
    LOG("DEV_KIT hand: Using hardcoded test JPEG data (%d bytes)", (int)test_jpeg_size);

    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                    s_temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);
    if (result != 0) {
        luaL_error(L, "RGB decode failed: %d", result);
        return -1;
    }
    LOG("DEV_KIT hand: decoded %dx%d", actual_width, actual_height);

    reload_watchdog(NULL, NULL);
    downscale_90_to_64_rgb_with_rotation(s_temp_buffer, s_rgb_buffer);
    return 0;

#else
    /* Camera path */
    if (!skip_camera_setup) {
        wake_camera(L);
        nrfx_systick_delay_ms(100);
        if (autoexpose(L, 5) != 0) {
            luaL_error(L, "camera.auto failed");
            return -1;
        }
    }

    if (trigger_capture(L) != 0) {
        luaL_error(L, "camera.capture failed");
        return -1;
    }

    if (!wait_image_ready(L)) {
        luaL_error(L, "capture timeout");
        return -1;
    }

    /* Read JPEG straight into s_rgb_buffer (scratch reuse - it will be
     * overwritten by the downscale step below). No heap allocation. */
    size_t jpeg_size = read_jpeg_into(L, s_rgb_buffer);
    if (jpeg_size == 0) {
        luaL_error(L, "JPEG read failed or empty");
        return -1;
    }

    LOG("Hand detect: JPEG size = %d bytes", (int)jpeg_size);

    /* Decode jpeg (in s_rgb_buffer) -> s_temp_buffer */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(s_rgb_buffer, jpeg_size,
                                    s_temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);
    if (result != 0) {
        luaL_error(L, "RGB decode failed: %d", result);
        return -1;
    }

    /* Upscale s_temp_buffer -> s_rgb_buffer (overwrites the consumed JPEG bytes) */
    reload_watchdog(NULL, NULL);
    downscale_90_to_64_rgb_with_rotation(s_temp_buffer, s_rgb_buffer);
    return 0;
#endif
}

/*-----------------------------------------------*/
/* Hand Detection Lua Functions                  */
/*-----------------------------------------------*/

/* Common body for the single-shot and fast-path hand-detection functions. */
static int run_hand_detection_common(lua_State *L, bool skip_camera_setup)
{
    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }

    /* Result lands in s_rgb_buffer (BSS, no caller ownership). */
    if (acquire_rgb_frame(L, skip_camera_setup) != 0) {
        /* luaL_error already raised */
        return 0;
    }

    /* Inference (no extra heap; uses the static TFLM tensor arena). */
    reload_watchdog(NULL, NULL);
    int8_t output_grid[FOMO_OUTPUT_SIZE];
    tflm_status_t infer_status = fomo_infer(s_rgb_buffer, output_grid);
    if (infer_status != TFLM_OK) {
        luaL_error(L, "FOMO hand inference failed");
        return 0;
    }

    LOG("Hand detect inference complete");

    build_cubes(output_grid);
    draw_hand_detection_overlay(output_grid);
    reload_watchdog(NULL, NULL);

    if (send_image_and_predictions(s_rgb_buffer, output_grid) != 0) {
        int free_b, frags;
        heap_stats(&free_b, &frags);
        luaL_error(L, "BT chunk allocation failed (heap free=%d, frags=%d)",
                   free_b, frags);
        return 0;
    }

    lua_pushinteger(L, RGB_OUTPUT_BYTES + FOMO_OUTPUT_SIZE);
    return 1;
}

/**
 * Run FOMO hand detection on camera image (full pipeline including camera
 * wake + auto-exposure each call).
 *
 * Bluetooth protocol (matches FOMO_BEER_CAN format):
 *   [IMAGE DATA]   12288 bytes (64x64x3 RGB), 200-byte chunks each prefixed 0x01
 *   [SEPARATOR]    0x01 0xFE 0xFE
 *   [PREDICTIONS]  128 bytes (8x8x2 int8 grid), 200-byte chunks each prefixed 0x01
 *   [END MARKER]   0x01 0xFF 0xFF 0x00 0x00
 */
static int lua_experiment_run_hand_detection(lua_State *L)
{
    return run_hand_detection_common(L, /* skip_camera_setup */ false);
}

/**
 * Fast hand detection - skips camera wake and auto-exposure.
 * Caller must ensure camera is awake and auto-adjusted before calling.
 */
static int lua_experiment_run_hand_detection_fast(lua_State *L)
{
    return run_hand_detection_common(L, /* skip_camera_setup */ true);
}

/**
 * Run hand detection benchmark - looped local inference, no Bluetooth in loop.
 * Lua: result = frame.experiment.run_hand_detection_benchmark(iterations)
 *
 * Memory pattern: temp + rgb buffers persist across iterations in BSS.
 * No heap allocation in the loop.
 *
 * Returns table with: iterations, hand_detections, total_time_ms,
 * avg_time_ms, and per-stage breakdown in ms.
 */
static int lua_experiment_run_hand_detection_benchmark(lua_State *L)
{
    uint16_t actual_width, actual_height;
    int result;

    lua_Integer iterations = luaL_checkinteger(L, 1);
    if (iterations < 1 || iterations > 1000) {
        luaL_error(L, "iterations must be between 1 and 1000");
        return 0;
    }

    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }

    /* temp + rgb live in BSS (s_temp_buffer, s_rgb_buffer) — no heap
     * allocation needed. Only the per-iteration JPEG buffer touches the
     * heap. */

#ifndef DEV_KIT_BUILD
    /* Initialize camera ONCE before the benchmark loop */
    wake_camera(L);
    nrfx_systick_delay_ms(100);
    if (autoexpose(L, 5) != 0) {
        luaL_error(L, "camera.auto failed");
        return 0;
    }
    LOG("Hand benchmark: camera initialized");
#endif

    int hand_count = 0;

    /* Enable DWT cycle counter */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t time_capture = 0;
    uint32_t time_wait_ready = 0;
    uint32_t time_read_jpeg = 0;
    uint32_t time_decode = 0;
    uint32_t time_upscale = 0;
    uint32_t time_inference = 0;
    uint32_t time_display = 0;
    uint32_t t0, t1;

    uint32_t start_cycles = DWT->CYCCNT;

    for (int iter = 0; iter < iterations; iter++) {
        reload_watchdog(NULL, NULL);

#ifdef DEV_KIT_BUILD
        t0 = DWT->CYCCNT;
        result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                        s_temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                        &actual_width, &actual_height,
                                        3, false);
        if (result != 0) {
            luaL_error(L, "RGB decode failed: %d", result);
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_decode += (t1 - t0);
#else
        t0 = DWT->CYCCNT;
        if (trigger_capture(L) != 0) {
            luaL_error(L, "camera.capture failed");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_capture += (t1 - t0);

        t0 = DWT->CYCCNT;
        if (!wait_image_ready(L)) {
            luaL_error(L, "capture timeout");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_wait_ready += (t1 - t0);

        /* JPEG read into s_rgb_buffer (scratch reuse before upscale) */
        t0 = DWT->CYCCNT;
        size_t jpeg_size = read_jpeg_into(L, s_rgb_buffer);
        if (jpeg_size == 0) {
            luaL_error(L, "JPEG read failed");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_read_jpeg += (t1 - t0);

        t0 = DWT->CYCCNT;
        reload_watchdog(NULL, NULL);
        result = jpeg_decode_rgb_scaled(s_rgb_buffer, jpeg_size,
                                        s_temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                        &actual_width, &actual_height,
                                        3, false);
        if (result != 0) {
            luaL_error(L, "RGB decode failed: %d", result);
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_decode += (t1 - t0);
#endif

        t0 = DWT->CYCCNT;
        reload_watchdog(NULL, NULL);
        downscale_90_to_64_rgb_with_rotation(s_temp_buffer, s_rgb_buffer);
        t1 = DWT->CYCCNT;
        time_upscale += (t1 - t0);

        t0 = DWT->CYCCNT;
        reload_watchdog(NULL, NULL);
        int8_t output_grid[FOMO_OUTPUT_SIZE];
        tflm_status_t infer_status = fomo_infer(s_rgb_buffer, output_grid);
        if (infer_status != TFLM_OK) {
            luaL_error(L, "FOMO hand inference failed");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_inference += (t1 - t0);

        bool any_hand = false;
        for (int i = 0; i < FOMO_GRID_SIZE * FOMO_GRID_SIZE; i++) {
            if (output_grid[i * FOMO_NUM_CLASSES + 1] > -50) {
                any_hand = true;
                break;
            }
        }
        if (any_hand) hand_count++;

        t0 = DWT->CYCCNT;
        build_cubes(output_grid);
        draw_hand_detection_overlay(output_grid);
        reload_watchdog(NULL, NULL);
        t1 = DWT->CYCCNT;
        time_display += (t1 - t0);

        if ((iter + 1) % 10 == 0) {
            LOG("Hand benchmark: %d/%d iterations", iter + 1, (int)iterations);
        }
    }

    uint32_t end_cycles = DWT->CYCCNT;

    /* CPU runs at 64 MHz */
    uint32_t elapsed_cycles = end_cycles - start_cycles;
    uint32_t total_ms = elapsed_cycles / 64000;
    uint32_t avg_ms = total_ms / (uint32_t)iterations;

    uint32_t capture_ms = time_capture / 64000;
    uint32_t wait_ready_ms = time_wait_ready / 64000;
    uint32_t read_jpeg_ms = time_read_jpeg / 64000;
    uint32_t decode_ms = time_decode / 64000;
    uint32_t upscale_ms = time_upscale / 64000;
    uint32_t inference_ms = time_inference / 64000;
    uint32_t display_ms = time_display / 64000;

    LOG("Hand benchmark complete: %d iterations, %d frames with hand, %lu ms total, %lu ms avg",
        (int)iterations, hand_count, total_ms, avg_ms);
    LOG("Timing breakdown (ms): capture=%lu wait=%lu read=%lu decode=%lu upscale=%lu infer=%lu display=%lu",
        capture_ms, wait_ready_ms, read_jpeg_ms, decode_ms, upscale_ms, inference_ms, display_ms);

    lua_newtable(L);
    lua_pushinteger(L, iterations);
    lua_setfield(L, -2, "iterations");
    lua_pushinteger(L, hand_count);
    lua_setfield(L, -2, "hand_detections");
    lua_pushinteger(L, total_ms);
    lua_setfield(L, -2, "total_time_ms");
    lua_pushinteger(L, avg_ms);
    lua_setfield(L, -2, "avg_time_ms");

    lua_pushinteger(L, capture_ms);
    lua_setfield(L, -2, "capture_ms");
    lua_pushinteger(L, wait_ready_ms);
    lua_setfield(L, -2, "wait_ready_ms");
    lua_pushinteger(L, read_jpeg_ms);
    lua_setfield(L, -2, "read_jpeg_ms");
    lua_pushinteger(L, decode_ms);
    lua_setfield(L, -2, "decode_ms");
    lua_pushinteger(L, upscale_ms);
    lua_setfield(L, -2, "upscale_ms");
    lua_pushinteger(L, inference_ms);
    lua_setfield(L, -2, "inference_ms");
    lua_pushinteger(L, display_ms);
    lua_setfield(L, -2, "display_ms");

    return 1;
}

/**
 * Set the FOMO detection threshold in float (post-softmax) space.
 * Lua: frame.experiment.set_threshold(0.3)
 * EI default = 0.5; lower values give more recall but also more false
 * positives. Use the LOG line "FOMO heatmap: max=X" to pick a sensible value.
 */
static int lua_experiment_set_threshold(lua_State *L)
{
    lua_Number t = luaL_checknumber(L, 1);
    if (t < 0.0 || t > 1.0) {
        luaL_error(L, "threshold must be in [0.0, 1.0]");
        return 0;
    }
    s_threshold = (float)t;
    LOG("FOMO threshold set to %.3f", (double)s_threshold);
    return 0;
}

/*-----------------------------------------------*/
/* Experiment Interface Implementation           */
/*-----------------------------------------------*/

const char* experiment_get_name(void)
{
    return "FOMO_HAND_DETECTION";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    lua_pushcfunction(L, lua_experiment_run_hand_detection);
    lua_setfield(L, -2, "run_hand_detection");

    lua_pushcfunction(L, lua_experiment_run_hand_detection_fast);
    lua_setfield(L, -2, "run_hand_detection_fast");

    lua_pushcfunction(L, lua_experiment_run_hand_detection_benchmark);
    lua_setfield(L, -2, "run_hand_detection_benchmark");

    lua_pushcfunction(L, lua_experiment_set_threshold);
    lua_setfield(L, -2, "set_threshold");
}
