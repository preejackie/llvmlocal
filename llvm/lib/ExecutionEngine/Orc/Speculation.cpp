//===---------- speculation.cpp - Utilities for Speculation ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/Speculation.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"

#include <vector>
namespace llvm {

namespace orc {

// Imap methods
void Imap::trackImpls(SymbolAliasMap ImplMaps, JITDylib *SrcJD) {
  std::lock_guard<std::mutex> lockit(ConcurrentAccess);
  for (auto &I : ImplMaps) {
    auto It = Maps.insert({I.first, {I.second.Aliasee, SrcJD}});
    assert(It.second && "ImplSymbols are already tracked for this Symbol?");
  }
}

// Speculator methods
// FIX ME: Register with Unified Stub Address, after JITLink Fix.
void Speculator::registerSymbolsWithAddr(TargetFAddr ImplAddr,
                                         SymbolNameSet likelySymbols) {
  std::lock_guard<std::mutex> lockit(ConcurrentAccess);
  GlobalSpecMap.insert({ImplAddr, std::move(likelySymbols)});
}

void Speculator::speculateFor(JITTargetAddress FAddr) { launchCompile(FAddr); }

void SpeculationLayer::emit(MaterializationResponsibility R,
                            ThreadSafeModule TSM) {
  auto Module = TSM.getModule();
  assert(Module && "Speculation Layer received Null Module");
  auto &ESession = this->getExecutionSession();
  auto &InContext = Module->getContext();

  // TODO
  // 1. external pointer reference to pass in - Global Variable
  // 2. change signature to include a pointer(i8*)
  // 3.

  // declare void @__orc_speculate_for(i8*,i64)
  auto RTFTy = FunctionType::get(Type::getVoidTy(InContext),
                                 {Type::getInt8PtrTy(InContext),Type::getInt64Ty(InContext)}, false);
  auto RTFn = Function::Create(RTFTy, Function::LinkageTypes::ExternalLinkage,
                               "__orc_speculate_for", *Module);

  auto SpeclAddr = new GlobalVariable(*Module,Type::getInt8PtrTy(InContext),false,
  GlobalValue::LinkageTypes::ExternalLinkage,nullptr,"orc_speculator");

  SpeclAddr->setDSOLocal(true);
  IRBuilder<> Mutator(InContext);

  DenseMap<SymbolStringPtr, DenseSet<SymbolStringPtr>> SymbolsToMap;
  for (auto &Fn : Module->getFunctionList()) {
    auto Candidates = Walker(Fn);
    // callees
    if (!Candidates.empty()) {
      Mutator.SetInsertPoint(&(Fn.getEntryBlock().front()));
      auto ImplAddrToUint =
          Mutator.CreatePtrToInt(&Fn, Type::getInt64Ty(InContext));
      auto LoadSpeculatorAddr = Mutator.CreateLoad(Type::getInt8PtrTy(InContext),SpeclAddr);
      Mutator.CreateCall(RTFTy, RTFn, {LoadSpeculatorAddr,ImplAddrToUint});
      auto FuncName = ESession.intern(Fn.getName());
      SymbolsToMap[FuncName] = internAllFns(std::move(Candidates));
    }
  }

  assert(!verifyModule(*Module) && "Speculation Instrumentation breaks IR?");
  auto &SpecMap = S;
  for (auto &Symbol : SymbolsToMap) {
    auto Target = Symbol.first;
    auto Likely = Symbol.second;
    // Appending Queries on the same symbol but with different callback
    // Callback should be OnReady,So Imap can track Impl Symbols.
    ESession.lookup(
        JITDylibSearchList({{&R.getTargetJITDylib(), false}}),
        SymbolNameSet({Target}), SymbolState::Ready,
        // FIX ME: Move Capture - Likely, Target, when we have C++14
        [&SpecMap, Likely, Target, &ESession](Expected<SymbolMap> ResSymMap) {
          if (ResSymMap) {
            auto RAddr = (*ResSymMap)[Target].getAddress();
            SpecMap.registerSymbolsWithAddr(RAddr, Likely);
            //SpecMap.speculateFor(RAddr);
          } else {
            ESession.reportError(ResSymMap.takeError());
            return;
          }
        },
        NoDependenciesToRegister);
  }
  NextLayer.emit(std::move(R), std::move(TSM));
}

SpeculationLayer::CalleSet SpeculationLayer::Walk(const Function &F) {
  CalleSet Candidates;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto Call = (dyn_cast<CallInst>(&I))) {
        auto Callee = Call->getCalledFunction();
        if (Callee)
          Candidates.insert(Callee);
      } else if (auto Call = (dyn_cast<InvokeInst>(&I))) {
        auto Callee = Call->getCalledFunction();
        if (Callee)
          Candidates.insert(Callee);
      } else {
      }
    }
  }
  return Candidates;
}
DenseSet<SymbolStringPtr> SpeculationLayer::internAllFns(CalleSet &&AR) {
  DenseSet<SymbolStringPtr> Symbols;
  auto &ESession = this->getExecutionSession();
  for (auto Name : AR)
    Symbols.insert(ESession.intern(Name->getName()));
  return Symbols;
}

} // namespace orc
} // namespace llvm