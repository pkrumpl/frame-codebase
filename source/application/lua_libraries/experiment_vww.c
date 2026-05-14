/*
 * VWW (Visual Wake Words) Experiment Implementation
 *
 * Person detection experiment using the VWW model.
 * This file is compiled when ML_EXPERIMENT=VWW is set.
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
#define OUTPUT_SIZE       96    /* ML input size (96x96) */

/* Buffer sizes */
#define GRAY_TEMP_BYTES   (SCALED_SIZE * SCALED_SIZE)         /* 8100 */
#define GRAY_INPUT_BYTES  (OUTPUT_SIZE * OUTPUT_SIZE)         /* 9216 */
#define MAX_JPEG_SIZE     (25 * 1024)

/* Static working buffers in BSS. Rationale (mirrors experiment_vww_rgb.c):
 *   - Per-call malloc/free of buffers this large fragments the heap fast,
 *     causing later allocations to fail mid-stream.
 *   - Static placement is contiguous at link time, no allocator
 *     involvement, no fragmentation.
 *   - The two Lua entry points below are mutually exclusive (only one
 *     runs at a time) so they share these buffers.
 *   - JPEG cannot alias gray_buffer the way vww_rgb does (gray is only
 *     9.2 KB; JPEG can be up to 25 KB), so three separate buffers. */
static uint8_t s_jpeg_buffer[MAX_JPEG_SIZE];     /* 25 KB raw JPEG */
static uint8_t s_temp_buffer[GRAY_TEMP_BYTES];   /* 8.1 KB decoded 90x90 */
static uint8_t s_gray_buffer[GRAY_INPUT_BYTES];  /* 9 KB final 96x96 */

/* Pure-inference benchmark constants and state.
 *   - WARMUP discards the first inferences so caches/branch predictors settle.
 *   - N is the number of timed inferences inside one cycle.
 *   - K is the number of cycles; after each one the N timings are streamed
 *     back over BLE before starting the next cycle. */
#define BENCH_K       8
#define BENCH_N       64
#define BENCH_WARMUP  10

/* Wire payload for one cycle: 4-byte LE cycle index followed by N raw
 * DWT cycle counts (uint32 LE). Packed naturally - all members are
 * uint32_t so sizeof == 4 + 4*BENCH_N == 1028 bytes, no padding. */
static struct {
    uint32_t cycle_idx;
    uint32_t cycles[BENCH_N];
} s_bench_payload;

/*-----------------------------------------------*/
/* Person Detection Overlay                      */
/*-----------------------------------------------*/

/**
 * Draw simple text overlay for person detection result.
 * Displays colored indicator for PERSON (green) or NO PERSON (red)
 */
static void draw_person_detection_overlay(bool is_person, int8_t person_score)
{
    (void)person_score;  /* Suppress unused parameter warning */

    const uint16_t DISPLAY_W = 640;
    const uint16_t DISPLAY_H = 400;

    /* Color indices */
    const uint8_t PERSON_COLOR = 10;     /* GREEN for person */
    const uint8_t NO_PERSON_COLOR = 3;   /* RED for no person */

    /* Rectangle indicator: 100x60 filled block */
    static const uint8_t block_sprite[750] = {  /* 100x60 = 6000 bits = 750 bytes, all 0xFF */
        [0 ... 749] = 0xFF
    };

    /* Position: center of display */
    int16_t px = (DISPLAY_W - 100) / 2;
    int16_t py = (DISPLAY_H - 60) / 2;

    uint8_t color = is_person ? PERSON_COLOR : NO_PERSON_COLOR;

    /* Draw indicator block */
    uint8_t meta[8] = {
        (uint8_t)(px >> 8), (uint8_t)(px & 0xFF),
        (uint8_t)(py >> 8), (uint8_t)(py & 0xFF),
        0, 100,  /* width = 100 */
        2,       /* 2 colors (1-bit) */
        color
    };

    uint8_t *payload = malloc(8 + 750);
    if (payload) {
        memcpy(payload, meta, 8);
        memcpy(payload + 8, block_sprite, 750);
        spi_write(FPGA, 0x12, payload, 8 + 750);
        free(payload);
    }

    /* Swap frame buffers to show */
    spi_write(FPGA, 0x14, NULL, 0);
}

