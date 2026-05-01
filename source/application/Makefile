#
# This file is a part of: https://github.com/brilliantlabsAR/frame-codebase
#
# Authored by: Raj Nakarja / Brilliant Labs Ltd. (raj@brilliant.xyz)
#              Rohit Rathnam / Silicon Witchery AB (rohit@siliconwitchery.com)
#              Uma S. Gupta / Techno Exponent (umasankar@technoexponent.com)
#
# ISC Licence
#
# Copyright © 2023 Brilliant Labs Ltd.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
# REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
# OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.
#

BUILD_VERSION ?= $(shell TZ= date +v%y.%j.%H%M)
GIT_COMMIT := $(shell git rev-parse --short HEAD)

$(info BUILD_VERSION is $(BUILD_VERSION))
$(info GIT_COMMIT is $(GIT_COMMIT))

# Include local config if it exists
-include config.mk

# ML Experiment selection - default to VWW if not set
ML_EXPERIMENT ?= VWW
$(info ML_EXPERIMENT is $(ML_EXPERIMENT))

# Validate ML_EXPERIMENT value
ifeq ($(filter $(ML_EXPERIMENT),VWW VWW_RGB FOMO_BEER_CAN HELLO_WORLD),)
$(error Invalid ML_EXPERIMENT value '$(ML_EXPERIMENT)'. Valid values: VWW, VWW_RGB, FOMO_BEER_CAN, HELLO_WORLD)
endif

# CMSIS-NN selection - default ON if not set in config.mk
USE_CMSIS_NN ?= 1
$(info USE_CMSIS_NN is $(USE_CMSIS_NN))

ifeq ($(filter $(USE_CMSIS_NN),0 1),)
$(error Invalid USE_CMSIS_NN value '$(USE_CMSIS_NN)'. Valid values: 0, 1)
endif

LIBRARIES := ../../libraries
BUILD := ../../build

# Check ARM GCC toolchain version (require 12.3.1)
# https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
GCC_VERSION := $(shell arm-none-eabi-gcc -dumpfullversion -dumpversion)
REQUIRED_GCC_VERSION := 12.3.1

ifneq ($(GCC_VERSION),$(REQUIRED_GCC_VERSION))
    $(error ERROR: ARM GNU Toolchain version $(REQUIRED_GCC_VERSION) is required. Found version $(GCC_VERSION). This build is only tested with version $(REQUIRED_GCC_VERSION).x)
endif

