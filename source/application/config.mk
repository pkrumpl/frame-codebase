# ML Experiment Selection
# Valid values: HELLO_WORLD, VWW, VWW_RGB, FOMO_BEER_CAN
ML_EXPERIMENT = VWW

# CMSIS-NN optimized kernels for TFLM.
#   1 = enable (default, faster; uses ARM-optimized integer kernels)
#   0 = disable (fall back to TFLM reference kernels - slower, useful for
#       benchmarking the speedup or for debugging numeric behaviour)
USE_CMSIS_NN = 0
