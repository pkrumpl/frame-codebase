"""
FOMO Experiment - Continuous Object Detection Stream

Displays live video feed with detections.
Runs autoexposure once at startup, then loops fast detection until Ctrl+C.
Requires: ML_EXPERIMENT=FOMO_BEER_CAN build flashed to Frame.
"""

import asyncio
import signal
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from frameutils import Bluetooth

# Image dimensions (must match OUTPUT_SIZE in experiment_fomo.c)
IMAGE_WIDTH = 64
IMAGE_HEIGHT = 64
EXPECTED_IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT  # 4096

# FOMO model output dimensions
GRID_SIZE = 8
NUM_CLASSES = 3
EXPECTED_PRED_BYTES = GRID_SIZE * GRID_SIZE * NUM_CLASSES  # 192

# Class names
CLASS_NAMES = ["Background", "Beer", "Can"]
CLASS_COLORS = ["none", "red", "blue"]

# Detection threshold
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

    # End marker check (0xFF 0xFF 0x00 0x00)
    if len(data) == 4 and data == b'\xff\xff\x00\x00':
        transfer_complete = True
        return

    # Separator check (0xFE 0xFE)
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


def parse_predictions(pred_data: bytes) -> np.ndarray:
    """Parse the prediction bytes into a grid."""
    if len(pred_data) < EXPECTED_PRED_BYTES:
        pred_data = pred_data + bytes(EXPECTED_PRED_BYTES - len(pred_data))
    pred_array = np.frombuffer(pred_data[:EXPECTED_PRED_BYTES], dtype=np.int8)
    pred_grid = pred_array.reshape((GRID_SIZE, GRID_SIZE, NUM_CLASSES))
    return pred_grid


def find_detections(pred_grid: np.ndarray, threshold: int = DETECTION_THRESHOLD) -> list:
    """Find all detections above the threshold."""
    detections = []
    for y in range(GRID_SIZE):
        for x in range(GRID_SIZE):
            scores = pred_grid[y, x, :]
            for class_id in range(1, NUM_CLASSES):
                score = int(scores[class_id])
                if score > threshold:
                    detections.append((x, y, class_id, score))
    return detections


def update_plot(axes, image, pred_grid, detections, frame_count):
    """Update the matplotlib plot with new frame data."""
    # Clear previous plots
    axes[0].clear()
    axes[1].clear()

    # Original image
    axes[0].imshow(image, cmap='gray', vmin=0, vmax=255)
    axes[0].set_title(f'Frame {frame_count} ({IMAGE_WIDTH}x{IMAGE_HEIGHT})')
    axes[0].axis('off')

    # Image with detections
    axes[1].imshow(image, cmap='gray', vmin=0, vmax=255)
    axes[1].set_title(f'Detections: {len(detections)}')

    cell_width = IMAGE_WIDTH / GRID_SIZE
    cell_height = IMAGE_HEIGHT / GRID_SIZE

    for x, y, class_id, score in detections:
        px = x * cell_width
        py = y * cell_height
        color = CLASS_COLORS[class_id]
        rect = patches.Rectangle(
            (px, py), cell_width, cell_height,
            linewidth=2, edgecolor=color, facecolor='none'
        )
        axes[1].add_patch(rect)
        label = f"{CLASS_NAMES[class_id][:4]}"
        axes[1].text(px + cell_width/2, py + cell_height/2, label,
                    ha='center', va='center', color=color, fontsize=6,
                    fontweight='bold',
                    bbox=dict(boxstyle='round,pad=0.1', facecolor='white', alpha=0.7))

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

    # Setup: Wake camera and run autoexposure once
    print("Initializing camera...")
    await b.send_lua("frame.camera.power_save(false)")
    await asyncio.sleep(0.2)

    print("Running autoexposure (5 iterations)...")
    for i in range(5):
        await b.send_lua("frame.camera.auto({})")
        await asyncio.sleep(0.15)
        print(f"  Autoexposure {i+1}/5")

    print("Camera ready. Starting stream (Ctrl+C to stop)...")

    # Setup matplotlib for live updates
    plt.ion()
    fig, axes = plt.subplots(1, 2, figsize=(10, 5))
    fig.suptitle("Object Detection Stream - Press Ctrl+C to stop")

    frame_count = 0

    while running:
        reset_buffers()

        # Call fast detection (skips camera wake and autoexposure)
        await b.send_lua("frame.experiment.run_object_detection_fast()")

        # Wait for transfer to complete
        timeout = 30
        elapsed = 0
        while not transfer_complete and elapsed < timeout and running:
            await asyncio.sleep(0.1)
            elapsed += 0.1

        if not running:
            break

        if not transfer_complete:
            print(f"Frame {frame_count}: timeout waiting for data")
            continue

        # Process frame
        if len(received_image) >= EXPECTED_IMAGE_BYTES and len(received_predictions) >= EXPECTED_PRED_BYTES:
            frame_count += 1

            img_data = np.frombuffer(bytes(received_image[:EXPECTED_IMAGE_BYTES]), dtype=np.uint8)
            img = img_data.reshape((IMAGE_HEIGHT, IMAGE_WIDTH))

            pred_grid = parse_predictions(bytes(received_predictions))
            detections = find_detections(pred_grid)

            update_plot(axes, img, pred_grid, detections, frame_count)
            fig.suptitle(f"Frame {frame_count} | Detections: {len(detections)}")

            plt.pause(0.01)

            # Print detection summary
            if detections:
                det_summary = ", ".join([f"{CLASS_NAMES[d[2]]}" for d in detections])
                print(f"Frame {frame_count}: {len(detections)} detections - {det_summary}")
            else:
                print(f"Frame {frame_count}: no detections", end="\r")
        else:
            print(f"Frame incomplete: img={len(received_image)}, pred={len(received_predictions)}")

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
