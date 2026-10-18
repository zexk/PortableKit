#include "user_modules.hpp"

#include "hle_common.hpp"
#include "../profile.hpp"
#include "../overlays.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <map>
#include <vector>

namespace portablekit {
namespace {

// NIDs of the unnamed "syslib" export table every module carries.
constexpr std::uint32_t kModuleStartNid = 0xD632ACDBu;

struct UserModule {
    std::string name;
    std::int32_t block{};
    std::uint32_t base{};
    std::uint32_t size{};
    std::uint32_t code_size{}; // the first segment: code and read-only data
    bool code_checked{};       // the corpus was looked for at the first jump in
    std::uint64_t code_hash{}; // of the code when it was checked
    bool started{};            // module_start returned, or there is none
    std::uint32_t start{};
    std::vector<psprecomp::PspImport> imports; // module_start, the ELF entry when not exported, or 0 for none
    // Runs of import stubs inside the code, as offsets: [start, end).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> inner_stubs;
    bool invalidated{}; // its code was flushed from the instruction cache after it was checked
};

std::map<std::uint32_t, UserModule> &modules() {
    static std::map<std::uint32_t, UserModule> table;
    return table;
}

// Every import of an exported function enters its code, as the PSP's loader
// links them; the caller's jal has already set ra.
void bind_export(Runtime &runtime, const std::string &library, std::uint32_t nid, std::uint32_t address) {
    static const bool trace = env("TRACE_MODULES") != nullptr;
    const std::string label = library + "::" + psprecomp::hex32(nid);
    runtime.register_hle(library, nid, [address, label](Runtime &rt, AllegrexContext &ctx) {
        std::string text;  // a0 as a string, when it points at one
        if (trace && rt.memory().contains(ctx.gpr[4], 1u)) {
            for (std::uint32_t i = 0; i < 64u && rt.memory().contains(ctx.gpr[4] + i, 1u); ++i) {
                const auto c = static_cast<char>(rt.memory().load8(ctx.gpr[4] + i));
                if (c == '\0') break;
                if (c < 0x20 || c > 0x7E) { text.clear(); break; }
                text += c;
            }
        }
        if (trace)
            std::cerr << "[module-call] " << label << (text.size() > 2 ? " \"" + text + "\"" : "")
                      << " a0=" << psprecomp::hex32(ctx.gpr[4]) << " a1="
                      << psprecomp::hex32(ctx.gpr[5]) << " a2=" << psprecomp::hex32(ctx.gpr[6])
                      << " ra=" << psprecomp::hex32(ctx.gpr[31]) << "\n";
        ctx.pc = address;
    });
}

} // namespace

std::optional<std::uint32_t> load_user_module(Runtime &runtime, std::span<const std::uint8_t> bytes,
                                              const std::string &where) {
    if (bytes.size() < 4u || bytes[0] != 0x7Fu || bytes[1] != 'E' || bytes[2] != 'L' || bytes[3] != 'F')
        return std::nullopt;
    const auto elf = psprecomp::Elf32Image::from_bytes(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), where);
    if (!elf.is_psp_prx()) return std::nullopt;

    std::uint32_t size = 0u;
    for (const auto &segment : elf.segments())
        if (segment.type == 1u) size = std::max(size, segment.vaddr + segment.memory_size);
    const std::int32_t block = kernel().allocate_block("module:" + where, 3u, size, 0x100u);
    if (block < 0) return static_cast<std::uint32_t>(block);
    const std::uint32_t base = kernel().find_block(block)->address;
    const auto relocations = elf.load_and_relocate(runtime.memory(), base);
    const auto info = elf.find_module_info(runtime.memory(), base);
    if (!info) throw psprecomp::Error("module info not found in " + where);

