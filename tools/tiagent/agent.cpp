#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/IntervalMap.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/OrcMCJITReplacement.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"

#include <jni.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <mutex>
#include "jvmti.h"

#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>

#include "common.h"
#include "jni_wrapper.h"

#define LOG_PREFIX "/data/data/com.eugene.sum/"
// #define LOG_PREFIX "/usr/local/google/home/ezemtsov/projects/j/"

using namespace llvm;
using namespace llvm::orc;


static std::mutex g_print_mutex;
template <class T>
void print(const T &x) {
  std::lock_guard<std::mutex> lock(g_print_mutex);
  std::error_code EC;
  raw_fd_ostream file_stream(LOG_PREFIX "debug_log.txt", EC, sys::fs::F_Append);
  file_stream << x << "\n";
  file_stream.close();
}

bool IsDebuggerPresent()
{
    char buf[1024];
    bool debugger_present = false;

    int status_fd = open("/proc/self/status", O_RDONLY);
    if (status_fd == -1)
        return 0;

    int num_read = read(status_fd, buf, sizeof(buf));

    if (num_read > 0)
    {
        static const char TracerPid[] = "TracerPid:";
        char *tracer_pid;

        buf[num_read] = 0;
        tracer_pid    = strstr(buf, TracerPid);
        if (tracer_pid)
            debugger_present = (atoi(tracer_pid + sizeof(TracerPid) - 1) != 0);
    }
    close(status_fd);
    print(buf);

    return debugger_present;
}

JNIEXPORT jint JNICALL fortytwo (JNIEnv *env, jobject instance, jint a, jint b) {
  return 42;
}

typedef jint (*add_ptr)(JNIEnv *, jclass, jint, jint);


typedef ArrayRef<uint8_t> Codeblock;

class SectionMemoryManagerWrapper : public SectionMemoryManager {
  SectionMemoryManager &SM;
  std::function<void(Codeblock)> report_alloc_;

public:
  SectionMemoryManagerWrapper(SectionMemoryManager &sm,
                              std::function<void(Codeblock)> report) :
      SM(sm), report_alloc_(std::move(report)) {}

  SectionMemoryManagerWrapper(const SectionMemoryManagerWrapper&) = delete;
  void operator=(const SectionMemoryManagerWrapper&) = delete;
  ~SectionMemoryManagerWrapper() override {}

  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override {
    auto result = SM.allocateCodeSection(Size, Alignment, SectionID, SectionName);
    report_alloc_(Codeblock(result, Size));
    return result;
  }


  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool isReadOnly) override {
    return SM.allocateDataSection(Size, Alignment, SectionID,
                                  SectionName, isReadOnly);
  }

  bool finalizeMemory(std::string *ErrMsg = nullptr) override {
    return SM.finalizeMemory(ErrMsg);
  }

  void invalidateInstructionCache() override {
    SM.invalidateInstructionCache();
  }
};

class KaleidoscopeJIT {
private:
  std::unique_ptr<TargetMachine> TM;
  const DataLayout DL;
  RTDyldObjectLinkingLayer<> ObjectLayer;
  IRCompileLayer<decltype(ObjectLayer)> CompileLayer;

public:
  typedef decltype(CompileLayer)::ModuleSetHandleT ModuleHandle;

  KaleidoscopeJIT()
      : TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
        CompileLayer(ObjectLayer, SimpleCompiler(*TM))
  {}

  TargetMachine &getTargetMachine() { return *TM; }

  ModuleHandle addModule(std::unique_ptr<Module> M,
                         std::unique_ptr<SectionMemoryManagerWrapper> MM) {
    auto Resolver = createLambdaResolver(
        [&](const std::string &Name) {
          if (auto Sym = CompileLayer.findSymbol(Name, false))
            return Sym;
          return JITSymbol(nullptr);
        },
        [](const std::string &Name) {
          if (auto SymAddr =
                RTDyldMemoryManager::getSymbolAddressInProcess(Name))
            return JITSymbol(SymAddr, JITSymbolFlags::Exported);
          return JITSymbol(nullptr);
        });

    print(*M);

    std::vector<std::unique_ptr<Module>> Ms;
    Ms.push_back(std::move(M));

    return CompileLayer.addModuleSet(std::move(Ms),
              std::move(MM),
              std::move(Resolver));
  }

  JITSymbol findSymbol(const std::string Name) {
    std::string MangledName;
    raw_string_ostream MangledNameStream(MangledName);
    Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
    return CompileLayer.findSymbol(MangledNameStream.str(), true);
  }
};

static std::mutex g_codetree_mutex;
static IntervalMap<uint64_t, uint64_t> *g_ret_pc_range_to_native_func;
static DenseMap<uint64_t, uint64_t> *g_ret_pc_to_native_func;

