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

// Include model data based on selected experiment
#if defined(ML_EXPERIMENT_FOMO_BEER_CAN)
#include "models/fomo_beer_can.h"
#elif defined(ML_EXPERIMENT_FOMO_HAND_DETECTION)
#include "models/fomo_hand_detection.h"
#elif defined(ML_EXPERIMENT_VWW)
#include "models/person_detect.h"
#elif defined(ML_EXPERIMENT_VWW_RGB)
#include "models/person_detect_rgb.h"
#elif defined(ML_EXPERIMENT_HELLO_WORLD)
#include "examples/hello_world/models/hello_world_float_model_data.h"
#include "examples/hello_world/models/hello_world_int8_model_data.h"
#endif

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

#if defined(ML_EXPERIMENT_FOMO_BEER_CAN) || defined(ML_EXPERIMENT_FOMO_HAND_DETECTION)

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

// FOMO tensor arena
// TODO: check for upper limit when removing hardcoded jpg data from experiment.c
#if defined(ML_EXPERIMENT_FOMO_HAND_DETECTION)
constexpr int kFomoTensorArenaSize = 115000;
#else
constexpr int kFomoTensorArenaSize = 135 * 1024;
#endif
static uint8_t fomo_tensor_arena[kFomoTensorArenaSize] __attribute__((aligned(16)));
static tflite::MicroInterpreter* fomo_interpreter = nullptr;
static FomoOpResolver* fomo_op_resolver = nullptr;
static bool fomo_initialized = false;

// Cached quantization parameters (filled at init from the model's tensors).
// Standard EI/MobileNetV2 export: input scale ~ 1/255, zp = -128;
// output (post-softmax) scale = 1/256, zp = -128. We do not assume - we read.
static float fomo_in_scale = 0.0f;
static int32_t fomo_in_zero_point = 0;
static float fomo_out_scale = 0.0f;
static int32_t fomo_out_zero_point = 0;
static bool fomo_input_fast_path = false;  // true if int8 = uint8 - 128 is exact

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

#if defined(ML_EXPERIMENT_FOMO_HAND_DETECTION)
  const tflite::Model* model = ::tflite::GetModel(fomo_hand_detection_model);
#else
  const tflite::Model* model = ::tflite::GetModel(fomo_beer_can_model);
#endif
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
  if (input->dims->data[1] != FOMO_INPUT_HEIGHT ||
      input->dims->data[2] != FOMO_INPUT_WIDTH ||
      input->dims->data[3] != FOMO_INPUT_CHANNELS) {
    MicroPrintf("WARNING: Expected %dx%dx%d input, got %dx%dx%d",
                FOMO_INPUT_HEIGHT, FOMO_INPUT_WIDTH, FOMO_INPUT_CHANNELS,
                input->dims->data[1], input->dims->data[2],
                input->dims->data[3]);
  }

  if (output->dims->data[1] != FOMO_GRID_SIZE ||
      output->dims->data[2] != FOMO_GRID_SIZE ||
      output->dims->data[3] != FOMO_NUM_CLASSES) {
    MicroPrintf("WARNING: Expected %dx%dx%d output, got %dx%dx%d",
                FOMO_GRID_SIZE, FOMO_GRID_SIZE, FOMO_NUM_CLASSES,
                output->dims->data[1], output->dims->data[2],
                output->dims->data[3]);
  }

  size_t arena_used = fomo_interpreter->arena_used_bytes();
  MicroPrintf("FOMO arena used: %u bytes (of %u available)",
              arena_used, kFomoTensorArenaSize);

  // Cache quant params and detect whether the input fast path applies
  // (input scale within 1% of 1/255 AND zero_point == -128).
  fomo_in_scale       = input->params.scale;
  fomo_in_zero_point  = input->params.zero_point;
  fomo_out_scale      = output->params.scale;
  fomo_out_zero_point = output->params.zero_point;

  const float ideal = 1.0f / 255.0f;
  fomo_input_fast_path =
      (fomo_in_zero_point == -128) &&
      (fomo_in_scale > ideal * 0.99f) &&
      (fomo_in_scale < ideal * 1.01f);

  MicroPrintf("FOMO input quant: scale=%f zp=%d (fast_path=%d)",
              (double)fomo_in_scale, (int)fomo_in_zero_point,
              (int)fomo_input_fast_path);
  MicroPrintf("FOMO output quant: scale=%f zp=%d",
              (double)fomo_out_scale, (int)fomo_out_zero_point);

  fomo_initialized = true;
  MicroPrintf("FOMO model initialized successfully!");
  return TFLM_OK;
}

