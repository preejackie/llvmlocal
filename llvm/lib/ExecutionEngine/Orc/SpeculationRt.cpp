//===---------- Speculation-rt.cpp - Utilities for lazy reexports
//----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/SpeculationRt.h"

#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "orc"

namespace llvm {
namespace orc {
extern "C" void __orc_speculate_for(uint64_t StubId) {
  // trigger compilation
}
} // namespace orc
} // namespace llvm