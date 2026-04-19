/*
 * FOMO Hand Detection Experiment Implementation
 *
 * Object detection experiment using a FOMO model trained for hand detection.
 * Input: 96x96 RGB. Output: 8x8x2 (background, hand).
 * Compiled when ML_EXPERIMENT=FOMO_HAND_DETECTION is set.
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
#define CAPTURE_SIZE     720
#define SCALED_SIZE      90    /* After TJpgDec scale=3 (720/8=90) */
#define OUT_DIM          96    /* ML input dimension (96x96) */
#define NUM_CHANNELS     3     /* RGB */

#define RGB_CAPTURE_BYTES  (SCALED_SIZE * SCALED_SIZE * NUM_CHANNELS)  /* 24,300 */
#define RGB_OUTPUT_BYTES   (OUT_DIM * OUT_DIM * NUM_CHANNELS)          /* 27,648 */

/*-----------------------------------------------*/
/* Hand Detection Overlay                        */
/*-----------------------------------------------*/

/**
 * Draw detection overlay on Frame display after FOMO inference.
 * Single class: hand (drawn as a green dot per detected grid cell).
 * Edge indicators (left/right vertical bars) for detections outside the
 * display FOV.
 *
 * @param output_grid Pointer to FOMO_OUTPUT_SIZE int8 output grid (8x8x2)
 */
static void draw_hand_detection_overlay(const int8_t *output_grid)
{
    const int8_t DETECTION_THRESHOLD = -50;

    /* Display dimensions */
    const uint16_t DISPLAY_W = 640;
    const uint16_t DISPLAY_H = 400;

    /*
     * FOV mapping: Camera sees ~56, display shows ~20 (center portion).
     * The 8x8 grid covers the full camera FOV.
     * Display FOV corresponds to roughly the center 4 cells.
     */
    const int CENTER_START = 2;
    const int CENTER_END = 5;
    const int CENTER_COLS = CENTER_END - CENTER_START + 1;

    const uint16_t CELL_HEIGHT = DISPLAY_H / FOMO_GRID_SIZE;

    const uint16_t SPRITE_SIZE = 24;

    /* Edge indicator dimensions */
    const uint16_t LINE_WIDTH = 12;
    const uint16_t LINE_HEIGHT = 80;

    /* Palette color for hand detection */
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

    for (int gy = 0; gy < FOMO_GRID_SIZE; gy++) {
        for (int gx = 0; gx < FOMO_GRID_SIZE; gx++) {
            int idx = (gy * FOMO_GRID_SIZE + gx) * FOMO_NUM_CLASSES;

            /* Class 0 = background, class 1 = hand */
            int8_t hand_score = output_grid[idx + 1];

            if (hand_score <= DETECTION_THRESHOLD) {
                continue;
            }

            int16_t py = gy * CELL_HEIGHT + (CELL_HEIGHT - SPRITE_SIZE) / 2;
            if (py < 0) py = 0;
            if (py > DISPLAY_H - SPRITE_SIZE) py = DISPLAY_H - SPRITE_SIZE;

            if (gx >= CENTER_START && gx <= CENTER_END) {
                int rel_x = gx - CENTER_START;
                uint16_t cell_width = DISPLAY_W / CENTER_COLS;
                int16_t px = rel_x * cell_width + (cell_width - SPRITE_SIZE) / 2;

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
            } else if (gx < CENTER_START) {
                left_edge = true;
                left_edge_y = py;
            } else {
                right_edge = true;
                right_edge_y = py;
            }
        }
    }

    if (left_edge) {
        uint16_t px = 0;
        uint16_t py = (left_edge_y >= 0) ? (uint16_t)left_edge_y : (DISPLAY_H - LINE_HEIGHT) / 2;
        if (py > DISPLAY_H - LINE_HEIGHT) py = DISPLAY_H - LINE_HEIGHT;

        size_t line_bytes = LINE_HEIGHT * 2;
        uint8_t *line_data = malloc(line_bytes);
        if (line_data) {
            memset(line_data, 0xFF, line_bytes);

            uint8_t meta[8] = {
                (uint8_t)(px >> 8), (uint8_t)(px & 0xFF),
                (uint8_t)(py >> 8), (uint8_t)(py & 0xFF),
                0, LINE_WIDTH,
                2,
                HAND_COLOR
            };

            uint8_t *payload = malloc(8 + line_bytes);
            if (payload) {
                memcpy(payload, meta, 8);
                memcpy(payload + 8, line_data, line_bytes);
                spi_write(FPGA, 0x12, payload, 8 + line_bytes);
                free(payload);
            }
            free(line_data);
        }
    }

    if (right_edge) {
        uint16_t px = DISPLAY_W - LINE_WIDTH;
        uint16_t py = (right_edge_y >= 0) ? (uint16_t)right_edge_y : (DISPLAY_H - LINE_HEIGHT) / 2;
        if (py > DISPLAY_H - LINE_HEIGHT) py = DISPLAY_H - LINE_HEIGHT;

        size_t line_bytes = LINE_HEIGHT * 2;
        uint8_t *line_data = malloc(line_bytes);
        if (line_data) {
            memset(line_data, 0xFF, line_bytes);

            uint8_t meta[8] = {
                (uint8_t)(px >> 8), (uint8_t)(px & 0xFF),
                (uint8_t)(py >> 8), (uint8_t)(py & 0xFF),
                0, LINE_WIDTH,
                2,
                HAND_COLOR
            };

            uint8_t *payload = malloc(8 + line_bytes);
            if (payload) {
                memcpy(payload, meta, 8);
                memcpy(payload + 8, line_data, line_bytes);
                spi_write(FPGA, 0x12, payload, 8 + line_bytes);
                free(payload);
            }
            free(line_data);
        }
    }

    /* Swap frame buffers */
    spi_write(FPGA, 0x14, NULL, 0);
}

