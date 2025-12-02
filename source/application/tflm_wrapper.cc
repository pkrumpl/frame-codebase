/*
 * TFLM C Wrapper Implementation
 *
 * This file wraps the TensorFlow Lite Micro hello_world example functions
 * with C linkage so they can be called from C code.
 */

/*
 * NOTE: Workarounds for picolibc/C++ standard library conflicts
 * are defined in CXXFLAGS in the Makefile:
 * - _READ_WRITE_RETURN_TYPE=_ssize_t
 * - _READ_WRITE_BUFSIZE_TYPE=int
 * - _Thread_local=thread_local
 */

#include <cstddef>  // For size_t

// Forward declare functions to avoid attribute issues
extern "C" {
void free(void*);
void* malloc(size_t);
void* realloc(void*, size_t);
void* calloc(size_t, size_t);
}

#include "tflm_wrapper.h"

// Include TFLM headers
#include "tensorflow/lite/core/c/common.h"
#include "examples/hello_world/models/hello_world_float_model_data.h"
#include "examples/hello_world/models/hello_world_int8_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <math.h>

// Compile-time check that CMSIS-NN is enabled
#ifdef CMSIS_NN
#pragma message("CMSIS-NN optimized kernels ENABLED")
#else
#pragma message("WARNING: CMSIS-NN is NOT enabled - using reference kernels (slower)")
#endif

namespace {
using HelloWorldOpResolver = tflite::MicroMutableOpResolver<1>;

TfLiteStatus RegisterOps(HelloWorldOpResolver& op_resolver) {
  TF_LITE_ENSURE_STATUS(op_resolver.AddFullyConnected());
  return kTfLiteOk;
}

// Persistent storage for the FLOAT model interpreter
constexpr int kTensorArenaSize = 3000;
uint8_t tensor_arena[kTensorArenaSize];
tflite::MicroInterpreter* interpreter = nullptr;
HelloWorldOpResolver* op_resolver = nullptr;

// Persistent storage for the INT8 model interpreter
constexpr int kTensorArenaSizeInt8 = 2500;  // Int8 needs less memory
uint8_t tensor_arena_int8[kTensorArenaSizeInt8];
tflite::MicroInterpreter* interpreter_int8 = nullptr;
HelloWorldOpResolver* op_resolver_int8 = nullptr;

// Cached quantization parameters for int8 model (avoid repeated memory reads)
struct QuantizationParams {
  float input_scale;
  int32_t input_zero_point;
  float output_scale;
  int32_t output_zero_point;
  float input_scale_inv;  // Pre-computed 1/input_scale for faster quantization
} int8_quant_params = {0};

}  // namespace

extern "C" {

/**
 * Initialize the TFLM model once
 * Must be called before tflm_infer()
 */
tflm_status_t tflm_initialize(void) {
  MicroPrintf("Initializing TFLM hello_world model...");
#ifdef CMSIS_NN
  MicroPrintf("CMSIS-NN optimized kernels enabled");
#else
  MicroPrintf("Using reference kernels (CMSIS-NN not enabled)");
#endif

  tflite::InitializeTarget();

  const tflite::Model* model =
      ::tflite::GetModel(g_hello_world_float_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Model schema version mismatch!");
    return TFLM_ERROR;
  }

  // Create op resolver (this needs to persist)
  static HelloWorldOpResolver static_op_resolver;
  op_resolver = &static_op_resolver;

  TfLiteStatus status = RegisterOps(*op_resolver);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register ops");
    return TFLM_ERROR;
  }

  // Create interpreter
  static tflite::MicroInterpreter static_interpreter(
      model, *op_resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  status = interpreter->AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("AllocateTensors failed");
    return TFLM_ERROR;
  }

  MicroPrintf("TFLM model initialized successfully!");
  return TFLM_OK;
}

/**
 * Run inference on a single input value
 * @param input Input value (angle in radians, typically 0 to 2*PI)
 * @param output Pointer to store the predicted output (sine value)
 * @return TFLM_OK on success, TFLM_ERROR on failure
 */
tflm_status_t tflm_infer(float input, float* output) {
  if (interpreter == nullptr) {
    MicroPrintf("ERROR: Model not initialized. Call tflm_initialize() first!");
    return TFLM_ERROR;
  }

  // Set input
  interpreter->input(0)->data.f[0] = input;

  // Run inference
  TfLiteStatus status = interpreter->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("Invoke failed for input %.3f", (double)input);
    return TFLM_ERROR;
  }

  // Get output
  *output = interpreter->output(0)->data.f[0];

  return TFLM_OK;
}

// Simplified version without profiling/recording to avoid linking issues
tflm_status_t tflm_profile_memory_and_latency(void) {
  MicroPrintf("Memory profiling skipped (simplified float-only version)");
  return TFLM_OK;
}

