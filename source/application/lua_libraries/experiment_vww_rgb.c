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
#include <stdio.h>
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

/* Static working buffers in BSS. Rationale:
 *   - Streaming inference calls these every frame; per-call malloc/free
 *     of buffers this large fragments the heap fast (a freed 25 KB
 *     slot can't satisfy a 27 KB request) which then causes the next
 *     allocation to fail mid-stream.
 *   - Static placement is contiguous at link time, so no allocator
 *     involvement and no fragmentation.
 *   - s_rgb_buffer is dual-purposed: it holds the raw JPEG during
 *     read+decode, then the final 96x96 RGB image after upscale. The
 *     decoder consumes the JPEG bytes before the upscale overwrites
 *     them, so the reuse is safe and saves ~25 KB vs a separate JPEG
 *     buffer. JPEG must fit in RGB_OUTPUT_BYTES (27,648 B), which is
 *     above the typical 720x720 MEDIUM-quality JPEG size. */
static uint8_t s_temp_buffer[RGB_CAPTURE_BYTES];  /* 24,300 B (decode dst) */
static uint8_t s_rgb_buffer[RGB_OUTPUT_BYTES];    /* 27,648 B (JPEG then RGB) */

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
 * uint32_t so sizeof == 4 + 4*BENCH_N == 516 bytes, no padding. */
static struct {
    uint32_t cycle_idx;
    uint32_t cycles[BENCH_N];
} s_bench_payload;

/* Pipeline benchmark constants. Names deliberately distinct from the
 * BENCH_* set above so the two benchmarks coexist without collision.
 * The pipeline benchmark runs the full image-acquisition->preprocess->
 * inference->display loop on real camera frames; the host receives one
 * "cycle" per stage with PIPE_N per-iteration cycle counts inside. */
#define PIPE_WARMUP   5
#define PIPE_N        30
#define PIPE_STAGES   7

enum {
    STAGE_CAPTURE     = 0,
    STAGE_WAIT_READY  = 1,
    STAGE_READ_JPEG   = 2,
    STAGE_DECODE      = 3,
    STAGE_UPSCALE     = 4,
    STAGE_INFERENCE   = 5,
    STAGE_DISPLAY     = 6,
};

/* Per-iteration DWT cycle counts collected during the measured loop.
 * 7 stages * 30 iters * 4 B = 840 B BSS. Read only at TX time. */
static uint32_t s_pipe_stage_cycles[PIPE_STAGES][PIPE_N];

/* Wire payload for one stage: 4-byte LE stage index followed by PIPE_N
 * raw DWT cycle counts (uint32 LE). 4 + 4*30 == 124 B. Filled per-stage
 * at TX time; the measured loop only writes s_pipe_stage_cycles. */
static struct {
    uint32_t stage_idx;
    uint32_t cycles[PIPE_N];
} s_pipe_payload;

/* Forward decl: bench_send_with_retry is defined alongside the inference
 * benchmark near the bottom of the file. Both the streaming path and the
 * benchmarks use it so partial BLE notifications don't get silently dropped
 * when the SoftDevice's send queue is saturated. */
static bool bench_send_with_retry(const uint8_t *data, size_t length);

/* Forward decl: rendered pixel width of a system-font string. Defined in
 * display.c. Used by draw_person_detection_overlay to center the result
 * text horizontally on the display. */
extern uint16_t display_text_pixel_width(const char *string);

/*-----------------------------------------------*/
/* Person Detection Overlay                      */
/*-----------------------------------------------*/

/**
 * Draw the person-detection result as a colored text string near the bottom
 * of the Frame display.
 *
 * Confidence is a simple linear margin: 50% when the two scores tie, 100%
 * when the predicted class fully dominates the other. No exp()/softmax in
 * firmware. The text is drawn via frame.display.text (system-font glyphs)
 * so it picks up the same rendering path the calibration screen uses; the
 * FPGA back-buffer is cleared automatically on the show() that follows
 * (see display_buffers.sv).
 */