/*-----------------------------------------------*/
/* Hand Detection Lua Functions                  */
/*-----------------------------------------------*/

/**
 * Run FOMO hand detection on camera image.
 *
 * Image pipeline: 720x720 JPEG -> scale=3 (90x90 RGB) -> upscale to 96x96 RGB
 * Inference: fomo_infer(rgb, 8x8x2 grid)
 * Display: green dots for detected hand cells (with edge indicators)
 *
 * Bluetooth protocol (matches FOMO_BEER_CAN format):
 *   [IMAGE DATA]   27648 bytes (96x96x3 RGB), 200-byte chunks each prefixed 0x01
 *   [SEPARATOR]    0x01 0xFE 0xFE
 *   [PREDICTIONS]  128 bytes (8x8x2 int8 grid), 200-byte chunks each prefixed 0x01
 *   [END MARKER]   0x01 0xFF 0xFF 0x00 0x00
 */
static int lua_experiment_run_hand_detection(lua_State *L)
{
    const size_t MAX_JPEG_SIZE = 25 * 1024;
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }

    uint8_t *jpeg_buffer = malloc(MAX_JPEG_SIZE);
    uint8_t *temp_buffer = malloc(RGB_CAPTURE_BYTES);
    uint8_t *rgb_buffer = malloc(RGB_OUTPUT_BYTES);
    if (!jpeg_buffer || !temp_buffer || !rgb_buffer) {
        if (jpeg_buffer) free(jpeg_buffer);
        if (temp_buffer) free(temp_buffer);
        if (rgb_buffer) free(rgb_buffer);
        luaL_error(L, "allocation failed");
        return 0;
    }

    memset(temp_buffer, 0, RGB_CAPTURE_BYTES);
    memset(rgb_buffer, 0, RGB_OUTPUT_BYTES);

#ifdef DEV_KIT_BUILD
    free(jpeg_buffer);
    LOG("DEV_KIT hand: Using hardcoded test JPEG data (%u bytes)", test_jpeg_size);

    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);
    if (result != 0) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }
    LOG("DEV_KIT hand: decoded %dx%d", actual_width, actual_height);
#else
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    /* Wake up camera */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "power_save");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, 0);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 3);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    nrfx_systick_delay_ms(100);

    /* Auto-adjust */
    for (int i = 0; i < 5; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "camera.auto not found");
            return 0;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "camera.auto failed");
            return 0;
        }
        lua_pop(L, 3);
        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }

    /* Capture */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        free(jpeg_buffer);
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "camera.capture not found");
        return 0;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "MEDIUM");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        lua_pop(L, 3);
        free(jpeg_buffer);
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "capture failed");
        return 0;
    }
    lua_pop(L, 2);

    /* Wait for image ready */
    uint32_t timeout = 1000000;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "image_ready failed");
            return 0;
        }
        ready = lua_toboolean(L, -1);
        lua_pop(L, 3);
        if (!ready) {
            nrfx_systick_delay_us(10);
        }
        reload_watchdog(NULL, NULL);
    }

    if (!ready) {
        free(jpeg_buffer);
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "capture timeout");
        return 0;
    }

    /* Read JPEG */
    jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "read failed");
            return 0;
        }

        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
            break;
        }

        size_t chunk_len;
        const char *chunk = lua_tolstring(L, -1, &chunk_len);
        memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
        jpeg_size += chunk_len;
        lua_pop(L, 3);
        reload_watchdog(NULL, NULL);
    }

    LOG("Hand detect: JPEG size = %u bytes", jpeg_size);

    /* Decode JPEG to 90x90 RGB */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);
    free(jpeg_buffer);

    if (result != 0) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }
