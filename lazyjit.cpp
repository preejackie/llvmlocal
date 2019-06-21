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

    Error addModule(std::unique_ptr<Module> M){

        return LazyStubs.add(ES.getMainJITDylib(),ThreadSafeModule(std::move(M),TSC));
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

    LLVMContext ContextIn;
    SMDiagnostic error;

    auto JITEngine = LazyJIT::CreateJITEngine();
    if(!JITEngine)
        JITEngine.takeError();

    auto mainll = parseIRFile("myapp.ll",error,ContextIn);      
    auto lib    = parseIRFile("appfns.ll",error,ContextIn);
    auto lib2   = parseIRFile("appdata.ll",error,ContextIn);
    
    
    auto G = (**JITEngine).addModule(std::move(mainll));
    if(G){

    }  
     
    auto l1 = (**JITEngine).addModule(std::move(lib));
    if(l1){
    }
    auto l2 = (**JITEngine).addModule(std::move(lib2));
   if(l2){
   }

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