static void draw_person_detection_overlay(lua_State *L,
                                          bool is_person,
                                          int8_t person_score,
                                          int8_t not_person_score)
{
    int margin = (int)person_score - (int)not_person_score;
    if (margin < 0) margin = -margin;
    if (margin > 255) margin = 255;
    int conf = 50 + (margin * 50) / 255;
    if (conf < 50) conf = 50;
    if (conf > 100) conf = 100;

    char buf[32];
    snprintf(buf, sizeof(buf),
             is_person ? "PERSON  %d%%" : "NO PERSON  %d%%",
             conf);

    /* Center the result text on the 640x400 display. System-font glyphs are
     * 48 px tall, so y = (400 - 48) / 2 = 176 puts the glyph row vertically
     * centered. The horizontal anchor is computed from the rendered width
     * so 'PERSON 92%' and 'NO PERSON 73%' both look centered. */
    uint16_t text_w = display_text_pixel_width(buf);
    lua_Integer text_x = (640 - (lua_Integer)text_w) / 2;
    if (text_x < 1) text_x = 1;  /* frame.display uses 1-based coordinates */
    const lua_Integer text_y = 176;
    const char *color_name = is_person ? "GREEN" : "RED";

    /* frame.display.text(buf, text_x, text_y, {color = color_name}) */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "display");
    lua_getfield(L, -1, "text");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        return;
    }
    lua_pushstring(L, buf);
    lua_pushinteger(L, text_x);
    lua_pushinteger(L, text_y);
    lua_newtable(L);
    lua_pushstring(L, color_name);
    lua_setfield(L, -2, "color");
    if (lua_pcall(L, 4, 0, 0) != LUA_OK) {
        /* Error string + the two outer table-getfield results. */
        lua_pop(L, 3);
        return;
    }
    lua_pop(L, 2); /* frame, display */

    /* frame.display.show() — buffer swap. */
    spi_write(FPGA, 0x14, NULL, 0);
}

/*-----------------------------------------------*/
/* Person Detection Lua Functions                */
/*-----------------------------------------------*/

#ifndef DEV_KIT_BUILD
/* Wake the camera (frame.camera.power_save(false)) and run 5 autoexposure
 * cycles. delay_ms is the spacing between cycles — 100 ms for the existing
 * host-driven slow path (preserves prior timing), 1000 ms for the on-device
 * demo so the "Calibrating..." screen stays visible long enough to read.
 *
 * frame.camera.auto is called with an empty options table, so AE uses its
 * own defaults (exposure=0.1, analog_gain_limit=16). The host-driven BLE
 * demo tunes these from Python via run_calibration. */
static void do_wake_and_autoexpose(lua_State *L, uint32_t delay_ms)
{
    /* Wake camera. */
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

    /* 5x camera.auto({}) — use AE defaults. */
    for (int i = 0; i < 5; i++) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "auto");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 3);
            luaL_error(L, "camera.auto not found");
            return;
        }
        lua_newtable(L);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            luaL_error(L, "camera.auto failed");
            return;
        }
        lua_pop(L, 3);
        nrfx_systick_delay_ms(delay_ms);
        reload_watchdog(NULL, NULL);
    }
}
#endif /* !DEV_KIT_BUILD */

/**
 * Run person detection on camera image using RGB model.
 *
 * If `skip_camera_init` is true, Steps 1-2 (wake + 5x autoexposure) are
 * skipped. The host is expected to have already woken the camera and
 * driven autoexposure (e.g. during the demo calibration phase). When
 * false, the slow path runs both initialization steps. DEV_KIT builds
 * ignore this flag because they don't touch the camera.
 *
 * If `skip_ble_tx` is true, the image + scores are NOT streamed over BLE
 * after inference. Used by the on-device tap-triggered demo where no host
 * is listening; saves ~3 s of BLE traffic per frame.
 *
 * Image pipeline: 720x720 JPEG -> scale=3 (90x90 RGB) -> upscale to 96x96 RGB
 * Output: Display PERSON (green) or NO PERSON (red) overlay
 *
 * Protocol (when skip_ble_tx is false):
 *   [IMAGE DATA]     27648 bytes (96x96x3 RGB)
 *   [SEPARATOR]      0x01 0xFE 0xFE
 *   [PREDICTIONS]    2 bytes (not_person_score, person_score)
 *   [END MARKER]     0x01 0xFF 0xFF 0x00 0x00
 */
