# Working with the Frame Glasses

A handover guide for deploying firmware to the modified Frame glasses and running the on-device ML showcase. For toolchain and `nrfutil` setup, see the [main README](../README.md#-getting-started-with-nrf52-firmware-development) — this document assumes that environment is already in place and focuses on the day-to-day workflow.

## Deploying firmware (OTA)

Because the glasses cannot be opened without risking damage, there is no wired debug access — all firmware updates go over Bluetooth using the nRF52840's built-in DFU (Device Firmware Update) bootloader. The full update is four steps.

### 1. Build the firmware

From the repository root:

```make release ```

This cleans the build and produces, in `build/`:

- `frame-firmware-<version>.zip` — the **signed DFU package**. This is the file uploaded to the glasses. - `frame-firmware-<version>.hex` — a merged image (only used for wired/J-Link flashing of a dev kit; not needed for OTA).

The active ML experiment is selected at build time in [`source/application/config.mk`](../source/application/config.mk) via the `ML_EXPERIMENT` variable — make sure it is set as intended before building.

### 2. Put the glasses into DFU mode

The glasses enter DFU mode when the application calls `frame.update()`. The easiest way to trigger this is the [`test_dfu.py`](../tests/test_dfu.py) testcase, which connects over Bluetooth, prints the current firmware/hardware version, and sends the update command:

```python tests/test_dfu.py ```

After this runs, the glasses reboot and advertise as **`Frame Update`**. (The testcase uses the `frameutils` Python package — `pip install frameutils` — which all the test scripts depend on.)

### 3. Upload via nRF Connect for Mobile

On the phone, open [nRF Connect for Mobile](https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-mobile):

1. Transfer / import the `frame-firmware-<version>.zip` package to the phone. 2. Scan and connect to the **`Frame Update`** device. 3. Open the **DFU** option, select the `.zip` package, and start the update.

The glasses reboot automatically into the new firmware once the transfer completes. If no DFU activity happens for ~5 minutes, the bootloader times out and returns to the existing application firmware.

### 4. Safety net — boot-safety auto-DFU

The firmware includes a boot-safety mechanism ([`boot_safety.c`](../source/application/boot_safety.c)): a watchdog reset, or `MAX_BOOT_ATTEMPTS` (currently 8) consecutive failed boots, automatically drops the glasses back into DFU mode. This means a faulty build cannot permanently brick the glasses — you can always recover by uploading a known-good package.

> Note: `boot_safety_mark_boot_successful()` in `luaport.c` is currently > commented out (a TODO), so the boot counter is not cleared on a *successful* > boot. A normal power cycle still clears the retained register, but be aware of > this if you see unexpected DFU-mode entry after many soft resets.

## Interacting with the original Frame firmware

This repository is a fork with custom ML firmware. For examples of how to talk to the **stock** Frame firmware (camera, display, IMU, etc.) over Bluetooth from Python, see:

<https://github.com/CitizenOneX/frame_examples_python>

Note that the microphone functionality has been disabled in the custom firmware.

## Firmware modifications deployed

This fork layers TensorFlow Lite for Microcontrollers (TFLM) into the Frame firmware so models run directly on the glasses. The full list of changes versus the original firmware — disabled microphone, reduced filesystem, static tensor arenas, C++/CMSIS-NN build support, boot safety — is in the [README "Key Differences" section](../README.md#-key-differences-from-original-firmware).

### VWW_RGB showcase (most recent change)

The currently deployed experiment is **VWW_RGB**: on-device person detection on a 96×96 RGB camera frame, with a `PERSON` / `NO PERSON` overlay drawn on the display. The relevant code is [`experiment_vww_rgb.c`](../source/application/lua_libraries/experiment_vww_rgb.c). There are two ways to run it:

**1. Double-tap the glasses (fully on-device, no host needed).** A double-tap handler is installed at boot in [`luaport.c`](../source/application/luaport.c) — two taps within 500 ms call `frame.experiment.run_vww_demo()`. This shows the ELSS logo + "Calibrating…" screen, wakes the camera and runs autoexposure, then runs 30 detection iterations drawing the result on the display each time, and finally restores the "Frame is Paired" screen. Nothing is streamed over Bluetooth.

**2. Run the streaming testcase (host-driven, live view).** [`vww_rgb_detection_stream.py`](../tests/experiments/vww_rgb_detection_stream.py) connects over Bluetooth, calibrates the camera, then loops detection — streaming each RGB frame and its prediction scores back to the host and displaying them in a live matplotlib window until `Ctrl+C`:

```python tests/experiments/vww_rgb_detection_stream.py ```

Both paths share the same underlying detection routine in `experiment_vww_rgb.c`; the double-tap demo simply skips the Bluetooth transfer.