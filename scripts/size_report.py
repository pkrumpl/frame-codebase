#!/usr/bin/env python3
"""Report flash/RAM utilisation for the Frame application image.

The report is derived directly from the linker symbols emitted in
``build/application.elf`` so it reflects the actual addresses that will be
programmed onto the nRF52840 (SoftDevice + bootloader reservations included).

Usage:

	python size_report.py [--elf build/application.elf]

Make sure a Release (or Development Kit) build has been generated so the ELF
contains the latest code. The script requires ``arm-none-eabi-nm`` in PATH (the
same toolchain the Makefile already enforces).
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict


# Application layout mirrors source/memory_layout.ld
FLASH_ORIGIN = 0x27000
FLASH_LENGTH = 0xCE000
FLASH_LIMIT = FLASH_ORIGIN + FLASH_LENGTH  # 0xF5000, right before bootloader

RAM_ORIGIN = 0x200029A8
RAM_LIMIT = 0x20040000
RAM_LENGTH = RAM_LIMIT - RAM_ORIGIN

# Symbols we expect to find in the ELF (defined in section_layout.ld)
REQUIRED_SYMBOLS = {
	"__empty_flash_start",
	"__empty_flash_end",
	"__bond_storage_start",
	"__bond_storage_end",
	"__stack_bottom",
	"__stack_top",
	"__heap_start",
	"__heap_end",
	"__bss_start",
	"__bss_end",
	"__data_start",
	"__data_end",
}


@dataclass
class FlashReport:
	firmware_used: int
	filesystem_capacity: int
	bond_storage: int

	@property
	def total(self) -> int:  # type: ignore[override]
		return FLASH_LENGTH


@dataclass
class RamReport:
	data_bss: int
	stack_reserved: int
	heap_available: int

	@property
	def total(self) -> int:  # type: ignore[override]
		return RAM_LENGTH


def parse_args() -> argparse.Namespace:
	parser = argparse.ArgumentParser(description="Report flash/RAM usage from ELF")
	parser.add_argument(
		"--elf",
		default=Path("build") / "application.elf",
		type=Path,
		help="Path to the ELF image to analyse (default: build/application.elf)",
	)
	return parser.parse_args()


def ensure_tool_exists(tool: str) -> None:
	if shutil.which(tool) is None:
		sys.exit(
			f"Error: '{tool}' not found in PATH. Install the ARM GCC toolchain "
			"or export its bin directory before running this script."
		)


def collect_symbols(elf_path: Path) -> Dict[str, int]:
	ensure_tool_exists("arm-none-eabi-nm")
	cmd = ["arm-none-eabi-nm", "--defined-only", "--numeric-sort", str(elf_path)]
	try:
		result = subprocess.run(
			cmd,
			capture_output=True,
			text=True,
			check=True,
		)
	except subprocess.CalledProcessError as exc:  # pragma: no cover - passthrough
		sys.stderr.write(exc.stdout)
		sys.stderr.write(exc.stderr)
		sys.exit(exc.returncode)

	symbols: Dict[str, int] = {}
	for line in result.stdout.splitlines():
		parts = line.strip().split()
		if len(parts) < 3:
			continue
		address_str, _sym_type, name = parts[:3]
		if name in REQUIRED_SYMBOLS and name not in symbols:
			symbols[name] = int(address_str, 16)
		if len(symbols) == len(REQUIRED_SYMBOLS):
			break

	missing = REQUIRED_SYMBOLS - symbols.keys()
	if missing:
		missing_list = ", ".join(sorted(missing))
		sys.exit(
			f"Error: could not find the following linker symbols in {elf_path}:"
			f" {missing_list}. Ensure you built the application and that the ELF "
			"was linked with section_layout.ld."
		)
	return symbols


def compute_flash(symbols: Dict[str, int]) -> FlashReport:
	firmware_used = symbols["__empty_flash_start"] - FLASH_ORIGIN
	filesystem_capacity = symbols["__empty_flash_end"] - symbols["__empty_flash_start"]
	bond_storage = FLASH_LIMIT - symbols["__empty_flash_end"]
	return FlashReport(firmware_used, filesystem_capacity, bond_storage)


def compute_ram(symbols: Dict[str, int]) -> RamReport:
	data_bss = symbols["__stack_bottom"] - RAM_ORIGIN
	stack_reserved = symbols["__stack_top"] - symbols["__stack_bottom"]
	heap_available = symbols["__heap_end"] - symbols["__heap_start"]
	return RamReport(data_bss, stack_reserved, heap_available)


def fmt_bytes(value: int) -> str:
	if value >= 1024 * 1024:
		return f"{value / (1024 * 1024):.2f} MB"
	if value >= 1024:
		return f"{value / 1024:.2f} KB"
	return f"{value} B"


def pct(value: int, total: int) -> float:
	return (value / total * 100.0) if total else 0.0


def print_flash(report: FlashReport) -> None:
	print("FLASH (application region 0x27000-0xF5000 / {:,.0f} KB)".format(FLASH_LENGTH / 1024))
	print("  Firmware image : {:>10} ({:4.1f}% of region)".format(
		fmt_bytes(report.firmware_used), pct(report.firmware_used, report.total)
	))
	print("  Filesystem room: {:>10} ({:4.1f}% — available to LittleFS)".format(
		fmt_bytes(report.filesystem_capacity), pct(report.filesystem_capacity, report.total)
	))
	print("  Bond storage   : {:>10} ({:4.1f}% reserved)".format(
		fmt_bytes(report.bond_storage), pct(report.bond_storage, report.total)
	))
	print()


def print_ram(report: RamReport) -> None:
	print("RAM (application region 0x200029A8-0x20040000 / {:,.0f} KB)".format(RAM_LENGTH / 1024))
	print("  .data + .bss   : {:>10} ({:4.1f}% of region)".format(
		fmt_bytes(report.data_bss), pct(report.data_bss, report.total)
	))
	print("  Stack reserve  : {:>10} ({:4.1f}%)".format(
		fmt_bytes(report.stack_reserved), pct(report.stack_reserved, report.total)
	))
	print("  Heap available : {:>10} ({:4.1f}% free for malloc/Lua/TFLM)".format(
		fmt_bytes(report.heap_available), pct(report.heap_available, report.total)
	))
	print()


def main() -> None:
	args = parse_args()
	elf_path: Path = args.elf

	if not elf_path.exists():
		sys.exit(f"Error: {elf_path} does not exist. Build the application first.")

	symbols = collect_symbols(elf_path)
	flash_report = compute_flash(symbols)
	ram_report = compute_ram(symbols)

	print("====== FRAME MEMORY REPORT ======")
	print(f"ELF: {elf_path}")
	print()
	print_flash(flash_report)
	print_ram(ram_report)

	data_size = symbols["__data_end"] - symbols["__data_start"]
	bss_size = symbols["__bss_end"] - symbols["__bss_start"]
	print("Detailed sections:")
	print(f"  .text + .rodata : {fmt_bytes(flash_report.firmware_used)} (in flash)")
	print(f"  .data           : {fmt_bytes(data_size)} (in RAM, backed by flash)")
	print(f"  .bss            : {fmt_bytes(bss_size)} (zeroed RAM)")
	print()


if __name__ == "__main__":
	main()