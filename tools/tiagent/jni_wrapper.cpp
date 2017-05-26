#include "jni_wrapper.h"
#include "agent.h"

using namespace llvm;

static jvmtiEnv *ti;

constexpr uint64_t kRefMask = 0; //(1ull << 62);
constexpr uint64_t kMagicNumber = 0xBAADF00DBAADF00Dull;

const jniNativeInterface *GetOverriddenJniTableMethods();

void *unwrap_raw_ref(void *ref) {
  if (ref == nullptr)
    return ref;
  uint64_t x = (uint64_t)ref;
  x ^= kRefMask;
  return (void *)x;
}
void *wrap_raw_ref(void *ref) {
  if (ref == nullptr)
    return ref;
  uint64_t x = (uint64_t)ref;
  x ^= kRefMask;
  return (void *)x;
}

struct JNIEnvWrapper : JNIEnv {
  JNIEnv *real_env;
  JNIEnvWrapper *previous_wrapper;
  uint64_t magic_number;
};

thread_local JNIEnvWrapper *current_wrapper = nullptr;

JNIEnv *construct_jni_env(JNIEnv *env) {
  JNIEnvWrapper *result = new JNIEnvWrapper;
  result->functions = GetOverriddenJniTableMethods();
  result->real_env = env;
  result->previous_wrapper = current_wrapper;
  result->magic_number = kMagicNumber;
  current_wrapper = result;
  return result;
}

JNIEnvWrapper *extract_wrapper_env(JNIEnv *env) {
  auto result = static_cast<JNIEnvWrapper *>(env);
  ASSERT(result->magic_number == kMagicNumber);
  return result;
}

JNIEnv *destroy_jni_env(JNIEnv *env) {
  if (extract_wrapper_env(env) != current_wrapper) {
    ASSERT(extract_wrapper_env(env) == current_wrapper);
    return nullptr;
  }
  auto wrapper = current_wrapper;
  current_wrapper = wrapper->previous_wrapper;
  delete wrapper;
  return current_wrapper;
}

JNIEnv *extract_real_env(JNIEnv *env) {
  return extract_wrapper_env(env)->real_env;
}

std::vector<jvalue> UnwrapAllArguments(jmethodID methodID, va_list args) {
  char *method_name_ptr = nullptr;
  char *method_signature_ptr = nullptr;
  jvmtiError error = ti->GetMethodName(methodID, &method_name_ptr,
                                       &method_signature_ptr, nullptr);
  ASSERT(error == JNI_OK);
  auto signature = ParseJavaSignature(method_signature_ptr);
  ASSERT((bool)signature);
  ti->Deallocate((unsigned char *)method_name_ptr);
  ti->Deallocate((unsigned char *)method_signature_ptr);
  std::vector<jvalue> result;
  result.reserve(signature.getValue().arguments.size());
  for (auto type : signature.getValue().arguments) {
    jvalue value;
    switch (type) {
    case JavaType::jvoid:
      ASSERT(false);
      break;
    case JavaType::jboolean:
      value.z = (jboolean)va_arg(args, int);
      break;
    case JavaType::jbyte:
      value.b = (jbyte)va_arg(args, int);
      break;
    case JavaType::jchar:
      value.c = (jchar)va_arg(args, int);
      break;
    case JavaType::jshort:
      value.s = (jshort)va_arg(args, int);
      break;
    case JavaType::jint:
      value.i = (jint)va_arg(args, int);
      break;
    case JavaType::jlong:
      value.j = (jlong)va_arg(args, jlong);
      break;
    case JavaType::jfloat:
      value.f = (jfloat)va_arg(args, double);
      break;
    case JavaType::jdouble:
      value.d = (jdouble)va_arg(args, double);
      break;
    case JavaType::jobject:
      value.l = va_arg(args, jobject);
      value.l = unwrap_ref(value.l);
      break;
    }
    result.push_back(value);
  }
  return result;
}

std::vector<jvalue> UnwrapAllArguments(jmethodID methodID, const jvalue *args) {
  char *method_name_ptr = nullptr;
  char *method_signature_ptr = nullptr;
  jvmtiError error = ti->GetMethodName(methodID, &method_name_ptr,
                                       &method_signature_ptr, nullptr);
  ASSERT(error == JNI_OK);
  auto signature = ParseJavaSignature(method_signature_ptr);
  ASSERT((bool)signature);
  ti->Deallocate((unsigned char *)method_name_ptr);
  ti->Deallocate((unsigned char *)method_signature_ptr);
  std::vector<jvalue> result;
  result.reserve(signature.getValue().arguments.size());
  const jvalue *arg = args;
  for (auto type : signature.getValue().arguments) {
    jvalue value;
    switch (type) {
    case JavaType::jvoid:
      ASSERT(false);
      break;
    case JavaType::jboolean:
    case JavaType::jbyte:
    case JavaType::jchar:
    case JavaType::jshort:
    case JavaType::jint:
    case JavaType::jlong:
    case JavaType::jfloat:
    case JavaType::jdouble:
      value = *arg;
      break;
    case JavaType::jobject:
      value = *arg;
      value.l = unwrap_ref(value.l);
      break;
    }
    ++arg;
    result.push_back(value);
  }
  return result;
}

