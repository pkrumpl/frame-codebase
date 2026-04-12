# 🧪 Benchmark Results – Hello World (Float vs. Quantized INT8)

This document compares the benchmark results of the **Hello World** TensorFlow Lite Micro example on the **Brilliant Labs Frame** (nRF52840, Cortex-M4F @ 64 MHz).

- **Float model:** benchmarked on **18 November 2025** on [d1444f82470fdaadc570491b27e9fd8f7f6ccfb6](https://github.com/pkrumpl/frame-codebase/tree/d1444f82470fdaadc570491b27e9fd8f7f6ccfb6)
- **Quantized INT8 model:** benchmarked on **19 November 2025** on [60861f57b4b0c9d03c3b34d74ad57ac38339afb8](https://github.com/pkrumpl/frame-codebase/tree/60861f57b4b0c9d03c3b34d74ad57ac38339afb8)
- **Quantized INT8 model with CMSIS-NN support:** benchmarked on **02 December 2025** on [5e356f6f37f6310e3e9cf3469d05ac77961b0ca4](https://github.com/brilliantlabsAR/frame-codebase/tree/5e356f6f37f6310e3e9cf3469d05ac77961b0ca4)

Each benchmark run evaluates **256 inferences**, repeated to ensure stable results.

---

## 📊 Float Model Benchmark (FP32)

### Raw Measurements

| Run | Total Time (µs) | Average per Inference (µs) |
|-----|-----------------|-----------------------------|
| 1 | 28,122.53 | 109.85 |
| 2 | 28,128.28 | 109.88 |
| 3 | 28,130.33 | 109.88 |
| 4 | 28,075.73 | 109.67 |
| 5 | 28,118.48 | 109.84 |

### Summary Statistics

| Metric | Value |
|--------|--------|
| Number of runs | 5 |
| Inferences per run | 256 |
| **Mean latency** | **109.82 µs** |
| **Median latency** | **109.84 µs** |
| **Min latency** | **109.67 µs** |
| **Max latency** | **109.88 µs** |
| Variance | 0.0084 µs |

### Notes

- This benchmark uses the **floating-point version** of the Hello World model.
- The model runs without quantization and without CMSIS-NN.
- Latency is extremely stable because float operations use the Cortex-M4F hardware FPU.

---

## 📉 Quantized INT8 Model Benchmark (Reference Kernels Only)

The quantized INT8 version of the model performs **slower** than the float version --> **~ 30 %** slower than *Float model*.

### Raw Measurements

| Run | Total Time (µs) | Average per Inference (µs) |
|-----|-----------------|-----------------------------|
| 1 | 36,234.75 | 141.54 |
| 2 | 36,242.02 | 141.57 |
| 3 | 36,241.38 | 141.57 |
| 4 | 36,243.23 | 141.58 |

### Summary Statistics

| Metric | Value |
|--------|--------|
| Number of runs | 4 |
| Inferences per run | 256 |
| **Mean latency** | **141.56 µs** |
| **Median latency** | **141.57 µs** |
| **Min latency** | **141.54 µs** |
| **Max latency** | **141.58 µs** |

---

## ⚠️ Why the INT8 Model Is Slower

Even though quantization usually improves performance, the INT8 model is slower here because:

### 1. **No CMSIS-NN kernels are used**
- The current setup uses **TensorFlow Lite Micro reference kernels**, which are simple, unoptimized C implementations.
- These kernels do **not** use DSP/SIMD instructions of the Cortex-M4.

### 2. **The float model benefits from hardware acceleration**
- The nRF52840 contains a **hardware Floating-Point Unit (FPU)**.
- FP32 operations are executed directly in hardware and are therefore very fast.

### 3. **INT8 operations run entirely in software**
- All INT8 arithmetic is handled in software without vectorization.
- This results in higher cycle counts per operation.

### 4. **Hello World is dominated by a fully-connected kernel**
- In this specific model, the fully-connected layer is the main cost.
- The reference INT8 fully-connected implementation is slow compared to the float FPU version.

---

## 🚀 Quantized INT8 Model Benchmark with CMSIS-NN Optimization

The quantized INT8 version with **CMSIS-NN optimized kernels** shows significant performance improvement, achieving **~11% better performance** than the float model and **~44% faster** than INT8 reference kernels.

### Raw Measurements

| Run | Total Time (µs) | Average per Inference (µs) |
|-----|-----------------|-----------------------------|
| 1 | 25,353.58 | 99.04 |
| 2 | 24,966.41 | 97.53 |
| 3 | 25,341.00 | 98.99 |
| 4 | 24,727.72 | 96.59 |

### Summary Statistics

| Metric | Value |
|--------|--------|
| Number of runs | 4 |
| Inferences per run | 256 |
| **Mean latency** | **98.04 µs** |
| **Median latency** | **98.26 µs** |
| **Min latency** | **96.59 µs** |
| **Max latency** | **99.04 µs** |
| Variance | 1.26 µs |

### Performance Gains

| Comparison | Speedup |
|------------|---------|
| **vs Float model** | **1.12x faster** (11% improvement) |
| **vs INT8 Reference** | **1.44x faster** (44% improvement) |

### Why CMSIS-NN Is Faster

1. **DSP/SIMD Instructions**: Uses ARM Cortex-M4 DSP extensions (SMLAD, SMLABB, etc.)
2. **Loop Unrolling**: Processes 4 elements at a time with optimized accumulation
3. **Optimized Memory Access**: Better cache/memory utilization patterns
4. **Reduced Overhead**: Eliminates unnecessary operations compared to reference kernels

---

## 📝 Conclusion

### Performance Summary

| Model Configuration | Mean Latency | Performance vs Float |
|---------------------|--------------|---------------------|
| **FP32 (Float)** | **109.82 µs** | Baseline |
| **INT8 Reference Kernels** | **141.56 µs** | **29% slower** ❌ |
| **INT8 + CMSIS-NN** | **98.04 µs** | **11% faster** ✅ |

### Key Findings

1. **CMSIS-NN delivers significant performance gains**: The INT8 model with CMSIS-NN optimization is **11% faster** than the float model and **44% faster** than INT8 reference kernels.

2. **Reference kernels are slower than float**: Without optimization, the INT8 model is 29% slower than float due to:
   - Use of **unoptimized reference kernels** (no DSP/SIMD)
   - The float model's **hardware FPU acceleration** on the Cortex-M4F

3. **CMSIS-NN makes INT8 the best choice**: With proper optimization, quantized models achieve the best performance on Cortex-M4 processors.
