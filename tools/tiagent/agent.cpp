#include "llvm/ADT/STLExtras.h"
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

void* gen_function() {
  using namespace llvm;
  print("1");
  InitializeNativeTarget();
  LLVMInitializeNativeAsmPrinter();
  LLVMInitializeNativeAsmParser();

  LLVMContext *pContext = new LLVMContext();
  LLVMContext &Context = *pContext;

  // Create some module to put our function into it.
  std::unique_ptr<Module> Owner = make_unique<Module>("test", Context);
  Module *M = Owner.get();
  print("2");

  // Now we're going to create function `foo', which returns an int and takes no
  // arguments.
  Function *FooF =
    cast<Function>(M->getOrInsertFunction("foo", Type::getInt32Ty(Context),
      Type::getDoublePtrTy(Context), Type::getDoublePtrTy(Context),
          Type::getInt32Ty(Context), Type::getInt32Ty(Context)/*, nullptr*/));

  // Add a basic block to the FooF function.
  BasicBlock *BB = BasicBlock::Create(Context, "EntryBlock", FooF);

  // Create a basic block builder with default parameters.  The builder will
  // automatically append instructions to the basic block `BB'.
  IRBuilder<> builder(BB);

  // Tell the basic block builder to attach itself to the new basic block
  builder.SetInsertPoint(BB);

  // Get pointer to the constant `142'.
  Value *Result = builder.getInt32(142);

  // Create the return instruction and add it to the basic block.
  builder.CreateRet(Result);

  print("3");
  // Now we create the JIT.
  EngineBuilder EB(std::move(Owner));
  EB.setMCJITMemoryManager(make_unique<SectionMemoryManager>());
  EB.setUseOrcMCJITReplacement(true);
  std::string error_str = "No error";
  EB.setErrorStr(&error_str);
  ExecutionEngine* EE = EB.create();
  if (EE == nullptr) {
    print(error_str.c_str());
    return nullptr;
  }
  print("4");
  EE->finalizeObject();

  print("5");
  //delete EE;
  //llvm_shutdown();
  void *result = EE->getPointerToFunction(FooF);
  if (result == nullptr) {
    print("null func");
  } else {
    print("6");
    ((add_ptr)result)(nullptr, nullptr, 3, 4);
    print("7");
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
  if (std::string(method_name_ptr) == "add") {
    void *f = gen_function();
    if (f)
      *new_address_ptr = f;
    else
      *new_address_ptr = (void *)fortytwo;

  }

  if (class_signature_ptr != nullptr && method_name_ptr != nullptr)
  {
    std::ofstream myfile;
    myfile.open (LOG_PREFIX "bind_log.txt", std::ios::out | std::ios::app);
    myfile << "Bind: class:" << class_signature_ptr << " method: " << method_name_ptr << "\n";
    myfile.close();
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
