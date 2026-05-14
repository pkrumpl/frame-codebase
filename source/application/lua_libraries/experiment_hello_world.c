/*
 * Hello World Experiment Implementation
 *
 * Simple sine wave prediction using TensorFlow Lite Micro hello_world model.
 * This file is compiled when ML_EXPERIMENT=HELLO_WORLD is set.
 */

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "experiment_common.h"
#include "error_logging.h"
#include "tflm_wrapper.h"
#include "bluetooth.h"
#include "watchdog.h"
#include "nrfx_systick.h"
#include "lauxlib.h"

/* For DWT cycle counter timing */
#include "nrf.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Pure-inference benchmark constants and state. Same scheme as
 * experiment_vww{,_rgb}.c so the host-side Python script and CSV
 * format are unchanged. */
#define BENCH_K       8
#define BENCH_N       64
#define BENCH_WARMUP  10

/* Wire payload for one cycle: 4-byte LE cycle index followed by N raw
 * DWT cycle counts (uint32 LE). Naturally packed - all uint32_t
 * members - so sizeof == 4 + 4*BENCH_N == 516 bytes. */
static struct {
    uint32_t cycle_idx;
    uint32_t cycles[BENCH_N];
} s_bench_payload;

/*-----------------------------------------------*/
/* Hello World Lua Functions                     */
/*-----------------------------------------------*/

/**
 * Run inference on the float model
 * Usage: frame.experiment.infer(angle_radians)
 * Returns: predicted sine value
 */
static int lua_experiment_infer(lua_State *L)
{
    float input = (float)luaL_checknumber(L, 1);
    float output;

    tflm_status_t status = tflm_infer(input, &output);
    if (status != TFLM_OK) {
        luaL_error(L, "Float model inference failed");
        return 0;
    }

    lua_pushnumber(L, output);
    return 1;
}

/**
 * Run inference on the int8 quantized model
 * Usage: frame.experiment.infer_int8(angle_radians)
 * Returns: predicted sine value
 */
static int lua_experiment_infer_int8(lua_State *L)
{
    float input = (float)luaL_checknumber(L, 1);
    float output;

    tflm_status_t status = tflm_infer_int8(input, &output);
    if (status != TFLM_OK) {
        luaL_error(L, "Int8 model inference failed");
        return 0;
    }

    lua_pushnumber(L, output);
    return 1;
}

/**
 * Run a test comparing model predictions with actual sine values
 * Usage: frame.experiment.run_test()
 * Returns: table with test results
 */
