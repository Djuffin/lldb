#include "llvm/ADT/IntervalMap.h"
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
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IRTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/LambdaResolver.h"
#include "llvm/ExecutionEngine/Orc/ObjectTransformLayer.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/RuntimeDyld.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"

#include "jvmti.h"
#include <fcntl.h>
#include <fstream>
#include <jni.h>
#include <mutex>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "agent.h"
#include "jni_wrapper.h"

using namespace llvm;
using namespace llvm::orc;


struct ModuleSO {
  std::string name;
  uint64_t start_address = 0;
  uint64_t end_address = 0;
  bool is_system = false;
};

std::vector<ModuleSO> ReadProcessModules()
{
// /proc/self/map usually looks like this:
// <address>      <perms> <offset> <dev> <inode>     <pathname>
//
// 7e0d0d8000-7e0d0d9000 ---p 00000000 00:00 0
// 7e0d0d9000-7e0d0da000 r--p 00015000 fd:00 2231    /system/lib64/libselinux.so
// 7e0d0da000-7e0d0db000 rw-p 00016000 fd:00 2231    /system/lib64/libselinux.so
// 7e0d0db000-7e0d0dc000 rw-p 00000000 00:00 0       [anon:.bss]
// 7e0d12b000-7e0d13b000 r-xp 00000000 fd:00 2090    /system/lib64/libcutils.so
// 7e0d13b000-7e0d13d000 r--p 0000f000 fd:00 2090    /system/lib64/libcutils.so
// 7e0d13d000-7e0d13e000 rw-p 00011000 fd:00 2090    /system/lib64/libcutils.so
// 7e0d172000-7e0d174000 r-xp 00000000 fd:00 2096    /system/lib64/libdl.so
// 7e0d174000-7e0d175000 r--p 00001000 fd:00 2096    /system/lib64/libdl.so
// 7e0d175000-7e0d176000 rw-p 00002000 fd:00 2096    /system/lib64/libdl.so
// 7e0d176000-7e0d177000 rw-p 00000000 00:00 0       [anon:.bss]
// 7e0d1a9000-7e0d26c000 r-xp 00000000 fd:00 2073    /system/lib64/libc.so
// 7e0d26c000-7e0d26d000 ---p 00000000 00:00 0
// 7e0d26d000-7e0d273000 r--p 000c3000 fd:00 2073    /system/lib64/libc.so

  std::vector<ModuleSO> result;
  FILE *maps_file = fopen("/proc/self/maps", "r");
  if (maps_file == nullptr)
    return result;
  char *line_buffer = nullptr;
  size_t line_length = 0;
  ssize_t read;
  while ((read = getline(&line_buffer, &line_length, maps_file)) != -1) {
    void *start_address;
    void *end_address;
    void *offset;
    int dev1, dev2, inode;
    std::vector<char> module_name_buf(line_length, '\0');
    if (sscanf(line_buffer, "%p-%p %*[-rwxsp] %p %x:%x %d %s\n", &start_address,
               &end_address, &offset, &dev1, &dev2, &inode,
               module_name_buf.data()) == 7) {
      ModuleSO module;
      module.name.append(module_name_buf.data());
      module.start_address = (uint64_t)start_address;
      module.end_address = (uint64_t)end_address;
      result.push_back(module);
    }
  }
  free(line_buffer);
  fclose(maps_file);
  return result;
}

bool IsSystemFunction(void *fp) {
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> lock(g_mutex);

  typedef IntervalMap<uint64_t, ModuleSO *> PtrIntervalMap;
  static PtrIntervalMap::Allocator g_allocator_;
  static PtrIntervalMap g_ptr_range_to_module(g_allocator_);
  static std::vector<ModuleSO> g_modules;
  constexpr char kSystemDir[] = "/system/";
  constexpr char kTiAgentName[] = "/libtiagent.so";

  if (fp == nullptr) return true;
  uint64_t fp_address = (uint64_t)fp;

  ModuleSO *module = g_ptr_range_to_module.lookup(fp_address, nullptr);
  if (module == nullptr) {
    // Function not found, try rebuilding module map.
    g_ptr_range_to_module.clear();
    g_modules.clear();
    g_modules = ReadProcessModules();

    for (auto &m : g_modules) {
      if (m.name.find(kSystemDir) == 0 ||
          m.name.find(kTiAgentName) != std::string::npos) {
        m.is_system = true;
      }
      // +1 / -1 to make the intervals non-intersecting
      // IntervalMap can't work otherwise.
      g_ptr_range_to_module.insert(m.start_address + 1,
                                   m.end_address - 1,
                                   &m);
    }
  }
  module = g_ptr_range_to_module.lookup(fp_address, nullptr);
  if (module == nullptr) {
    print("function from unknown module");
    // Module not found, assume system module;
    return true;
  }
  return module->is_system;
}

