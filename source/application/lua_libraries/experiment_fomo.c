/*
 * FOMO Beer Can Experiment Implementation
 *
 * Object detection experiment using FOMO model for beer can detection.
 * This file is compiled when ML_EXPERIMENT=FOMO_BEER_CAN is set.
 */

#include <string.h>
#include <stdlib.h>
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
#define OUTPUT_SIZE       64    /* FOMO model input is 64x64 grayscale */

/* Buffer sizes */
#define GRAY_TEMP_BYTES   (SCALED_SIZE * SCALED_SIZE)         /* 8100 */
#define GRAY_INPUT_BYTES  (OUTPUT_SIZE * OUTPUT_SIZE)         /* 4096 */
#define MAX_JPEG_SIZE     (25 * 1024)

/* Static working buffers in BSS. Rationale (mirrors experiment_vww_rgb.c):
 *   - Per-call malloc/free of buffers this large fragments the heap fast,
 *     causing later allocations to fail mid-stream.
 *   - Static placement is contiguous at link time, no allocator
 *     involvement, no fragmentation.
 *   - The three Lua entry points below are mutually exclusive (only one
 *     runs at a time) so they share these buffers. */
static uint8_t s_jpeg_buffer[MAX_JPEG_SIZE];     /* 25 KB raw JPEG */
static uint8_t s_temp_buffer[GRAY_TEMP_BYTES];   /* 8.1 KB decoded 90x90 */
static uint8_t s_gray_buffer[GRAY_INPUT_BYTES];  /* 4 KB final 64x64 */

/* Pure-inference benchmark constants and state. Same scheme as
 * experiment_vww{,_rgb}.c so the host-side Python script and CSV
 * format are unchanged. */
#define BENCH_K       8
#define BENCH_N       64
#define BENCH_WARMUP  10

/* Wire payload for one cycle: 4-byte LE cycle index followed by N raw
 * DWT cycle counts (uint32 LE). */
static struct {
    uint32_t cycle_idx;
    uint32_t cycles[BENCH_N];
} s_bench_payload;

/*-----------------------------------------------*/
/* FOMO Detection Overlay                        */
/*-----------------------------------------------*/

/**
 * Draw detection overlay on Frame display after FOMO inference
 * Maps camera FOV to display FOV with edge indicators for peripheral detections.
 *
 * @param output_grid Pointer to 192-byte int8 output grid (8x8x3)
 */