#define STATIC_PRINTER()                                                       \
  static int i = 0;                                                            \
  if (i++ < 100) {                                                             \
    print(__FUNCTION__);                                                       \
  }

#define DEFINE_CALL_WITH_TYPE(RetType, MethodName)                             \
  JNICALL RetType W_##MethodName(JNIEnv *env, jobject obj, jmethodID methodID, \
                                 ...) {                                        \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    va_list args;                                                              \
    RetType result;                                                            \
    va_start(args, methodID);                                                  \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, obj, methodID,                 \
                                           unwrapperdArgs.data());             \
    va_end(args);                                                              \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##V(JNIEnv *env, jobject obj,                  \
                                    jmethodID methodID, va_list args) {        \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    RetType result;                                                            \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, obj, methodID,                 \
                                           unwrapperdArgs.data());             \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##A(JNIEnv *env, jobject obj,                  \
                                    jmethodID methodID, const jvalue *args) {  \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    RetType result;                                                            \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, obj, methodID,                 \
                                           unwrapperdArgs.data());             \
    return WRAP(result);                                                       \
  }

#define DEFINE_VOID_CALL(MethodName)                                           \
  JNICALL void W_##MethodName(JNIEnv *env, jobject obj, jmethodID methodID,    \
                              ...) {                                           \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    va_list args;                                                              \
    va_start(args, methodID);                                                  \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, obj, methodID, unwrapperdArgs.data());  \
    va_end(args);                                                              \
  }                                                                            \
  JNICALL void W_##MethodName##V(JNIEnv *env, jobject obj, jmethodID methodID, \
                                 va_list args) {                               \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, obj, methodID, unwrapperdArgs.data());  \
  }                                                                            \
  JNICALL void W_##MethodName##A(JNIEnv *env, jobject obj, jmethodID methodID, \
                                 const jvalue *args) {                         \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, obj, methodID, unwrapperdArgs.data());  \
  }

#define DEFINE_NONVIRT_CALL_WITH_TYPE(RetType, MethodName)                     \
  JNICALL RetType W_##MethodName(JNIEnv *env, jobject obj, jclass clazz,       \
                                 jmethodID methodID, ...) {                    \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    clazz = unwrap_ref(clazz);                                                 \
    va_list args;                                                              \
    RetType result;                                                            \
    va_start(args, methodID);                                                  \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, obj, clazz, methodID,          \
                                           unwrapperdArgs.data());             \
    va_end(args);                                                              \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##V(JNIEnv *env, jobject obj, jclass clazz,    \
                                    jmethodID methodID, va_list args) {        \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    clazz = unwrap_ref(clazz);                                                 \
    RetType result;                                                            \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, obj, clazz, methodID,          \
                                           unwrapperdArgs.data());             \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##A(JNIEnv *env, jobject obj, jclass clazz,    \
                                    jmethodID methodID, const jvalue *args) {  \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    clazz = unwrap_ref(clazz);                                                 \
    RetType result;                                                            \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, obj, clazz, methodID,          \
                                           unwrapperdArgs.data());             \
    return WRAP(result);                                                       \
  }

#define DEFINE_NONVIRT_VOID_CALL(MethodName)                                   \
  JNICALL void W_##MethodName(JNIEnv *env, jobject obj, jclass clazz,          \
                              jmethodID methodID, ...) {                       \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    clazz = unwrap_ref(clazz);                                                 \
    va_list args;                                                              \
    va_start(args, methodID);                                                  \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, obj, clazz, methodID,                   \
                                  unwrapperdArgs.data());                      \
    va_end(args);                                                              \
  }                                                                            \
  JNICALL void W_##MethodName##V(JNIEnv *env, jobject obj, jclass clazz,       \
                                 jmethodID methodID, va_list args) {           \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    clazz = unwrap_ref(clazz);                                                 \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, obj, clazz, methodID,                   \
                                  unwrapperdArgs.data());                      \
  }                                                                            \
  JNICALL void W_##MethodName##A(JNIEnv *env, jobject obj, jclass clazz,       \
                                 jmethodID methodID, const jvalue *args) {     \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    clazz = unwrap_ref(clazz);                                                 \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, obj, clazz, methodID,                   \
                                  unwrapperdArgs.data());                      \
  }