SmallString<8> SignatureToShortString(const MethodSignature &sig) {
  auto get_type_char = [](JavaType type) -> char {
    switch (type) {
    case JavaType::jvoid:
      return 'V';
    case JavaType::jboolean:
      return 'Z';
    case JavaType::jbyte:
      return 'B';
    case JavaType::jchar:
      return 'C';
    case JavaType::jshort:
      return 'S';
    case JavaType::jint:
      return 'I';
    case JavaType::jlong:
      return 'J';
    case JavaType::jfloat:
      return 'F';
    case JavaType::jdouble:
      return 'D';
    case JavaType::jobject:
      return 'L';
    }
    return 0;
  };

  SmallString<8> result;
  result.append(1, get_type_char(sig.return_type));
  for (auto type : sig.arguments) {
    result.append(1, get_type_char(type));
  }
  result.append(1, '\0');
  return result;
}

FunctionType *ConvertSignatureToFunctionType(const MethodSignature &signature,
                                             llvm::LLVMContext &context) {
  auto convert_type = [&context](JavaType type) -> Type * {
    switch (type) {
    case JavaType::jvoid:
      return Type::getVoidTy(context);
    case JavaType::jboolean:
      return Type::getInt8Ty(context);
    case JavaType::jbyte:
      return Type::getInt8Ty(context);
    case JavaType::jchar:
      return Type::getInt16Ty(context);
    case JavaType::jshort:
      return Type::getInt16Ty(context);
    case JavaType::jint:
      return Type::getInt32Ty(context);
    case JavaType::jlong:
      return Type::getInt64Ty(context);
    case JavaType::jfloat:
      return Type::getFloatTy(context);
    case JavaType::jdouble:
      return Type::getDoubleTy(context);
    case JavaType::jobject:
      return Type::getInt8Ty(context)->getPointerTo();
    }
    return nullptr;
  };

  std::vector<Type *> args;
  for (auto type : signature.arguments) {
    args.push_back(convert_type(type));
  }

  return FunctionType::get(convert_type(signature.return_type), args, false);
}

llvm::Optional<MethodSignature> ParseJavaSignature(const char *str,
                                                   int extraPtrArgs) {
  if (str == nullptr)
    return None;
  const char *ptr = str;
  std::function<Optional<JavaType>()> consumeType =
      [&ptr, &consumeType]() -> Optional<JavaType> {
    switch (*ptr) {
    case 'V':
      ptr++;
      return JavaType::jvoid;
    case 'Z':
      ptr++;
      return JavaType::jboolean;
    case 'B':
      ptr++;
      return JavaType::jbyte;
    case 'C':
      ptr++;
      return JavaType::jchar;
    case 'S':
      ptr++;
      return JavaType::jshort;
    case 'I':
      ptr++;
      return JavaType::jint;
    case 'J':
      ptr++;
      return JavaType::jlong;
    case 'F':
      ptr++;
      return JavaType::jfloat;
    case 'D':
      ptr++;
      return JavaType::jdouble;
    case 'L':
      while (*ptr && *ptr != ';')
        ptr++;
      if (*ptr == ';') {
        ptr++;
        return JavaType::jobject;
      } else {
        return None;
      }
    case '[': {
      ptr++;
      if (consumeType())
        return JavaType::jobject;
      else
        return None;
    }
    default:
      return None;
    };
  };

  MethodSignature result;
  if (*ptr != '(')
    return None;
  ptr++;
  for (; extraPtrArgs > 0; --extraPtrArgs) {
    result.arguments.push_back(JavaType::jobject);
  }

  while (*ptr && *ptr != ')') {
    auto type = consumeType();
    if (!type)
      return None;
    result.arguments.push_back(type.getValue());
  }
  if (*ptr != ')')
    return None;
  ptr++;
  Optional<JavaType> returnType = consumeType();
  if (!returnType || *ptr)
    return None;

  result.return_type = returnType.getValue();
  return result;
};

void print(const char *fmt, ...) {
  static std::mutex g_print_mutex;
  std::lock_guard<std::mutex> lock(g_print_mutex);

  FILE *f = fopen("/data/data/com.eugene.sum/agent_log.txt", "a+");
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  fprintf(f, "\n");
  fclose(f);
  va_end(args);
}