void fomo_get_quant_params(float *in_scale, int32_t *in_zero_point,
                           float *out_scale, int32_t *out_zero_point) {
  if (in_scale)       *in_scale       = fomo_in_scale;
  if (in_zero_point)  *in_zero_point  = fomo_in_zero_point;
  if (out_scale)      *out_scale      = fomo_out_scale;
  if (out_zero_point) *out_zero_point = fomo_out_zero_point;
}

/**
 * Run FOMO inference on an FOMO_INPUT_WIDTH x FOMO_INPUT_HEIGHT image
 * (grayscale or RGB depending on the active experiment).
 * Input: uint8 image data [0-255]
 * Output: int8 grid [1, FOMO_GRID_SIZE, FOMO_GRID_SIZE, FOMO_NUM_CLASSES]
 *
 * Quantization: input scale ~ 1/255, zero_point = -128
 * So uint8 -> int8 = byte - 128 (per channel/byte)
 */
tflm_status_t fomo_infer(const uint8_t* input_data_u8, int8_t* output_grid) {
  if (!fomo_initialized || fomo_interpreter == nullptr) {
    MicroPrintf("ERROR: FOMO model not initialized. Call fomo_initialize() first!");
    return TFLM_ERROR;
  }

  if (input_data_u8 == nullptr || output_grid == nullptr) {
    MicroPrintf("ERROR: Invalid input/output pointers");
    return TFLM_ERROR;
  }

  // Get input tensor
  TfLiteTensor* input = fomo_interpreter->input(0);

  // Quantize uint8 [0-255] -> int8 using model's actual scale/zero_point.
  // Fast path (typical EI MobileNetV2 export, scale ~ 1/255, zp = -128):
  //   int8 = uint8 - 128
  // Slow path (any other quant params):
  //   int8 = round((uint8/255) / scale) + zero_point
  // Per Edge Impulse's tflite_helper.h.
  int8_t* input_data = input->data.int8;
  if (fomo_input_fast_path) {
    for (int i = 0; i < FOMO_INPUT_SIZE; i++) {
      input_data[i] = static_cast<int8_t>(static_cast<int16_t>(input_data_u8[i]) - 128);
    }
  } else {
    const float inv_255 = 1.0f / 255.0f;
    const float inv_scale = 1.0f / fomo_in_scale;
    for (int i = 0; i < FOMO_INPUT_SIZE; i++) {
      float normalized = input_data_u8[i] * inv_255;
      int32_t q = static_cast<int32_t>(normalized * inv_scale + (normalized >= 0 ? 0.5f : -0.5f))
                  + fomo_in_zero_point;
      if (q < -128) q = -128;
      if (q >  127) q =  127;
      input_data[i] = static_cast<int8_t>(q);
    }
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

#endif  // ML_EXPERIMENT_FOMO_BEER_CAN || ML_EXPERIMENT_FOMO_HAND_DETECTION

/*=============================================================================
 * Person Detection Model Implementation
 *============================================================================*/

#if defined(ML_EXPERIMENT_VWW) || defined(ML_EXPERIMENT_VWW_RGB)

// Person detect model requires 7 ops for larger models
using PersonDetectOpResolver = tflite::MicroMutableOpResolver<7>;

static TfLiteStatus RegisterPersonDetectOps(PersonDetectOpResolver& op_resolver) {
  TF_LITE_ENSURE_STATUS(op_resolver.AddAveragePool2D());
  TF_LITE_ENSURE_STATUS(op_resolver.AddConv2D());
  TF_LITE_ENSURE_STATUS(op_resolver.AddDepthwiseConv2D());
  TF_LITE_ENSURE_STATUS(op_resolver.AddReshape());
  TF_LITE_ENSURE_STATUS(op_resolver.AddSoftmax());
  TF_LITE_ENSURE_STATUS(op_resolver.AddMean());
  TF_LITE_ENSURE_STATUS(op_resolver.AddFullyConnected());
  return kTfLiteOk;
}

// Person detect tensor arena - separate from FOMO
// Model benchmarked to use < 90KB RAM (RGB model may need more)
#if defined(ML_EXPERIMENT_VWW_RGB)
constexpr int kPersonDetectTensorArenaSize = 100 * 1024;  // RGB model needs more arena
#else
constexpr int kPersonDetectTensorArenaSize = 78 * 1024;
#endif
static uint8_t person_detect_tensor_arena[kPersonDetectTensorArenaSize] __attribute__((aligned(16)));
static tflite::MicroInterpreter* person_detect_interpreter = nullptr;
static PersonDetectOpResolver* person_detect_op_resolver = nullptr;
static bool person_detect_initialized = false;

/**
 * Initialize the person detection model
 */
tflm_status_t person_detect_initialize(void) {
#if defined(ML_EXPERIMENT_VWW_RGB)
  MicroPrintf("Initializing Person Detection RGB model...");
#else
  MicroPrintf("Initializing Person Detection model...");
#endif
#ifdef CMSIS_NN
  MicroPrintf("CMSIS-NN optimized kernels enabled");
#else
  MicroPrintf("Using reference kernels (CMSIS-NN not enabled)");
#endif

  tflite::InitializeTarget();

#if defined(ML_EXPERIMENT_VWW_RGB)
  const tflite::Model* model = ::tflite::GetModel(person_detect_rgb_tflite);
#else
  const tflite::Model* model = ::tflite::GetModel(person_detect_tflite);
#endif
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Person detect model schema version mismatch! Expected %d, got %d",
                TFLITE_SCHEMA_VERSION, model->version());
    return TFLM_ERROR;
  }

  // Create op resolver (needs to persist)
  static PersonDetectOpResolver static_person_detect_op_resolver;
  person_detect_op_resolver = &static_person_detect_op_resolver;

  TfLiteStatus status = RegisterPersonDetectOps(*person_detect_op_resolver);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register Person Detect ops");
    return TFLM_ERROR;
  }

  // Create interpreter
  static tflite::MicroInterpreter static_person_detect_interpreter(
      model, *person_detect_op_resolver, person_detect_tensor_arena,
      kPersonDetectTensorArenaSize);
  person_detect_interpreter = &static_person_detect_interpreter;

  status = person_detect_interpreter->AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("Person detect AllocateTensors failed");
    return TFLM_ERROR;
  }

  // Verify input/output dimensions
  TfLiteTensor* input = person_detect_interpreter->input(0);
  TfLiteTensor* output = person_detect_interpreter->output(0);

  MicroPrintf("Person detect input: dims=%d, shape=[%d,%d,%d,%d], type=%d",
              input->dims->size,
              input->dims->data[0], input->dims->data[1],
              input->dims->data[2], input->dims->data[3],
              input->type);

  MicroPrintf("Person detect output: dims=%d, type=%d",
              output->dims->size, output->type);

  // Verify expected dimensions: [1, 96, 96, channels] input
  if (input->dims->data[1] != 96 || input->dims->data[2] != 96) {
    MicroPrintf("WARNING: Expected 96x96 input, got %dx%d",
                input->dims->data[1], input->dims->data[2]);
  }
