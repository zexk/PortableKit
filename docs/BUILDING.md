# Building

How a port is built, where the time goes, and how to work on the code without paying for a full build each time.

You need your own copy of the game as a disc image. Nothing else from it is needed, and nothing from it is ever committed.

## The framework on its own

```bash
cmake -S . -B out -G Ninja
cmake --build out -j2
ctest --test-dir out
```

Minutes, and no game data. It builds `psp_analyze`, `psp_recomp`, `dump_function`,
the runtime, and the tests that need no game. The `portablekit_host_check`
target also compiles and links the backend-independent host against a stand-in
profile. The renderer and interface still need a port build before calling a
system-layer change done.

## A port

A port's repository adds this one as a subdirectory:

```bash
git clone --recursive https://github.com/TeamGDB/<Port>.git
cd <Port>
cmake -S . -B out -G Ninja
cmake --build out -j2
```

Framework tests default off when PortableKit is added as a subdirectory, so
each edition builds its game without also compiling and linking the test
programs and two extra copies of the host layer. To include them, configure
with `-DPSPRECOMP_BUILD_TESTS=ON`. Existing build directories retain their
cached setting; use `-DPSPRECOMP_BUILD_TESTS=OFF` to opt out there.

Five stages, and only the last two are slow:

| Stage | What happens | Command |
| --- | --- | --- |
| 1. Bootstrap | Build the executable with no game code in it yet | `cmake --build out -j2` |
| 2. Prepare | The bootstrap checks your disc image and decrypts the game's executable from it | `<Game>Native --install image.iso` |
| 3. Run it | Under the interpreter, before anything is recompiled — see [BRINGING_UP_A_GAME.md](BRINGING_UP_A_GAME.md) | `<Game>Native` |
| 4. Generate | Analyse the executable and write it out as C++ units | `psp_recomp <EBOOT.ELF> --auto generated` |
| 5. Compile | Compile those units with the host | `cmake --build out -j2` |

A game with code overlays has a sixth stage: extract each overlay, recompile it, and build a shared library per overlay that the host loads at run time. How the overlays are extracted is the game's business, because the container they sit in is the game's own; the recompiling and the CMake target are the framework's (`tools/add_overlay.py`, and `portablekit_add_game()`).

Overlay modules use the corpus ABI's function table. They build independently
of the host executable: no executable import library, exported runtime symbols
or host rebuild is needed to link them. Editing the host therefore does not
relink every overlay on Windows or macOS. Older generated code that directly
calls `Runtime` must be regenerated for the current corpus ABI.

### Where the time goes

Measured on an Apple M1 with 8 GB, from a fresh clone with an empty compiler cache:

| Stage | Time |
| --- | --- |
| Framework, bootstrap, prepare, first interpreted run | minutes |
| Generating the corpus (a 2.6 MB executable, 153 units) | about 2.5 minutes |
| Compiling the corpus | hours |
| Each code overlay | seconds to a minute |

**The generated units are very large, so compiling them needs a lot of memory.** The build limits how many compile at once to one per 4 GiB, whatever `-j` you pass (`PSPRECOMP_GENERATED_JOBS`). That is 2 on an 8 GB machine, and passing `-j8` does not make it faster, only more likely to swap.

**It is slow only once.** With `ccache` installed, which the build uses automatically, a later rebuild of unchanged generated code takes seconds, across checkouts.

## Working on the code

- **Host-only changes rebuild in seconds.** Changes under `include/psprecomp/` rebuild everything, including every overlay, so avoid them unless they are the point.
- **Don't rebuild the corpus per checkout.** Copy `generated/` from a checkout that has it with a plain `cp -R`, not a copy that keeps old timestamps. Point at an existing overlay build with `<PREFIX>_OVERLAY_DIR`.
- **Don't re-prepare the game per checkout.** `<PREFIX>_GAME_DIR` points at a directory that already holds `EBOOT.ELF`, `disc.iso` and `ms0/`.
- **Build with `cmake --build`, never `ninja` directly.** `cmake --build` holds the per-directory lock and repairs the Ninja log first; see [BUILD_SYSTEM.md](BUILD_SYSTEM.md). Run one build per build directory, and never delete `.ninja_deps` or `.ninja_log`.

## Options