tflm_status_t tflm_load_float_model_and_infer(void) {
  MicroPrintf("\n=== TFLM Hello World Float Model Demo ===");

  const tflite::Model* model =
      ::tflite::GetModel(g_hello_world_float_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Model schema version mismatch!");
    return TFLM_ERROR;
  }

  HelloWorldOpResolver op_resolver;
  TfLiteStatus status = RegisterOps(op_resolver);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register ops");
    return TFLM_ERROR;
  }

  // Arena size for tensor allocations
  constexpr int kTensorArenaSize = 3000;
  uint8_t tensor_arena[kTensorArenaSize];

  tflite::MicroInterpreter interpreter(model, op_resolver, tensor_arena,
                                       kTensorArenaSize);
  status = interpreter.AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("AllocateTensors failed");
    return TFLM_ERROR;
  }

  MicroPrintf("Model loaded successfully!");
  MicroPrintf("Running sine wave predictions...\n");

  // Test values covering the range 0 to 2*PI
  constexpr int kNumTestValues = 8;
  float test_inputs[kNumTestValues] = {0.0f, 0.785f, 1.57f, 2.356f, 3.14f, 3.927f, 4.712f, 5.498f};

  for (int i = 0; i < kNumTestValues; ++i) {
    float x = test_inputs[i];

    // Set input
    interpreter.input(0)->data.f[0] = x;

    // Run inference
    status = interpreter.Invoke();
    if (status != kTfLiteOk) {
      MicroPrintf("Invoke failed at index %d", i);
      return TFLM_ERROR;
    }

    // Get prediction
    float y_pred = interpreter.output(0)->data.f[0];
    float y_true = sinf(x);
    float error = fabsf(y_true - y_pred);

    // Print results (multiply by 1000 to show 3 decimal places with integer formatting)
    MicroPrintf("x=%.3f  sin(x)=%.3f  predicted=%.3f  error=%.4f",
                (double)x, (double)y_true, (double)y_pred, (double)error);

    // Verify accuracy
    if (error > 0.05f) {
      MicroPrintf("ERROR: Prediction error too large!");
      return TFLM_ERROR;
    }
  }

  MicroPrintf("\nAll predictions accurate! Model working correctly.");
  return TFLM_OK;
}

// Quantized model removed to avoid linking issues with quantization utilities
tflm_status_t tflm_load_quant_model_and_infer(void) {
  MicroPrintf("Quantized model test skipped (float-only version)");
  return TFLM_OK;
}

tflm_status_t tflm_run_all_tests(void) {
  MicroPrintf("\n========================================");
  MicroPrintf("  TensorFlow Lite Micro Hello World");
  MicroPrintf("  Float Model Demo");
  MicroPrintf("========================================\n");

  tflite::InitializeTarget();

  // Run the float model inference demo
  tflm_status_t status = tflm_load_float_model_and_infer();
  if (status != TFLM_OK) {
    MicroPrintf("\n*** TFLM DEMO FAILED ***");
    return TFLM_ERROR;
  }

  MicroPrintf("\n========================================");
  MicroPrintf("  TFLM Demo Completed Successfully!");
  MicroPrintf("========================================\n");
  return TFLM_OK;
}

/**
 * Initialize the int8 quantized model
 */
tflm_status_t tflm_initialize_int8(void) {
  MicroPrintf("Initializing TFLM int8 quantized model...");
#ifdef CMSIS_NN
  MicroPrintf("✓ CMSIS-NN optimized kernels enabled");
#else
  MicroPrintf("⚠ Using reference kernels (CMSIS-NN not enabled)");
#endif

  tflite::InitializeTarget();

  const tflite::Model* model =
      ::tflite::GetModel(g_hello_world_int8_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Int8 model schema version mismatch!");
    return TFLM_ERROR;
  }

  // Create op resolver (this needs to persist)
  static HelloWorldOpResolver static_op_resolver_int8;
  op_resolver_int8 = &static_op_resolver_int8;

  TfLiteStatus status = RegisterOps(*op_resolver_int8);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register ops for int8 model");
    return TFLM_ERROR;
  }

  // Create interpreter
  static tflite::MicroInterpreter static_interpreter_int8(
      model, *op_resolver_int8, tensor_arena_int8, kTensorArenaSizeInt8);
  interpreter_int8 = &static_interpreter_int8;

  status = interpreter_int8->AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("AllocateTensors failed for int8 model");
    return TFLM_ERROR;
  }

  // Cache quantization parameters to avoid repeated memory reads during inference
  TfLiteTensor* input_tensor = interpreter_int8->input(0);
  TfLiteTensor* output_tensor = interpreter_int8->output(0);

  int8_quant_params.input_scale = input_tensor->params.scale;
  int8_quant_params.input_zero_point = input_tensor->params.zero_point;
  int8_quant_params.output_scale = output_tensor->params.scale;
  int8_quant_params.output_zero_point = output_tensor->params.zero_point;
  int8_quant_params.input_scale_inv = 1.0f / int8_quant_params.input_scale;  // Pre-compute for faster quantization

  MicroPrintf("Int8 quantization params: input[scale=%.6f, zero=%d], output[scale=%.6f, zero=%d]",
              (double)int8_quant_params.input_scale, int8_quant_params.input_zero_point,
              (double)int8_quant_params.output_scale, int8_quant_params.output_zero_point);

  MicroPrintf("TFLM int8 model initialized successfully!");
  return TFLM_OK;
}

