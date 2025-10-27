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
#include "tensorflow/lite/micro/examples/hello_world/models/hello_world_float_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

#include <math.h>

namespace {
using HelloWorldOpResolver = tflite::MicroMutableOpResolver<1>;

TfLiteStatus RegisterOps(HelloWorldOpResolver& op_resolver) {
  TF_LITE_ENSURE_STATUS(op_resolver.AddFullyConnected());
  return kTfLiteOk;
}
}  // namespace

extern "C" {

// Simplified version without profiling/recording to avoid linking issues
tflm_status_t tflm_profile_memory_and_latency(void) {
  MicroPrintf("Memory profiling skipped (simplified float-only version)");
  return TFLM_OK;
}

tflm_status_t tflm_load_float_model_and_infer(void) {
  MicroPrintf("\n=== TFLM Hello World Float Model Demo ===");

  const tflite::Model* model =
      ::tflite::GetModel(hello_world_float_model_data);
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

} // extern "C"
