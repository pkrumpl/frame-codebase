"""
HELLO_WORLD Experiment - Basic Inference Test

Tests the hello_world sine wave prediction model (both float and int8).
Requires: ML_EXPERIMENT=HELLO_WORLD build flashed to Frame.
"""

import asyncio
import math
from frameutils import Bluetooth


def print_handler(s: str):
    """Handle print/text responses"""
    print(f"[Frame]: {s}")


async def main():
    b = Bluetooth()

    print("Connecting to Frame...")
    await b.connect(print_response_handler=print_handler)

    print("\n=== HELLO_WORLD Inference Test ===\n")

    # Test float model inference
    print("Testing float model inference:")
    test_angles = [0, math.pi/4, math.pi/2, math.pi, 3*math.pi/2]

    for angle in test_angles:
        expected = math.sin(angle)
        await b.send_lua(f"print('Float: sin({angle:.3f}) = '..frame.experiment.infer({angle}))")
        await asyncio.sleep(0.3)
        print(f"  Expected: {expected:.4f}")

    print("\nTesting int8 model inference:")
    for angle in test_angles:
        expected = math.sin(angle)
        await b.send_lua(f"print('Int8: sin({angle:.3f}) = '..frame.experiment.infer_int8({angle}))")
        await asyncio.sleep(0.3)
        print(f"  Expected: {expected:.4f}")

    print("\n=== Test Complete ===")
    await b.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
