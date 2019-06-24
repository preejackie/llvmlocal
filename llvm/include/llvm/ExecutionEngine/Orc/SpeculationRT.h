//===-- Speculation-rt.h - Runtime Calls --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// Interfaces for Runtime Calls
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_SPECULATIONRT_H
#define LLVM_EXECUTIONENGINE_ORC_SPECULATIONRT_H

#include <cstdint>
namespace llvm {
namespace orc {
extern "C" {
void __orc_speculate_for(void* Ptr,uint64_t stub_id);
}
} // namespace orc
} // namespace llvm
#endif