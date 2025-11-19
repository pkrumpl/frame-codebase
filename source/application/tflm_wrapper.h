/*
 * TFLM C Wrapper
 *
 * This header provides C-compatible wrappers for TensorFlow Lite Micro
 * hello_world example functions, allowing them to be called from C code.
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
 * Run inference on the int8 quantized model with native int8 input/output
 * NO automatic conversion - caller handles quantization/dequantization
 * This is the fastest path with minimal overhead (pure inference, no wrapper overhead)
 *
 * @param input_quantized Quantized int8 input value
 * @param output_quantized Pointer to store the quantized int8 output value
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_infer_int8_direct(int8_t input_quantized, int8_t* output_quantized);

/**
 * Helper: Quantize a float input to int8 for the int8 model
 * @param input Float input value
 * @return Quantized int8 value
 */
int8_t tflm_quantize_input(float input);

/**
 * Helper: Dequantize an int8 output from the int8 model to float
 * @param output_quantized Quantized int8 output value
 * @return Dequantized float value
 */
float tflm_dequantize_output(int8_t output_quantized);

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

#ifdef __cplusplus
}
#endif

#endif // TFLM_WRAPPER_H