# C source files
# Note: bluetooth.c and compression.c are handled with explicit rules to avoid naming conflicts
C_FILES += \
	main.c \
	boot_safety.c \
	flash.c \
	luaport.c \
	spi.c \
	watchdog.c \
	lua_libraries/camera.c \
	lua_libraries/display.c \
	lua_libraries/experiment_common.c \
	lua_libraries/file.c \
	lua_libraries/imu.c \
	lua_libraries/led.c \
	lua_libraries/microphone.c \
	lua_libraries/system.c \
	lua_libraries/time.c \
	lua_libraries/version.c \
	../error_logging.c \
	../i2c.c \
	../startup.c \
	../syscalls.c \
	$(LIBRARIES)/littlefs/lfs_util.c \
	$(LIBRARIES)/littlefs/lfs.c \
	$(LIBRARIES)/lua/lapi.c \
	$(LIBRARIES)/lua/lauxlib.c \
	$(LIBRARIES)/lua/lbaselib.c \
	$(LIBRARIES)/lua/lcode.c \
	$(LIBRARIES)/lua/lcorolib.c \
	$(LIBRARIES)/lua/lctype.c \
	$(LIBRARIES)/lua/ldblib.c \
	$(LIBRARIES)/lua/ldebug.c \
	$(LIBRARIES)/lua/ldo.c \
	$(LIBRARIES)/lua/ldump.c \
	$(LIBRARIES)/lua/lfunc.c \
	$(LIBRARIES)/lua/lgc.c \
	$(LIBRARIES)/lua/linit.c \
	$(LIBRARIES)/lua/llex.c \
	$(LIBRARIES)/lua/lmathlib.c \
	$(LIBRARIES)/lua/lmem.c \
	$(LIBRARIES)/lua/loadlib.c \
	$(LIBRARIES)/lua/lobject.c \
	$(LIBRARIES)/lua/lopcodes.c \
	$(LIBRARIES)/lua/lparser.c \
	$(LIBRARIES)/lua/lstate.c \
	$(LIBRARIES)/lua/lstring.c \
	$(LIBRARIES)/lua/lstrlib.c \
	$(LIBRARIES)/lua/ltable.c \
	$(LIBRARIES)/lua/ltablib.c \
	$(LIBRARIES)/lua/ltm.c \
	$(LIBRARIES)/lua/lundump.c \
	$(LIBRARIES)/lua/lutf8lib.c \
	$(LIBRARIES)/lua/lvm.c \
	$(LIBRARIES)/lua/lzio.c \
	$(LIBRARIES)/lz4/lz4.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_gpiote.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_pdm.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_pwm.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_rtc.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_saadc.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_spim.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_systick.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_twim.c \
	$(LIBRARIES)/nrfx/drivers/src/nrfx_wdt.c \
	$(LIBRARIES)/nrfx/helpers/nrfx_flag32_allocator.c \
	$(LIBRARIES)/nrfx/mdk/system_nrf52840.c \
	$(LIBRARIES)/segger/SEGGER_RTT.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_1x1_s8_fast.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_1x1_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_1_x_n_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_wrapper_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_get_buffer_sizes_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_wrapper_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_get_buffer_sizes_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_wrapper_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_get_buffer_sizes_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_1x1_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_1x1_s4_fast.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_convolve_1_x_n_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_nn_mat_mult_kernel_row_offset_s8_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_nn_mat_mult_kernel_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_nn_mat_mult_kernel_s4_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_nn_mat_mult_kernel_s8_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_3x3_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_s8_opt.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_wrapper_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_get_buffer_sizes_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_wrapper_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_get_buffer_sizes_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_s4_opt.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_wrapper_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_depthwise_conv_get_buffer_sizes_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_nn_depthwise_conv_s8_core.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions/arm_nn_mat_mult_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mul_core_1x_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mul_core_4x_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mul_core_1x_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mult_nt_t_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mult_nt_t_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mult_nt_t_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_mat_mult_nt_interleaved_t_even_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_depthwise_conv_nt_t_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_depthwise_conv_nt_t_padded_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_depthwise_conv_nt_t_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_depthwise_conv_nt_t_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nntables.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_q7_to_q15_with_offset.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_s8_to_s16_unordered_with_offset.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions/arm_avgpool_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions/arm_avgpool_get_buffer_sizes_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions/arm_avgpool_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions/arm_avgpool_get_buffer_sizes_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ReshapeFunctions/arm_reshape_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/SoftmaxFunctions/arm_softmax_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/SoftmaxFunctions/arm_softmax_s8_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/SoftmaxFunctions/arm_softmax_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/SoftmaxFunctions/arm_nn_softmax_common_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_s4.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_wrapper_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_per_channel_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_get_buffer_sizes_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_fully_connected_get_buffer_sizes_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions/arm_vector_sum_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions/arm_nn_vec_mat_mult_t_per_ch_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions/arm_max_pool_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions/arm_max_pool_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/BasicMathFunctions/arm_elementwise_add_s8.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/BasicMathFunctions/arm_elementwise_add_s16.c \
	$(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PadFunctions/arm_pad_s8.c \
	$(LIBRARIES)/tjpgd/tjpgd.c \

# Add selected experiment file based on ML_EXPERIMENT
ifeq ($(ML_EXPERIMENT),VWW)
C_FILES += lua_libraries/experiment_vww.c
endif
ifeq ($(ML_EXPERIMENT),VWW_RGB)
C_FILES += lua_libraries/experiment_vww_rgb.c
endif
ifeq ($(ML_EXPERIMENT),FOMO_BEER_CAN)
C_FILES += lua_libraries/experiment_fomo.c
endif
ifeq ($(ML_EXPERIMENT),HELLO_WORLD)
C_FILES += lua_libraries/experiment_hello_world.c
endif

# C++ source files (add your C++ files here)
# Note: common.cc files are handled with explicit rules to avoid naming conflicts
CPP_FILES += \
	tflm_wrapper.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/core/api/flatbuffer_conversions.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/core/api/tensor_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_interpreter.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_interpreter_graph.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_log.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_resource_variable.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_profiler.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/cortex_m_generic/micro_time.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_helpers.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_op_resolver.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_context.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_interpreter_context.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/flatbuffer_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/recording_micro_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/fake_micro_context.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/system_setup.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/cortex_m_generic/debug_log.cc \
	$(LIBRARIES)/tflm/tensorflow/compiler/mlir/lite/schema/schema_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/tflite_bridge/micro_error_reporter.cc \
	$(LIBRARIES)/tflm/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/quantization_util.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/portable_tensor_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/tensor_ctypes.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/runtime_shape.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/recording_single_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner/linear_memory_planner.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner/non_persistent_buffer_planner_shim.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_allocation_info.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/conv_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/depthwise_conv_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/pooling_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/reshape.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/reshape_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/softmax_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/reduce.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/reduce_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/fully_connected_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/add_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/pad_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/activations.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/activations_common.cc

# TFLM kernel registration variants. Each of these ops has two
# implementations - the CMSIS-NN-optimized one in kernels/cmsis_nn/ and
# the upstream reference one in kernels/. They share Register_*() symbol
# names, so we pick exactly one set per build.
ifeq ($(USE_CMSIS_NN),1)
TFLM_KERNEL_DIR := tensorflow/lite/micro/kernels/cmsis_nn
else
TFLM_KERNEL_DIR := tensorflow/lite/micro/kernels
endif

CPP_FILES += \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/conv.cc \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/depthwise_conv.cc \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/pooling.cc \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/softmax.cc \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/fully_connected.cc \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/add.cc \
	$(LIBRARIES)/tflm/$(TFLM_KERNEL_DIR)/pad.cc

# Add HELLO_WORLD model data files
ifeq ($(ML_EXPERIMENT),HELLO_WORLD)
CPP_FILES += \
	$(LIBRARIES)/tflm/examples/hello_world/models/hello_world_float_model_data.cc \
	$(LIBRARIES)/tflm/examples/hello_world/models/hello_world_int8_model_data.cc
endif

# Common header file paths (used by both C and C++)
COMMON_INCLUDE_FLAGS := \
	-I. \
	-I.. \
	-I../fpga \
	-Ilua_libraries \
	-Ilua_libraries/graphical_assets \
	-I$(LIBRARIES)/cmsis/CMSIS/Core/Include \
	-I$(LIBRARIES)/tflm/third_party/cmsis_nn/Include \
	-I$(LIBRARIES)/tflm/third_party/cmsis_nn \
	-I$(LIBRARIES)/littlefs \
	-I$(LIBRARIES)/lua \
	-I$(LIBRARIES)/lz4 \
	-I$(LIBRARIES)/nrfx \
	-I$(LIBRARIES)/nrfx/drivers/include \
	-I$(LIBRARIES)/nrfx/hal \
	-I$(LIBRARIES)/nrfx/mdk \
	-I$(LIBRARIES)/nrfx/soc \
	-I$(LIBRARIES)/segger \
	-I$(LIBRARIES)/softdevice/include \
	-I$(LIBRARIES)/tflm \
	-I$(LIBRARIES)/tflm/examples/hello_world \
	-I$(LIBRARIES)/tflm/third_party/flatbuffers/include \
	-I$(LIBRARIES)/tflm/third_party/gemmlowp \
	-I$(LIBRARIES)/tflm/third_party/ruy \
	-I$(LIBRARIES)/tjpgd

# C-only header paths (excluded from C++ to avoid conflicts)
C_ONLY_INCLUDE_FLAGS := \
	-I$(LIBRARIES)/picolibc

# Common flags for both C and C++
COMMON_FLAGS += \
	-Wall \
	-Wdouble-promotion  \
	-Wfloat-conversion \
	-falign-functions=16 \
	-fdata-sections  \
	-ffunction-sections  \
	-fmax-errors=1 \
	-fno-delete-null-pointer-checks \
	-fno-strict-aliasing \
	-fshort-enums \
	-g \
	-mabi=aapcs \
	-mcpu=cortex-m4 \
	-mfloat-abi=hard \
	-mthumb \
	-nostdlib \
	-Os \
	-DBUILD_VERSION='"$(BUILD_VERSION)"' \
	-DGIT_COMMIT='"$(GIT_COMMIT)"' \
	-DLFS_NO_DEBUG \
	-DLFS_NO_ERROR \
	-DLFS_NO_WARN \
	-DNDEBUG \
	-DNRF52840_XXAA \

# Check if build is for development kit and include flag if necessary
ifdef DEV_KIT
$(info Including DEV_KIT_BUILD flag for Development Kit build...)
COMMON_FLAGS += -DDEV_KIT_BUILD
endif

# Add ML experiment define
COMMON_FLAGS += -DML_EXPERIMENT_$(ML_EXPERIMENT)

#* Note: Disable microphone functionality to save RAM
COMMON_FLAGS += -DFRAME_DISABLE_MICROPHONE

# CMSIS-NN compile flag, gated by USE_CMSIS_NN. When disabled, the TFLM
# kernels' internal `#ifdef CMSIS_NN` guards take the reference-impl
# branch, and tflm_wrapper.cc emits a #pragma message warning.
ifeq ($(USE_CMSIS_NN),1)
CMSIS_NN_FLAG := -DCMSIS_NN
else
CMSIS_NN_FLAG :=
endif

# C-specific flags (includes local picolibc)
CFLAGS := $(COMMON_FLAGS) $(COMMON_INCLUDE_FLAGS) $(C_ONLY_INCLUDE_FLAGS) \
	-std=gnu17 \
	$(CMSIS_NN_FLAG) \
	-DARM_MATH_DSP \
	-DARM_MATH_LOOPUNROLL

# C++-specific flags (excludes local picolibc to avoid header conflicts)
CXXFLAGS := $(COMMON_FLAGS) $(COMMON_INCLUDE_FLAGS) \
	-std=gnu++14 \
	-fno-rtti \
	-fno-exceptions \
	-fno-threadsafe-statics \
	-fno-use-cxa-atexit \
	-DTF_LITE_STATIC_MEMORY \
	-DTF_LITE_DISABLE_X86_NEON \
	$(CMSIS_NN_FLAG) \
	-DARM_MATH_DSP \
	-DARM_MATH_LOOPUNROLL \
	-D_READ_WRITE_RETURN_TYPE=_ssize_t \
	-D_READ_WRITE_BUFSIZE_TYPE=int \
	-D_Thread_local=thread_local \
	-D__malloc_like= \
	-D__result_use_check=

# Check if build is for development kit to remove timing functions that cause errors
ifdef DEV_KIT
$(info Including cxx development flags for Development Kit build...)
# To skip timing functions that cause errors
CXXFLAGS += -DARMCM0
endif

# Strip the error strings from TensorFlow Lite Micro to reduce binary size for non-development builds
ifndef DEV_KIT
$(info Including TF_LITE_STRIP_ERROR_STRINGS flag for non-Development Kit build to reduce binary size...)
CXXFLAGS += -DTF_LITE_STRIP_ERROR_STRINGS
endif

# Linker flags
LDFLAGS := \
	-Wl,--gc-sections \
	-Wl,-Map=$(BUILD)/application.map \
	-T ../memory_layout.ld \
	-T section_layout.ld \
	-L$(LIBRARIES)/picolibc \

# Link required libraries
LINKED_LIBRARIES += \
	-lc \
	-lgcc \
	-lstdc++ \
	-lm

# Generate object file lists using notdir to flatten paths, then add BUILD/obj prefix
C_OBJECTS := $(addprefix $(BUILD)/obj/,$(notdir $(C_FILES:.c=.o)))
# Handle both .cpp and .cc files
CPP_CPP_FILES := $(filter %.cpp,$(CPP_FILES))
CPP_CC_FILES := $(filter %.cc,$(CPP_FILES))
CPP_OBJECTS := $(addprefix $(BUILD)/obj/,$(notdir $(CPP_CPP_FILES:.cpp=.o))) \
               $(addprefix $(BUILD)/obj/,$(notdir $(CPP_CC_FILES:.cc=.o)))
# Add explicit object files for conflicting names
EXPLICIT_OBJECTS := $(BUILD)/obj/common_core.o $(BUILD)/obj/common_kernels.o \
                    $(BUILD)/obj/kernel_util_kernels.o $(BUILD)/obj/kernel_util_micro.o \
                    $(BUILD)/obj/bluetooth_app.o $(BUILD)/obj/bluetooth_lua.o \
                    $(BUILD)/obj/compression_app.o $(BUILD)/obj/compression_lua.o
ALL_OBJECTS := $(C_OBJECTS) $(CPP_OBJECTS) $(EXPLICIT_OBJECTS)

# VPATH tells Make where to find source files
VPATH := . .. lua_libraries \
         $(LIBRARIES)/littlefs \
         $(LIBRARIES)/lua \
         $(LIBRARIES)/lz4 \
         $(LIBRARIES)/nrfx/drivers/src \
         $(LIBRARIES)/nrfx/helpers \
         $(LIBRARIES)/nrfx/mdk \
         $(LIBRARIES)/segger \
         $(LIBRARIES)/cpp_example \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ActivationFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/BasicMathFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConcatenationFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ConvolutionFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/FullyConnectedFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/NNSupportFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PadFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/PoolingFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/ReshapeFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/SoftmaxFunctions \
         $(LIBRARIES)/tflm/third_party/cmsis_nn/Source/SVDFunctions \
         $(LIBRARIES)/tflm/tensorflow/lite/core/c \
         $(LIBRARIES)/tflm/tensorflow/lite/core/api \
         $(LIBRARIES)/tflm/tensorflow/lite/micro \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/cortex_m_generic \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/kernels \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/cmsis_nn \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/tflite_bridge \
         $(LIBRARIES)/tflm/tensorflow/lite/kernels \
         $(LIBRARIES)/tflm/tensorflow/lite/kernels/internal \
         $(LIBRARIES)/tflm/tensorflow/compiler/mlir/lite/schema \
         $(LIBRARIES)/tflm/examples/hello_world/models \
         $(LIBRARIES)/tjpgd

# Default target
all: $(BUILD)/application.hex

# Explicit rules for conflicting filenames
$(BUILD)/obj/common_core.o: $(LIBRARIES)/tflm/tensorflow/lite/core/c/common.cc
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/common_kernels.o: $(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/common.cc
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/kernel_util_kernels.o: $(LIBRARIES)/tflm/tensorflow/lite/kernels/kernel_util.cc
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/kernel_util_micro.o: $(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/kernel_util.cc
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/obj/bluetooth_app.o: bluetooth.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

$(BUILD)/obj/bluetooth_lua.o: lua_libraries/bluetooth.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

$(BUILD)/obj/compression_app.o: compression.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

$(BUILD)/obj/compression_lua.o: lua_libraries/compression.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

$(BUILD)/obj/error_reporter.o: $(LIBRARIES)/tflm/tensorflow/compiler/mlir/lite/core/api/error_reporter.cc
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

# Compile C files
$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@arm-none-eabi-gcc $(CFLAGS) -c $< -o $@

# Compile C++ files (.cpp)
$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

# Compile C++ files (.cc)
$(BUILD)/obj/%.o: %.cc
	@mkdir -p $(dir $@)
	@echo "  CXX   $<"
	@arm-none-eabi-g++ $(CXXFLAGS) -c $< -o $@

# Link everything
$(BUILD)/application.elf: $(ALL_OBJECTS)
	@echo "  LINK  $@"
	@arm-none-eabi-g++ $(COMMON_FLAGS) $(LDFLAGS) -o $@ $^ $(LINKED_LIBRARIES)

# Convert to HEX
$(BUILD)/application.hex: $(BUILD)/application.elf
	@echo "  HEX   $@"
	@arm-none-eabi-objcopy -O ihex $< $@
	@echo "Build complete:"
	@arm-none-eabi-size $<

# Show detailed size report with remaining memory
size-report: $(BUILD)/application.elf
	@echo ""
	@echo "====== MEMORY USAGE REPORT ======"
	@arm-none-eabi-size $(BUILD)/application.elf | awk 'NR==1 {print; next} {text=$$1; data=$$2; bss=$$3; flash_used=(text+data)/1024; ram_used=(data+bss)/1024; flash_total=824; ram_total=245; flash_remain=flash_total-flash_used; ram_remain=ram_total-ram_used; flash_pct=flash_used/flash_total*100; ram_pct=ram_used/ram_total*100; printf "\nFLASH:\n  Used:      %.2f KB (%.1f%%)\n  Remaining: %.2f KB\n  Total:     %d KB\n\nRAM:\n  Used:      %.2f KB (%.1f%%)\n  Remaining: %.2f KB (will be used for heap)\n  Total:     %d KB\n\n====== SECTION SIZES ======\n  .text: %.2f KB (code)\n  .data: %.2f KB (initialized data)\n  .bss:  %.2f KB (uninitialized data)\n", flash_used, flash_pct, flash_remain, flash_total, ram_used, ram_pct, ram_remain, ram_total, text/1024, data/1024, bss/1024}'
	@echo ""

# Clean
clean:
	@rm -rf $(BUILD)/obj
	@rm -f $(BUILD)/application.elf $(BUILD)/application.hex

.PHONY: all clean size-report