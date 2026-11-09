// Exercise the module miss path without a game or a shared corpus library.
// Keep the module table private in production; substitute only corpus lookup.
#include "overlays.hpp"
#include "psprecomp/interpreter.hpp"
namespace portablekit {
bool startup_test_install(psprecomp::Runtime &, std::uint32_t, std::span<const std::uint8_t>);
}
#define install_module_corpus startup_test_install
#include "../host/hle/user_modules.cpp"
#undef install_module_corpus

#include <cstdio>
#include <stdexcept>

namespace {
constexpr std::uint32_t kBase = 0x08810000u;
unsigned lookups = 0;
std::uint8_t expected = 1;
bool matched = true;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
void native_entry(psprecomp::CorpusRuntime &, psprecomp::AllegrexContext &ctx) {
    ctx.pc = ctx.gpr[31];
}
}

namespace portablekit {
bool startup_test_install(Runtime &runtime, std::uint32_t base, std::span<const std::uint8_t> code) {
    ++lookups;
    require(base == kBase && code.size() == 16 && code[0] == expected,
            "Module lookup did not use current guest bytes");
    if (matched) runtime.register_function(base, &native_entry, "startup_test");
    return matched;
}
}

int main() {
    using namespace portablekit;
    Runtime runtime;
    AllegrexContext ctx{};
    runtime.memory().store8(kBase, expected);
    UserModule module{};
    module.name = "startup_test";
    module.base = kBase;
    module.code_size = 16;
    modules().emplace(1, module); // module_start has not returned

    psprecomp::set_interpreter_fallback_enabled(false);
    psprecomp::reset_interpreter_stats();
    psprecomp::set_runtime_missing_function_hook(&user_module_code_miss);
    runtime.cpu().gpr[31] = kBase + 0x100;
    runtime.run(kBase, 1);
    require(runtime.cpu().pc == kBase + 0x100, "Native module_start did not return");
    require(psprecomp::interpreter_stats().instructions == 0, "Startup used the interpreter");
    require(lookups == 1 && modules().at(1).code_checked, "Startup lookup was not cached");
    require(!user_module_code_miss(runtime, ctx, kBase), "Checked module was searched again");
    require(lookups == 1, "Repeated startup lookup");

    modules().at(1).started = true;
    runtime.memory().store8(kBase, ++expected); // engine links the stage
    invalidate_user_module_code(runtime, kBase, kBase + 16);
    require(!modules().at(1).code_checked, "Cache flush retained startup corpus");
    require(runtime.registered_function(kBase) == nullptr, "Cache flush retained stale native entry");
    require(user_module_code_miss(runtime, ctx, kBase), "Linked corpus was not installed");
    require(lookups == 2, "Linked image was not searched");

    invalidate_user_module_code(runtime, kBase, kBase + 16);
    modules().at(1).started = false;
    matched = false;
    require(!user_module_code_miss(runtime, ctx, kBase), "Unknown startup image claimed native coverage");
    require(lookups == 3, "Unknown startup image was not searched");
    modules().clear();
    // A PRX without a syslib export must retain its ELF entry. Positional
    // aggregate initialization once put this entry into code_hash instead.
    kernel().install(runtime, 0, 0x08804000u);
    std::vector<std::uint8_t> prx(288);
    const auto word = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) prx[offset + i] = value >> (i * 8);
    };
    prx[0] = 0x7f; prx[1] = 'E'; prx[2] = 'L'; prx[3] = 'F';
    prx[4] = 1; prx[5] = 1; prx[6] = 1;
    word(16, 0x0008ffa0u); // PSP PRX, MIPS
    word(20, 1); word(24, 0x6c); word(28, 52);
    word(40, 0x00200034u); word(44, 1); // header sizes, one segment
    word(52, 1); word(56, 96); word(64, 224); // PT_LOAD and module info
    word(68, 192); word(72, 192); word(76, 7); word(80, 4);
    const auto uid = load_user_module(runtime, prx, "synthetic startup PRX");
    require(uid.has_value() && modules().contains(*uid), "Synthetic PRX did not load");
    const auto &loaded = modules().at(*uid);
    require(loaded.start == loaded.base + 0x6c && !loaded.started && loaded.code_hash == 0,
            "ELF startup entry or initial lifecycle state was lost");
    require(unload_user_module(runtime, *uid), "Synthetic PRX did not unload");
    std::puts("user module startup checks passed");
}