void *lookup_native_func() {
  std::lock_guard<std::mutex> lock(g_codetree_mutex);
  uint64_t ret_addr = (uint64_t)__builtin_return_address(0);
  uint64_t native_func = g_ret_pc_to_native_func->lookup(ret_addr);
  if (native_func != 0)
    return reinterpret_cast<void *>(native_func);

  native_func = g_ret_pc_range_to_native_func->lookup(ret_addr, 0);
  if (native_func != 0)
    g_ret_pc_to_native_func->insert({ret_addr, native_func});
  return reinterpret_cast<void *>(native_func);
}

void register_native_func(Codeblock block, void *native_func) {
  std::lock_guard<std::mutex> lock(g_codetree_mutex);
  g_ret_pc_range_to_native_func->insert(
    (uint64_t)block.data(),
    ((uint64_t)block.data()) + block.size(),
    (uint64_t)native_func);
}

class Codegen {
 public:
  Codegen() {
    g_ret_pc_range_to_native_func = new IntervalMap<uint64_t, uint64_t> (allocator_);
    g_ret_pc_to_native_func = new DenseMap<uint64_t, uint64_t>(10000);

    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
    llvm::sys::DynamicLibrary::AddSymbol("wrap_ref", reinterpret_cast<void*>(wrap_ref));
    llvm::sys::DynamicLibrary::AddSymbol("unwrap_ref", reinterpret_cast<void*>(unwrap_ref));
    llvm::sys::DynamicLibrary::AddSymbol("print", reinterpret_cast<void*>(print<char *>));
    llvm::sys::DynamicLibrary::AddSymbol("lookup_native_func", reinterpret_cast<void*>(lookup_native_func));
    InitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    context_ = make_unique<LLVMContext>();
    jit_ = make_unique<KaleidoscopeJIT>();
  }

  void *jit_new_stub(const MethodSignature &sig) {
    FunctionType *func_type = ConvertSignatureToFunctionType(sig, *context_);
    if (func_type == nullptr) {
      return nullptr;
    }
    auto short_sig_str = SignatureToShortString(sig);
    std::string name("stub_");
    name += short_sig_str.data();

    auto module = make_unique<Module>("Module_" + name, *context_);
    auto M = module.get();
    M->setDataLayout(jit_->getTargetMachine().createDataLayout());
    auto ptr_to_ptr_func_type = FunctionType::get(
        Type::getInt8Ty(*context_)->getPointerTo(),
        { Type::getInt8Ty(*context_)->getPointerTo() }, false);
    Function* wrap_ref = llvm::Function::Create(ptr_to_ptr_func_type,
                                                Function::ExternalLinkage,
                                                "wrap_ref", M);


    Function* unwrap_ref = llvm::Function::Create(ptr_to_ptr_func_type,
                                                Function::ExternalLinkage,
                                                "unwrap_ref", M);

    Function* lookup_native_func = llvm::Function::Create(
        FunctionType::get(func_type->getPointerTo(), { }, false),
                          Function::ExternalLinkage,
                          "lookup_native_func", M);

    Function *F = cast<Function>(M->getOrInsertFunction(name, func_type));
    BasicBlock *BB = BasicBlock::Create(*context_, "", F);
    IRBuilder<> builder(BB);

    std::vector<Argument *> args;
    for (auto it = F->arg_begin(); it != F->arg_end(); ++it) {
      args.push_back(&*it);
    }

    std::vector<Value *> values;
    for (auto arg : args) {
      if (arg->getType()->isPointerTy()) {
        values.push_back(builder.CreateCall(wrap_ref, { arg }));
      } else {
        values.push_back(arg);
      }
    }

    Value* func_value = builder.CreateCall(lookup_native_func, {});
    Value *ret = builder.CreateCall(func_value, values);
    if (func_type->getReturnType()->isVoidTy()) {
      builder.CreateRetVoid();
    } else if ( func_type->getReturnType()->isPointerTy()) {
      ret = builder.CreateCall(unwrap_ref, { ret });
      builder.CreateRet(ret);
    } else {
      builder.CreateRet(ret);
    }

    auto MM = make_unique<SectionMemoryManagerWrapper>(memory_manager_,
        [this, short_sig_str](Codeblock blk) {
          signature_to_code_.insert({short_sig_str, blk});
        });
    jit_->addModule(std::move(module), std::move(MM));
    auto func_symbol = jit_->findSymbol(name);
    void *result = (void*)func_symbol.getAddress();

    return result;
  }

