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

void global_free(){
llvm::errs() << "\n Global free";
}
class LazyJIT{
private:
    //Order is important
    orc::ExecutionSession ES;
    std::unique_ptr<LazyCallThroughManager> LCTM;

    orc::RTDyldObjectLinkingLayer linklayer;
    orc::IRCompileLayer compilelayer;
   
    orc::Imap imap;
    orc::Speculator smap;
    orc::SpeculationLayer SL;
   
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
    LCTM(cantFail(orc::createLocalLazyCallThroughManager(MchBuilder.getTargetTriple(),this->ES,reinterpret_cast<uint64_t>(global_free)))),
    linklayer(this->ES,[](){
    return llvm::make_unique<SectionMemoryManager>();}),
    compilelayer(this->ES,linklayer,ConcurrentIRCompiler(std::move(MchBuilder))),
    smap(imap,this->ES),
    SL(this->ES,compilelayer,smap),
    LazyStubs(this->ES,SL,*LCTM,std::move(orc::createLocalIndirectStubsManagerBuilder(std::move(triple)))),
    
    TSC(llvm::make_unique<LLVMContext>()),
    CompileThreads (nullptr),
    DL(std::move(DL)),
    Mangle(ES,DL)
{

    ES.getMainJITDylib().setGenerator(
    cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));

    LazyStubs.setImap(&imap);
   
 
    this->CompileThreads = llvm::make_unique<ThreadPool>(4);

    ES.setDispatchMaterialization(
        [this](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
          auto SharedMU = std::shared_ptr<MaterializationUnit>(std::move(MU));
          auto Work = [SharedMU, &JD]() { SharedMU->doMaterialize(JD); };
          CompileThreads->async(std::move(Work));   
        });
    
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

        return LazyStubs.add(ES.getMainJITDylib(),std::move(M));
    }
    void loadCurrentProcessSymbols(StringRef JName){
       ES.getJITDylibByName(JName)->setGenerator(cantFail(DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));
    }

    Expected<JITEvaluatedSymbol> lookup(StringRef Name){
        return ES.lookup({&ES.getMainJITDylib()},Name);
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
        
        this->dumpState();
       
    }
};

int main()
{

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    SMDiagnostic error1,error2,error3;

    auto JITEngine = LazyJIT::CreateJITEngine();
    if(!JITEngine)
        JITEngine.takeError();

    
    ThreadSafeContext Ctx1(llvm::make_unique<LLVMContext>());
    auto M1 = parseIRFile("myapp.ll", error1,*Ctx1.getContext());
    auto G = (**JITEngine).addModule(ThreadSafeModule(std::move(M1), std::move(Ctx1)));
    if(G){}

    ThreadSafeContext Ctx2(llvm::make_unique<LLVMContext>());
    auto M2 = parseIRFile("appfns.ll",error2,*Ctx2.getContext());
    auto h1 = (**JITEngine).addModule(ThreadSafeModule(std::move(M2), std::move(Ctx2)));
    if(h1){}
    
    ThreadSafeContext Ctx3(llvm::make_unique<LLVMContext>());
    auto M3 = parseIRFile("appdata.ll",error3,*Ctx3.getContext());
    auto h = (**JITEngine).addModule(ThreadSafeModule(std::move(M3), std::move(Ctx3)));
    if(h){}


    auto R = (**JITEngine).lookup("main");
    if(!R){
        llvm::errs() << "\n Error";
    }
    


    using Type = int ();
     
   llvm::errs() << "Ans is : "<< ((Type*) (*R).getAddress())();
    
   llvm::errs() << "\n After WorkSet";
   

   
   llvm::errs() << "\n Before JIT destruction ";
    return 0;
}



