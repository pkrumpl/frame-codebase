"""
VWW RGB pipeline benchmark host script.

Drives `frame.experiment.run_person_detection_benchmark()` on the Frame
device, receives one BLE "cycle" per pipeline stage (PIPE_STAGES = 7
stages, each carrying PIPE_N = 30 per-iteration DWT cycle counts), and
writes a long-format CSV with one row per (iteration, stage). Also
prints summary statistics per stage and per logical group
(image_acquisition / preprocessing / inference / display).

Wire protocol (must match experiment_vww_rgb.c
lua_experiment_run_person_detection_benchmark):
  Per stage s = 0..K-1:
    - Optional 4-byte sacrificial wake ping 0xAA*4 (its own packet).
      Burned to absorb the first BLE notification after a long
      blocking call; ignored on the host.
    - 4-byte LE stage index + N x uint32 LE cycle counts, streamed
      in chunked BLE packets (frameutils strips the 0x01 data flag).
    - 2-byte stage separator 0xFE 0xFE (its own packet).
  Final terminator: 4-byte packet 0xFF 0xFF 0x00 0x00.

Defaults match the firmware: K=7 stages, N=30 iterations, 5 warmup
iterations (untimed). Adjust --k / --n only if the firmware constants
PIPE_STAGES / PIPE_N are changed.
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

STAGE_NAMES = [
    "capture",      # 0 - frame.camera.capture()
    "wait_ready",   # 1 - poll frame.camera.image_ready()
    "read_jpeg",    # 2 - chunked frame.camera.read()
    "decode",       # 3 - jpeg_decode_rgb_scaled() to 90x90 RGB
    "upscale",      # 4 - bilinear 90->96 + 90 CCW rotation
    "inference",    # 5 - person_detect_infer()
    "display",      # 6 - draw overlay via FPGA SPI
]

# Logical groupings the user requested. The CSV stays granular (per
# stage); these are only used for the printed summary.
STAGE_GROUPS = [
    ("image_acquisition", ["capture", "wait_ready", "read_jpeg"]),
    ("preprocessing",     ["decode", "upscale"]),
    ("inference",         ["inference"]),
    ("display",           ["display"]),
]


class BenchState:
    """Wire decoder. Same shape as inference_benchmark.BenchState; the
    only difference is that cycle_idx here means stage_id (0..K-1)
    rather than a re-randomised cycle of N inferences."""

    def __init__(self, expected_k: int, expected_n: int, *,
                 verbose: bool = False):
        self.expected_k = expected_k
        self.expected_n = expected_n
        self.payload_bytes = 4 + 4 * expected_n
        self.cycle_buffer = bytearray()
        self.cycle_packet_sizes: list[int] = []
        self.cycles: list[tuple[int, list[int]]] = []
        self.transfer_complete = False
        self.error: str | None = None
        self.verbose = verbose

    def feed(self, data: bytes) -> None:
        if self.transfer_complete:
            return
        # End marker (4-byte packet on the wire).
        if len(data) == 4 and data == b"\xff\xff\x00\x00":
            self.transfer_complete = True
            return
        # Sacrificial wake-ping: ignore.
        if len(data) == 4 and data == b"\xaa\xaa\xaa\xaa":
            if self.verbose:
                _print("  rx wake ping (ignored)")
            return
        # Stage separator (2-byte packet).
        if len(data) == 2 and data == b"\xfe\xfe":
            self._finalize_cycle()
            return
        self.cycle_buffer.extend(data)
        self.cycle_packet_sizes.append(len(data))
        if self.verbose:
            _print(
                f"  rx packet {len(data)} B (stage buffer now "
                f"{len(self.cycle_buffer)}/{self.payload_bytes})"
            )

    def _finalize_cycle(self) -> None:
        buf = bytes(self.cycle_buffer)
        sizes = list(self.cycle_packet_sizes)
        self.cycle_buffer.clear()
        self.cycle_packet_sizes.clear()
        if len(buf) != self.payload_bytes:
            self.error = (
                f"stage {len(self.cycles)}: expected "
                f"{self.payload_bytes} bytes, got {len(buf)}; "
                f"received packet sizes: {sizes}"
            )
            self.transfer_complete = True
            return
        stage_idx = struct.unpack("<I", buf[:4])[0]
        timings = list(struct.unpack(f"<{self.expected_n}I", buf[4:]))
        self.cycles.append((stage_idx, timings))


def cycles_to_ns(cycles: int) -> int:
    # 1 cycle = 1e9 / 64e6 ns = 125/8 ns exactly.
    return cycles * 125 // 8


def cycles_to_us(cycles: int) -> float:
    return cycles / (CPU_HZ / 1_000_000.0)  # cycles / 64.0


def _print(msg: str) -> None:
    print(msg, flush=True)


def _print_stats_row(label: str, sorted_us: list[float]) -> None:
    n = len(sorted_us)
    p99 = sorted_us[max(0, int(n * 0.99) - 1)]
    _print(
        f"  {label:<18} "
        f"{statistics.fmean(sorted_us):>12.2f} "
        f"{statistics.median(sorted_us):>12.2f} "
        f"{p99:>12.2f} "
        f"{statistics.pstdev(sorted_us):>12.2f} "
        f"{sorted_us[0]:>12.2f} "
        f"{sorted_us[-1]:>12.2f}"
    )


def _print_stats_header(title: str) -> None:
    _print(f"\n{title}")
    _print(
        f"  {'name':<18} "
        f"{'mean':>12} {'median':>12} {'p99':>12} "
        f"{'stdev':>12} {'min':>12} {'max':>12}"
    )


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
            f"Starting pipeline benchmark (variant={variant}, "
            f"warmup=5, N={n}, stages={k}). The device runs ~{5 + n} full "
            f"pipeline iterations before transmitting; expect tens of "
            f"seconds of silence before the first stage arrives."
        )
        _print("This is a long-running call. Do not disconnect.")
        await b.send_lua("frame.experiment.run_person_detection_benchmark()")

        start = asyncio.get_event_loop().time()
        last_progress = 0
        while not state.transfer_complete:
            await asyncio.sleep(0.5)
            elapsed = asyncio.get_event_loop().time() - start
            if elapsed > timeout:
                state.error = (
                    f"timed out after {timeout:.0f}s with "
                    f"{len(state.cycles)}/{k} stages received - "
                    f"pass --timeout <seconds> to extend"
                )
                break
            if len(state.cycles) != last_progress:
                last_progress = len(state.cycles)
                _print(
                    f"  stages received: {last_progress}/{k} "
                    f"({elapsed:.0f}s elapsed)"
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
            f"ERROR: received {len(state.cycles)} stages, expected {k}"
        )
        return 3

    # Sanity-check stage ordering. Firmware sends 0..K-1 in order.
    for expected_idx, (got_idx, _) in enumerate(state.cycles):
        if got_idx != expected_idx:
            _print(
                f"WARNING: stage {expected_idx} arrived with id {got_idx}"
            )

    # Pivot to per-iteration form: per_iter[i][stage_name] = cycles.
    per_iter: list[dict[str, int]] = [{} for _ in range(n)]
    for stage_idx, timings in state.cycles:
        if not 0 <= stage_idx < len(STAGE_NAMES):
            _print(f"ERROR: stage_idx {stage_idx} out of range")
            return 4
        name = STAGE_NAMES[stage_idx]
        for i, c in enumerate(timings):
            per_iter[i][name] = c

    # Long-format CSV: variant, iteration, stage, cycles, time_ns, time_us.
    rows_written = 0
    with open(output_path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["variant", "iteration", "stage",
                    "cycles", "time_ns", "time_us"])
        for i in range(n):
            for stage in STAGE_NAMES:
                c = per_iter[i].get(stage, 0)
                w.writerow([
                    variant, i, stage, c,
                    cycles_to_ns(c), f"{cycles_to_us(c):.3f}",
                ])
                rows_written += 1

    _print(f"\nWrote {rows_written} rows to {output_path}")

    # Per-stage stats.
    _print_stats_header("Per-stage timing (microseconds):")
    for stage in STAGE_NAMES:
        us = sorted(cycles_to_us(per_iter[i][stage]) for i in range(n))
        _print_stats_row(stage, us)

    # Per-group stats: sum stage cycles within each iteration, then stat.
    _print_stats_header("Per-group timing (microseconds, summed per iteration):")
    for group, members in STAGE_GROUPS:
        per_iter_us = sorted(
            sum(cycles_to_us(per_iter[i][m]) for m in members)
            for i in range(n)
        )
        _print_stats_row(group, per_iter_us)

    # Total per iteration.
    totals = sorted(
        sum(cycles_to_us(per_iter[i][s]) for s in STAGE_NAMES)
        for i in range(n)
    )
    _print_stats_row("TOTAL", totals)

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="VWW RGB pipeline benchmark host. Drives "
                    "frame.experiment.run_person_detection_benchmark() and "
                    "writes a long-format CSV of per-iteration per-stage "
                    "DWT cycle counts."
    )
    parser.add_argument(
        "--variant",
        default="vww_rgb",
        help="Build label (default: vww_rgb). Stored in the CSV.",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Output CSV path (default: vww_pipeline_benchmark_"
             "<variant>_<UTC-timestamp>.csv).",
    )
    parser.add_argument(
        "--k", type=int, default=7,
        help="Expected number of stages K (must match firmware "
             "PIPE_STAGES; default 7).",
    )
    parser.add_argument(
        "--n", type=int, default=30,
        help="Expected iterations per stage N (must match firmware "
             "PIPE_N; default 30).",
    )
    parser.add_argument(
        "--timeout", type=float, default=600.0,
        help="Overall timeout in seconds (default 600 = 10 min). The "
             "device runs warmup + measured loop (~35 full pipeline "
             "iterations) before any data hits BLE.",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Log every received BLE packet size as it arrives.",
    )
    args = parser.parse_args()

    if args.output is None:
        ts = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        args.output = f"vww_pipeline_benchmark_{args.variant}_{ts}.csv"

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
