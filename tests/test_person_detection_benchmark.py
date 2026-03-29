"""
Person detection benchmark test - runs local inference without Bluetooth data transfer.
Measures inference timing over multiple iterations.
"""

import asyncio
import argparse
from frameutils import Bluetooth


def print_handler(s: str):
    """Handle print/text responses"""
    print(f"[Frame]: {s}")


def data_handler(data: bytes):
    """Handle incoming binary data (not used in benchmark)"""
    pass


async def run_benchmark(iterations: int = 10):
    """Run person detection benchmark with specified iterations."""

    b = Bluetooth()

    print("Connecting to Frame...")
    try:
        await b.connect(
            print_response_handler=print_handler,
            data_response_handler=data_handler
        )
    except Exception as e:
        print(f"Failed to connect: {e}")
        print("\nMake sure:")
        print("  - Frame is powered on and nearby")
        print("  - Frame is not connected to another device")
        print("  - Bluetooth is enabled on your PC")
        return

    print(f"\nRunning person detection benchmark with {iterations} iterations...")
    print("(Check RTT logs for progress updates)\n")

    try:
        # Run benchmark and store result in global variable
        await b.send_lua(f"_r=frame.experiment.run_person_detection_benchmark({iterations})")

        # Wait for benchmark to complete
        # Estimate timeout based on iterations (rough estimate of ~500ms per iteration + buffer)
        timeout = max(30, iterations * 0.6 + 10)
        print(f"Waiting up to {timeout:.0f}s for benchmark to complete...")
        await asyncio.sleep(timeout)

        # Print results with separate short commands
        print("\n=== BENCHMARK RESULTS ===")
        await b.send_lua("print('Iterations: '.._r.iterations)")
        await asyncio.sleep(0.2)
        await b.send_lua("print('Detections: '.._r.person_detections)")
        await asyncio.sleep(0.2)
        await b.send_lua("print('Total: '.._r.total_time_ms..' ms')")
        await asyncio.sleep(0.2)
        await b.send_lua("print('Avg: '.._r.avg_time_ms..' ms/frame')")
        await asyncio.sleep(0.5)
        print("=========================")

    except Exception as e:
        print(f"Error during benchmark: {e}")

    finally:
        # Cleanup
        print("\nCleaning up...")
        try:
            await b.send_lua("frame.camera.power_save(true)")
            await asyncio.sleep(0.5)
            await b.disconnect()
        except Exception:
            pass

    print("Benchmark complete!")


async def main():
    parser = argparse.ArgumentParser(
        description="Run person detection benchmark on Frame device"
    )
    parser.add_argument(
        "-n", "--iterations",
        type=int,
        default=10,
        help="Number of inference iterations (1-1000, default: 10)"
    )
    args = parser.parse_args()

    if args.iterations < 1 or args.iterations > 1000:
        print("Error: iterations must be between 1 and 1000")
        return

    await run_benchmark(args.iterations)


if __name__ == "__main__":
    asyncio.run(main())