static void draw_detection_overlay(const int8_t *output_grid)
{
    const int8_t DETECTION_THRESHOLD = -50;

    /* Display dimensions */
    const uint16_t DISPLAY_W = 640;
    const uint16_t DISPLAY_H = 400;

    /*
     * FOV mapping: Camera sees ~56, display shows ~20 (center portion)
     * The 8x8 grid covers the full camera FOV.
     * Display FOV corresponds to roughly the center 2-3 cells.
     */
    const int CENTER_START = 2;  /* First grid column shown on display */
    const int CENTER_END = 5;    /* Last grid column shown on display */
    const int CENTER_COLS = CENTER_END - CENTER_START + 1;  /* 4 columns */

    /* Vertical: use full grid since display vertical FOV is similar */
    const uint16_t CELL_HEIGHT = DISPLAY_H / FOMO_GRID_SIZE;  /* 50 px per cell */

    /* Sprite size */
    const uint16_t SPRITE_SIZE = 24;

    /* Edge indicator dimensions */
    const uint16_t LINE_WIDTH = 12;
    const uint16_t LINE_HEIGHT = 80;

    /* Palette color indices */
    const uint8_t BEER_COLOR = 3;       /* RED */
    const uint8_t CAN_COLOR = 14;       /* SKYBLUE */

    /* 24x24 filled circle sprite (2-color, 1-bit): 576 pixels / 8 = 72 bytes */
    static const uint8_t dot_sprite[72] = {
        0x00, 0x7E, 0x00,  /* Row 0 */
        0x01, 0xFF, 0x80,  /* Row 1 */
        0x07, 0xFF, 0xE0,  /* Row 2 */
        0x0F, 0xFF, 0xF0,  /* Row 3 */
        0x1F, 0xFF, 0xF8,  /* Row 4 */
        0x3F, 0xFF, 0xFC,  /* Row 5 */
        0x3F, 0xFF, 0xFC,  /* Row 6 */
        0x7F, 0xFF, 0xFE,  /* Row 7 */
        0x7F, 0xFF, 0xFE,  /* Row 8 */
        0xFF, 0xFF, 0xFF,  /* Row 9 */
        0xFF, 0xFF, 0xFF,  /* Row 10 */
        0xFF, 0xFF, 0xFF,  /* Row 11 */
        0xFF, 0xFF, 0xFF,  /* Row 12 */
        0xFF, 0xFF, 0xFF,  /* Row 13 */
        0xFF, 0xFF, 0xFF,  /* Row 14 */
        0x7F, 0xFF, 0xFE,  /* Row 15 */
        0x7F, 0xFF, 0xFE,  /* Row 16 */
        0x3F, 0xFF, 0xFC,  /* Row 17 */
        0x3F, 0xFF, 0xFC,  /* Row 18 */
        0x1F, 0xFF, 0xF8,  /* Row 19 */
        0x0F, 0xFF, 0xF0,  /* Row 20 */
        0x07, 0xFF, 0xE0,  /* Row 21 */
        0x01, 0xFF, 0x80,  /* Row 22 */
        0x00, 0x7E, 0x00,  /* Row 23 */
    };
    const size_t DOT_SPRITE_SIZE = 72;

    /* Track edge detections (store best Y position for each side) */
    uint8_t left_edge_color = 0;
    uint8_t right_edge_color = 0;
    int16_t left_edge_y = -1;
    int16_t right_edge_y = -1;

    for (int gy = 0; gy < FOMO_GRID_SIZE; gy++) {
        for (int gx = 0; gx < FOMO_GRID_SIZE; gx++) {
            int idx = (gy * FOMO_GRID_SIZE + gx) * FOMO_NUM_CLASSES;

            int8_t beer_score = output_grid[idx + 1];
            int8_t can_score = output_grid[idx + 2];

            uint8_t color = 0;
            if (beer_score > DETECTION_THRESHOLD && beer_score >= can_score) {
                color = BEER_COLOR;
            } else if (can_score > DETECTION_THRESHOLD) {
                color = CAN_COLOR;
            }

            if (color == 0) continue;

            /* Calculate Y position (same for all cases) */
            int16_t py = gy * CELL_HEIGHT + (CELL_HEIGHT - SPRITE_SIZE) / 2;
            if (py < 0) py = 0;
            if (py > DISPLAY_H - SPRITE_SIZE) py = DISPLAY_H - SPRITE_SIZE;

            if (gx >= CENTER_START && gx <= CENTER_END) {
                /* Detection is within display FOV - draw dot */
                int rel_x = gx - CENTER_START;  /* 0 to 3 */
                uint16_t cell_width = DISPLAY_W / CENTER_COLS;
                int16_t px = rel_x * cell_width + (cell_width - SPRITE_SIZE) / 2;

                if (px < 0) px = 0;
                if (px > DISPLAY_W - SPRITE_SIZE) px = DISPLAY_W - SPRITE_SIZE;

                uint8_t meta[8] = {
                    (uint8_t)(px >> 8), (uint8_t)(px & 0xFF),
                    (uint8_t)(py >> 8), (uint8_t)(py & 0xFF),
                    0, SPRITE_SIZE,
                    2,
                    color
                };

                uint8_t *payload = malloc(8 + DOT_SPRITE_SIZE);
                if (payload) {
                    memcpy(payload, meta, 8);
                    memcpy(payload + 8, dot_sprite, DOT_SPRITE_SIZE);
                    spi_write(FPGA, 0x12, payload, 8 + DOT_SPRITE_SIZE);
                    free(payload);
                }
            } else if (gx < CENTER_START) {
                /* Left of display FOV - mark for edge indicator */
                if (left_edge_color == 0 || color == BEER_COLOR) {
                    left_edge_color = color;
                    left_edge_y = py;
                }
            } else {
                /* Right of display FOV - mark for edge indicator */
                if (right_edge_color == 0 || color == BEER_COLOR) {
                    right_edge_color = color;
                    right_edge_y = py;
                }
            }
        }
    }

    /* Draw left edge indicator line */
    if (left_edge_color != 0) {
        uint16_t px = 0;
        uint16_t py = (left_edge_y >= 0) ? (uint16_t)left_edge_y : (DISPLAY_H - LINE_HEIGHT) / 2;
        if (py > DISPLAY_H - LINE_HEIGHT) py = DISPLAY_H - LINE_HEIGHT;

        /* Line sprite: 12 wide x 80 tall, 1-bit = 2 bytes per row */
        size_t line_bytes = LINE_HEIGHT * 2;
        uint8_t *line_data = malloc(line_bytes);
        if (line_data) {
            memset(line_data, 0xFF, line_bytes);

            uint8_t meta[8] = {
                (uint8_t)(px >> 8), (uint8_t)(px & 0xFF),
                (uint8_t)(py >> 8), (uint8_t)(py & 0xFF),
                0, LINE_WIDTH,
                2,
                left_edge_color
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

    /* Draw right edge indicator line */
    if (right_edge_color != 0) {
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
                right_edge_color
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

    /* Swap frame buffers to display */
    spi_write(FPGA, 0x14, NULL, 0);
}

/*-----------------------------------------------*/
/* FOMO Grayscale Send Function                  */
/*-----------------------------------------------*/

static int lua_experiment_send_grayscale(lua_State *L)
{
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    char str_buf[128];
    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    /* Static buffers (see file-scope declarations). */
    uint8_t *jpeg_buffer = s_jpeg_buffer;
    uint8_t *temp_buffer = s_temp_buffer;
    uint8_t *gray_buffer = s_gray_buffer;
    (void)jpeg_buffer;  /* unused in DEV_KIT path */

    memset(temp_buffer, 0, GRAY_TEMP_BYTES);
    memset(gray_buffer, 0, GRAY_INPUT_BYTES);

#ifdef DEV_KIT_BUILD
    /* Use test image data instead of camera capture */
    result = jpeg_decode_grayscale_scaled(test_jpeg_data, test_jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);

    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("DEV_KIT: decoded %dx%d, downscaling to %dx%d", actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);

    /* Bilinear downscale 90x90 to 64x64 (no rotation for test image) */
    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;
        for (int dy = 0; dy < OUTPUT_SIZE; dy++) {
            for (int dx = 0; dx < OUTPUT_SIZE; dx++) {
                float sx = dx * scale;
                float sy = dy * scale;

                int x0 = (int)sx;
                int y0 = (int)sy;
                int x1 = (x0 + 1 < SCALED_SIZE) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < SCALED_SIZE) ? y0 + 1 : y0;

                float fx = sx - x0;
                float fy = sy - y0;

                float v00 = temp_buffer[y0 * SCALED_SIZE + x0];
                float v10 = temp_buffer[y0 * SCALED_SIZE + x1];
                float v01 = temp_buffer[y1 * SCALED_SIZE + x0];
                float v11 = temp_buffer[y1 * SCALED_SIZE + x1];

                float v = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                          v01 * (1-fx) * fy + v11 * fx * fy;

                gray_buffer[dy * OUTPUT_SIZE + dx] = (uint8_t)(v + 0.5f);
            }
        }
    }

    actual_width = OUTPUT_SIZE;
    actual_height = OUTPUT_SIZE;
#else
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    /* Camera capture path - same as original */
    lua_getglobal(L, "frame");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "frame global not found");
        return 0;
    }
    lua_getfield(L, -1, "camera");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        luaL_error(L, "frame.camera not found");
        return 0;
    }
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

    /* Auto-adjust camera */
    const int AUTO_ITERATIONS = 5;
    for (int i = 0; i < AUTO_ITERATIONS; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            luaL_error(L, "frame.camera.auto not found");
            return 0;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.auto failed: %s", err);
            return 0;
        }
        lua_pop(L, 3);

        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }

    /* Capture image */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        luaL_error(L, "frame.camera.capture not found");
        return 0;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "HIGH");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        lua_pop(L, 3);
        luaL_error(L, "camera.capture failed: %s", err);
        return 0;
    }
    lua_pop(L, 2);

    /* Wait for image ready */
    uint32_t timeout = 1000000;
    uint32_t wdt_counter = 0;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.image_ready failed: %s", err);
            return 0;
        }
        ready = lua_toboolean(L, -1);
        lua_pop(L, 3);
        if (!ready) {
            nrfx_systick_delay_us(10);
            if (++wdt_counter >= 10000) {
                reload_watchdog(NULL, NULL);
                wdt_counter = 0;
            }
        }
    }

    if (!ready) {
        luaL_error(L, "capture timeout");
        return 0;
    }

    /* Read JPEG data */
    jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.read failed: %s", err);
            return 0;
        }

        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
            break;
        }

        size_t chunk_len;
        const char *chunk = lua_tolstring(L, -1, &chunk_len);
        if (jpeg_size + chunk_len > MAX_JPEG_SIZE) {
            lua_pop(L, 3);
            luaL_error(L, "JPEG too large");
            return 0;
        }
        memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
        jpeg_size += chunk_len;
        lua_pop(L, 3);
        reload_watchdog(NULL, NULL);
    }

    if (jpeg_size == 0) {
        luaL_error(L, "no JPEG data received");
        return 0;
    }

    /* Decode JPEG */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_grayscale_scaled(jpeg_buffer, jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);

    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("Decoded %dx%d, downscaling to %dx%d with 90 CCW rotation",
        actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);

    /* Downscale 90x90 to 64x64 with 90 CCW rotation */
    reload_watchdog(NULL, NULL);
    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;
        for (int dy = 0; dy < OUTPUT_SIZE; dy++) {
            for (int dx = 0; dx < OUTPUT_SIZE; dx++) {
                float sx = dx * scale;
                float sy = dy * scale;

                int x0 = (int)sx;
                int y0 = (int)sy;
                int x1 = (x0 + 1 < SCALED_SIZE) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < SCALED_SIZE) ? y0 + 1 : y0;

                float fx = sx - x0;
                float fy = sy - y0;

                float v00 = temp_buffer[y0 * SCALED_SIZE + x0];
                float v10 = temp_buffer[y0 * SCALED_SIZE + x1];
                float v01 = temp_buffer[y1 * SCALED_SIZE + x0];
                float v11 = temp_buffer[y1 * SCALED_SIZE + x1];

                float v = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                          v01 * (1-fx) * fy + v11 * fx * fy;

                /* Apply 90 CCW rotation: (dx, dy) -> (dy, OUTPUT_SIZE-1-dx) */
                int rx = dy;
                int ry = OUTPUT_SIZE - 1 - dx;
                gray_buffer[ry * OUTPUT_SIZE + rx] = (uint8_t)(v + 0.5f);
            }
        }
    }

    actual_width = OUTPUT_SIZE;
    actual_height = OUTPUT_SIZE;