#endif

    /* Upscale 90x90 RGB to 96x96 RGB with 90 CCW rotation */
    reload_watchdog(NULL, NULL);
    upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);
    free(temp_buffer);

    /* Run FOMO inference */
    reload_watchdog(NULL, NULL);
    int8_t output_grid[FOMO_OUTPUT_SIZE];
    tflm_status_t infer_status = fomo_infer(rgb_buffer, output_grid);
    if (infer_status != TFLM_OK) {
        free(rgb_buffer);
        luaL_error(L, "FOMO hand inference failed");
        return 0;
    }

    LOG("Hand detect inference complete");

    /* Display overlay (green dots for hands) */
    draw_hand_detection_overlay(output_grid);
    reload_watchdog(NULL, NULL);

    /* Send RGB image data via Bluetooth */
    size_t total_bytes = RGB_OUTPUT_BYTES;
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        free(rgb_buffer);
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, rgb_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    free(rgb_buffer);

    /* Separator */
    nrfx_systick_delay_ms(50);
    uint8_t separator[3] = {0x01, 0xFE, 0xFE};
    bluetooth_send_data(separator, 3);

    /* Predictions */
    nrfx_systick_delay_ms(50);
    offset = 0;
    while (offset < FOMO_OUTPUT_SIZE) {
        size_t chunk = (FOMO_OUTPUT_SIZE - offset > CHUNK_SIZE) ? CHUNK_SIZE : (FOMO_OUTPUT_SIZE - offset);
        chunk_buffer[0] = 0x01;
        memcpy(chunk_buffer + 1, output_grid + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    /* End marker */
    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bluetooth_send_data(end_marker, 5);

    free(chunk_buffer);

    lua_pushinteger(L, total_bytes + FOMO_OUTPUT_SIZE);
    return 1;
}

/**
 * Fast hand detection - skips camera wake and autoexposure.
 * Caller must ensure camera is awake and auto-adjusted before calling.
 * Used by the streaming test to keep per-frame latency low.
 */
static int lua_experiment_run_hand_detection_fast(lua_State *L)
{
    const size_t MAX_JPEG_SIZE = 25 * 1024;
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }

    uint8_t *jpeg_buffer = malloc(MAX_JPEG_SIZE);
    uint8_t *temp_buffer = malloc(RGB_CAPTURE_BYTES);
    uint8_t *rgb_buffer = malloc(RGB_OUTPUT_BYTES);
    if (!jpeg_buffer || !temp_buffer || !rgb_buffer) {
        if (jpeg_buffer) free(jpeg_buffer);
        if (temp_buffer) free(temp_buffer);
        if (rgb_buffer) free(rgb_buffer);
        luaL_error(L, "allocation failed");
        return 0;
    }

    memset(temp_buffer, 0, RGB_CAPTURE_BYTES);
    memset(rgb_buffer, 0, RGB_OUTPUT_BYTES);

#ifdef DEV_KIT_BUILD
    free(jpeg_buffer);
    result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);
    if (result != 0) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }
#else
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    /* Capture - assumes camera is already awake */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        free(jpeg_buffer);
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "camera.capture not found");
        return 0;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "MEDIUM");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        lua_pop(L, 3);
        free(jpeg_buffer);
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "capture failed");
        return 0;
    }
    lua_pop(L, 2);

    uint32_t timeout = 1000000;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "image_ready failed");
            return 0;
        }
        ready = lua_toboolean(L, -1);
        lua_pop(L, 3);
        if (!ready) {
            nrfx_systick_delay_us(10);
        }
        reload_watchdog(NULL, NULL);
    }

    if (!ready) {
        free(jpeg_buffer);
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "capture timeout");
        return 0;
    }

    jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "read failed");
            return 0;
        }

        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
            break;
        }

        size_t chunk_len;
        const char *chunk = lua_tolstring(L, -1, &chunk_len);
        memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
        jpeg_size += chunk_len;
        lua_pop(L, 3);
        reload_watchdog(NULL, NULL);
    }

    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);
    free(jpeg_buffer);

    if (result != 0) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }
