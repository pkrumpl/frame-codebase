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
#include "lauxlib.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
}
