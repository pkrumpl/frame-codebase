# ML Experiment Selection
# Valid values: HELLO_WORLD, VWW, VWW_RGB, FOMO_BEER_CAN
ML_EXPERIMENT = VWW_RGB

# CMSIS-NN optimized kernels for TFLM. 1 = enable, 0 = reference kernels.
USE_CMSIS_NN = 1

# Hardware watchdog reload window in milliseconds. Must exceed the
# longest single TFLM Invoke() call.
WATCHDOG_TIMEOUT_MS = 60000