#endif

    reload_watchdog(NULL, NULL);
    upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);
    free(temp_buffer);

    reload_watchdog(NULL, NULL);
    int8_t output_grid[FOMO_OUTPUT_SIZE];
    tflm_status_t infer_status = fomo_infer(rgb_buffer, output_grid);
    if (infer_status != TFLM_OK) {
        free(rgb_buffer);
        luaL_error(L, "FOMO hand inference failed");
        return 0;
    }

    draw_hand_detection_overlay(output_grid);
    reload_watchdog(NULL, NULL);

    size_t total_bytes = RGB_OUTPUT_BYTES;
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        free(rgb_buffer);
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, rgb_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    free(rgb_buffer);

    nrfx_systick_delay_ms(50);
    uint8_t separator[3] = {0x01, 0xFE, 0xFE};
    bluetooth_send_data(separator, 3);

    nrfx_systick_delay_ms(50);
    offset = 0;
    while (offset < FOMO_OUTPUT_SIZE) {
        size_t chunk = (FOMO_OUTPUT_SIZE - offset > CHUNK_SIZE) ? CHUNK_SIZE : (FOMO_OUTPUT_SIZE - offset);
        chunk_buffer[0] = 0x01;
        memcpy(chunk_buffer + 1, output_grid + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bluetooth_send_data(end_marker, 5);

    free(chunk_buffer);

    lua_pushinteger(L, total_bytes + FOMO_OUTPUT_SIZE);
    return 1;
}

/**
 * Run hand detection benchmark - looped local inference, no Bluetooth in loop.
 * Lua: result = frame.experiment.run_hand_detection_benchmark(iterations)
 * Returns table with iterations, total_time_ms, avg_time_ms,
 * and per-stage breakdown (capture/wait/read/decode/upscale/inference/display ms).
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

    uint8_t *temp_buffer = malloc(RGB_CAPTURE_BYTES);
    uint8_t *rgb_buffer = malloc(RGB_OUTPUT_BYTES);
    if (!temp_buffer || !rgb_buffer) {
        if (temp_buffer) free(temp_buffer);
        if (rgb_buffer) free(rgb_buffer);
        luaL_error(L, "allocation failed");
        return 0;
    }

#ifndef DEV_KIT_BUILD
    const size_t MAX_JPEG_SIZE = 25 * 1024;
    const size_t READ_CHUNK_SIZE = 512;
    size_t jpeg_size = 0;

    uint8_t *jpeg_buffer = malloc(MAX_JPEG_SIZE);
    if (!jpeg_buffer) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "JPEG allocation failed");
        return 0;
    }

    /* Initialize camera ONCE before the benchmark loop */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "power_save");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, 0);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 3);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    nrfx_systick_delay_ms(100);

    for (int i = 0; i < 5; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "camera.auto not found");
            return 0;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "camera.auto failed");
            return 0;
        }
        lua_pop(L, 3);
        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }
    LOG("Hand benchmark: camera initialized");
#endif

    int hand_count = 0;

    /* Enable DWT cycle counter */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    uint32_t time_memset = 0;
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

        t0 = DWT->CYCCNT;
        memset(temp_buffer, 0, RGB_CAPTURE_BYTES);
        memset(rgb_buffer, 0, RGB_OUTPUT_BYTES);
        t1 = DWT->CYCCNT;
        time_memset += (t1 - t0);