static int lua_experiment_run_test(lua_State *L)
{
    const int NUM_SAMPLES = 10;
    float total_error_float = 0;
    float total_error_int8 = 0;
    float max_error_float = 0;
    float max_error_int8 = 0;
    char lua_script[512];

    LOG("Running hello_world sine prediction test...");

    for (int i = 0; i < NUM_SAMPLES; i++) {
        float angle = (float)i * 2.0f * (float)M_PI / (float)NUM_SAMPLES;
        float expected = sinf(angle);
        float predicted_float, predicted_int8;

        /* Float model */
        tflm_status_t status = tflm_infer(angle, &predicted_float);
        if (status != TFLM_OK) {
            luaL_error(L, "Float inference failed at sample %d", i);
            return 0;
        }

        /* Int8 model */
        status = tflm_infer_int8(angle, &predicted_int8);
        if (status != TFLM_OK) {
            luaL_error(L, "Int8 inference failed at sample %d", i);
            return 0;
        }

        float error_float = fabsf(predicted_float - expected);
        float error_int8 = fabsf(predicted_int8 - expected);

        total_error_float += error_float;
        total_error_int8 += error_int8;

        if (error_float > max_error_float) max_error_float = error_float;
        if (error_int8 > max_error_int8) max_error_int8 = error_int8;

        LOG("Sample %d: angle=%.3f, expected=%.3f, float=%.3f (err=%.4f), int8=%.3f (err=%.4f)",
            i, angle, expected, predicted_float, error_float, predicted_int8, error_int8);

        /* Display results on Frame */
        snprintf(lua_script, sizeof(lua_script),
            "frame.display.text('TF Lite Micro', 200, 50);"
            "frame.display.text('sin() prediction', 200, 90);"
            "frame.display.text('Sample %d/%d', 200, 140, { color = 'WHITE' });"
            "frame.display.text('Input: %.3f rad', 200, 180, { color = 'SEABLUE' });"
            "frame.display.text('Actual: %.4f', 200, 220, { color = 'GREEN' });"
            "frame.display.text('Float: %.4f (err %.4f)', 200, 260, { color = 'ORANGE' });"
            "frame.display.text('Int8:  %.4f (err %.4f)', 200, 300, { color = 'YELLOW' });"
            "frame.display.show();",
            i + 1, NUM_SAMPLES,
            (double)angle,
            (double)expected,
            (double)predicted_float, (double)error_float,
            (double)predicted_int8, (double)error_int8);
        luaL_dostring(L, lua_script);
        luaL_dostring(L, "frame.sleep(0.15)");
    }

    /* Display final summary */
    snprintf(lua_script, sizeof(lua_script),
        "frame.display.text('TF Lite Micro', 200, 50);"
        "frame.display.text('Test Complete!', 200, 90, { color = 'GREEN' });"
        "frame.display.text('Samples: %d', 200, 140, { color = 'WHITE' });"
        "frame.display.text('Float Model:', 200, 190, { color = 'ORANGE' });"
        "frame.display.text('  Avg err: %.4f', 200, 220, { color = 'ORANGE' });"
        "frame.display.text('  Max err: %.4f', 200, 250, { color = 'ORANGE' });"
        "frame.display.text('Int8 Model:', 200, 290, { color = 'YELLOW' });"
        "frame.display.text('  Avg err: %.4f', 200, 320, { color = 'YELLOW' });"
        "frame.display.text('  Max err: %.4f', 200, 350, { color = 'YELLOW' });"
        "frame.display.show();",
        NUM_SAMPLES,
        (double)(total_error_float / NUM_SAMPLES),
        (double)max_error_float,
        (double)(total_error_int8 / NUM_SAMPLES),
        (double)max_error_int8);
    luaL_dostring(L, lua_script);
    luaL_dostring(L, "frame.sleep(2)");

    /* Return results as table */
    lua_newtable(L);

    lua_pushnumber(L, total_error_float / NUM_SAMPLES);
    lua_setfield(L, -2, "avg_error_float");

    lua_pushnumber(L, total_error_int8 / NUM_SAMPLES);
    lua_setfield(L, -2, "avg_error_int8");

    lua_pushnumber(L, max_error_float);
    lua_setfield(L, -2, "max_error_float");

    lua_pushnumber(L, max_error_int8);
    lua_setfield(L, -2, "max_error_int8");

    lua_pushinteger(L, NUM_SAMPLES);
    lua_setfield(L, -2, "num_samples");

    return 1;
}

/**
 * Get model information
 * Usage: frame.experiment.get_model_info()
 * Returns: table with model info
 */
static int lua_experiment_get_model_info(lua_State *L)
{
    tflm_model_info_t float_info, int8_info;

    tflm_get_float_model_info(&float_info);
    tflm_get_int8_model_info(&int8_info);

    lua_newtable(L);

    /* Float model info */
    lua_newtable(L);
    lua_pushinteger(L, float_info.model_size_bytes);
    lua_setfield(L, -2, "model_size");
    lua_pushinteger(L, float_info.arena_size_bytes);
    lua_setfield(L, -2, "arena_size");
    lua_pushboolean(L, float_info.initialized);
    lua_setfield(L, -2, "initialized");
    lua_setfield(L, -2, "float_model");

    /* Int8 model info */
    lua_newtable(L);
    lua_pushinteger(L, int8_info.model_size_bytes);
    lua_setfield(L, -2, "model_size");
    lua_pushinteger(L, int8_info.arena_size_bytes);
    lua_setfield(L, -2, "arena_size");
    lua_pushboolean(L, int8_info.initialized);
    lua_setfield(L, -2, "initialized");
    lua_setfield(L, -2, "int8_model");

    return 1;
}

/*-----------------------------------------------*/
/* Pure-inference benchmark                      */
/*-----------------------------------------------*/

