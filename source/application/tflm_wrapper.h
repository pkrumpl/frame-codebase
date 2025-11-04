/*
 * TFLM C Wrapper
 *
 * This header provides C-compatible wrappers for TensorFlow Lite Micro
 * hello_world example functions, allowing them to be called from C code.
 */

#ifndef TFLM_WRAPPER_H
#define TFLM_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Return codes that match TfLiteStatus
typedef enum {
    TFLM_OK = 0,
    TFLM_ERROR = 1
} tflm_status_t;

/**
 * Initialize the TFLM model (must be called once before inference)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_initialize(void);

/**
 * Run inference on a single input value
 * @param input Input value (angle in radians, typically 0 to 2*PI)
 * @param output Pointer to store the predicted output (sine value)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_infer(float input, float* output);

/**
 * Run the TFLM hello_world example which profiles memory and latency
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_profile_memory_and_latency(void);

/**
 * Run inference with the float model
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_load_float_model_and_infer(void);

/**
 * Run inference with the quantized int8 model
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_load_quant_model_and_infer(void);

/**
 * Run all TFLM hello_world tests
 * @return TFLM_OK if all tests pass, TFLM_ERROR otherwise
 */
tflm_status_t tflm_run_all_tests(void);

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H
