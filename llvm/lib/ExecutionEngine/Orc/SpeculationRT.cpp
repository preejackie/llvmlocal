//===---------- Speculation-rt.cpp - Utilities for lazy reexports
//----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/SpeculationRT.h"
#include "llvm/ExecutionEngine/Orc/Speculation.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "orc"

namespace llvm {
namespace orc {
// Ptr to the Speculator instance
extern "C" void __orc_speculate_for(void* Ptr, uint64_t StubId) {
  // trigger compilation
assert(Ptr && "Null Address Received in orc_speculate_for ");
reinterpret_cast<Speculator*>(Ptr)->speculateFor(StubId);
}
} // namespace orc
} // namespace llvm