#if defined(ML_EXPERIMENT_VWW_RGB)
  if (input->dims->data[3] != 3) {
    MicroPrintf("WARNING: Expected 3 channels (RGB), got %d",
                input->dims->data[3]);
  }
#else
  if (input->dims->data[3] != 1) {
    MicroPrintf("WARNING: Expected 1 channel (grayscale), got %d",
                input->dims->data[3]);
  }
#endif

  size_t arena_used = person_detect_interpreter->arena_used_bytes();
  MicroPrintf("Person detect arena used: %u bytes (of %u available)",
              arena_used, kPersonDetectTensorArenaSize);

  person_detect_initialized = true;
  MicroPrintf("Person detection model initialized successfully!");
  return TFLM_OK;
}

/**
 * Run person detection inference on a 96x96 grayscale image
 * Input: uint8 grayscale [0-255]
 * Output: int8 scores [not_person, person]
 *
 * Quantization: input scale ~ 1/255, zero_point = -128
 * So uint8_grayscale -> int8_input = grayscale - 128
 */
tflm_status_t person_detect_infer(const uint8_t* input_grayscale, int8_t* output_scores) {
  if (!person_detect_initialized || person_detect_interpreter == nullptr) {
    MicroPrintf("ERROR: Person detect model not initialized");
    return TFLM_ERROR;
  }

  if (input_grayscale == nullptr || output_scores == nullptr) {
    MicroPrintf("ERROR: Invalid input/output pointers");
    return TFLM_ERROR;
  }

  // Get input tensor
  TfLiteTensor* input = person_detect_interpreter->input(0);

  // Convert uint8 grayscale [0-255] to int8 [-128, 127]
  int8_t* input_data = input->data.int8;
  for (int i = 0; i < PERSON_INPUT_SIZE; i++) {
    input_data[i] = static_cast<int8_t>(static_cast<int16_t>(input_grayscale[i]) - 128);
  }

  // Run inference
  TfLiteStatus status = person_detect_interpreter->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("Person detect Invoke failed");
    return TFLM_ERROR;
  }

  // Copy output (2 int8 values)
  TfLiteTensor* output = person_detect_interpreter->output(0);
  memcpy(output_scores, output->data.int8, PERSON_OUTPUT_SIZE);

  return TFLM_OK;
}

