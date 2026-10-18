// The game's own PRX modules: libraries and code a title ships unencrypted
// on its disc and loads with sceKernelLoadModule, as opposed to Sony's
// encrypted stock modules, whose exports the HLE serves directly.
//
// A user module is loaded into a block of guest memory, relocated, and its
// exports bound so every import of them (in the executable or in other
// modules) jumps into its code. Its code has no recompiled functions until a
// profile supplies some, so it runs under the interpreter.
#pragma once

#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace portablekit {

// Loads `bytes` as a user module when it is a plain PRX. Returns its module
// id, or nothing when the bytes are something else (an encrypted module).
std::optional<std::uint32_t> load_user_module(psprecomp::Runtime &runtime, std::span<const std::uint8_t> bytes,
                                              const std::string &where);

// sceKernelStartModule for a user module: runs its module_start with
// (argument_size, argument) and finishes the import with the module id,
// storing module_start's result at `status` when it is not zero. False when
// `uid` is not a user module.
bool start_user_module(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, std::uint32_t uid,
                       std::uint32_t argument_size, std::uint32_t argument, std::uint32_t status);

// Dispatch found no function at `pc`. If it lies in a user module's code
// that has not been checked yet, installs the module's recompiled corpus
// (overlays.hpp) when one matches its code as it now is in memory, and
// returns true to retry; otherwise, with <prefix>_DUMP_MODULES=<dir>, writes
// that code to <dir>/<id>.bin for recompilation and returns false.
bool user_module_code_miss(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx, std::uint32_t pc);

// While set, a module's first run after its code was patched (flushed from
// the instruction cache once it had run) is identified (and dumped) as usual
// but not run: it returns to its caller at once. For a port that starts every
// module of a game once, to make their corpora, without entering them.
void set_skip_module_first_run(bool skip);

// The guest invalidated the instruction cache over [start, end), as it does
// after patching code: every checked module there loses its corpus and is
// identified again at its next run.
void invalidate_user_module_code(psprecomp::Runtime &runtime, std::uint32_t start, std::uint32_t end);

// sceKernelUnloadModule for a user module: frees its memory and drops any
// functions registered inside it. False when `uid` is not a user module.
bool unload_user_module(psprecomp::Runtime &runtime, std::uint32_t uid);

} // namespace portablekit