/*-----------------------------------------------*/
/* Person Detection Lua Functions                */
/*-----------------------------------------------*/

/**
 * Run person detection on camera image.
 *
 * Image pipeline: 720x720 JPEG -> scale=3 (90x90) -> upscale to 96x96
 * Output: Display PERSON (green) or NO PERSON (red) overlay
 *
 * Protocol:
 *   [IMAGE DATA]     9216 bytes (96x96 grayscale)
 *   [SEPARATOR]      0x01 0xFE 0xFE
 *   [PREDICTIONS]    2 bytes (not_person_score, person_score)
 *   [END MARKER]     0x01 0xFF 0xFF 0x00 0x00
 */
static int lua_experiment_run_person_detection(lua_State *L)
{
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    /* Check if person detect model is initialized */
    if (!person_detect_is_initialized()) {
        luaL_error(L, "Person detect model not initialized");
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
    /* DEV_KIT: Use hardcoded test JPEG data instead of camera */
    LOG("DEV_KIT: Using hardcoded test JPEG data (%u bytes)", test_jpeg_size);

    /* Decode test JPEG to 90x90 grayscale */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_grayscale_scaled(test_jpeg_data, test_jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);  /* scale=3 (1/8), no rotation */
    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }
    LOG("DEV_KIT: decoded %dx%d", actual_width, actual_height);

#else /* !DEV_KIT_BUILD */
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    /* ===== Step 1: Wake up camera ===== */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "power_save");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, 0);  /* power_save(false) */
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 3);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    nrfx_systick_delay_ms(100);

    /* ===== Step 2: Auto-adjust camera (5 iterations) ===== */
    for (int i = 0; i < 5; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            luaL_error(L, "camera.auto not found");
            return 0;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            luaL_error(L, "camera.auto failed");
            return 0;
        }
        lua_pop(L, 3);
        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }

    /* ===== Step 3: Capture image ===== */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
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
        luaL_error(L, "capture failed");
        return 0;
    }
    lua_pop(L, 2);

    /* ===== Step 4: Wait for image ready ===== */
    uint32_t timeout = 1000000;
    bool ready = false;
    while (timeout-- && !ready) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "image_ready");
        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
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
        luaL_error(L, "capture timeout");
        return 0;
    }

    /* ===== Step 5: Read JPEG data ===== */
    jpeg_size = 0;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
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

    LOG("Person detect: JPEG size = %u bytes", jpeg_size);

    /* ===== Step 6: Decode JPEG to 90x90 grayscale ===== */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_grayscale_scaled(jpeg_buffer, jpeg_size,
                                           temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                           &actual_width, &actual_height,
                                           3, false);  /* scale=3 (1/8), no rotation */

    if (result != 0) {
        luaL_error(L, "decode failed: %d", result);
        return 0;
    }

    LOG("Person detect: decoded %dx%d, UPSCALING to %dx%d", actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);
