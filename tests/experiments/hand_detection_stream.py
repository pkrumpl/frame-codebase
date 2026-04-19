"""
FOMO Hand Detection - Continuous Stream

Displays live RGB video feed from Frame with hand detection overlays.
Runs autoexposure once at startup, then loops fast detection until Ctrl+C.
Requires: ML_EXPERIMENT=FOMO_HAND_DETECTION build flashed to Frame.
"""

import asyncio
import signal
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from frameutils import Bluetooth

# Image dimensions (must match OUT_DIM in experiment_fomo_hand.c)
IMAGE_WIDTH = 96
IMAGE_HEIGHT = 96
NUM_CHANNELS = 3
EXPECTED_IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * NUM_CHANNELS  # 27648

# FOMO model output dimensions (must match FOMO_GRID_SIZE / FOMO_NUM_CLASSES)
GRID_SIZE = 8
NUM_CLASSES = 2
EXPECTED_PRED_BYTES = GRID_SIZE * GRID_SIZE * NUM_CLASSES  # 128

CLASS_NAMES = ["Background", "Hand"]
HAND_COLOR = "lime"

# Detection threshold (matches DETECTION_THRESHOLD in experiment_fomo_hand.c)
DETECTION_THRESHOLD = -50

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

    if len(data) == 4 and data == b'\xff\xff\x00\x00':
        transfer_complete = True
        return

    if len(data) == 2 and data == b'\xfe\xfe':
        receiving_predictions = True
        return

    if not receiving_predictions:
        received_image.extend(data)
    else:
        received_predictions.extend(data)


def print_handler(s: str):
    print(f"[Frame]: {s}")


def parse_predictions(pred_data: bytes) -> np.ndarray:
    if len(pred_data) < EXPECTED_PRED_BYTES:
        pred_data = pred_data + bytes(EXPECTED_PRED_BYTES - len(pred_data))
    pred_array = np.frombuffer(pred_data[:EXPECTED_PRED_BYTES], dtype=np.int8)
    return pred_array.reshape((GRID_SIZE, GRID_SIZE, NUM_CLASSES))


def find_detections(pred_grid: np.ndarray, threshold: int = DETECTION_THRESHOLD) -> list:
    """Find all hand detections (class 1) above the threshold."""
    detections = []
    for y in range(GRID_SIZE):
        for x in range(GRID_SIZE):
            score = int(pred_grid[y, x, 1])
            if score > threshold:
                detections.append((x, y, score))
    return detections


def update_plot(axes, image, detections, frame_count):
    axes[0].clear()
    axes[1].clear()

    # interpolation='nearest' avoids matplotlib's antialiased upsampling,
    # which produces moire/banding artifacts on small images.
    axes[0].imshow(image, interpolation='nearest')
    axes[0].set_title(f'Frame {frame_count} ({IMAGE_WIDTH}x{IMAGE_HEIGHT} RGB)')
    axes[0].axis('off')

    axes[1].imshow(image, interpolation='nearest')
    axes[1].set_title(f'Hand detections: {len(detections)}')

    cell_width = IMAGE_WIDTH / GRID_SIZE
    cell_height = IMAGE_HEIGHT / GRID_SIZE

    for x, y, score in detections:
        px = x * cell_width
        py = y * cell_height
        # Cell rectangle
        rect = patches.Rectangle(
            (px, py), cell_width, cell_height,
            linewidth=1.5, edgecolor=HAND_COLOR, facecolor='none', alpha=0.6
        )
        axes[1].add_patch(rect)
        # Centered dot
        axes[1].plot(px + cell_width / 2, py + cell_height / 2,
                     marker='o', markersize=10, color=HAND_COLOR,
                     markeredgecolor='black', markeredgewidth=1)
        axes[1].text(px + cell_width / 2, py - 1, f"{score}",
                     ha='center', va='bottom', color=HAND_COLOR, fontsize=7,
                     fontweight='bold')

    axes[1].axis('off')


async def main():
    global running

    signal.signal(signal.SIGINT, signal_handler)

    b = Bluetooth()

    print("Connecting to Frame...")
    await b.connect(
        print_response_handler=print_handler,
        data_response_handler=data_handler
    )

    print("Initializing camera...")
    await b.send_lua("frame.camera.power_save(false)")
    await asyncio.sleep(0.2)

    print("Running autoexposure (5 iterations)...")
    for i in range(5):
        await b.send_lua("frame.camera.auto({})")
        await asyncio.sleep(0.15)
        print(f"  Autoexposure {i+1}/5")

    print("Camera ready. Starting stream (Ctrl+C to stop)...")

    plt.ion()
    fig, axes = plt.subplots(1, 2, figsize=(10, 5))
    fig.suptitle("Hand Detection Stream - Press Ctrl+C to stop")

    frame_count = 0

    while running:
        reset_buffers()

        # Full pipeline (with per-frame power_save+autoexpose). The fast
        # variant skips that and fires a new 27 KB image transmission
        # immediately after the previous one - the BLE TX queue never
        # drains between frames and packets get dropped at the link
        # layer. VWW_RGB stream uses the same full-pipeline pattern.
        await b.send_lua("frame.experiment.run_hand_detection()")

        timeout = 45
        elapsed = 0
        while not transfer_complete and elapsed < timeout and running:
            await asyncio.sleep(0.05)
            elapsed += 0.05

        if not running:
            break

        if not transfer_complete:
            print(f"Frame {frame_count}: timeout waiting for data")
            continue

        if (len(received_image) >= EXPECTED_IMAGE_BYTES
                and len(received_predictions) >= EXPECTED_PRED_BYTES):
            frame_count += 1

            # Diagnostic: byte counts. If image > 27648 we lost the separator
            # mid-stream (image bytes got mis-classified). If image == 27648
            # exactly, framing was clean.
            img_extra = len(received_image) - EXPECTED_IMAGE_BYTES
            pred_extra = len(received_predictions) - EXPECTED_PRED_BYTES
            if img_extra != 0 or pred_extra != 0:
                print(f"  [warn] byte drift: image={len(received_image)} (+{img_extra}), "
                      f"pred={len(received_predictions)} (+{pred_extra})")

            img_data = np.frombuffer(bytes(received_image[:EXPECTED_IMAGE_BYTES]), dtype=np.uint8)
            img = img_data.reshape((IMAGE_HEIGHT, IMAGE_WIDTH, NUM_CHANNELS))

            pred_grid = parse_predictions(bytes(received_predictions))
            detections = find_detections(pred_grid)

            update_plot(axes, img, detections, frame_count)
            fig.suptitle(f"Frame {frame_count} | Hands: {len(detections)}")
            plt.pause(0.01)

            if detections:
                print(f"Frame {frame_count}: {len(detections)} hand cell(s)")
            else:
                print(f"Frame {frame_count}: no hands", end="\r")
        else:
            print(f"Frame incomplete: img={len(received_image)}, "
                  f"pred={len(received_predictions)}")

    print("\nCleaning up...")
    await b.send_lua("frame.camera.power_save(true)")
    await asyncio.sleep(0.1)
    await b.disconnect()

    plt.ioff()
    plt.close(fig)

    print(f"Stream ended. Total frames: {frame_count}")


if __name__ == "__main__":
    asyncio.run(main())
