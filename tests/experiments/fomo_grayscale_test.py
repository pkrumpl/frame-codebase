"""
FOMO Experiment - Grayscale Image Test

Tests the grayscale image capture and transfer functionality.
Receives grayscale image data and displays it.
Requires: ML_EXPERIMENT=FOMO_BEER_CAN build flashed to Frame.
"""

import asyncio
import numpy as np
import matplotlib.pyplot as plt
from frameutils import Bluetooth

# Image dimensions (must match CAPTURE_SIZE in experiment_fomo.c)
IMAGE_WIDTH = 96
IMAGE_HEIGHT = 96
EXPECTED_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT

# Buffer to collect received data
received_data = bytearray()
transfer_complete = False


def data_handler(data: bytes):
    """Handle incoming binary data from Bluetooth.
    Note: frameutils already stripped the 0x01 flag byte before calling this handler.
    """
    global received_data, transfer_complete

    # End marker is 4 bytes: 0xFF 0xFF 0x00 0x00 (after 0x01 flag stripped by frameutils)
    if len(data) == 4 and data == b'\xff\xff\x00\x00':
        transfer_complete = True
        print(f"\nTransfer complete! Total: {len(received_data)} bytes")
        return

    # Append all data (flag already stripped by frameutils)
    received_data.extend(data)
    print(f"Received {len(data)} bytes, total: {len(received_data)}/{EXPECTED_BYTES}", end="\r")


def print_handler(s: str):
    """Handle print/text responses"""
    print(f"[Frame]: {s}")


async def main():
    global received_data, transfer_complete
    received_data = bytearray()
    transfer_complete = False

    b = Bluetooth()

    # Connect with both data and print handlers
    await b.connect(
        print_response_handler=print_handler,
        data_response_handler=data_handler
    )

    print("Requesting grayscale image...")
    await b.send_lua("frame.experiment.send_grayscale()")

    # Wait for transfer to complete (with timeout)
    timeout = 30  # seconds
    elapsed = 0
    while not transfer_complete and elapsed < timeout:
        await asyncio.sleep(0.5)
        elapsed += 0.5

    await b.disconnect()

    if len(received_data) < EXPECTED_BYTES:
        print(f"Warning: Only received {len(received_data)} of {EXPECTED_BYTES} bytes")

    # Convert to numpy array and reshape to image
    if len(received_data) > 0:
        # Use only the expected number of bytes
        img_data = np.frombuffer(bytes(received_data[:EXPECTED_BYTES]), dtype=np.uint8)

        if len(img_data) >= EXPECTED_BYTES:
            img = img_data[:EXPECTED_BYTES].reshape((IMAGE_HEIGHT, IMAGE_WIDTH))

            # Display the image
            plt.figure(figsize=(6, 6))
            plt.imshow(img, cmap='gray', vmin=0, vmax=255)
            plt.title(f'Grayscale Image ({IMAGE_WIDTH}x{IMAGE_HEIGHT})')
            plt.colorbar(label='Pixel Value')
            plt.show()
        else:
            print(f"Error: Expected {EXPECTED_BYTES} bytes, got {len(img_data)}")
    else:
        print("No data received")


if __name__ == "__main__":
    asyncio.run(main())
