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
void Imap::saveImpls(SymbolAliasMap ImplMaps, JITDylib *SrcJD) {
  std::lock_guard<std::mutex> lockit(ConcurrentAccess);
  for (auto &I : ImplMaps) {
    recordImpl(I.first, {I.second.Aliasee, SrcJD});
  }
}

// Speculator methods

void Speculator::registerSymbolsWithAddr(TargetFAddr ImplAddr,
                                         SymbolNameSet likelySymbols) {
  std::lock_guard<std::mutex> lockit(ConcurrentAccess);
  GlobalSpecMap.insert({ImplAddr, std::move(likelySymbols)});
}

void Speculator::speculateFor(JITTargetAddress FAddr) {
  std::lock_guard<std::mutex> lockit(ConcurrentAccess);
  launchCompile(FAddr);
}

// SpeculationLayer methods

void SpeculationLayer::emit(MaterializationResponsibility R,
                            ThreadSafeModule TSM) {
  auto Module = TSM.getModule();
  assert(Module && "Speculation Layer received Null Module");
  auto &ESession = this->getExecutionSession();
  auto &InContext = Module->getContext();

  // reinterpret_cast of Stub Address to i64
  auto RTFTy = FunctionType::get(Type::getVoidTy(InContext),
                                 Type::getInt64Ty(InContext), false);
  auto RTFn = Function::Create(RTFTy, Function::LinkageTypes::ExternalLinkage,
                               "__orc_speculate_for", *Module);
  IRBuilder<> Mutator(InContext);

  DenseMap<SymbolStringPtr, DenseSet<SymbolStringPtr>> SymbolsToMap;

  for (auto &Fn : Module->getFunctionList()) {
    auto Candidates = Walker(Fn);
    // callees
    if (!Candidates.empty()) {
      Mutator.SetInsertPoint(&(Fn.getEntryBlock().front()));
      auto ImplAddrToUint =
          Mutator.CreatePtrToInt(&Fn, Type::getInt64Ty(InContext));
      Mutator.CreateCall(RTFTy, RTFn, {ImplAddrToUint});
      auto FuncName = ESession.intern(Fn.getName());
      SymbolsToMap[FuncName] = internAllFns(std::move(Candidates));
    }
  }

  assert(!verifyModule(*Module) && "Speculation Instrumentation breaks IR?");
  auto &SpecMap = getSpeculator();
  for (auto &Symbol : SymbolsToMap) {
    auto Target = Symbol.first;
    auto likely = Symbol.second;
    // Appending Queries on the same symbol but with different callback action
    ESession.lookup(
        JITDylibSearchList({{&R.getTargetJITDylib(), false}}),
        SymbolNameSet({Symbol.first}), SymbolState::Resolved,
        [&SpecMap, likely, Target, &ESession](Expected<SymbolMap> ResSymMap) {
          if (ResSymMap) {
            auto RAddr = (*ResSymMap)[Target].getAddress();
            SpecMap.registerSymbolsWithAddr(std::move(RAddr), likely);
          } else {
            ESession.reportError(ResSymMap.takeError());
            return; // fail MaterializationResponsibility is called by first
                    // queued query.
          }
        },
        NoDependenciesToRegister);
  }
  NextLayer.emit(std::move(R), std::move(TSM));
}

DenseSet<SymbolStringPtr> SpeculationLayer::internAllFns(WalkerResultTy &&AR) {
  DenseSet<SymbolStringPtr> Symbols;
  auto &ESession = this->getExecutionSession();
  for (auto Name : AR) {
    Symbols.insert(ESession.intern(Name->getName()));
  }
  return Symbols;
}

SpeculationLayer::WalkerResultTy SpeculationLayer::Walk(const Function &F) {
  WalkerResultTy Candidates;
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

} // namespace orc
} // namespace llvm