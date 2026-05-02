"""
Pure-inference benchmark host script.

Drives `frame.experiment.run_inference_benchmark()` on the device, receives
the streamed DWT cycle counts, and writes one row per timed inference to a
CSV. The same script works for every ML_EXPERIMENT build that exposes
that Lua function (VWW, VWW_RGB, HELLO_WORLD, FOMO_BEER_CAN); pass
`--variant` so the build label ends up in the CSV.

Wire protocol (must match experiment_*.c):
  Per cycle k = 0..K-1:
    - 4-byte sacrificial wake ping: 0xAA 0xAA 0xAA 0xAA (its own packet).
      The first BLE notification after a long inference burst is reliably
      dropped at the link layer, so we burn this slot intentionally and
      ignore it on the host. May or may not arrive; we don't care.
    - 4-byte LE cycle index + N x uint32 LE cycle counts, streamed in
      chunked BLE packets (frameutils strips the leading 0x01 data flag).
    - 2-byte cycle separator: 0xFE 0xFE (its own packet).
  Final terminator: 4-byte packet 0xFF 0xFF 0x00 0x00.

Defaults match the firmware: K=8, N=64. Adjust with --k / --n if you
change the firmware constants.
"""

import argparse
import asyncio
import csv
import datetime as _dt
import statistics
import struct
import sys

from frameutils import Bluetooth


CPU_HZ = 64_000_000  # nRF52840 system clock used to drive DWT->CYCCNT


class BenchState:
    def __init__(self, expected_k: int, expected_n: int, *,
                 verbose: bool = False):
        self.expected_k = expected_k
        self.expected_n = expected_n
        self.payload_bytes = 4 + 4 * expected_n
        self.cycle_buffer = bytearray()
        # Sizes of each BLE packet that contributed to the current cycle's
        # buffer, so we can diagnose dropped chunks if the size is wrong.
        self.cycle_packet_sizes: list[int] = []
        self.cycles = []  # list of (cycle_idx, [N timings])
        self.transfer_complete = False
        self.error = None
        self.verbose = verbose

    def feed(self, data: bytes) -> None:
        if self.transfer_complete:
            return
        # End marker (its own 4-byte packet on the wire).
        if len(data) == 4 and data == b"\xff\xff\x00\x00":
            self.transfer_complete = True
            return
        # Sacrificial wake-ping: ignore. (See module docstring.)
        if len(data) == 4 and data == b"\xaa\xaa\xaa\xaa":
            if self.verbose:
                _print("  rx wake ping (ignored)")
            return
        # Cycle separator (its own 2-byte packet on the wire).
        if len(data) == 2 and data == b"\xfe\xfe":
            self._finalize_cycle()
            return
        self.cycle_buffer.extend(data)
        self.cycle_packet_sizes.append(len(data))
        if self.verbose:
            _print(
                f"  rx packet {len(data)} B (cycle buffer now "
                f"{len(self.cycle_buffer)}/{self.payload_bytes})"
            )

    def _finalize_cycle(self) -> None:
        buf = bytes(self.cycle_buffer)
        sizes = list(self.cycle_packet_sizes)
        self.cycle_buffer.clear()
        self.cycle_packet_sizes.clear()
        if len(buf) != self.payload_bytes:
            self.error = (
                f"cycle {len(self.cycles)}: expected "
                f"{self.payload_bytes} bytes, got {len(buf)}; "
                f"received packet sizes: {sizes}"
            )
            self.transfer_complete = True
            return
        cycle_idx = struct.unpack("<I", buf[:4])[0]
        timings = list(struct.unpack(f"<{self.expected_n}I", buf[4:]))
        self.cycles.append((cycle_idx, timings))


def cycles_to_ns(cycles: int) -> int:
    # 1 cycle = 1e9 / 64e6 ns = 125/8 ns exactly.
    return cycles * 125 // 8


def cycles_to_us(cycles: int) -> float:
    return cycles / (CPU_HZ / 1_000_000.0)  # cycles / 64.0


def _print(msg: str) -> None:
    print(msg, flush=True)