static int run_person_detection_body(lua_State *L,
                                     bool skip_camera_init,
                                     bool skip_ble_tx)
{
    /* Max JPEG size == RGB_OUTPUT_BYTES because the JPEG is read into
     * s_rgb_buffer (which is unused until upscale). 27,648 B is above
     * typical 720x720 MEDIUM JPEG sizes. */
    const size_t MAX_JPEG_SIZE = RGB_OUTPUT_BYTES;
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

    /* Static buffers (see file-scope declarations). jpeg_buffer and
     * rgb_buffer alias the same 27.6 KB BSS region - the JPEG bytes are
     * consumed before the upscale overwrites them. */
    uint8_t *jpeg_buffer = s_rgb_buffer;
    uint8_t *temp_buffer = s_temp_buffer;
    uint8_t *rgb_buffer  = s_rgb_buffer;

    memset(temp_buffer, 0, RGB_CAPTURE_BYTES);
    memset(rgb_buffer, 0, RGB_OUTPUT_BYTES);

#ifdef DEV_KIT_BUILD
    /* DEV_KIT: Use hardcoded test JPEG data instead of camera */
    (void)jpeg_buffer;  /* not used in test path (would alias s_rgb_buffer anyway) */
    LOG("DEV_KIT RGB: Using hardcoded test JPEG data (%u bytes)", test_jpeg_size);

    /* Decode test JPEG to 90x90 RGB */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);  /* scale=3 (1/8), no rotation */
    if (result != 0) {
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }
    LOG("DEV_KIT RGB: decoded %dx%d", actual_width, actual_height);

#else /* !DEV_KIT_BUILD */
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);

    if (!skip_camera_init) {
        /* Steps 1-2: wake + 5x autoexposure, 100 ms between cycles. */
        do_wake_and_autoexpose(L, 100);
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

    LOG("Person detect RGB: JPEG size = %u bytes", jpeg_size);

    /* ===== Step 6: Decode JPEG to 90x90 RGB ===== */
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);  /* scale=3 (1/8), no rotation */
    /* jpeg_buffer bytes no longer needed - upscale below overwrites s_rgb_buffer */

    if (result != 0) {
        luaL_error(L, "RGB decode failed: %d", result);
        return 0;
    }

    LOG("Person detect RGB: decoded %dx%d, UPSCALING to %dx%d", actual_width, actual_height, OUTPUT_SIZE, OUTPUT_SIZE);
