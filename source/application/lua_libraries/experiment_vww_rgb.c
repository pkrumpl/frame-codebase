/*
 * VWW_RGB (Visual Wake Words RGB) Experiment Implementation
 *
 * Person detection experiment using the VWW RGB model.
 * This file is compiled when ML_EXPERIMENT=VWW_RGB is set.
 *
 * Key differences from VWW (grayscale):
 * - Input size: 27,648 bytes (96x96x3) vs 9,216 bytes (96x96x1)
 * - JPEG decoder outputs RGB888 instead of grayscale
 * - Model file: person_detect_rgb.h instead of person_detect.h
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
#define OUTPUT_SIZE      96    /* ML input size (96x96) */
#define NUM_CHANNELS     3     /* RGB */

/* Buffer sizes */
#define RGB_CAPTURE_BYTES  (SCALED_SIZE * SCALED_SIZE * NUM_CHANNELS)  /* 24,300 */
#define RGB_OUTPUT_BYTES   (OUTPUT_SIZE * OUTPUT_SIZE * NUM_CHANNELS)  /* 27,648 */

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
 * Run person detection on camera image using RGB model.
 *
 * Image pipeline: 720x720 JPEG -> scale=3 (90x90 RGB) -> upscale to 96x96 RGB
 * Output: Display PERSON (green) or NO PERSON (red) overlay
 *
 * Protocol:
 *   [IMAGE DATA]     27648 bytes (96x96x3 RGB)
 *   [SEPARATOR]      0x01 0xFE 0xFE
 *   [PREDICTIONS]    2 bytes (not_person_score, person_score)
 *   [END MARKER]     0x01 0xFF 0xFF 0x00 0x00
 */
static int lua_experiment_run_person_detection(lua_State *L)
{
    const size_t MAX_JPEG_SIZE = 25 * 1024;
    const size_t CHUNK_SIZE = 200;
    const size_t READ_CHUNK_SIZE = 512;

    size_t jpeg_size = 0;
    uint16_t actual_width, actual_height;
    int result;

    /* Check if person detect model is initialized */
    if (!person_detect_is_initialized()) {
        luaL_error(L, "Person detect RGB model not initialized");
        return 0;
    }

    /* Allocate buffers (RGB: 3 bytes per pixel) */
    uint8_t *jpeg_buffer = malloc(MAX_JPEG_SIZE);
    uint8_t *temp_buffer = malloc(RGB_CAPTURE_BYTES);  /* 90x90x3 = 24.3KB */
    uint8_t *rgb_buffer = malloc(RGB_OUTPUT_BYTES);    /* 96x96x3 = 27.6KB */
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
    /* DEV_KIT: Use hardcoded test JPEG data instead of camera */
    free(jpeg_buffer);  /* Not needed for test path */
    LOG("DEV_KIT RGB: Using hardcoded test JPEG data (%u bytes)", test_jpeg_size);

    /* Decode test JPEG to 90x90 RGB */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);  /* scale=3 (1/8), no rotation */
    if (result != 0) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }
    LOG("DEV_KIT RGB: decoded %dx%d", actual_width, actual_height);

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

    /* ===== Step 3: Capture image ===== */
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

    /* ===== Step 4: Wait for image ready ===== */
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

    /* ===== Step 5: Read JPEG data ===== */
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

    LOG("Person detect RGB: JPEG size = %u bytes", jpeg_size);

    /* ===== Step 6: Decode JPEG to 90x90 RGB ===== */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);  /* scale=3 (1/8), no rotation */
    free(jpeg_buffer);

    if (result != 0) {
        free(temp_buffer);
        free(rgb_buffer);
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }

    LOG("Person detect RGB: decoded %dx%d, UPSCALING to %dx%d", actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);