#endif

    /* Send header with dimensions */
    snprintf(str_buf, sizeof(str_buf), "GRAY:%d,%d,%d\n",
             actual_width, actual_height, actual_width * actual_height);

    /* Small delay to let header be received */
    nrfx_systick_delay_ms(50);

    /* Send grayscale data in chunks */
    size_t total_bytes = actual_width * actual_height;
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, gray_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    /* Send end marker */
    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bluetooth_send_data(end_marker, 5);

    free(chunk_buffer);

    lua_pushinteger(L, total_bytes);
    return 1;
}

/*-----------------------------------------------*/
/* FOMO Object Detection Functions               */
/*-----------------------------------------------*/

/**
 * Run FOMO object detection model and send results via Bluetooth
 */
static int lua_experiment_run_object_detection(lua_State *L)
{
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    /* Check if FOMO model is initialized */
    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }

    /* Static buffers (see file-scope declarations). */
    uint8_t *jpeg_buffer = s_jpeg_buffer;
    uint8_t *temp_buffer = s_temp_buffer;
    uint8_t *gray_buffer = s_gray_buffer;
    (void)jpeg_buffer;  /* unused in DEV_KIT path */

    memset(temp_buffer, 0, GRAY_TEMP_BYTES);
    memset(gray_buffer, 0, GRAY_INPUT_BYTES);

