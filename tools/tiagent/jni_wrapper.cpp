#include "jni_wrapper.h"
#include "agent.h"
#include <mutex>
#include <iostream>
#include <vector>
#include <numeric>
#include <chrono>

using namespace llvm;

constexpr uint64_t kRefMask = 0;//(1ull << 62);

static jvmtiEnv *ti;
static jniNativeInterface *old_native_table;

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

struct RefWrapper {
  const bool is_system;
  RefWrapper(void *ret_addr) : is_system(IsSystemFunction(ret_addr)) {}

  template <class T> T *unwrap_ref(T *ref) {
    if (is_system)
      return ref;
    return (T *)unwrap_raw_ref((void *)ref);
  }

  template <class T> T *wrap_ref(T *ref) {
    if (is_system)
      return ref;
    return (T *)wrap_raw_ref((void *)ref);
  }
};

void enter_user_native_code(JNIEnv *env) {}

void leave_user_native_code(JNIEnv *env) {}

const MethodSignature *GetMethodSignature(JNIEnv *env, jmethodID methodID) {
  if (methodID == nullptr) return nullptr;
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> lock(g_mutex);
  static DenseMap<void *, MethodSignature *> g_id_to_signature(1000);
  void *voidMethodID = (void *)methodID;
  MethodSignature *sig_ptr = g_id_to_signature.lookup(voidMethodID);
  if (sig_ptr == nullptr) {
    char *method_name_ptr = nullptr;
    char *method_signature_ptr = nullptr;
    char *class_signature_ptr = nullptr;
    jclass declaring_class = nullptr;
    jvmtiError error = ti->GetMethodName(methodID, &method_name_ptr,
                                         &method_signature_ptr, nullptr);

    ASSERT(error == JNI_OK);
    error = ti->GetMethodDeclaringClass(methodID, &declaring_class);
    ASSERT(error == JNI_OK);
    error = ti->GetClassSignature(declaring_class, &class_signature_ptr,
                                  nullptr);
    ASSERT(error == JNI_OK);
    old_native_table->DeleteLocalRef(env, declaring_class);

    auto signature = ParseJavaSignature(method_signature_ptr);
    //ti->Deallocate((unsigned char *)method_name_ptr);
    //ti->Deallocate((unsigned char *)method_signature_ptr);
    //ti->Deallocate((unsigned char *)class_signature_ptr);
    if (signature) {
      sig_ptr = new MethodSignature(signature.getValue());
      sig_ptr->method_name = method_name_ptr;
      sig_ptr->method_signature = method_signature_ptr;
      sig_ptr->class_signature = class_signature_ptr;
      g_id_to_signature.insert({voidMethodID, sig_ptr});
    }
  }
  return sig_ptr;
}

SmallVector<jvalue, 5> UnwrapAllArguments(JNIEnv *env, RefWrapper &RW,
                                          jmethodID methodID, va_list args) {
  const MethodSignature *signature = GetMethodSignature(env, methodID);
  ASSERT(signature != nullptr);
  SmallVector<jvalue, 5> result;
  result.reserve(signature->arguments.size());
  for (auto type : signature->arguments) {
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
      value.l = RW.unwrap_ref(value.l);
      break;
    }
    result.push_back(value);
  }
  return result;
}

SmallVector<jvalue, 5> UnwrapAllArguments(JNIEnv *env, RefWrapper &RW,
                                    jmethodID methodID, const jvalue *args) {
  const MethodSignature *signature = GetMethodSignature(env, methodID);
  ASSERT(signature != nullptr);
  SmallVector<jvalue, 5> result;
  const jvalue *arg = args;
  result.reserve(signature->arguments.size());
  for (auto type : signature->arguments) {
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
      value.l = RW.unwrap_ref(value.l);
      break;
    }
    ++arg;
    result.push_back(value);
  }
  return result;
}

uint64_t get_milliseconds() {
  static auto app_start = std::chrono::system_clock::now();
  auto now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - app_start).count();
}

void print_jni_call(const char *name, bool is_system, int counter,
                    const MethodSignature *sig) {
  if (sig == nullptr) {
    print ("%" PRIu64 ",%d,%s,%d", get_milliseconds(),
                    (is_system ? 1 : 0), name, counter);
  } else {

    print ("%" PRIu64 ",%d,%s,%d,%s.%s(%s)", get_milliseconds(),
                    (is_system ? 1 : 0), name, counter,
                    sig->class_signature, sig->method_name,
                    sig->method_signature);
  }
}

