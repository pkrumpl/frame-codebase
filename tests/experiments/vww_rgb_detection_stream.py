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


async def main():
    global running

    signal.signal(signal.SIGINT, signal_handler)

    b = Bluetooth()

    print("Connecting to Frame...")
    await b.connect(
        print_response_handler=print_handler,
        data_response_handler=data_handler
    )

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

        # Call person detection
        await b.send_lua("frame.experiment.run_person_detection()")

        # Wait for transfer to complete (longer timeout for larger data)
        timeout = 45  # seconds (increased for RGB - 3x more data)
        elapsed = 0
        while not transfer_complete and elapsed < timeout and running:
            await asyncio.sleep(0.05)
            elapsed += 0.05

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
    await asyncio.sleep(0.1)
    await b.disconnect()

    plt.ioff()
    plt.close(fig)

    print(f"Stream ended. Total frames: {frame_count}")


if __name__ == "__main__":
    asyncio.run(main())