#ifdef DEV_KIT_BUILD
    /* Use test image data instead of camera capture */
    result = jpeg_decode_grayscale_scaled(test_jpeg_data, test_jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);

    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("DEV_KIT: decoded %dx%d, downscaling to %dx%d", actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);

    /* Bilinear downscale 90x90 to 64x64 (no rotation for test image) */
    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;
        for (int dy = 0; dy < OUTPUT_SIZE; dy++) {
            for (int dx = 0; dx < OUTPUT_SIZE; dx++) {
                float sx = dx * scale;
                float sy = dy * scale;

                int x0 = (int)sx;
                int y0 = (int)sy;
                int x1 = (x0 + 1 < SCALED_SIZE) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < SCALED_SIZE) ? y0 + 1 : y0;

                float fx = sx - x0;
                float fy = sy - y0;

                float v00 = temp_buffer[y0 * SCALED_SIZE + x0];
                float v10 = temp_buffer[y0 * SCALED_SIZE + x1];
                float v01 = temp_buffer[y1 * SCALED_SIZE + x0];
                float v11 = temp_buffer[y1 * SCALED_SIZE + x1];

                float v = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                          v01 * (1-fx) * fy + v11 * fx * fy;

                gray_buffer[dy * OUTPUT_SIZE + dx] = (uint8_t)(v + 0.5f);
            }
        }
    }

    actual_width = OUTPUT_SIZE;
    actual_height = OUTPUT_SIZE;
