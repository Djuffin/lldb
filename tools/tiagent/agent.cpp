#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/DynamicLibrary.h"
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

#include <jni.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <mutex>
#include "jvmti.h"

#define LOG_PREFIX "/data/data/com.eugene.sum/"
// #define LOG_PREFIX "/usr/local/google/home/ezemtsov/projects/j/"

using namespace llvm;

JNIEXPORT jint JNICALL fortytwo (JNIEnv *env, jobject instance, jint a, jint b) {
  return 42;
}

typedef jint (*add_ptr)(JNIEnv *, jclass, jint, jint);

void print(const char *str)  {
  std::ofstream myfile;
  myfile.open (LOG_PREFIX "debug_log.txt", std::ios::out | std::ios::app);
  myfile << str << "\n";
  myfile.close();
}

int wrap_int(int x) {
  return x;
}

void* wrap_ref(void *ref) {
  return ref;
}

class Codegen {
 public:
  Codegen() {
    llvm::sys::DynamicLibrary::AddSymbol("wrap_int", reinterpret_cast<void*>(wrap_int));
    llvm::sys::DynamicLibrary::AddSymbol("wrap_ref", reinterpret_cast<void*>(wrap_ref));
    InitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    context_ = make_unique<LLVMContext>();

    EngineBuilder EB(make_unique<Module>("default_module", *context_));
    EB.setMCJITMemoryManager(make_unique<SectionMemoryManager>());
    EB.setUseOrcMCJITReplacement(true);
    EB.setErrorStr(&error_str);
    engine_ = std::unique_ptr<ExecutionEngine>(EB.create());
  }

  void *gen_transparent_wrapper(const char *name, char *signature, void *func_ptr) {
    FunctionType *func_type = ParseJavaSignature(signature, 2);
    if (func_type == nullptr) {
      return nullptr;
    }

    auto module = make_unique<Module>(std::string(name) + "_mod", *context_);
    auto M = module.get();
    Function* wrap_ref = llvm::Function::Create(ParseJavaSignature("(L;)L;"),
                                                Function::ExternalLinkage,
                                                "wrap_ref", M);

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
        values.push_back(builder.CreateCall(wrap_ref, std::vector<Value *>{arg}));
      } else {
        values.push_back(arg);
      }
    }

    Value* func_value = builder.CreateIntToPtr(builder.getInt64((uint64_t)func_ptr),func_type->getPointerTo());
    Value *ret = builder.CreateCall(func_value, values);
    builder.CreateRet(ret);

    engine_->addModule(std::move(module));
    void *result = engine_->getPointerToFunction(F);
    return result;
  }

 private:
  llvm::Type *GetPointerType() {
    return Type::getVoidTy(*context_)->getPointerTo();
  }

  FunctionType *ParseJavaSignature(const char *str, int extraPtrArgs = 0) {
    if (str == nullptr) return nullptr;
    const char* ptr = str;
    std::function<Type*()> consumeType =
      [&ptr, &consumeType, this]()->Type * {
      switch (*ptr) {
        case 'Z':
          ptr++;
          return Type::getInt8Ty(*context_);
        case 'B':
          ptr++;
          return Type::getInt8Ty(*context_);
        case 'C':
          ptr++;
          return Type::getInt16Ty(*context_);
        case 'S':
          ptr++;
          return Type::getInt16Ty(*context_);
        case 'I':
          ptr++;
          return Type::getInt32Ty(*context_);
        case 'J':
          ptr++;
          return Type::getInt64Ty(*context_);
        case 'F':
          ptr++;
          return Type::getFloatTy(*context_);
        case 'D':
          ptr++;
          return Type::getDoubleTy(*context_);
        case 'V':
          ptr++;
          return Type::getVoidTy(*context_);
        case 'L':
          while (*ptr && *ptr != ';') ptr++;
          if (*ptr == ';') {
            ptr++;
            return GetPointerType();
          } else {
            return nullptr;
          }
        case '[': {
          ptr++;
          if (consumeType())
            return GetPointerType();
          else
            return nullptr;
        }
        default:
          return nullptr;
      };
    };

    if (*ptr != '(') return nullptr;
    ptr++;
    std::vector<Type*> args;
    for (;extraPtrArgs > 0; --extraPtrArgs) {
      args.push_back(GetPointerType());
    }
    Type *returnType;
    while (*ptr && *ptr != ')') {
      auto type = consumeType();
      if (type == nullptr) return nullptr;
      args.push_back(type);
    }
    if (*ptr != ')') return nullptr;
    ptr++;
    returnType = consumeType();
    if (returnType == nullptr || *ptr) return nullptr;

    return FunctionType::get(returnType, args, false);

  }

  std::unique_ptr<LLVMContext> context_;
  std::unique_ptr<ExecutionEngine> engine_;
  std::string error_str;
};

void *gen_function(char* name, char *signature, void *func_ptr) {
  static Codegen codegen;
  static int n = 0;
  char name_buffer[200];
  sprintf(name_buffer, "%s_ti_%d", name, n++);
  void *result = codegen.gen_transparent_wrapper(name_buffer, signature, func_ptr);
  if (result == nullptr) {
    print("codegen error");
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
    void *f = gen_function(method_name_ptr, method_signature_ptr, address);
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

  return 0;
}
