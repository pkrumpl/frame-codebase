#!/usr/bin/env python3
"""
Flash Memory Analyzer for ARM Embedded Projects

Analyzes object files to show memory usage breakdown by section (.text, .data, .bss)
and identifies the largest contributors.

Usage:
    python flash_analyzer.py [build_dir] [--output report.html]
"""

import subprocess
import os
import sys
import argparse
from pathlib import Path
from dataclasses import dataclass
from typing import List, Dict, Tuple
import html


@dataclass
class ObjectFileInfo:
    name: str
    text: int
    data: int
    bss: int

    @property
    def total(self) -> int:
        return self.text + self.data + self.bss

    @property
    def flash(self) -> int:
        """Flash usage = text + data (bss is RAM only)"""
        return self.text + self.data


@dataclass
class SymbolInfo:
    name: str
    size: int
    symbol_type: str
    object_file: str


def format_size(size_bytes: int) -> str:
    """Format size in human-readable format."""
    if size_bytes >= 1024 * 1024:
        return f"{size_bytes / (1024 * 1024):.2f} MB"
    elif size_bytes >= 1024:
        return f"{size_bytes / 1024:.1f} KB"
    else:
        return f"{size_bytes} B"


def run_command(cmd: List[str]) -> str:
    """Run a command and return stdout."""
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return result.stdout
    except subprocess.CalledProcessError as e:
        return ""
    except FileNotFoundError:
        print(f"Error: Command '{cmd[0]}' not found. Make sure ARM toolchain is in PATH.")
        sys.exit(1)


def parse_size_output(output: str) -> List[ObjectFileInfo]:
    """Parse output from arm-none-eabi-size."""
    results = []
    lines = output.strip().split('\n')

    for line in lines[1:]:  # Skip header
        parts = line.split()
        if len(parts) >= 6:
            try:
                text = int(parts[0])
                data = int(parts[1])
                bss = int(parts[2])
                name = parts[5]
                results.append(ObjectFileInfo(name, text, data, bss))
            except (ValueError, IndexError):
                continue

    return results


def parse_nm_output(output: str, obj_file: str) -> List[SymbolInfo]:
    """Parse output from arm-none-eabi-nm --size-sort -S."""
    results = []

    for line in output.strip().split('\n'):
        parts = line.split()
        if len(parts) >= 4:
            try:
                size = int(parts[1], 16)
                sym_type = parts[2]
                name = parts[3]
                results.append(SymbolInfo(name, size, sym_type, obj_file))
            except (ValueError, IndexError):
                continue

    return results


def get_symbol_category(sym_type: str) -> str:
    """Categorize symbol type into text/data/bss."""
    if sym_type in ['t', 'T']:
        return 'text'
    elif sym_type in ['r', 'R']:
        return 'rodata'  # Read-only data (goes to flash)
    elif sym_type in ['d', 'D']:
        return 'data'
    elif sym_type in ['b', 'B']:
        return 'bss'
    else:
        return 'other'