#define DEFINE_STATIC_CALL_WITH_TYPE(RetType, MethodName)                      \
  JNICALL RetType W_##MethodName(JNIEnv *env, jclass clazz,                    \
                                 jmethodID methodID, ...) {                    \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    va_list args;                                                              \
    RetType result;                                                            \
    va_start(args, methodID);                                                  \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, clazz, methodID,               \
                                           unwrapperdArgs.data());             \
    va_end(args);                                                              \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##V(JNIEnv *env, jclass clazz,                 \
                                    jmethodID methodID, va_list args) {        \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    RetType result;                                                            \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, clazz, methodID,               \
                                           unwrapperdArgs.data());             \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##A(JNIEnv *env, jclass clazz,                 \
                                    jmethodID methodID, const jvalue *args) {  \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    RetType result;                                                            \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    result = env->functions->MethodName##A(env, clazz, methodID,               \
                                           unwrapperdArgs.data());             \
    return WRAP(result);                                                       \
  }

#define DEFINE_STATIC_VOID_CALL(MethodName)                                    \
  JNICALL void W_##MethodName(JNIEnv *env, jclass clazz, jmethodID methodID,   \
                              ...) {                                           \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    va_list args;                                                              \
    va_start(args, methodID);                                                  \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, clazz, methodID,                        \
                                  unwrapperdArgs.data());                      \
    va_end(args);                                                              \
  }                                                                            \
  JNICALL void W_##MethodName##V(JNIEnv *env, jclass clazz,                    \
                                 jmethodID methodID, va_list args) {           \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, clazz, methodID,                        \
                                  unwrapperdArgs.data());                      \
  }                                                                            \
  JNICALL void W_##MethodName##A(JNIEnv *env, jclass clazz,                    \
                                 jmethodID methodID, const jvalue *args) {     \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    auto unwrapperdArgs = UnwrapAllArguments(methodID, args);                  \
    env->functions->MethodName##A(env, clazz, methodID,                        \
                                  unwrapperdArgs.data());                      \
  }

#define DEFINE_FIELD_GETTER(FieldType, TypeName)                               \
  FieldType W_Get##TypeName##Field(JNIEnv *env, jobject obj,                   \
                                   jfieldID fieldID) {                         \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    FieldType result;                                                          \
    result = env->functions->Get##TypeName##Field(env, obj, fieldID);          \
    return WRAP(result);                                                       \
  }

#define DEFINE_FIELD_SETTER(FieldType, TypeName)                               \
  void W_Set##TypeName##Field(JNIEnv *env, jobject obj, jfieldID fieldID,      \
                              FieldType val) {                                 \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    obj = unwrap_ref(obj);                                                     \
    val = UNWRAP(val);                                                         \
    env->functions->Set##TypeName##Field(env, obj, fieldID, val);              \
  }

#define DEFINE_STATIC_FIELD_GETTER(FieldType, TypeName)                        \
  FieldType W_GetStatic##TypeName##Field(JNIEnv *env, jclass clazz,            \
                                         jfieldID fieldID) {                   \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    FieldType result;                                                          \
    result = env->functions->GetStatic##TypeName##Field(env, clazz, fieldID);  \
    return WRAP(result);                                                       \
  }

#define DEFINE_STATIC_FIELD_SETTER(FieldType, TypeName)                        \
  void W_SetStatic##TypeName##Field(JNIEnv *env, jclass clazz,                 \
                                    jfieldID fieldID, FieldType val) {         \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    clazz = unwrap_ref(clazz);                                                 \
    val = UNWRAP(val);                                                         \
    env->functions->SetStatic##TypeName##Field(env, clazz, fieldID, val);      \
  }

#define DEFINE_NEW_ARRAY(ArrayType, TypeName)                                  \
  ArrayType##Array W_New##TypeName##Array(JNIEnv *env, jsize len) {            \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    ArrayType##Array result = env->functions->New##TypeName##Array(env, len);  \
    return wrap_ref(result);                                                   \
  }

#define DEFINE_GET_ARRAY_ELEMENTS(ArrayType, TypeName)                         \
  ArrayType *W_Get##TypeName##ArrayElements(                                   \
      JNIEnv *env, ArrayType##Array array, jboolean *isCopy) {                 \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    array = unwrap_ref(array);                                                 \
    return env->functions->Get##TypeName##ArrayElements(env, array, isCopy);   \
  }

