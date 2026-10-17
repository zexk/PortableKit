#include "user_modules.hpp"

#include "hle_common.hpp"
#include "../profile.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"

#include <iostream>
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
    std::uint32_t start{}; // module_start, the ELF entry when not exported, or 0 for none
};

std::map<std::uint32_t, UserModule> &modules() {
    static std::map<std::uint32_t, UserModule> table;
    return table;
}

// Every import of an exported function enters its code, as the PSP's loader
// links them; the caller's jal has already set ra.
void bind_export(Runtime &runtime, const std::string &library, std::uint32_t nid, std::uint32_t address) {
    runtime.register_hle(library, nid, [address](Runtime &, AllegrexContext &ctx) { ctx.pc = address; });
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
    UserModule module{info->name, block, base, size, elf.entry() == 0xFFFFFFFFu ? 0u : elf.runtime_entry(base)};
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
    for (const auto &import : elf.scan_imports(runtime.memory(), *info)) {
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
        kernel().finish(ctx, uid);
    });
    return true;
}

} // namespace portablekit
