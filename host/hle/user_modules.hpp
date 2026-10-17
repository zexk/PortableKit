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

// sceKernelUnloadModule for a user module: frees its memory and drops any
// functions registered inside it. False when `uid` is not a user module.
bool unload_user_module(psprecomp::Runtime &runtime, std::uint32_t uid);

} // namespace portablekit
