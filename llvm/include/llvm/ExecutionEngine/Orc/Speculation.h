//===-- Speculation.h - Speculative Compilation --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Contains the definition to support speculative compilation when laziness is
// enabled.
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_SPECULATION_H
#define LLVM_EXECUTIONENGINE_ORC_SPECULATION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/Layer.h"

#include <functional>
#include <mutex>
#include <vector>

namespace llvm {
namespace orc {

class Speculator;

// Track the Impls (JITDylib,Symbols) of Symbols while lazy call through
// trampolines are created. Operations are guarded by locks tp ensure that Imap
// stays in consistent state after read/write

class Imap {
  friend class Speculator;

public:
  using ImplPair = std::pair<SymbolStringPtr, JITDylib *>;
  using ImapTy = DenseMap<SymbolStringPtr, ImplPair>;

  void trackImpls(SymbolAliasMap ImplMaps, JITDylib *SrcJD);

private:
  ImplPair getImplFor(const SymbolStringPtr &StubAddr) {
    std::lock_guard<std::mutex> Lockit(ConcurrentAccess);
    auto Position = Maps.find(StubAddr);
    assert(Position != Maps.end() &&
           "ImplSymbols are not tracked for this Symbol?");
    return Position->getSecond();
  }

  std::mutex ConcurrentAccess;
  ImapTy Maps;
};

class Speculator {
public:
  using TargetFAddr = JITTargetAddress;
  using SpeculationMap = DenseMap<TargetFAddr, SymbolNameSet>;

  explicit Speculator(Imap &Impl, ExecutionSession &ref)
      : AliaseeImplTable(Impl), ES(ref),GlobalSpecMap(0) {}

  Speculator(const Speculator&) = delete;

  Speculator(Speculator&&) = delete;

  // Speculation Layer registers likely function through this method.
  void registerSymbolsWithAddr(TargetFAddr, SymbolNameSet);

  // Speculatively compile likely functions for the given Stub Address.
  // destination of __orc_speculate_for jump
  void speculateFor(JITTargetAddress);

  Imap &getImapRef() { return AliaseeImplTable; }

  ~Speculator(){
    llvm::errs() << "\n Premature Destruction of Speculator ";
  }

private:
  void launchCompile(JITTargetAddress FAddr) {
    auto It = GlobalSpecMap.find(FAddr);
    assert(It != GlobalSpecMap.end() &&
           "launching speculative compiles for unexpected function address?");
    for (auto &Calle : It->getSecond()) {
      auto ImplSymbol = AliaseeImplTable.getImplFor(Calle);
      const auto &ImplSymbolName = ImplSymbol.first;
      auto *ImplJD = ImplSymbol.second;
      ES.lookup(JITDylibSearchList({{ImplJD, true}}),
                SymbolNameSet({ImplSymbolName}), SymbolState::Ready,
                [this](Expected<SymbolMap> Result) {
                  if (auto Err = Result.takeError())
                    ES.reportError(std::move(Err));
                },
                NoDependenciesToRegister);
    }
  }

  std::mutex ConcurrentAccess;
  Imap &AliaseeImplTable;
  ExecutionSession &ES;
  SpeculationMap GlobalSpecMap;
};

// Walks the LLVM Module and collect likely functions for each LLVM Function if
// any, and instruments the IR with runtime call named __orc_speculate_for.
// Construct IR level function mapping.

class SpeculationLayer : public IRLayer {

public:
  using CalleSet = DenseSet<Function *>;
  using WalkerTy = std::function<CalleSet(const Function &)>;

  SpeculationLayer(ExecutionSession &ES, IRCompileLayer &BaseLayer,
                   Speculator &Spec)
      : IRLayer(ES), NextLayer(BaseLayer), S(Spec) {}

  // For each function F
  // Walk F to find likely functions.
  // If F has callers, then instrument F
  void emit(MaterializationResponsibility R, ThreadSafeModule TSM);

  void setModuleWalker(WalkerTy W) { Walker = std::move(W); }

  Speculator &getSpeculator() const { return S; }

  IRCompileLayer &getCompileLayer() const { return NextLayer; }

  static CalleSet Walk(const Function &);

  DenseSet<SymbolStringPtr> internAllFns(CalleSet &&AR);

private:
  IRCompileLayer &NextLayer;
  Speculator &S;
  WalkerTy Walker = Walk;
};

} // namespace orc
} // namespace llvm
#endif