#define DEFINE_RELEASE_ARRAY_ELEMENTS(ArrayType, TypeName)                     \
  void W_Release##TypeName##ArrayElements(JNIEnv *env, ArrayType##Array array, \
                                          ArrayType *elems, jint mode) {       \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    array = unwrap_ref(array);                                                 \
    return env->functions->Release##TypeName##ArrayElements(env, array, elems, \
                                                            mode);             \
  }

#define DEFINE_GET_ARRAY_REGION(ArrayType, TypeName)                           \
  void W_Get##TypeName##ArrayRegion(JNIEnv *env, ArrayType##Array array,       \
                                    jsize start, jsize len, ArrayType *buf) {  \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    array = unwrap_ref(array);                                                 \
    env->functions->Get##TypeName##ArrayRegion(env, array, start, len, buf);   \
  }

#define DEFINE_SET_ARRAY_REGION(ArrayType, TypeName)                           \
  void W_Set##TypeName##ArrayRegion(JNIEnv *env, ArrayType##Array array,       \
                                    jsize start, jsize len,                    \
                                    const ArrayType *buf) {                    \
    STATIC_PRINTER()                                                           \
    env = extract_real_env(env);                                               \
    array = unwrap_ref(array);                                                 \
    env->functions->Set##TypeName##ArrayRegion(env, array, start, len, buf);   \
  }

#define CALL(type, name)                                                       \
  DEFINE_CALL_WITH_TYPE(type, Call##name##Method)                              \
  DEFINE_NONVIRT_CALL_WITH_TYPE(type, CallNonvirtual##name##Method)            \
  DEFINE_STATIC_CALL_WITH_TYPE(type, CallStatic##name##Method)                 \
  DEFINE_FIELD_GETTER(type, name)                                              \
  DEFINE_FIELD_SETTER(type, name)                                              \
  DEFINE_STATIC_FIELD_GETTER(type, name)                                       \
  DEFINE_STATIC_FIELD_SETTER(type, name)

#define WRAP(x) x
#define UNWRAP(x) x
#include "jni_types.inc"
#undef WRAP
#undef UNWRAP
#define WRAP(x) wrap_ref(x)
#define UNWRAP(x) unwrap_ref(x)
CALL(jobject, Object)
DEFINE_VOID_CALL(CallVoidMethod)
DEFINE_NONVIRT_VOID_CALL(CallNonvirtualVoidMethod)
DEFINE_STATIC_VOID_CALL(CallStaticVoidMethod)
#undef CALL
#undef WRAP
#undef UNWRAP

#define CALL(type, name)                                                       \
  DEFINE_NEW_ARRAY(type, name)                                                 \
  DEFINE_GET_ARRAY_ELEMENTS(type, name)                                        \
  DEFINE_RELEASE_ARRAY_ELEMENTS(type, name)                                    \
  DEFINE_GET_ARRAY_REGION(type, name)                                          \
  DEFINE_SET_ARRAY_REGION(type, name)
#include "jni_types.inc"
#undef CALL

JNICALL jobject W_NewObject(JNIEnv *env, jclass clazz, jmethodID methodID,
                            ...) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  va_list args;
  va_start(args, methodID);
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args);
  jobject result =
      env->functions->NewObjectA(env, clazz, methodID, unwrapperdArgs.data());
  va_end(args);
  return wrap_ref(result);
}

JNICALL jobject W_NewObjectV(JNIEnv *env, jclass clazz, jmethodID methodID,
                             va_list args) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args);
  jobject result =
      env->functions->NewObjectA(env, clazz, methodID, unwrapperdArgs.data());
  return wrap_ref(result);
}
JNICALL jobject W_NewObjectA(JNIEnv *env, jclass clazz, jmethodID methodID,
                             const jvalue *args) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args);
  jobject result =
      env->functions->NewObjectA(env, clazz, methodID, unwrapperdArgs.data());
  return wrap_ref(result);
}

jclass W_DefineClass(JNIEnv *env, const char *name, jobject loader,
                     const jbyte *buf, jsize len) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  loader = unwrap_ref(loader);
  auto result = env->functions->DefineClass(env, name, loader, buf, len);
  return wrap_ref(result);
}

jclass W_FindClass(JNIEnv *env, const char *name) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  auto result = env->functions->FindClass(env, name);
  return wrap_ref(result);
}

jmethodID W_FromReflectedMethod(JNIEnv *env, jobject method) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  method = unwrap_ref(method);
  return env->functions->FromReflectedMethod(env, method);
}

jfieldID W_FromReflectedField(JNIEnv *env, jobject field) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  field = unwrap_ref(field);
  return env->functions->FromReflectedField(env, field);
}

jobject W_ToReflectedMethod(JNIEnv *env, jclass cls, jmethodID methodID,
                            jboolean isStatic) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  cls = unwrap_ref(cls);
  auto result = env->functions->ToReflectedMethod(env, cls, methodID, isStatic);
  result = wrap_ref(result);
  return result;
}