#define JNI_HEADER(m_id)                                              \
  RefWrapper RW(__builtin_return_address(0));                                  \
  static int counter = 0; \
  { \
      static std::mutex g_mutex; \
      std::lock_guard<std::mutex> lock(g_mutex);\
      counter++;\
  }\
  const MethodSignature *method_sig = GetMethodSignature(env, m_id); \
  print_jni_call(__FUNCTION__, RW.is_system, counter, method_sig);

#define JNI_WRAPPER_HEADER()  JNI_HEADER(nullptr)

#define JNI_CALL_HEADER()  JNI_HEADER(methodID)

#define DEFINE_CALL_WITH_TYPE(RetType, MethodName)                             \
  JNICALL RetType W_##MethodName(JNIEnv *env, jobject obj, jmethodID methodID, \
                                 ...) {                                        \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    va_list args;                                                              \
    RetType result;                                                            \
    va_start(args, methodID);                                                  \
    result = old_native_table->MethodName##V(env, obj, methodID,               \
                                             args);           \
    va_end(args);                                                              \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##V(JNIEnv *env, jobject obj,                  \
                                    jmethodID methodID, va_list args) {        \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    RetType result;                                                            \
    result = old_native_table->MethodName##V(env, obj, methodID,               \
                                             args);           \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##A(JNIEnv *env, jobject obj,                  \
                                    jmethodID methodID, const jvalue *args) {  \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    RetType result;                                                            \
    result = old_native_table->MethodName##A(env, obj, methodID,               \
                                             args);           \
    return WRAP(result);                                                       \
  }

#define DEFINE_VOID_CALL(MethodName)                                           \
  JNICALL void W_##MethodName(JNIEnv *env, jobject obj, jmethodID methodID,    \
                              ...) {                                           \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    va_list args;                                                              \
    va_start(args, methodID);                                                  \
    old_native_table->MethodName##V(env, obj, methodID,                        \
                                    args);                    \
    va_end(args);                                                              \
  }                                                                            \
  JNICALL void W_##MethodName##V(JNIEnv *env, jobject obj, jmethodID methodID, \
                                 va_list args) {                               \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
                  \
    old_native_table->MethodName##V(env, obj, methodID,                        \
                                    args);                    \
  }                                                                            \
  JNICALL void W_##MethodName##A(JNIEnv *env, jobject obj, jmethodID methodID, \
                                 const jvalue *args) {                         \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    old_native_table->MethodName##A(env, obj, methodID,                        \
                                    args);                    \
  }

#define DEFINE_NONVIRT_CALL_WITH_TYPE(RetType, MethodName)                     \
  JNICALL RetType W_##MethodName(JNIEnv *env, jobject obj, jclass clazz,       \
                                 jmethodID methodID, ...) {                    \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    clazz = RW.unwrap_ref(clazz);                                              \
    va_list args;                                                              \
    RetType result;                                                            \
    va_start(args, methodID);                                                  \
    result = old_native_table->MethodName##V(env, obj, clazz, methodID,        \
                                             args);           \
    va_end(args);                                                              \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##V(JNIEnv *env, jobject obj, jclass clazz,    \
                                    jmethodID methodID, va_list args) {        \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    clazz = RW.unwrap_ref(clazz);                                              \
    RetType result;                                                            \
    result = old_native_table->MethodName##V(env, obj, clazz, methodID,        \
                                             args);           \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##A(JNIEnv *env, jobject obj, jclass clazz,    \
                                    jmethodID methodID, const jvalue *args) {  \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    clazz = RW.unwrap_ref(clazz);                                              \
    RetType result;                                                            \
    result = old_native_table->MethodName##A(env, obj, clazz, methodID,        \
                                             args);           \
    return WRAP(result);                                                       \
  }

