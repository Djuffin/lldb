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

void print(const char *str);