#ifdef DEV_KIT_BUILD
        t0 = DWT->CYCCNT;
        result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                        temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                        &actual_width, &actual_height,
                                        3, false);
        if (result != 0) {
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "RGB decode failed: %d", result);
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_decode += (t1 - t0);
#else
        memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

        t0 = DWT->CYCCNT;
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "capture");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "camera.capture not found");
            return 0;
        }
        lua_newtable(L);
        lua_pushinteger(L, CAPTURE_SIZE);
        lua_setfield(L, -2, "resolution");
        lua_pushstring(L, "MEDIUM");
        lua_setfield(L, -2, "quality");
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 3);
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "capture failed");
            return 0;
        }
        lua_pop(L, 2);
        t1 = DWT->CYCCNT;
        time_capture += (t1 - t0);

        t0 = DWT->CYCCNT;
        uint32_t timeout = 1000000;
        bool ready = false;
        while (timeout-- && !ready) {
            lua_getglobal(L, "frame");
            lua_getfield(L, -1, "camera");
            lua_getfield(L, -1, "image_ready");
            if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
                lua_pop(L, 3);
                free(jpeg_buffer);
                free(temp_buffer);
                free(rgb_buffer);
                luaL_error(L, "image_ready failed");
                return 0;
            }
            ready = lua_toboolean(L, -1);
            lua_pop(L, 3);
            if (!ready) {
                nrfx_systick_delay_us(10);
            }
            reload_watchdog(NULL, NULL);
        }

        if (!ready) {
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "capture timeout");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_wait_ready += (t1 - t0);

        t0 = DWT->CYCCNT;
        jpeg_size = 0;
        while (jpeg_size < MAX_JPEG_SIZE) {
            lua_getglobal(L, "frame");
            lua_getfield(L, -1, "camera");
            lua_getfield(L, -1, "read");
            lua_pushinteger(L, READ_CHUNK_SIZE);
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                lua_pop(L, 3);
                free(jpeg_buffer);
                free(temp_buffer);
                free(rgb_buffer);
                luaL_error(L, "read failed");
                return 0;
            }

            if (lua_isnil(L, -1)) {
                lua_pop(L, 3);
                break;
            }

            size_t chunk_len;
            const char *chunk = lua_tolstring(L, -1, &chunk_len);
            memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
            jpeg_size += chunk_len;
            lua_pop(L, 3);
            reload_watchdog(NULL, NULL);
        }
        t1 = DWT->CYCCNT;
        time_read_jpeg += (t1 - t0);

        t0 = DWT->CYCCNT;
        reload_watchdog(NULL, NULL);
        result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                        temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                        &actual_width, &actual_height,
                                        3, false);
        if (result != 0) {
            free(jpeg_buffer);
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "RGB decode failed: %d", result);
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_decode += (t1 - t0);
#endif

        t0 = DWT->CYCCNT;
        reload_watchdog(NULL, NULL);
        upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);
        t1 = DWT->CYCCNT;
        time_upscale += (t1 - t0);

        t0 = DWT->CYCCNT;
        reload_watchdog(NULL, NULL);
        int8_t output_grid[FOMO_OUTPUT_SIZE];
        tflm_status_t infer_status = fomo_infer(rgb_buffer, output_grid);
        if (infer_status != TFLM_OK) {
            free(temp_buffer);
            free(rgb_buffer);
#ifndef DEV_KIT_BUILD
            free(jpeg_buffer);
#endif
            luaL_error(L, "FOMO hand inference failed");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_inference += (t1 - t0);

        /* Count cells with hand detected (class 1 above threshold) */
        bool any_hand = false;
        for (int i = 0; i < FOMO_GRID_SIZE * FOMO_GRID_SIZE; i++) {
            if (output_grid[i * FOMO_NUM_CLASSES + 1] > -50) {
                any_hand = true;
                break;
            }
        }
        if (any_hand) hand_count++;

        t0 = DWT->CYCCNT;
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

    free(temp_buffer);
    free(rgb_buffer);
#ifndef DEV_KIT_BUILD
    free(jpeg_buffer);
#endif

    uint32_t memset_ms = time_memset / 64000;
    uint32_t capture_ms = time_capture / 64000;
    uint32_t wait_ready_ms = time_wait_ready / 64000;
    uint32_t read_jpeg_ms = time_read_jpeg / 64000;
    uint32_t decode_ms = time_decode / 64000;
    uint32_t upscale_ms = time_upscale / 64000;
    uint32_t inference_ms = time_inference / 64000;
    uint32_t display_ms = time_display / 64000;

    LOG("Hand benchmark complete: %d iterations, %d frames with hand, %lu ms total, %lu ms avg",
        (int)iterations, hand_count, total_ms, avg_ms);
    LOG("Timing breakdown (ms): memset=%lu capture=%lu wait=%lu read=%lu decode=%lu upscale=%lu infer=%lu display=%lu",
        memset_ms, capture_ms, wait_ready_ms, read_jpeg_ms, decode_ms, upscale_ms, inference_ms, display_ms);

    lua_newtable(L);
    lua_pushinteger(L, iterations);
    lua_setfield(L, -2, "iterations");
    lua_pushinteger(L, hand_count);
    lua_setfield(L, -2, "hand_detections");
    lua_pushinteger(L, total_ms);
    lua_setfield(L, -2, "total_time_ms");
    lua_pushinteger(L, avg_ms);
    lua_setfield(L, -2, "avg_time_ms");

    lua_pushinteger(L, memset_ms);
    lua_setfield(L, -2, "memset_ms");
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
}
