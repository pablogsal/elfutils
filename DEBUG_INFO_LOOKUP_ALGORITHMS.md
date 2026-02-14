# How Tools Find Separated & Split Debug Information

A comprehensive survey of the algorithms used by different tools and libraries
to locate debug information that is not embedded in the main binary. This covers
separate debug files (`.debug`), split DWARF (`.dwo`), DWARF packages (`.dwp`),
and supplementary debug files (`.dwz`/debugaltlink).

**Date**: 2025
**Methodology**: Source code analysis of each project's repository.

---

## Table of Contents

1. [Feature Support Matrix](#1-feature-support-matrix)
2. [Elfutils (libdw / libdwfl)](#2-elfutils-libdw--libdwfl)
3. [libbacktrace](#3-libbacktrace)
4. [Libunwind (GNU)](#4-libunwind-gnu)
5. [LLVM libunwind](#5-llvm-libunwind)
6. [glibc backtrace()](#6-glibc-backtrace)
7. [Rust addr2line (gimli-rs)](#7-rust-addr2line-gimli-rs)
8. [GNU addr2line (binutils)](#8-gnu-addr2line-binutils)
9. [Linux perf](#9-linux-perf)
10. [GDB](#10-gdb)
11. [LLDB](#11-lldb)
12. [Heaptrack](#12-heaptrack)
13. [Comparison and Analysis](#13-comparison-and-analysis)

---

## 1. Feature Support Matrix

| Tool | Build-ID | .gnu_debuglink | .dwo | .dwp | debuginfod | .gnu_debugaltlink | .gnu_debugdata |
|------|:--------:|:--------------:|:----:|:----:|:----------:|:-----------------:|:--------------:|
| **Elfutils** | YES | YES | YES | YES | YES | YES | - |
| **libbacktrace** | YES | YES | NO | NO | NO | YES | YES |
| **Libunwind** | YES | YES | NO | NO | NO | NO | YES* |
| **LLVM libunwind** | NO | NO | NO | NO | NO | NO | NO |
| **glibc backtrace** | NO | NO | NO | NO | NO | NO | NO |
| **Rust addr2line** | NO** | NO** | YES | YES | NO | YES*** | NO |
| **GNU addr2line** | YES | YES | NO | NO | NO | NO | NO |
| **Linux perf** | YES | YES | NO**** | NO**** | YES | NO | YES |
| **GDB** | YES | YES | YES | YES | YES | YES | NO |
| **LLDB** | YES | YES | YES | YES | YES | NO | NO |
| **Heaptrack** | YES | YES | YES | YES | YES | YES | - |

\* Libunwind supports `.gnu_debugdata` (minidebuginfo) when compiled with LZMA support.
\** Rust addr2line has a TODO comment for debuglink/debugaltlink auto-discovery; requires manual path for supplement files.
\*** Rust addr2line supports supplementary files via explicit `--sup` path, not auto-discovery.
\**** Linux perf delegates split DWARF to libdw/libdwfl when doing DWARF-level analysis.

---

## 2. Elfutils (libdw / libdwfl)

Elfutils provides the most comprehensive debug info lookup, split across two layers:
**libdwfl** (high-level module/debug-file management) and **libdw** (DWARF parsing
including split DWARF).

### 2.1 Separate Debug Files (libdwfl)

**Entry point**: `dwfl_standard_find_debuginfo()` in `libdwfl/find-debuginfo.c`

#### Search Order

**Step 1 — Build-ID lookup** (`libdwfl/find-debuginfo.c:360-383`)

Calls `dwfl_build_id_find_debuginfo()` which delegates to
`__libdwfl_open_by_build_id()` (`libdwfl/dwfl_build_id_find_elf.c:39-140`).

Constructs path: `<dir>/.build-id/XX/YYYYYY...YYY.debug`
- `XX` = first byte of build-id in hex
- `YYY...` = remaining bytes in hex
- `.debug` suffix appended for debug info lookups

Iterates over the colon-separated `debuginfo_path` (default: `":.debug:/usr/lib/debug"`).
For each **absolute** path entry, constructs `<path>/.build-id/XX/YYY.debug` and
attempts to open. Uses `realpath()` to resolve symlinks.

**Validation**: Extracts build-id from the found file and compares with expected build-id
(`libdwfl/find-debuginfo.c:88-158`).

**Step 2 — Path-based / .gnu_debuglink lookup** (`libdwfl/find-debuginfo.c:160-345`)

Extracts `.gnu_debuglink` section via `dwelf_elf_gnu_debuglink()`
(`libdwelf/dwelf_elf_gnu_debuglink.c:35-99`). Returns filename and CRC32.

For each entry in `debuginfo_path` (default `":.debug:/usr/lib/debug"`):

| Path entry type | Search behavior |
|----------------|----------------|
| Empty (`:`) | `<binary_dir>/<debuglink_file>` |
| Absolute (e.g. `/usr/lib/debug`) | `<path>/<binary_dir>/<debuglink_file>` |
| Relative (e.g. `.debug`) | `<binary_dir>/<relpath>/<debuglink_file>` |

CRC validation controlled by path prefix flags:
- `+` prefix = force CRC checking
- `-` prefix = skip CRC checking

**Step 3 — Symlink resolution** (`libdwfl/find-debuginfo.c:390-401`)

If the binary path is a symlink, resolves with `realpath()` and repeats the path-based search.

**Step 4 — debuginfod** (`libdwfl/find-debuginfo.c:403-425`)

If `ENABLE_LIBDEBUGINFOD` is set, calls `__libdwfl_debuginfod_find_debuginfo()`
which uses the build-id to query remote debuginfod servers. Server URLs from
`DEBUGINFOD_URLS` environment variable.

### 2.2 Supplementary / Alt Debug Files (.dwz)

**Entry point**: `find_debug_altlink()` in `libdwfl/dwfl_module_getdwarf.c:568-616`

Reads `.gnu_debugaltlink` section (`libdwelf/dwelf_dwarf_gnu_debugaltlink.c:35-62`),
which contains a filename and a build-id. Uses the same separate-debug-file search
pipeline. When searching absolute paths, also tries a `.dwz/` subdirectory.
Validated by build-id match. Linked via `dwarf_setalt()`.

### 2.3 Split DWARF — .dwo Files (libdw)

**Entry point**: `__libdw_find_split_unit()` in `libdw/libdw_find_split_unit.c:159-249`

Only applies to skeleton units (`DW_UT_skeleton`).

**Search order**:

1. **Try DWP first** (see section 2.4 below)

2. **Try `debugdir/dwo_name`** (lines 194-201)
   - `debugdir` = directory of the ELF file
   - `dwo_name` = value of `DW_AT_dwo_name` (DWARF 5) or `DW_AT_GNU_dwo_name` (DWARF 4)

3. **Try `comp_dir/dwo_name`** (lines 206-217)
   - `comp_dir` = value of `DW_AT_comp_dir`

4. **DWO lookup callback** (lines 224-239)
   - Calls a registered callback set via `dwarf_set_dwo_lookup()`
   - libdwfl registers this callback for debuginfod integration
     (`libdwfl/dwfl_module_getdwarf.c:1412-1415`), enabling DWO files to be
     fetched from debuginfod servers

**Validation**: The DWO file must contain a unit with a matching 8-byte `unit_id`
(DWO ID signature).

### 2.4 DWARF Package Files — .dwp (libdw)

**Entry point**: `try_dwp_file()` in `libdw/libdw_find_split_unit.c:94-157`

- Constructs path: `<elf_path>.dwp` (e.g., `/path/to/binary.dwp`)
- Opens and validates: must contain `.debug_cu_index` or `.debug_tu_index` section
- Result cached in `cu->dbg->dwp_dwarf`

**Index lookup** (`libdw/dwarf_cu_dwp_section_info.c:324-359`):
Uses open-addressing hash table:
```
hash = unit_id (lower 32 bits)
hash2 = (unit_id >> 32) | 1
probe = (hash + i * hash2) % slot_count
```

**Large file support** (>4GB `.debug_info.dwo`):
Reconstructs correct offsets when GNU/LLVM dwp tools truncate offsets
to 32 bits (`libdw/dwarf_cu_dwp_section_info.c:208-309`).

### 2.5 Environment Variables & Configuration

| Variable/Setting | Effect |
|-----------------|--------|
| `DEBUGINFOD_URLS` | Colon-separated list of debuginfod server URLs |
| `debuginfo_path` (callback) | Colon-separated search paths (default: `":.debug:/usr/lib/debug"`) |

### 2.6 Key Source Files

| File | Purpose |
|------|---------|
| `libdwfl/find-debuginfo.c` | Separate debug file search (build-id, debuglink) |
| `libdwfl/dwfl_build_id_find_elf.c` | Build-ID based file opening |
| `libdwelf/dwelf_elf_gnu_debuglink.c` | `.gnu_debuglink` section parsing |
| `libdw/libdw_find_split_unit.c` | Split DWARF (.dwo) and DWP lookup |
| `libdw/dwarf_cu_dwp_section_info.c` | DWP package index parsing |
| `libdwfl/debuginfod-client.c` | debuginfod client integration |

---

## 3. libbacktrace

Ian Lance Taylor's libbacktrace provides backtrace and symbol resolution for C/C++.
All debug info lookup is in `elf.c`.

### 3.1 Separate Debug Files

**Entry point**: Main flow at `elf.c:6950-7044`

**Search order**:

1. **Build-ID lookup** (`elf_open_debugfile_by_buildid()`, `elf.c:869-918`)
   - Path: `/usr/lib/debug/.build-id/XX/YYYYYY...YYY.debug`
   - **Hardcoded** to `/usr/lib/debug/.build-id/` — no configurable path
   - Note: Unlike GDB, libbacktrace does NOT validate the build-id of the found file

2. **GNU debuglink** (`elf_find_debugfile_by_debuglink()`, `elf.c:958-1068`)
   - Reads `.gnu_debuglink` section (`elf.c:6803-6824`)
   - Search paths (in order):
     1. `<binary_dir>/<debuglink_file>`
     2. `<binary_dir>/.debug/<debuglink_file>`
     3. `/usr/lib/debug/<binary_dir>/<debuglink_file>`
   - Resolves symlinks (e.g., `/proc/self/exe`)
   - CRC32 validation (`elf.c:1089-1098`)

3. **GNU debugaltlink** (`elf.c:7016-7038`)
   - Reads `.gnu_debugaltlink` section (`elf.c:6826-6850`)
   - Uses same debuglink search logic

4. **Fallback: .gnu_debugdata** (minidebuginfo) (`elf.c:7046-7070`)
   - LZMA-compressed minimal ELF embedded in `.gnu_debugdata` section

### 3.2 NOT Supported

- **No .dwo/.dwp support** — No split DWARF handling at all
- **No debuginfod** — No network-based debug file retrieval
- **No environment variables** — All paths are hardcoded
- **No configurable search paths**

---

## 4. Libunwind (GNU)

The GNU libunwind library focuses on stack unwinding. Debug info lookup is in `src/elfxx.c`.

### 4.1 Separate Debug Files

**Entry point**: `elf_w(load_debuginfo)` in `src/elfxx.c:897-1008`

**Search order**:

1. **Build-ID lookup** (`elf_w(find_build_id_path)`, `src/elfxx.c:816-887`)
   - Reads `NT_GNU_BUILD_ID` from ELF `PT_NOTE` program headers
   - Path: `/usr/lib/debug/.build-id/XX/YYYYYY...YYY.debug`
   - **Hardcoded** path — not configurable

2. **GNU debuglink** (`src/elfxx.c:936-988`)
   - Reads `.gnu_debuglink` section
   - Search paths (in order):
     1. `<binary_dir>/<debuglink_file>`
     2. `<binary_dir>/.debug/<debuglink_file>`
     3. `/usr/lib/debug/<binary_dir>/<debuglink_file>` (only if `is_local==1`)

3. **Minidebuginfo** (`.gnu_debugdata`, `src/elfxx.c:549-611`)
   - XZ decompression via liblzma (when compiled with LZMA support)

### 4.2 NOT Supported

- **No .dwo/.dwp** — No split DWARF
- **No debuginfod**
- **No configurable search paths or environment variables**

---

## 5. LLVM libunwind

LLVM's libunwind is a minimal, in-process stack unwinder. It operates exclusively
on memory-mapped sections within the running process.

### 5.1 How It Finds Unwind Info

- Uses `dl_iterate_phdr()` (Linux) or `_dyld_find_unwind_sections()` (macOS) to
  enumerate loaded objects
- Reads `.eh_frame` / `.eh_frame_hdr` sections directly from mapped memory
- On modern glibc, uses `_dl_find_object()` for faster lookup
- Supports dynamic registration via `__unw_add_find_dynamic_unwind_sections()`

Key function: `findUnwindSections()` in `src/AddressSpace.hpp:506-675`

### 5.2 NOT Supported

- **No separate debug files** — no file I/O for debug data at all
- **No .gnu_debuglink or build-id**
- **No .dwo/.dwp**
- **No debuginfod**

**Design rationale**: LLVM libunwind is purpose-built for fast, in-process stack
unwinding. All unwind information must already be mapped into the process address space.

---

## 6. glibc backtrace()

The glibc `backtrace()` function is deliberately minimal.

### 6.1 How It Works

- **Unwinding**: Delegates to libgcc's `_Unwind_Backtrace()` via dynamically loaded
  `libgcc_s.so` (`misc/unwind-link.c:40-115`). Uses `.eh_frame` (NOT DWARF debug info).
- **Symbol resolution**: `backtrace_symbols()` (`debug/backtracesyms.c:35-119`) calls
  `_dl_addr()` which looks up symbols from the loaded binary's `DT_SYMTAB` ELF symbol table.
  Returns format: `file(symbol+offset) [address]`.

### 6.2 NOT Supported

- **No debug info lookup at all** — does not read DWARF, does not look for external files
- **No .gnu_debuglink, build-id, .dwo/.dwp, debuginfod**
- Symbol names come exclusively from the runtime symbol table (`.dynsym`)

**Design rationale**: Intended for crash diagnostics where speed and minimal
dependencies matter. Works even without any debug info installed.

---

## 7. Rust addr2line (gimli-rs)

The Rust `addr2line` crate (`gimli-rs/addr2line`) uses the `gimli` DWARF parser
and the `object` crate for ELF/Mach-O parsing.

### 7.1 Split DWARF — .dwo Files

**Entry point**: `ResUnit::dwarf_and_unit()` in `src/unit.rs:40-112`

- Reads `DW_AT_dwo_name` / `DW_AT_GNU_dwo_name` from the skeleton CU
- Gets `DW_AT_comp_dir` for path resolution
- Uses a **callback-based lazy loading pattern** (`SplitDwarfLoad` struct in `src/lookup.rs`)

The `Loader` implementation (`src/loader.rs:335-379`):
1. First checks DWP package (see 7.2)
2. Falls back to filesystem: `comp_dir + dwo_name`
3. Validates DWO ID match

### 7.2 DWARF Package — .dwp Files

**Location**: `src/loader.rs:225-254`

- Constructs DWP path: `<binary_path>.dwp`
  - If binary has extension (e.g., `.debug`), appends `.dwp` → `file.debug.dwp`
  - If no extension, sets extension to `.dwp` → `file.dwp`
- Uses `gimli::DwarfPackage::load()` for index parsing

### 7.3 Supplementary Files

- Supports explicit supplementary file path via `Loader::new_with_sup(path, sup_path)`
- CLI: `--sup` flag (`src/bin/addr2line.rs:117-121`)
- Handles `DW_FORM_ref_sup4/8` references in `src/function.rs:501-509`

### 7.4 macOS dSYM Support

`src/loader.rs:179-207`: Extracts UUID from binary, searches adjacent `*.dSYM`
bundles, matches by UUID within `Contents/Resources/DWARF/`.

### 7.5 NOT Supported (Yet)

- **No automatic .gnu_debuglink** — `src/loader.rs:169` has `// TODO: use debuglink and debugaltlink`
- **No build-id based lookup**
- **No debuginfod**
- **No configurable search paths or environment variables**

---

## 8. GNU addr2line (binutils)

GNU addr2line uses the BFD (Binary File Descriptor) library for all file operations.

### 8.1 Separate Debug Files

**Entry point**: `find_separate_debug_file()` in `bfd/opncls.c`

BFD supports two methods, tried by `_bfd_dwarf2_slurp_debug_info()` in `bfd/dwarf2.c`:

1. **Build-ID lookup** (`bfd_follow_build_id_debuglink()`)
   - Constructs: `<debug-file-dir>/.build-id/XX/YYYYYY...YYY.debug`
   - Validates by comparing build-id of found file (`check_build_id_file()`)

2. **GNU debuglink** (`bfd_follow_gnu_debuglink()`)
   - Reads `.gnu_debuglink` section (filename + CRC32)
   - Search paths (in order):
     1. `<binary_dir>/<debuglink_file>`
     2. `<binary_dir>/.debug/<debuglink_file>`
     3. `/usr/lib/debug/<binary_dir>/<debuglink_file>`
     4. `/usr/lib/debug/usr/<binary_dir>/<debuglink_file>`
     5. `<debug-file-directory>/<binary_dir>/<debuglink_file>`
   - CRC32 validation via `separate_debug_file_exists()` — reads entire file
     in 8KB chunks and computes CRC32

### 8.2 NOT Supported

- **No .dwo/.dwp** — BFD's `dwarf2.c` only handles monolithic DWARF
- **No debuginfod**
- **No configurable paths at runtime** (uses compile-time defaults + `--debug-file-directory` link option)

---

## 9. Linux perf

Linux perf has an elaborate multi-strategy debug info search with a local build-id cache.

### 9.1 Separate Debug Files

**Entry point**: Symbol loading iterates `binary_type_symtab[]` array in
`tools/perf/util/symbol.c:84-103`

**Search order** (DSO binary types tried sequentially):

1. `DSO_BINARY_TYPE__DEBUGLINK` — `.gnu_debuglink` section lookup
   - `filename__read_debuglink()` in `symbol-elf.c:984-1038`
   - Paths tried:
     1. `<binary_dir>/<debuglink_file>`
     2. `<binary_dir>/.debug/<debuglink_file>`
     3. `/usr/lib/debug/<binary_dir>/<debuglink_file>`

2. `DSO_BINARY_TYPE__BUILD_ID_CACHE` — Local build-id cache
   - Default location: `~/.debug/.build-id/XX/YYYYYY...`
   - `dso__build_id_filename()` in `build-id.c:280-286`

3. `DSO_BINARY_TYPE__BUILD_ID_CACHE_DEBUGINFO` — Debug-only cache variant

4. `DSO_BINARY_TYPE__FEDORA_DEBUGINFO` — `/usr/lib/debug<path>.debug`

5. `DSO_BINARY_TYPE__UBUNTU_DEBUGINFO` — `/usr/lib/debug<path>` (no `.debug` suffix)

6. `DSO_BINARY_TYPE__BUILDID_DEBUGINFO` — `/usr/lib/debug/.build-id/XX/YYY.debug`

7. `DSO_BINARY_TYPE__GNU_DEBUGDATA` — Embedded `.gnu_debugdata` (LZMA-compressed)

8. `DSO_BINARY_TYPE__SYSTEM_PATH_DSO` — Original binary as fallback

### 9.2 Build-ID Cache Structure

```
~/.debug/
├── .build-id/
│   └── XX/
│       └── YYYYYY.../
│           ├── elf        # cached binary
│           ├── debug      # cached debug info
│           ├── kallsyms   # kernel symbols
│           └── vdso       # virtual DSO
```

Controlled by:
- `set_buildid_dir()` in `config.c:913-932`
- Default: `$HOME/.debug`

### 9.3 debuginfod Integration

`tools/perf/util/debuginfo.c:188-204`:
- Uses `debuginfod_begin()` / `debuginfod_find_source()` / `debuginfod_find_debuginfo()`
- Falls back to debuginfod when distro paths fail
- Server URLs from `DEBUGINFOD_URLS` environment variable

### 9.4 Split DWARF (.dwo/.dwp)

Perf itself does **not** implement DWO/DWP file search in its DSO lookup layer.
However, when perf uses libdw/libdwfl for deeper DWARF analysis (e.g., `perf probe`),
the split DWARF support from elfutils is available.

### 9.5 Environment Variables

| Variable | Effect |
|----------|--------|
| `DEBUGINFOD_URLS` | debuginfod server URLs |
| `HOME` | Base for `~/.debug` cache |
| `PERF_BUILDID_DIR` | Override build-id cache location |

---

## 10. GDB

GDB has the most sophisticated debug info lookup of any debugger, with full support
for all mechanisms.

### 10.1 Separate Debug Files

GDB tries build-id first, then debuglink.

**Search order**:

1. **Build-ID** lookup:
   - Path: `<debug-file-directory>/.build-id/XX/YYYYYY...YYY.debug`
   - `debug-file-directory` defaults to `/usr/lib/debug`
   - Validation: checks build-id in found file matches

2. **GNU debuglink** (if build-id lookup fails):
   - Reads `.gnu_debuglink` section
   - Search paths:
     1. `<binary_dir>/<debuglink_file>`
     2. `<binary_dir>/.debug/<debuglink_file>`
     3. `<debug-file-directory>/<binary_dir>/<debuglink_file>`
   - CRC32 validation

### 10.2 Split DWARF — .dwo Files

**Key functions** in `gdb/dwarf2/read.c`: `open_dwo_file()`, `try_open_dwop_file()`

**Search order**:

1. **`comp_dir/dwo_name`** — Join `DW_AT_comp_dir` with `DW_AT_GNU_dwo_name` /
   `DW_AT_dwo_name` and search relative to CWD
2. **`debug-file-directory/dwo_name`** — Search `dwo_name` basename in `debug-file-directory`
3. **debuginfod** (in-progress integration) — Query debuginfod servers with DWO ID

### 10.3 DWARF Package — .dwp Files

**Key functions** in `gdb/dwarf2/read.c`: `open_dwp_file()`, `lookup_dwo_in_dwp()`

**Search order**:

1. **`<executable>.dwp`** — Append `.dwp` to executable path, search relative to CWD
2. **`debug-file-directory/<basename>.dwp`** — Strip directory, search in `debug-file-directory`

**Priority**: DWP is checked before individual DWO files. If a DWP is found, GDB
does NOT look for individual .dwo files.

### 10.4 debuginfod

Full integration in `gdb/debuginfod-support.c`:
- `debuginfod_debuginfo_query()` for separate debug files
- `debuginfod_source_query()` for source files
- DWO query support (in development)
- Uses `DEBUGINFOD_URLS` environment variable
- Cache: `$HOME/.cache/debuginfod_client`

### 10.5 Configuration

| Setting | Default | Effect |
|---------|---------|--------|
| `debug-file-directory` | `/usr/lib/debug` | Base path for all debug file searches |
| `DEBUGINFOD_URLS` (env) | (none) | debuginfod server URLs |

---

## 11. LLDB

LLDB uses a plugin-based architecture for debug info lookup with platform-specific
strategies.

### 11.1 Separate Debug Files

**SymbolLocator plugin chain** (tried in order):
1. **DebugSymbols** (macOS only — Spotlight-based dSYM lookup)
2. **Debuginfod** (`SymbolLocatorDebuginfod.cpp:143-211`)
3. **Default** (`SymbolLocatorDefault.cpp:97-254`)

**Default search order** (`SymbolLocatorDefault::LocateExecutableSymbolFile()`):
1. User-specified symbol file spec (if absolute & exists)
2. Module's directory (binary location)
3. Current working directory (if external lookup enabled)
4. `/usr/lib/debug` (Linux) or `/usr/libdata/debug` (NetBSD)
5. FreeBSD: `$LOCALBASE/lib/debug` (usually `/usr/local/lib/debug`)
6. **Build-ID directory**: `/usr/lib/debug/.build-id/XX/YYYYYY...YYY.debug`
7. `.debug/` subdirectories relative to search paths
8. Full path preservation: mirror source directory structure under debug dir

**Validation**: UUID (build-id) matching — skips files if UUID doesn't match.

### 11.2 Split DWARF — .dwo Files

**Entry point**: `SymbolFileDWARF::GetDwoSymbolFileForCompileUnit()` in
`source/Plugins/SymbolFile/DWARF/SymbolFileDWARF.cpp:1774-1878`

- Reads `DW_AT_GNU_dwo_name` first (DWARF 4), falls back to `DW_AT_dwo_name` (DWARF 5)

**Search order**:
1. Try DWP first (see 11.3)
2. Try DWO filename as-is (absolute or relative to CWD)
3. If `DW_AT_comp_dir` is absolute: `comp_dir/dwo_name`
4. If `DW_AT_comp_dir` is relative: resolve relative to binary location, then `comp_dir/dwo_name`
5. User-configured `Target::GetDefaultDebugFileSearchPaths()`

**Loading**: Lazy, on-demand per compile unit.

### 11.3 DWARF Package — .dwp Files

**Entry point**: `SymbolFileDWARF::GetDwpSymbolFile()` in
`source/Plugins/SymbolFile/DWARF/SymbolFileDWARF.cpp:4419-4478`

**Search order**:
1. `<module_path>.dwp`
2. `<symbol_file_path>.dwp` (if different from module path)
3. Basename without extension + `.dwp`
4. `Target::GetDefaultDebugFileSearchPaths()` directories
5. **debuginfod fallback** — calls `PluginManager::LocateExecutableSymbolFile()`
   with UUID to enable network lookup

**Priority**: DWP checked before individual DWO files (same as GDB).

**Note**: UUID validation is skipped for DWP files since dwp tools don't embed build-ids.

### 11.4 debuginfod

Full integration via `SymbolLocatorDebuginfod` plugin:
- Build-ID based lookup
- Supports both executable and debug info retrieval
- Uses `DEBUGINFOD_URLS` environment variable
- Cache managed by LLVM's debuginfod client
- Configurable timeout and cache path via LLDB settings

### 11.5 Key Source Files

| File | Purpose |
|------|---------|
| `source/Plugins/ObjectFile/ELF/ObjectFileELF.cpp` | Build-ID and debuglink extraction |
| `source/Plugins/SymbolLocator/Default/SymbolLocatorDefault.cpp` | Local filesystem search |
| `source/Plugins/SymbolLocator/Debuginfod/SymbolLocatorDebuginfod.cpp` | debuginfod client |
| `source/Plugins/SymbolFile/DWARF/SymbolFileDWARF.cpp` | DWO/DWP lookup |
| `source/Plugins/SymbolFile/DWARF/SymbolFileDWARFDwo.cpp` | DWO unit validation |

---

## 12. Heaptrack

Heaptrack (KDE memory profiler) **delegates all debug info lookup to elfutils/libdwfl**.

### 12.1 How It Works

- Links against `libdw` (requires elfutils >= 0.158)
- Initializes a `Dwfl` object with standard callbacks:
  - `dwfl_build_id_find_elf` — finds ELF by build-id
  - `dwfl_standard_find_debuginfo` — finds separate debug info
  - `dwfl_offline_section_address` — handles section addresses
- Uses `dwfl_report_elf()` to register modules and `dwfl_addrmodule()` for lookup

### 12.2 What Heaptrack Adds

- **DWARF caching layer**: `DwarfDieCache`, `CuDieRangeMapping` for efficient queries
  (`src/interpret/dwarfdiecache.cpp`)
- **Symbol caching** (`src/interpret/symbolcache.cpp`)
- **Demangling**: C++, Rust, D language support

### 12.3 Capabilities

Since it uses elfutils/libdwfl, heaptrack inherits all of elfutils' debug info
lookup capabilities including build-id, debuglink, split DWARF, DWP, and debuginfod.

---

## 13. Comparison and Analysis

### 13.1 Separate Debug File Search Paths

Almost all tools that support separate debug files use the same core algorithm
(originated from GDB):

```
1. <binary_dir>/<debuglink_file>
2. <binary_dir>/.debug/<debuglink_file>
3. /usr/lib/debug/<binary_dir>/<debuglink_file>
```

**Variations**:
- **Elfutils** additionally supports configurable `debuginfo_path` with colon-separated entries
- **Linux perf** adds distro-specific paths (Fedora `.debug` suffix, Ubuntu without)
- **LLDB** adds platform-specific paths (NetBSD `/usr/libdata/debug`, FreeBSD `$LOCALBASE`)
- **GNU addr2line (BFD)** adds `/usr/lib/debug/usr/` as extra root

### 13.2 Build-ID Directory Structure

All tools use the same structure: `<root>/.build-id/XX/YYYYYY...YYY.debug`

| Tool | Build-ID root | Configurable? |
|------|--------------|---------------|
| Elfutils | `debuginfo_path` entries | YES (callback) |
| libbacktrace | `/usr/lib/debug` | NO |
| Libunwind | `/usr/lib/debug` | NO |
| Linux perf | `~/.debug` + `/usr/lib/debug` | YES (`PERF_BUILDID_DIR`) |
| GDB | `debug-file-directory` | YES (GDB setting) |
| LLDB | `/usr/lib/debug` + search paths | YES (target settings) |
| GNU addr2line | Compile-time default | NO (at runtime) |

### 13.3 Split DWARF (.dwo) Search Paths

Tools that support .dwo files use a similar core strategy:

| Tool | comp_dir + name | debugdir + name | debug-file-directory | debuginfod |
|------|:--------------:|:---------------:|:-------------------:|:----------:|
| Elfutils | YES | YES | via callback | YES |
| Rust addr2line | YES | NO | NO | NO |
| GDB | YES | NO | YES | In progress |
| LLDB | YES | YES (binary dir) | YES (search paths) | YES (fallback) |

### 13.4 DWP Search

| Tool | `<binary>.dwp` | debug-file-directory | debuginfod |
|------|:--------------:|:-------------------:|:----------:|
| Elfutils | YES | NO | via callback |
| Rust addr2line | YES | NO | NO |
| GDB | YES | YES (basename) | In progress |
| LLDB | YES | YES (search paths) | YES |

### 13.5 Tools That Delegate

Several tools don't implement their own debug info lookup:

| Tool | Delegates to | Inherits capabilities |
|------|-------------|----------------------|
| Heaptrack | elfutils/libdwfl | All elfutils features |
| Linux perf (DWARF) | elfutils/libdwfl | Split DWARF via libdw |
| glibc backtrace | libgcc (eh_frame only) | None — no debug info |
| LLVM libunwind | dynamic linker only | None — no debug info |

### 13.6 debuginfod Adoption

| Tool | Status |
|------|--------|
| Elfutils | Full (native — elfutils provides the reference implementation) |
| GDB | Full |
| LLDB | Full (via LLVM's debuginfod client) |
| Linux perf | Full |
| libbacktrace | None |
| Libunwind | None |
| Rust addr2line | None |
| GNU addr2line | None |

### 13.7 Validation Methods

| Tool | Build-ID match | CRC32 check | DWO ID match |
|------|:--------------:|:-----------:|:------------:|
| Elfutils | YES | YES (configurable) | YES |
| libbacktrace | NO (skips) | YES | N/A |
| Libunwind | NO | NO | N/A |
| GDB | YES | YES | YES |
| LLDB | YES (UUID) | NO | YES |
| GNU addr2line | YES | YES | N/A |
| Linux perf | YES | NO | N/A |

### 13.8 Notable Gaps and Differences

1. **libbacktrace** does not validate build-id of found debug files (comment notes
   GDB does but they chose not to).

2. **Libunwind** has the most basic implementation — hardcoded paths, no validation,
   no configurability.

3. **LLVM libunwind** and **glibc backtrace** are pure unwinders with zero debug
   info file lookup — they only use `.eh_frame` from mapped memory.

4. **Rust addr2line** has a notable gap: no automatic `.gnu_debuglink`/build-id
   discovery (marked TODO), but excellent split DWARF support.

5. **GNU addr2line** (BFD-based) lacks split DWARF support entirely — BFD's DWARF
   reader is monolithic only.

6. **GDB vs LLDB** on DWP: Both check DWP before DWO. GDB uses `debug-file-directory`
   for fallback; LLDB uses a plugin chain with debuginfod as final fallback.

7. **Elfutils** is unique in providing a DWO lookup callback mechanism (`dwarf_set_dwo_lookup()`)
   that allows library users to plug in custom search strategies (used by libdwfl
   for debuginfod integration).

8. **Linux perf** is unique in having a local build-id cache (`~/.debug/`) that
   stores copies of binaries and debug info for offline analysis.

9. **Elfutils** has special handling for >4GB `.debug_info.dwo` sections in DWP
   files, working around a bug where GNU/LLVM dwp tools truncate offsets to 32 bits.
