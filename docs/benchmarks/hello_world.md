# 🧪 Benchmark Results – Hello World (Float vs. Quantized INT8)

This document compares the benchmark results of the **Hello World** TensorFlow Lite Micro example on the **Brilliant Labs Frame** (nRF52840, Cortex-M4F @ 64 MHz).

- **Float model:** benchmarked on **18 November 2025** on [d1444f82470fdaadc570491b27e9fd8f7f6ccfb6](https://github.com/pkrumpl/frame-codebase/tree/d1444f82470fdaadc570491b27e9fd8f7f6ccfb6)
- **Quantized INT8 model:** benchmarked on **19 November 2025** on [60861f57b4b0c9d03c3b34d74ad57ac38339afb8](https://github.com/pkrumpl/frame-codebase/tree/60861f57b4b0c9d03c3b34d74ad57ac38339afb8)

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

The quantized INT8 version of the model performs **slower** than the float version.

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

## 📝 Conclusion

- The FP32 model achieves ~**109.8 µs** per inference.
- The INT8 model achieves ~**141.6 µs** per inference.
- The INT8 slowdown is fully explained by:
  - use of **unoptimized reference kernels**, and  
  - the float model’s **hardware FPU acceleration** on the Cortex-M4F.

---

## 🚀 Next Steps

### ✔ 1. Integrate CMSIS-NN into the TensorFlow Lite Micro build
CMSIS-NN provides optimized kernels for Cortex-M processors.  
Integrating it **might significantly improve** the performance of INT8 models, since it uses:

- DSP instructions (SIMD)
- optimized fully-connected and convolution kernels
- hand-tuned assembly paths