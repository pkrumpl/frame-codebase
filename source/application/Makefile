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
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_time.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_helpers.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_op_resolver.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_context.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_interpreter_context.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/flatbuffer_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/recording_micro_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/fake_micro_context.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/system_setup.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/debug_log.cc \
	$(LIBRARIES)/tflm/tensorflow/compiler/mlir/lite/schema/schema_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/tflite_bridge/micro_error_reporter.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/quantization_util.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/internal/portable_tensor_utils.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/single_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/recording_single_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/persistent_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator/non_persistent_arena_buffer_allocator.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner/greedy_memory_planner.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner/linear_memory_planner.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner/non_persistent_buffer_planner_shim.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/micro_allocation_info.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/tflite_bridge/flatbuffer_conversions_bridge.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/kernels/kernel_util.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/fully_connected.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/kernels/fully_connected_common.cc \
	$(LIBRARIES)/tflm/tensorflow/lite/micro/examples/hello_world/models/hello_world_float_model_data.cc

# Common header file paths (used by both C and C++)
COMMON_INCLUDE_FLAGS := \
	-I. \
	-I.. \
	-I../fpga \
	-Ilua_libraries \
	-Ilua_libraries/graphical_assets \
	-I$(LIBRARIES)/cmsis/CMSIS/Core/Include \
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
	-I$(LIBRARIES)/tflm/third_party/flatbuffers/include \
	-I$(LIBRARIES)/tflm/third_party/gemmlowp \
	-I$(LIBRARIES)/tflm/third_party/ruy

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
	-O2 \
	-DBUILD_VERSION='"$(BUILD_VERSION)"' \
	-DGIT_COMMIT='"$(GIT_COMMIT)"' \
	-DLFS_NO_DEBUG \
	-DLFS_NO_ERROR \
	-DLFS_NO_WARN \
	-DNDEBUG \
	-DNRF52840_XXAA

# Check if build is for development kit and include flag if necessary
ifdef DEV_KIT
$(info Including DEV_KIT_BUILD flag for Development Kit build...)
COMMON_FLAGS += -DDEV_KIT_BUILD
endif

# C-specific flags (includes local picolibc)
CFLAGS := $(COMMON_FLAGS) $(COMMON_INCLUDE_FLAGS) $(C_ONLY_INCLUDE_FLAGS) -std=gnu17

# C++-specific flags (excludes local picolibc to avoid header conflicts)
CXXFLAGS := $(COMMON_FLAGS) $(COMMON_INCLUDE_FLAGS) \
	-std=gnu++14 \
	-fno-rtti \
	-fno-exceptions \
	-fno-threadsafe-statics \
	-fno-use-cxa-atexit \
	-DTF_LITE_STATIC_MEMORY \
	-DTF_LITE_DISABLE_X86_NEON \
	-DTF_LITE_STRIP_ERROR_STRINGS \
	-D_READ_WRITE_RETURN_TYPE=_ssize_t \
	-D_READ_WRITE_BUFSIZE_TYPE=int \
	-D_Thread_local=thread_local \
	-D__malloc_like= \
	-D__result_use_check=

# Linker flags
LDFLAGS := \
	-Wl,--gc-sections \
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
                    $(BUILD)/obj/kernel_util_kernels.o \
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
         $(LIBRARIES)/tflm/tensorflow/lite/core/c \
         $(LIBRARIES)/tflm/tensorflow/lite/core/api \
         $(LIBRARIES)/tflm/tensorflow/lite/micro \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/arena_allocator \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/memory_planner \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/kernels \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/tflite_bridge \
         $(LIBRARIES)/tflm/tensorflow/lite/kernels \
         $(LIBRARIES)/tflm/tensorflow/lite/kernels/internal \
         $(LIBRARIES)/tflm/tensorflow/compiler/mlir/lite/schema \
         $(LIBRARIES)/tflm/tensorflow/lite/micro/examples/hello_world/models

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
	@arm-none-eabi-size $(BUILD)/application.elf | awk 'NR==1 {print; next} {text=$$1; data=$$2; bss=$$3; flash_used=(text+data)/1024; ram_used=(data+bss)/1024; flash_total=824; ram_total=245; flash_remain=flash_total-flash_used; ram_remain=ram_total-ram_used; flash_pct=flash_used/flash_total*100; ram_pct=ram_used/ram_total*100; printf "\nFLASH:\n  Used:      %.2f KB (%.1f%%)\n  Remaining: %.2f KB\n  Total:     %d KB\n\nRAM:\n  Used:      %.2f KB (%.1f%%)\n  Remaining: %.2f KB\n  Total:     %d KB\n\n====== SECTION SIZES ======\n  .text: %.2f KB (code)\n  .data: %.2f KB (initialized data)\n  .bss:  %.2f KB (uninitialized data)\n", flash_used, flash_pct, flash_remain, flash_total, ram_used, ram_pct, ram_remain, ram_total, text/1024, data/1024, bss/1024}'
	@echo ""

# Clean
clean:
	@rm -rf $(BUILD)/obj
	@rm -f $(BUILD)/application.elf $(BUILD)/application.hex

.PHONY: all clean size-report