def categorize_object_file(name: str) -> str:
    """Categorize object file by library/component."""
    # Extract just the filename (not full path)
    basename = Path(name).name
    name_lower = basename.lower()

    # Model data - check FIRST before TFLM (models may have tflm-like names)
    if any(x in name_lower for x in ['person_detect', 'fomo', '_model_data', 'model_data']):
        return 'ML Models'

    # TFLM Kernels (operation implementations)
    tflm_kernel_names = ['conv.o', 'depthwise_conv.o', 'pooling.o', 'softmax.o', 'reshape.o',
                         'add.o', 'pad.o', 'activations.o', 'reduce.o', 'fully_connected.o',
                         'conv_common.o', 'depthwise_conv_common.o', 'pooling_common.o',
                         'softmax_common.o', 'reshape_common.o', 'add_common.o', 'pad_common.o',
                         'activations_common.o', 'reduce_common.o', 'fully_connected_common.o',
                         'kernel_util.o', 'kernel_util_micro.o', 'kernel_util_kernels.o']
    if name_lower in tflm_kernel_names or 'kernel' in name_lower:
        return 'TFLM Kernels'

    # TFLM Core (infrastructure, not kernels)
    tflm_core_patterns = ['micro_interpreter', 'micro_allocator', 'micro_context', 'micro_log',
                          'micro_profiler', 'micro_resource', 'micro_utils', 'memory_helpers',
                          'micro_op_resolver', 'flatbuffer', 'schema', 'tensor_utils',
                          'quantization_util', 'portable_tensor', 'runtime_shape',
                          'arena_allocator', 'memory_planner', 'micro_allocation',
                          'error_reporter', 'common_core', 'common_kernels', 'system_setup',
                          'debug_log', 'micro_time']
    if any(p in name_lower for p in tflm_core_patterns):
        return 'TFLM Core'

    # CMSIS-NN categories
    if name_lower.startswith('arm_'):
        if 'conv' in name_lower:
            return 'CMSIS-NN Conv'
        elif 'pool' in name_lower:
            return 'CMSIS-NN Pool'
        elif 'softmax' in name_lower:
            return 'CMSIS-NN Softmax'
        elif 'fully_connected' in name_lower or 'fc' in name_lower:
            return 'CMSIS-NN FullyConn'
        elif 'nn_' in name_lower or 'nntables' in name_lower:
            return 'CMSIS-NN Support'
        else:
            return 'CMSIS-NN Other'

    # TFLM wrapper (contains model data embedded via #include)
    if 'tflm_wrapper' in name_lower:
        return 'ML Models (in tflm_wrapper)'

    # FPGA
    if 'fpga' in name_lower:
        return 'FPGA Bitstream'

    # Lua
    if name_lower.startswith('l') and name_lower[1:2].isalpha() and len(name_lower) < 15:
        if name_lower in ['lapi.o', 'lauxlib.o', 'lbaselib.o', 'lcode.o', 'lcorolib.o',
                          'lctype.o', 'ldblib.o', 'ldebug.o', 'ldo.o', 'ldump.o',
                          'lfunc.o', 'lgc.o', 'linit.o', 'llex.o', 'lmathlib.o',
                          'lmem.o', 'loadlib.o', 'lobject.o', 'lopcodes.o', 'lparser.o',
                          'lstate.o', 'lstring.o', 'lstrlib.o', 'ltable.o', 'ltablib.o',
                          'ltm.o', 'lundump.o', 'lutf8lib.o', 'lvm.o', 'lzio.o']:
            return 'Lua Runtime'

    # Application code
    if name_lower in ['main.o', 'bluetooth_app.o', 'bluetooth_lua.o', 'camera.o',
                      'display.o', 'experiment.o', 'file.o', 'imu.o', 'led.o',
                      'microphone.o', 'system.o', 'time.o', 'version.o', 'flash.o',
                      'luaport.o', 'spi.o', 'watchdog.o', 'boot_safety.o',
                      'compression_app.o', 'compression_lua.o']:
        return 'Application'

    # nRF/Nordic
    if 'nrf' in name_lower or 'system_nrf' in name_lower:
        return 'nRF Drivers'

    # Other libraries
    if 'lfs' in name_lower or 'littlefs' in name_lower:
        return 'LittleFS'
    if 'lz4' in name_lower:
        return 'LZ4'
    if 'segger' in name_lower or 'rtt' in name_lower:
        return 'SEGGER RTT'
    if 'tjpg' in name_lower:
        return 'TJpgDec'

    return 'Other'


@dataclass
class CategoryInfo:
    name: str
    text: int = 0
    data: int = 0
    bss: int = 0
    file_count: int = 0

    @property
    def flash(self) -> int:
        return self.text + self.data

    @property
    def ram(self) -> int:
        return self.data + self.bss