#define DEFINE_NONVIRT_VOID_CALL(MethodName)                                   \
  JNICALL void W_##MethodName(JNIEnv *env, jobject obj, jclass clazz,          \
                              jmethodID methodID, ...) {                       \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    clazz = RW.unwrap_ref(clazz);                                              \
    va_list args;                                                              \
    va_start(args, methodID);                                                  \
    old_native_table->MethodName##V(env, obj, clazz, methodID,                 \
                                    args);                    \
    va_end(args);                                                              \
  }                                                                            \
  JNICALL void W_##MethodName##V(JNIEnv *env, jobject obj, jclass clazz,       \
                                 jmethodID methodID, va_list args) {           \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    clazz = RW.unwrap_ref(clazz);                                              \
    old_native_table->MethodName##V(env, obj, clazz, methodID,                 \
                                    args);                    \
  }                                                                            \
  JNICALL void W_##MethodName##A(JNIEnv *env, jobject obj, jclass clazz,       \
                                 jmethodID methodID, const jvalue *args) {     \
    JNI_CALL_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    clazz = RW.unwrap_ref(clazz);                                              \
    old_native_table->MethodName##A(env, obj, clazz, methodID,                 \
                                    args);                    \
  }

#define DEFINE_STATIC_CALL_WITH_TYPE(RetType, MethodName)                      \
  JNICALL RetType W_##MethodName(JNIEnv *env, jclass clazz,                    \
                                 jmethodID methodID, ...) {                    \
    JNI_CALL_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    va_list args;                                                              \
    RetType result;                                                            \
    va_start(args, methodID);                                                  \
    result = old_native_table->MethodName##V(env, clazz, methodID,             \
                                             args);           \
    va_end(args);                                                              \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##V(JNIEnv *env, jclass clazz,                 \
                                    jmethodID methodID, va_list args) {        \
    JNI_CALL_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    RetType result;                                                            \
    result = old_native_table->MethodName##V(env, clazz, methodID,             \
                                             args);           \
    return WRAP(result);                                                       \
  }                                                                            \
  JNICALL RetType W_##MethodName##A(JNIEnv *env, jclass clazz,                 \
                                    jmethodID methodID, const jvalue *args) {  \
    JNI_CALL_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    RetType result;                                                            \
    result = old_native_table->MethodName##A(env, clazz, methodID,             \
                                             args);           \
    return WRAP(result);                                                       \
  }

#define DEFINE_STATIC_VOID_CALL(MethodName)                                    \
  JNICALL void W_##MethodName(JNIEnv *env, jclass clazz, jmethodID methodID,   \
                              ...) {                                           \
    JNI_CALL_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    va_list args;                                                              \
    va_start(args, methodID);                                                  \
    old_native_table->MethodName##V(env, clazz, methodID,                      \
                                    args);                    \
    va_end(args);                                                              \
  }                                                                            \
  JNICALL void W_##MethodName##V(JNIEnv *env, jclass clazz,                    \
                                 jmethodID methodID, va_list args) {           \
    JNI_CALL_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    old_native_table->MethodName##V(env, clazz, methodID,                      \
                                    args);                    \
  }                                                                            \
  JNICALL void W_##MethodName##A(JNIEnv *env, jclass clazz,                    \
                                 jmethodID methodID, const jvalue *args) {     \
    JNI_CALL_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    old_native_table->MethodName##A(env, clazz, methodID,                      \
                                    args);                    \
  }

#define DEFINE_FIELD_GETTER(FieldType, TypeName)                               \
  FieldType W_Get##TypeName##Field(JNIEnv *env, jobject obj,                   \
                                   jfieldID fieldID) {                         \
    JNI_WRAPPER_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    FieldType result;                                                          \
    result = old_native_table->Get##TypeName##Field(env, obj, fieldID);        \
    return WRAP(result);                                                       \
  }

#define DEFINE_FIELD_SETTER(FieldType, TypeName)                               \
  void W_Set##TypeName##Field(JNIEnv *env, jobject obj, jfieldID fieldID,      \
                              FieldType val) {                                 \
    JNI_WRAPPER_HEADER()                                                       \
    obj = RW.unwrap_ref(obj);                                                  \
    val = UNWRAP(val);                                                         \
    old_native_table->Set##TypeName##Field(env, obj, fieldID, val);            \
  }

#define DEFINE_STATIC_FIELD_GETTER(FieldType, TypeName)                        \
  FieldType W_GetStatic##TypeName##Field(JNIEnv *env, jclass clazz,            \
                                         jfieldID fieldID) {                   \
    JNI_WRAPPER_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    FieldType result;                                                          \
    result =                                                                   \
        old_native_table->GetStatic##TypeName##Field(env, clazz, fieldID);     \
    return WRAP(result);                                                       \
  }

