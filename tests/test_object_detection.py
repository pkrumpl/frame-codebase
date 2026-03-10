"""
Tests the FOMO object detection model on Frame.
Receives grayscale image data and predictions, displays results with detection overlays.
"""

import asyncio
import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as patches
from frameutils import Bluetooth

# Image dimensions (must match OUTPUT_SIZE in experiment.c)
IMAGE_WIDTH = 64
IMAGE_HEIGHT = 64
EXPECTED_IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT  # 4096

# FOMO model output dimensions
GRID_SIZE = 8
NUM_CLASSES = 3
EXPECTED_PRED_BYTES = GRID_SIZE * GRID_SIZE * NUM_CLASSES  # 192

# Class names
CLASS_NAMES = ["Background", "Beer", "Can"]
CLASS_COLORS = ["none", "red", "blue"]  # Background not shown

# Detection threshold (for int8 quantized output)
# Higher values mean more confident detections
DETECTION_THRESHOLD = -50  # int8 range is -128 to 127, 0 means above average confidence

# Buffer states
class DataState:
    RECEIVING_IMAGE = 0
    RECEIVING_SEPARATOR = 1
    RECEIVING_PREDICTIONS = 2
    COMPLETE = 3

# Global state
received_image = bytearray()
received_predictions = bytearray()
current_state = DataState.RECEIVING_IMAGE
transfer_complete = False


def data_handler(data: bytes):
    """Handle incoming binary data from Bluetooth.
    Note: frameutils already stripped the 0x01 flag byte before calling this handler.

    Protocol:
        [IMAGE DATA]     9216 bytes
        [SEPARATOR]      0xFE 0xFE
        [PREDICTIONS]    432 bytes
        [END MARKER]     0xFF 0xFF 0x00 0x00
    """
    global received_image, received_predictions, current_state, transfer_complete

    # End marker check (0xFF 0xFF 0x00 0x00)
    if len(data) == 4 and data == b'\xff\xff\x00\x00':
        transfer_complete = True
        current_state = DataState.COMPLETE
        print(f"\nTransfer complete! Image: {len(received_image)} bytes, Predictions: {len(received_predictions)} bytes")
        return

    # Separator check (0xFE 0xFE)
    if len(data) == 2 and data == b'\xfe\xfe':
        if current_state == DataState.RECEIVING_IMAGE:
            current_state = DataState.RECEIVING_PREDICTIONS
            print(f"\nSeparator received. Image: {len(received_image)} bytes. Now receiving predictions...")
        return

    # Append data based on current state
    if current_state == DataState.RECEIVING_IMAGE:
        received_image.extend(data)
        print(f"Image: {len(received_image)}/{EXPECTED_IMAGE_BYTES} bytes", end="\r")
    elif current_state == DataState.RECEIVING_PREDICTIONS:
        received_predictions.extend(data)
        print(f"Predictions: {len(received_predictions)}/{EXPECTED_PRED_BYTES} bytes", end="\r")


def print_handler(s: str):
    """Handle print/text responses"""
    print(f"[Frame]: {s}")


def parse_predictions(pred_data: bytes) -> np.ndarray:
    """Parse the 432 bytes of int8 predictions into a 12x12x3 grid."""
    if len(pred_data) < EXPECTED_PRED_BYTES:
        print(f"Warning: Only got {len(pred_data)} of {EXPECTED_PRED_BYTES} prediction bytes")
        # Pad with zeros
        pred_data = pred_data + bytes(EXPECTED_PRED_BYTES - len(pred_data))

    # Convert to int8 numpy array
    pred_array = np.frombuffer(pred_data[:EXPECTED_PRED_BYTES], dtype=np.int8)
    # Reshape to [12, 12, 3]
    pred_grid = pred_array.reshape((GRID_SIZE, GRID_SIZE, NUM_CLASSES))
    return pred_grid


def find_detections(pred_grid: np.ndarray, threshold: int = DETECTION_THRESHOLD) -> list:
    """Find all detections above the threshold.

    Args:
        pred_grid: 12x12x3 int8 array
        threshold: int8 threshold value

    Returns:
        List of (grid_x, grid_y, class_id, confidence) tuples
    """
    detections = []

    for y in range(GRID_SIZE):
        for x in range(GRID_SIZE):
            # Get class scores for this grid cell
            scores = pred_grid[y, x, :]

            # Find best non-background class
            # Class 0 is background, 1 is Beer, 2 is Can
            for class_id in range(1, NUM_CLASSES):  # Skip background
                score = int(scores[class_id])
                if score > threshold:
                    detections.append((x, y, class_id, score))

    return detections