| Option | Default | What it does |
| --- | --- | --- |
| `PSPRECOMP_GENERATED_OPT_LEVEL` | `2` | Optimisation level for generated units. `0` compiles much faster and runs much slower |
| `PSPRECOMP_GENERATED_JOBS` | memory / 4 GiB | How many generated units compile at once |
| `PSPRECOMP_BUILD_TESTS` | `ON` standalone, `OFF` in a port | Build framework tests and host compile checks |
| `PORTABLEKIT_RENDERER` | `ON` | Build the Vulkan renderer. Off gives a headless build |
| `PORTABLEKIT_FFMPEG` | `bundled` | `bundled` builds FFmpeg with the project, `system` uses one found through pkg-config, `OFF` drops music and movies |
| `PORTABLEKIT_FFMPEG_WINDOWS_PREBUILT` | `OFF` for Unix-to-MinGW, `ON` on Windows | Use full prebuilt Windows FFmpeg instead of the minimal source build |
| `PORTABLEKIT_RELEASE` | `OFF` | Build for distribution: no paths into the checkout, libraries from `lib/` next to the executable |
| `PSPRECOMP_LTO` | `OFF` | Whole-program optimisation |
| `PORTABLEKIT_BUILD_LABEL` | empty | A name for the build, shown beside the version: in the window title, the menu's About section, the desktop app's library header and `portablekit --version`. For telling a build made with other components (see below) from the plain one without patching sources |
| `PORTABLEKIT_HLE_EXTENSION_DIRS` | empty | Directories of [HLE extension modules](HLE_EXTENSIONS.md) to link into every program |

## Dependencies

SDL3, Vulkan and `glslangValidator` for the renderer; without them the build says so and produces a headless port. FFmpeg for music and movies, built by the project unless you ask otherwise. `ccache` if you have it. Python 3 for the build's own scripts.

### Generated registration size

Automatic corpora register each unit's entry addresses with one loop, using
arithmetic for consecutive entries and a constant array for sparse entries.
This keeps the same entry order, function pointers and chainable
names, without asking the compiler to optimize thousands of repeated calls.
Regenerate existing corpora to get this change; runtime ABI stays unchanged.

On Linux x86-64 with GCC 16.2, compiling the median-sized source unit from
Portable Ops' main corpus (`-O2`) and the pooled Portable Ops and Portable
Ops+ module corpora (`-Os`) gave these samples, with the compiler cache bypassed:

| Unit | Compile before / after | Object bytes before / after |
| --- | --- | --- |
| Main | 17.56 / 15.61 seconds | 819,960 / 639,080 |
| Module | 4.02 / 3.72 seconds | 180,144 / 123,944 |
| Plus module | 9.54 / 8.58 seconds | 266,200 / 187,696 |

These are individual compile samples with other builds running, not full-build
or gameplay benchmarks. Generated game data is not part of this repository.
The `portablekit_codegen_registration` CTest compiles and runs registration
from a synthetic ELF, for both a main corpus and a compact overlay. It needs
Python 3 and a native GCC-compatible compiler on Unix.

Regenerating the complete Portable Ops main corpus preserved all 482,156 entry
registrations and all code outside registration across 118 units. Its generated
C++ fell from 129,972,829 to 94,315,233 bytes (27%). This comparison excludes
headers and the JSON report.

With llvm-mingw's x86-64 Clang, the same three sample objects fell from
416,552 to 351,195 bytes, 99,230 to 89,968 bytes, and 166,527 to 153,493 bytes.
Three alternating before/after runs gave median compiler CPU times of
13.50 / 13.07, 1.97 / 1.91 and 3.17 / 3.34 seconds respectively. Size improved
in all three; compile time did not improve uniformly.

Removing host exports and the executable import library also reduced a
Portable Ops+ Windows executable from 38,205,952 to 37,982,720 bytes when
relinked from the same existing objects. The 1,350,976-byte import library is
no longer needed. A real pooled Portable Ops module linked independently on
Linux (with undefined symbols rejected) and llvm-mingw; the Windows DLL imports
only system libraries. The synthetic registration check also loads its module
into a host built without exported symbols. Windows execution, macOS and
Android remain unverified.

The complete Portable Ops Linux target also builds with the Vulkan renderer
and system FFmpeg enabled. Across all 118 generated units at `-O2 -g0`, GCC
16.2 produced 78,361,744 bytes of objects, down from 99,678,328 (21.4%). A
controlled relink with identical final host objects and linker flags, replacing
only those 118 unit objects, reduced the executable from 62,243,280 to
52,584,968 bytes (15.5%). These are unstripped development executables, not
packaged releases. Removing host exports separately saved 729,088 bytes from
the executable with the new corpus.

All 22 framework CTests pass, including generated registration and loading an
independent overlay. The registration test also passes with Ninja selected
through `CMAKE_GENERATOR`. In a fresh consumer configuration, disabling the
framework tests removes 223 compile commands (236 down to 13 for the framework
portion); existing caches still require an explicit opt-out. No game was
launched for these checks, and no complete cold-build timing comparison or
full regenerated overlay-package build was performed.

### Faster code generation