/**
 * Send one BLE packet, retrying on busy/not-connected. Mirrors the
 * helper in experiment_vww{,_rgb}.c so the wire protocol is identical.
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
 * Pure-inference benchmark for the hello_world int8 sine model.
 *
 *   - BENCH_WARMUP untimed inferences first to settle caches and the
 *     branch predictor.
 *   - Then BENCH_K cycles. Each cycle picks a fresh random input angle
 *     in [0, 2pi) (the equivalent of the "random buffer" the VWW
 *     benchmark refreshes per cycle - here the "buffer" is one float)
 *     and times BENCH_N back-to-back inferences using DWT->CYCCNT.
 *   - After each cycle the BENCH_N raw cycle counts are streamed back
 *     over BLE in 200-byte chunks, prefixed with a 4-byte LE cycle
 *     index. Cycles are separated by 0x01 0xFE 0xFE on the wire and
 *     the run terminates with 0x01 0xFF 0xFF 0x00 0x00. Identical to
 *     the VWW protocol so the same host-side script parses both.
 *
 * Lua: frame.experiment.run_inference_benchmark()
 * Returns: total number of timed inferences (BENCH_K * BENCH_N).
 */
static int lua_experiment_run_inference_benchmark(lua_State *L)
{
    if (!bluetooth_is_connected()) {
        luaL_error(L, "Bluetooth not connected");
        return 0;
    }

    LOG("bench: start (K=%u, N=%u, warmup=%u, payload=%u B)",
        (unsigned)BENCH_K, (unsigned)BENCH_N, (unsigned)BENCH_WARMUP,
        (unsigned)sizeof(s_bench_payload));

    /* Enable DWT cycle counter. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* Different seed per invocation. */
    srand((unsigned int)DWT->CYCCNT);

    float output;

    /* One-time warmup at a fixed angle. */
    {
        float warmup_angle = (float)M_PI / 4.0f;  /* arbitrary, in-range */
        for (int w = 0; w < BENCH_WARMUP; w++) {
            if (tflm_infer_int8(warmup_angle, &output) != TFLM_OK) {
                luaL_error(L, "warmup inference failed");
                return 0;
            }
            reload_watchdog(NULL, NULL);
        }
    }
    LOG("bench: warmup done");

    /* See experiment_vww.c for rationale on these BLE pacing constants. */
    const size_t CHUNK_SIZE = 100;
    const uint32_t SETTLE_MS = 250;
    const uint32_t CHUNK_DELAY_MS = 100;
    uint8_t chunk_buffer[CHUNK_SIZE + 1];
    chunk_buffer[0] = 0x01;  /* data flag */

    for (uint32_t k = 0; k < BENCH_K; k++) {
        /* Fresh random angle for this cycle, kept stable through the
         * inner loop. Same input for all BENCH_N inferences. */
        float frac = (float)rand() / (float)RAND_MAX;  /* [0, 1] */
        float angle = frac * 2.0f * (float)M_PI;
        reload_watchdog(NULL, NULL);

        s_bench_payload.cycle_idx = k;

        /* Tight inference loop - this is what the benchmark measures.
         * Keep the loop body free of LOG / BLE / anything that might
         * touch the softdevice. */
        for (uint32_t i = 0; i < BENCH_N; i++) {
            uint32_t t0 = DWT->CYCCNT;
            tflm_status_t st = tflm_infer_int8(angle, &output);
            uint32_t t1 = DWT->CYCCNT;
            if (st != TFLM_OK) {
                luaL_error(L, "inference failed at k=%u i=%u",
                           (unsigned)k, (unsigned)i);
                return 0;
            }
            /* uint32 wrap is safe (~67 s span @ 64 MHz). */
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

        /* Cycle separator (host-side: 2-byte packet 0xFE 0xFE). */
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
    return "HELLO_WORLD";
}

void experiment_register_lua_functions(lua_State *L, int experiment_table)
{
    (void)experiment_table;

    lua_pushcfunction(L, lua_experiment_infer);
    lua_setfield(L, -2, "infer");

    lua_pushcfunction(L, lua_experiment_infer_int8);
    lua_setfield(L, -2, "infer_int8");

    lua_pushcfunction(L, lua_experiment_run_test);
    lua_setfield(L, -2, "run_test");

    lua_pushcfunction(L, lua_experiment_get_model_info);
    lua_setfield(L, -2, "get_model_info");

    lua_pushcfunction(L, lua_experiment_run_inference_benchmark);
    lua_setfield(L, -2, "run_inference_benchmark");
}