#endif /* !DEV_KIT_BUILD */

    /* ===== Step 7: UPSCALE 90x90 RGB to 96x96 RGB with 90 CCW rotation =====
     * Reads s_temp_buffer, writes s_rgb_buffer (overwriting the JPEG bytes
     * that were previously there via jpeg_buffer alias). */
    reload_watchdog(NULL, NULL);
    upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);

    /* ===== Step 8: Run person detection inference ===== */
    reload_watchdog(NULL, NULL);
    int8_t output_scores[PERSON_OUTPUT_SIZE];  /* 2 bytes */

    tflm_status_t infer_status = person_detect_infer(rgb_buffer, output_scores);
    if (infer_status != TFLM_OK) {
        luaL_error(L, "Person detect RGB inference failed");
        return 0;
    }

    int8_t not_person_score = output_scores[PERSON_NOT_PERSON_INDEX];
    int8_t person_score = output_scores[PERSON_PERSON_INDEX];
    bool is_person = (person_score > not_person_score);

    LOG("Person detect RGB: not_person=%d, person=%d, result=%s",
        not_person_score, person_score, is_person ? "PERSON" : "NO PERSON");

    /* ===== Step 9: Display overlay ===== */
    draw_person_detection_overlay(L, is_person, person_score, not_person_score);
    reload_watchdog(NULL, NULL);

    size_t total_bytes = RGB_OUTPUT_BYTES;  /* 27648 */

    if (!skip_ble_tx) {
        /* ===== Step 10: Send RGB image data via Bluetooth ===== */
        size_t offset = 0;

        uint8_t *chunk_buffer = malloc(CHUNK_SIZE + 1);
        if (!chunk_buffer) {
            luaL_error(L, "chunk allocation failed");
            return 0;
        }
        chunk_buffer[0] = 0x01;  /* Data flag */

        /* bench_send_with_retry retries when sd_ble_gatts_hvx returns
         * NRF_ERROR_RESOURCES (BLE TX queue full). A bare bluetooth_send_data()
         * would silently drop the chunk. With per-frame autoexposure removed,
         * frames stream back-to-back and the queue saturates without retry. */
        while (offset < total_bytes) {
            size_t chunk = (total_bytes - offset > CHUNK_SIZE) ? CHUNK_SIZE : (total_bytes - offset);
            memcpy(chunk_buffer + 1, rgb_buffer + offset, chunk);
            (void)bench_send_with_retry(chunk_buffer, chunk + 1);
            offset += chunk;
            nrfx_systick_delay_ms(20);
            reload_watchdog(NULL, NULL);
        }

        /* ===== Step 11: Send separator ===== */
        nrfx_systick_delay_ms(50);
        uint8_t separator[3] = {0x01, 0xFE, 0xFE};
        (void)bench_send_with_retry(separator, 3);

        /* ===== Step 12: Send predictions (2 bytes) ===== */
        nrfx_systick_delay_ms(50);
        chunk_buffer[0] = 0x01;
        memcpy(chunk_buffer + 1, output_scores, PERSON_OUTPUT_SIZE);
        (void)bench_send_with_retry(chunk_buffer, PERSON_OUTPUT_SIZE + 1);

        /* ===== Step 13: Send end marker ===== */
        nrfx_systick_delay_ms(100);
        uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
        (void)bench_send_with_retry(end_marker, 5);

        free(chunk_buffer);
    }

    lua_pushinteger(L, total_bytes + PERSON_OUTPUT_SIZE);
    return 1;
}

/* Lua entry: full slow path (wakes camera + 5x autoexposure + detect),
 * streams over BLE. Used when a host is driving the demo. */
static int lua_experiment_run_person_detection(lua_State *L)
{
    return run_person_detection_body(L,
                                     /* skip_camera_init */ false,
                                     /* skip_ble_tx     */ false);
}

/* Lua entry: fast path that skips wake + autoexposure but still streams
 * over BLE. The host must have already powered the camera on and driven
 * autoexposure (typically via the demo calibration phase). Used for
 * steady-state host-driven streaming. */
static int lua_experiment_run_person_detection_fast(lua_State *L)
{
    return run_person_detection_body(L,
                                     /* skip_camera_init */ true,
                                     /* skip_ble_tx     */ false);
}

/* Number of detection iterations the on-device tap-triggered demo runs
 * between the calibration screen and the return to the pairing screen. */
#define VWW_DEMO_ITERATIONS 30

/* Lua entry: fully on-device demo. Triggered by a double-tap when the
 * tap handler in luaport.c sees that this build defines the function.
 *
 * Flow:
 *   1. Draw the ELSS logo (U+F0011) + "Calibrating..." on the display.
 *   2. Wake the camera + run 5x autoexposure with 1 s spacing so the
 *      calibration screen stays visible.
 *   3. Run VWW_DEMO_ITERATIONS detections; each draws the centered
 *      result text on the display. BLE TX is skipped (no host expected).
 *   4. Restore the standard "Frame is Paired" screen.
 *   5. Power the camera back down.
 */
