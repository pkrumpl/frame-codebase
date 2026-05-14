# ✨ Brilliant Labs Frame Firmware × TensorFlow Lite for Microcontrollers

![Header image (Brilliant Labs X TensorFlow Lite for Microcontrollers)](/docs/img/BrilliantLabs_TFLite.png)

> 🕶️ + 🧠 = ultra-light ML on your face. This fork layers [TensorFlow Lite for Microcontrollers (TFLM)](https://github.com/tensorflow/tflite-micro) directly into the open-source [Frame smart glasses](https://docs.brilliant.xyz/frame/frame/).

By embedding TFLM into the firmware itself, the glasses can **run lightweight machine learning models locally**, without needing a connected host device. This approach improves **reliability**, enables **real-time inference**, and reduces **latency** for ML-driven applications such as gesture recognition, sensor fusion, or low-power computer vision.

This project is **experimental** and intended for developers exploring how TFLM can be deployed directly on the Frame hardware. If you're looking for the standard firmware or general documentation, please visit the official Brilliant Labs [documentation](https://docs.brilliant.xyz).

## 🎓 Academic Context

This work is part of a Master's Thesis at [Graz University of Technology (TU Graz)](https://www.tugraz.at), conducted within the [Embedded Learning and Sensing Systems (ELSS)](https://www.tugraz.at/arbeitsgruppen/iti-teams/elss/team-contact) group at the Institute of Technical Informatics.

The thesis is supervised by Assoc.Prof. Olga Saukh, who leads the ELSS group, with co-supervision from Dong Wang and Francesco Corti. The group studies how to run advanced AI models on devices where every kilobyte and milliwatt matters, focusing on making deep learning work on mobile and embedded devices like smartphones or wearables. Data privacy and environmental sustainability are important aspects of their research.

<div style="background-color: white; display: inline-flex; align-items: center; gap: 20px; padding: 10px; border-radius: 4px;">
  <a href="https://www.tugraz.at"><img src="docs/img/TU_Graz.png" alt="TU Graz" height="60"></a>
  <a href="https://www.tugraz.at/arbeitsgruppen/iti-teams/elss/team-contact"><img src="docs/img/ELSS_logo.png" alt="ELSS Group" height="60"></a>
</div>

## 🔀 Key Differences from Original Firmware

This fork modifies the original Frame firmware to enable on-device ML inference. The goal was to minimize changes to the original codebase while making necessary trade-offs to fit ML models within the hardware constraints (256 KB RAM, 824 KB available flash for application).

### Memory Trade-offs

| Change | Reason |
|--------|--------|
| Disabled microphone | `FRAME_DISABLE_MICROPHONE` compile flag to free up RAM |
| Reduced file system | Smaller LittleFS allocation and reduced storage space for file system |
| Static tensor arenas | Required for TFLM inference |

### Build System Adaptations

- **C++ support**: Added compilation rules for TFLM's C++ codebase
- **CMSIS-NN integration**: ARM Cortex-M4 optimized kernels for neural network operations
- **Embedded ML flags**: `-fno-rtti`, `-fno-exceptions`, `-DTF_LITE_STATIC_MEMORY`
- **Experiment selection**: `config.mk` for build-time model selection

### Boot Safety Mechanism

A safety mechanism prevents the glasses from becoming unusable after a faulty firmware update:
- Detects watchdog resets and consecutive boot failures
- Automatically enters DFU mode after 5 failed boot attempts
- Allows recovery via Bluetooth OTA update

### New Components

- `tflm_wrapper.cc/h`: C-compatible wrapper for TFLM C++ code
- `lua_libraries/experiment_*.c`: Lua APIs for ML inference
- `scripts/`: Memory analysis tools

## 🔬 ML Experiments

This fork serves as a testbed to explore what's possible with on-device machine learning on the Frame glasses. The goal is to evaluate different ML use cases and measure their performance on the constrained hardware (Cortex-M4F with 256 KB RAM).

### Selecting an Experiment

The active ML experiment is configured in `source/application/config.mk`:

Simply change the `ML_EXPERIMENT` value and rebuild the firmware to switch between use cases:

| Experiment       | Description                                      |
|------------------|--------------------------------------------------|
| `VWW`            | Visual Wake Words - person detection (grayscale) |
| `VWW_RGB`        | Visual Wake Words - person detection (RGB input) |
| `FOMO_BEER_CAN`  | Object detection using FOMO architecture         |

### Benchmarks

Performance benchmarks and results for each experiment are documented in [`docs/benchmarks/`](docs/benchmarks/) and will be expanded.

## 🧱 System architecture

The codebase is split into three sections: the **nRF52 Application**, the **nRF52 Bootloader**, and the **FPGA RTL**.

The nRF52 is designed to handle the overall system operation. It runs Lua, manages Bluetooth networking, handles AI tasks, and looks after power management. The FPGA accelerates graphics and camera pipelines.

![Frame system architecture diagram](docs/diagrams/frame-system-architecture.drawio.png)

## 🚀 Getting started with nRF52 firmware development

1. Ensure you have the [ARM GCC Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads) installed.

    *This fork has only been tested with version **12.3** of the toolchain (AArch32 bare-metal, `arm-none-eabi`).*

2. Install the [nRF Command Line Tools](https://www.nordicsemi.com/Products/Development-tools/nRF-Command-Line-Tools).

3. Install [nRF Util](https://www.nordicsemi.com/Products/Development-tools/nRF-Util) along with the `device` and `nrf5sdk-tools` subcommands:

    ```sh
    ./nrfutil install device
    ./nrfutil install nrf5sdk-tools
    ```

4. Clone this repository and initialize the submodules:

    ```sh
    git clone https://github.com/pkrumpl/frame-codebase.git
    cd frame-codebase
    git submodule update --init
    ```

5. Build and flash the project to an [nRF52840 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF52840-DK):

    ```sh
    make release
    make erase-jlink   # Unlocks the flash protection if needed
    make flash-jlink
    ```

### Deployment Options

> For a step-by-step walkthrough of the OTA update process and running the on-device ML showcase, see [`docs/working-with-the-glasses.md`](docs/working-with-the-glasses.md).

**Over-the-Air (OTA) Update via Bluetooth:**
- Build a DFU package with `make release`
- Use the Frame app or nRF Connect to upload the firmware
- The glasses must be in DFU mode (hold button during power-on, or use `frame.update()` in Lua)

**J-Link (Development):**
- Direct flashing for development (see commands above)
- Supports debugging with RTT logging

**Boot Safety:**
- If the firmware crashes repeatedly, the glasses automatically enter DFU mode
- This prevents bricking and allows recovery via Bluetooth

## 🛠️ Debugging

> Note: The `Application (J-Link)` launch configuration requires the SEGGER **J-Link Software and Documentation Pack** so that `JLinkGDBServerCL` is available to Cortex-Debug. Install it from [segger.com/downloads/jlink](https://www.segger.com/downloads/jlink) and ensure the binaries are on your `PATH` (or set `serverpath` in `.vscode/launch.json`).

1. Open the project in [VS Code](https://code.visualstudio.com) and run the build tasks listed in `.vscode/tasks.json` (`Ctrl+Shift+P` → "Tasks: Run Task"). The `Build` task should complete without issues.

2. If flashing fails, try the `Erase` task to unlock the device before reprogramming.

3. For IntelliSense, select the `arm-none-eabi-gcc` configuration (`Ctrl+Shift+P` → `C/C++: Select IntelliSense Configuration`).

4. Install the [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) extension, then launch the configured `Application (J-Link)` target (`F5`) to build, flash, and start a debug session.

5. Use the `RTT Debug Console (J-Link)` task to watch logs while the application is running.

6. To debug with a Black Magic Probe, follow `/production/blackmagic/README.md`.

## 🧮 Getting started with FPGA development

> **Note:** The FPGA RTL was not modified for this thesis. The information below is from the original firmware documentation.

The complete FPGA architecture is described in `docs/fpga-architecture.md`.

The FPGA RTL is prebuilt and included in `fpga_application.h` for convenience. If you wish to modify the FPGA RTL, follow `docs/fpga-toolchain-setup.md` to rebuild the bitstream.

## 🙏 Acknowledgments

This project is based on the official [Frame firmware by Brilliant Labs](https://github.com/brilliantlabsAR/frame-codebase).