#define DEFINE_STATIC_FIELD_SETTER(FieldType, TypeName)                        \
  void W_SetStatic##TypeName##Field(JNIEnv *env, jclass clazz,                 \
                                    jfieldID fieldID, FieldType val) {         \
    JNI_WRAPPER_HEADER()                                                       \
    clazz = RW.unwrap_ref(clazz);                                              \
    val = UNWRAP(val);                                                         \
    old_native_table->SetStatic##TypeName##Field(env, clazz, fieldID, val);    \
  }

#define DEFINE_NEW_ARRAY(ArrayType, TypeName)                                  \
  ArrayType##Array W_New##TypeName##Array(JNIEnv *env, jsize len) {            \
    JNI_WRAPPER_HEADER()                                                       \
    ArrayType##Array result =                                                  \
        old_native_table->New##TypeName##Array(env, len);                      \
    return RW.wrap_ref(result);                                                \
  }

#define DEFINE_GET_ARRAY_ELEMENTS(ArrayType, TypeName)                         \
  ArrayType *W_Get##TypeName##ArrayElements(                                   \
      JNIEnv *env, ArrayType##Array array, jboolean *isCopy) {                 \
    JNI_WRAPPER_HEADER()                                                       \
    array = RW.unwrap_ref(array);                                              \
    return old_native_table->Get##TypeName##ArrayElements(env, array, isCopy); \
  }

#define DEFINE_RELEASE_ARRAY_ELEMENTS(ArrayType, TypeName)                     \
  void W_Release##TypeName##ArrayElements(JNIEnv *env, ArrayType##Array array, \
                                          ArrayType *elems, jint mode) {       \
    JNI_WRAPPER_HEADER()                                                       \
    array = RW.unwrap_ref(array);                                              \
    return old_native_table->Release##TypeName##ArrayElements(env, array,      \
                                                              elems, mode);    \
  }

#define DEFINE_GET_ARRAY_REGION(ArrayType, TypeName)                           \
  void W_Get##TypeName##ArrayRegion(JNIEnv *env, ArrayType##Array array,       \
                                    jsize start, jsize len, ArrayType *buf) {  \
    JNI_WRAPPER_HEADER()                                                       \
    array = RW.unwrap_ref(array);                                              \
    old_native_table->Get##TypeName##ArrayRegion(env, array, start, len, buf); \
  }

#define DEFINE_SET_ARRAY_REGION(ArrayType, TypeName)                           \
  void W_Set##TypeName##ArrayRegion(JNIEnv *env, ArrayType##Array array,       \
                                    jsize start, jsize len,                    \
                                    const ArrayType *buf) {                    \
    JNI_WRAPPER_HEADER()                                                       \
    array = RW.unwrap_ref(array);                                              \
    old_native_table->Set##TypeName##ArrayRegion(env, array, start, len, buf); \
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
#define WRAP(x) RW.wrap_ref(x)
#define UNWRAP(x) RW.unwrap_ref(x)
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
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);
  va_list args;
  va_start(args, methodID);

  jobject result =
      old_native_table->NewObjectV(env, clazz, methodID, args);
  va_end(args);
  return RW.wrap_ref(result);
}

JNICALL jobject W_NewObjectV(JNIEnv *env, jclass clazz, jmethodID methodID,
                             va_list args) {
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);

  jobject result =
      old_native_table->NewObjectV(env, clazz, methodID, args);
  return RW.wrap_ref(result);
}
JNICALL jobject W_NewObjectA(JNIEnv *env, jclass clazz, jmethodID methodID,
                             const jvalue *args) {
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);

  jobject result =
      old_native_table->NewObjectA(env, clazz, methodID, args);
  return RW.wrap_ref(result);
}

jclass W_DefineClass(JNIEnv *env, const char *name, jobject loader,
                     const jbyte *buf, jsize len) {
  JNI_WRAPPER_HEADER()
  loader = RW.unwrap_ref(loader);
  auto result = old_native_table->DefineClass(env, name, loader, buf, len);
  return RW.wrap_ref(result);
}

jclass W_FindClass(JNIEnv *env, const char *name) {
  JNI_WRAPPER_HEADER()
  auto result = old_native_table->FindClass(env, name);
  return RW.wrap_ref(result);
}

jmethodID W_FromReflectedMethod(JNIEnv *env, jobject method) {
  JNI_WRAPPER_HEADER()
  method = RW.unwrap_ref(method);
  return old_native_table->FromReflectedMethod(env, method);
}