def parse_map_file(map_path: Path) -> Tuple[List[ObjectFileInfo], List[SymbolInfo], Dict[str, int]]:
    """Parse linker map file to extract size information."""
    import re

    obj_sizes: Dict[str, ObjectFileInfo] = {}
    symbols: List[SymbolInfo] = []
    section_totals: Dict[str, int] = {'text': 0, 'data': 0, 'bss': 0}

    try:
        content = map_path.read_text(encoding='utf-8', errors='ignore')
    except Exception as e:
        print(f"Error reading map file: {e}")
        return [], [], section_totals

    # First, extract top-level section sizes
    # Format: .text           0x00027000    0xd8fd4
    section_pattern = re.compile(r'^(\.text|\.rodata|\.data|\.bss)\s+0x[0-9a-fA-F]+\s+(0x[0-9a-fA-F]+)')
    for line in content.split('\n'):
        match = section_pattern.match(line)
        if match:
            section_name = match.group(1)
            size = int(match.group(2), 16)
            if section_name in ['.text', '.rodata']:
                section_totals['text'] += size
            elif section_name == '.data':
                section_totals['data'] = size
            elif section_name == '.bss':
                section_totals['bss'] = size

    # Define valid sections (stop when we hit debug or other non-code sections)
    valid_sections = {'.text', '.rodata', '.data', '.bss'}
    stop_sections = {'.debug', '.comment', '.stack', '.ARM', '.glue', '.vfp', '.iplt', '.rel', '.igot'}

    # Parse individual function/symbol contributions
    current_section = None
    lines = content.split('\n')

    for i, line in enumerate(lines):
        stripped = line.strip()

        # Check if we hit a stop section
        for stop_sec in stop_sections:
            if stripped.startswith(stop_sec):
                current_section = None
                break

        # Detect section from main section headers (not subsections)
        # Format: .text           0xADDRESS    0xSIZE
        if stripped.startswith('.text') and not stripped.startswith('.text.'):
            current_section = 'text'
            continue
        elif stripped.startswith('.rodata') and not stripped.startswith('.rodata.'):
            current_section = 'text'  # rodata is flash
            continue
        elif stripped.startswith('.data') and not stripped.startswith('.data.'):
            current_section = 'data'
            continue
        elif stripped.startswith('.bss') and not stripped.startswith('.bss.'):
            current_section = 'bss'
            continue

        # Track subsections within main sections
        if current_section:
            if stripped.startswith('.text.') or stripped.startswith('.rodata.'):
                current_section = 'text'
            elif stripped.startswith('.data.'):
                current_section = 'data'
            elif stripped.startswith('.bss.'):
                current_section = 'bss'

        # Parse contribution lines: starts with spaces, has 0x addresses, has object file
        if current_section and line.startswith(' ') and '0x' in line and '.o' in line:
            parts = stripped.split()
            if len(parts) >= 2:
                try:
                    addr = None
                    size = None
                    obj_file = None

                    for part in parts:
                        if part.startswith('0x'):
                            if addr is None:
                                addr = part
                            elif size is None:
                                size = int(part, 16)
                        elif '.o' in part or '.a(' in part:
                            if '.a(' in part:
                                match = re.search(r'\(([^)]+\.o)\)', part)
                                if match:
                                    obj_file = match.group(1)
                            else:
                                obj_file = Path(part).name

                    if size and size > 0 and obj_file:
                        if obj_file not in obj_sizes:
                            obj_sizes[obj_file] = ObjectFileInfo(obj_file, 0, 0, 0)

                        if current_section == 'text':
                            obj_sizes[obj_file].text += size
                        elif current_section == 'data':
                            obj_sizes[obj_file].data += size
                        elif current_section == 'bss':
                            obj_sizes[obj_file].bss += size

                except (ValueError, IndexError):
                    continue

    return list(obj_sizes.values()), symbols, section_totals