bool IsDebuggerPresent() {
  char buf[1024];
  bool debugger_present = false;

  int status_fd = open("/proc/self/status", O_RDONLY);
  if (status_fd == -1)
    return 0;

  int num_read = read(status_fd, buf, sizeof(buf));

  if (num_read > 0) {
    static const char TracerPid[] = "TracerPid:";
    char *tracer_pid;

    buf[num_read] = 0;
    tracer_pid = strstr(buf, TracerPid);
    if (tracer_pid)
      debugger_present = (atoi(tracer_pid + sizeof(TracerPid) - 1) != 0);
  }
  close(status_fd);

  return debugger_present;
}

typedef ArrayRef<uint8_t> Codeblock;

class SectionMemoryManagerWrapper : public SectionMemoryManager {
  SectionMemoryManager &SM;
  Codeblock &last_codeblock_ptr_;

public:
  SectionMemoryManagerWrapper(SectionMemoryManager &sm,
                              Codeblock &last_codeblock_ptr)
      : SM(sm), last_codeblock_ptr_(last_codeblock_ptr) {}

  SectionMemoryManagerWrapper(const SectionMemoryManagerWrapper &) = delete;
  void operator=(const SectionMemoryManagerWrapper &) = delete;
  ~SectionMemoryManagerWrapper() override {}

  uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID,
                               StringRef SectionName) override {
    auto result =
        SM.allocateCodeSection(Size, Alignment, SectionID, SectionName);
    last_codeblock_ptr_ = Codeblock(result, Size);
    return result;
  }

  uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                               unsigned SectionID, StringRef SectionName,
                               bool isReadOnly) override {
    return SM.allocateDataSection(Size, Alignment, SectionID, SectionName,
                                  isReadOnly);
  }

  bool finalizeMemory(std::string *ErrMsg = nullptr) override {
    return SM.finalizeMemory(ErrMsg);
  }

  void invalidateInstructionCache() override {
    SM.invalidateInstructionCache();
  }
};

class LlvmJit {
private:
  std::unique_ptr<TargetMachine> TM;
  const DataLayout DL;
  RTDyldObjectLinkingLayer ObjectLayer;
  IRCompileLayer<decltype(ObjectLayer), SimpleCompiler> CompileLayer;
  Codeblock last_codeblock_;
  SectionMemoryManager memory_manager_;

public:
  LlvmJit()
      : TM(EngineBuilder().selectTarget()), DL(TM->createDataLayout()),
        ObjectLayer([this]() {
            return std::make_shared<SectionMemoryManagerWrapper>(
              memory_manager_, last_codeblock_);
        }),
        CompileLayer(ObjectLayer, SimpleCompiler(*TM)) {}

  TargetMachine &getTargetMachine() { return *TM; }

  SectionMemoryManager& getSectionMemoryManager() { return memory_manager_; }
  Codeblock getLastCodeBlock() { return last_codeblock_; }

  using ModuleHandle = decltype(CompileLayer)::ModuleHandleT;

  ModuleHandle addModule(std::unique_ptr<Module> M) {
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

    // Add the set to the JIT with the resolver we created above and a newly
    // created SectionMemoryManager.
    return cantFail(CompileLayer.addModule(std::move(M),
                                           std::move(Resolver)));
  }

  JITSymbol findSymbol(const std::string &name) {
    return CompileLayer.findSymbol(name, true);
  }
};

static std::mutex g_codetree_mutex;
static IntervalMap<uint64_t, void *> *g_ret_pc_range_to_native_func;
static DenseMap<uint64_t, void *> *g_ret_pc_to_native_func;
static DenseMap<void *, char *> *g_func_to_name;

void *lookup_native_func() {
  std::lock_guard<std::mutex> lock(g_codetree_mutex);
  uint64_t ret_addr = (uint64_t)__builtin_return_address(0);
  void *native_func = g_ret_pc_to_native_func->lookup(ret_addr);
  if (native_func == nullptr) {
    native_func = g_ret_pc_range_to_native_func->lookup(ret_addr, nullptr);
    if (native_func != nullptr)
      g_ret_pc_to_native_func->insert({ret_addr, native_func});
  }
  if (char *name = g_func_to_name->lookup(native_func)) {
    print("called %s", name);
  }
  return native_func;
}

void register_native_func(const char *name, Codeblock block,
                          void *native_func) {
  std::lock_guard<std::mutex> lock(g_codetree_mutex);
  g_ret_pc_range_to_native_func->insert((uint64_t)block.data(),
                                        ((uint64_t)block.data()) + block.size(),
                                        native_func);
  g_func_to_name->insert({native_func, strdup(name)});
}