jfieldID W_FromReflectedField(JNIEnv *env, jobject field) {
  JNI_WRAPPER_HEADER()
  field = RW.unwrap_ref(field);
  return old_native_table->FromReflectedField(env, field);
}

jobject W_ToReflectedMethod(JNIEnv *env, jclass cls, jmethodID methodID,
                            jboolean isStatic) {
  JNI_WRAPPER_HEADER()
  cls = RW.unwrap_ref(cls);
  auto result =
      old_native_table->ToReflectedMethod(env, cls, methodID, isStatic);
  result = RW.wrap_ref(result);
  return result;
}

jobject W_ToReflectedField(JNIEnv *env, jclass cls, jfieldID fieldID,
                           jboolean isStatic) {
  JNI_WRAPPER_HEADER()
  cls = RW.unwrap_ref(cls);
  auto result = old_native_table->ToReflectedField(env, cls, fieldID, isStatic);
  result = RW.wrap_ref(result);
  return result;
}

jmethodID W_GetMethodID(JNIEnv *env, jclass clazz, const char *name,
                        const char *sig) {
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->GetMethodID(env, clazz, name, sig);
}

jfieldID W_GetFieldID(JNIEnv *env, jclass clazz, const char *name,
                      const char *sig) {
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->GetFieldID(env, clazz, name, sig);
}

jmethodID W_GetStaticMethodID(JNIEnv *env, jclass clazz, const char *name,
                              const char *sig) {
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->GetStaticMethodID(env, clazz, name, sig);
}

jfieldID W_GetStaticFieldID(JNIEnv *env, jclass clazz, const char *name,
                            const char *sig) {
  JNI_WRAPPER_HEADER()
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->GetStaticFieldID(env, clazz, name, sig);
}

jclass W_GetSuperclass(JNIEnv *env, jclass sub) {
  JNI_WRAPPER_HEADER()
  sub = RW.unwrap_ref(sub);
  auto result = old_native_table->GetSuperclass(env, sub);
  result = RW.wrap_ref(result);
  return result;
}

jboolean W_IsAssignableFrom(JNIEnv *env, jclass sub, jclass sup) {
  JNI_WRAPPER_HEADER()
  sub = RW.unwrap_ref(sub);
  sup = RW.unwrap_ref(sup);
  return old_native_table->IsAssignableFrom(env, sub, sup);
}

jboolean W_IsSameObject(JNIEnv *env, jobject obj1, jobject obj2) {
  JNI_WRAPPER_HEADER();
  obj1 = RW.unwrap_ref(obj1);
  obj2 = RW.unwrap_ref(obj2);
  return old_native_table->IsSameObject(env, obj1, obj2);
}

jobject W_AllocObject(JNIEnv *env, jclass clazz) {
  JNI_WRAPPER_HEADER();
  clazz = RW.unwrap_ref(clazz);
  auto result = old_native_table->AllocObject(env, clazz);
  return RW.wrap_ref(result);
}

jclass W_GetObjectClass(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  auto result = old_native_table->GetObjectClass(env, obj);
  return RW.wrap_ref(result);
}

jboolean W_IsInstanceOf(JNIEnv *env, jobject obj, jclass clazz) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->IsInstanceOf(env, obj, clazz);
}

jobjectArray W_NewObjectArray(JNIEnv *env, jsize len, jclass clazz,
                              jobject init) {
  JNI_WRAPPER_HEADER();
  clazz = RW.unwrap_ref(clazz);
  init = RW.unwrap_ref(init);
  auto result = old_native_table->NewObjectArray(env, len, clazz, init);
  return RW.wrap_ref(result);
}

jobject W_GetObjectArrayElement(JNIEnv *env, jobjectArray array, jsize index) {
  JNI_WRAPPER_HEADER();
  array = RW.unwrap_ref(array);
  auto result = old_native_table->GetObjectArrayElement(env, array, index);
  return RW.wrap_ref(result);
}

void W_SetObjectArrayElement(JNIEnv *env, jobjectArray array, jsize index,
                             jobject val) {
  JNI_WRAPPER_HEADER();
  array = RW.unwrap_ref(array);
  val = RW.unwrap_ref(val);
  old_native_table->SetObjectArrayElement(env, array, index, val);
}

jint W_Throw(JNIEnv *env, jthrowable obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  return old_native_table->Throw(env, obj);
}

jint W_ThrowNew(JNIEnv *env, jclass clazz, const char *msg) {
  JNI_WRAPPER_HEADER();
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->ThrowNew(env, clazz, msg);
}