/**
 * Check if person detection model is initialized
 */
bool person_detect_is_initialized(void) {
  return person_detect_initialized;
}

#endif  // ML_EXPERIMENT_VWW || ML_EXPERIMENT_VWW_RGB

/*=============================================================================
 * Hello World Model Implementation
 *============================================================================*/

#if defined(ML_EXPERIMENT_HELLO_WORLD)

namespace {
using HelloWorldOpResolver = tflite::MicroMutableOpResolver<1>;

TfLiteStatus RegisterHelloWorldOps(HelloWorldOpResolver& op_resolver) {
  TF_LITE_ENSURE_STATUS(op_resolver.AddFullyConnected());
  return kTfLiteOk;
}

// Float model storage
constexpr int kTensorArenaSize = 3000;
uint8_t tensor_arena[kTensorArenaSize] __attribute__((aligned(16)));
tflite::MicroInterpreter* interpreter = nullptr;
HelloWorldOpResolver* op_resolver = nullptr;

// Int8 model storage
constexpr int kTensorArenaSizeInt8 = 2500;
uint8_t tensor_arena_int8[kTensorArenaSizeInt8] __attribute__((aligned(16)));
tflite::MicroInterpreter* interpreter_int8 = nullptr;
HelloWorldOpResolver* op_resolver_int8 = nullptr;

// Cached quantization parameters for int8 model
struct QuantizationParams {
  float input_scale;
  int32_t input_zero_point;
  float output_scale;
  int32_t output_zero_point;
  float input_scale_inv;
} int8_quant_params = {0};

}  // namespace

/**
 * Initialize the float hello_world model
 */
tflm_status_t tflm_initialize(void) {
  MicroPrintf("Initializing TFLM hello_world float model...");
#ifdef CMSIS_NN
  MicroPrintf("CMSIS-NN optimized kernels enabled");
#else
  MicroPrintf("Using reference kernels (CMSIS-NN not enabled)");
#endif

  tflite::InitializeTarget();

  const tflite::Model* model = ::tflite::GetModel(g_hello_world_float_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Float model schema version mismatch!");
    return TFLM_ERROR;
  }

  static HelloWorldOpResolver static_op_resolver;
  op_resolver = &static_op_resolver;

  TfLiteStatus status = RegisterHelloWorldOps(*op_resolver);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register ops for float model");
    return TFLM_ERROR;
  }

  static tflite::MicroInterpreter static_interpreter(
      model, *op_resolver, tensor_arena, kTensorArenaSize);
  interpreter = &static_interpreter;

  status = interpreter->AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("AllocateTensors failed for float model");
    return TFLM_ERROR;
  }

  MicroPrintf("Float model initialized successfully!");
  return TFLM_OK;
}

/**
 * Run inference on float model
 */
tflm_status_t tflm_infer(float input, float* output) {
  if (interpreter == nullptr) {
    MicroPrintf("ERROR: Float model not initialized");
    return TFLM_ERROR;
  }

  interpreter->input(0)->data.f[0] = input;

  TfLiteStatus status = interpreter->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("Float model invoke failed");
    return TFLM_ERROR;
  }

  *output = interpreter->output(0)->data.f[0];
  return TFLM_OK;
}