class Codegen {
public:
  Codegen() {
    g_ret_pc_range_to_native_func =
        new IntervalMap<uint64_t, void *>(allocator_);
    g_ret_pc_to_native_func = new DenseMap<uint64_t, void *>(1000);
    g_func_to_name = new DenseMap<void *, char *>(1000);

    llvm::sys::DynamicLibrary::LoadLibraryPermanently(nullptr);
    llvm::sys::DynamicLibrary::AddSymbol(
        "wrap_ref", reinterpret_cast<void *>(wrap_raw_ref));
    llvm::sys::DynamicLibrary::AddSymbol(
        "unwrap_ref", reinterpret_cast<void *>(unwrap_raw_ref));
    llvm::sys::DynamicLibrary::AddSymbol(
        "print", reinterpret_cast<void *>(print));
    llvm::sys::DynamicLibrary::AddSymbol(
        "lookup_native_func", reinterpret_cast<void *>(lookup_native_func));
    llvm::sys::DynamicLibrary::AddSymbol(
        "enter_user_native_code",
        reinterpret_cast<void *>(enter_user_native_code));
    llvm::sys::DynamicLibrary::AddSymbol(
        "leave_user_native_code",
        reinterpret_cast<void *>(leave_user_native_code));

    InitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    context_ = make_unique<LLVMContext>();
    jit_ = make_unique<LlvmJit>();
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
    auto ptr_to_ptr_func_type =
        FunctionType::get(Type::getInt8Ty(*context_)->getPointerTo(),
                          {Type::getInt8Ty(*context_)->getPointerTo()}, false);

    auto ptr_to_void_func_type =
        FunctionType::get(Type::getVoidTy(*context_),
                          {Type::getInt8Ty(*context_)->getPointerTo()}, false);

    Function *wrap_ref = llvm::Function::Create(
        ptr_to_ptr_func_type, Function::ExternalLinkage, "wrap_ref", M);

    Function *enter_user_native_code =
        llvm::Function::Create(ptr_to_void_func_type, Function::ExternalLinkage,
                               "enter_user_native_code", M);

    Function *leave_user_native_code =
        llvm::Function::Create(ptr_to_void_func_type, Function::ExternalLinkage,
                               "leave_user_native_code", M);

    Function *unwrap_ref = llvm::Function::Create(
        ptr_to_ptr_func_type, Function::ExternalLinkage, "unwrap_ref", M);

    Function *lookup_native_func = llvm::Function::Create(
        FunctionType::get(func_type->getPointerTo(), {}, false),
        Function::ExternalLinkage, "lookup_native_func", M);

    Function *F = cast<Function>(M->getOrInsertFunction(name, func_type));
    BasicBlock *BB = BasicBlock::Create(*context_, "", F);
    IRBuilder<> builder(BB);

    std::vector<Argument *> args;
    for (auto it = F->arg_begin(); it != F->arg_end(); ++it) {
      args.push_back(&*it);
    }

    std::vector<Value *> values;
    // Handle JNIEnv pointer separately
    Value *jni_env = args[0];
    values.push_back(jni_env);

    for (size_t i = 1; i < args.size(); ++i) {
      Argument *arg = args[i];
      if (arg->getType()->isPointerTy()) {
        values.push_back(builder.CreateCall(wrap_ref, {arg}));
      } else {
        values.push_back(arg);
      }
    }

    builder.CreateCall(enter_user_native_code, {jni_env});
    Value *func_value = builder.CreateCall(lookup_native_func, {});
    Value *ret = builder.CreateCall(func_value, values);
    if (func_type->getReturnType()->isVoidTy()) {
      builder.CreateCall(leave_user_native_code, {jni_env});
      builder.CreateRetVoid();
    } else if (func_type->getReturnType()->isPointerTy()) {
      ret = builder.CreateCall(unwrap_ref, {ret});
      builder.CreateCall(leave_user_native_code, {jni_env});
      builder.CreateRet(ret);
    } else {
      builder.CreateCall(leave_user_native_code, {jni_env});
      builder.CreateRet(ret);
    }

    jit_->addModule(std::move(module));
    auto func_symbol = jit_->findSymbol(name);
    auto maybe_address = func_symbol.getAddress();
    if (maybe_address) {
      Codeblock blk = jit_->getLastCodeBlock();
      signature_to_code_.insert({short_sig_str, blk});
      return reinterpret_cast<void*>(maybe_address.get());
    } else {
      return nullptr;
    }
  }

