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

// Track the Impl JITDylibs of Symbols while lazy call through trampolines
// are created and send the impl details to GlobalSpeculationObserver

class Imap {
  friend class Speculator;

public:
  using ImplPair = std::pair<SymbolStringPtr, JITDylib *>;
  using ImapTy = DenseMap<SymbolStringPtr, ImplPair>;
  void saveImpls(SymbolAliasMap ImplMaps, JITDylib *SrcJD);

private:
  void recordImpl(SymbolStringPtr FaceSymbol, ImplPair Implementations) {
    auto It = Maps.insert({FaceSymbol, Implementations});
    if (It.second == false)
      assert(0 && "Source Entities are already tracked for this Symbol?");
  }

  ImplPair getImplFor(SymbolStringPtr StubAddr) {
    auto Position = Maps.find(StubAddr);
    if (Position != Maps.end()) {
      return Position->getSecond();
    }
    assert(0 && "Source Entities are not tracked for a Symbol?");
  }

  std::mutex ConcurrentAccess;
  ImapTy Maps;
};

class Speculator {

public:
  using TargetFAddr = JITTargetAddress;
  using SpeculationMap = DenseMap<TargetFAddr, SymbolNameSet>;

  Speculator(Imap &Impl) : AliaseeImplTable(Impl) {}

  // Speculation Layer registers likely function through this method.
  void registerSymbolsWithAddr(TargetFAddr, SymbolNameSet);

  // Speculatively compile likely functions for the given Stub Address.
  void speculateFor(JITTargetAddress);

private:
  void launchCompile(JITTargetAddress FAddr) {
    auto It = GlobalSpecMap.find(FAddr);
    if (It != GlobalSpecMap.end()) {
      for (auto &Pos : It->getSecond()) {
        auto SourceEntities = AliaseeImplTable.getImplFor(Pos);
      }
    } else
      assert(0 && "launching compiles for Unexpected Function Address?");
  }

  std::mutex ConcurrentAccess;
  Imap &AliaseeImplTable;
  SpeculationMap GlobalSpecMap;
};

// Walks the LLVM Module and collect likely functions for each LLVM Function if
// any, and instruments the IR with runtime call named __orc_speculate_for.
// Construct IR level function mapping.

class SpeculationLayer : public IRLayer {
public:
  using WalkerResultTy = DenseSet<Function *>;
  using WalkerTy = std::function<WalkerResultTy(const Function &)>;

  SpeculationLayer(ExecutionSession &ES, IRCompileLayer &BaseLayer,
                   Speculator &Spec)
      : IRLayer(ES), NextLayer(BaseLayer), S(Spec) {}

  // For each function F
  // Walk F to find likely functions.
  // If F has callers, then instrument F
  void emit(MaterializationResponsibility R, ThreadSafeModule TSM);

  void setModuleWalker(WalkerTy W) { Walker = std::move(W); }

  Speculator &getSpeculator() const { return S; }

  static WalkerResultTy Walk(const Function &);

  DenseSet<SymbolStringPtr> internAllFns(WalkerResultTy &&AR);

private:
  IRCompileLayer &NextLayer;
  Speculator &S;
  WalkerTy Walker = Walk;
};

} // namespace orc
} // namespace llvm
#endif