jobject W_ToReflectedField(JNIEnv *env, jclass cls, jfieldID fieldID,
                           jboolean isStatic) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  cls = unwrap_ref(cls);
  auto result = env->functions->ToReflectedField(env, cls, fieldID, isStatic);
  result = wrap_ref(result);
  return result;
}

jmethodID W_GetMethodID(JNIEnv *env, jclass clazz, const char *name,
                        const char *sig) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  return env->functions->GetMethodID(env, clazz, name, sig);
}

jfieldID W_GetFieldID(JNIEnv *env, jclass clazz, const char *name,
                      const char *sig) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  return env->functions->GetFieldID(env, clazz, name, sig);
}

jmethodID W_GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name,
                              const char *sig) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  return env->functions->GetStaticMethodID(env, clazz, name, sig);
}

jfieldID W_GetStaticFieldID(JNIEnv *env, jclass clazz, const char *name,
                            const char *sig) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  return env->functions->GetStaticFieldID(env, clazz, name, sig);
}

jclass W_GetSuperclass(JNIEnv *env, jclass sub) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  sub = unwrap_ref(sub);
  auto result = env->functions->GetSuperclass(env, sub);
  result = wrap_ref(result);
  return result;
}

jboolean W_IsAssignableFrom(JNIEnv *env, jclass sub, jclass sup) {
  STATIC_PRINTER()
  env = extract_real_env(env);
  sub = unwrap_ref(sub);
  sup = unwrap_ref(sup);
  return env->functions->IsAssignableFrom(env, sub, sup);
}

jboolean W_IsSameObject(JNIEnv *env, jobject obj1, jobject obj2) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj1 = unwrap_ref(obj1);
  obj2 = unwrap_ref(obj2);
  return env->functions->IsSameObject(env, obj1, obj2);
}

jobject W_AllocObject(JNIEnv *env, jclass clazz) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  auto result = env->functions->AllocObject(env, clazz);
  return wrap_ref(result);
}

jclass W_GetObjectClass(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  auto result = env->functions->GetObjectClass(env, obj);
  return wrap_ref(result);
}

jboolean W_IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  clazz = unwrap_ref(clazz);
  return env->functions->IsInstanceOf(env, obj, clazz);
}

jobjectArray W_NewObjectArray(JNIEnv *env, jsize len, jclass clazz,
                              jobject init) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  init = unwrap_ref(init);
  auto result = env->functions->NewObjectArray(env, len, clazz, init);
  return wrap_ref(result);
}

jobject W_GetObjectArrayElement(JNIEnv *env, jobjectArray array, jsize index) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  array = unwrap_ref(array);
  auto result = env->functions->GetObjectArrayElement(env, array, index);
  return wrap_ref(result);
}

void W_SetObjectArrayElement(JNIEnv *env, jobjectArray array, jsize index,
                             jobject val) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  array = unwrap_ref(array);
  val = unwrap_ref(val);
  env->functions->SetObjectArrayElement(env, array, index, val);
}

jint W_Throw(JNIEnv *env, jthrowable obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  return env->functions->Throw(env, obj);
}

jint W_ThrowNew(JNIEnv *env, jclass clazz, const char *msg) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  return env->functions->ThrowNew(env, clazz, msg);
}

jthrowable W_ExceptionOccurred(JNIEnv *env) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  auto result = env->functions->ExceptionOccurred(env);
  return result;
}

jint W_PushLocalFrame(JNIEnv *env, jint capacity) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  return env->functions->PushLocalFrame(env, capacity);
}

jobject W_PopLocalFrame(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  auto result = env->functions->PopLocalFrame(env, obj);
  return wrap_ref(result);
}
jint W_EnsureLocalCapacity(JNIEnv *env, jint capacity) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  return env->functions->EnsureLocalCapacity(env, capacity);
}
jobject W_NewGlobalRef(JNIEnv *env, jobject lobj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  lobj = unwrap_ref(lobj);
  auto result = env->functions->NewGlobalRef(env, lobj);
  return wrap_ref(result);
}

void W_DeleteGlobalRef(JNIEnv *env, jobject gref) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  gref = unwrap_ref(gref);
  env->functions->DeleteGlobalRef(env, gref);
}

void W_DeleteLocalRef(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  env->functions->DeleteLocalRef(env, obj);
}

jobject W_NewLocalRef(JNIEnv *env, jobject ref) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  ref = unwrap_ref(ref);
  auto result = env->functions->NewLocalRef(env, ref);
  return wrap_ref(result);
}