#else
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    /* Camera capture path */
    lua_getglobal(L, "frame");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "frame global not found");
        return 0;
    }
    lua_getfield(L, -1, "camera");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        luaL_error(L, "frame.camera not found");
        return 0;
    }
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

    /* Auto-adjust camera */
    const int AUTO_ITERATIONS = 5;
    for (int i = 0; i < AUTO_ITERATIONS; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            luaL_error(L, "frame.camera.auto not found");
            return 0;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.auto failed: %s", err);
            return 0;
        }
        lua_pop(L, 3);

        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }

    /* Capture image */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        luaL_error(L, "frame.camera.capture not found");
        return 0;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "MEDIUM");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        lua_pop(L, 3);
        luaL_error(L, "camera.capture failed: %s", err);
        return 0;
    }
    lua_pop(L, 2);

    /* Wait for image ready */
    uint32_t timeout = 1000000;
    uint32_t wdt_counter = 0;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.image_ready failed: %s", err);
            return 0;
        }
        ready = lua_toboolean(L, -1);
        lua_pop(L, 3);
        if (!ready) {
            nrfx_systick_delay_us(10);
            if (++wdt_counter >= 10000) {
                reload_watchdog(NULL, NULL);
                wdt_counter = 0;
            }
        }
    }

    if (!ready) {
        luaL_error(L, "capture timeout");
        return 0;
    }

    /* Read JPEG data */
    jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.read failed: %s", err);
            return 0;
        }

        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
            break;
        }

        size_t chunk_len;
        const char *chunk = lua_tolstring(L, -1, &chunk_len);
        if (jpeg_size + chunk_len > MAX_JPEG_SIZE) {
            lua_pop(L, 3);
            luaL_error(L, "JPEG too large");
            return 0;
        }
        memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
        jpeg_size += chunk_len;
        lua_pop(L, 3);
        reload_watchdog(NULL, NULL);
    }

    if (jpeg_size == 0) {
        luaL_error(L, "no JPEG data received");
        return 0;
    }

    /* Decode JPEG */
    reload_watchdog(NULL, NULL);

    result = jpeg_decode_grayscale_scaled(jpeg_buffer, jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);

    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("Decoded %dx%d, downscaling to %dx%d with 90 CCW rotation",
        actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);

    /* Downscale 90x90 to 64x64 with 90 CCW rotation */
    reload_watchdog(NULL, NULL);
    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;
        for (int dy = 0; dy < OUTPUT_SIZE; dy++) {
            for (int dx = 0; dx < OUTPUT_SIZE; dx++) {
                float sx = dx * scale;
                float sy = dy * scale;

                int x0 = (int)sx;
                int y0 = (int)sy;
                int x1 = (x0 + 1 < SCALED_SIZE) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < SCALED_SIZE) ? y0 + 1 : y0;

                float fx = sx - x0;
                float fy = sy - y0;

                float v00 = temp_buffer[y0 * SCALED_SIZE + x0];
                float v10 = temp_buffer[y0 * SCALED_SIZE + x1];
                float v01 = temp_buffer[y1 * SCALED_SIZE + x0];
                float v11 = temp_buffer[y1 * SCALED_SIZE + x1];

                float v = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                          v01 * (1-fx) * fy + v11 * fx * fy;

                /* Apply 90 CCW rotation: (dx, dy) -> (dy, OUTPUT_SIZE-1-dx) */
                int rx = dy;
                int ry = OUTPUT_SIZE - 1 - dx;
                gray_buffer[ry * OUTPUT_SIZE + rx] = (uint8_t)(v + 0.5f);
            }
        }
    }

    actual_width = OUTPUT_SIZE;
    actual_height = OUTPUT_SIZE;
#endif

    /* Run FOMO inference */
    reload_watchdog(NULL, NULL);
    int8_t output_grid[FOMO_OUTPUT_SIZE];

    tflm_status_t infer_status = fomo_infer(gray_buffer, output_grid);
    if (infer_status != TFLM_OK) {
        luaL_error(L, "FOMO inference failed");
        return 0;
    }

    LOG("FOMO inference complete");

    /* Display detection overlay on Frame */
    draw_detection_overlay(output_grid);
    reload_watchdog(NULL, NULL);

    /* Send image data via Bluetooth */
    size_t total_bytes = actual_width * actual_height;
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, gray_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    /* Send separator */
    nrfx_systick_delay_ms(50);
    uint8_t separator[3] = {0x01, 0xFE, 0xFE};
    bluetooth_send_data(separator, 3);

    /* Send predictions */
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

    /* Send end marker */
    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bluetooth_send_data(end_marker, 5);

    free(chunk_buffer);

    lua_pushinteger(L, total_bytes + FOMO_OUTPUT_SIZE);
    return 1;
}

