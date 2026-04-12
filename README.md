# ✨ Brilliant Labs Frame Firmware × TensorFlow Lite for Microcontrollers

![Header image (Brilliant Labs X TensorFlow Lite for Microcontrollers)](/docs/img/BrilliantLabs_TFLite.png)

> 🕶️ + 🧠 = ultra-light ML on your face. This fork layers [TensorFlow Lite for Microcontrollers (TFLM)](https://github.com/tensorflow/tflite-micro) directly into the open-source [Frame smart glasses](https://docs.brilliant.xyz/frame/frame/).

By embedding TFLM into the firmware itself, the glasses can **run lightweight machine learning models locally**, without needing a connected host device. This approach improves **reliability**, enables **real-time inference**, and reduces **latency** for ML-driven applications such as gesture recognition, sensor fusion, or low-power computer vision.

This project is **experimental** and intended for developers exploring how TFLM can be deployed directly on the Frame hardware. If you're looking for the standard firmware or general documentation, please visit the official Brilliant Labs [documentation](https://docs.brilliant.xyz).

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
    git clone https://github.com/brilliantlabsAR/frame-codebase.git
    cd frame-codebase
    git submodule update --init
    ```

5. Build and flash the project to an [nRF52840 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF52840-DK):

    ```sh
    make release
    make erase-jlink   # Unlocks the flash protection if needed
    make flash-jlink
    ```

## 🛠️ Debugging

> Note: The `Application (J-Link)` launch configuration requires the SEGGER **J-Link Software and Documentation Pack** so that `JLinkGDBServerCL` is available to Cortex-Debug. Install it from [segger.com/downloads/jlink](https://www.segger.com/downloads/jlink) and ensure the binaries are on your `PATH` (or set `serverpath` in `.vscode/launch.json`).

1. Open the project in [VS Code](https://code.visualstudio.com) and run the build tasks listed in `.vscode/tasks.json` (`Ctrl+Shift+P` → "Tasks: Run Task"). The `Build` task should complete without issues.

2. If flashing fails, try the `Erase` task to unlock the device before reprogramming.

3. For IntelliSense, select the `arm-none-eabi-gcc` configuration (`Ctrl+Shift+P` → `C/C++: Select IntelliSense Configuration`).

4. Install the [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) extension, then launch the configured `Application (J-Link)` target (`F5`) to build, flash, and start a debug session.

5. Use the `RTT Debug Console (J-Link)` task to watch logs while the application is running.

6. To debug with a Black Magic Probe, follow `/production/blackmagic/README.md`.

## 🧮 Getting started with FPGA development

The complete FPGA architecture is described in `docs/fpga-architecture.md`.

The FPGA RTL is prebuilt and included in `fpga_application.h` for convenience. If you wish to modify the FPGA RTL, follow `docs/fpga-toolchain-setup.md` to rebuild the bitstream.
