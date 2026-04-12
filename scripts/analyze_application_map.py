#!/usr/bin/env python3
"""
Linker Map File Analyzer for ARM GCC
Parses a .map file and prints a concise overview of:
  - Memory region usage
  - Flash/RAM breakdown by section (.text, .data, .bss, .rodata)
  - Per-object-file sizes (grouped by category)
  - Discarded (garbage-collected) sections and savings
  - Top functions by size
  - Actionable recommendations
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

def parse_map_file(filepath):
    with open(filepath, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()
    
    lines = content.replace('\r\n', '\n').split('\n')
    
    # ── 1. Parse Memory Configuration ──
    memory_regions = {}
    in_memory_config = False
    for line in lines:
        if 'Memory Configuration' in line:
            in_memory_config = True
            continue
        if in_memory_config:
            if line.strip() == '' or 'Linker script' in line:
                if memory_regions:
                    break
                continue
            if line.startswith('Name') or line.startswith('*default*'):
                continue
            parts = line.split()
            if len(parts) >= 3:
                try:
                    name = parts[0]
                    origin = int(parts[1], 16)
                    length = int(parts[2], 16)
                    attrs = parts[3] if len(parts) > 3 else ''
                    memory_regions[name] = {
                        'origin': origin, 'length': length, 'attrs': attrs
                    }
                except ValueError:
                    pass
    
    # ── 2. Parse Discarded Sections ──
    discarded_by_obj = defaultdict(int)     # obj_file -> total discarded bytes
    discarded_sections = []                  # (section_name, size, obj_file)
    
    in_discarded = False
    discard_end = None
    for i, line in enumerate(lines):
        if 'Discarded input sections' in line:
            in_discarded = True
            continue
        if in_discarded and ('Memory Configuration' in line or 'Linker script' in line):
            in_discarded = False
            break
        if not in_discarded:
            continue
        
        # Match: " .text.func  0x00000000  0xSIZE  obj.o"
        m = re.match(
            r'\s+(\.[\w.]+)\s+0x[0-9a-f]+\s+(0x[0-9a-f]+)\s+(.*\.o\b.*)', line
        )
        if m:
            sec_name = m.group(1)
            size = int(m.group(2), 16)
            obj_file = m.group(3).strip()
            # Only count .text, .data, .bss, .rodata (skip debug sections)
            if any(sec_name.startswith(p) for p in ['.text', '.data', '.bss', '.rodata']):
                if size > 0:
                    discarded_by_obj[obj_file] += size
                    discarded_sections.append((sec_name, size, obj_file))
    
    # ── 3. Parse Allocated Sections ──
    allocated_by_obj = defaultdict(lambda: defaultdict(int))  # obj -> {section_type -> size}
    allocated_functions = []  # (func_name, size, obj_file)
    
    # Determine flash/ram address ranges from memory regions
    flash_start = memory_regions.get('APPLICATION_FLASH', {}).get('origin', 0x27000)
    flash_end = flash_start + memory_regions.get('APPLICATION_FLASH', {}).get('length', 0)
    ram_start = memory_regions.get('APPLICATION_RAM', {}).get('origin', 0x20000000)
    
    def classify_section(sec_name, addr):
        """Classify a section by its name first, then by address range."""
        if sec_name:
            if sec_name.startswith('.text') or sec_name.startswith('.isr'):
                return '.text'
            if sec_name.startswith('.rodata'):
                return '.rodata'
            if sec_name.startswith('.data'):
                return '.data'
            if sec_name.startswith('.bss'):
                return '.bss'
        # Fallback: classify by address
        if addr >= ram_start:
            return '.bss'  # RAM region (could be .data or .bss)
        if addr >= flash_start and addr < flash_end:
            return '.text'  # Flash region (could be .text or .rodata)
        return '.other'
    
    # Find start of linker map
    map_start = 0
    for i, line in enumerate(lines):
        if 'Linker script and memory map' in line:
            map_start = i
            break
    
    last_section_name = None  # Track the most recent named subsection
    skip_next = False
    
    for i in range(map_start, len(lines)):
        if skip_next:
            skip_next = False
            continue
        
        line = lines[i]
        
        # Skip debug sections entirely
        if '.debug_' in line or '.comment' in line or '.ARM.attributes' in line:
            last_section_name = None
            continue
        
        # Match named subsections: " .text.func  0xADDR  0xSIZE  obj.o"
        m = re.match(
            r'\s+(\.[\w.]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.*\.o\b.*)', line
        )
        if m:
            sec_name = m.group(1)
            addr = int(m.group(2), 16)
            size = int(m.group(3), 16)
            obj_file = m.group(4).strip()
            last_section_name = sec_name
            
            if addr > 0 and size > 0:
                sec_type = classify_section(sec_name, addr)
                allocated_by_obj[obj_file][sec_type] += size
                if sec_type == '.text' and size >= 16:
                    allocated_functions.append((sec_name, size, obj_file))
            continue
        
        # Match section name on its own line (split format):
        # " .rodata._ZL20person_detect_tflite"
        m2 = re.match(r'\s+(\.[\w.]+)\s*$', line)
        if m2:
            last_section_name = m2.group(1)
            # Look ahead for the continuation
            if i + 1 < len(lines):
                next_line = lines[i + 1]
                m3 = re.match(
                    r'\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.*\.o\b.*)', next_line
                )
                if m3:
                    addr = int(m3.group(1), 16)
                    size = int(m3.group(2), 16)
                    obj_file = m3.group(3).strip()
                    if addr > 0 and size > 0:
                        sec_type = classify_section(last_section_name, addr)
                        allocated_by_obj[obj_file][sec_type] += size
                        if sec_type == '.text' and size >= 16:
                            allocated_functions.append((last_section_name, size, obj_file))
                    skip_next = True
            continue
        
        # Match continuation lines (no section name, just addr/size/obj):
        # "                0xADDR  0xSIZE  obj.o"
        m4 = re.match(
            r'\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.*\.o\b.*)', line
        )
        if m4:
            addr = int(m4.group(1), 16)
            size = int(m4.group(2), 16)
            obj_file = m4.group(3).strip()
            if addr > 0 and size > 0:
                sec_type = classify_section(last_section_name, addr)
                allocated_by_obj[obj_file][sec_type] += size
                if sec_type == '.text' and size >= 16 and last_section_name:
                    allocated_functions.append((last_section_name, size, obj_file))
    
    return memory_regions, discarded_by_obj, discarded_sections, allocated_by_obj, allocated_functions


def short_name(obj_path):
    """Extract a readable short name from an object file path."""
    # Remove common prefixes and get just the filename
    name = obj_path.replace('\\', '/')
    name = name.split('/')[-1]
    # Remove .o extension
    name = re.sub(r'\.o\)?$', '', name)
    return name


def categorize(obj_path):
    """Categorize an object file into a human-readable group."""
    p = obj_path.replace('\\', '/').lower()
    name = p.split('/')[-1]
    
    # CMSIS-NN: match by path or by arm_* naming pattern typical of CMSIS-NN
    if 'cmsis_nn' in p:
        return 'CMSIS-NN'
    cmsis_prefixes = ['arm_convolve', 'arm_depthwise', 'arm_fully_connected',
                      'arm_avgpool', 'arm_max_pool', 'arm_softmax', 'arm_reshape',
                      'arm_pad', 'arm_svdf', 'arm_nn_', 'arm_elementwise',
                      'arm_concatenation', 'arm_relu', 'arm_batch_matmul',
                      'arm_transpose_conv', 'arm_vector_sum', 'arm_q7_to',
                      'arm_s8_to', 'arm_nntables']
    if any(name.startswith(prefix) for prefix in cmsis_prefixes):
        return 'CMSIS-NN'
    
    if 'tflm/' in p or 'tensorflow' in p:
        return 'TFLite Micro'
    # TFLite Micro kernel/runtime files compiled to build/obj/
    tflm_names = ['micro_interpreter', 'micro_allocator', 'micro_context',
                  'micro_op_resolver', 'micro_profiler', 'micro_log',
                  'micro_utils', 'micro_resource', 'micro_allocation',
                  'micro_interpreter_graph', 'micro_interpreter_context',
                  'micro_error_reporter', 'micro_time',
                  'flatbuffer_conversions', 'flatbuffer_utils',
                  'flatbuffer_conversions_bridge', 'schema_utils',
                  'tensor_utils', 'tensor_ctypes', 'runtime_shape',
                  'quantization_util', 'portable_tensor_utils',
                  'memory_helpers', 'memory_planner', 'greedy_memory',
                  'linear_memory', 'non_persistent', 'persistent_arena',
                  'single_arena', 'recording_single', 'recording_micro',
                  'fake_micro', 'system_setup', 'debug_log',
                  'common_core', 'common_kernels', 'kernel_util',
                  'conv.o', 'conv_common', 'depthwise_conv.o',
                  'depthwise_conv_common', 'fully_connected.o',
                  'fully_connected_common', 'pooling.o', 'pooling_common',
                  'softmax.o', 'softmax_common', 'reshape.o', 'reshape_common',
                  'add.o', 'add_common', 'pad.o', 'pad_common',
                  'activations.o', 'activations_common',
                  'reduce.o', 'reduce_common', 'error_reporter.o']
    if any(name.startswith(tn.replace('.o', '')) or name == tn for tn in tflm_names):
        return 'TFLite Micro'
    
    if 'lua/' in p or 'lua_libraries' in p:
        return 'Lua'
    # Lua files compiled to build/obj/
    lua_names = ['lapi', 'lauxlib', 'lbaselib', 'lcode', 'lcorolib', 'lctype',
                 'ldblib', 'ldebug', 'ldo', 'ldump', 'lfunc', 'lgc', 'linit',
                 'llex', 'lmathlib', 'lmem', 'loadlib', 'lobject', 'lopcodes',
                 'lparser', 'lstate', 'lstring', 'lstrlib', 'ltable', 'ltablib',
                 'ltm', 'lundump', 'lutf8lib', 'lvm', 'lzio', 'luaport']
    if any(name.startswith(ln + '.') or name == ln for ln in lua_names):
        return 'Lua'
    
    if 'littlefs' in p or name.startswith('lfs'):
        return 'LittleFS'
    if 'nrfx' in p or 'softdevice' in p or name.startswith('nrfx_'):
        return 'nRF SDK/Drivers'
    if 'picolibc' in p or 'libc.a' in p:
        return 'C Library'
    if 'libgcc' in p or 'libstdc++' in p or 'libm' in p:
        return 'GCC Runtime'
    if 'lz4' in p:
        return 'LZ4'
    if 'segger' in p or name.startswith('segger'):
        return 'SEGGER RTT'
    if 'tjpgd' in p:
        return 'JPEG Decoder'
    return 'Application'


def format_size(size_bytes):
    """Format bytes into human-readable KB."""
    if size_bytes >= 1024:
        return f"{size_bytes/1024:.1f} KB"
    return f"{size_bytes} B"


def print_report(memory_regions, discarded_by_obj, discarded_sections,
                 allocated_by_obj, allocated_functions):
    
    W = 80  # report width
    
    # ════════════════════════════════════════════
    # MEMORY REGIONS
    # ════════════════════════════════════════════
    print("=" * W)
    print("MEMORY REGION CONFIGURATION".center(W))
    print("=" * W)
    for name, info in memory_regions.items():
        if 'default' in name.lower():
            continue
        print(f"  {name:<28s} Origin: 0x{info['origin']:08X}   "
              f"Size: {format_size(info['length']):>10s}   [{info['attrs']}]")
    print()
    
    # ════════════════════════════════════════════
    # OVERALL SECTION TOTALS
    # ════════════════════════════════════════════
    section_totals = defaultdict(int)
    for obj, sections in allocated_by_obj.items():
        for sec_type, size in sections.items():
            section_totals[sec_type] += size
    
    flash_used = section_totals.get('.text', 0) + section_totals.get('.rodata', 0) + section_totals.get('.data', 0)
    ram_used = section_totals.get('.data', 0) + section_totals.get('.bss', 0)
    
    flash_region = memory_regions.get('APPLICATION_FLASH', {})
    ram_region = memory_regions.get('APPLICATION_RAM', {})
    flash_total = flash_region.get('length', 0)
    ram_total = ram_region.get('length', 0)
    
    print("=" * W)
    print("SECTION TOTALS".center(W))
    print("=" * W)
    for sec in ['.text', '.rodata', '.data', '.bss']:
        val = section_totals.get(sec, 0)
        dest = 'Flash' if sec in ['.text', '.rodata'] else ('Flash+RAM' if sec == '.data' else 'RAM')
        print(f"  {sec:<12s} {format_size(val):>12s}   ({dest})")
    
    print(f"  {'─'*40}")
    print(f"  {'Flash used':<12s} {format_size(flash_used):>12s}", end='')
    if flash_total:
        pct = flash_used / flash_total * 100
        remain = flash_total - flash_used
        print(f"   ({pct:.1f}% of {format_size(flash_total)}, {format_size(remain)} remaining)")
    else:
        print()
    
    print(f"  {'RAM used':<12s} {format_size(ram_used):>12s}", end='')
    if ram_total:
        pct = ram_used / ram_total * 100
        remain = ram_total - ram_used
        print(f"   ({pct:.1f}% of {format_size(ram_total)}, {format_size(remain)} remaining)")
    else:
        print()
    print()
    
    # ════════════════════════════════════════════
    # SIZE BY CATEGORY
    # ════════════════════════════════════════════
    cat_sizes = defaultdict(lambda: defaultdict(int))
    for obj, sections in allocated_by_obj.items():
        cat = categorize(obj)
        for sec_type, size in sections.items():
            cat_sizes[cat][sec_type] += size
    
    # Sort by total size descending
    cat_totals = {}
    for cat, sections in cat_sizes.items():
        cat_totals[cat] = sum(sections.values())
    sorted_cats = sorted(cat_totals.items(), key=lambda x: -x[1])
    
    print("=" * W)
    print("SIZE BY CATEGORY".center(W))
    print("=" * W)
    print(f"  {'Category':<20s} {'Total':>10s}  {'.text':>10s}  {'.rodata':>10s}  {'.data':>8s}  {'.bss':>8s}")
    print(f"  {'─'*70}")
    for cat, total in sorted_cats:
        s = cat_sizes[cat]
        print(f"  {cat:<20s} {format_size(total):>10s}  "
              f"{format_size(s.get('.text',0)):>10s}  "
              f"{format_size(s.get('.rodata',0)):>10s}  "
              f"{format_size(s.get('.data',0)):>8s}  "
              f"{format_size(s.get('.bss',0)):>8s}")
    print(f"  {'─'*70}")
    grand_total = sum(cat_totals.values())
    print(f"  {'TOTAL':<20s} {format_size(grand_total):>10s}")
    print()
    
    # ════════════════════════════════════════════
    # TOP OBJECT FILES BY SIZE
    # ════════════════════════════════════════════
    obj_totals = {}
    for obj, sections in allocated_by_obj.items():
        obj_totals[obj] = sum(sections.values())
    sorted_objs = sorted(obj_totals.items(), key=lambda x: -x[1])
    
    print("=" * W)
    print("TOP 30 OBJECT FILES BY SIZE".center(W))
    print("=" * W)
    print(f"  {'Object File':<45s} {'Total':>10s}  {'.text':>10s}  {'.bss':>8s}")
    print(f"  {'─'*75}")
    for obj, total in sorted_objs[:30]:
        s = allocated_by_obj[obj]
        print(f"  {short_name(obj):<45s} {format_size(total):>10s}  "
              f"{format_size(s.get('.text',0)):>10s}  "
              f"{format_size(s.get('.bss',0)):>8s}")
    print()
    
    # ════════════════════════════════════════════
    # TOP FUNCTIONS BY SIZE
    # ════════════════════════════════════════════
    sorted_funcs = sorted(allocated_functions, key=lambda x: -x[1])
    
    print("=" * W)
    print("TOP 30 FUNCTIONS BY SIZE".center(W))
    print("=" * W)
    print(f"  {'Function':<50s} {'Size':>10s}  {'Object':<20s}")
    print(f"  {'─'*75}")
    for name, size, obj in sorted_funcs[:30]:
        func = name.replace('.text.', '')
        print(f"  {func:<50s} {format_size(size):>10s}  {short_name(obj):<20s}")
    print()
    
    # ════════════════════════════════════════════
    # GARBAGE-COLLECTED (DISCARDED) SAVINGS
    # ════════════════════════════════════════════
    total_discarded = sum(discarded_by_obj.values())
    disc_by_cat = defaultdict(int)
    for obj, size in discarded_by_obj.items():
        disc_by_cat[categorize(obj)] += size
    sorted_disc_cat = sorted(disc_by_cat.items(), key=lambda x: -x[1])
    
    print("=" * W)
    print(f"DISCARDED BY LINKER (gc-sections saved {format_size(total_discarded)})".center(W))
    print("=" * W)
    print(f"  {'Category':<30s} {'Discarded':>12s}")
    print(f"  {'─'*44}")
    for cat, size in sorted_disc_cat:
        print(f"  {cat:<30s} {format_size(size):>12s}")
    print()
    
    # Top discarded object files
    sorted_disc = sorted(discarded_by_obj.items(), key=lambda x: -x[1])
    print(f"  Top discarded object files:")
    print(f"  {'Object File':<45s} {'Discarded':>12s}")
    print(f"  {'─'*59}")
    for obj, size in sorted_disc[:15]:
        print(f"  {short_name(obj):<45s} {format_size(size):>12s}")
    print()
    
    # ════════════════════════════════════════════
    # FILES THAT COULD POTENTIALLY BE REMOVED
    # ════════════════════════════════════════════
    # Files where discarded > allocated (mostly dead code)
    print("=" * W)
    print("FILES WITH HIGH DEAD CODE RATIO".center(W))
    print("(discarded > 50% of total compiled code)".center(W))
    print("=" * W)
    
    candidates = []
    for obj in set(list(allocated_by_obj.keys()) + list(discarded_by_obj.keys())):
        alloc = sum(allocated_by_obj.get(obj, {}).values())
        disc = discarded_by_obj.get(obj, 0)
        total_compiled = alloc + disc
        if total_compiled > 0 and disc > 0:
            ratio = disc / total_compiled
            if ratio > 0.5 and disc > 64:
                candidates.append((obj, alloc, disc, ratio))
    
    candidates.sort(key=lambda x: -x[2])
    if candidates:
        print(f"  {'Object File':<40s} {'Kept':>8s}  {'Discarded':>10s}  {'Dead%':>6s}")
        print(f"  {'─'*68}")
        for obj, alloc, disc, ratio in candidates[:20]:
            print(f"  {short_name(obj):<40s} {format_size(alloc):>8s}  "
                  f"{format_size(disc):>10s}  {ratio*100:>5.0f}%")
    else:
        print("  No significant candidates found.")
    print()
    
    # ════════════════════════════════════════════
    # CMSIS-NN DETAIL (since this is often the biggest optimization target)
    # ════════════════════════════════════════════
    cmsis_files = {}
    for obj, sections in allocated_by_obj.items():
        if categorize(obj) == 'CMSIS-NN':
            total = sum(sections.values())
            cmsis_files[obj] = total
    
    if cmsis_files:
        sorted_cmsis = sorted(cmsis_files.items(), key=lambda x: -x[1])
        cmsis_total = sum(cmsis_files.values())
        
        print("=" * W)
        print(f"CMSIS-NN DETAIL ({format_size(cmsis_total)} total)".center(W))
        print("=" * W)
        print(f"  {'File':<50s} {'Size':>10s}")
        print(f"  {'─'*62}")
        for obj, size in sorted_cmsis:
            marker = ""
            name = short_name(obj)
            if '_s16' in name or '_s4' in name:
                marker = "  ◄ NOT NEEDED for int8 model"
            print(f"  {name:<50s} {format_size(size):>10s}{marker}")
        print()
    
    # ════════════════════════════════════════════
    # SUMMARY & RECOMMENDATIONS
    # ════════════════════════════════════════════
    print("=" * W)
    print("RECOMMENDATIONS".center(W))
    print("=" * W)
    
    # Check for s4/s16 files still linked
    removable_cmsis = 0
    for obj, size in cmsis_files.items():
        name = short_name(obj)
        if '_s16' in name or '_s4' in name:
            removable_cmsis += size
    
    if removable_cmsis > 0:
        print(f"\n  1. REMOVE UNUSED CMSIS-NN VARIANTS")
        print(f"     s4/s16 files still in binary: {format_size(removable_cmsis)}")
        print(f"     Your int8 model only needs _s8 variants.")
    
    print(f"\n  2. COMPILER OPTIMIZATION")
    print(f"     Try -Os instead of -O2 for size optimization (10-30% code savings)")
    print(f"     Add -flto to COMMON_FLAGS and LDFLAGS for link-time optimization")
    
    print(f"\n  3. STRIP OPTIONS")
    print(f"     Add -fno-unwind-tables -fno-asynchronous-unwind-tables")
    print(f"     Use --strip-debug in objcopy step")
    
    total_potential = removable_cmsis
    print(f"\n  Estimated easy savings: ~{format_size(removable_cmsis)} (CMSIS-NN cleanup)")
    print(f"  Plus -Os/-flto could save an additional 10-30% of .text")
    text_savings_low = int(section_totals.get('.text', 0) * 0.10)
    text_savings_high = int(section_totals.get('.text', 0) * 0.30)
    print(f"  That's roughly {format_size(text_savings_low)} – {format_size(text_savings_high)} more")
    print()


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <application.map>")
        sys.exit(1)
    
    filepath = sys.argv[1]
    print(f"\nAnalyzing: {filepath}\n")
    
    results = parse_map_file(filepath)
    print_report(*results)


if __name__ == '__main__':
    main()
