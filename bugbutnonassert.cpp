#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/Orc/LazyReexports.h"
#include "llvm/ExecutionEngine/Orc/Speculation.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/IR/IRBuilder.h"
#include <memory>
#include <cstdint>
using namespace llvm;
using namespace orc;

ExitOnError ExitOnErr;

void SymbolNotFound(){
llvm::errs() << "\n Symbol Not Found ??? ";
}

class LazyJIT{
private:
    //Order is important
    orc::ExecutionSession ES;
    std::unique_ptr<LazyCallThroughManager> LCTM;

    orc::RTDyldObjectLinkingLayer linklayer;
    orc::IRCompileLayer compilelayer;
    
    orc::Imap imap;
    orc::Speculator Specls;
    orc::SpeculationLayer SL;
    orc::Speculator* WierdPointer;
    orc::CompileOnDemandLayer LazyStubs;

    orc::ThreadSafeContext TSC;

    using IndirectStubsManagerBuilder =
      std::function<std::unique_ptr<IndirectStubsManager>()>;
   
    std::unique_ptr<ThreadPool> CompileThreads;
    DataLayout DL;
    MangleAndInterner Mangle;
   
public:
LazyJIT(orc::JITTargetMachineBuilder MchBuilder,DataLayout DL,Triple triple)
:
    LCTM(cantFail(orc::createLocalLazyCallThroughManager(MchBuilder.getTargetTriple(),this->ES,
        reinterpret_cast<uint64_t>(SymbolNotFound)))),
    linklayer(this->ES,[](){
    return llvm::make_unique<SectionMemoryManager>();}),
    compilelayer(this->ES,linklayer,ConcurrentIRCompiler(std::move(MchBuilder))),
    Specls(imap,this->ES),
    SL(this->ES,compilelayer,Specls),
    LazyStubs(this->ES,SL,*LCTM,std::move(orc::createLocalIndirectStubsManagerBuilder(std::move(triple)))),
    
    TSC(llvm::make_unique<LLVMContext>()),
    CompileThreads (nullptr),
    DL(std::move(DL)),
    Mangle(ES,this->DL)
{

    ES.getMainJITDylib().setGenerator(
    cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(this->DL.getGlobalPrefix())));

    LazyStubs.setImap(&imap);
   
    this->CompileThreads = llvm::make_unique<ThreadPool>(4);
    ES.setDispatchMaterialization(
        [this](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
          auto SharedMU = std::shared_ptr<MaterializationUnit>(std::move(MU));
          auto Work = [SharedMU, &JD]() { SharedMU->doMaterialize(JD); };
          CompileThreads->async(std::move(Work));   
        });
    
    auto orc_internal_symbol = Mangle("orc_speculator");
    llvm::errs() << "\n JIT Target Addr before absoluteSymbols " << JITTargetAddress(&Specls);

    WierdPointer = &Specls;
    JITEvaluatedSymbol Js(JITTargetAddress(&WierdPointer),JITSymbolFlags::Exported);
    SymbolMap SM;
    SM[orc_internal_symbol] = Js;

    for(auto& o:SM){
        llvm::errs() <<"\n JIT Target in SymbolMap " << o.getSecond().getAddress();
    }

    if( auto Err = ES.getMainJITDylib().define(absoluteSymbols(SM))){
        llvm::errs() <<"\n Error while adding abs Symbol";
    }

}

public:
    static Expected<std::unique_ptr<LazyJIT>> CreateJITEngine(){
        // This returns a expected value;
        auto MchBuilder = orc::JITTargetMachineBuilder::detectHost();
        if(!MchBuilder)
            return MchBuilder.takeError();
        auto DL = MchBuilder->getDefaultDataLayoutForTarget();
        if(!DL)
            return DL.takeError();
        auto triple = MchBuilder->getTargetTriple();
        JITTargetAddress Addr = 0;
        return llvm::make_unique<LazyJIT>(std::move(*MchBuilder),std::move(*DL),std::move(triple));
    }

    Error addModule(ThreadSafeModule M){
      M.getModule()->setDataLayout(DL);
        return LazyStubs.add(ES.getMainJITDylib(),std::move(M));
    }
    void loadCurrentProcessSymbols(){
      ES.getMainJITDylib().setGenerator(cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));
    }

    Expected<JITEvaluatedSymbol> lookup(StringRef Name){
        return ES.lookup({&ES.getMainJITDylib()},Mangle(Name));
    }
    void dumpState(){
        ES.dump(llvm::errs());
    }
  
    ExecutionSession& getES(){
        return ES;
    }
    MangleAndInterner& getM(){
        return Mangle;
    }
    DataLayout& getDL(){
        return DL;
    }

    ~LazyJIT(){
     if (CompileThreads)
      CompileThreads->wait();
        
     //this->dumpState();
       
    }
};

int main(int argc, char *argv[])
{
    ExitOnErr.setBanner(std::string(argv[0]) + ":");

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    SMDiagnostic error1;

    auto JITEngine = ExitOnErr(LazyJIT::CreateJITEngine());
    JITEngine->loadCurrentProcessSymbols();

    ThreadSafeContext Ctx1(llvm::make_unique<LLVMContext>());
    auto M1 = parseIRFile("myapp.ll", error1,*Ctx1.getContext());
    ExitOnErr(JITEngine->addModule(ThreadSafeModule(std::move(M1), std::move(Ctx1))));

    auto R = ExitOnErr(JITEngine->lookup("main"));
    
    using Type = int ();
     
    llvm::errs() << "\nAns is : "<< ((Type*)R.getAddress())();
    
   return 0;
}

//export PATH=/home/preejackie/llvminstallers/orcs/bin:$PATH
//export LD_LIBRARY_PATH=/home/preejackie/llvminstallers/orcs/lib:$LD_LIBRARY_PATH