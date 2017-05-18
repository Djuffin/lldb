#include "common.h"

using namespace llvm;


FunctionType *ConvertSignatureToFunctionType(const MethodSignature& signature,
                                 llvm::LLVMContext &context) {
  auto convert_type = [&context] (JavaType type) -> Type * {
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

  std::vector<Type*> args;
  for (auto type : signature.arguments) {
    args.push_back(convert_type(type));
  }

  return FunctionType::get(convert_type(signature.return_type), args, false);
}

llvm::Optional<MethodSignature> ParseJavaSignature(const char *str,
                                                   int extraPtrArgs) {
    if (str == nullptr) return None;
    const char* ptr = str;
    std::function<Optional<JavaType>()> consumeType =
      [&ptr, &consumeType]()->Optional<JavaType> {
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
          while (*ptr && *ptr != ';') ptr++;
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
    if (*ptr != '(') return None;
    ptr++;
    for (;extraPtrArgs > 0; --extraPtrArgs) {
      result.arguments.push_back(JavaType::jobject);
    }

    while (*ptr && *ptr != ')') {
      auto type = consumeType();
      if (!type) return None;
      result.arguments.push_back(type.getValue());
    }
    if (*ptr != ')') return None;
    ptr++;
    Optional<JavaType> returnType = consumeType();
    if (!returnType || *ptr) return None;

    result.return_type = returnType.getValue();
    return result;
};