    // An entry of -1 means no start function, as for a pure library.
    UserModule module{info->name, block, base, size, elf.segments().front().file_size, false, elf.entry() == 0xFFFFFFFFu ? 0u : elf.runtime_entry(base)};
    module.started = module.start == 0u;
    std::size_t exported = 0u;
    for (std::uint32_t cursor = info->ent_top; cursor < info->ent_end;) {
        const std::uint32_t libname = runtime.memory().load32(cursor);
        const std::uint32_t length = std::max<std::uint32_t>(4u, runtime.memory().load8(cursor + 8u)) * 4u;
        const std::uint32_t variables = runtime.memory().load8(cursor + 9u);
        const std::uint32_t functions = runtime.memory().load16(cursor + 10u);
        const std::uint32_t table = runtime.memory().load32(cursor + 12u);
        const std::uint32_t total = variables + functions;
        for (std::uint32_t i = 0; i < functions; ++i) {
            const std::uint32_t nid = runtime.memory().load32(table + i * 4u);
            const std::uint32_t address = runtime.memory().load32(table + (total + i) * 4u);
            if (libname == 0u) {
                if (nid == kModuleStartNid) module.start = address;
                continue;
            }
            bind_export(runtime, runtime.memory().read_c_string(libname, 128u), nid, address);
            ++exported;
        }
        cursor += length;
    }

    // The module's own imports: stubs the interpreter can call, and a logged
    // failure for any the HLE and the loaded modules do not provide.
    std::size_t missing = 0u;
    module.imports = elf.scan_imports(runtime.memory(), *info);
    // The code a corpus is built from ends where the import stubs begin:
    // compiled, a stub is two plain instructions, and a call to it from the
    // same unit would skip the HLE. A module may keep some stubs inside its
    // code (Metal Gear Solid: Portable Ops' kjfs, for zlibdec): the code ends
    // at the last run of stubs, and the runs before it are left out of the
    // corpus (dumped beside the code as <id>.stubs).
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    std::vector<std::uint32_t> stubs;
    for (const auto &import : module.imports) stubs.push_back(import.stub_address - base);
    std::sort(stubs.begin(), stubs.end());
    for (const std::uint32_t stub : stubs) {
        if (!runs.empty() && stub <= runs.back().second) runs.back().second = std::max(runs.back().second, stub + 8u);
        else runs.emplace_back(stub, stub + 8u);
    }
    if (!runs.empty()) {
        module.code_size = std::min(module.code_size, runs.back().first);
        runs.pop_back();
        for (const auto &run : runs)
            if (run.second <= module.code_size) module.inner_stubs.push_back(run);
    }
    for (const auto &import : module.imports) {
        if (!runtime.has_hle(import.library, import.nid)) {
            ++missing;
            const std::string label = import.library + "::" +
                runtime.nids().resolve(import.library, import.nid).value_or(psprecomp::hex32(import.nid));
            runtime.register_hle(import.library, import.nid, [label](Runtime &, AllegrexContext &ctx) {
                log_once("hle-stub:" + label, "[hle-stub] " + label + " a0=" + psprecomp::hex32(ctx.gpr[4]) +
                                                  " ra=" + psprecomp::hex32(ctx.gpr[31]));
                kernel().finish(ctx, 0u);
            });
        }
        (void)runtime.register_import_stub(import.stub_address, import.library, import.nid);
    }

    const auto uid = static_cast<std::uint32_t>(kernel().allocate_uid());
    std::cout << "[module] loaded user module " << module.name << " from " << where << " at "
              << psprecomp::hex32(base) << " (" << relocations.total << " relocations, " << exported
              << " exports, " << missing << " unresolved imports)\n";
    modules().emplace(uid, std::move(module));
    return uid;
}

namespace {
bool g_skip_first_run = false;

// With <prefix>_DUMP_MODULES=<dir>, writes a module's code (and the import
// stubs inside it) for recompilation; otherwise says how to.
void dump_module_code(const UserModule &module, const std::vector<std::uint8_t> &code, std::uint64_t hash) {
    std::ostringstream id;
    id << "mod" << psprecomp::hex32(module.base).substr(2) << "_" << module.name << "_" << std::hex
       << std::uppercase << std::setw(16) << std::setfill('0') << hash;
    if (const char *directory = env("DUMP_MODULES"); directory != nullptr) {
        std::filesystem::create_directories(directory);
        std::ofstream(std::filesystem::path(directory) / (id.str() + ".bin"), std::ios::binary)
            .write(reinterpret_cast<const char *>(code.data()), static_cast<std::streamsize>(code.size()));
        if (!module.inner_stubs.empty()) {
            std::ofstream stubs(std::filesystem::path(directory) / (id.str() + ".stubs"));
            for (const auto &[start, end] : module.inner_stubs) stubs << std::hex << start << "-" << end << "\n";
        }
        std::cout << "[module] " << module.name << " at " << psprecomp::hex32(module.base)
                  << ": interpreted; dumped " << id.str() << ".bin\n";
    } else {
        log_once("module-code:" + id.str(), "[module] " + module.name + " at " + psprecomp::hex32(module.base) +
                                                ": interpreted (" + id.str() + "); <prefix>_DUMP_MODULES=<dir> dumps it");
    }
}

} // namespace