#endif /* !DEV_KIT_BUILD */

    /* ===== Step 7: UPSCALE 90x90 to 96x96 with 90 CCW rotation ===== */
    reload_watchdog(NULL, NULL);
    {
        const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;  /* 90/96 = 0.9375 */
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

    /* ===== Step 8: Run person detection inference ===== */
    reload_watchdog(NULL, NULL);
    int8_t output_scores[PERSON_OUTPUT_SIZE];  /* 2 bytes */

    tflm_status_t infer_status = person_detect_infer(gray_buffer, output_scores);
    if (infer_status != TFLM_OK) {
        luaL_error(L, "Person detect inference failed");
        return 0;
    }

    int8_t not_person_score = output_scores[PERSON_NOT_PERSON_INDEX];
    int8_t person_score = output_scores[PERSON_PERSON_INDEX];
    bool is_person = (person_score > not_person_score);

    LOG("Person detect: not_person=%d, person=%d, result=%s",
        not_person_score, person_score, is_person ? "PERSON" : "NO PERSON");

    /* ===== Step 9: Display overlay ===== */
    draw_person_detection_overlay(is_person, person_score);
    reload_watchdog(NULL, NULL);

    /* ===== Step 10: Send image data via Bluetooth ===== */
    size_t total_bytes = GRAY_INPUT_BYTES;  /* 9216 */
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;  /* Data flag */

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, gray_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    /* ===== Step 11: Send separator ===== */
    nrfx_systick_delay_ms(50);
    uint8_t separator[3] = {0x01, 0xFE, 0xFE};
    bluetooth_send_data(separator, 3);

    /* ===== Step 12: Send predictions (2 bytes) ===== */
    nrfx_systick_delay_ms(50);
    chunk_buffer[0] = 0x01;
    memcpy(chunk_buffer + 1, output_scores, PERSON_OUTPUT_SIZE);
    bluetooth_send_data(chunk_buffer, PERSON_OUTPUT_SIZE + 1);

    /* ===== Step 13: Send end marker ===== */
    nrfx_systick_delay_ms(100);
    uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    bluetooth_send_data(end_marker, 5);

    free(chunk_buffer);

    lua_pushinteger(L, total_bytes + PERSON_OUTPUT_SIZE);
    return 1;
}

/**
 * Run person detection benchmark - local inference without Bluetooth transmission
 * Lua: result = frame.experiment.run_person_detection_benchmark(iterations)
 * Returns: { iterations, person_detections, total_time_ms, avg_time_ms }
 */
static int lua_experiment_run_person_detection_benchmark(lua_State *L)
{
    uint16_t actual_width, actual_height;
    int result;

    /* Get iterations parameter */
    lua_Integer iterations = luaL_checkinteger(L, 1);
    if (iterations < 1 || iterations > 1000) {
        luaL_error(L, "iterations must be between 1 and 1000");
        return 0;
    }

    /* Check if person detect model is initialized */
    if (!person_detect_is_initialized()) {
        luaL_error(L, "Person detect model not initialized");
        return 0;
    }

    /* Static buffers (see file-scope declarations). */
    uint8_t *temp_buffer = s_temp_buffer;
    uint8_t *gray_buffer = s_gray_buffer;

#ifndef DEV_KIT_BUILD
    const size_t READ_CHUNK_SIZE = 512;
    size_t jpeg_size = 0;

    uint8_t *jpeg_buffer = s_jpeg_buffer;

    /* ===== Initialize camera ONCE before benchmark loop ===== */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "power_save");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, 0);  /* power_save(false) */
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            lua_pop(L, 3);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    nrfx_systick_delay_ms(100);

    /* Auto-adjust camera (5 iterations) */
    for (int i = 0; i < 5; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            luaL_error(L, "camera.auto not found");
            return 0;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            luaL_error(L, "camera.auto failed");
            return 0;
        }
        lua_pop(L, 3);
        nrfx_systick_delay_ms(100);
        reload_watchdog(NULL, NULL);
    }
    LOG("Benchmark: camera initialized");
