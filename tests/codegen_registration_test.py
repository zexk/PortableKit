#!/usr/bin/env python3
"""Compile and run synthetic main/overlay registration, including sparse entries.

Usage: python3 tests/codegen_registration_test.py out/psp_recomp c++
Requires a native GCC-compatible C++ compiler; no game data.
"""
import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def check(recompiler, compiler):
    with tempfile.TemporaryDirectory(prefix='portablekit-registration-') as directory:
        root = pathlib.Path(directory)
        # Two reachable return blocks, separated by unreachable words. The
        # branch targets offset 16; offset 8 is the untaken continuation.
        code = [0x10000003, 0, 0x03E00008, 0, 0x03E00008, 0, 0, 0]
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
            prefix = 'overlay' if compact else 'recomp'
            registry = 'register_overlay_functions' if compact else 'register_generated_functions'
            subprocess.run([recompiler, str(elf), '--auto', str(generated),
                            '0x08804000', '16', '--prefix', prefix] +
                           (['--compact'] if compact else []), check=True, capture_output=True)
            runner = root / 'runner.cpp'
            runner.write_text('''#include "psprecomp/corpus_abi.hpp"
#include <cassert>
#include <cstdio>
using namespace psprecomp;
namespace psprecomp { void REGISTRY(CorpusRuntime &); }
static void record(CorpusRuntime &, std::uint32_t pc, CorpusFunction function, const char *name) {
    assert(function != nullptr);
    std::printf("%08X %s\\n", pc, name);
}
static void unit(CorpusRuntime &, std::uint32_t, std::uint32_t, std::uint32_t,
                 CorpusFunction function, CorpusEntryFunction entry) {
    assert(function != nullptr && entry != nullptr);
}
struct Host : CorpusRuntime {
    Host(const CorpusHostApi *api) : CorpusRuntime(api, nullptr) {}
};
int main() {
    CorpusHostApi api{};
    api.register_function = record;
    api.register_unit = unit;
    Host host(&api);
    REGISTRY(host);
}
'''.replace('REGISTRY', registry))
            executable = root / 'registration'
            subprocess.run([compiler, '-std=c++20', '-O2', '-I' + str(ROOT / 'include'),
                            *map(str, sorted(generated.glob('*.cpp'))), str(runner),
                            '-o', str(executable)], check=True)
            actual = subprocess.check_output([str(executable)], text=True).splitlines()
            offsets = (0, 8, 16) if compact else range(0, 32, 4)
            expected = [f'{0x08804000 + offset:08X} recomp_unit_{prefix}_unit_{offset // 16:04d}'
                        for offset in offsets]
            assert actual == expected, (actual, expected)
    print('Generated registration passed (main and compact overlay).')


if __name__ == '__main__':
    check(*sys.argv[1:])
