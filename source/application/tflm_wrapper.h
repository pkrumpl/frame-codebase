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
 * FOMO Object Detection Model API (only when ML_EXPERIMENT_FOMO_BEER_CAN)
 *============================================================================*/

#if defined(ML_EXPERIMENT_FOMO_BEER_CAN)

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

#endif /* ML_EXPERIMENT_FOMO_BEER_CAN */

/*=============================================================================
 * Person Detection Model API (only when ML_EXPERIMENT_VWW or ML_EXPERIMENT_VWW_RGB)
 *============================================================================*/

#if defined(ML_EXPERIMENT_VWW) || defined(ML_EXPERIMENT_VWW_RGB)

// Person detect model constants
#define PERSON_INPUT_WIDTH   96
#define PERSON_INPUT_HEIGHT  96

#if defined(ML_EXPERIMENT_VWW_RGB)
#define PERSON_INPUT_CHANNELS 3
#define PERSON_INPUT_SIZE    27648  // 96x96x3 RGB
#else
#define PERSON_INPUT_CHANNELS 1
#define PERSON_INPUT_SIZE    9216   // 96x96x1 grayscale
#endif

#define PERSON_OUTPUT_SIZE   2      // 2 classes: not_person, person
#define PERSON_NOT_PERSON_INDEX 0
#define PERSON_PERSON_INDEX     1

/**
 * Initialize the person detection model (must be called once before inference)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t person_detect_initialize(void);

/**
 * Run person detection inference on a 96x96 image
 * @param input_image Pointer to PERSON_INPUT_SIZE bytes of uint8 image data
 *                    (9216 bytes for grayscale, 27648 bytes for RGB)
 * @param output_scores Pointer to 2 bytes buffer for int8 output scores [not_person, person]
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t person_detect_infer(const uint8_t* input_image, int8_t* output_scores);

/**
 * Check if person detection model is initialized
 * @return true if initialized, false otherwise
 */
bool person_detect_is_initialized(void);

#endif /* ML_EXPERIMENT_VWW || ML_EXPERIMENT_VWW_RGB */

/*=============================================================================
 * Hello World Model API (only when ML_EXPERIMENT_HELLO_WORLD)
 *============================================================================*/

#if defined(ML_EXPERIMENT_HELLO_WORLD)

/**
 * Initialize the float hello_world model (must be called once before inference)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_initialize(void);

/**
 * Run inference on a single input value (float model)
 * @param input Input value (angle in radians, typically 0 to 2*PI)
 * @param output Pointer to store the predicted output (sine value)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_infer(float input, float* output);

/**
 * Initialize the int8 quantized model (separate from float model)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_initialize_int8(void);

/**
 * Run inference on the int8 quantized model
 * Automatically handles quantization of input and dequantization of output
 * @param input Input value (angle in radians, typically 0 to 2*PI)
 * @param output Pointer to store the predicted output (sine value)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_infer_int8(float input, float* output);

/**
 * Get model information
 */
typedef enum {
    TFLM_MODEL_FLOAT,
    TFLM_MODEL_INT8
} tflm_model_type_t;

typedef struct {
    tflm_model_type_t type;
    uint32_t model_size_bytes;
    uint32_t arena_size_bytes;
    bool initialized;
} tflm_model_info_t;

/**
 * Get information about the float model
 * @param info Pointer to store model information
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_get_float_model_info(tflm_model_info_t* info);

/**
 * Get information about the int8 model
 * @param info Pointer to store model information
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_get_int8_model_info(tflm_model_info_t* info);

#endif /* ML_EXPERIMENT_HELLO_WORLD */

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H