#endif /* !DEV_KIT_BUILD */

    /* ===== Step 7: UPSCALE 90x90 RGB to 96x96 RGB with 90 CCW rotation ===== */
    reload_watchdog(NULL, NULL);
    upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);
    free(temp_buffer);

    /* ===== Step 8: Run person detection inference ===== */
    reload_watchdog(NULL, NULL);
    int8_t output_scores[PERSON_OUTPUT_SIZE];  /* 2 bytes */

    tflm_status_t infer_status = person_detect_infer(rgb_buffer, output_scores);
    if (infer_status != TFLM_OK) {
        free(rgb_buffer);
        luaL_error(L, "Person detect RGB inference failed");
        return 0;
    }

    int8_t not_person_score = output_scores[PERSON_NOT_PERSON_INDEX];
    int8_t person_score = output_scores[PERSON_PERSON_INDEX];
    bool is_person = (person_score > not_person_score);

    LOG("Person detect RGB: not_person=%d, person=%d, result=%s",
        not_person_score, person_score, is_person ? "PERSON" : "NO PERSON");

    /* ===== Step 9: Display overlay ===== */
    draw_person_detection_overlay(is_person, person_score);
    reload_watchdog(NULL, NULL);

    /* ===== Step 10: Send RGB image data via Bluetooth ===== */
    size_t total_bytes = RGB_OUTPUT_BYTES;  /* 27648 */
    size_t offset = 0;

    uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
    if (!chunk_buffer) {
        free(rgb_buffer);
        luaL_error(L, "chunk allocation failed");
        return 0;
    }
    chunk_buffer[0] = 0x01;  /* Data flag */

    while (offset < total_bytes) {
        size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
        memcpy(chunk_buffer + 1, rgb_buffer + offset, chunk);
        bluetooth_send_data(chunk_buffer, chunk + 1);
        offset += chunk;
        nrfx_systick_delay_ms(20);
        reload_watchdog(NULL, NULL);
    }

    free(rgb_buffer);

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
        luaL_error(L, "Person detect RGB model not initialized");
        return 0;
    }

    /* Allocate buffers - reused across all iterations */
    uint8_t *temp_buffer = malloc(RGB_CAPTURE_BYTES);  /* 90x90x3 = 24.3KB */
    uint8_t *rgb_buffer = malloc(RGB_OUTPUT_BYTES);    /* 96x96x3 = 27.6KB */
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
    LOG("Benchmark RGB: camera initialized");
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
        memset(temp_buffer, 0, RGB_CAPTURE_BYTES);
        memset(rgb_buffer, 0, RGB_OUTPUT_BYTES);
        t1 = DWT->CYCCNT;
        time_memset += (t1 - t0);

#ifdef DEV_KIT_BUILD
        /* Time: decode (DEV_KIT only - no capture/read) */
        t0 = DWT->CYCCNT;
        /* DEV_KIT: Use hardcoded test JPEG data (same image each iteration) */
        result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                        temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                        &actual_width, &actual_height,
                                        3, false);  /* scale=3 (1/8), no rotation */
        if (result != 0) {
            free(temp_buffer);
            free(rgb_buffer);
            luaL_error(L, "RGB decode failed: %d", result);
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

        /* Time: decode JPEG */
        t0 = DWT->CYCCNT;
        /* Decode JPEG to 90x90 RGB */
        reload_watchdog(NULL, NULL);
        result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                        temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                        &actual_width, &actual_height,
                                        3, false);  /* scale=3 (1/8), no rotation */
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

        /* Time: upscale */
        t0 = DWT->CYCCNT;
        /* Upscale 90x90 RGB to 96x96 RGB with 90 CCW rotation */
        reload_watchdog(NULL, NULL);
        upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);
        t1 = DWT->CYCCNT;
        time_upscale += (t1 - t0);

        /* Time: inference */
        t0 = DWT->CYCCNT;
        /* Run person detection inference */
        reload_watchdog(NULL, NULL);
        int8_t output_scores[PERSON_OUTPUT_SIZE];

        tflm_status_t infer_status = person_detect_infer(rgb_buffer, output_scores);
        if (infer_status != TFLM_OK) {
            free(temp_buffer);
            free(rgb_buffer);
#ifndef DEV_KIT_BUILD
            free(jpeg_buffer);
#endif
            luaL_error(L, "Person detect RGB inference failed");
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
            LOG("Benchmark RGB: %d/%d iterations", iter + 1, (int)iterations);
        }
    }

    uint32_t end_cycles = DWT->CYCCNT;

    /* Calculate timing - CPU runs at 64MHz */
    uint32_t elapsed_cycles = end_cycles - start_cycles;
    uint32_t total_ms = elapsed_cycles / 64000;  /* 64MHz = 64000 cycles/ms */
    uint32_t avg_ms = total_ms / (uint32_t)iterations;

    /* Cleanup */
    free(temp_buffer);
    free(rgb_buffer);
#ifndef DEV_KIT_BUILD
    free(jpeg_buffer);
#endif

    /* Convert timing to milliseconds */
    uint32_t memset_ms = time_memset / 64000;
    uint32_t capture_ms = time_capture / 64000;
    uint32_t wait_ready_ms = time_wait_ready / 64000;
    uint32_t read_jpeg_ms = time_read_jpeg / 64000;
    uint32_t decode_ms = time_decode / 64000;
    uint32_t upscale_ms = time_upscale / 64000;
    uint32_t inference_ms = time_inference / 64000;
    uint32_t display_ms = time_display / 64000;

    LOG("Benchmark RGB complete: %d iterations, %d detections, %lu ms total, %lu ms avg",
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
/* Experiment Interface Implementation           */
/*-----------------------------------------------*/

const char* experiment_get_name(void)
{
    return "VWW_RGB";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    lua_pushcfunction(L, lua_experiment_run_person_detection);
    lua_setfield(L, -2, "run_person_detection");

    lua_pushcfunction(L, lua_experiment_run_person_detection_benchmark);
    lua_setfield(L, -2, "run_person_detection_benchmark");
}