  void *get_transparent_wrapper(const char *name,
                                char *signature,
                                void *func_ptr) {
    auto parsed_sig = ParseJavaSignature(signature, 2);
    if (!parsed_sig) {
      return nullptr;
    }

    auto sig_str = SignatureToShortString(parsed_sig.getValue());
    Codeblock prototype_block = signature_to_code_.lookup(sig_str);
    void *sym_address = nullptr;
    if (prototype_block.size() == 0) {
      sym_address = jit_new_stub(parsed_sig.getValue());
    }

    prototype_block = signature_to_code_.lookup(sig_str);
    if (prototype_block.size() == 0) {
      return nullptr;
    }
    if (sym_address != nullptr && sym_address != (void *)prototype_block.data()) {
      print("ERROR: non-0 offset function start");
      print(name);
      print(signature);
      print(((uint8_t *)sym_address) - prototype_block.data());
      return nullptr;
    }

    uint8_t *new_block_mem = memory_manager_.allocateCodeSection(
                                  prototype_block.size(), 16, 1, ".text");

    memcpy(new_block_mem, prototype_block.data(), prototype_block.size());
    memory_manager_.finalizeMemory();
    memory_manager_.invalidateInstructionCache();
    Codeblock copy_block(new_block_mem, prototype_block.size());
    register_native_func(copy_block, func_ptr);

    return (void *)new_block_mem;
  }

 private:
  std::unique_ptr<LLVMContext> context_;
  std::unique_ptr<KaleidoscopeJIT> jit_;
  StringMap<Codeblock> signature_to_code_;
  SectionMemoryManager memory_manager_;
  std::string error_str;
  IntervalMap<uint64_t, uint64_t>::Allocator allocator_;
};

void *gen_function(char* name, char *signature, void *func_ptr) {
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> lock(g_mutex);
  static Codegen codegen;
  void *result = codegen.get_transparent_wrapper(name, signature, func_ptr);
  if (result == nullptr) {
    print("codegen error");
    print(name);
    print(signature);
  } else {
    print("codegen OK");
    print(name);
    print(signature);
  }
  return result;
}


jvmtiEnv* CreateJvmtiEnv(JavaVM* vm) {
  jvmtiEnv* jvmti_env;
  jint result = vm->GetEnv((void**)&jvmti_env, JVMTI_VERSION_1_2);
  if (result != JNI_OK) {
    return nullptr;
  }
  return jvmti_env;
}

void JNICALL
NativeMethodBind(jvmtiEnv *ti,
            JNIEnv* jni_env,
            jthread thread,
            jmethodID method,
            void* address,
            void** new_address_ptr) {
  char* method_name_ptr = nullptr;
  char* method_signature_ptr = nullptr;
  char* class_signature_ptr = nullptr;
  jclass declaring_class = nullptr;
  jvmtiError error = ti->GetMethodName(method, &method_name_ptr, &method_signature_ptr, nullptr);
  error = ti->GetMethodDeclaringClass(method, &declaring_class);
  if (error != JNI_OK) return;
  error = ti->GetClassSignature(declaring_class, &class_signature_ptr, nullptr);
  if (error != JNI_OK) return;
  {
    void *f = nullptr;
    //print("NativeMethodBind");
    //print(method_name_ptr);
    //if (std::string(method_name_ptr) == "add")
      f = gen_function(method_name_ptr, method_signature_ptr, address);
    if (f) {
      *new_address_ptr = f;
    }
  }

  jni_env->DeleteLocalRef(declaring_class);
  ti->Deallocate((unsigned char *)method_name_ptr);
  ti->Deallocate((unsigned char *)method_signature_ptr);
  ti->Deallocate((unsigned char *)class_signature_ptr);
}

JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
  return Agent_OnAttach(vm, options, reserved);
}

JNIEXPORT jint JNICALL
Agent_OnAttach(JavaVM* vm, char* options, void* reserved) {
  jvmtiError error;

  jvmtiEnv* ti = CreateJvmtiEnv(vm);
  if (ti == nullptr) return 1;

  // Hook up event callbacks
  jvmtiEventCallbacks callbacks = {};
  callbacks.NativeMethodBind = &NativeMethodBind;
  error = ti->SetEventCallbacks(&callbacks, sizeof(callbacks));
  if (error != JNI_OK) return 1;

  jvmtiCapabilities caps;
  error = ti->GetPotentialCapabilities(&caps);
  if (error != JNI_OK) return 1;

  error = ti->AddCapabilities(&caps);
  if (error != JNI_OK) return 1;

  error = ti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_NATIVE_METHOD_BIND, nullptr);
  if (error != JNI_OK) return 1;


  // while (!IsDebuggerPresent()) {
  //   sleep(0);
  // }

  error = RegisterNewJniTable(ti);
  if (error != JNI_OK) return 1;

  print("Agent initialized!");
  return 0;
}