VFPU lowering now checks for each helper's literal name before running its
regex and compiles the patterns once per process. The lowering passes also
move their source buffer instead of copying it between passes.

On Linux x86-64 with the Release GCC 16.2 recompiler, three alternating runs
reduced median CPU time for the complete Portable Ops main corpus from 27.776
to 22.231 seconds (20%). A representative 807,292-byte compact stage dropped
from 3.741 to 2.659 seconds (29%). All 120 main-corpus and 52 stage C++/header
files were byte-identical before and after. These are generation times, not
C++ compilation times; unchanged port units keep their timestamps and cached
objects. The framework's 22 tests pass, including automatic VFPU lowering.

### Minimal Windows FFmpeg

Unix-to-MinGW x64 builds now compile the same pinned FFmpeg 7.1.5 source as
Linux, with only ATRAC3, ATRAC3plus and H.264 decoding. Enable C and C++ in the
consuming project and supply the MinGW tools through its CMake toolchain.
Native Windows builds keep the prebuilt default. Set
`PORTABLEKIT_FFMPEG_WINDOWS_PREBUILT=ON` to use that fallback when cross-building.
Projects without a configured C compiler also keep the prebuilt default.

With llvm-mingw 20260922 UCRT x86-64, the two DLLs total 2,707,968 bytes:
1,995,776 for avcodec and 712,192 for avutil. The prebuilt DLLs, including their
swresample dependency, total 69,278,720 bytes. This saves 96.1% of the FFmpeg
runtime size. The source build took about 34 seconds with two jobs and a cached
source archive; reconfiguration reused it without recompiling. DLLs contain no
absolute checkout paths. The build stages `FFmpeg-LICENSE.txt` (LGPL 2.1) and
`FFmpeg-SOURCE.txt` beside them. Packagers should follow DLL imports rather than
require swresample, which this configuration does not use.

The standalone synthetic decoder check is runnable without a port:

```sh
cmake -S tests/ffmpeg -B out-ffmpeg -G Ninja
cmake --build out-ffmpeg -j2
ctest --test-dir out-ffmpeg --output-on-failure
```

For Windows, add the port's `CMAKE_TOOLCHAIN_FILE` and run
`bin/ffmpeg_check.exe` on Windows or under Wine. It checks both ATRAC decoder
registrations and decodes a synthetic H.264 frame, comparing every pixel.
The check passes with Linux source libraries and under Wine with both Windows
source and prebuilt libraries. Existing complete Portable Ops+ Windows objects
also relink against the minimal import libraries; recursive package dependency
checks collect SDL3, avcodec, avutil, libc++ and libunwind without swresample.
No game data is included. Audio sample decoding, gameplay, native Windows
execution and other cross-toolchains remain unverified.

### Consecutive entry tables

Units whose entries cover every instruction word now compute their dispatch
IDs and registration addresses directly. Sparse units keep their tables. The
entry range and alignment guards, direct entry IDs, registration order and
runtime ABI stay unchanged. Regenerate the main corpus to obtain this change.

In the complete Portable Ops main corpus, 117 of 118 units have consecutive
entries. Generated C++ falls from 94,315,233 to 81,471,450 bytes (13.6%). All
482,156 entry addresses and IDs match, and instruction bodies are unchanged.
All 52 C++/header files of the representative compact stage remain
byte-identical, so these overlays do not need recompilation.
Three alternating generations of the main corpus reduce median recompiler
CPU time from 22.725 to 21.860 seconds (3.8%).

With llvm-mingw 20260922 UCRT x86-64 at `-O2 -g0`, the median-sized main unit's
object falls from 351,213 to 326,414 bytes (7.1%). Three alternating compile
runs give median CPU times of 10.901 and 10.745 seconds; that small difference
does not establish a substantial compilation speedup. Other builds were
running during the measurement.

The synthetic registration check also executes all registered entries,
compares normal and direct-ID dispatch, checks local returns, and rejects
misaligned, out-of-range and missing compact entries. All 22 framework tests
pass on Linux. The main-corpus synthetic runner also passes under Wine after
compilation with llvm-mingw. Gameplay remains unverified.

The complete Linux port builds with the Vulkan renderer and system FFmpeg.
Across all 118 main units at `-O2 -g0`, GCC 16.2 object size falls from
78,361,744 to 75,462,864 bytes (3.7%). Controlled relinks with identical host
objects and linker flags reduce the executable from 52,584,968 to 49,690,608
bytes (5.5%). These are unstripped development executables, not release ZIPs.
A registration-only runner linked against each complete corpus checks all
482,156 entries, their order, names, unit bounds and function bindings; both
runs produce the same result without executing guest instructions. No paired
full-build timing or full Windows corpus build was performed.