async def run_benchmark(variant: str, output_path: str, k: int, n: int,
                        timeout: float, verbose: bool) -> int:
    state = BenchState(expected_k=k, expected_n=n, verbose=verbose)
    b = Bluetooth()

    def print_handler(s: str) -> None:
        _print(f"[Frame]: {s}")

    def data_handler(data: bytes) -> None:
        state.feed(data)

    _print("Connecting to Frame...")
    try:
        await b.connect(
            print_response_handler=print_handler,
            data_response_handler=data_handler,
        )
    except Exception as e:
        _print(f"Failed to connect: {e}")
        _print(
            "Make sure the Frame is powered on, nearby, not connected to "
            "another device, and that Bluetooth is enabled on your PC."
        )
        return 1

    try:
        _print(
            f"Starting pure-inference benchmark (variant={variant}, "
            f"K={k}, N={n}, total inferences={k * n})."
        )
        _print("This is a long-running call. Do not disconnect.")
        await b.send_lua("frame.experiment.run_inference_benchmark()")

        start = asyncio.get_event_loop().time()
        last_progress = 0
        while not state.transfer_complete:
            await asyncio.sleep(0.2)
            elapsed = asyncio.get_event_loop().time() - start
            if elapsed > timeout:
                state.error = (
                    f"timed out after {timeout:.0f}s with "
                    f"{len(state.cycles)}/{k} cycles received - "
                    f"pass --timeout <seconds> to extend"
                )
                break
            if len(state.cycles) != last_progress:
                last_progress = len(state.cycles)
                rate = elapsed / last_progress if last_progress else 0
                eta = rate * (k - last_progress)
                _print(
                    f"  cycles received: {last_progress}/{k} "
                    f"({elapsed:.0f}s elapsed, ~{eta:.0f}s remaining)"
                )
    finally:
        try:
            await b.disconnect()
        except Exception:
            pass

    if state.error:
        _print(f"ERROR: {state.error}")
        return 2
    if len(state.cycles) != k:
        _print(
            f"ERROR: received {len(state.cycles)} cycles, expected {k}"
        )
        return 3

    # Sanity-check: cycle indices should be 0..K-1 in order.
    for expected_idx, (got_idx, _) in enumerate(state.cycles):
        if got_idx != expected_idx:
            _print(
                f"WARNING: cycle {expected_idx} arrived with index {got_idx}"
            )

    # Write CSV.
    rows_written = 0
    with open(output_path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["variant", "cycle", "iteration", "cycles", "time_ns",
                    "time_us"])
        for cycle_idx, timings in state.cycles:
            for i, c in enumerate(timings):
                w.writerow([
                    variant,
                    cycle_idx,
                    i,
                    c,
                    cycles_to_ns(c),
                    f"{cycles_to_us(c):.3f}",
                ])
                rows_written += 1

    _print(f"Wrote {rows_written} rows to {output_path}")

    # Tiny summary so the user gets a sanity-check without opening the CSV.
    all_us = [cycles_to_us(c) for _, ts in state.cycles for c in ts]
    if all_us:
        all_us_sorted = sorted(all_us)
        mean_us = statistics.fmean(all_us)
        median_us = statistics.median(all_us_sorted)
        p99_us = all_us_sorted[max(0, int(len(all_us_sorted) * 0.99) - 1)]
        stdev_us = statistics.pstdev(all_us)
        _print("Summary (microseconds per inference):")
        _print(f"  mean   = {mean_us:.3f} us")
        _print(f"  median = {median_us:.3f} us")
        _print(f"  p99    = {p99_us:.3f} us")
        _print(f"  stdev  = {stdev_us:.3f} us")
        _print(f"  min    = {all_us_sorted[0]:.3f} us")
        _print(f"  max    = {all_us_sorted[-1]:.3f} us")

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pure-inference benchmark for any ML_EXPERIMENT build "
                    "on Frame (VWW / VWW_RGB / HELLO_WORLD / FOMO)."
    )
    parser.add_argument(
        "--variant",
        choices=("vww", "vww_rgb", "hello_world", "fomo"),
        default="vww",
        help="Which firmware build is flashed (only affects CSV labelling).",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output CSV path (default: inference_benchmark_<variant>"
             "_<UTC-timestamp>.csv).",
    )
    parser.add_argument(
        "--k", type=int, default=8,
        help="Expected number of cycles K (must match firmware BENCH_K).",
    )
    parser.add_argument(
        "--n", type=int, default=64,
        help="Expected inferences per cycle N (must match firmware BENCH_N).",
    )
    parser.add_argument(
        "--timeout", type=float, default=7200.0,
        help="Overall timeout in seconds (default 7200 = 2 h). "
             "Reference-kernel builds (USE_CMSIS_NN=0) can need this; "
             "CMSIS-NN-enabled builds typically finish in minutes.",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Log every received BLE packet size as it arrives.",
    )
    args = parser.parse_args()

    if args.output is None:
        ts = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        args.output = f"inference_benchmark_{args.variant}_{ts}.csv"

    return asyncio.run(run_benchmark(
        variant=args.variant,
        output_path=args.output,
        k=args.k,
        n=args.n,
        timeout=args.timeout,
        verbose=args.verbose,
    ))


if __name__ == "__main__":
    sys.exit(main())