static int lua_experiment_run_vww_demo(lua_State *L)
{
#ifdef DEV_KIT_BUILD
    /* No camera path on the dev kit; nothing useful to do. */
    (void)L;
    return 0;
#else
    /* 1. Calibration screen. The ELSS logo UTF-8 is F3 B0 80 91. */
    (void)luaL_dostring(L,
        "frame.display.text('\xF3\xB0\x80\x91', 102, 130, {color='WHITE'});"
        "frame.display.text('Calibrating...', 240, 290, {color='WHITE'});"
        "frame.display.show()");

    /* 2. Wake + autoexposure with the slower demo cadence so the logo
     *    is on screen for ~5 s before detections start. */
    do_wake_and_autoexpose(L, 1000);

    /* 3. N detections, on-device only (no BLE TX). */
    for (int i = 0; i < VWW_DEMO_ITERATIONS; i++) {
        run_person_detection_body(L,
                                  /* skip_camera_init */ true,
                                  /* skip_ble_tx     */ true);
        /* The body pushes one int return value; drop it so the stack
         * stays balanced across iterations. */
        lua_pop(L, 1);
        reload_watchdog(NULL, NULL);
    }

    /* The last detection's show() triggers a sequential back-buffer
     * clear in the FPGA (display_buffers.sv:165, ~512000 cycles). If we
     * start writing the pairing screen immediately, our draws race the
     * clear sweep: some pixels get erased, and addresses the sweep
     * hasn't reached yet still hold the previous frame's content.
     * Wait long enough for the clear to finish before drawing. */
    nrfx_systick_delay_ms(50);

    /* 4. Restore the standard pairing screen (mirrors
     *    luaport.c:show_pairing_screen for the paired case). */
    (void)luaL_dostring(L,
        "frame.display.text('Frame is Paired', 185, 140);"
        "frame.display.text("
        "'Frame '..frame.bluetooth.address():sub(-2, -1), "
        "245, 210, {color='ORANGE'});"
        "frame.display.show()");

    /* 5. Camera back to sleep. */
    (void)luaL_dostring(L, "frame.camera.power_save(true)");

    return 0;
#endif
}

/* bench_send_with_retry forward-declared near the top of the file. */

/**
 * Run one full pipeline iteration: capture -> wait_ready -> read_jpeg ->
 * decode -> upscale -> inference -> display. When `collect` is true,
 * each stage's DWT cycle delta is written into
 * s_pipe_stage_cycles[STAGE_*][iter]; when false (warmup), the timings
 * are discarded. The same code path runs in both phases so caches,
 * branch predictors, FPGA SPI state, and the camera capture pipeline
 * are warm by the time measurement starts.
 *
 * On any failure raises luaL_error and returns non-zero so the caller
 * can bail out cleanly. Note: luaL_error long-jumps, so the non-zero
 * return is defensive only.
 */