/**
 * Initialize the int8 quantized model
 */
tflm_status_t tflm_initialize_int8(void) {
  MicroPrintf("Initializing TFLM hello_world int8 model...");

  tflite::InitializeTarget();

  const tflite::Model* model = ::tflite::GetModel(g_hello_world_int8_model_data);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    MicroPrintf("Int8 model schema version mismatch!");
    return TFLM_ERROR;
  }

  static HelloWorldOpResolver static_op_resolver_int8;
  op_resolver_int8 = &static_op_resolver_int8;

  TfLiteStatus status = RegisterHelloWorldOps(*op_resolver_int8);
  if (status != kTfLiteOk) {
    MicroPrintf("Failed to register ops for int8 model");
    return TFLM_ERROR;
  }

  static tflite::MicroInterpreter static_interpreter_int8(
      model, *op_resolver_int8, tensor_arena_int8, kTensorArenaSizeInt8);
  interpreter_int8 = &static_interpreter_int8;

  status = interpreter_int8->AllocateTensors();
  if (status != kTfLiteOk) {
    MicroPrintf("AllocateTensors failed for int8 model");
    return TFLM_ERROR;
  }

  // Cache quantization parameters
  TfLiteTensor* input_tensor = interpreter_int8->input(0);
  TfLiteTensor* output_tensor = interpreter_int8->output(0);

  int8_quant_params.input_scale = input_tensor->params.scale;
  int8_quant_params.input_zero_point = input_tensor->params.zero_point;
  int8_quant_params.output_scale = output_tensor->params.scale;
  int8_quant_params.output_zero_point = output_tensor->params.zero_point;
  int8_quant_params.input_scale_inv = 1.0f / int8_quant_params.input_scale;

  MicroPrintf("Int8 model initialized successfully!");
  return TFLM_OK;
}

/**
 * Run inference on int8 model with automatic quantization
 */
tflm_status_t tflm_infer_int8(float input, float* output) {
  if (interpreter_int8 == nullptr) {
    MicroPrintf("ERROR: Int8 model not initialized");
    return TFLM_ERROR;
  }

  TfLiteTensor* input_tensor = interpreter_int8->input(0);
  TfLiteTensor* output_tensor = interpreter_int8->output(0);

  // Quantize input
  float input_scaled = input * int8_quant_params.input_scale_inv + int8_quant_params.input_zero_point;
  int32_t input_quantized_32 = static_cast<int32_t>(input_scaled + (input_scaled >= 0 ? 0.5f : -0.5f));
  input_quantized_32 = (input_quantized_32 < -128) ? -128 : input_quantized_32;
  input_quantized_32 = (input_quantized_32 > 127) ? 127 : input_quantized_32;
  input_tensor->data.int8[0] = static_cast<int8_t>(input_quantized_32);

  // Run inference
  TfLiteStatus status = interpreter_int8->Invoke();
  if (status != kTfLiteOk) {
    MicroPrintf("Int8 model invoke failed");
    return TFLM_ERROR;
  }

  // Dequantize output
  int8_t output_quantized = output_tensor->data.int8[0];
  *output = static_cast<float>(output_quantized - int8_quant_params.output_zero_point) * int8_quant_params.output_scale;

  return TFLM_OK;
}

/**
 * Get float model info
 */
tflm_status_t tflm_get_float_model_info(tflm_model_info_t* info) {
  if (info == nullptr) return TFLM_ERROR;

  info->type = TFLM_MODEL_FLOAT;
  info->model_size_bytes = g_hello_world_float_model_data_size;
  info->arena_size_bytes = kTensorArenaSize;
  info->initialized = (interpreter != nullptr);

  return TFLM_OK;
}

/**
 * Get int8 model info
 */
tflm_status_t tflm_get_int8_model_info(tflm_model_info_t* info) {
  if (info == nullptr) return TFLM_ERROR;

  info->type = TFLM_MODEL_INT8;
  info->model_size_bytes = g_hello_world_int8_model_data_size;
  info->arena_size_bytes = kTensorArenaSizeInt8;
  info->initialized = (interpreter_int8 != nullptr);

  return TFLM_OK;
}

#endif  // ML_EXPERIMENT_HELLO_WORLD

} // extern "C"
