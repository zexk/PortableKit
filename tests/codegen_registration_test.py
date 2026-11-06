#!/usr/bin/env python3
"""Compile and run synthetic main/overlay registration and entry dispatch.

Usage: python3 tests/codegen_registration_test.py out/psp_recomp c++ [cmake]
Requires CMake and a native GCC-compatible C++ compiler on Unix; no game data.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def check(recompiler, compiler, cmake='cmake'):
    with tempfile.TemporaryDirectory(prefix='portablekit-registration-') as directory:
        root = pathlib.Path(directory)
        # Two reachable return blocks, separated by unreachable words. The
        # branch targets offset 16; offset 8 is the untaken continuation.
        code = [0x10000003, 0x24020001, 0x03E00008, 0x24020002, 0x03E00008, 0x24020003, 0, 0]
        image = bytearray(128 + 4 * len(code))
        image[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHIIIIIHHHHHH', image, 16,
                         2, 8, 1, 0x08804000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
        struct.pack_into('<IIIIIIII', image, 52,
                         1, 128, 0x08804000, 0x08804000, 4 * len(code), 4 * len(code), 5, 16)
        struct.pack_into('<' + 'I' * len(code), image, 128, *code)
        elf = root / 'fixture.elf'
        elf.write_bytes(image)
        for compact in (False, True):
            generated = root / ('compact' if compact else 'full')
            prefix = 'compact' if compact else 'recomp'
            registry = 'register_compact_functions' if compact else 'register_generated_functions'
            subprocess.run([recompiler, str(elf), '--auto', str(generated),
                            '0x08804000', '16', '--prefix', prefix] +
                           (['--compact'] if compact else []), check=True, capture_output=True)
            sources = sorted(generated.glob('*.cpp'))
            load = ''
            arguments = []
            if compact:
                # No host target exists. Building this module must not build
                # or link a host, and loading it must need no host exports.
                (generated / 'meta.txt').write_text(
                    'base=0x08804000\nsize=32\ncode_size=32\nhash=0x1\nname=fixture\n')
                (root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(OverlayCheck LANGUAGES CXX)
include("{ROOT}/cmake/PortableKit.cmake")
set(PSPRECOMP_OVERLAY_OPT_LEVEL s)
set_property(GLOBAL PROPERTY JOB_POOLS psprecomp_generated=2)
_portablekit_add_overlay(UnbuiltHost "{generated}/meta.txt" "{root}/modules")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(overlay_compact PRIVATE "LINKER:--no-undefined")
endif()
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/module-path" CONTENT "$<TARGET_FILE:overlay_compact>")
''')
                build = root / 'build'
                subprocess.run([cmake, '-S', str(root), '-B', str(build),
                                '-DCMAKE_CXX_COMPILER=' + compiler], check=True, capture_output=True)
                subprocess.run([cmake, '--build', str(build), '--target', 'overlay_compact', '-j2'],
                               check=True, capture_output=True)
                arguments = [(build / 'module-path').read_text()]
                sources = []
                load = '''void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) { std::fprintf(stderr, "%s\\n", dlerror()); return 1; }
    const auto register_all = reinterpret_cast<void (*)(CorpusRuntime &)>(
        dlsym(module, "portablekit_register_overlay"));
    assert(register_all != nullptr);'''
                registry = 'register_all'
            runner = root / 'runner.cpp'
            runner.write_text('''#include "psprecomp/corpus_abi.hpp"
#include <cassert>
#include <cstdio>
#include <dlfcn.h>
#include <map>
using namespace psprecomp;
namespace psprecomp { void REGISTRY(CorpusRuntime &); }
static std::map<std::uint32_t, CorpusFunction> functions;
static std::map<std::uint32_t, CorpusEntryFunction> unit_entries;
static unsigned errors;
static void record(CorpusRuntime &, std::uint32_t pc, CorpusFunction function, const char *name) {
    assert(function != nullptr);
    assert(functions.emplace(pc, function).second);
    std::printf("%08X %s\\n", pc, name);
}
static void unit(CorpusRuntime &, std::uint32_t, std::uint32_t base, std::uint32_t,
                 CorpusFunction function, CorpusEntryFunction entry) {
    assert(function != nullptr && entry != nullptr);
    unit_entries.emplace(base, entry);
}
struct Host : CorpusRuntime {
    Host(const CorpusHostApi *api, const CorpusProcessState *process) : CorpusRuntime(api, process) {}
};
int main(int, char **argv) {
    CorpusHostApi api{};
    api.register_function = record;
    api.register_unit = unit;
    api.chained_call = [](CorpusRuntime &, AllegrexContext &, AotFastView *) { return false; };
    api.chained_unit = [](CorpusRuntime &, AllegrexContext &, std::uint32_t, AotFastView *) { return false; };
    api.unsupported = [](CorpusRuntime &, std::uint32_t, std::uint32_t, const char *reason) {
        assert(std::strcmp(reason, "invalid internal function entry") == 0);
        ++errors;
    };
    CorpusProcessState process{};
    Host host(&api, &process);
    LOAD
    REGISTRY(host);
    constexpr std::uint32_t base = 0x08804000u, return_pc = base + 64u;
    const std::uint32_t expected_pc[] = {base + 16, return_pc, return_pc, base + 16,
                                       return_pc, base + 32, base + 32, base + 32};
    const std::uint32_t expected_v0[] = {1, 2, 2, 2, 3, 3, 0, 0};
    for (const auto &[pc, function] : functions) {
        AllegrexContext ctx{};
        ctx.pc = pc;
        ctx.gpr[31] = return_pc;
        function(host, ctx);
        assert(errors == 0 && ctx.pc == expected_pc[(pc - base) / 4]);
        assert(ctx.gpr[2] == expected_v0[(pc - base) / 4]);
        // Direct ids must select the same entries as PC dispatch.
        if (!unit_entries.empty()) {
            ctx = {};
            ctx.pc = pc;
            ctx.gpr[31] = return_pc;
            AotFastView view;
            unit_entries.at(pc & ~15u)(host, ctx, ((pc - base) % 16) / 4 + 1, view);
            assert(errors == 0 && ctx.pc == expected_pc[(pc - base) / 4]);
            assert(ctx.gpr[2] == expected_v0[(pc - base) / 4]);
        }
    }
    // A local return reaches emitted code in the full corpus, but leaves
    // through the dispatcher when the compact corpus has no entry there.
    AllegrexContext local{};
    local.pc = base + 16;
    local.gpr[31] = base + 24;
    functions.at(base + 16)(host, local);
    assert(errors == 0 && local.gpr[2] == 3);
    assert(local.pc == (functions.contains(base + 24) ? base + 32 : base + 24));
    // Reject below/above each unit, misalignment and holes in compact tables.
    for (std::uint32_t unit_base : {base, base + 16}) {
        for (std::uint32_t pc = unit_base - 4; pc <= unit_base + 16; ++pc) {
            if (pc >= unit_base && pc < unit_base + 16 && functions.contains(pc)) continue;
            AllegrexContext ctx{};
            ctx.pc = pc;
            const auto before = errors;
            functions.at(unit_base)(host, ctx);
            assert(errors == before + 1 && ctx.pc == pc);
        }
    }
}
'''.replace('REGISTRY', registry).replace('LOAD', load))
            executable = root / 'registration'
            subprocess.run([compiler, '-std=c++20', '-O2', '-I' + str(ROOT / 'include'),
                            *map(str, sources), str(runner),
                            '-o', str(executable)] + (['-ldl'] if sys.platform == 'linux' else []), check=True)
            actual = subprocess.check_output([str(executable), *arguments], text=True).splitlines()
            offsets = (0, 8, 16) if compact else range(0, 32, 4)
            expected = [f'{0x08804000 + offset:08X} recomp_unit_{prefix}_unit_{offset // 16:04d}'
                        for offset in offsets]
            assert actual == expected, (actual, expected)
    print('Generated registration and dispatch passed (main and compact overlay).')


if __name__ == '__main__':
    try:
        check(*sys.argv[1:])
    except subprocess.CalledProcessError as error:
        for output in (error.stdout, error.stderr):
            if output:
                sys.stderr.write(output.decode() if isinstance(output, bytes) else output)
        raise