jthrowable W_ExceptionOccurred(JNIEnv *env) {
  JNI_WRAPPER_HEADER();
  auto result = old_native_table->ExceptionOccurred(env);
  return result;
}

jint W_PushLocalFrame(JNIEnv *env, jint capacity) {
  JNI_WRAPPER_HEADER();
  return old_native_table->PushLocalFrame(env, capacity);
}

jobject W_PopLocalFrame(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  auto result = old_native_table->PopLocalFrame(env, obj);
  return RW.wrap_ref(result);
}
jint W_EnsureLocalCapacity(JNIEnv *env, jint capacity) {
  JNI_WRAPPER_HEADER();
  return old_native_table->EnsureLocalCapacity(env, capacity);
}
jobject W_NewGlobalRef(JNIEnv *env, jobject lobj) {
  JNI_WRAPPER_HEADER();
  lobj = RW.unwrap_ref(lobj);
  auto result = old_native_table->NewGlobalRef(env, lobj);
  return RW.wrap_ref(result);
}

void W_DeleteGlobalRef(JNIEnv *env, jobject gref) {
  JNI_WRAPPER_HEADER();
  gref = RW.unwrap_ref(gref);
  old_native_table->DeleteGlobalRef(env, gref);
}

void W_DeleteLocalRef(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  old_native_table->DeleteLocalRef(env, obj);
}

jobject W_NewLocalRef(JNIEnv *env, jobject ref) {
  JNI_WRAPPER_HEADER();
  ref = RW.unwrap_ref(ref);
  auto result = old_native_table->NewLocalRef(env, ref);
  return RW.wrap_ref(result);
}

jstring W_NewString(JNIEnv *env, const jchar *unicode, jsize len) {
  JNI_WRAPPER_HEADER();
  auto result = old_native_table->NewString(env, unicode, len);
  return RW.wrap_ref(result);
}
jsize W_GetStringLength(JNIEnv *env, jstring str) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  return old_native_table->GetStringLength(env, str);
}
const jchar *W_GetStringChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  return old_native_table->GetStringChars(env, str, isCopy);
}
void W_ReleaseStringChars(JNIEnv *env, jstring str, const jchar *chars) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  old_native_table->ReleaseStringChars(env, str, chars);
}

jstring W_NewStringUTF(JNIEnv *env, const char *utf) {
  JNI_WRAPPER_HEADER();
  auto result = old_native_table->NewStringUTF(env, utf);
  return RW.wrap_ref(result);
}
jsize W_GetStringUTFLength(JNIEnv *env, jstring str) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  return old_native_table->GetStringUTFLength(env, str);
}
const char *W_GetStringUTFChars(JNIEnv *env, jstring str, jboolean *isCopy) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  return old_native_table->GetStringUTFChars(env, str, isCopy);
}
void W_ReleaseStringUTFChars(JNIEnv *env, jstring str, const char *chars) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  old_native_table->ReleaseStringUTFChars(env, str, chars);
}

jsize W_GetArrayLength(JNIEnv *env, jarray array) {
  JNI_WRAPPER_HEADER();
  array = RW.unwrap_ref(array);
  return old_native_table->GetArrayLength(env, array);
}
jint W_RegisterNatives(JNIEnv *env, jclass clazz,
                       const JNINativeMethod *methods, jint nMethods) {
  JNI_WRAPPER_HEADER();
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->RegisterNatives(env, clazz, methods, nMethods);
}
jint W_UnregisterNatives(JNIEnv *env, jclass clazz) {
  JNI_WRAPPER_HEADER();
  clazz = RW.unwrap_ref(clazz);
  return old_native_table->UnregisterNatives(env, clazz);
}

jint W_MonitorEnter(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  return old_native_table->MonitorEnter(env, obj);
}
jint W_MonitorExit(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  return old_native_table->MonitorExit(env, obj);
}

void W_GetStringRegion(JNIEnv *env, jstring str, jsize start, jsize len,
                       jchar *buf) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  old_native_table->GetStringRegion(env, str, start, len, buf);
}
void W_GetStringUTFRegion(JNIEnv *env, jstring str, jsize start, jsize len,
                          char *buf) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  old_native_table->GetStringUTFRegion(env, str, start, len, buf);
}

