# How Tools Find Separated & Split Debug Information

A comprehensive survey of the algorithms used by different tools and libraries
to locate debug information that is not embedded in the main binary. This covers
separate debug files, split DWARF (`.dwo`), DWARF packages (`.dwp`),
supplementary/alt debug files (`.dwz`/debugaltlink), and minidebuginfo
(`.gnu_debugdata`).

**Date**: 2025
**Methodology**: Source code analysis of each project's repository.

---

## Table of Contents

1. [Background: The Mechanisms](#1-background-the-mechanisms)
2. [Feature Support Matrix](#2-feature-support-matrix)
3. [Elfutils (libdw / libdwfl)](#3-elfutils-libdw--libdwfl)
4. [libbacktrace](#4-libbacktrace)
5. [Libunwind (GNU)](#5-libunwind-gnu)
6. [LLVM libunwind](#6-llvm-libunwind)
7. [glibc backtrace()](#7-glibc-backtrace)
8. [Rust addr2line (gimli-rs)](#8-rust-addr2line-gimli-rs)
9. [GNU addr2line (binutils/BFD)](#9-gnu-addr2line-binutilsbfd)
10. [Linux perf](#10-linux-perf)
11. [GDB](#11-gdb)
12. [LLDB](#12-lldb)
13. [Heaptrack](#13-heaptrack)
14. [Comparison and Analysis](#14-comparison-and-analysis)

---

## 1. Background: The Mechanisms

Before diving into each tool, it is important to understand the various
mechanisms that exist for pointing a consumer at external debug information.
Tools may support any subset of these.

### 1.1 The `.gnu_debuglink` Section

A non-allocated ELF section named `.gnu_debuglink` may be present in a stripped
binary. It contains two things: a null-terminated filename (with no directory
component — just a bare name like `libfoo.so.6.debug`) followed by padding to a
4-byte boundary and then a 4-byte CRC-32 checksum of the full contents of the
separate debug file.

The consumer is expected to search for a file with that name in a set of
conventional directories, then validate the CRC-32 before trusting it. The
original convention (established by GDB) is to search:

1. The same directory that contains the executable or shared library.
2. A subdirectory called `.debug/` under that directory.
3. A global debug directory (typically `/usr/lib/debug/`) with the full
   directory path of the binary appended.

### 1.2 The Build-ID (`.note.gnu.build-id`)

The GNU build-ID is a unique identifier embedded in an ELF note section,
typically named `.note.gnu.build-id`, with the note type `NT_GNU_BUILD_ID`.
The linker generates this — commonly a 20-byte SHA-1 hash — from the contents of
the binary. The same build-ID appears in both the stripped binary and its
corresponding debug file.

When using the build-ID to locate debug info, the convention is to take the
hex-encoded build-ID, split off the first two hex characters as a directory name,
and use the remainder (plus a `.debug` suffix) as the filename, all under a
`.build-id/` directory within the debug root. For example, a build-ID of
`abcdef1234...` would produce the path:

```
/usr/lib/debug/.build-id/ab/cdef1234...debug
```

### 1.3 Split DWARF (`.dwo` Files)

Split DWARF (sometimes called "DWARF Fission" or `-gsplit-dwarf`) is a compiler
feature where the bulky type and line information is emitted into a separate
`.dwo` ("DWARF object") file at compile time, and the main object file retains
only a lightweight "skeleton" compilation unit. The skeleton unit contains a
`DW_AT_dwo_name` attribute (DWARF 5) or `DW_AT_GNU_dwo_name` attribute (DWARF 4
GNU extension) naming the `.dwo` file, a `DW_AT_comp_dir` attribute giving the
compilation directory, and a `DW_AT_dwo_id` 8-byte hash that uniquely identifies
the split unit.

The consumer must locate the `.dwo` file on disk, open it, find the compilation
unit whose DWO ID matches the skeleton's, and then merge the two halves to
reconstitute the full DWARF information.

### 1.4 DWARF Package Files (`.dwp`)

A `.dwp` (DWARF Package) file is produced by the `dwp` tool (GNU or LLVM
variant). It aggregates many `.dwo` files into a single archive, with an index
section (`.debug_cu_index` and/or `.debug_tu_index`) that allows efficient
lookup by DWO ID. The index is a hash table using open addressing with double
hashing.

The conventional location for a `.dwp` file is right next to the executable
with `.dwp` appended to its name (e.g., `myprogram.dwp` for `myprogram`).

Tools that support split DWARF typically check for a `.dwp` file first, since
one `.dwp` lookup replaces potentially thousands of individual `.dwo` file opens.

### 1.5 Supplementary / Alt Debug Files (`.gnu_debugaltlink`)

The `.gnu_debugaltlink` section is used by the `dwz` multifile optimization tool.
When `dwz` deduplicates DWARF across multiple binaries, the shared portion is
placed in a single supplementary file. The `.gnu_debugaltlink` section contains
a filename and a build-ID for the supplementary file. DWARF references from the
main debug info use special forms (`DW_FORM_GNU_ref_alt`, `DW_FORM_GNU_strp_alt`,
or their DWARF 5 equivalents `DW_FORM_ref_sup4`/`DW_FORM_ref_sup8`) to point
into the supplementary file.

### 1.6 Minidebuginfo (`.gnu_debugdata`)

The `.gnu_debugdata` section contains a minimal, LZMA-compressed ELF image with
just enough symbol information (typically `.symtab` and `.dynsym`) to provide
function names in backtraces. It is used by Fedora/RHEL as a middle ground
between fully stripped binaries and full debuginfo. The consumer decompresses
the section with XZ/LZMA and processes the resulting ELF image as if it were a
separate debug file.

### 1.7 debuginfod

debuginfod is a client/server protocol (and reference implementation in elfutils)
that allows tools to download debug information, executables, and source files
on demand from a network server. The client identifies what it needs by build-ID.
The environment variable `DEBUGINFOD_URLS` provides a space- or
colon-separated list of server URLs. Downloaded artifacts are cached locally
(typically under `$HOME/.cache/debuginfod_client/`).

---

## 2. Feature Support Matrix

| Tool | Build-ID | `.gnu_debuglink` | `.dwo` | `.dwp` | debuginfod | `.gnu_debugaltlink` | `.gnu_debugdata` |
|------|:--------:|:----------------:|:------:|:------:|:----------:|:-------------------:|:----------------:|
| **Elfutils** | Yes | Yes | Yes | Yes | Yes | Yes | — |
| **libbacktrace** | Yes | Yes | No | No | No | Yes | Yes |
| **Libunwind (GNU)** | Yes | Yes | No | No | No | No | Yes\* |
| **LLVM libunwind** | No | No | No | No | No | No | No |
| **glibc backtrace** | No | No | No | No | No | No | No |
| **Rust addr2line** | No† | No† | Yes | Yes | No | Partial‡ | No |
| **GNU addr2line** | Yes | Yes | No | No | No | No | No |
| **Linux perf** | Yes | Yes | Delegated§ | Delegated§ | Yes | No | Yes |
| **GDB** | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| **LLDB** | Yes | Yes | Yes | Yes | Yes | No | No |
| **Heaptrack** | Yes | Yes | Yes | Yes | Yes | Yes | — |

\* When compiled with LZMA support.
† The source code contains a TODO for automatic debuglink and debugaltlink
discovery; the library currently requires callers to provide paths manually.
‡ Supplementary files are supported but only via an explicit path (`--sup`),
not via automatic discovery from the `.gnu_debugaltlink` section.
§ Perf does not implement split DWARF in its own DSO layer, but when it uses
libdw/libdwfl for deep DWARF analysis (e.g., `perf probe`), elfutils' split
DWARF support is available.

---

## 3. Elfutils (libdw / libdwfl)

Elfutils provides the most comprehensive debug information lookup of any library
in this survey. The work is split across two layers: **libdwfl** handles
high-level concerns like finding modules, locating separate debug files, and
managing the relationship between an executable and its debug data; **libdw**
handles DWARF parsing, including split DWARF and DWARF package files.

### 3.1 Separate Debug Files

The standard entry point is `dwfl_standard_find_debuginfo()`, the default
callback that libdwfl uses when an application asks for DWARF data for a module
and the module's own ELF does not contain it (or contains only a skeleton).

The algorithm proceeds through four stages in order. If any stage succeeds, the
remaining stages are skipped.

#### Stage 1: Build-ID Lookup

If the module has a build-ID (extracted from its `.note.gnu.build-id` section),
elfutils attempts a build-ID-based lookup first because it is the most reliable
identification method — it does not depend on filenames or directory layouts.

The build-ID is converted to a hex string. The first byte becomes a two-character
directory name; the remaining bytes become the filename, with `.debug` appended.
This produces a relative path fragment like `ab/cdef0123456789...debug` under a
`.build-id/` directory.

Elfutils then iterates over a colon-separated search path. The default path is
`":.debug:/usr/lib/debug"`, which provides three entries:

- The empty entry (between the leading colon or colons), which means "the
  directory containing the binary itself."
- `.debug`, which means "a `.debug/` subdirectory of the binary's directory."
- `/usr/lib/debug`, the conventional system-wide debug directory.

For each **absolute** entry in the path, elfutils constructs the full path as
`<entry>/.build-id/<hex-fragment>` and attempts to open it. Relative entries
are skipped for build-ID lookup (they would not make sense for the
`.build-id/` hierarchy). If the file is opened successfully, elfutils extracts
the build-ID from the candidate file and compares it byte-for-byte with the
expected build-ID. Only if they match is the file accepted.

The search path is configurable. Applications that embed libdwfl can provide
their own path through the `debuginfo_path` field of the `Dwfl_Callbacks`
structure. This path can be changed at any time.

#### Stage 2: debuglink / Path-Based Lookup

If build-ID lookup did not produce a result, elfutils falls back to the
`.gnu_debuglink` section. It reads the section to obtain the bare debug filename
and a CRC-32 checksum.

Elfutils then searches for the debug file by iterating over the same
colon-separated search path. For each entry, the behavior depends on the entry
type:

- **Empty entry**: Searches in the binary's own directory. The candidate path is
  simply `<binary_dir>/<debuglink_filename>`.

- **Relative entry** (e.g., `.debug`): Searches in a subdirectory of the
  binary's directory. The candidate path is
  `<binary_dir>/<relative_entry>/<debuglink_filename>`.

- **Absolute entry** (e.g., `/usr/lib/debug`): Searches under the absolute
  directory with the binary's directory path appended. The candidate path is
  `<absolute_entry>/<binary_dir>/<debuglink_filename>`.

Each path entry in the search path may optionally be prefixed with `+` or `-`:
- `+` forces CRC-32 validation for files found via that entry.
- `-` disables CRC-32 validation for that entry.
- No prefix uses the default behavior (CRC validation happens when a CRC is
  available from the debuglink section).

When CRC validation is enabled and a candidate file is found, elfutils computes
the CRC-32 of the entire candidate file and compares it against the CRC stored
in the `.gnu_debuglink` section. A mismatch causes the candidate to be rejected
and the search continues. If build-ID information is available for the candidate,
a build-ID match check is performed instead of (or in addition to) the CRC
check, since build-ID matching is more reliable.

#### Stage 3: Symlink Resolution

If the binary's path is a symbolic link and the path-based search (Stage 2)
did not find anything, elfutils resolves the symlink using `realpath()` to obtain
the true location on disk. It then repeats the Stage 2 search using the resolved
path. This handles the common case where a binary at `/usr/bin/foo` is actually a
symlink to `/usr/bin/foo-2.3`, and the debug file is stored under the real path.

#### Stage 4: debuginfod

If all local searches fail and elfutils was compiled with debuginfod support,
it queries debuginfod servers as a last resort. The query uses the module's
build-ID. The `DEBUGINFOD_URLS` environment variable provides the list of
servers to contact. Downloaded files are cached locally so subsequent lookups
for the same build-ID do not require a network round-trip.

This stage is truly a last resort — it only runs after all local filesystem
searches have been exhausted.

### 3.2 Supplementary / Alt Debug Files (`.gnu_debugaltlink` / dwz)

When elfutils opens the DWARF data for a module and encounters a
`.gnu_debugaltlink` section, it triggers a separate search for the supplementary
file. The section contains a filename and a raw build-ID for the supplementary
file.

The search reuses the same pipeline as separate debug files (Stages 1–4 above),
with two modifications:

- When searching under absolute path entries, elfutils also tries a `.dwz/`
  subdirectory. For example, if the search path includes `/usr/lib/debug` and the
  alt filename is `shared-data`, elfutils will try both
  `/usr/lib/debug/.../shared-data` and `/usr/lib/debug/.dwz/shared-data`.

- Validation is always by build-ID match (the build-ID is embedded directly in
  the `.gnu_debugaltlink` section).

Once found, the supplementary DWARF file is linked to the main DWARF via
`dwarf_setalt()`, and any `DW_FORM_GNU_ref_alt` or `DW_FORM_GNU_strp_alt`
references in the main DWARF are transparently resolved through the alt file.

### 3.3 Split DWARF — `.dwo` Files

Split DWARF handling lives in the lower-level libdw layer (not libdwfl). When
a consumer iterates over compilation units and encounters a skeleton unit
(identified by `DW_UT_skeleton` in DWARF 5, or a unit containing
`DW_AT_GNU_dwo_name` in DWARF 4), libdw must find and load the corresponding
split unit.

The algorithm is:

**Step 1 — Try a DWARF Package file (`.dwp`) first.**
Elfutils looks for `<path_to_elf>.dwp` — that is, the exact path of the
executable or shared library with `.dwp` appended. If this file exists, elfutils
opens it, verifies it has a `.debug_cu_index` or `.debug_tu_index` section, and
caches the result. If a DWP is found, the DWO ID from the skeleton unit is
looked up in the DWP's hash-table index (described in section 3.4). If the unit
is found in the DWP, the search is complete.

The DWP file is cached per-Dwarf handle: once a DWP has been opened (or its
absence noted), subsequent skeleton units from the same binary reuse the cached
result without re-probing the filesystem.

**Step 2 — Try `<elf_directory>/<dwo_name>`.**
Elfutils reads `DW_AT_dwo_name` (DWARF 5) or `DW_AT_GNU_dwo_name` (DWARF 4)
from the skeleton compilation unit DIE. It then tries to find the file by
prepending the directory containing the ELF binary. For example, if the binary
is `/usr/bin/myapp` and the DWO name is `src/main.dwo`, elfutils tries
`/usr/bin/src/main.dwo`.

**Step 3 — Try `<comp_dir>/<dwo_name>`.**
If Step 2 fails, elfutils reads `DW_AT_comp_dir` from the skeleton CU (the
compilation directory recorded by the compiler) and constructs a path by joining
`comp_dir` with the DWO name. For example, if `comp_dir` is
`/home/user/project/build` and the DWO name is `src/main.dwo`, the candidate
path is `/home/user/project/build/src/main.dwo`.

**Step 4 — DWO Lookup Callback.**
Elfutils provides a hook mechanism: applications can register a callback via
`dwarf_set_dwo_lookup()`. If the local filesystem searches (Steps 2 and 3) both
fail, elfutils invokes this callback, passing it the DWO name and the DWO ID.
The callback can locate the DWO file by any means — for example, by querying
a debuginfod server. libdwfl registers exactly such a callback when it
initializes DWARF for a module, allowing DWO files to be transparently fetched
from debuginfod.

**Validation**: After finding a candidate `.dwo` file, elfutils opens it, iterates
its compilation units, and looks for one whose 8-byte DWO ID matches the
skeleton's. If no matching unit is found, the file is rejected.

### 3.4 DWARF Package Files — `.dwp` Index

The `.dwp` file contains an index section (`.debug_cu_index` for compile units,
`.debug_tu_index` for type units). The index is structured as:

- A header with the version, section count, unit count, and slot count.
- A hash table of `slot_count` entries. Each slot holds an 8-byte unit ID
  (the DWO ID) and a 4-byte row index.
- A section offset table and a section size table, each with one row per
  unit and one column per section type (`.debug_info.dwo`, `.debug_str_offsets.dwo`,
  etc.).

Lookup uses **open-addressing with double hashing**:
- The primary hash is the lower 32 bits of the DWO ID.
- The secondary hash is `(dwo_id >> 32) | 1` (forced odd to guarantee the
  probe sequence covers all slots).
- Starting from `primary_hash % slot_count`, the probe walks the table by
  `secondary_hash` increments until it finds a slot with the matching ID
  (success) or an empty slot (unit not present).

Elfutils also works around a known bug in both GNU `dwp` and LLVM `dwp`: when
the `.debug_info.dwo` section within a DWP exceeds 4 GB, the offset values in
the index are silently truncated to 32 bits. Elfutils detects this condition and
reconstructs correct 64-bit offsets by walking the compilation unit headers in
sequence order and accumulating their sizes.

### 3.5 Environment Variables and Configuration

| Setting | Effect |
|---------|--------|
| `DEBUGINFOD_URLS` | Space-separated list of debuginfod server URLs for network-based lookups. |
| `debuginfo_path` (callback field) | Colon-separated search path for build-ID and debuglink lookups. Default: `":.debug:/usr/lib/debug"`. |
| `Dwfl_Callbacks.find_debuginfo` | Pointer to the debug-info-finding callback. Can be replaced entirely. |
| `dwarf_set_dwo_lookup()` | Registers a custom callback for DWO file discovery (used by libdwfl for debuginfod). |

---

## 4. libbacktrace

libbacktrace (by Ian Lance Taylor, originally for the Go runtime) provides
backtrace and symbol-to-source-location resolution. All of its debug info lookup
is in a single file (`elf.c`) with no external dependencies beyond libc and
optionally LZMA.

### 4.1 Separate Debug Files

When libbacktrace opens an ELF file and finds that it lacks DWARF sections (or
only has minimal ones), it attempts to locate a separate debug file. The search
proceeds in the following order:

**Step 1 — Build-ID.**
libbacktrace reads the `.note.gnu.build-id` section. If present, it converts
the build-ID to hex and constructs the path
`/usr/lib/debug/.build-id/XX/YYYYYY...YYY.debug` (where `XX` is the first byte
and the rest follows). The path `/usr/lib/debug` is **hardcoded** — there is no
way to change it at runtime or compile time (no environment variable, no
callback, no API).

An important difference from GDB and elfutils: **libbacktrace does not validate
the build-ID of the file it finds.** If a file exists at the expected path, it
is accepted on faith. The source code notes that GDB performs this validation
but libbacktrace chooses not to.

**Step 2 — `.gnu_debuglink`.**
If build-ID lookup fails (either no build-ID, or no file at the expected path),
libbacktrace reads the `.gnu_debuglink` section and extracts the filename and
CRC-32.

Before doing any path searching, libbacktrace resolves symbolic links in the
binary's path. This is important because if the binary was invoked via
`/proc/self/exe` (a symlink), the search paths need to be based on the real
location.

The search then tries three paths in order:
1. `<real_binary_dir>/<debuglink_filename>`
2. `<real_binary_dir>/.debug/<debuglink_filename>`
3. `/usr/lib/debug/<real_binary_dir>/<debuglink_filename>`

For each candidate, if it exists, libbacktrace computes the CRC-32 of the entire
file and compares it to the CRC stored in the `.gnu_debuglink` section. The file
is only accepted if the checksums match.

**Step 3 — `.gnu_debugaltlink` (supplementary file).**
If the opened debug file (or the original binary) contains a `.gnu_debugaltlink`
section, libbacktrace reads it to find the supplementary file. The section
contains a filename and a build-ID. libbacktrace uses the same path search
logic as `.gnu_debuglink` (the three candidate paths). The supplementary file
provides shared DWARF data referenced via `DW_FORM_GNU_ref_alt` and
`DW_FORM_GNU_strp_alt` forms.

**Step 4 — `.gnu_debugdata` (minidebuginfo) fallback.**
As a last resort, if no separate debug file was found, libbacktrace looks for a
`.gnu_debugdata` section in the original binary. If present, it decompresses the
LZMA-compressed content to obtain a minimal ELF image containing at least a
symbol table. This provides function names for backtraces even when no debug
packages are installed.

### 4.2 What libbacktrace Does NOT Support

- **No split DWARF** (`.dwo`/`.dwp`). libbacktrace only processes monolithic
  DWARF sections. If a binary uses `-gsplit-dwarf`, libbacktrace will see the
  skeleton units but cannot find or load the corresponding `.dwo` files.
- **No debuginfod.** There is no network-based fallback.
- **No configurable search paths.** The build-ID root and the debuglink search
  directories are all hardcoded.
- **No environment variables** affect the search.

---

## 5. Libunwind (GNU)

GNU libunwind is primarily a stack unwinding library. It reads debug information
for the limited purpose of finding unwind tables and resolving procedure names.

### 5.1 Separate Debug Files

When libunwind needs to resolve a procedure name and the loaded ELF image does
not contain a symbol table, it searches for a separate debug file.

**Step 1 — Build-ID.**
Libunwind parses the ELF program headers (not section headers) looking for
`PT_NOTE` segments that contain a GNU build-ID note (`NT_GNU_BUILD_ID`). If
found, it constructs the path `/usr/lib/debug/.build-id/XX/YYY...YYY.debug`.
Like libbacktrace, this path is **hardcoded** with no configuration option.

Libunwind does **not validate** the build-ID of the found file — it opens it
without any integrity check.

**Step 2 — `.gnu_debuglink`.**
If build-ID lookup fails, libunwind reads the `.gnu_debuglink` section and
searches three paths:
1. `<binary_dir>/<debuglink_filename>`
2. `<binary_dir>/.debug/<debuglink_filename>`
3. `/usr/lib/debug/<binary_dir>/<debuglink_filename>`

The third path (the global debug directory) is only tried when the binary is a
local file (not a remote/coredump context where `is_local` is false).

Libunwind does **not perform CRC validation** on the found file. It simply opens
the first file that exists at any of the candidate paths.

**Step 3 — `.gnu_debugdata` (minidebuginfo).**
If the binary contains a `.gnu_debugdata` section, libunwind decompresses it
with LZMA (when compiled with LZMA support) and uses the resulting minimal ELF
for symbol resolution.

### 5.2 What Libunwind Does NOT Support

- **No split DWARF** (`.dwo`/`.dwp`).
- **No `.gnu_debugaltlink`** (supplementary files).
- **No debuginfod.**
- **No configurable search paths or environment variables.**
- **No CRC or build-ID validation** of found debug files.

---

## 6. LLVM libunwind

LLVM's libunwind is a minimal, in-process-only stack unwinder. Its design
philosophy is fundamentally different from the other tools in this survey: it
performs **zero file I/O** for debug data.

### 6.1 How It Works

LLVM libunwind finds unwind information exclusively from data that is already
mapped into the process address space:

- On **Linux**, it calls `dl_iterate_phdr()` to enumerate all loaded shared
  objects and executables. For each one, it looks for a `PT_GNU_EH_FRAME`
  program header, which points to the `.eh_frame_hdr` section (and indirectly
  to `.eh_frame`). On newer glibc versions, it uses the faster
  `_dl_find_object()` API instead.

- On **macOS**, it uses `_dyld_find_unwind_sections()` to get compact unwind
  info from the dyld shared cache.

- On **bare-metal / embedded**, it uses linker-defined symbols
  (`__eh_frame_start`, `__eh_frame_end`) to locate the unwind tables.

It also supports dynamic registration for JIT-compiled code through
`__unw_add_find_dynamic_unwind_sections()`.

### 6.2 What LLVM libunwind Does NOT Support

Everything related to external debug files:
- No separate debug file lookup of any kind.
- No build-ID, no `.gnu_debuglink`.
- No `.dwo`/`.dwp`.
- No debuginfod.
- No `.gnu_debugdata`.
- No file I/O whatsoever for debug data.

This is by design. LLVM libunwind is purpose-built for fast, in-process stack
unwinding during exception handling and crash reporting, where all necessary
unwind information (`.eh_frame`) is expected to be present in the loaded
binary's mapped memory.

---

## 7. glibc backtrace()

The glibc `backtrace()` and `backtrace_symbols()` functions are intentionally
minimal.

### 7.1 How It Works

**Unwinding**: `backtrace()` dynamically loads `libgcc_s.so` at runtime and calls
`_Unwind_Backtrace()`, which walks the stack using `.eh_frame` call frame
information. It does not read DWARF `.debug_frame` or any debug sections — only
the exception-handling unwind tables that are present in every non-stripped
binary (and even many stripped ones, since `.eh_frame` is an allocated section
needed at runtime for C++ exceptions).

**Symbol resolution**: `backtrace_symbols()` calls `_dl_addr()` (the internal
implementation behind `dladdr()`), which looks up symbols from the loaded
binary's dynamic symbol table (`.dynsym`). This is the same symbol table used
by the dynamic linker and is always present in shared libraries. The function
returns strings in the format `filename(symbol+offset) [address]`.

### 7.2 What glibc backtrace Does NOT Support

- **No debug information lookup whatsoever.** It does not read DWARF, does not
  open any files beyond what is already loaded, and does not look for external
  debug files.
- **No build-ID, debuglink, `.dwo`/`.dwp`, debuginfod, or any other mechanism.**
- Symbol names come exclusively from `.dynsym`, the runtime dynamic symbol
  table. If a function is not exported (static functions, inlined functions),
  `backtrace_symbols()` cannot name it.

This is by design for crash-time diagnostics: the function must work from signal
handlers, with minimal memory allocation, and no dependency on debug packages.

---

## 8. Rust addr2line (gimli-rs)

The Rust `addr2line` crate (part of the gimli project) uses the `gimli` DWARF
parser and the `object` crate for ELF/Mach-O/PE parsing. It has strong split
DWARF support but weak separate-debug-file support.

### 8.1 Split DWARF — `.dwo` Files

When the `addr2line` library encounters a skeleton compilation unit (one with a
DWO ID), it needs the caller to provide the split DWARF data. It uses a
**callback-based lazy loading pattern**: the library emits a `SplitDwarfLoad`
request containing the DWO name, the compilation directory, and the DWO ID,
and the caller is responsible for locating and loading the file.

The included `Loader` implementation (used by the `addr2line` binary) handles
this as follows:

1. **Check the DWP file** — If a DWARF Package file was found during
   initialization (see 8.2), look up the DWO ID in the package index. If found,
   use the data from the package.

2. **Fall back to filesystem** — Construct the path `comp_dir / dwo_name`
   (joining the compilation directory with the DWO name from the attribute) and
   attempt to open it. There is no additional search path logic — only the single
   `comp_dir`-based path is tried.

3. **Validate the DWO ID** — After loading, the DWO ID from the loaded file is
   compared against the expected ID from the skeleton unit. Mismatches are
   rejected.

The lazy loading is per-compilation-unit: each skeleton CU triggers its own
independent DWO lookup on first access. The `Loader` caches the DWP file, but
individual DWO files are loaded on demand.

### 8.2 DWARF Package — `.dwp` Files

During initialization, the `Loader` constructs a DWP path from the binary path:
- If the binary path has an extension (e.g., `myapp.debug`), `.dwp` is appended,
  producing `myapp.debug.dwp`.
- If the binary path has no extension (e.g., `myapp`), the extension is set to
  `.dwp`, producing `myapp.dwp`.

If this file exists, it is loaded and its index parsed using `gimli::DwarfPackage`.
The DWP is checked **before** individual `.dwo` files for every skeleton unit.

### 8.3 Supplementary Files

Supplementary (alt/dwz) files are supported, but only when the caller explicitly
provides the path. The `Loader::new_with_sup(path, sup_path)` constructor takes
an optional supplementary file path. The command-line `addr2line` binary accepts
a `--sup` flag. Once loaded, references using `DW_FORM_ref_sup4` / `DW_FORM_ref_sup8`
are resolved through the supplementary file.

There is **no automatic discovery** from the `.gnu_debugaltlink` section — the
source code contains a `// TODO: use debuglink and debugaltlink` comment.

### 8.4 macOS dSYM Support

On macOS, the `Loader` extracts the UUID from the binary's Mach-O load commands,
then searches for `.dSYM` bundles adjacent to the binary. Each `.dSYM` bundle's
`Contents/Resources/DWARF/` directory is checked for a file whose UUID matches.
This is automatic — no manual path is needed.

### 8.5 What Rust addr2line Does NOT Support

- **No automatic `.gnu_debuglink` discovery** — marked as TODO in the source.
- **No build-ID based lookup** — no search of `.build-id/` directories.
- **No debuginfod** — no network-based fallback.
- **No configurable search paths or environment variables.**
- Only one filesystem path is tried for `.dwo` files (`comp_dir + dwo_name`).

---

## 9. GNU addr2line (binutils/BFD)

GNU addr2line uses the BFD (Binary File Descriptor) library for all ELF and
DWARF operations. BFD's debug info lookup is more limited than GDB's own (even
though they share a repository).

### 9.1 Separate Debug Files

BFD's DWARF reader (`_bfd_dwarf2_slurp_debug_info()`) attempts to locate
separate debug files when the primary binary lacks DWARF sections. It tries two
methods in order:

**Method 1 — Build-ID.**
BFD reads the `.note.gnu.build-id` section and constructs the standard
`.build-id/XX/YYYYYY...YYY.debug` path under the global debug directory.
Validation is done by reading the build-ID from the candidate file and comparing
it against the expected value.

**Method 2 — `.gnu_debuglink`.**
BFD reads the `.gnu_debuglink` section to obtain a filename and CRC-32. It then
searches five locations in order:

1. `<binary_dir>/<debuglink_file>` — Same directory as the binary.
2. `<binary_dir>/.debug/<debuglink_file>` — A `.debug/` subdirectory.
3. `/usr/lib/debug/<binary_dir>/<debuglink_file>` — The primary global debug
   directory with the binary's full directory path appended.
4. `/usr/lib/debug/usr/<binary_dir>/<debuglink_file>` — A secondary global root
   (this handles some distribution quirks where `/usr` is prepended).
5. `<debug-file-directory>/<binary_dir>/<debuglink_file>` — A configurable
   directory that can be set at BFD compile time.

For each candidate, BFD performs **full CRC-32 validation**: it reads the entire
file in 8 KB chunks, computes the CRC-32, and compares it against the stored
value. Only if the checksums match is the file accepted. This is the most
conservative validation approach of any tool in this survey.

### 9.2 What GNU addr2line Does NOT Support

- **No split DWARF** (`.dwo`/`.dwp`). BFD's DWARF reader handles only
  monolithic DWARF. Binaries compiled with `-gsplit-dwarf` will show skeleton
  compilation units but no type, variable, or line information.
- **No debuginfod.** There is no network-based fallback.
- **No `.gnu_debugaltlink`** support in BFD's DWARF reader.
- **No `.gnu_debugdata`** support.
- **No runtime-configurable search paths.** The debug-file-directory is a
  compile-time option for BFD.

---

## 10. Linux perf

Linux perf has the most elaborate DSO (Dynamic Shared Object) search logic of
any tool in this survey, driven by the need to handle offline analysis of
recorded performance data across many distributions. It also maintains a local
build-ID cache.

### 10.1 The Search Strategy

When perf needs symbols for a DSO, it iterates through a prioritized list of
"binary types," trying each one in order. The strategy is **merge-based, not
first-match**: perf keeps searching until it has found both a source for the
full symbol table (`.symtab`) and a source for runtime symbols (`.dynsym`).
Once both are found, or the list is exhausted, the search stops. This means a
later entry can provide runtime symbols even if an earlier entry already provided
the full symbol table.

The search order for user-space binaries is:

1. **`/proc/kallsyms`** — Only for kernel contexts; provides kernel symbol names
   without needing vmlinux.

2. **Java JIT maps** — `/tmp/perf-<pid>.map` files generated by Java VMs.

3. **`.gnu_debuglink`** — Read the `.gnu_debuglink` section from the DSO and
   search for the named file in the standard three locations (binary's directory,
   `.debug/` subdirectory, `/usr/lib/debug/` global directory).

4. **Build-ID cache (binary)** — Look in the local build-ID cache at
   `~/.debug/.build-id/XX/YYYYYY...` for a cached copy of the binary itself.

5. **Build-ID cache (debuginfo)** — Look in the same cache structure but for
   the debug variant (`~/.debug/.build-id/XX/YYYYYY.../debug`).

   If the build-ID cache misses and debuginfod is compiled in, perf falls back
   to querying debuginfod servers at this point. Downloaded files are stored in
   the debuginfod client cache (`~/.cache/debuginfod_client/`), not in perf's
   own build-ID cache.

6. **Fedora-style debuginfo** — `/usr/lib/debug/<full_path>.debug`. Fedora and
   RHEL name their debug files by appending `.debug` to the full path under
   `/usr/lib/debug/`. For `/usr/lib/libfoo.so.6`, the debug file would be
   `/usr/lib/debug/usr/lib/libfoo.so.6.debug`.

7. **Ubuntu-style debuginfo** — `/usr/lib/debug/<full_path>` (without the
   `.debug` suffix). Debian and Ubuntu use the same directory structure but do
   not append `.debug` to the filename.

8. **Build-ID debuginfo (distro)** — `/usr/lib/debug/.build-id/XX/YYYY.debug`.
   This is the distribution-provided build-ID-indexed debug file, as opposed to
   perf's local cache.

9. **`.gnu_debugdata` (minidebuginfo)** — Decompress the LZMA-compressed
   minidebuginfo from the `.gnu_debugdata` section for minimal symbol names.

10. **Original binary** — Fall back to whatever symbols are in the original
    binary at its system path.

11. **Kernel modules** — `/lib/modules/<version>/kernel/...` with support for
    compressed `.ko.xz`, `.ko.gz`, `.ko.zst` modules.

12. **OpenEmbedded/Yocto debuginfo** — `<binary_dir>/.debug/<basename>`. The
    Yocto Project places debug files in a `.debug/` subdirectory using only the
    basename (not the full path hierarchy).

13. **Mixed-up Ubuntu debuginfo** — `/usr/lib/debug/lib/...` (for binaries in
    `/usr/lib/` where Ubuntu sometimes strips the `/usr` prefix in the debug
    hierarchy).

### 10.2 Build-ID Cache

Perf maintains a local cache of binaries and debug info indexed by build-ID.
This is critical for offline analysis: when you run `perf record`, perf captures
the build-IDs of all DSOs that were active during recording, and then during
`perf report` it needs to find those exact binaries even if the system has been
updated since then.

The cache lives at `~/.debug/` (overridable via `PERF_BUILDID_DIR` or perf
configuration). Its structure is:

```
~/.debug/
├── .build-id/
│   └── ab/
│       └── cdef123456...    ← symlink to actual cached file
└── usr/lib/libfoo.so.6/
    └── abcdef123456.../
        ├── elf              ← cached copy of the binary
        └── debug            ← cached copy of the debug file
```

The `.build-id/` entries are **relative symlinks** pointing to the
actual-path-based entries. When perf records, it attempts hard-linking the binary
into the cache (to save space); if hard-linking fails (e.g., cross-filesystem),
it falls back to copying.

`perf buildid-cache --add <binary>` manually adds a binary to the cache.
`perf buildid-cache --list` lists all cached build-IDs.

### 10.3 Kernel Symbol Handling

Kernel debug info follows a completely separate search path:

1. User-specified vmlinux path (`--vmlinux` option or `symbol_conf.vmlinux_name`).
2. Build-ID cache lookup for vmlinux.
3. Standard vmlinux locations (`/boot/vmlinux-<version>`, etc.).
4. `/proc/kallsyms` as a fallback (provides symbol names but no debug info).
5. `/proc/kcore` for live kernel memory.

Kernel modules are searched through their standard `/lib/modules/` paths, with
support for compressed modules (`.ko.xz`, `.ko.gz`, `.ko.zst` are decompressed
on the fly).

### 10.4 debuginfod Integration

debuginfod is **not a separate slot** in the main search order. Instead, it is
invoked as a **fallback within the build-ID cache lookup** — specifically when
the build-ID cache misses. This means debuginfod effectively sits between
"build-ID cache" and "distro debuginfo paths" in the priority order. Downloaded
files are managed by the debuginfod client library's own cache, not by perf's
build-ID cache.

### 10.5 Validation

Perf's validation is moderate:
- **Build-ID matching**: Done for vmlinux and when adding to the build-ID cache.
  For regular DSO lookups, the build-ID stored in perf.data is compared against
  the build-ID of the found file.
- **ELF header validation**: Every candidate is opened and its ELF headers
  parsed; corrupt or non-ELF files are rejected.
- **No CRC-32 validation**: Even when `.gnu_debuglink` is used, perf does not
  verify the CRC.

### 10.6 Split DWARF

Perf's own DSO layer does **not** implement `.dwo`/`.dwp` search. However, perf
links against elfutils' libdw for features like `perf probe` (which places
tracepoints based on DWARF variable locations). In those contexts, elfutils'
full split DWARF support (described in section 3) is available.

### 10.7 Environment Variables

| Variable | Effect |
|----------|--------|
| `DEBUGINFOD_URLS` | Space-separated list of debuginfod server URLs. |
| `HOME` | Base for the `~/.debug` build-ID cache. |
| `PERF_BUILDID_DIR` | Override the build-ID cache directory. |

---

## 11. GDB

GDB has the most mature and configurable debug info lookup, with full support
for every mechanism.

### 11.1 Separate Debug Files

When GDB loads a binary and needs its debug info, it tries two methods in
sequence. The search is controlled by the `debug-file-directory` GDB setting,
which defaults to `/usr/lib/debug` and can contain multiple colon-separated
directories.

**Method 1 — Build-ID (tried first).**
GDB checks the `.note.gnu.build-id` section. If present, for each directory in
`debug-file-directory`, it constructs the path
`<dir>/.build-id/XX/YYYYYY...YYY.debug` and checks if the file exists.
Validation: GDB opens the candidate and reads its build-ID to confirm it matches.

**Method 2 — `.gnu_debuglink` (tried if build-ID lookup fails).**
GDB reads the `.gnu_debuglink` section to get a filename and CRC-32. It then
searches, for each directory in `debug-file-directory`:
1. `<binary_dir>/<debuglink_file>`
2. `<binary_dir>/.debug/<debuglink_file>`
3. `<debug-file-dir>/<binary_dir>/<debuglink_file>`

GDB computes the CRC-32 of each candidate file and compares it against the
stored value.

**Method 3 — debuginfod (tried if local searches fail).**
If `DEBUGINFOD_URLS` is set, GDB queries debuginfod servers using the build-ID.
Downloaded files are cached in `$HOME/.cache/debuginfod_client/`.

### 11.2 Split DWARF — `.dwo` Files

GDB has its own DWARF reader (independent of BFD's) that fully supports split
DWARF. When GDB encounters a skeleton compilation unit, it extracts
`DW_AT_GNU_dwo_name` (DWARF 4) or `DW_AT_dwo_name` (DWARF 5) and
`DW_AT_comp_dir`.

**DWP is checked first.** GDB looks for a DWARF Package file before trying
individual DWO files (see 11.3). If the unit is found in a DWP, individual DWO
search is skipped entirely.

If no DWP is available (or the unit is not in the DWP), the DWO search proceeds:

1. **`comp_dir / dwo_name`** — GDB joins the compilation directory with the DWO
   name. If `comp_dir` is relative, it is resolved relative to the current
   working directory (not the binary's location — this differs from LLDB). The
   file is searched relative to the CWD.

2. **`debug-file-directory / dwo_name`** — If the first attempt fails and
   `debug-file-directory` is set, GDB searches for the DWO file (using just its
   basename) under each directory in `debug-file-directory`. The rationale is
   that the original `comp_dir` may be stale (the source tree may have moved),
   so the debug file directory serves as a fallback.

3. **debuginfod** — Work is in progress to support querying debuginfod servers
   for DWO files by DWO ID, but this is not yet fully integrated.

Note that GDB does NOT implicitly search the current working directory when
looking in `debug-file-directory`. If you want `.` searched, it must be
explicitly added to the directory.

### 11.3 DWARF Package — `.dwp` Files

GDB constructs the DWP filename by appending `.dwp` to the executable's full
path (e.g., `/usr/bin/myapp` → `/usr/bin/myapp.dwp`).

The search:
1. Try the full DWP path, searching relative to the current working directory.
2. If that fails, strip the directory component and search just the basename
   (e.g., `myapp.dwp`) in each directory listed in `debug-file-directory`. This
   fallback exists because GDB may have `realpath()`-resolved the executable's
   location, losing the original path where the DWP might reside.

**DWP takes absolute priority over DWO.** When a DWP file is found, GDB does
not look for individual `.dwo` files at all. All split units are resolved through
the package index.

### 11.4 Supplementary Files (`.gnu_debugaltlink`)

GDB fully supports dwz-compressed supplementary files. When it encounters a
`.gnu_debugaltlink` section, it reads the filename and build-ID, locates the
supplementary file using the standard separate-debug-file search, and links it
for transparent resolution of alt-form references.

### 11.5 `.gnu_debugdata` (Minidebuginfo)

GDB supports minidebuginfo: if a binary contains a `.gnu_debugdata` section,
GDB decompresses the LZMA data and uses the embedded minimal symbol table for
function names when full debug info is unavailable.

### 11.6 Configuration

| Setting | Default | Effect |
|---------|---------|--------|
| `set debug-file-directory` | `/usr/lib/debug` | Colon-separated list of directories for all debug file searches (separate debug, DWO, DWP). |
| `DEBUGINFOD_URLS` (env) | *(none)* | Debuginfod server URLs. |
| `set debuginfod enabled` | `ask` | Controls debuginfod usage (`on`, `off`, `ask`). |

---

## 12. LLDB

LLDB uses a plugin-based architecture where different "SymbolLocator" plugins
handle different strategies for finding debug files. The plugin chain is tried
in registration order, and the first plugin to return a result wins.

### 12.1 Separate Debug Files

LLDB's ELF object file parser extracts two pieces of identification from the
binary:
- The **build-ID** (from the `NT_GNU_BUILD_ID` note in the `.note.gnu.build-id`
  section), stored as the module's UUID.
- The **debuglink** filename and CRC-32 (from the `.gnu_debuglink` section).

These are then used by the SymbolLocator plugin chain:

**Plugin 1 — DebugSymbols (macOS only).**
Uses macOS Spotlight and the DebugSymbols framework to locate `.dSYM` bundles.
Not relevant on Linux.

**Plugin 2 — Debuginfod.**
If compiled in, the debuginfod plugin queries servers listed in `DEBUGINFOD_URLS`
using the module's build-ID. It can find both executables and debug info files.
The cache is managed by LLVM's debuginfod client library.

**Plugin 3 — Default (local filesystem search).**
The default plugin performs a systematic local filesystem search:

1. If the user specified a symbol file path and it is absolute and exists, use it.
2. Search the **module's directory** (the directory containing the binary).
3. Search the **current working directory** (only if external lookup is enabled).
4. Search the **platform's debug directory**:
   - Linux: `/usr/lib/debug`
   - NetBSD: `/usr/libdata/debug`
   - FreeBSD: `$LOCALBASE/lib/debug` (usually `/usr/local/lib/debug`)
5. **Build-ID directory lookup**: Construct
   `/usr/lib/debug/.build-id/XX/YYYY...YYYY.debug` from the module's UUID.
6. **`.debug/` subdirectories**: For each search path, also check a `.debug/`
   subdirectory.
7. **Full path mirroring**: Try the full directory hierarchy of the binary
   reproduced under the debug directory.

Validation is by UUID (build-ID) matching: any candidate whose UUID does not
match the module's is rejected.

### 12.2 Split DWARF — `.dwo` Files

When LLDB's DWARF symbol file plugin encounters a skeleton compilation unit, it
reads the DWO name from `DW_AT_GNU_dwo_name` (tried first, for DWARF 4
compatibility) or `DW_AT_dwo_name` (DWARF 5).

**DWP is checked first** (see 12.3). If a DWP file was found and contains the
needed unit, no DWO search is performed.

If no DWP is available, the DWO file search proceeds:

1. **Try the DWO name as-is.** If it is an absolute path, open it directly. If
   relative, try it relative to the current working directory.

2. **Try `comp_dir / dwo_name`.** Read `DW_AT_comp_dir` from the skeleton CU.
   - If `comp_dir` is **absolute**, use it directly as the base directory.
   - If `comp_dir` is **relative**, resolve it relative to the **binary's
     location** (not the CWD — this is a key difference from GDB, which resolves
     relative to CWD).
   Then append the DWO name and check if the resulting path exists.

3. **Try user-configured search paths.** LLDB has configurable debug file search
   paths via the target settings. For each search path, try the DWO name within
   that directory.

DWO loading is **lazy and per-compilation-unit**: each skeleton CU triggers its
own DWO lookup on first access (e.g., when a symbol in that CU is first queried).
Results are cached per-unit, so subsequent accesses to the same CU do not
re-probe the filesystem.

### 12.3 DWARF Package — `.dwp` Files

LLDB's DWP search is more thorough than GDB's. It constructs multiple candidate
DWP paths:

1. **Module path + `.dwp`** — Append `.dwp` to the path of the executable or
   shared library (e.g., `/usr/bin/myapp.dwp`).

2. **Symbol file path + `.dwp`** — If the symbol file is different from the
   module (e.g., a separate `.debug` file was loaded), append `.dwp` to that
   path (e.g., `/usr/lib/debug/usr/bin/myapp.debug.dwp`).

3. **Basename without extension + `.dwp`** — Strip the extension from the
   filename and replace it with `.dwp`. For example, `myapp.debug` becomes
   `myapp.dwp`. This handles the common case where the user has `myapp.dwp`
   alongside `myapp.debug`.

4. **User-configured search paths** — Each directory in the target's debug file
   search paths is tried with the DWP filename.

5. **Debuginfod fallback (two-phase)** — If no local DWP file is found:
   - **Phase 1**: The SymbolLocator plugin chain is queried without a UUID
     (since DWP files do not contain build-IDs, the UUID check is skipped).
     The Default plugin searches local paths.
   - **Phase 2**: The module's UUID is attached to the request, and the plugin
     chain is queried again. This time the Debuginfod plugin can use the UUID
     to find the DWP on a remote server.

The DWP file is loaded at most once per SymbolFileDWARF instance, protected by
a thread-safe once-flag mechanism. Subsequent calls return the cached result
(which may be null if no DWP was found).

An important detail: **UUID validation is skipped for DWP files.** Neither
`llvm-dwp` nor GNU `dwp` embed build-IDs in the DWP output, so there is no UUID
to compare against. LLDB accepts any DWP file found at the expected path.

### 12.4 What LLDB Does NOT Support

- **No `.gnu_debugaltlink`** (supplementary/dwz files). LLDB does not currently
  handle `DW_FORM_GNU_ref_alt` or `DW_FORM_GNU_strp_alt`.
- **No `.gnu_debugdata`** (minidebuginfo).

### 12.5 Configuration

| Setting | Effect |
|---------|--------|
| `DEBUGINFOD_URLS` (env) | Debuginfod server URLs. |
| `plugin.symbol-locator.debuginfod.symbol-cache-path` | Override debuginfod cache location. |
| Target debug file search paths | Additional directories to search for DWO/DWP files. |
| `settings set plugin.symbol-file.dwarf.comp-dir-symlink-paths` | Additional symlink resolution paths for `comp_dir`. |

---

## 13. Heaptrack

Heaptrack (KDE's heap memory profiler) **delegates all debug information lookup
to elfutils/libdwfl**. It has no debug file search logic of its own.

### 13.1 How It Works

Heaptrack initializes a `Dwfl` handle with the standard elfutils callbacks:
- `dwfl_build_id_find_elf` for locating executables by build-ID.
- `dwfl_standard_find_debuginfo` for the complete separate-debug-file search
  pipeline (build-ID, debuglink, symlink resolution, debuginfod).
- `dwfl_offline_section_address` for section address computation.

It then registers modules via `dwfl_report_elf()` and looks up addresses via
`dwfl_addrmodule()` and the standard libdw DWARF-walking APIs.

### 13.2 What Heaptrack Adds

On top of elfutils' lookup, heaptrack adds:
- **DWARF caching**: A custom cache layer (`DwarfDieCache`, `CuDieRangeMapping`)
  that accelerates repeated lookups by caching the mapping from address ranges
  to DWARF DIEs.
- **Symbol caching**: Caches resolved symbol names to avoid redundant DWARF walks.
- **Demangling**: C++, Rust, and D language symbol demangling.

### 13.3 Capabilities

Since heaptrack uses elfutils' standard callbacks, it inherits **all of elfutils'
debug info lookup capabilities**, including:
- Build-ID based lookup with configurable search paths.
- `.gnu_debuglink` with CRC validation.
- `.gnu_debugaltlink` (dwz supplementary files).
- Split DWARF (`.dwo` files) with DWO lookup callback.
- DWARF Package (`.dwp`) files.
- debuginfod integration (if the system's elfutils was built with debuginfod support).

---

## 14. Comparison and Analysis

### 14.1 The Canonical debuglink Search Algorithm

Nearly every tool that supports `.gnu_debuglink` implements the same three-path
search (originated by GDB):

```
<binary_dir>/<debuglink_file>
<binary_dir>/.debug/<debuglink_file>
/usr/lib/debug/<binary_dir>/<debuglink_file>
```

**Variations across tools:**

- **GNU addr2line (BFD)** adds two extra paths:
  `/usr/lib/debug/usr/<binary_dir>/<debuglink_file>` and a compile-time-configurable
  `<debug-file-directory>/<binary_dir>/<debuglink_file>`.
- **Linux perf** replaces the generic algorithm with a typed dispatch system
  that also handles Fedora, Ubuntu, OpenEmbedded, and mixed-path conventions.
- **LLDB** adds platform-specific debug directories (NetBSD's `/usr/libdata/debug`,
  FreeBSD's `$LOCALBASE/lib/debug`).
- **Elfutils** generalizes the algorithm with a configurable colon-separated
  search path and support for per-entry CRC control via `+`/`-` prefixes.

### 14.2 Build-ID Directory Layout

All tools use the same directory layout convention:
```
<root>/.build-id/XX/YYYYYY...YYY.debug
```
where `XX` is the first byte in hex and `YYY...` is the remainder.

The difference is in what `<root>` is and whether it can be changed:

| Tool | Root directory | Configurable? |
|------|---------------|---------------|
| Elfutils | Each entry in `debuginfo_path` | Yes, via callback |
| libbacktrace | `/usr/lib/debug` | No |
| Libunwind | `/usr/lib/debug` | No |
| Linux perf | `~/.debug` (local cache) + `/usr/lib/debug` (distro) | Yes (`PERF_BUILDID_DIR`) |
| GDB | Each dir in `debug-file-directory` | Yes (`set debug-file-directory`) |
| LLDB | `/usr/lib/debug` + search paths | Yes (target settings) |
| GNU addr2line | Compile-time default | No (at runtime) |

### 14.3 Split DWARF: Who Supports It and How

Only four tools natively support split DWARF:

**Elfutils**: Checks DWP first (at `<binary>.dwp`), then tries `<binary_dir>/<dwo_name>`,
then `<comp_dir>/<dwo_name>`, then a registered callback (used for debuginfod).
Unique features: the DWO callback mechanism and >4GB DWP offset reconstruction.

**GDB**: Checks DWP first (at `<binary>.dwp` then `<debug-file-dir>/<basename>.dwp`),
then tries `<comp_dir>/<dwo_name>` relative to CWD, then tries `<debug-file-dir>/<dwo_basename>`.
DWP takes absolute priority — if found, DWO search is skipped.

**LLDB**: Checks DWP first (at `<binary>.dwp`, `<symbol_file>.dwp`, `<basename_no_ext>.dwp`,
and search paths; with debuginfod fallback). Then tries the DWO name as-is,
then `<comp_dir>/<dwo_name>` resolved relative to the binary's location (not CWD),
then user-configured search paths. Loading is lazy and per-compilation-unit.

**Rust addr2line**: Checks DWP first (at `<binary>.dwp`), then tries
`<comp_dir>/<dwo_name>`. Only one path is tried for DWO files — the simplest
implementation.

**Key behavioral difference**: GDB resolves relative `comp_dir` relative to the
**current working directory**. LLDB resolves it relative to the **binary's
location on disk**. This matters when the binary has been moved after compilation.

### 14.4 DWP Search Locations

| Tool | Primary path | Additional paths | debuginfod fallback |
|------|-------------|-----------------|-------------------|
| Elfutils | `<binary>.dwp` | Via DWO callback | Yes (via callback) |
| GDB | `<binary>.dwp` | `<debug-file-dir>/<basename>.dwp` | In progress |
| LLDB | `<binary>.dwp` | `<sym_file>.dwp`, `<base_no_ext>.dwp`, search paths | Yes (two-phase) |
| Rust addr2line | `<binary>.dwp` | None | No |

### 14.5 Validation Rigor

How thoroughly each tool validates that a found debug file is the correct one:

| Tool | Build-ID match | CRC-32 check | DWO ID match | Notes |
|------|:--------------:|:------------:|:------------:|-------|
| Elfutils | Yes | Yes (configurable) | Yes | Most thorough; CRC can be forced or disabled per path entry |
| GDB | Yes | Yes | Yes | Full validation of all methods |
| LLDB | Yes (UUID) | No | Yes | Skips UUID for DWP files since they lack build-IDs |
| GNU addr2line (BFD) | Yes | Yes | N/A | Reads entire file to compute CRC |
| libbacktrace | **No** | Yes | N/A | Deliberately skips build-ID validation |
| Libunwind | **No** | **No** | N/A | No validation at all — accepts first file that exists |
| Linux perf | Yes | **No** | N/A | Validates build-ID but not CRC |

Libunwind is the least strict: it accepts any file found at the expected path
without checking that it actually corresponds to the binary being analyzed. This
could silently produce wrong results if a stale debug file is present.

### 14.6 debuginfod Adoption

| Tool | Status | Notes |
|------|--------|-------|
| Elfutils | Native (reference implementation) | Client library is part of elfutils itself |
| GDB | Full | Configurable via `set debuginfod enabled` |
| LLDB | Full | Via SymbolLocator plugin; uses LLVM's debuginfod client |
| Linux perf | Full | Fallback within build-ID cache lookup |
| libbacktrace | None | |
| Libunwind | None | |
| Rust addr2line | None | |
| GNU addr2line (BFD) | None | |

### 14.7 Tools That Delegate

| Tool | Delegates to | What it inherits |
|------|-------------|-----------------|
| Heaptrack | elfutils/libdwfl | Everything: build-ID, debuglink, split DWARF, DWP, debuginfod, dwz |
| Linux perf (deep DWARF) | elfutils/libdw | Split DWARF support when using `perf probe` |
| glibc backtrace | libgcc `.eh_frame` | Nothing — no debug info at all |
| LLVM libunwind | dynamic linker | Nothing — in-process `.eh_frame` only |

### 14.8 Notable Observations

1. **libbacktrace's deliberate choice not to validate build-IDs** is documented
   in its source code: it notes that GDB performs this validation but considers
   it unnecessary for libbacktrace's use case (worst case: you get the wrong
   file names in a backtrace, not a crash).

2. **Libunwind performs no validation at all** — no build-ID check, no CRC check.
   It will happily use a completely unrelated debug file if one happens to exist
   at the expected path.

3. **GDB and LLDB disagree on how to resolve relative `comp_dir`** for split
   DWARF. GDB resolves relative to the current working directory; LLDB resolves
   relative to the binary's directory. This means the same binary with split
   DWARF can work in one debugger and fail in the other depending on where the
   debugger is invoked from.

4. **Rust addr2line has the best DWO/DWP support but the worst separate-debug
   support** — it cannot automatically discover separate debug files at all (no
   build-ID lookup, no debuglink). This is explicitly noted as a TODO.

5. **Linux perf is unique in maintaining a local build-ID cache** (`~/.debug/`)
   that stores copies of binaries and debug info. No other tool in this survey
   maintains a persistent cache (debuginfod has its own cache, but that is
   managed by the debuginfod client library, not by the tools themselves).

6. **Elfutils is unique in providing a DWO lookup callback mechanism**. This
   allows library consumers to plug in arbitrary DWO search strategies. libdwfl
   uses this to wire in debuginfod support, but any application could register
   its own callback.

7. **Elfutils is the only tool that works around the >4GB DWP offset truncation
   bug** in GNU and LLVM's `dwp` tools. Other tools will silently produce
   incorrect results when processing a DWP file whose `.debug_info.dwo` section
   exceeds 4 GB.

8. **LLDB's two-phase DWP search via debuginfod** (first without UUID, then with)
   is a clever workaround for the fact that DWP files lack build-IDs: the first
   phase finds local files by name, and the second phase enables the debuginfod
   plugin to use the UUID for remote lookup.

9. **Linux perf's merge strategy** (continuing to search even after finding a
   usable symbol source) is unique. All other tools use a simple "first match
   wins" approach. Perf continues searching until it has found both a full
   symbol table source and a runtime symbol source, since these may come from
   different files.

10. **LLDB is the only tool with platform-specific debug directories** beyond the
    standard `/usr/lib/debug`. It handles NetBSD (`/usr/libdata/debug`),
    FreeBSD (`$LOCALBASE/lib/debug`), and macOS (dSYM via Spotlight) natively
    through its plugin architecture.
