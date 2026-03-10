/*
 * TFLM C Wrapper
 *
 * This header provides C-compatible wrappers for TensorFlow Lite Micro
 * FOMO object detection model, allowing it to be called from C code.
 */

#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#ifdef __cplusplus
#include <cstdint>
#include <cstdbool>
extern "C" {
#else
#include <stdint.h>
#include <stdbool.h>
#endif

// Return codes that match TfLiteStatus
typedef enum {
    TFLM_OK = 0,
    TFLM_ERROR = 1
} tflm_status_t;

/*=============================================================================
 * FOMO Object Detection Model API
 *============================================================================*/

// FOMO model constants
#define FOMO_INPUT_SIZE   4096   // 64x64 grayscale
#define FOMO_OUTPUT_SIZE  192    // 8x8x3 grid (8*8*3 classes)
#define FOMO_GRID_SIZE    8      // Output grid dimension
#define FOMO_NUM_CLASSES  3      // Background, Beer, Can

/**
 * Initialize the FOMO object detection model (must be called once before inference)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t fomo_initialize(void);

/**
 * Run FOMO inference on a 64x64 grayscale image
 * @param input_grayscale Pointer to 4096 bytes of uint8 grayscale image data
 * @param output_grid Pointer to 192 bytes buffer for int8 output grid (8x8x3)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t fomo_infer(const uint8_t* input_grayscale, int8_t* output_grid);

/**
 * Check if FOMO model is initialized
 * @return true if initialized, false otherwise
 */
bool fomo_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H
