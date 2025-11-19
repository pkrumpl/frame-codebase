#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "error_logging.h"
#include "lauxlib.h"
#include "lua.h"
#include "main.h"
#include "nrf_soc.h"
#include "nrf52840.h"
#include "nrfx_saadc.h"
#include "pinout.h"
#include "spi.h"
#include "tflm_wrapper.h"
#include "nrfx_systick.h"

static int lua_experiment_hello_world(lua_State *L)
{
    enum { NUM_TEST_VALS = 256, BUF_LEN = 128 };
    char *str_buf = malloc(BUF_LEN);
    float *inputs = malloc(sizeof(float) * NUM_TEST_VALS);
    if (inputs == NULL || str_buf == NULL) {
        luaL_error(L, "not enough memory");
        return -1;
    }

    for (uint32_t i = 0; i < NUM_TEST_VALS; i++) {
        inputs[i] = ((float)i / (float)NUM_TEST_VALS) * 6.28318f;  // 0 to 2*PI
    }

    float predicted_output = 0.0f;
    nrfx_systick_state_t start_state, end_state;

    // Start measurement
    nrfx_systick_get(&start_state);

    for (uint32_t i = 0; i < NUM_TEST_VALS; i++) {
        tflm_status_t result = tflm_infer(inputs[i], &predicted_output);

        if (result != TFLM_OK) {
            luaL_error(L, "inference failed");
            return -1;
        }
    }

    // End measurement
    nrfx_systick_get(&end_state);

    uint32_t elapsed_ticks = (start_state.time > end_state.time)
                        ? (start_state.time - end_state.time)
                        : (0xFFFFFF - end_state.time + start_state.time);

    // 64 MHz (64 * 10^6 ticks per second)
    float elapsed_time_us = (float)elapsed_ticks / 64.0f;
    float avg_time_us = elapsed_time_us / (float)NUM_TEST_VALS;

    // Send results via Bluetooth
    snprintf(str_buf, BUF_LEN, "TFLite Benchmark: %d iterations\n", NUM_TEST_VALS);
    bluetooth_send_data((uint8_t*)str_buf, strlen(str_buf));

    snprintf(str_buf, BUF_LEN, "Total: %.2f us\n", (double)elapsed_time_us);
    bluetooth_send_data((uint8_t*)str_buf, strlen(str_buf));

    snprintf(str_buf, BUF_LEN, "Average: %.2f us per inference\n", (double)avg_time_us);
    bluetooth_send_data((uint8_t*)str_buf, strlen(str_buf));

    free(str_buf);
    free(inputs);

    return 0;
}

static int lua_experiment_hello_world_quant(lua_State *L)
{
    enum { NUM_TEST_VALS = 256, BUF_LEN = 128 };
    char *str_buf = malloc(BUF_LEN);
    int8_t *inputs = malloc(sizeof(int8_t) * NUM_TEST_VALS);
    if (inputs == NULL || str_buf == NULL) {
        luaL_error(L, "not enough memory");
        return -1;
    }

    for (uint32_t i = 0; i < NUM_TEST_VALS; i++) {
        inputs[i] = tflm_quantize_input(((float)i / (float)NUM_TEST_VALS) * 6.28318f);  // 0 to 2*PI
    }

    int8_t predicted_output = 0;
    nrfx_systick_state_t start_state, end_state;

    // Start measurement
    nrfx_systick_get(&start_state);

    for (uint32_t i = 0; i < NUM_TEST_VALS; i++) {
        tflm_status_t result = tflm_infer_int8_direct(inputs[i], &predicted_output);

        if (result != TFLM_OK) {
            luaL_error(L, "inference failed");
            return -1;
        }
    }

    // End measurement
    nrfx_systick_get(&end_state);

    uint32_t elapsed_ticks = (start_state.time > end_state.time)
                        ? (start_state.time - end_state.time)
                        : (0xFFFFFF - end_state.time + start_state.time);

    // 64 MHz (64 * 10^6 ticks per second)
    float elapsed_time_us = (float)elapsed_ticks / 64.0f;
    float avg_time_us = elapsed_time_us / (float)NUM_TEST_VALS;

    // Send results via Bluetooth
    snprintf(str_buf, BUF_LEN, "TFLite Int8 Benchmark: %d iterations\n", NUM_TEST_VALS);
    bluetooth_send_data((uint8_t*)str_buf, strlen(str_buf));

    snprintf(str_buf, BUF_LEN, "Total: %.2f us\n", (double)elapsed_time_us);
    bluetooth_send_data((uint8_t*)str_buf, strlen(str_buf));

    snprintf(str_buf, BUF_LEN, "Average: %.2f us per inference\n", (double)avg_time_us);
    bluetooth_send_data((uint8_t*)str_buf, strlen(str_buf));

    free(str_buf);
    free(inputs);

    return 0;
}

/**
 * Experiments are callebale as 'frame.experiment.hello_world' for example
 */
void lua_open_experiment_library(lua_State *L)
{
    lua_getglobal(L, "frame");

    // New table to have nested commands for experiments
    lua_newtable(L);

    lua_pushcfunction(L, lua_experiment_hello_world);
    lua_setfield(L, -2, "hello_world");

    lua_pushcfunction(L, lua_experiment_hello_world_quant);
    lua_setfield(L, -2, "hello_world_quant");

    lua_setfield(L, -2, "experiment");

    lua_pop(L, 1);
}