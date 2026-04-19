"""
FOMO Hand Detection - Capture-Only Image Stream (Diagnostic)

Streams the 96x96 RGB image from the Frame's camera pipeline (capture +
JPEG decode + upscale) WITHOUT running inference or sending predictions.
Use this to verify the image-acquisition pipeline in isolation - if these
images look correct, any later visual artifact is downstream of capture.

Requires: ML_EXPERIMENT=FOMO_HAND_DETECTION build flashed to Frame.
"""

import asyncio
import signal
import numpy as np
import matplotlib.pyplot as plt
from frameutils import Bluetooth

IMAGE_WIDTH = 96
IMAGE_HEIGHT = 96
NUM_CHANNELS = 3
EXPECTED_IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * NUM_CHANNELS  # 27648

running = True
received_image = bytearray()
transfer_complete = False


def signal_handler(sig, frame):
    global running
    print("\nStopping stream...")
    running = False


def reset_buffers():
    global received_image, transfer_complete
    received_image = bytearray()
    transfer_complete = False


def data_handler(data: bytes):
    """Image bytes arrive directly; only the end marker is structural."""
    global received_image, transfer_complete

    if len(data) == 4 and data == b'\xff\xff\x00\x00':
        transfer_complete = True
        return

    received_image.extend(data)


def print_handler(s: str):
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

    print("Starting capture-only image stream (Ctrl+C to stop)...")
    print(f"Expecting {EXPECTED_IMAGE_BYTES} bytes per frame")

    plt.ion()
    fig, ax = plt.subplots(1, 1, figsize=(6, 6))
    fig.suptitle("Capture-only stream (no inference)")

    initial_img = np.zeros((IMAGE_HEIGHT, IMAGE_WIDTH, NUM_CHANNELS), dtype=np.uint8)
    im = ax.imshow(initial_img, interpolation='nearest')
    title = ax.set_title('Waiting for first frame...', fontsize=11)
    ax.axis('off')
    fig.tight_layout()
    fig.canvas.draw()
    plt.show(block=False)

    frame_count = 0

    while running:
        reset_buffers()

        # Full pipeline (with auto-exposure each frame). Pass `true` to
        # send_image() to skip auto-exposure once the camera is warmed
        # up - useful to test whether the fast path corrupts data.
        await b.send_lua("frame.experiment.send_image()")

        timeout = 30
        elapsed = 0
        while not transfer_complete and elapsed < timeout and running:
            await asyncio.sleep(0.05)
            elapsed += 0.05

        if not running:
            break

        if not transfer_complete:
            print(f"Frame {frame_count}: timeout "
                  f"(received {len(received_image)} of {EXPECTED_IMAGE_BYTES} bytes)")
            continue

        n = len(received_image)
        drift = n - EXPECTED_IMAGE_BYTES
        if drift != 0:
            print(f"  [warn] received {n} bytes, expected {EXPECTED_IMAGE_BYTES} "
                  f"(drift {drift:+d})")

        if n >= EXPECTED_IMAGE_BYTES:
            frame_count += 1
            img_data = np.frombuffer(bytes(received_image[:EXPECTED_IMAGE_BYTES]), dtype=np.uint8)
            img = img_data.reshape((IMAGE_HEIGHT, IMAGE_WIDTH, NUM_CHANNELS))

            im.set_data(img)
            title.set_text(f'Frame {frame_count} ({IMAGE_WIDTH}x{IMAGE_HEIGHT} RGB)')
            fig.canvas.draw_idle()
            fig.canvas.flush_events()
            print(f"Frame {frame_count}: ok ({n} bytes)")
        else:
            print(f"Frame incomplete: {n}/{EXPECTED_IMAGE_BYTES} bytes")

    print("\nCleaning up...")
    await b.send_lua("frame.camera.power_save(true)")
    await asyncio.sleep(0.1)
    await b.disconnect()
    plt.ioff()
    plt.close(fig)
    print(f"Stream ended. Total frames: {frame_count}")


if __name__ == "__main__":
    asyncio.run(main())
