"""
HELLO_WORLD Experiment - Run Test Function

Runs the built-in test function that compares model predictions with actual sine values.
Requires: ML_EXPERIMENT=HELLO_WORLD build flashed to Frame.
"""

import asyncio
from frameutils import Bluetooth


def print_handler(s: str):
    """Handle print/text responses"""
    print(f"[Frame]: {s}")


async def main():
    b = Bluetooth()

    print("Connecting to Frame...")
    await b.connect(print_response_handler=print_handler)

    print("\n=== HELLO_WORLD Run Test ===\n")

    # Run the built-in test and capture results
    await b.send_lua("_r = frame.experiment.run_test()")
    await asyncio.sleep(2)  # Wait for test to complete

    # Print results
    print("\n--- Test Results ---")
    await b.send_lua("print('Samples: '.._r.num_samples)")
    await asyncio.sleep(0.2)
    await b.send_lua("print('Float Avg Error: '.._r.avg_error_float)")
    await asyncio.sleep(0.2)
    await b.send_lua("print('Float Max Error: '.._r.max_error_float)")
    await asyncio.sleep(0.2)
    await b.send_lua("print('Int8 Avg Error: '.._r.avg_error_int8)")
    await asyncio.sleep(0.2)
    await b.send_lua("print('Int8 Max Error: '.._r.max_error_int8)")
    await asyncio.sleep(0.5)

    print("\n=== Test Complete ===")
    await b.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
