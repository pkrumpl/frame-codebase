## 🧪 Benchmark Results – Hello World (Float Model)

The **Hello World** TensorFlow Lite Micro example (floating-point version) was benchmarked on **18 November 2025** using the **Brilliant Labs Frame** (nRF52840, Cortex-M4F @ 64 MHz). The firmware commit used for this experiment is [d1444f82470fdaadc570491b27e9fd8f7f6ccfb6](https://github.com/pkrumpl/frame-codebase/tree/d1444f82470fdaadc570491b27e9fd8f7f6ccfb6).


Each benchmark run evaluates **256 inferences**, repeated multiple times to check stability and variance.

---

### 📊 Raw Measurements

| Run | Total Time (µs) | Average per Inference (µs) |
|-----|-----------------|-----------------------------|
| 1 | 28,122.53 | 109.85 |
| 2 | 28,128.28 | 109.88 |
| 3 | 28,130.33 | 109.88 |
| 4 | 28,075.73 | 109.67 |
| 5 | 28,118.48 | 109.84 |

---

### 📈 Summary Statistics

| Metric | Value |
|--------|--------|
| **Number of runs** | 5 |
| **Inferences per run** | 256 |
| **Mean latency** | **109.82 µs** |
| **Median latency** | **109.84 µs** |
| **Min latency** | **109.67 µs** |
| **Max latency** | **109.88 µs** |
| **Variance** | **0.0084 µs** |

---

### 📝 Notes

- This benchmark uses the **floating-point version** of the Hello World model. 
- The model was executed on the MCU **without quantization** and **without CMSIS-NN acceleration**.  
- Results show **high repeatability** and **extremely low jitter**, indicating that the measurement method is stable.  
- Quantized INT8 models are expected to be significantly faster due to optimized kernels.