def plot_results(image: np.ndarray, pred_grid: np.ndarray, detections: list):
    """Plot the image with detection overlays."""
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))

    # Original image
    axes[0].imshow(image, cmap='gray', vmin=0, vmax=255)
    axes[0].set_title(f'Grayscale Image ({IMAGE_WIDTH}x{IMAGE_HEIGHT})')
    axes[0].axis('off')

    # Image with detections
    axes[1].imshow(image, cmap='gray', vmin=0, vmax=255)
    axes[1].set_title(f'Detections (threshold={DETECTION_THRESHOLD})')

    # Calculate cell size in pixels
    cell_width = IMAGE_WIDTH / GRID_SIZE
    cell_height = IMAGE_HEIGHT / GRID_SIZE

    # Draw detections
    for x, y, class_id, score in detections:
        # Calculate pixel coordinates
        px = x * cell_width
        py = y * cell_height

        color = CLASS_COLORS[class_id]
        rect = patches.Rectangle(
            (px, py), cell_width, cell_height,
            linewidth=2, edgecolor=color, facecolor='none'
        )
        axes[1].add_patch(rect)

        # Add label
        label = f"{CLASS_NAMES[class_id]}\n{score}"
        axes[1].text(px + cell_width/2, py + cell_height/2, label,
                    ha='center', va='center', color=color, fontsize=6,
                    fontweight='bold',
                    bbox=dict(boxstyle='round,pad=0.1', facecolor='white', alpha=0.7))

    axes[1].axis('off')

    # Heatmap of detections
    # Create a heatmap showing max non-background score per cell
    heatmap = np.zeros((GRID_SIZE, GRID_SIZE))
    for y in range(GRID_SIZE):
        for x in range(GRID_SIZE):
            # Max of non-background classes
            heatmap[y, x] = max(pred_grid[y, x, 1], pred_grid[y, x, 2])

    im = axes[2].imshow(heatmap, cmap='hot', vmin=-128, vmax=127)
    axes[2].set_title('Detection Heatmap (max non-bg score)')
    plt.colorbar(im, ax=axes[2], label='Confidence')

    # Add grid lines
    for i in range(GRID_SIZE + 1):
        axes[2].axhline(i - 0.5, color='white', linewidth=0.5, alpha=0.5)
        axes[2].axvline(i - 0.5, color='white', linewidth=0.5, alpha=0.5)

    plt.tight_layout()
    plt.show()


def print_detection_summary(detections: list):
    """Print summary of detections."""
    print("\n" + "="*50)
    print("DETECTION SUMMARY")
    print("="*50)

    if not detections:
        print("No detections found above threshold")
        return

    # Count by class
    class_counts = {name: 0 for name in CLASS_NAMES[1:]}  # Skip background
    for _, _, class_id, _ in detections:
        class_counts[CLASS_NAMES[class_id]] += 1

    print(f"Total detections: {len(detections)}")
    for class_name, count in class_counts.items():
        print(f"  {class_name}: {count}")

    print("\nDetailed detections:")
    for i, (x, y, class_id, score) in enumerate(detections):
        print(f"  [{i+1}] {CLASS_NAMES[class_id]} at grid ({x},{y}) with score {score}")


async def main():
    global received_image, received_predictions, current_state, transfer_complete

    # Reset state
    received_image = bytearray()
    received_predictions = bytearray()
    current_state = DataState.RECEIVING_IMAGE
    transfer_complete = False

    b = Bluetooth()

    # Connect with both data and print handlers
    await b.connect(
        print_response_handler=print_handler,
        data_response_handler=data_handler
    )

    print("Running FOMO object detection model...")
    await b.send_lua("frame.experiment.run_object_detection_model()")

    # Wait for transfer to complete (with timeout)
    timeout = 60  # seconds - inference + transfer can take time
    elapsed = 0
    while not transfer_complete and elapsed < timeout:
        await asyncio.sleep(0.5)
        elapsed += 0.5

    await b.disconnect()

    # Validate received data
    if len(received_image) < EXPECTED_IMAGE_BYTES:
        print(f"Warning: Only received {len(received_image)} of {EXPECTED_IMAGE_BYTES} image bytes")

    if len(received_predictions) < EXPECTED_PRED_BYTES:
        print(f"Warning: Only received {len(received_predictions)} of {EXPECTED_PRED_BYTES} prediction bytes")

    # Process and display results
    if len(received_image) > 0 and len(received_predictions) > 0:
        # Convert image to numpy array
        img_data = np.frombuffer(bytes(received_image[:EXPECTED_IMAGE_BYTES]), dtype=np.uint8)

        if len(img_data) >= EXPECTED_IMAGE_BYTES:
            img = img_data[:EXPECTED_IMAGE_BYTES].reshape((IMAGE_HEIGHT, IMAGE_WIDTH))

            # Parse predictions
            pred_grid = parse_predictions(bytes(received_predictions))

            # Find detections
            detections = find_detections(pred_grid)

            # Print summary
            print_detection_summary(detections)

            # Plot results
            plot_results(img, pred_grid, detections)
        else:
            print(f"Error: Expected {EXPECTED_IMAGE_BYTES} bytes, got {len(img_data)}")
    else:
        print("Incomplete data received")


if __name__ == "__main__":
    asyncio.run(main())