def analyze_build_directory(build_dir: Path) -> Tuple[List[ObjectFileInfo], List[SymbolInfo], Dict[str, CategoryInfo]]:
    """Analyze all .o files in the build directory."""
    obj_files = list(build_dir.rglob("*.o"))

    if not obj_files:
        print(f"No .o files found in {build_dir}")
        sys.exit(1)

    print(f"Found {len(obj_files)} object files...")

    # Get size info for all object files
    size_output = run_command(["arm-none-eabi-size"] + [str(f) for f in obj_files])
    obj_infos = parse_size_output(size_output)

    # Check if LTO is enabled (all objects show 0 bytes)
    total_size = sum(o.text + o.data + o.bss for o in obj_infos)
    section_totals = None
    lto_enabled = False
    if total_size == 0:
        lto_enabled = True
        print("WARNING: Object files show 0 bytes - LTO (-flto) is likely enabled.")
        print("         Per-library breakdown unavailable (code merged by linker).")
        print("Attempting to parse map file instead...")

        # Try to parse map file
        map_file = build_dir / "application.map"
        if map_file.exists():
            print(f"Found map file: {map_file}")
            obj_infos, _, section_totals = parse_map_file(map_file)
            if obj_infos:
                print(f"Extracted size info for {len(obj_infos)} objects from map file")
            if section_totals:
                print(f"Section totals: .text={section_totals['text']}, .data={section_totals['data']}, .bss={section_totals['bss']}")
                # Use section totals for accurate reporting
                total_text = section_totals['text']
                total_data = section_totals['data']
                total_bss = section_totals['bss']
        else:
            print(f"No map file found at {map_file}")

    # Categorize objects by library
    categories: Dict[str, CategoryInfo] = {}
    for obj in obj_infos:
        cat_name = categorize_object_file(obj.name)
        if cat_name not in categories:
            categories[cat_name] = CategoryInfo(name=cat_name)
        cat = categories[cat_name]
        cat.text += obj.text
        cat.data += obj.data
        cat.bss += obj.bss
        cat.file_count += 1

    # Get symbol info for largest objects (skip if LTO)
    all_symbols = []
    if total_size > 0:  # Only if not using LTO
        for obj_file in obj_files:
            nm_output = run_command(["arm-none-eabi-nm", "--size-sort", "-S", str(obj_file)])
            symbols = parse_nm_output(nm_output, obj_file.name)
            all_symbols.extend(symbols)

    return obj_infos, all_symbols, categories, lto_enabled, section_totals


