/*
 * Experiment Common Header
 *
 * Shared types and function declarations for ML experiment modules.
 */

#ifndef EXPERIMENT_COMMON_H
#define EXPERIMENT_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "lua.h"

/*-----------------------------------------------*/
/* JPEG Decoder Context                          */
/*-----------------------------------------------*/

typedef struct {
    /* Input context */
    const uint8_t *data;     /* Pointer to JPEG data in memory */
    size_t size;             /* Total size of JPEG data */
    size_t offset;           /* Current read position */
    /* Output context */
    uint8_t *buffer;         /* Output grayscale buffer */
    uint16_t width;          /* Image width */
    uint16_t height;         /* Image height */
    uint16_t buf_width;      /* Buffer row width */
} jpeg_ctx_t;

/*-----------------------------------------------*/
/* Shared JPEG/Image Processing Functions        */
/*-----------------------------------------------*/

/**
 * Decode JPEG data to grayscale with optional scaling
 * @param jpeg_data Pointer to JPEG data
 * @param jpeg_size Size of JPEG data
 * @param out_buffer Output buffer for grayscale pixels (must be pre-allocated)
 * @param out_width Expected output width (after scaling)
 * @param out_height Expected output height (after scaling)
 * @param actual_width Pointer to store actual decoded width (after scaling)
 * @param actual_height Pointer to store actual decoded height (after scaling)
 * @param scale TJpgDec scale factor: 0=1:1, 1=1:2, 2=1:4, 3=1:8
 * @param apply_rotation If true, apply 90 CCW rotation during decode
 * @return 0 on success, negative error code on failure
 */
int jpeg_decode_grayscale_scaled(const uint8_t *jpeg_data, size_t jpeg_size,
                                  uint8_t *out_buffer, uint16_t out_width, uint16_t out_height,
                                  uint16_t *actual_width, uint16_t *actual_height,
                                  uint8_t scale, bool apply_rotation);

/**
 * Decode JPEG data to grayscale (legacy wrapper, 1:1 scale with rotation)
 */
int jpeg_decode_grayscale(const uint8_t *jpeg_data, size_t jpeg_size,
                          uint8_t *out_buffer, uint16_t out_width, uint16_t out_height,
                          uint16_t *actual_width, uint16_t *actual_height);

/**
 * Bilinear upscale from 90x90 to 96x96 with 90 CW rotation
 */
void upscale_90_to_96_with_rotation(const uint8_t *src, uint8_t *dst);

#if defined(ML_EXPERIMENT_VWW_RGB) || defined(ML_EXPERIMENT_FOMO_HAND_DETECTION)
/*-----------------------------------------------*/
/* RGB Image Processing Functions                */
/*-----------------------------------------------*/

/**
 * Decode JPEG data to RGB888 with optional scaling
 * @param jpeg_data Pointer to JPEG data
 * @param jpeg_size Size of JPEG data
 * @param out_buffer Output buffer for RGB pixels (must be pre-allocated, 3 bytes per pixel)
 * @param out_width Expected output width (after scaling)
 * @param out_height Expected output height (after scaling)
 * @param actual_width Pointer to store actual decoded width (after scaling)
 * @param actual_height Pointer to store actual decoded height (after scaling)
 * @param scale TJpgDec scale factor: 0=1:1, 1=1:2, 2=1:4, 3=1:8
 * @param apply_rotation If true, apply 90 CCW rotation during decode
 * @return 0 on success, negative error code on failure
 */
int jpeg_decode_rgb_scaled(const uint8_t *jpeg_data, size_t jpeg_size,
                           uint8_t *out_buffer, uint16_t out_width, uint16_t out_height,
                           uint16_t *actual_width, uint16_t *actual_height,
                           uint8_t scale, bool apply_rotation);

/**
 * Bilinear upscale from 90x90 to 96x96 RGB with 90 CCW rotation
 */
void upscale_90_to_96_rgb_with_rotation(const uint8_t *src, uint8_t *dst);

/**
 * Bilinear downscale from 90x90 to 64x64 RGB with 90 CCW rotation.
 * Source must be 90*90*3 = 24300 bytes; dst must be 64*64*3 = 12288 bytes.
 */
void downscale_90_to_64_rgb_with_rotation(const uint8_t *src, uint8_t *dst);

#endif /* ML_EXPERIMENT_VWW_RGB || ML_EXPERIMENT_FOMO_HAND_DETECTION */

/*-----------------------------------------------*/
/* Experiment Interface                          */
/*-----------------------------------------------*/

/**
 * Get the name of the currently compiled experiment
 * @return String name of the experiment (e.g., "VWW", "FOMO_BEER_CAN")
 */
const char* experiment_get_name(void);

/**
 * Register experiment-specific Lua functions
 * Called by lua_open_experiment_library()
 * @param L Lua state
 * @param experiment_table Stack index of the experiment table
 */
void experiment_register_lua_functions(lua_State *L, int experiment_table);

/**
 * Open the experiment library (main entry point)
 * @param L Lua state
 */
void lua_open_experiment_library(lua_State *L);

/*-----------------------------------------------*/
/* Test Data (DEV_KIT builds only)               */
/*-----------------------------------------------*/

#ifdef DEV_KIT_BUILD
extern uint8_t test_jpeg_data[];
extern const size_t test_jpeg_size;
#endif

#endif /* EXPERIMENT_COMMON_H */