jstring W_NewString(JNIEnv *env, const jchar *unicode, jsize len) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  auto result = env->functions->NewString(env, unicode, len);
  return wrap_ref(result);
}
jsize W_GetStringLength(JNIEnv *env, jstring str) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  return env->functions->GetStringLength(env, str);
}
const jchar *W_GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  return env->functions->GetStringChars(env, str, isCopy);
}
void W_ReleaseStringChars(JNIEnv *env, jstring str, const jchar *chars) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  env->functions->ReleaseStringChars(env, str, chars);
}

jstring W_NewStringUTF(JNIEnv *env, const char *utf) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  auto result = env->functions->NewStringUTF(env, utf);
  return wrap_ref(result);
}
jsize W_GetStringUTFLength(JNIEnv *env, jstring str) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  return env->functions->GetStringUTFLength(env, str);
}
const char *W_GetStringUTFChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  return env->functions->GetStringUTFChars(env, str, isCopy);
}
void W_ReleaseStringUTFChars(JNIEnv *env, jstring str, const char *chars) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  env->functions->ReleaseStringUTFChars(env, str, chars);
}

jsize W_GetArrayLength(JNIEnv *env, jarray array) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  array = unwrap_ref(array);
  return env->functions->GetArrayLength(env, array);
}
jint W_RegisterNatives(JNIEnv *env, jclass clazz,
                       const JNINativeMethod *methods, jint nMethods) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  std::vector<JNINativeMethod> new_methods(nMethods);
  for (int i = 0; i < nMethods; ++i) {
    new_methods[i].name = methods[i].name;
    new_methods[i].signature = methods[i].signature;
    // new_methods[i].fnPtr = gen_function(methods[i].name,
    //                                     methods[i].signature,
    //                                     methods[i].fnPtr);
    new_methods[i].fnPtr = methods[i].fnPtr;
  }
  return env->functions->RegisterNatives(env, clazz, new_methods.data(),
                                         nMethods);
}
jint W_UnregisterNatives(JNIEnv *env, jclass clazz) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  clazz = unwrap_ref(clazz);
  return env->functions->UnregisterNatives(env, clazz);
}

jint W_MonitorEnter(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  return env->functions->MonitorEnter(env, obj);
}
jint W_MonitorExit(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  return env->functions->MonitorExit(env, obj);
}

void W_GetStringRegion(JNIEnv *env, jstring str, jsize start, jsize len,
                       jchar *buf) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  env->functions->GetStringRegion(env, str, start, len, buf);
}
void W_GetStringUTFRegion(JNIEnv *env, jstring str, jsize start, jsize len,
                          char *buf) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  env->functions->GetStringUTFRegion(env, str, start, len, buf);
}

void *W_GetPrimitiveArrayCritical(JNIEnv *env, jarray array, jboolean *isCopy) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  array = unwrap_ref(array);
  return env->functions->GetPrimitiveArrayCritical(env, array, isCopy);
}
void W_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray array, void *carray,
                                     jint mode) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  array = unwrap_ref(array);
  env->functions->ReleasePrimitiveArrayCritical(env, array, carray, mode);
}

const jchar *W_GetStringCritical(JNIEnv *env, jstring str, jboolean *isCopy) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  return env->functions->GetStringCritical(env, str, isCopy);
}
void W_ReleaseStringCritical(JNIEnv *env, jstring str, const jchar *cstring) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  str = unwrap_ref(str);
  env->functions->ReleaseStringCritical(env, str, cstring);
}

jweak W_NewWeakGlobalRef(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  return env->functions->NewWeakGlobalRef(env, obj);
}
void W_DeleteWeakGlobalRef(JNIEnv *env, jweak ref) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  env->functions->DeleteWeakGlobalRef(env, ref);
}

jobject W_NewDirectByteBuffer(JNIEnv *env, void *address, jlong capacity) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  auto result = env->functions->NewDirectByteBuffer(env, address, capacity);
  return wrap_ref(result);
}
void *W_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  buf = unwrap_ref(buf);
  return env->functions->GetDirectBufferAddress(env, buf);
}
jlong W_GetDirectBufferCapacity(JNIEnv *env, jobject buf) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  buf = unwrap_ref(buf);
  return env->functions->GetDirectBufferCapacity(env, buf);
}
jobjectRefType W_GetObjectRefType(JNIEnv *env, jobject obj) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  obj = unwrap_ref(obj);
  return env->functions->GetObjectRefType(env, obj);
}
jint W_GetVersion(JNIEnv *env) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  return env->functions->GetVersion(env);
}
void W_ExceptionDescribe(JNIEnv *env) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  env->functions->ExceptionDescribe(env);
}
void W_ExceptionClear(JNIEnv *env) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  env->functions->ExceptionClear(env);
}
void W_FatalError(JNIEnv *env, const char *msg) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  env->functions->FatalError(env, msg);
}
jint W_GetJavaVM(JNIEnv *env, JavaVM **vm) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  return env->functions->GetJavaVM(env, vm);
}
jboolean W_ExceptionCheck(JNIEnv *env) {
  STATIC_PRINTER();
  env = extract_real_env(env);
  return env->functions->ExceptionCheck(env);
}

