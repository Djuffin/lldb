#include "llvm/ADT/STLExtras.h"
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
  std::ofstream myfile;
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

struct Allocation
{
  uint8_t *ptr;
  uintptr_t size;
};

class SectionMemoryManagerWrapper : public SectionMemoryManager {
  SectionMemoryManager &SM;
  std::vector<Allocation> &Allocs;
public:
  SectionMemoryManagerWrapper(SectionMemoryManager &sm,
                              std::vector<Allocation> &allocs) :
      SM(sm), Allocs(allocs) {}
  SectionMemoryManagerWrapper(const SectionMemoryManagerWrapper&) = delete;
  void operator=(const SectionMemoryManagerWrapper&) = delete;
  ~SectionMemoryManagerWrapper() override {}

  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override {
    auto result = SM.allocateCodeSection(Size, Alignment, SectionID, SectionName);
    Allocs.push_back({result, Size});
    return result;
  }


  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool isReadOnly) override {
    print("allocate data");
    print(Size);
    return SM.allocateDataSection(Size, Alignment, SectionID, SectionName,
                                  isReadOnly);

  }

  bool finalizeMemory(std::string *ErrMsg = nullptr) override {
    print("binary code");

    Allocation &last_alloc = Allocs.back();
    ArrayRef<uint8_t> binary(last_alloc.ptr, last_alloc.size);
    auto range = make_range(binary.begin(), binary.end());
    std::string text = formatv("{0:@(x-2)}", range).str();
    print(text);


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
  SectionMemoryManager RealMemoryManager;
  std::vector<Allocation> Allocs;

public:
  typedef decltype(CompileLayer)::ModuleSetHandleT ModuleHandle;

  KaleidoscopeJIT()
      : TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
        CompileLayer(ObjectLayer, SimpleCompiler(*TM))
  {
    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
  }

  TargetMachine &getTargetMachine() { return *TM; }

  ModuleHandle addModule(std::unique_ptr<Module> M) {
    // Build our symbol resolver:
    // Lambda 1: Look back into the JIT itself to find symbols that are part of
    //           the same "logical dylib".
    // Lambda 2: Search for external symbols in the host process.
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
    // Build a singleton module set to hold our module.
    std::vector<std::unique_ptr<Module>> Ms;
    Ms.push_back(std::move(M));

    auto MM = make_unique<SectionMemoryManagerWrapper>(RealMemoryManager,
                                                       Allocs);

    // Add the set to the JIT with the resolver we created above and a newly
    // created SectionMemoryManager.
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

void *wrap_f_const(uint64_t x) {
  uint64_t ret_addr = (uint64_t)__builtin_return_address(0);
  print("return address");
  print(ret_addr);
  return reinterpret_cast<void *>(x);
}

class Codegen {
 public:
  Codegen() {
    llvm::sys::DynamicLibrary::AddSymbol("wrap_ref", reinterpret_cast<void*>(wrap_ref));
    llvm::sys::DynamicLibrary::AddSymbol("print", reinterpret_cast<void*>(print<char *>));
    llvm::sys::DynamicLibrary::AddSymbol("wrap_f_const", reinterpret_cast<void*>(wrap_f_const));
    InitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    context_ = make_unique<LLVMContext>();
    jit_ = make_unique<KaleidoscopeJIT>();
  }

  void *gen_transparent_wrapper(const char *name, char *signature, void *func_ptr) {
    FunctionType *func_type = GetFunctionType(signature, 2);
    if (func_type == nullptr) {
      return nullptr;
    }

    auto module = make_unique<Module>(std::string(name) + "_mod", *context_);
    auto M = module.get();
    M->setDataLayout(jit_->getTargetMachine().createDataLayout());
    Function* wrap_ref = llvm::Function::Create(GetFunctionType("(L;)L;"),
                                                Function::ExternalLinkage,
                                                "wrap_ref", M);
    Function* wrap_f_const = llvm::Function::Create(
        FunctionType::get(func_type->getPointerTo(),
                          std::vector<Type*> { Type::getInt64Ty(*context_) },
                          false),
                          Function::ExternalLinkage,
                          "wrap_f_const", M);

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
        values.push_back(builder.CreateCall(wrap_ref,
                                            std::vector<Value *>{arg}));
      } else {
        values.push_back(arg);
      }
    }

    GlobalVariable * header_const = builder.CreateGlobalString("function called");


    Value* func_address = builder.getInt64((int64_t)func_ptr);
    Value* func_value = builder.CreateCall(wrap_f_const,
                std::vector<Value *> { func_address });

    // Value* func_value = builder.CreateIntToPtr(
    //                         builder.getInt64((uint64_t)func_ptr),
    //                         func_type->getPointerTo());
    Value *ret = builder.CreateCall(func_value, values);
    if (func_type->getReturnType()->isVoidTy()) {
      builder.CreateRetVoid();
    } else {
      builder.CreateRet(ret);
    }

    jit_->addModule(std::move(module));
    auto func_symbol = jit_->findSymbol(name);
    void *result = (void*)func_symbol.getAddress();

    return result;
  }

 private:
  FunctionType *GetFunctionType(const char *str, int extraPtrArgs = 0) {
    llvm::Optional<MethodSignature> signature = ParseJavaSignature(str,
                                                                extraPtrArgs);
    if (!signature) return nullptr;
    return ConvertSignatureToFunctionType(signature.getValue(), *context_);
  }

  std::unique_ptr<LLVMContext> context_;
  std::unique_ptr<KaleidoscopeJIT> jit_;
  std::string error_str;
};

void *gen_function(char* name, char *signature, void *func_ptr) {
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> lock(g_mutex);
  static Codegen codegen;
  static int n = 0;
  char name_buffer[200];
  sprintf(name_buffer, "%s_ti_%d", name, n++);
  void *result = codegen.gen_transparent_wrapper(name_buffer, signature, func_ptr);
  if (result == nullptr) {
    print("codegen error");
    print(name_buffer);
    print(signature);
  } else {
    print("codegen OK");
    print(name_buffer);
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

  //error = RegisterNewJniTable(ti);
  if (error != JNI_OK) return 1;

  print("Agent initialized!");
  return 0;
}
