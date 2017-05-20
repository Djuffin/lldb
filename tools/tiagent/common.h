#pragma once
#include "llvm/ADT/Optional.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"

enum class JavaType {
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
    std::vector<JavaType> arguments;
    JavaType return_type;
};

llvm::Optional<MethodSignature> ParseJavaSignature(const char *str,
                                                   int extraPtrArgs = 0);

llvm::FunctionType *ConvertSignatureToFunctionType(
                                 const MethodSignature& signature,
                                 llvm::LLVMContext &context);


template <class T>
void print(const T &x);

inline void print(const char *x) {
    print(std::string(x));
}