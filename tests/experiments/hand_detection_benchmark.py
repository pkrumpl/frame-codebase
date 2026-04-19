"""
FOMO Hand Detection - On-Device Benchmark

Triggers `frame.experiment.run_hand_detection_benchmark(N)` on the Frame and
prints a per-stage timing report (capture, wait, read, decode, upscale,
inference, display) parsed from the device's CSV-formatted print output.

Requires: ML_EXPERIMENT=FOMO_HAND_DETECTION build flashed to Frame.
"""

import argparse
import asyncio
import re
from frameutils import Bluetooth

CSV_TAG = "BENCH"
CSV_FIELDS = [
    "iterations",
    "hand_detections",
    "total_time_ms",
    "avg_time_ms",
    "memset_ms",
    "capture_ms",
    "wait_ready_ms",
    "read_jpeg_ms",
    "decode_ms",
    "upscale_ms",
    "inference_ms",
    "display_ms",
]

# Capture the CSV line emitted by the Lua snippet below
CSV_RE = re.compile(rf"^{CSV_TAG}:([\d,]+)$")

result_csv = None


def print_handler(s: str):
    """Print every line from the device, but pluck out the CSV result line."""
    global result_csv
    print(f"[Frame]: {s}")
    line = s.strip()
    m = CSV_RE.match(line)
    if m:
        result_csv = m.group(1)


def data_handler(_data: bytes):
    pass


def build_lua(iterations: int) -> str:
    """Run the on-device benchmark and print one CSV line of results."""
    parts = "..','..".join(f"tostring(r.{f})" for f in CSV_FIELDS)
    return (
        f"r = frame.experiment.run_hand_detection_benchmark({iterations}); "
        f"print('{CSV_TAG}:'..{parts})"
    )


def render_report(csv_line: str):
    parts = csv_line.split(",")
    if len(parts) != len(CSV_FIELDS):
        print(f"ERROR: expected {len(CSV_FIELDS)} fields, got {len(parts)}: {csv_line}")
        return
    values = dict(zip(CSV_FIELDS, (int(p) for p in parts)))

    print()
    print("=" * 60)
    print(f"FOMO Hand Detection Benchmark Results")
    print("=" * 60)
    print(f"Iterations:           {values['iterations']}")
    print(f"Frames with hand:     {values['hand_detections']}")
    print(f"Total time:           {values['total_time_ms']} ms")
    print(f"Average per iter:     {values['avg_time_ms']} ms")
    print()
    print("Per-stage breakdown (total ms across all iterations):")
    print(f"  memset:             {values['memset_ms']} ms")
    print(f"  camera capture:     {values['capture_ms']} ms")
    print(f"  wait image_ready:   {values['wait_ready_ms']} ms")
    print(f"  read JPEG:          {values['read_jpeg_ms']} ms")
    print(f"  decode JPEG:        {values['decode_ms']} ms")
    print(f"  upscale (90->96):   {values['upscale_ms']} ms")
    print(f"  inference (TFLM):   {values['inference_ms']} ms")
    print(f"  display overlay:    {values['display_ms']} ms")
    print()
    iters = max(values['iterations'], 1)
    print("Per-iteration (avg ms):")
    print(f"  inference:          {values['inference_ms'] / iters:.2f} ms")
    print(f"  decode:             {values['decode_ms'] / iters:.2f} ms")
    print(f"  upscale:            {values['upscale_ms'] / iters:.2f} ms")
    print(f"  capture+read+wait:  "
          f"{(values['capture_ms'] + values['read_jpeg_ms'] + values['wait_ready_ms']) / iters:.2f} ms")
    print("=" * 60)


async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-n", "--iterations", type=int, default=50,
                        help="Number of inference iterations (1-1000, default: 50)")
    args = parser.parse_args()
    if not 1 <= args.iterations <= 1000:
        parser.error("iterations must be between 1 and 1000")

    b = Bluetooth()
    print("Connecting to Frame...")
    await b.connect(print_response_handler=print_handler,
                    data_response_handler=data_handler)

    lua = build_lua(args.iterations)
    print(f"Sending: {lua}")
    await b.send_lua(lua)

    # Wait for the benchmark to finish.  Worst case: 1000 iterations *
    # ~600ms per iteration is ~10 minutes; default 50 iterations is well
    # under a minute.  Poll for the CSV line.
    timeout_s = max(60.0, args.iterations * 1.5)
    elapsed = 0.0
    while result_csv is None and elapsed < timeout_s:
        await asyncio.sleep(0.5)
        elapsed += 0.5

    await b.send_lua("frame.camera.power_save(true)")
    await asyncio.sleep(0.2)
    await b.disconnect()

    if result_csv is None:
        print(f"\nERROR: no benchmark result received within {timeout_s:.0f}s")
        return

    render_report(result_csv)


if __name__ == "__main__":
    asyncio.run(main())