/**
 * Fast object detection - skips camera wake and autoexposure.
 */
static int lua_experiment_run_object_detection_fast(lua_State *L)
{
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }

    /* Static buffers (see file-scope declarations). */
    uint8_t *jpeg_buffer = s_jpeg_buffer;
    uint8_t *temp_buffer = s_temp_buffer;
    uint8_t *gray_buffer = s_gray_buffer;
    (void)jpeg_buffer;  /* unused in DEV_KIT path */

    memset(temp_buffer, 0, GRAY_TEMP_BYTES);
    memset(gray_buffer, 0, GRAY_INPUT_BYTES);

#ifdef DEV_KIT_BUILD
    result = jpeg_decode_grayscale_scaled(test_jpeg_data, test_jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);
    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("DEV_KIT: decoded %dx%d, downscaling to %dx%d", actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);

    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;
        for (int dy = 0; dy < OUTPUT_SIZE; dy++) {
            for (int dx = 0; dx < OUTPUT_SIZE; dx++) {
                float sx = dx * scale;
                float sy = dy * scale;
                int x0 = (int)sx;
                int y0 = (int)sy;
                int x1 = (x0 + 1 < SCALED_SIZE) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < SCALED_SIZE) ? y0 + 1 : y0;
                float fx = sx - x0;
                float fy = sy - y0;
                float v00 = temp_buffer[y0 * SCALED_SIZE + x0];
                float v10 = temp_buffer[y0 * SCALED_SIZE + x1];
                float v01 = temp_buffer[y1 * SCALED_SIZE + x0];
                float v11 = temp_buffer[y1 * SCALED_SIZE + x1];
                float v = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                          v01 * (1-fx) * fy + v11 * fx * fy;
                gray_buffer[dy * OUTPUT_SIZE + dx] = (uint8_t)(v + 0.5f);
            }
        }
    }
    actual_width = OUTPUT_SIZE;
    actual_height = OUTPUT_SIZE;
#else
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    /* Fast path: Skip camera wake and autoexposure */
    /* Caller must ensure camera is awake and auto-adjusted before calling */

    /* Capture image */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        luaL_error(L, "frame.camera.capture not found");
        return 0;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "MEDIUM");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        lua_pop(L, 3);
        luaL_error(L, "camera.capture failed: %s", err);
        return 0;
    }
    lua_pop(L, 2);

    /* Wait for image ready */
    uint32_t timeout = 1000000;
    uint32_t wdt_counter = 0;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.image_ready failed: %s", err);
            return 0;
        }
        ready = lua_toboolean(L, -1);
        lua_pop(L, 3);
        if (!ready) {
            nrfx_systick_delay_us(10);
            if (++wdt_counter >= 10000) {
                reload_watchdog(NULL, NULL);
                wdt_counter = 0;
            }
        }
    }

    if (!ready) {
        luaL_error(L, "capture timeout");
        return 0;
    }

    /* Read JPEG data */
    jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            lua_pop(L, 3);
            luaL_error(L, "camera.read failed: %s", err);
            return 0;
        }

        if (lua_isnil(L, -1)) {
            lua_pop(L, 3);
            break;
        }

        size_t chunk_len;
        const char *chunk = lua_tolstring(L, -1, &chunk_len);
        if (jpeg_size + chunk_len > MAX_JPEG_SIZE) {
            lua_pop(L, 3);
            luaL_error(L, "JPEG too large");
            return 0;
        }
        memcpy(jpeg_buffer + jpeg_size, chunk, chunk_len);
        jpeg_size += chunk_len;
        lua_pop(L, 3);
        reload_watchdog(NULL, NULL);
    }

    if (jpeg_size == 0) {
        luaL_error(L, "no JPEG data received");
        return 0;
    }

    /* Decode JPEG */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_grayscale_scaled(jpeg_buffer, jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);

    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("Decoded %dx%d, downscaling to %dx%d with 90 CCW rotation",
        actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);

    /* Downscale 90x90 to 64x64 with 90 CCW rotation */
    reload_watchdog(NULL, NULL);
    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;
        for (int dy = 0; dy < OUTPUT_SIZE; dy++) {
            for (int dx = 0; dx < OUTPUT_SIZE; dx++) {
                float sx = dx * scale;
                float sy = dy * scale;
                int x0 = (int)sx;
                int y0 = (int)sy;
                int x1 = (x0 + 1 < SCALED_SIZE) ? x0 + 1 : x0;
                int y1 = (y0 + 1 < SCALED_SIZE) ? y0 + 1 : y0;
                float fx = sx - x0;
                float fy = sy - y0;
                float v00 = temp_buffer[y0 * SCALED_SIZE + x0];
                float v10 = temp_buffer[y0 * SCALED_SIZE + x1];
                float v01 = temp_buffer[y1 * SCALED_SIZE + x0];
                float v11 = temp_buffer[y1 * SCALED_SIZE + x1];
                float v = v00 * (1-fx) * (1-fy) + v10 * fx * (1-fy) +
                          v01 * (1-fx) * fy + v11 * fx * fy;
                int rx = dy;
                int ry = OUTPUT_SIZE - 1 - dx;
                gray_buffer[ry * OUTPUT_SIZE + rx] = (uint8_t)(v + 0.5f);
            }
        }
    }
    actual_width = OUTPUT_SIZE;
    actual_height = OUTPUT_SIZE;
