#pragma once
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"

enum class JavaType : uint8_t {
  jvoid,
  jboolean,
  jbyte,
  jchar,
  jshort,
  jint,
  jlong,
  jfloat,
  jdouble,
  jobject
};

struct MethodSignature {
  llvm::SmallVector<JavaType, 8> arguments;
  JavaType return_type;
  char *method_name;
  char *method_signature;
  char *class_signature;
};

llvm::SmallString<8> SignatureToShortString(const MethodSignature &sig);

llvm::Optional<MethodSignature> ParseJavaSignature(const char *str,
                                                   int extraPtrArgs = 0);

llvm::FunctionType *
ConvertSignatureToFunctionType(const MethodSignature &signature,
                               llvm::LLVMContext &context);

bool IsSystemFunction(void *fp);

void print(const char *fmt, ...);

void *gen_function(char *name, char *signature, void *func_ptr);

#define ASSERT(x)                                                              \
  if (!(x)) {                                                                  \
    print("ASSERT FAIL");                                                      \
    print(#x);                                                                 \
    print(__FUNCTION__);                                                       \
  }