def generate_html_report(
    obj_infos: List[ObjectFileInfo],
    symbols: List[SymbolInfo],
    categories: Dict[str, CategoryInfo],
    flash_available: int = 824 * 1024,
    ram_available: int = 245 * 1024,
    lto_enabled: bool = False,
    section_totals: Dict[str, int] = None
) -> str:
    """Generate HTML report."""

    # Calculate totals - prefer section_totals from map file when available (more accurate with LTO)
    if section_totals:
        total_text = section_totals['text']
        total_data = section_totals['data']
        total_bss = section_totals['bss']
    else:
        total_text = sum(o.text for o in obj_infos)
        total_data = sum(o.data for o in obj_infos)
        total_bss = sum(o.bss for o in obj_infos)

    total_flash = total_text + total_data
    total_ram = total_data + total_bss

    # Sort categories by flash usage
    sorted_categories = sorted(categories.values(), key=lambda x: x.flash, reverse=True)
    max_cat_flash = max(c.flash for c in sorted_categories) if sorted_categories else 1

    # Calculate TFLM and CMSIS-NN totals
    tflm_total = sum(c.flash for c in categories.values() if 'TFLM' in c.name)
    cmsis_total = sum(c.flash for c in categories.values() if 'CMSIS' in c.name)

    # Sort objects by each section
    by_text = sorted(obj_infos, key=lambda x: x.text, reverse=True)[:10]
    by_data = sorted(obj_infos, key=lambda x: x.data, reverse=True)[:10]
    by_bss = sorted(obj_infos, key=lambda x: x.bss, reverse=True)[:10]
    by_flash = sorted(obj_infos, key=lambda x: x.flash, reverse=True)[:10]

    # Sort symbols by size and category
    text_symbols = sorted([s for s in symbols if s.symbol_type in ['t', 'T']],
                          key=lambda x: x.size, reverse=True)[:15]
    rodata_symbols = sorted([s for s in symbols if s.symbol_type in ['r', 'R']],
                            key=lambda x: x.size, reverse=True)[:15]
    data_symbols = sorted([s for s in symbols if s.symbol_type in ['d', 'D']],
                          key=lambda x: x.size, reverse=True)[:10]
    bss_symbols = sorted([s for s in symbols if s.symbol_type in ['b', 'B']],
                         key=lambda x: x.size, reverse=True)[:10]

    # Calculate percentages
    flash_pct = (total_flash / flash_available) * 100 if flash_available else 0
    ram_pct = (total_ram / ram_available) * 100 if ram_available else 0

    flash_status = "over" if total_flash > flash_available else "ok"
    ram_status = "over" if total_ram > ram_available else "ok"

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Flash Memory Analysis Report</title>
    <style>
        :root {{
            --bg-color: #1a1a2e;
            --card-bg: #16213e;
            --text-color: #eee;
            --accent-green: #4ade80;
            --accent-red: #f87171;
            --accent-blue: #60a5fa;
            --accent-yellow: #fbbf24;
            --accent-purple: #a78bfa;
        }}

        * {{ box-sizing: border-box; margin: 0; padding: 0; }}

        body {{
            font-family: 'Segoe UI', system-ui, sans-serif;
            background: var(--bg-color);
            color: var(--text-color);
            padding: 20px;
            line-height: 1.6;
        }}

        h1 {{
            text-align: center;
            margin-bottom: 30px;
            color: var(--accent-blue);
        }}

        h2 {{
            color: var(--accent-purple);
            margin: 20px 0 15px 0;
            padding-bottom: 5px;
            border-bottom: 2px solid var(--accent-purple);
        }}

        h3 {{
            color: var(--accent-yellow);
            margin: 15px 0 10px 0;
        }}

        .summary {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
            gap: 20px;
            margin-bottom: 30px;
        }}

        .card {{
            background: var(--card-bg);
            border-radius: 12px;
            padding: 20px;
            box-shadow: 0 4px 6px rgba(0, 0, 0, 0.3);
        }}

        .card h3 {{
            margin-top: 0;
        }}

        .metric {{
            font-size: 2em;
            font-weight: bold;
            margin: 10px 0;
        }}

        .metric.ok {{ color: var(--accent-green); }}
        .metric.over {{ color: var(--accent-red); }}

        .progress-bar {{
            background: #333;
            border-radius: 10px;
            height: 20px;
            overflow: hidden;
            margin: 10px 0;
        }}

        .progress-fill {{
            height: 100%;
            border-radius: 10px;
            transition: width 0.3s;
        }}

        .progress-fill.ok {{ background: linear-gradient(90deg, var(--accent-green), #22c55e); }}
        .progress-fill.over {{ background: linear-gradient(90deg, var(--accent-red), #dc2626); }}

        table {{
            width: 100%;
            border-collapse: collapse;
            margin: 15px 0;
            background: var(--card-bg);
            border-radius: 8px;
            overflow: hidden;
        }}

        th, td {{
            padding: 12px 15px;
            text-align: left;
            border-bottom: 1px solid #333;
        }}

        th {{
            background: rgba(255, 255, 255, 0.1);
            font-weight: 600;
            color: var(--accent-blue);
        }}

        tr:hover {{
            background: rgba(255, 255, 255, 0.05);
        }}

        .size-bar {{
            background: #333;
            border-radius: 4px;
            height: 8px;
            min-width: 100px;
        }}

        .size-bar-fill {{
            height: 100%;
            border-radius: 4px;
            background: var(--accent-blue);
        }}

        .symbol-name {{
            font-family: 'Consolas', 'Monaco', monospace;
            font-size: 0.9em;
            word-break: break-all;
        }}

        .legend {{
            display: flex;
            gap: 20px;
            flex-wrap: wrap;
            margin: 15px 0;
            padding: 10px;
            background: rgba(255,255,255,0.05);
            border-radius: 8px;
        }}

        .legend-item {{
            display: flex;
            align-items: center;
            gap: 8px;
        }}

        .legend-color {{
            width: 16px;
            height: 16px;
            border-radius: 4px;
        }}

        .grid-2 {{
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(400px, 1fr));
            gap: 20px;
        }}

        .note {{
            background: rgba(251, 191, 36, 0.1);
            border-left: 4px solid var(--accent-yellow);
            padding: 15px;
            margin: 20px 0;
            border-radius: 0 8px 8px 0;
        }}
    </style>
</head>
<body>
    <h1>Flash Memory Analysis Report</h1>

    <div class="summary">
        <div class="card">
            <h3>Flash Usage (text + data)</h3>
            <div class="metric {flash_status}">{format_size(total_flash)}</div>
            <div>of {format_size(flash_available)} available</div>
            <div class="progress-bar">
                <div class="progress-fill {flash_status}" style="width: {min(flash_pct, 100):.1f}%"></div>
            </div>
            <div>{flash_pct:.1f}% used {f"(overflow: {format_size(total_flash - flash_available)})" if total_flash > flash_available else ""}</div>
        </div>

        <div class="card">
            <h3>RAM Usage (data + bss)</h3>
            <div class="metric {ram_status}">{format_size(total_ram)}</div>
            <div>of {format_size(ram_available)} available</div>
            <div class="progress-bar">
                <div class="progress-fill {ram_status}" style="width: {min(ram_pct, 100):.1f}%"></div>
            </div>
            <div>{ram_pct:.1f}% used {f"(overflow: {format_size(total_ram - ram_available)})" if total_ram > ram_available else ""}</div>
        </div>

        <div class="card">
            <h3>Section Breakdown</h3>
            <table>
                <tr><td>.text (code + rodata)</td><td><strong>{format_size(total_text)}</strong></td></tr>
                <tr><td>.data (initialized)</td><td><strong>{format_size(total_data)}</strong></td></tr>
                <tr><td>.bss (uninitialized)</td><td><strong>{format_size(total_bss)}</strong></td></tr>
            </table>
        </div>
    </div>

    <div class="note">
        <strong>Note:</strong> These are pre-linking sizes. The linker removes unused code (-ffunction-sections -fdata-sections + --gc-sections),
        so actual flash usage may be lower. However, large constants (models, FPGA bitstreams) cannot be optimized away.
    </div>

    <h2>Library/Component Breakdown</h2>
    <table>
        <tr>
            <th>Component</th>
            <th>Files</th>
            <th>.text</th>
            <th>.data</th>
            <th>.bss</th>
            <th>Flash</th>
            <th>% of Flash</th>
            <th>Visual</th>
        </tr>
        {"".join(f'''
        <tr>
            <td><strong>{html.escape(cat.name)}</strong></td>
            <td>{cat.file_count}</td>
            <td>{format_size(cat.text)}</td>
            <td>{format_size(cat.data)}</td>
            <td>{format_size(cat.bss)}</td>
            <td><strong>{format_size(cat.flash)}</strong></td>
            <td>{(cat.flash / total_flash * 100) if total_flash > 0 else 0:.1f}%</td>
            <td><div class="size-bar"><div class="size-bar-fill" style="width: {(cat.flash / max_cat_flash * 100) if max_cat_flash > 0 else 0:.1f}%"></div></div></td>
        </tr>''' for cat in sorted_categories)}
        <tr style="background: rgba(255,255,255,0.1); font-weight: bold;">
            <td>TOTAL</td>
            <td>{sum(c.file_count for c in sorted_categories)}</td>
            <td>{format_size(sum(c.text for c in sorted_categories))}</td>
            <td>{format_size(sum(c.data for c in sorted_categories))}</td>
            <td>{format_size(sum(c.bss for c in sorted_categories))}</td>
            <td>{format_size(sum(c.flash for c in sorted_categories))}</td>
            <td>100%</td>
            <td></td>
        </tr>
    </table>

    <h3>TFLM + CMSIS-NN Combined</h3>
    <div class="card" style="max-width: 600px;">
        <table>
            <tr><td>TFLM Total</td><td><strong>{format_size(tflm_total)}</strong></td><td>{(tflm_total / total_flash * 100) if total_flash > 0 else 0:.1f}% of flash</td></tr>
            <tr><td>CMSIS-NN Total</td><td><strong>{format_size(cmsis_total)}</strong></td><td>{(cmsis_total / total_flash * 100) if total_flash > 0 else 0:.1f}% of flash</td></tr>
            <tr style="background: rgba(255,255,255,0.1);"><td><strong>ML Stack Total</strong></td><td><strong>{format_size(tflm_total + cmsis_total)}</strong></td><td><strong>{((tflm_total + cmsis_total) / total_flash * 100) if total_flash > 0 else 0:.1f}% of flash</strong></td></tr>
        </table>
    </div>

    <h2>Top 10 Object Files by Flash Usage</h2>
    <table>
        <tr>
            <th>Object File</th>
            <th>.text</th>
            <th>.data</th>
            <th>Flash Total</th>
            <th>Visual</th>
        </tr>
        {"".join(f'''
        <tr>
            <td>{html.escape(o.name)}</td>
            <td>{format_size(o.text)}</td>
            <td>{format_size(o.data)}</td>
            <td><strong>{format_size(o.flash)}</strong></td>
            <td><div class="size-bar"><div class="size-bar-fill" style="width: {(o.flash / by_flash[0].flash * 100) if by_flash and by_flash[0].flash > 0 else 0:.1f}%"></div></div></td>
        </tr>''' for o in by_flash)}
    </table>

    <h2>Top 10 Object Files by RAM Usage (.bss)</h2>
    <table>
        <tr>
            <th>Object File</th>
            <th>.bss</th>
            <th>.data</th>
            <th>RAM Total</th>
            <th>Visual</th>
        </tr>
        {"".join(f'''
        <tr>
            <td>{html.escape(o.name)}</td>
            <td>{format_size(o.bss)}</td>
            <td>{format_size(o.data)}</td>
            <td><strong>{format_size(o.bss + o.data)}</strong></td>
            <td><div class="size-bar"><div class="size-bar-fill" style="width: {((o.bss + o.data) / (by_bss[0].bss + by_bss[0].data) * 100) if by_bss and (by_bss[0].bss + by_bss[0].data) > 0 else 0:.1f}%"></div></div></td>
        </tr>''' for o in by_bss if o.bss > 0)}
    </table>

    <h2>Largest Symbols</h2>

    <div class="legend">
        <div class="legend-item"><div class="legend-color" style="background: var(--accent-green)"></div> t/T = Code (.text)</div>
        <div class="legend-item"><div class="legend-color" style="background: var(--accent-blue)"></div> r/R = Read-only data (.rodata)</div>
        <div class="legend-item"><div class="legend-color" style="background: var(--accent-yellow)"></div> d/D = Initialized data (.data)</div>
        <div class="legend-item"><div class="legend-color" style="background: var(--accent-purple)"></div> b/B = Uninitialized data (.bss)</div>
    </div>

    <div class="grid-2">
        <div>
            <h3>Largest Read-Only Data (Flash)</h3>
            <table>
                <tr><th>Symbol</th><th>Size</th><th>Object</th></tr>
                {"".join(f'''
                <tr>
                    <td class="symbol-name">{html.escape(s.name[:50])}</td>
                    <td><strong>{format_size(s.size)}</strong></td>
                    <td>{html.escape(s.object_file)}</td>
                </tr>''' for s in rodata_symbols)}
            </table>
        </div>

        <div>
            <h3>Largest Code Functions</h3>
            <table>
                <tr><th>Symbol</th><th>Size</th><th>Object</th></tr>
                {"".join(f'''
                <tr>
                    <td class="symbol-name">{html.escape(s.name[:50])}</td>
                    <td><strong>{format_size(s.size)}</strong></td>
                    <td>{html.escape(s.object_file)}</td>
                </tr>''' for s in text_symbols)}
            </table>
        </div>
    </div>

    <div class="grid-2">
        <div>
            <h3>Largest BSS (RAM, uninitialized)</h3>
            <table>
                <tr><th>Symbol</th><th>Size</th><th>Object</th></tr>
                {"".join(f'''
                <tr>
                    <td class="symbol-name">{html.escape(s.name[:50])}</td>
                    <td><strong>{format_size(s.size)}</strong></td>
                    <td>{html.escape(s.object_file)}</td>
                </tr>''' for s in bss_symbols)}
            </table>
        </div>

        <div>
            <h3>Largest Initialized Data</h3>
            <table>
                <tr><th>Symbol</th><th>Size</th><th>Object</th></tr>
                {"".join(f'''
                <tr>
                    <td class="symbol-name">{html.escape(s.name[:50])}</td>
                    <td><strong>{format_size(s.size)}</strong></td>
                    <td>{html.escape(s.object_file)}</td>
                </tr>''' for s in data_symbols) if data_symbols else "<tr><td colspan='3'>No significant initialized data</td></tr>"}
            </table>
        </div>
    </div>

    <h2>All Object Files</h2>
    <table>
        <tr>
            <th>Object File</th>
            <th>.text</th>
            <th>.data</th>
            <th>.bss</th>
            <th>Flash</th>
        </tr>
        {"".join(f'''
        <tr>
            <td>{html.escape(o.name)}</td>
            <td>{format_size(o.text)}</td>
            <td>{format_size(o.data)}</td>
            <td>{format_size(o.bss)}</td>
            <td>{format_size(o.flash)}</td>
        </tr>''' for o in sorted(obj_infos, key=lambda x: x.flash, reverse=True))}
        <tr style="background: rgba(255,255,255,0.1); font-weight: bold;">
            <td>TOTAL</td>
            <td>{format_size(total_text)}</td>
            <td>{format_size(total_data)}</td>
            <td>{format_size(total_bss)}</td>
            <td>{format_size(total_flash)}</td>
        </tr>
    </table>

    <footer style="text-align: center; margin-top: 40px; color: #666;">
        Generated by flash_analyzer.py
    </footer>
</body>
</html>
"""
    return html_content


def main():
    parser = argparse.ArgumentParser(description="Analyze flash memory usage from object files")
    parser.add_argument("build_dir", nargs="?", default="build",
                        help="Build directory containing .o files (default: build)")
    parser.add_argument("-o", "--output", default="flash_report.html",
                        help="Output HTML file (default: flash_report.html)")
    parser.add_argument("--flash", type=int, default=824,
                        help="Available flash in KB (default: 824)")
    parser.add_argument("--ram", type=int, default=245,
                        help="Available RAM in KB (default: 245)")

    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.exists():
        print(f"Error: Build directory '{build_dir}' not found")
        sys.exit(1)

    print(f"Analyzing object files in {build_dir}...")
    obj_infos, symbols, categories, lto_enabled, section_totals = analyze_build_directory(build_dir)

    print(f"Generating report...")
    html_report = generate_html_report(
        obj_infos,
        symbols,
        categories,
        flash_available=args.flash * 1024,
        ram_available=args.ram * 1024,
        lto_enabled=lto_enabled,
        section_totals=section_totals
    )

    output_path = Path(args.output)
    output_path.write_text(html_report, encoding='utf-8')

    print(f"Report saved to: {output_path.absolute()}")

    # Print quick summary - use section_totals when available (more accurate with LTO)
    if section_totals:
        total_text = section_totals['text']
        total_data = section_totals['data']
        total_bss = section_totals['bss']
    else:
        total_text = sum(o.text for o in obj_infos)
        total_data = sum(o.data for o in obj_infos)
        total_bss = sum(o.bss for o in obj_infos)

    total_flash = total_text + total_data

    print(f"\n{'='*50}")
    print(f"SUMMARY")
    print(f"{'='*50}")
    print(f"Flash: {format_size(total_flash)} / {format_size(args.flash * 1024)}")
    print(f"RAM:   {format_size(total_data + total_bss)} / {format_size(args.ram * 1024)}")

    if total_flash > args.flash * 1024:
        print(f"\n[WARNING] Flash overflow: {format_size(total_flash - args.flash * 1024)}")

    # Print library breakdown
    print(f"\n{'='*50}")
    print(f"LIBRARY BREAKDOWN")
    print(f"{'='*50}")

    if lto_enabled:
        print("  (Per-library breakdown unavailable with LTO)")
    else:
        sorted_cats = sorted(categories.values(), key=lambda x: x.flash, reverse=True)
        for cat in sorted_cats:
            pct = (cat.flash / total_flash * 100) if total_flash > 0 else 0
            print(f"  {cat.name:20s} {format_size(cat.flash):>10s} ({pct:5.1f}%)")

        tflm_total = sum(c.flash for c in categories.values() if 'TFLM' in c.name)
        cmsis_total = sum(c.flash for c in categories.values() if 'CMSIS' in c.name)
        print(f"\n  {'TFLM Total':20s} {format_size(tflm_total):>10s}")
        print(f"  {'CMSIS-NN Total':20s} {format_size(cmsis_total):>10s}")
        print(f"  {'ML Stack Total':20s} {format_size(tflm_total + cmsis_total):>10s}")


if __name__ == "__main__":
    main()