/**
 * Run inference on the int8 quantized model
 * Handles quantization of input and dequantization of output
 * OPTIMIZED: Uses cached quantization parameters and faster math
 */
tflm_status_t tflm_infer_int8(float input, float* output) {
  if (interpreter_int8 == nullptr) {
    MicroPrintf("ERROR: Int8 model not initialized. Call tflm_initialize_int8() first!");
    return TFLM_ERROR;
  }

  // Get input and output tensors (only need data pointers, not params)
  TfLiteTensor* input_tensor = interpreter_int8->input(0);
  TfLiteTensor* output_tensor = interpreter_int8->output(0);

  // Quantize input using cached parameters and pre-computed inverse scale
  // Optimized: multiply by inverse instead of divide (faster on ARM)
  float input_scaled = input * int8_quant_params.input_scale_inv + int8_quant_params.input_zero_point;

  // Fast rounding and clamping
  int32_t input_quantized_32 = static_cast<int32_t>(input_scaled + (input_scaled >= 0 ? 0.5f : -0.5f));

  // Clamp to int8 range [-128, 127] using branchless min/max
  input_quantized_32 = (input_quantized_32 < -128) ? -128 : input_quantized_32;
  input_quantized_32 = (input_quantized_32 > 127) ? 127 : input_quantized_32;

  input_tensor->data.int8[0] = static_cast<int8_t>(input_quantized_32);

  // Run inference
  TfLiteStatus status = interpreter_int8->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("Invoke failed for int8 model with input %.3f", (double)input);
    return TFLM_ERROR;
  }

  // Dequantize output using cached parameters
  int8_t output_quantized = output_tensor->data.int8[0];
  *output = static_cast<float>(output_quantized - int8_quant_params.output_zero_point) * int8_quant_params.output_scale;

  return TFLM_OK;
}

/**
 * Run inference on the int8 quantized model with DIRECT int8 input/output
 * NO conversion overhead - this is the pure inference path
 * Use tflm_quantize_input() and tflm_dequantize_output() helpers if needed
 */
tflm_status_t tflm_infer_int8_direct(int8_t input_quantized, int8_t* output_quantized) {
  if (interpreter_int8 == nullptr) {
    MicroPrintf("ERROR: Int8 model not initialized. Call tflm_initialize_int8() first!");
    return TFLM_ERROR;
  }

  // Set input
  interpreter_int8->input(0)->data.int8[0] = input_quantized;

  // Run inference
  TfLiteStatus status = interpreter_int8->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("Invoke failed for int8 model with input %d", input_quantized);
    return TFLM_ERROR;
  }

  // Get output directly (no conversion)
  *output_quantized = interpreter_int8->output(0)->data.int8[0];

  return TFLM_OK;
}

/**
 * Helper function: Quantize float input to int8
 * Formula: int8 = clamp(round(float * (1/scale) + zero_point), -128, 127)
 */
int8_t tflm_quantize_input(float input) {
  // Using cached quantization parameters
  float input_scaled = input * int8_quant_params.input_scale_inv + int8_quant_params.input_zero_point;

  // Round and convert to int32
  int32_t input_quantized_32 = static_cast<int32_t>(input_scaled + (input_scaled >= 0 ? 0.5f : -0.5f));

  // Clamp to int8 range [-128, 127]
  if (input_quantized_32 < -128) return -128;
  if (input_quantized_32 > 127) return 127;

  return static_cast<int8_t>(input_quantized_32);
}

/**
 * Helper function: Dequantize int8 output to float
 * Formula: float = (int8 - zero_point) * scale
 */
float tflm_dequantize_output(int8_t output_quantized) {
  // Using cached quantization parameters
  return static_cast<float>(output_quantized - int8_quant_params.output_zero_point)
         * int8_quant_params.output_scale;
}

/**
 * Get information about the float model
 */
tflm_status_t tflm_get_float_model_info(tflm_model_info_t* info) {
  if (info == nullptr) {
    return TFLM_ERROR;
  }

  info->type = TFLM_MODEL_FLOAT;
  info->model_size_bytes = g_hello_world_float_model_data_size;
  info->arena_size_bytes = kTensorArenaSize;
  info->initialized = (interpreter != nullptr);

  return TFLM_OK;
}

/**
 * Get information about the int8 model
 */
tflm_status_t tflm_get_int8_model_info(tflm_model_info_t* info) {
  if (info == nullptr) {
    return TFLM_ERROR;
  }

  info->type = TFLM_MODEL_INT8;
  info->model_size_bytes = g_hello_world_int8_model_data_size;
  info->arena_size_bytes = kTensorArenaSizeInt8;
  info->initialized = (interpreter_int8 != nullptr);

  return TFLM_OK;
}

} // extern "C"