void set_skip_module_first_run(bool skip) { g_skip_first_run = skip; }

bool user_module_code_miss(Runtime &runtime, AllegrexContext &ctx, std::uint32_t pc) {
    for (auto &[uid, module] : modules()) {
        if (pc < module.base || pc >= module.base + module.code_size || module.code_checked) continue;
        // A loader may fix up a module's code once module_start has run:
        // Metal Gear Solid Portable Ops starts a stage, which registers its
        // entry points, patches the stage's jumps into the engine, then calls
        // them. So a module is identified at its first run after module_start
        // returned, as its code is then, and interpreted until that point.
        if (!module.started) return false;
        module.code_checked = true;
        std::vector<std::uint8_t> code(module.code_size);
        for (std::uint32_t i = 0; i < module.code_size; ++i) code[i] = runtime.memory().load8(module.base + i);
        const std::uint64_t hash = module_code_hash(code);
        module.code_hash = hash;
        const bool installed = install_module_corpus(runtime, module.base, code);
        if (installed)
            std::cout << "[module] " << module.name << " at " << psprecomp::hex32(module.base) << ": recompiled code (first run at " << psprecomp::hex32(pc) << ")\n";
        const bool skip = g_skip_first_run && module.invalidated;
        if (installed && !skip) return true;
        if (installed) {
            // Nothing left to make for it.
            runtime.stop("skipped module " + module.name + " already has recompiled code");
            return true;
        }
        dump_module_code(module, code, hash);
        if (!skip) return false;
        ctx.pc = ctx.gpr[31];
        return true;
        dump_module_code(module, code, hash);
        return false;
    }
    return false;
}

void invalidate_user_module_code(Runtime &runtime, std::uint32_t start, std::uint32_t end) {
    for (auto &[uid, module] : modules()) {
        if (!module.code_checked || end <= module.base || start >= module.base + module.code_size) continue;
        module.invalidated = true;
        // The code may have been patched: drop what was compiled for it and
        // identify it again, by its bytes then, at its next run.
        runtime.unregister_functions(module.base, module.base + module.code_size);
        for (const auto &import : module.imports)
            (void)runtime.register_import_stub(import.stub_address, import.library, import.nid);
        module.code_checked = false;
    }
}

bool unload_user_module(Runtime &runtime, std::uint32_t uid) {
    const auto found = modules().find(uid);
    if (found == modules().end()) return false;
    const UserModule &module = found->second;
    // Imports still bound to its exports would jump into freed memory; a game
    // unloads a library only after it is done with it, so they are left, and
    // a later load of the same library binds them again.
    runtime.unregister_functions(module.base, module.base + module.size);
    (void)kernel().free_block(module.block);
    if (env("TRACE_KERNEL") != nullptr)
        std::cerr << "[module] unloaded user module " << module.name << " at " << psprecomp::hex32(module.base) << "\n";
    modules().erase(found);
    return true;
}

bool start_user_module(Runtime &, AllegrexContext &ctx, std::uint32_t uid, std::uint32_t argument_size,
                       std::uint32_t argument, std::uint32_t status) {
    const auto found = modules().find(uid);
    if (found == modules().end()) return false;
    if (found->second.start == 0u) {
        found->second.started = true;
        kernel().finish(ctx, uid);
        return true;
    }
    GuestCallArguments arguments{};
    arguments[0] = argument_size;
    arguments[1] = argument;
    // module_start runs in the caller's thread, which waits for it as it
    // would for the module's start thread on the console.
    kernel().call_guest(ctx, found->second.start, arguments, [uid, status](AllegrexContext &ctx, std::uint32_t result) {
        if (status != 0u) kernel().runtime().memory().store32(status, result);
        if (const auto module = modules().find(uid); module != modules().end()) module->second.started = true;
        kernel().finish(ctx, uid);
    });
    return true;
}

} // namespace portablekit