#endif

    int person_count = 0;

    /* Enable DWT cycle counter for precise timing */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Timing accumulators for detailed breakdown */
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

    /* ===== Main benchmark loop ===== */
    for (int iter = 0; iter < iterations; iter++) {
        reload_watchdog(NULL, NULL);

        /* Time: memset */
        t0 = DWT->CYCCNT;
        memset(temp_buffer, 0, GRAY_TEMP_BYTES);
        memset(gray_buffer, 0, GRAY_INPUT_BYTES);
        t1 = DWT->CYCCNT;
        time_memset += (t1 - t0);

#ifdef DEV_KIT_BUILD
        /* Time: decode (DEV_KIT only - no capture/read) */
        t0 = DWT->CYCCNT;
        /* DEV_KIT: Use hardcoded test JPEG data (same image each iteration) */
        result = jpeg_decode_grayscale_scaled(test_jpeg_data, test_jpeg_size,
                                               temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                               &actual_width, &actual_height,
                                               3, false);  /* scale=3 (1/8), no rotation */
        if (result != 0) {
            luaL_error(L, "decode failed: %d", result);
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_decode += (t1 - t0);
#else
        /* Capture new image each iteration */
        memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

        /* Time: capture */
        t0 = DWT->CYCCNT;
        /* Capture image */
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "capture");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
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
            luaL_error(L, "capture failed");
            return 0;
        }
        lua_pop(L, 2);
        t1 = DWT->CYCCNT;
        time_capture += (t1 - t0);

        /* Time: wait for image ready */
        t0 = DWT->CYCCNT;
        /* Wait for image ready */
        uint32_t timeout = 1000000;
        bool ready = false;
        while (timeout-- && !ready) {
            lua_getglobal(L, "frame");
            lua_getfield(L, -1, "camera");
            lua_getfield(L, -1, "image_ready");
            if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
                lua_pop(L, 3);
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
            luaL_error(L, "capture timeout");
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_wait_ready += (t1 - t0);

        /* Time: read JPEG data */
        t0 = DWT->CYCCNT;
        /* Read JPEG data */
        jpeg_size = 0;
        while (jpeg_size < MAX_JPEG_SIZE) {
            lua_getglobal(L, "frame");
            lua_getfield(L, -1, "camera");
            lua_getfield(L, -1, "read");
            lua_pushinteger(L, READ_CHUNK_SIZE);
            if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
                lua_pop(L, 3);
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

        /* Time: decode JPEG */
        t0 = DWT->CYCCNT;
        /* Decode JPEG to 90x90 grayscale */
        reload_watchdog(NULL, NULL);
        result = jpeg_decode_grayscale_scaled(jpeg_buffer, jpeg_size,
                                               temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                               &actual_width, &actual_height,
                                               3, false);  /* scale=3 (1/8), no rotation */
        if (result != 0) {
            luaL_error(L, "decode failed: %d", result);
            return 0;
        }
        t1 = DWT->CYCCNT;
        time_decode += (t1 - t0);
#endif

        /* Time: upscale */
        t0 = DWT->CYCCNT;
        /* Upscale 90x90 to 96x96 with 90 CCW rotation */
        reload_watchdog(NULL, NULL);
        {
            const float scale = (float)SCALED_SIZE / (float)OUTPUT_SIZE;  /* 90/96 = 0.9375 */
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
        t1 = DWT->CYCCNT;
        time_upscale += (t1 - t0);

        /* Time: inference */
        t0 = DWT->CYCCNT;
        /* Run person detection inference */
        reload_watchdog(NULL, NULL);
        int8_t output_scores[PERSON_OUTPUT_SIZE];

        tflm_status_t infer_status = person_detect_infer(gray_buffer, output_scores);
        if (infer_status != TFLM_OK) {
            luaL_error(L, "Person detect inference failed");
            return 0;
        }

        int8_t person_score = output_scores[PERSON_PERSON_INDEX];
        int8_t not_person_score = output_scores[PERSON_NOT_PERSON_INDEX];
        bool is_person = (person_score > not_person_score);

        if (is_person) {
            person_count++;
        }
        t1 = DWT->CYCCNT;
        time_inference += (t1 - t0);

        /* Time: display */
        t0 = DWT->CYCCNT;
        /* Update display every iteration */
        draw_person_detection_overlay(is_person, person_score);
        reload_watchdog(NULL, NULL);
        t1 = DWT->CYCCNT;
        time_display += (t1 - t0);

        /* Log progress every 10 iterations */
        if ((iter + 1) % 10 == 0) {
            LOG("Benchmark: %d/%d iterations", iter + 1, (int)iterations);
        }
    }

    uint32_t end_cycles = DWT->CYCCNT;

    /* Calculate timing - CPU runs at 64MHz */
    uint32_t elapsed_cycles = end_cycles - start_cycles;
    uint32_t total_ms = elapsed_cycles / 64000;  /* 64MHz = 64000 cycles/ms */
    uint32_t avg_ms = total_ms / (uint32_t)iterations;

    /* Static buffers - no free needed. */

    /* Convert timing to milliseconds */
    uint32_t memset_ms = time_memset / 64000;
    uint32_t capture_ms = time_capture / 64000;
    uint32_t wait_ready_ms = time_wait_ready / 64000;
    uint32_t read_jpeg_ms = time_read_jpeg / 64000;
    uint32_t decode_ms = time_decode / 64000;
    uint32_t upscale_ms = time_upscale / 64000;
    uint32_t inference_ms = time_inference / 64000;
    uint32_t display_ms = time_display / 64000;

    LOG("Benchmark complete: %d iterations, %d detections, %lu ms total, %lu ms avg",
        (int)iterations, person_count, total_ms, avg_ms);
    LOG("Timing breakdown (ms): memset=%lu capture=%lu wait=%lu read=%lu decode=%lu upscale=%lu infer=%lu display=%lu",
        memset_ms, capture_ms, wait_ready_ms, read_jpeg_ms, decode_ms, upscale_ms, inference_ms, display_ms);

    /* Return results table */
    lua_newtable(L);
    lua_pushinteger(L, iterations);
    lua_setfield(L, -2, "iterations");
    lua_pushinteger(L, person_count);
    lua_setfield(L, -2, "person_detections");
    lua_pushinteger(L, total_ms);
    lua_setfield(L, -2, "total_time_ms");
    lua_pushinteger(L, avg_ms);
    lua_setfield(L, -2, "avg_time_ms");

    /* Detailed timing breakdown */
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
/* Pure-inference benchmark                      */
/*-----------------------------------------------*/

/**
 * Send one BLE packet, retrying on busy/not-connected. The benchmark
 * runs for many minutes; a transient busy from the softdevice should
 * not abort the whole run.
 */
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
 * Pure-inference benchmark: tight loop over person_detect_infer().
 *
 *   - BENCH_WARMUP untimed inferences first to settle caches and the
 *     branch predictor.
 *   - Then BENCH_K cycles. Each cycle refills the input buffer with
 *     fresh pseudo-random bytes and times BENCH_N back-to-back
 *     inferences using the DWT cycle counter (64 MHz).
 *   - After each cycle the BENCH_N raw cycle counts are streamed back
 *     over BLE in 200-byte chunks, prefixed with a 4-byte LE cycle
 *     index. Cycles are separated by 0x01 0xFE 0xFE on the wire and
 *     the run terminates with 0x01 0xFF 0xFF 0x00 0x00.
 *
 * Lua: frame.experiment.run_inference_benchmark()
 * Returns: total number of timed inferences (BENCH_K * BENCH_N).
 */
static int lua_experiment_run_inference_benchmark(lua_State *L)
{
    if (!person_detect_is_initialized()) {
        luaL_error(L, "Person detect model not initialized");
        return 0;
    }
    if (!bluetooth_is_connected()) {
        luaL_error(L, "Bluetooth not connected");
        return 0;
    }

    LOG("bench: start (K=%u, N=%u, warmup=%u, payload=%u B)",
        (unsigned)BENCH_K, (unsigned)BENCH_N, (unsigned)BENCH_WARMUP,
        (unsigned)sizeof(s_bench_payload));

    /* Enable DWT cycle counter (same sequence as the existing pipeline
     * benchmark above). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Different seed per invocation so successive runs don't see the
     * exact same input distribution. */
    srand((unsigned int)DWT->CYCCNT);

    uint8_t *input = s_gray_buffer;
    int8_t output_scores[PERSON_OUTPUT_SIZE];

    /* One-time warmup. */
    for (size_t i = 0; i < PERSON_INPUT_SIZE; i++) {
        input[i] = (uint8_t)(rand() & 0xFF);
    }
    for (int w = 0; w < BENCH_WARMUP; w++) {
        if (person_detect_infer(input, output_scores) != TFLM_OK) {
            luaL_error(L, "warmup inference failed");
            return 0;
        }
        reload_watchdog(NULL, NULL);
    }
    LOG("bench: warmup done");

    /* BLE pacing constants. Inference bursts are long (seconds), then we
     * dump a few hundred bytes back-to-back. The softdevice notification
     * queue is shallow, so we leave a settle delay BEFORE the first
     * chunk of each cycle to let any pending TX_COMPLETE events drain,
     * then a generous inter-chunk delay. The 20 ms gap the existing
     * image-streaming code uses is too tight after a long silence. */
    const size_t CHUNK_SIZE = 100;
    const uint32_t SETTLE_MS = 250;
    const uint32_t CHUNK_DELAY_MS = 100;
    uint8_t chunk_buffer[CHUNK_SIZE + 1];
    chunk_buffer[0] = 0x01;  /* data flag */

    for (uint32_t k = 0; k < BENCH_K; k++) {
        /* Fresh random buffer for this cycle, kept stable through the
         * inner loop. Same buffer for all BENCH_N inferences. */
        for (size_t i = 0; i < PERSON_INPUT_SIZE; i++) {
            input[i] = (uint8_t)(rand() & 0xFF);
        }
        reload_watchdog(NULL, NULL);

        s_bench_payload.cycle_idx = k;

        /* Tight inference loop - this is what the benchmark measures.
         * Keep the loop body free of LOG / BLE / anything that might
         * touch the softdevice. */
        for (uint32_t i = 0; i < BENCH_N; i++) {
            uint32_t t0 = DWT->CYCCNT;
            tflm_status_t st = person_detect_infer(input, output_scores);
            uint32_t t1 = DWT->CYCCNT;
            if (st != TFLM_OK) {
                luaL_error(L, "inference failed at k=%u i=%u",
                           (unsigned)k, (unsigned)i);
                return 0;
            }
            /* uint32 wrap is fine: a single inference is far below 2^32
             * cycles (~67 s @ 64 MHz). */
            s_bench_payload.cycles[i] = t1 - t0;
            reload_watchdog(NULL, NULL);
        }

        LOG("bench: cycle %u inferences done; first=%u last=%u",
            (unsigned)k,
            (unsigned)s_bench_payload.cycles[0],
            (unsigned)s_bench_payload.cycles[BENCH_N - 1]);

        /* Let any TX_COMPLETE backlog drain before bursting the payload. */
        nrfx_systick_delay_ms(SETTLE_MS);
        reload_watchdog(NULL, NULL);

        /* Sacrificial "wake" notification. We've observed (RTT+host)
         * that the very first BLE notification after a long inference
         * silence is silently dropped at the link layer - the firmware
         * thinks it sent it (sd_ble_gatts_hvx returns NRF_SUCCESS) but
         * the host never sees it. Burning a 4-byte wake here means the
         * dropped slot is something we don't care about. The host
         * parser ignores 4-byte 0xAA 0xAA 0xAA 0xAA packets. */
        const uint8_t wake[5] = {0x01, 0xAA, 0xAA, 0xAA, 0xAA};
        (void)bench_send_with_retry(wake, 5);
        nrfx_systick_delay_ms(CHUNK_DELAY_MS);
        reload_watchdog(NULL, NULL);

        /* Stream this cycle's payload (4-byte LE index + BENCH_N x
         * uint32 LE cycle counts) in CHUNK_SIZE chunks. */
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

        /* Cycle separator (arrives host-side as a 2-byte packet
         * 0xFE 0xFE - the data flag is stripped by frameutils). */
        nrfx_systick_delay_ms(CHUNK_DELAY_MS);
        const uint8_t separator[3] = {0x01, 0xFE, 0xFE};
        if (!bench_send_with_retry(separator, 3)) {
            luaL_error(L, "BLE separator failed at cycle %u", (unsigned)k);
            return 0;
        }
        LOG("bench: cycle %u/%u separator sent",
            (unsigned)(k + 1), (unsigned)BENCH_K);
    }

    /* End marker (host-side: 4-byte packet 0xFF 0xFF 0x00 0x00). */
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
    return "VWW";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    lua_pushcfunction(L, lua_experiment_run_person_detection);
    lua_setfield(L, -2, "run_person_detection");

    lua_pushcfunction(L, lua_experiment_run_person_detection_benchmark);
    lua_setfield(L, -2, "run_person_detection_benchmark");

    lua_pushcfunction(L, lua_experiment_run_inference_benchmark);
    lua_setfield(L, -2, "run_inference_benchmark");
}