const jniNativeInterface *GetOverriddenJniTableMethods() {
  static jniNativeInterface g_jniNativeInterface = {
      nullptr, // reserved0.
      nullptr, // reserved1.
      nullptr, // reserved2.
      nullptr, // reserved3.
      W_GetVersion,
      W_DefineClass,
      W_FindClass,
      W_FromReflectedMethod,
      W_FromReflectedField,
      W_ToReflectedMethod,
      W_GetSuperclass,
      W_IsAssignableFrom,
      W_ToReflectedField,
      W_Throw,
      W_ThrowNew,
      W_ExceptionOccurred,
      W_ExceptionDescribe,
      W_ExceptionClear,
      W_FatalError,
      W_PushLocalFrame,
      W_PopLocalFrame,
      W_NewGlobalRef,
      W_DeleteGlobalRef,
      W_DeleteLocalRef,
      W_IsSameObject,
      W_NewLocalRef,
      W_EnsureLocalCapacity,
      W_AllocObject,
      W_NewObject,
      W_NewObjectV,
      W_NewObjectA,
      W_GetObjectClass,
      W_IsInstanceOf,
      W_GetMethodID,
      W_CallObjectMethod,
      W_CallObjectMethodV,
      W_CallObjectMethodA,
      W_CallBooleanMethod,
      W_CallBooleanMethodV,
      W_CallBooleanMethodA,
      W_CallByteMethod,
      W_CallByteMethodV,
      W_CallByteMethodA,
      W_CallCharMethod,
      W_CallCharMethodV,
      W_CallCharMethodA,
      W_CallShortMethod,
      W_CallShortMethodV,
      W_CallShortMethodA,
      W_CallIntMethod,
      W_CallIntMethodV,
      W_CallIntMethodA,
      W_CallLongMethod,
      W_CallLongMethodV,
      W_CallLongMethodA,
      W_CallFloatMethod,
      W_CallFloatMethodV,
      W_CallFloatMethodA,
      W_CallDoubleMethod,
      W_CallDoubleMethodV,
      W_CallDoubleMethodA,
      W_CallVoidMethod,
      W_CallVoidMethodV,
      W_CallVoidMethodA,
      W_CallNonvirtualObjectMethod,
      W_CallNonvirtualObjectMethodV,
      W_CallNonvirtualObjectMethodA,
      W_CallNonvirtualBooleanMethod,
      W_CallNonvirtualBooleanMethodV,
      W_CallNonvirtualBooleanMethodA,
      W_CallNonvirtualByteMethod,
      W_CallNonvirtualByteMethodV,
      W_CallNonvirtualByteMethodA,
      W_CallNonvirtualCharMethod,
      W_CallNonvirtualCharMethodV,
      W_CallNonvirtualCharMethodA,
      W_CallNonvirtualShortMethod,
      W_CallNonvirtualShortMethodV,
      W_CallNonvirtualShortMethodA,
      W_CallNonvirtualIntMethod,
      W_CallNonvirtualIntMethodV,
      W_CallNonvirtualIntMethodA,
      W_CallNonvirtualLongMethod,
      W_CallNonvirtualLongMethodV,
      W_CallNonvirtualLongMethodA,
      W_CallNonvirtualFloatMethod,
      W_CallNonvirtualFloatMethodV,
      W_CallNonvirtualFloatMethodA,
      W_CallNonvirtualDoubleMethod,
      W_CallNonvirtualDoubleMethodV,
      W_CallNonvirtualDoubleMethodA,
      W_CallNonvirtualVoidMethod,
      W_CallNonvirtualVoidMethodV,
      W_CallNonvirtualVoidMethodA,
      W_GetFieldID,
      W_GetObjectField,
      W_GetBooleanField,
      W_GetByteField,
      W_GetCharField,
      W_GetShortField,
      W_GetIntField,
      W_GetLongField,
      W_GetFloatField,
      W_GetDoubleField,
      W_SetObjectField,
      W_SetBooleanField,
      W_SetByteField,
      W_SetCharField,
      W_SetShortField,
      W_SetIntField,
      W_SetLongField,
      W_SetFloatField,
      W_SetDoubleField,
      W_GetStaticMethodID,
      W_CallStaticObjectMethod,
      W_CallStaticObjectMethodV,
      W_CallStaticObjectMethodA,
      W_CallStaticBooleanMethod,
      W_CallStaticBooleanMethodV,
      W_CallStaticBooleanMethodA,
      W_CallStaticByteMethod,
      W_CallStaticByteMethodV,
      W_CallStaticByteMethodA,
      W_CallStaticCharMethod,
      W_CallStaticCharMethodV,
      W_CallStaticCharMethodA,
      W_CallStaticShortMethod,
      W_CallStaticShortMethodV,
      W_CallStaticShortMethodA,
      W_CallStaticIntMethod,
      W_CallStaticIntMethodV,
      W_CallStaticIntMethodA,
      W_CallStaticLongMethod,
      W_CallStaticLongMethodV,
      W_CallStaticLongMethodA,
      W_CallStaticFloatMethod,
      W_CallStaticFloatMethodV,
      W_CallStaticFloatMethodA,
      W_CallStaticDoubleMethod,
      W_CallStaticDoubleMethodV,
      W_CallStaticDoubleMethodA,
      W_CallStaticVoidMethod,
      W_CallStaticVoidMethodV,
      W_CallStaticVoidMethodA,
      W_GetStaticFieldID,
      W_GetStaticObjectField,
      W_GetStaticBooleanField,
      W_GetStaticByteField,
      W_GetStaticCharField,
      W_GetStaticShortField,
      W_GetStaticIntField,
      W_GetStaticLongField,
      W_GetStaticFloatField,
      W_GetStaticDoubleField,
      W_SetStaticObjectField,
      W_SetStaticBooleanField,
      W_SetStaticByteField,
      W_SetStaticCharField,
      W_SetStaticShortField,
      W_SetStaticIntField,
      W_SetStaticLongField,
      W_SetStaticFloatField,
      W_SetStaticDoubleField,
      W_NewString,
      W_GetStringLength,
      W_GetStringChars,
      W_ReleaseStringChars,
      W_NewStringUTF,
      W_GetStringUTFLength,
      W_GetStringUTFChars,
      W_ReleaseStringUTFChars,
      W_GetArrayLength,
      W_NewObjectArray,
      W_GetObjectArrayElement,
      W_SetObjectArrayElement,
      W_NewBooleanArray,
      W_NewByteArray,
      W_NewCharArray,
      W_NewShortArray,
      W_NewIntArray,
      W_NewLongArray,
      W_NewFloatArray,
      W_NewDoubleArray,
      W_GetBooleanArrayElements,
      W_GetByteArrayElements,
      W_GetCharArrayElements,
      W_GetShortArrayElements,
      W_GetIntArrayElements,
      W_GetLongArrayElements,
      W_GetFloatArrayElements,
      W_GetDoubleArrayElements,
      W_ReleaseBooleanArrayElements,
      W_ReleaseByteArrayElements,
      W_ReleaseCharArrayElements,
      W_ReleaseShortArrayElements,
      W_ReleaseIntArrayElements,
      W_ReleaseLongArrayElements,
      W_ReleaseFloatArrayElements,
      W_ReleaseDoubleArrayElements,
      W_GetBooleanArrayRegion,
      W_GetByteArrayRegion,
      W_GetCharArrayRegion,
      W_GetShortArrayRegion,
      W_GetIntArrayRegion,
      W_GetLongArrayRegion,
      W_GetFloatArrayRegion,
      W_GetDoubleArrayRegion,
      W_SetBooleanArrayRegion,
      W_SetByteArrayRegion,
      W_SetCharArrayRegion,
      W_SetShortArrayRegion,
      W_SetIntArrayRegion,
      W_SetLongArrayRegion,
      W_SetFloatArrayRegion,
      W_SetDoubleArrayRegion,
      W_RegisterNatives,
      W_UnregisterNatives,
      W_MonitorEnter,
      W_MonitorExit,
      W_GetJavaVM,
      W_GetStringRegion,
      W_GetStringUTFRegion,
      W_GetPrimitiveArrayCritical,
      W_ReleasePrimitiveArrayCritical,
      W_GetStringCritical,
      W_ReleaseStringCritical,
      W_NewWeakGlobalRef,
      W_DeleteWeakGlobalRef,
      W_ExceptionCheck,
      W_NewDirectByteBuffer,
      W_GetDirectBufferAddress,
      W_GetDirectBufferCapacity,
      W_GetObjectRefType};
  return &g_jniNativeInterface;
}

jvmtiError RegisterNewJniTable(jvmtiEnv *tiEnv) {
  ti = tiEnv;
  if (ti == nullptr)
    return JVMTI_ERROR_INVALID_OBJECT;

  return JVMTI_ERROR_NONE;
}