#endif

    /* Run FOMO inference */
    reload_watchdog(NULL, NULL);
    int8_t output_grid[FOMO_OUTPUT_SIZE];

    tflm_status_t infer_status = fomo_infer(gray_buffer, output_grid);
    if (infer_status != TFLM_OK) {
        luaL_error(L, "FOMO inference failed");
        return 0;
    }

    LOG("FOMO inference complete");

    /* Display detection overlay on Frame */
    draw_detection_overlay(output_grid);
    reload_watchdog(NULL, NULL);

    /* Send image data via Bluetooth */
    size_t total_bytes = actual_width * actual_height;
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, gray_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    /* Send separator */
    nrfx_systick_delay_ms(50);
    uint8_t separator[3] = {0x01, 0xFE, 0xFE};
    bluetooth_send_data(separator, 3);

    /* Send predictions */
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

    /* Send end marker */
    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bluetooth_send_data(end_marker, 5);

    free(chunk_buffer);

    lua_pushinteger(L, total_bytes + FOMO_OUTPUT_SIZE);
    return 1;
}

/*-----------------------------------------------*/
/* Pure-inference benchmark                      */
/*-----------------------------------------------*/

static bool bench_send_with_retry(const uint8_t *data, size_t length)
{
    for (int attempt = 0; attempt < 50; attempt++) {
        if (!bluetooth_send_data(data, length)) {
            return true;
        }
        nrfx_systick_delay_ms(5);
        reload_watchdog(NULL, NULL);
    }
    return false;
}

/**
 * Pure-inference benchmark: tight loop over fomo_infer().
 *
 *   - BENCH_WARMUP untimed inferences first to settle caches and the
 *     branch predictor.
 *   - Then BENCH_K cycles. Each cycle refills the input buffer with
 *     fresh pseudo-random bytes and times BENCH_N back-to-back
 *     inferences using the DWT cycle counter (64 MHz).
 *   - After each cycle the BENCH_N raw cycle counts are streamed back
 *     over BLE in chunks, prefixed with a 4-byte LE cycle index.
 *     Cycles are separated by 0x01 0xFE 0xFE on the wire and the run
 *     terminates with 0x01 0xFF 0xFF 0x00 0x00. Identical to the VWW
 *     protocol so the same host-side script parses both.
 *
 * Lua: frame.experiment.run_inference_benchmark()
 * Returns: total number of timed inferences (BENCH_K * BENCH_N).
 */
