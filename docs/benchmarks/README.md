# Experiment Benchmarks

All experiments have been conducted on [Frame Glasses](https://docs.brilliant.xyz/frame/frame/) by [BrilliantLabs](https://brilliant.xyz/).

## Hardware specification

- *Device name*: Brilliant Labs Frame
- *MCU*: Nordic nRF52840, Cortex-M4F @ 64 MHz
- *RAM*: 256 KB
- *Flash*: 1 MB

## Software

The experiments have been conducted using the [modified firmware](https://github.com/pkrumpl/frame-codebase). The software has been modified to port TensorFlow Lite Micro onto the MCU. The respective commit used for the experiment is noted in the table with the results.

## Benchmark Description

### 📊 Inference Latency
Inference latency measures how long the device needs to execute a single forward pass of a machine-learning model. On constrained hardware like the nRF52840, latency directly affects real-time performance and determines whether tasks such as object classification or sensor processing can run smoothly.  

Latency is typically reported as:
- **Single-run latency (ms)**
- **Average latency over many runs (e.g., 1,000 inferences)**
- **Best / worst-case latency**
- **Latency jitter (variance)**  

This metric is essential for understanding how efficiently the model executes on-device and how suitable it is for time-sensitive applications.


### 📦 Memory Footprint
Memory footprint describes the amount of **RAM** and **flash** consumed by the whole firmware.
On the nRF52840, memory is limited (256 KB RAM, 1 MB flash), so understanding usage is crucial for deciding which models can run locally.

Memory usage typically includes:
- **Flash usage**: model binary, operator kernels, and TFLM runtime, frame firmware
- **RAM usage**:
  - Tensor arena (temporary tensors and activations)
  - Persistent buffers
  - Peak RAM required during inference  

This metric helps ensure the firmware remains stable and prevents crashes caused by memory exhaustion.

---

### 📁 Model Size
Model size refers to the storage space occupied by the `.tflite` file embedded into the firmware.  
This value strongly depends on:
- Model architecture  
- Whether the model is quantized (e.g., INT8) or uses floating-point weights  
- Pruning or compression techniques  

Smaller models not only reduce flash usage but also typically improve inference speed and lower energy consumption. Tracking model size helps evaluate how well a model fits within the hardware limits and how different optimization techniques affect storage requirements.


