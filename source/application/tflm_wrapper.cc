/*
 * TFLM C Wrapper Implementation
 *
 * This file wraps the TensorFlow Lite Micro FOMO object detection model
 * with C linkage so it can be called from C code.
 */

/*
 * NOTE: Workarounds for picolibc/C++ standard library conflicts
 * are defined in CXXFLAGS in the Makefile:
 * - _READ_WRITE_RETURN_TYPE=_ssize_t
 * - _READ_WRITE_BUFSIZE_TYPE=int
 * - _Thread_local=thread_local
 */

#include <cstddef>  // For size_t
#include <cstring>  // For memcpy

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
#include "models/fomo_beer_can_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Compile-time check that CMSIS-NN is enabled
#ifdef CMSIS_NN
#pragma message("CMSIS-NN optimized kernels ENABLED")
#else
#pragma message("WARNING: CMSIS-NN is NOT enabled - using reference kernels (slower)")
#endif

extern "C" {

/*=============================================================================
 * FOMO Object Detection Model Implementation
 *============================================================================*/

// FOMO model requires these ops
using FomoOpResolver = tflite::MicroMutableOpResolver<9>;

static TfLiteStatus RegisterFomoOps(FomoOpResolver& op_resolver) {
  TF_LITE_ENSURE_STATUS(op_resolver.AddConv2D());
  TF_LITE_ENSURE_STATUS(op_resolver.AddDepthwiseConv2D());
  TF_LITE_ENSURE_STATUS(op_resolver.AddAdd());
  TF_LITE_ENSURE_STATUS(op_resolver.AddReshape());
  TF_LITE_ENSURE_STATUS(op_resolver.AddPad());
  TF_LITE_ENSURE_STATUS(op_resolver.AddMaxPool2D());
  TF_LITE_ENSURE_STATUS(op_resolver.AddRelu6());
  TF_LITE_ENSURE_STATUS(op_resolver.AddMean());
  TF_LITE_ENSURE_STATUS(op_resolver.AddSoftmax());
  return kTfLiteOk;
}

// FOMO tensor arena - 70KB
constexpr int kFomoTensorArenaSize = 70 * 1024;
static uint8_t fomo_tensor_arena[kFomoTensorArenaSize] __attribute__((aligned(16)));
static tflite::MicroInterpreter* fomo_interpreter = nullptr;
static FomoOpResolver* fomo_op_resolver = nullptr;
static bool fomo_initialized = false;

/**
 * Initialize the FOMO object detection model
 */
tflm_status_t fomo_initialize(void) {
  MicroPrintf("Initializing FOMO object detection model...");
#ifdef CMSIS_NN
  MicroPrintf("CMSIS-NN optimized kernels enabled");
#else
  MicroPrintf("Using reference kernels (CMSIS-NN not enabled)");
#endif

  tflite::InitializeTarget();

  const tflite::Model* model = ::tflite::GetModel(fomo_beer_can_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("FOMO model schema version mismatch! Expected %d, got %d",
                TFLITE_SCHEMA_VERSION, model->version());
    return TFLM_ERROR;
  }

  // Create op resolver (this needs to persist)
  static FomoOpResolver static_fomo_op_resolver;
  fomo_op_resolver = &static_fomo_op_resolver;

  TfLiteStatus status = RegisterFomoOps(*fomo_op_resolver);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register FOMO ops");
    return TFLM_ERROR;
  }

  // Create interpreter
  static tflite::MicroInterpreter static_fomo_interpreter(
      model, *fomo_op_resolver, fomo_tensor_arena, kFomoTensorArenaSize);
  fomo_interpreter = &static_fomo_interpreter;

  status = fomo_interpreter->AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("FOMO AllocateTensors failed");
    return TFLM_ERROR;
  }

  // Verify input/output dimensions
  TfLiteTensor* input = fomo_interpreter->input(0);
  TfLiteTensor* output = fomo_interpreter->output(0);

  MicroPrintf("FOMO input: dims=%d, shape=[%d,%d,%d,%d], type=%d",
              input->dims->size,
              input->dims->data[0], input->dims->data[1],
              input->dims->data[2], input->dims->data[3],
              input->type);

  MicroPrintf("FOMO output: dims=%d, shape=[%d,%d,%d,%d], type=%d",
              output->dims->size,
              output->dims->data[0], output->dims->data[1],
              output->dims->data[2], output->dims->data[3],
              output->type);

  // Verify expected dimensions
  if (input->dims->data[1] != 96 || input->dims->data[2] != 96) {
    MicroPrintf("WARNING: Expected 96x96 input, got %dx%d",
                input->dims->data[1], input->dims->data[2]);
  }

  if (output->dims->data[1] != 12 || output->dims->data[2] != 12 ||
      output->dims->data[3] != 3) {
    MicroPrintf("WARNING: Expected 12x12x3 output, got %dx%dx%d",
                output->dims->data[1], output->dims->data[2],
                output->dims->data[3]);
  }

  size_t arena_used = fomo_interpreter->arena_used_bytes();
  MicroPrintf("FOMO arena used: %u bytes (of %u available)",
              arena_used, kFomoTensorArenaSize);

  fomo_initialized = true;
  MicroPrintf("FOMO model initialized successfully!");
  return TFLM_OK;
}

/**
 * Run FOMO inference on a 96x96 grayscale image
 * Input: uint8 grayscale [0-255]
 * Output: int8 grid [1, 12, 12, 3]
 *
 * Quantization: input scale ~ 1/255, zero_point = -128
 * So uint8_grayscale -> int8_input = grayscale - 128
 */
tflm_status_t fomo_infer(const uint8_t* input_grayscale, int8_t* output_grid) {
  if (!fomo_initialized || fomo_interpreter == nullptr) {
    MicroPrintf("ERROR: FOMO model not initialized. Call fomo_initialize() first!");
    return TFLM_ERROR;
  }

  if (input_grayscale == nullptr || output_grid == nullptr) {
    MicroPrintf("ERROR: Invalid input/output pointers");
    return TFLM_ERROR;
  }

  // Get input tensor
  TfLiteTensor* input = fomo_interpreter->input(0);

  // Convert uint8 grayscale [0-255] to int8 [-128, 127]
  int8_t* input_data = input->data.int8;
  for (int i = 0; i < FOMO_INPUT_SIZE; i++) {
    input_data[i] = static_cast<int8_t>(static_cast<int16_t>(input_grayscale[i]) - 128);
  }

  // Run inference
  TfLiteStatus status = fomo_interpreter->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("FOMO Invoke failed");
    return TFLM_ERROR;
  }

  // Copy output
  TfLiteTensor* output = fomo_interpreter->output(0);
  memcpy(output_grid, output->data.int8, FOMO_OUTPUT_SIZE);

  return TFLM_OK;
}

/**
 * Check if FOMO model is initialized
 */
bool fomo_is_initialized(void) {
  return fomo_initialized;
}

} // extern "C"