void *W_GetPrimitiveArrayCritical(JNIEnv *env, jarray array, jboolean *isCopy) {
  JNI_WRAPPER_HEADER();
  array = RW.unwrap_ref(array);
  return old_native_table->GetPrimitiveArrayCritical(env, array, isCopy);
}
void W_ReleasePrimitiveArrayCritical(JNIEnv *env, jarray array, void *carray,
                                     jint mode) {
  JNI_WRAPPER_HEADER();
  array = RW.unwrap_ref(array);
  old_native_table->ReleasePrimitiveArrayCritical(env, array, carray, mode);
}

const jchar *W_GetStringCritical(JNIEnv *env, jstring str, jboolean *isCopy) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  return old_native_table->GetStringCritical(env, str, isCopy);
}
void W_ReleaseStringCritical(JNIEnv *env, jstring str, const jchar *cstring) {
  JNI_WRAPPER_HEADER();
  str = RW.unwrap_ref(str);
  old_native_table->ReleaseStringCritical(env, str, cstring);
}

jweak W_NewWeakGlobalRef(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  return old_native_table->NewWeakGlobalRef(env, obj);
}
void W_DeleteWeakGlobalRef(JNIEnv *env, jweak ref) {
  JNI_WRAPPER_HEADER();
  old_native_table->DeleteWeakGlobalRef(env, ref);
}

jobject W_NewDirectByteBuffer(JNIEnv *env, void *address, jlong capacity) {
  JNI_WRAPPER_HEADER();
  auto result = old_native_table->NewDirectByteBuffer(env, address, capacity);
  return RW.wrap_ref(result);
}
void *W_GetDirectBufferAddress(JNIEnv *env, jobject buf) {
  JNI_WRAPPER_HEADER();
  buf = RW.unwrap_ref(buf);
  return old_native_table->GetDirectBufferAddress(env, buf);
}
jlong W_GetDirectBufferCapacity(JNIEnv *env, jobject buf) {
  JNI_WRAPPER_HEADER();
  buf = RW.unwrap_ref(buf);
  return old_native_table->GetDirectBufferCapacity(env, buf);
}
jobjectRefType W_GetObjectRefType(JNIEnv *env, jobject obj) {
  JNI_WRAPPER_HEADER();
  obj = RW.unwrap_ref(obj);
  return old_native_table->GetObjectRefType(env, obj);
}
jint W_GetVersion(JNIEnv *env) {
  JNI_WRAPPER_HEADER();
  return old_native_table->GetVersion(env);
}
void W_ExceptionDescribe(JNIEnv *env) {
  JNI_WRAPPER_HEADER();
  old_native_table->ExceptionDescribe(env);
}
void W_ExceptionClear(JNIEnv *env) {
  JNI_WRAPPER_HEADER();
  old_native_table->ExceptionClear(env);
}
void W_FatalError(JNIEnv *env, const char *msg) {
  JNI_WRAPPER_HEADER();
  old_native_table->FatalError(env, msg);
}
jint W_GetJavaVM(JNIEnv *env, JavaVM **vm) {
  JNI_WRAPPER_HEADER();
  return old_native_table->GetJavaVM(env, vm);
}
jboolean W_ExceptionCheck(JNIEnv *env) {
  JNI_WRAPPER_HEADER();
  return old_native_table->ExceptionCheck(env);
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

  g_jniNativeInterface.ExceptionCheck = old_native_table->ExceptionCheck;
  g_jniNativeInterface.PopLocalFrame = old_native_table->PopLocalFrame;
  g_jniNativeInterface.PushLocalFrame = old_native_table->PushLocalFrame;
  g_jniNativeInterface.NewLocalRef = old_native_table->NewLocalRef;
  g_jniNativeInterface.DeleteLocalRef = old_native_table->DeleteLocalRef;
  g_jniNativeInterface.IsSameObject = old_native_table->IsSameObject;
  g_jniNativeInterface.ExceptionOccurred = old_native_table->ExceptionOccurred;
  return &g_jniNativeInterface;
}

jvmtiError RegisterNewJniTable(jvmtiEnv *tiEnv) {
  ti = tiEnv;
  if (ti == nullptr)
    return JVMTI_ERROR_INVALID_OBJECT;

  jvmtiError error = ti->GetJNIFunctionTable(&old_native_table);
  if (error != JNI_OK)
    return error;

  error = ti->SetJNIFunctionTable(GetOverriddenJniTableMethods());
  if (error != JNI_OK)
    return error;

  return JVMTI_ERROR_NONE;
}