static int lua_experiment_run_inference_benchmark(lua_State *L)
{
    if (!fomo_is_initialized()) {
        luaL_error(L, "FOMO model not initialized");
        return 0;
    }
    if (!bluetooth_is_connected()) {
        luaL_error(L, "Bluetooth not connected");
        return 0;
    }

    LOG("bench: start (kernels=%s, K=%u, N=%u, warmup=%u, payload=%u B)",
        TFLM_KERNEL_MODE_STRING,
        (unsigned)BENCH_K, (unsigned)BENCH_N, (unsigned)BENCH_WARMUP,
        (unsigned)sizeof(s_bench_payload));

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    srand((unsigned int)DWT->CYCCNT);

    uint8_t *input = s_gray_buffer;
    int8_t output_grid[FOMO_OUTPUT_SIZE];

    /* One-time warmup. */
    for (size_t i = 0; i < FOMO_INPUT_SIZE; i++) {
        input[i] = (uint8_t)(rand() & 0xFF);
    }
    for (int w = 0; w < BENCH_WARMUP; w++) {
        if (fomo_infer(input, output_grid) != TFLM_OK) {
            luaL_error(L, "warmup inference failed");
            return 0;
        }
        reload_watchdog(NULL, NULL);
    }
    LOG("bench: warmup done");

    const size_t CHUNK_SIZE = 100;
    const uint32_t SETTLE_MS = 250;
    const uint32_t CHUNK_DELAY_MS = 100;
    uint8_t chunk_buffer[CHUNK_SIZE + 1];
    chunk_buffer[0] = 0x01;

    for (uint32_t k = 0; k < BENCH_K; k++) {
        for (size_t i = 0; i < FOMO_INPUT_SIZE; i++) {
            input[i] = (uint8_t)(rand() & 0xFF);
        }
        reload_watchdog(NULL, NULL);

        s_bench_payload.cycle_idx = k;

        for (uint32_t i = 0; i < BENCH_N; i++) {
            uint32_t t0 = DWT->CYCCNT;
            tflm_status_t st = fomo_infer(input, output_grid);
            uint32_t t1 = DWT->CYCCNT;
            if (st != TFLM_OK) {
                luaL_error(L, "inference failed at k=%u i=%u",
                           (unsigned)k, (unsigned)i);
                return 0;
            }
            s_bench_payload.cycles[i] = t1 - t0;
            reload_watchdog(NULL, NULL);
        }

        LOG("bench: cycle %u inferences done; first=%u last=%u",
            (unsigned)k,
            (unsigned)s_bench_payload.cycles[0],
            (unsigned)s_bench_payload.cycles[BENCH_N - 1]);

        nrfx_systick_delay_ms(SETTLE_MS);
        reload_watchdog(NULL, NULL);

        /* Sacrificial wake-ping (see experiment_vww.c for rationale). */
        const uint8_t wake[5] = {0x01, 0xAA, 0xAA, 0xAA, 0xAA};
        (void)bench_send_with_retry(wake, 5);
        nrfx_systick_delay_ms(CHUNK_DELAY_MS);
        reload_watchdog(NULL, NULL);

        const uint8_t *payload = (const uint8_t *)&s_bench_payload;
        size_t total = sizeof(s_bench_payload);
        size_t offset = 0;
        unsigned chunk_idx = 0;
        while (offset < total) {
            size_t chunk = (total - offset > CHUNK_SIZE)
                              ? CHUNK_SIZE
                              : (total - offset);
            chunk_buffer[0] = 0x01;
            memcpy(chunk_buffer + 1, payload + offset, chunk);
            if (!bench_send_with_retry(chunk_buffer, chunk + 1)) {
                luaL_error(L, "BLE send failed at cycle %u chunk %u",
                           (unsigned)k, chunk_idx);
                return 0;
            }
            LOG("bench: cycle %u chunk %u sent (%u B, offset %u/%u)",
                (unsigned)k, chunk_idx, (unsigned)chunk,
                (unsigned)(offset + chunk), (unsigned)total);
            offset += chunk;
            chunk_idx++;
            nrfx_systick_delay_ms(CHUNK_DELAY_MS);
            reload_watchdog(NULL, NULL);
        }

        nrfx_systick_delay_ms(CHUNK_DELAY_MS);
        const uint8_t separator[3] = {0x01, 0xFE, 0xFE};
        if (!bench_send_with_retry(separator, 3)) {
            luaL_error(L, "BLE separator failed at cycle %u", (unsigned)k);
            return 0;
        }
        LOG("bench: cycle %u/%u separator sent",
            (unsigned)(k + 1), (unsigned)BENCH_K);
    }

    nrfx_systick_delay_ms(SETTLE_MS);
    const uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    if (!bench_send_with_retry(end_marker, 5)) {
        luaL_error(L, "BLE end marker failed");
        return 0;
    }
    LOG("bench: end marker sent, %u inferences total",
        (unsigned)(BENCH_K * BENCH_N));

    lua_pushinteger(L, (lua_Integer)(BENCH_K * BENCH_N));
    return 1;
}

/*-----------------------------------------------*/
/* Experiment Interface Implementation           */
/*-----------------------------------------------*/

const char* experiment_get_name(void)
{
    return "FOMO_BEER_CAN";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    lua_pushcfunction(L, lua_experiment_send_grayscale);
    lua_setfield(L, -2, "send_grayscale");

    lua_pushcfunction(L, lua_experiment_run_object_detection);
    lua_setfield(L, -2, "run_object_detection_model");

    lua_pushcfunction(L, lua_experiment_run_object_detection_fast);
    lua_setfield(L, -2, "run_object_detection_fast");

    lua_pushcfunction(L, lua_experiment_run_inference_benchmark);
    lua_setfield(L, -2, "run_inference_benchmark");
}