static int pipe_run_iteration(lua_State *L, int iter, bool collect,
                              int *out_is_person)
{
    uint16_t actual_width, actual_height;
    int result;
    uint32_t t0, t1;

    uint8_t *temp_buffer = s_temp_buffer;
    uint8_t *rgb_buffer  = s_rgb_buffer;
#ifndef DEV_KIT_BUILD
    uint8_t *jpeg_buffer = s_rgb_buffer;  /* aliased; safe because the
                                             decode below consumes JPEG
                                             bytes before upscale
                                             overwrites the buffer. */
    const size_t MAX_JPEG_SIZE   = RGB_OUTPUT_BYTES;
    const size_t READ_CHUNK_SIZE = 512;
    size_t jpeg_size = 0;
#endif

    /* Bookkeeping memsets - NOT a measured stage. The user explicitly
     * dropped the previous "memset" stage; the calls themselves stay
     * because the inference path needs a clean buffer when the JPEG is
     * shorter than RGB_OUTPUT_BYTES. */
    memset(temp_buffer, 0, RGB_CAPTURE_BYTES);
    memset(rgb_buffer, 0, RGB_OUTPUT_BYTES);
#ifndef DEV_KIT_BUILD
    memset(jpeg_buffer, 0, MAX_JPEG_SIZE);
#endif
    reload_watchdog(NULL, NULL);

#ifdef DEV_KIT_BUILD
    /* DEV_KIT path: no camera. Stages 0/1/2 are forced to zero so they
     * are visible in the host CSV but do not pollute statistics. The
     * decode stage uses the embedded test JPEG. */
    if (collect) {
        s_pipe_stage_cycles[STAGE_CAPTURE][iter]    = 0;
        s_pipe_stage_cycles[STAGE_WAIT_READY][iter] = 0;
        s_pipe_stage_cycles[STAGE_READ_JPEG][iter]  = 0;
    }

    t0 = DWT->CYCCNT;
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(test_jpeg_data, test_jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);  /* scale=3 (1/8), no rotation */
    if (result != 0) {
        luaL_error(L, "RGB decode failed: %d", result);
        return 1;
    }
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_DECODE][iter] = t1 - t0;
#else
    /* Stage 0: capture */
    t0 = DWT->CYCCNT;
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "capture");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        luaL_error(L, "camera.capture not found");
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, CAPTURE_SIZE);
    lua_setfield(L, -2, "resolution");
    lua_pushstring(L, "MEDIUM");
    lua_setfield(L, -2, "quality");
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        lua_pop(L, 3);
        luaL_error(L, "capture failed");
        return 1;
    }
    lua_pop(L, 2);
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_CAPTURE][iter] = t1 - t0;

    /* Stage 1: wait_ready */
    t0 = DWT->CYCCNT;
    {
        uint32_t timeout = 1000000;
        bool ready = false;
        while (timeout-- && !ready) {
            lua_getglobal(L, "frame");
            lua_getfield(L, -1, "camera");
            lua_getfield(L, -1, "image_ready");
            if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
                lua_pop(L, 3);
                luaL_error(L, "image_ready failed");
                return 1;
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
            return 1;
        }
    }
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_WAIT_READY][iter] = t1 - t0;

    /* Stage 2: read_jpeg */
    t0 = DWT->CYCCNT;
    while (jpeg_size < MAX_JPEG_SIZE) {
        lua_getglobal(L, "frame");
        lua_getfield(L, -1, "camera");
        lua_getfield(L, -1, "read");
        lua_pushinteger(L, READ_CHUNK_SIZE);
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            lua_pop(L, 3);
            luaL_error(L, "read failed");
            return 1;
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
    if (collect) s_pipe_stage_cycles[STAGE_READ_JPEG][iter] = t1 - t0;

    /* Stage 3: decode */
    t0 = DWT->CYCCNT;
    reload_watchdog(NULL, NULL);
    result = jpeg_decode_rgb_scaled(jpeg_buffer, jpeg_size,
                                    temp_buffer, SCALED_SIZE, SCALED_SIZE,
                                    &actual_width, &actual_height,
                                    3, false);  /* scale=3 (1/8), no rotation */
    if (result != 0) {
        luaL_error(L, "RGB decode failed: %d", result);
        return 1;
    }
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_DECODE][iter] = t1 - t0;
#endif /* !DEV_KIT_BUILD */

    /* Stage 4: upscale */
    t0 = DWT->CYCCNT;
    reload_watchdog(NULL, NULL);
    upscale_90_to_96_rgb_with_rotation(temp_buffer, rgb_buffer);
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_UPSCALE][iter] = t1 - t0;

    /* Stage 5: inference */
    int8_t output_scores[PERSON_OUTPUT_SIZE];
    t0 = DWT->CYCCNT;
    reload_watchdog(NULL, NULL);
    if (person_detect_infer(rgb_buffer, output_scores) != TFLM_OK) {
        luaL_error(L, "Person detect RGB inference failed at iter %d", iter);
        return 1;
    }
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_INFERENCE][iter] = t1 - t0;

    bool is_person = (output_scores[PERSON_PERSON_INDEX]
                      > output_scores[PERSON_NOT_PERSON_INDEX]);

    /* Stage 6: display */
    t0 = DWT->CYCCNT;
    draw_person_detection_overlay(L,
                                  is_person,
                                  output_scores[PERSON_PERSON_INDEX],
                                  output_scores[PERSON_NOT_PERSON_INDEX]);
    reload_watchdog(NULL, NULL);
    t1 = DWT->CYCCNT;
    if (collect) s_pipe_stage_cycles[STAGE_DISPLAY][iter] = t1 - t0;

    if (out_is_person) *out_is_person = is_person ? 1 : 0;
    return 0;
}