  void *get_transparent_wrapper(const char *name, char *signature,
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
    if (sym_address != nullptr &&
        sym_address != (void *)prototype_block.data()) {
      print("ERROR: non-0 offset function start name:%s signature:%s"
            " offset:%zd",
            name, signature,
            ((uint8_t *)sym_address) - prototype_block.data());
      return nullptr;
    }

    auto &memory_manager = jit_->getSectionMemoryManager();
    uint8_t *new_block_mem = memory_manager.allocateCodeSection(
        prototype_block.size(), 16, 1, ".text");

    memcpy(new_block_mem, prototype_block.data(), prototype_block.size());
    memory_manager.finalizeMemory();
    memory_manager.invalidateInstructionCache();
    Codeblock copy_block(new_block_mem, prototype_block.size());
    register_native_func(name, copy_block, func_ptr);

    return (void *)new_block_mem;
  }

private:
  std::unique_ptr<LLVMContext> context_;
  std::unique_ptr<LlvmJit> jit_;
  StringMap<Codeblock> signature_to_code_;
  std::string error_str;
  IntervalMap<uint64_t, void *>::Allocator allocator_;
};

void *gen_function(char *name, char *signature, void *func_ptr) {
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> lock(g_mutex);
  static Codegen codegen;
  void *result = codegen.get_transparent_wrapper(name, signature, func_ptr);
  if (result == nullptr) {
    print("codegen error func:%s, signature:%s", name, signature);
  } else {
    print("codegen OK func:%s, signature:%s", name, signature);
  }
  return result;
}

jvmtiEnv *CreateJvmtiEnv(JavaVM *vm) {
  jvmtiEnv *jvmti_env;
  jint result = vm->GetEnv((void **)&jvmti_env, JVMTI_VERSION_1_2);
  if (result != JNI_OK) {
    return nullptr;
  }
  return jvmti_env;
}

void JNICALL NativeMethodBind(jvmtiEnv *ti, JNIEnv *jni_env, jthread thread,
                              jmethodID method, void *address,
                              void **new_address_ptr) {
  char *method_name_ptr = nullptr;
  char *method_signature_ptr = nullptr;
  char *class_signature_ptr = nullptr;
  jclass declaring_class = nullptr;
  jvmtiError error = ti->GetMethodName(method, &method_name_ptr,
                                       &method_signature_ptr, nullptr);
  error = ti->GetMethodDeclaringClass(method, &declaring_class);
  if (error != JNI_OK)
    return;
  error = ti->GetClassSignature(declaring_class, &class_signature_ptr, nullptr);
  if (error != JNI_OK)
    return;
  {
    bool blacklisted = true; //IsSystemFunction(address);
    if (!blacklisted) {
      void *f = gen_function(method_name_ptr, method_signature_ptr, address);
      if (f) {
        *new_address_ptr = f;
      }
    }
    print("NativeMethodBind class: %s method: %s",
      class_signature_ptr, method_name_ptr);
  }

  jni_env->DeleteLocalRef(declaring_class);
  ti->Deallocate((unsigned char *)method_name_ptr);
  ti->Deallocate((unsigned char *)method_signature_ptr);
  ti->Deallocate((unsigned char *)class_signature_ptr);
}

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *vm, char *options, void *reserved) {
  return Agent_OnAttach(vm, options, reserved);
}

JNIEXPORT jint JNICALL Agent_OnAttach(JavaVM *vm, char *options,
                                      void *reserved) {
  jvmtiError error;

  jvmtiEnv *ti = CreateJvmtiEnv(vm);
  if (ti == nullptr)
    return 1;

  // Hook up event callbacks
  // jvmtiEventCallbacks callbacks = {};
  // callbacks.NativeMethodBind = &NativeMethodBind;
  // error = ti->SetEventCallbacks(&callbacks, sizeof(callbacks));
  // if (error != JNI_OK)
  //   return 1;

  jvmtiCapabilities caps;
  error = ti->GetPotentialCapabilities(&caps);
  if (error != JNI_OK)
    return 1;

  error = ti->AddCapabilities(&caps);
  if (error != JNI_OK)
    return 1;

  // error = ti->SetEventNotificationMode(JVMTI_ENABLE,
  //                                      JVMTI_EVENT_NATIVE_METHOD_BIND, nullptr);
  if (error != JNI_OK)
    return 1;

  // while (!IsDebuggerPresent()) {
  //   sleep(0);
  // }

  error = RegisterNewJniTable(ti);
  if (error != JNI_OK)
    return 1;

  print("Agent initialized!");
  return 0;
}
