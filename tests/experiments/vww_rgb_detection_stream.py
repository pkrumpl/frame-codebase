"""
VWW_RGB Experiment - Person Detection Stream (RGB)

Displays live video feed with person detection classification using RGB input.
Runs autoexposure once at startup, then loops detection until Ctrl+C.
Requires: ML_EXPERIMENT=VWW_RGB build flashed to Frame.

Key differences from VWW (grayscale):
- Input size: 27,648 bytes (96x96x3) vs 9,216 bytes (96x96x1)
- Displays color image instead of grayscale
"""

import asyncio
import signal
import numpy as np
import matplotlib.pyplot as plt
from frameutils import Bluetooth

# Image dimensions (must match OUTPUT_SIZE in experiment_vww_rgb.c)
IMAGE_WIDTH = 96
IMAGE_HEIGHT = 96
NUM_CHANNELS = 3  # RGB
EXPECTED_IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * NUM_CHANNELS  # 27648

# Person detect output
EXPECTED_PRED_BYTES = 2  # [not_person, person]

# Global state
running = True
received_image = bytearray()
received_predictions = bytearray()
transfer_complete = False
receiving_predictions = False


def signal_handler(sig, frame):
    global running
    print("\nStopping stream...")
    running = False


def reset_buffers():
    global received_image, received_predictions, transfer_complete, receiving_predictions
    received_image = bytearray()
    received_predictions = bytearray()
    transfer_complete = False
    receiving_predictions = False


def data_handler(data: bytes):
    """Handle incoming binary data from Bluetooth."""
    global received_image, received_predictions, transfer_complete, receiving_predictions

    # End marker check (0xFF 0xFF 0x00 0x00) - after flag stripped
    if len(data) == 4 and data == b'\xff\xff\x00\x00':
        transfer_complete = True
        return

    # Separator check (0xFE 0xFE) - after flag stripped
    if len(data) == 2 and data == b'\xfe\xfe':
        receiving_predictions = True
        return

    # Append data based on current state
    if not receiving_predictions:
        received_image.extend(data)
    else:
        received_predictions.extend(data)


def print_handler(s: str):
    """Handle print/text responses"""
    print(f"[Frame]: {s}")


# ELSS logo is embedded as private-use codepoint U+F0011 in system_font.h
# (added via `frameutils create_sprites`). Its UTF-8 byte sequence is
# F3 B0 80 91. We send the bytes as Lua \xHH escapes so the Lua snippet
# stays ASCII over BLE.
ELSS_LOGO_LUA_LITERAL = r"'\xF3\xB0\x80\x91'"

# Autoexposure tuning for `frame.camera.auto` (see camera.c:556-670).
# Interpolated into the Lua call in `run_calibration`.
#
#   key                range     default  indoor     outdoor
#   exposure           0.0-1.0   0.1      0.20-0.30  0.10-0.15
#   analog_gain_limit  1-248     16       32-64      16-24
#
# Higher exposure -> brighter target, risk of blown highlights.
# Higher analog_gain_limit -> brighter dim scenes, more sensor noise.
AUTOEXP_EXPOSURE = 0.15
AUTOEXP_ANALOG_GAIN_LIMIT = 16


async def run_calibration(b: Bluetooth) -> None:
    """Draw the ELSS logo + 'Calibrating...' on the Frame display, then run
    5 autoexposure cycles from Python.

    Mirrors what a future on-device tap handler will do — same display
    primitives, same autoexposure cadence — just driven over BLE instead
    of from an IMU callback.
    """
    DISPLAY_W = 640

    # Logo glyph (U+F0011) is 437x144 in the system font. Center horizontally;
    # the +1 is because frame.display uses 1-based coordinates.
    logo_x = 1 + (DISPLAY_W - 437) // 2  # = 102
    logo_y = 130                          # leaves room for caption below

    caption_x = 240                       # tuned to roughly center the caption
    caption_y = 290

    lua = (
        f"frame.display.text({ELSS_LOGO_LUA_LITERAL}, "
        f"{logo_x}, {logo_y}, {{color='WHITE'}});"
        f"frame.display.text('Calibrating...', "
        f"{caption_x}, {caption_y}, {{color='WHITE'}});"
        f"frame.display.show()"
    )
    await b.send_lua(lua)

    await b.send_lua("frame.camera.power_save(false)")
    await asyncio.sleep(0.2)

    print(
        f"Running autoexposure (5 iterations, "
        f"exposure={AUTOEXP_EXPOSURE}, "
        f"analog_gain_limit={AUTOEXP_ANALOG_GAIN_LIMIT})..."
    )
    auto_args = (
        f"{{exposure={AUTOEXP_EXPOSURE}, "
        f"analog_gain_limit={AUTOEXP_ANALOG_GAIN_LIMIT}}}"
    )
    for i in range(5):
        await b.send_lua(f"frame.camera.auto({auto_args})")
        await asyncio.sleep(1)
        print(f"  Autoexposure {i + 1}/5")


