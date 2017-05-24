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
  llvm::SmallVector<JavaType, 5> arguments;
  JavaType return_type;
};

llvm::SmallString<8> SignatureToShortString(const MethodSignature &sig);

llvm::Optional<MethodSignature> ParseJavaSignature(const char *str,
                                                   int extraPtrArgs = 0);

llvm::FunctionType *
ConvertSignatureToFunctionType(const MethodSignature &signature,
                               llvm::LLVMContext &context);

template <class T> void print(const T &x);

inline void print(const char *x) { print(std::string(x)); }

void *gen_function(char *name, char *signature, void *func_ptr);