/**
 * Pipeline benchmark: warmup + measured loop + binary BLE TX.
 *
 *   - PIPE_WARMUP untimed full-pipeline iterations to warm caches /
 *     branch predictor / camera state.
 *   - PIPE_N measured iterations, recording per-iteration DWT cycle
 *     counts for each of PIPE_STAGES stages.
 *   - After the measured loop, stream s_pipe_stage_cycles back over BLE
 *     using the same chunked protocol as run_inference_benchmark: one
 *     "cycle" per stage, with cycle_idx = stage id (0..6) and
 *     cycles[] = PIPE_N per-iteration cycle counts.
 *
 * Lua: frame.experiment.run_person_detection_benchmark()  (no args)
 * Returns: PIPE_N (purely informational; host doesn't read it).
 */
static int lua_experiment_run_person_detection_benchmark(lua_State *L)
{
    /* Check if person detect model is initialized */
    if (!person_detect_is_initialized()) {
        luaL_error(L, "Person detect RGB model not initialized");
        return 0;
    }
    if (!bluetooth_is_connected()) {
        luaL_error(L, "Bluetooth not connected");
        return 0;
    }

    LOG("pipe-bench: start (warmup=%u, N=%u, stages=%u, payload=%u B)",
        (unsigned)PIPE_WARMUP, (unsigned)PIPE_N, (unsigned)PIPE_STAGES,
        (unsigned)sizeof(s_pipe_payload));

#ifndef DEV_KIT_BUILD
    /* ===== Initialize camera ONCE before warmup loop =====
     * Auto-exposure runs only once - subsequent warmup/measured frames
     * see the same exposure settings (the user explicitly chose this
     * order; warmup #1 may still have some exposure transient). */
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
    LOG("pipe-bench: camera initialized (auto-exposure x5)");
#endif

    /* Enable DWT cycle counter (same sequence as the pure-inference
     * benchmark below). Cycle wraparound is safe here: at 64 MHz one
     * stage would need >67 s of contiguous cycles to wrap, and no stage
     * approaches that. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    /* ===== Warmup: PIPE_WARMUP full-pipeline iterations, untimed ===== */
    for (int i = 0; i < PIPE_WARMUP; i++) {
        reload_watchdog(NULL, NULL);
        if (pipe_run_iteration(L, i, /*collect=*/false, NULL) != 0) {
            return 0;  /* luaL_error already raised */
        }
    }
    LOG("pipe-bench: warmup done");

    /* ===== Measured: PIPE_N iterations, fill s_pipe_stage_cycles ===== */
    int person_count = 0;
    for (int iter = 0; iter < PIPE_N; iter++) {
        reload_watchdog(NULL, NULL);
        int is_person = 0;
        if (pipe_run_iteration(L, iter, /*collect=*/true, &is_person) != 0) {
            return 0;
        }
        if (is_person) person_count++;
        if ((iter + 1) % 10 == 0) {
            LOG("pipe-bench: %d/%d iters", iter + 1, (int)PIPE_N);
        }
    }
    LOG("pipe-bench: measured loop done, person_count=%d", person_count);

#ifndef DEV_KIT_BUILD
    /* Camera off after measurement: leaves the device in the same idle
     * state as the pure-inference benchmark for clean teardown. */
    lua_getglobal(L, "frame");
    lua_getfield(L, -1, "camera");
    lua_getfield(L, -1, "power_save");
    if (lua_isfunction(L, -1)) {
        lua_pushboolean(L, 1);  /* power_save(true) */
        (void)lua_pcall(L, 1, 0, 0);
    } else {
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
#endif

    /* ===== BLE TX: one cycle per stage =====
     * Same chunked protocol as run_inference_benchmark below: 250 ms
     * settle, sacrificial wake ping, payload in 100-byte chunks at
     * 100 ms each, 0xFE 0xFE separator between stages, end marker
     * 0xFF 0xFF 0x00 0x00. The host (vww_detection_benchmark.py) maps
     * the cycle_idx in each payload back to a stage name. */
    const size_t   CHUNK_SIZE     = 100;
    const uint32_t SETTLE_MS      = 250;
    const uint32_t CHUNK_DELAY_MS = 100;
    uint8_t chunk_buffer[CHUNK_SIZE + 1];
    chunk_buffer[0] = 0x01;  /* data flag */

    for (uint32_t s = 0; s < PIPE_STAGES; s++) {
        s_pipe_payload.stage_idx = s;
        memcpy(s_pipe_payload.cycles, s_pipe_stage_cycles[s],
               sizeof(s_pipe_payload.cycles));
        reload_watchdog(NULL, NULL);

        nrfx_systick_delay_ms(SETTLE_MS);
        reload_watchdog(NULL, NULL);

        /* Sacrificial wake-ping (see experiment_vww.c rationale). */
        const uint8_t wake[5] = {0x01, 0xAA, 0xAA, 0xAA, 0xAA};
        (void)bench_send_with_retry(wake, 5);
        nrfx_systick_delay_ms(CHUNK_DELAY_MS);
        reload_watchdog(NULL, NULL);

        const uint8_t *payload = (const uint8_t *)&s_pipe_payload;
        size_t total = sizeof(s_pipe_payload);
        size_t offset = 0;
        unsigned chunk_idx = 0;
        while (offset < total) {
            size_t chunk = (total - offset > CHUNK_SIZE)
                              ? CHUNK_SIZE
                              : (total - offset);
            chunk_buffer[0] = 0x01;
            memcpy(chunk_buffer + 1, payload + offset, chunk);
            if (!bench_send_with_retry(chunk_buffer, chunk + 1)) {
                luaL_error(L, "BLE send failed at stage %u chunk %u",
                           (unsigned)s, chunk_idx);
                return 0;
            }
            offset += chunk;
            chunk_idx++;
            nrfx_systick_delay_ms(CHUNK_DELAY_MS);
            reload_watchdog(NULL, NULL);
        }

        /* Stage separator (host-side: 2-byte packet 0xFE 0xFE). */
        nrfx_systick_delay_ms(CHUNK_DELAY_MS);
        const uint8_t separator[3] = {0x01, 0xFE, 0xFE};
        if (!bench_send_with_retry(separator, 3)) {
            luaL_error(L, "BLE separator failed at stage %u", (unsigned)s);
            return 0;
        }
        LOG("pipe-bench: stage %u/%u sent",
            (unsigned)(s + 1), (unsigned)PIPE_STAGES);
    }

    /* End marker (host-side: 4-byte packet 0xFF 0xFF 0x00 0x00). */
    nrfx_systick_delay_ms(SETTLE_MS);
    const uint8_t end_marker[5] = {0x01, 0xFF, 0xFF, 0x00, 0x00};
    if (!bench_send_with_retry(end_marker, 5)) {
        luaL_error(L, "BLE end marker failed");
        return 0;
    }
    LOG("pipe-bench: end marker sent");

    /* Match pure-inference benchmark return convention. Host doesn't
     * read this; useful only for Lua-level smoke testing. */
    lua_pushinteger(L, (lua_Integer)PIPE_N);
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

    uint8_t *input = s_rgb_buffer;
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

    /* See experiment_vww.c for rationale on these BLE pacing constants. */
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

        /* Sacrificial wake-ping (see experiment_vww.c for rationale). */
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
    return "VWW_RGB";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    lua_pushcfunction(L, lua_experiment_run_person_detection);
    lua_setfield(L, -2, "run_person_detection");

    lua_pushcfunction(L, lua_experiment_run_person_detection_fast);
    lua_setfield(L, -2, "run_person_detection_fast");

    lua_pushcfunction(L, lua_experiment_run_vww_demo);
    lua_setfield(L, -2, "run_vww_demo");

    lua_pushcfunction(L, lua_experiment_run_person_detection_benchmark);
    lua_setfield(L, -2, "run_person_detection_benchmark");

    lua_pushcfunction(L, lua_experiment_run_inference_benchmark);
    lua_setfield(L, -2, "run_inference_benchmark");
}