async def main():
    global running

    signal.signal(signal.SIGINT, signal_handler)

    b = Bluetooth()

    print("Connecting to Frame...")
    await b.connect(
        print_response_handler=print_handler,
        data_response_handler=data_handler
    )

    print("Calibrating camera...")
    await run_calibration(b)
    print("Camera ready.")

    print("Starting RGB person detection stream (Ctrl+C to stop)...")
    print(f"Expecting {EXPECTED_IMAGE_BYTES} bytes RGB image data per frame")

    # Setup matplotlib with optimized settings
    plt.ion()
    fig, ax = plt.subplots(1, 1, figsize=(6, 6))
    fig.suptitle("Person Detection Stream (RGB) - Press Ctrl+C to stop")

    # Create initial RGB image and title objects (reuse these for performance)
    initial_img = np.zeros((IMAGE_HEIGHT, IMAGE_WIDTH, NUM_CHANNELS), dtype=np.uint8)
    im = ax.imshow(initial_img)  # No cmap for RGB
    title = ax.set_title('Waiting for first frame...', fontsize=14)
    ax.axis('off')

    # Tight layout once
    fig.tight_layout()
    fig.canvas.draw()
    plt.show(block=False)

    frame_count = 0

    while running:
        reset_buffers()

        # Call person detection (camera already woken + auto-exposed in run_calibration)
        await b.send_lua("frame.experiment.run_person_detection_fast()")

        # Wait for transfer to complete (longer timeout for larger data).
        # Pump matplotlib events each tick so the figure stays responsive
        # during the BLE transfer (~3 s/frame). Without this, the window
        # freezes between frames and clicks/closes feel laggy.
        timeout = 45  # seconds (increased for RGB - 3x more data)
        elapsed = 0
        while not transfer_complete and elapsed < timeout and running:
            await asyncio.sleep(0.05)
            elapsed += 0.05
            try:
                fig.canvas.flush_events()
            except Exception:
                pass

        if not running:
            break

        if not transfer_complete:
            print(f"Frame {frame_count}: timeout waiting for data "
                  f"(received {len(received_image)} of {EXPECTED_IMAGE_BYTES} bytes)")
            continue

        # Process frame
        if len(received_image) >= EXPECTED_IMAGE_BYTES and len(received_predictions) >= EXPECTED_PRED_BYTES:
            frame_count += 1

            # Parse RGB image
            img_data = np.frombuffer(bytes(received_image[:EXPECTED_IMAGE_BYTES]), dtype=np.uint8)
            img = img_data.reshape((IMAGE_HEIGHT, IMAGE_WIDTH, NUM_CHANNELS))

            # Parse predictions
            scores = np.frombuffer(bytes(received_predictions[:EXPECTED_PRED_BYTES]), dtype=np.int8)

            not_person_score = int(scores[0])
            person_score = int(scores[1])
            is_person = person_score > not_person_score

            # Update display efficiently (no ax.clear()!)
            im.set_data(img)

            result_text = "PERSON DETECTED" if is_person else "NO PERSON"
            color = 'green' if is_person else 'red'
            title.set_text(f'Frame {frame_count}: {result_text}\n'
                          f'Scores: person={person_score}, not_person={not_person_score}')
            title.set_color(color)

            # Fast redraw
            fig.canvas.draw_idle()
            fig.canvas.flush_events()

            # Print result
            print(f"Frame {frame_count}: {'PERSON' if is_person else 'NO PERSON'} "
                  f"(person={scores[1]}, not_person={scores[0]})")
        else:
            print(f"Frame incomplete: image={len(received_image)}/{EXPECTED_IMAGE_BYTES} bytes, "
                  f"pred={len(received_predictions)}/{EXPECTED_PRED_BYTES} bytes")

    # Cleanup
    print("\nCleaning up...")
    await b.send_lua("frame.camera.power_save(true)")
    # Restore the standard 'Frame is Paired' screen. The firmware draws this
    # once at boot via show_pairing_screen() but never re-draws it on its own,
    # so without this step the last detection text stays on the glasses after
    # the script exits. The Lua snippet here mirrors luaport.c:show_pairing_screen.
    await b.send_lua(
        "frame.display.text('Frame is Paired', 185, 140);"
        "frame.display.text("
        "'Frame '..frame.bluetooth.address():sub(-2, -1), "
        "245, 210, { color = 'ORANGE' });"
        "frame.display.show()"
    )
    await asyncio.sleep(0.1)
    await b.disconnect()

    plt.ioff()
    plt.close(fig)

    print(f"Stream ended. Total frames: {frame_count}")


if __name__ == "__main__":
    asyncio.run(main())
