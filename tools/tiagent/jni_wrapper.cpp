#include "common.h"
#include "jni.h"
#include "jvmti.h"

using namespace llvm;

static jniNativeInterface* old_native_table;
static jniNativeInterface* new_native_table;
static jvmtiEnv* ti;

template<class T>
T *unwrap_ref(T *ref) {
  return ref;
}

jobject unwrap_ref(jobject ref) {
  return ref;
}

jobject wrap_ref(jobject ref) {
  return ref;
}

#define ASSERT(x) if (!(x)) { \
  print("ASSERT FAIL");\
  print(#x); \
  print(__FUNCTION__);\
}

std::vector<jvalue> UnwrapAllArguments(jmethodID methodID,
                                       va_list args) {
  char* method_name_ptr = nullptr;
  char* method_signature_ptr = nullptr;
  jvmtiError error = ti->GetMethodName(methodID, &method_name_ptr,
                                       &method_signature_ptr,
                                       nullptr);
  print("UnwrapAllArguments va_list");
  print(method_name_ptr);
  print(method_signature_ptr);

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

std::vector<jvalue> UnwrapAllArguments(jmethodID methodID,
                                       const jvalue *args) {
  char* method_name_ptr = nullptr;
  char* method_signature_ptr = nullptr;
  jvmtiError error = ti->GetMethodName(methodID, &method_name_ptr,
                                       &method_signature_ptr,
                                       nullptr);
  print("UnwrapAllArguments jvalue *");
  print(method_name_ptr);
  print(method_signature_ptr);

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


#define STATIC_PRINTER() \
  static int i = 0; if (i++ == 0) { print("called"); print(__FUNCTION__); }

JNICALL jobject W_NewObject(JNIEnv *env, jclass clazz, jmethodID methodID, ...) {
  STATIC_PRINTER()
  clazz = unwrap_ref(clazz);
  methodID = unwrap_ref(methodID);
  va_list args;
  va_start(args, methodID);
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args);
  jobject result = old_native_table->NewObjectA(env,
                      clazz, methodID, unwrapperdArgs.data());
  va_end(args);
  return wrap_ref(result);
}

JNICALL jobject W_NewObjectV(JNIEnv *env, jclass clazz, jmethodID methodID,
                   va_list args) {
  STATIC_PRINTER()
  clazz = unwrap_ref(clazz);
  methodID = unwrap_ref(methodID);
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args);
  jobject result = old_native_table->NewObjectA(env,
                    clazz, methodID, unwrapperdArgs.data());
  return wrap_ref(result);
}
JNICALL jobject W_NewObjectA(JNIEnv *env, jclass clazz, jmethodID methodID,
                   const jvalue *args) {
  STATIC_PRINTER()
  clazz = unwrap_ref(clazz);
  methodID = unwrap_ref(methodID);
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args);
  jobject result = old_native_table->NewObjectA(env,
                    clazz, methodID, unwrapperdArgs.data());
  return wrap_ref(result);
}


#define DEFINE_CALL_WITH_TYPE(RetType, MethodName) \
JNICALL RetType W_##MethodName(JNIEnv *env, jobject obj, jmethodID methodID, ...) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  methodID = unwrap_ref(methodID); \
  va_list args; \
  RetType result; \
  va_start(args,methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, obj, methodID, \
                                    unwrapperdArgs.data()); \
  va_end(args); \
  return WRAP(result); \
} \
JNICALL RetType W_##MethodName##V(JNIEnv *env, jobject obj, jmethodID methodID, \
                            va_list args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  methodID = unwrap_ref(methodID); \
  RetType result; \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, \
                    obj, methodID, unwrapperdArgs.data()); \
  return WRAP(result); \
} \
JNICALL RetType W_##MethodName##A(JNIEnv *env, jobject obj, jmethodID methodID, \
                            const jvalue * args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  methodID = unwrap_ref(methodID); \
  RetType result; \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, \
                    obj, methodID, unwrapperdArgs.data()); \
  return WRAP(result); \
}

#define DEFINE_VOID_CALL(MethodName) \
JNICALL void W_##MethodName(JNIEnv *env, jobject obj, jmethodID methodID, ...) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  methodID = unwrap_ref(methodID); \
  va_list args; \
  va_start(args,methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, obj, methodID, \
                                    unwrapperdArgs.data()); \
  va_end(args); \
} \
JNICALL void W_##MethodName##V(JNIEnv *env, jobject obj, jmethodID methodID, \
                            va_list args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  methodID = unwrap_ref(methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, \
                    obj, methodID, unwrapperdArgs.data()); \
} \
JNICALL void W_##MethodName##A(JNIEnv *env, jobject obj, jmethodID methodID, \
                            const jvalue * args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  methodID = unwrap_ref(methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, \
                    obj, methodID, unwrapperdArgs.data()); \
}


#define DEFINE_NONVIRT_CALL_WITH_TYPE(RetType, MethodName) \
JNICALL RetType W_##MethodName(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  va_list args; \
  RetType result; \
  va_start(args,methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, obj, clazz, methodID, \
                                    unwrapperdArgs.data()); \
  va_end(args); \
  return WRAP(result); \
} \
JNICALL RetType W_##MethodName##V(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, \
                            va_list args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  RetType result; \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, \
                    obj, clazz, methodID, unwrapperdArgs.data()); \
  return WRAP(result); \
} \
JNICALL RetType W_##MethodName##A(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, \
                            const jvalue * args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  RetType result; \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, \
                    obj, clazz, methodID, unwrapperdArgs.data()); \
  return WRAP(result); \
}

#define DEFINE_NONVIRT_VOID_CALL(MethodName) \
JNICALL void W_##MethodName(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, ...) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  va_list args; \
  va_start(args,methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, obj, clazz, methodID, \
                                    unwrapperdArgs.data()); \
  va_end(args); \
} \
JNICALL void W_##MethodName##V(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, \
                            va_list args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, \
                    obj, clazz, methodID, unwrapperdArgs.data()); \
} \
JNICALL void W_##MethodName##A(JNIEnv *env, jobject obj, jclass clazz, jmethodID methodID, \
                            const jvalue * args) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, \
                    obj, clazz, methodID, unwrapperdArgs.data()); \
}

#define DEFINE_STATIC_CALL_WITH_TYPE(RetType, MethodName) \
JNICALL RetType W_##MethodName(JNIEnv *env, jclass clazz, jmethodID methodID, ...) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  va_list args; \
  RetType result; \
  va_start(args,methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, clazz, methodID, \
                                    unwrapperdArgs.data()); \
  va_end(args); \
  return WRAP(result); \
} \
JNICALL RetType W_##MethodName##V(JNIEnv *env, jclass clazz, jmethodID methodID, \
                            va_list args) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  RetType result; \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, \
                    clazz, methodID, unwrapperdArgs.data()); \
  return WRAP(result); \
} \
JNICALL RetType W_##MethodName##A(JNIEnv *env, jclass clazz, jmethodID methodID, \
                            const jvalue * args) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  RetType result; \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  result = old_native_table->MethodName##A(env, \
                    clazz, methodID, unwrapperdArgs.data()); \
  return WRAP(result); \
}

#define DEFINE_STATIC_VOID_CALL(MethodName) \
JNICALL void W_##MethodName(JNIEnv *env, jclass clazz, jmethodID methodID, ...) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  va_list args; \
  va_start(args,methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, clazz, methodID, \
                                    unwrapperdArgs.data()); \
  va_end(args); \
} \
JNICALL void W_##MethodName##V(JNIEnv *env, jclass clazz, jmethodID methodID, \
                            va_list args) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, \
                    clazz, methodID, unwrapperdArgs.data()); \
} \
JNICALL void W_##MethodName##A(JNIEnv *env, jclass clazz, jmethodID methodID, \
                            const jvalue * args) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  methodID = unwrap_ref(methodID); \
  auto unwrapperdArgs = UnwrapAllArguments(methodID, args); \
  old_native_table->MethodName##A(env, \
                    clazz, methodID, unwrapperdArgs.data()); \
}

#define DEFINE_FIELD_GETTER(FieldType, TypeName) \
FieldType W_Get##TypeName##Field(JNIEnv *env, jobject obj, jfieldID fieldID) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  fieldID = unwrap_ref(fieldID); \
  FieldType result; \
  result = old_native_table->Get##TypeName##Field(env, \
                    obj, fieldID); \
  return WRAP(result); \
}

#define DEFINE_FIELD_SETTER(FieldType, TypeName) \
void W_Set##TypeName##Field(JNIEnv *env, jobject obj, jfieldID fieldID, FieldType val) { \
  STATIC_PRINTER() \
  obj = unwrap_ref(obj); \
  fieldID = unwrap_ref(fieldID); \
  val = UNWRAP(val); \
  old_native_table->Set##TypeName##Field(env, \
                    obj, fieldID, val); \
}

#define DEFINE_STATIC_FIELD_GETTER(FieldType, TypeName) \
FieldType W_GetStatic##TypeName##Field(JNIEnv *env, jclass clazz, jfieldID fieldID) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  fieldID = unwrap_ref(fieldID); \
  FieldType result; \
  result = old_native_table->GetStatic##TypeName##Field(env, \
                    clazz, fieldID); \
  return WRAP(result); \
}

#define DEFINE_STATIC_FIELD_SETTER(FieldType, TypeName) \
void W_SetStatic##TypeName##Field(JNIEnv *env, jclass clazz, jfieldID fieldID, FieldType val) { \
  STATIC_PRINTER() \
  clazz = unwrap_ref(clazz); \
  fieldID = unwrap_ref(fieldID); \
  val = UNWRAP(val); \
  old_native_table->SetStatic##TypeName##Field(env, \
                    clazz, fieldID, val); \
}

#define CALL(type, name) \
    DEFINE_CALL_WITH_TYPE(type, Call##name##Method) \
    DEFINE_NONVIRT_CALL_WITH_TYPE(type, CallNonvirtual##name##Method) \
    DEFINE_STATIC_CALL_WITH_TYPE(type, CallStatic##name##Method) \
    DEFINE_FIELD_GETTER(type, name) \
    DEFINE_FIELD_SETTER(type, name) \
    DEFINE_STATIC_FIELD_GETTER(type, name) \
    DEFINE_STATIC_FIELD_SETTER(type, name)

#define WRAP(x) x
#define UNWRAP(x) x
#include "jni_calls.inc"
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

void OverrideCallMethods(jniNativeInterface *jni_table) {
#define CALL(type, name) \
    jni_table->Call##name##Method = W_Call##name##Method; \
    jni_table->Call##name##MethodA = W_Call##name##MethodA; \
    jni_table->Call##name##MethodV = W_Call##name##MethodV; \
    jni_table->CallNonvirtual##name##Method = W_CallNonvirtual##name##Method; \
    jni_table->CallNonvirtual##name##MethodA = W_CallNonvirtual##name##MethodA; \
    jni_table->CallNonvirtual##name##MethodV = W_CallNonvirtual##name##MethodV; \
    jni_table->CallStatic##name##Method = W_CallStatic##name##Method; \
    jni_table->CallStatic##name##MethodA = W_CallStatic##name##MethodA; \
    jni_table->CallStatic##name##MethodV = W_CallStatic##name##MethodV;
#define CALL_OBJECT() CALL(jobject, Object)
#define CALL_VOID() CALL(0, Void)
#include "jni_calls.inc"
#undef CALL
#undef CALL_OBJECT
#undef CALL_VOID

#define CALL(type, name) \
    jni_table->Set##name##Field = W_Set##name##Field; \
    jni_table->Get##name##Field = W_Get##name##Field; \
    jni_table->SetStatic##name##Field = W_SetStatic##name##Field; \
    jni_table->GetStatic##name##Field = W_GetStatic##name##Field;
#define CALL_OBJECT()
#define CALL_VOID()
#include "jni_calls.inc"
#undef CALL
#undef CALL_OBJECT
#undef CALL_VOID

}

jvmtiError RegisterNewJniTable(jvmtiEnv *tiEnv) {
  ti = tiEnv;
  if (ti == nullptr) return JVMTI_ERROR_INVALID_OBJECT;

  jvmtiError error = ti->GetJNIFunctionTable(&old_native_table);
  if(error != JNI_OK) return error;

  error = ti->GetJNIFunctionTable(&new_native_table);
  if(error != JNI_OK) return error;

  new_native_table->NewObject = W_NewObject;
  new_native_table->NewObjectA = W_NewObjectA;
  new_native_table->NewObjectV = W_NewObjectV;
  OverrideCallMethods(new_native_table);
  error = ti->SetJNIFunctionTable(new_native_table);
  if(error != JNI_OK) return error;
  return JVMTI_ERROR_NONE;
}


/*
struct JNIEnv {
    const struct JNINativeInterface_ *functions;

    jint GetVersion() {
        return functions->GetVersion(this);
    }
    jclass DefineClass(const char *name, jobject loader, const jbyte *buf,
                       jsize len) {
        return functions->DefineClass(this, name, loader, buf, len);
    }
    jclass FindClass(const char *name) {
        return functions->FindClass(this, name);
    }
    jmethodID FromReflectedMethod(jobject method) {
        return functions->FromReflectedMethod(this,method);
    }
    jfieldID FromReflectedField(jobject field) {
        return functions->FromReflectedField(this,field);
    }

    jobject ToReflectedMethod(jclass cls, jmethodID methodID, jboolean isStatic) {
        return functions->ToReflectedMethod(this, cls, methodID, isStatic);
    }

    jclass GetSuperclass(jclass sub) {
        return functions->GetSuperclass(this, sub);
    }
    jboolean IsAssignableFrom(jclass sub, jclass sup) {
        return functions->IsAssignableFrom(this, sub, sup);
    }

    jobject ToReflectedField(jclass cls, jfieldID fieldID, jboolean isStatic) {
        return functions->ToReflectedField(this,cls,fieldID,isStatic);
    }

    jint Throw(jthrowable obj) {
        return functions->Throw(this, obj);
    }
    jint ThrowNew(jclass clazz, const char *msg) {
        return functions->ThrowNew(this, clazz, msg);
    }
    jthrowable ExceptionOccurred() {
        return functions->ExceptionOccurred(this);
    }
    void ExceptionDescribe() {
        functions->ExceptionDescribe(this);
    }
    void ExceptionClear() {
        functions->ExceptionClear(this);
    }
    void FatalError(const char *msg) {
        functions->FatalError(this, msg);
    }

    jint PushLocalFrame(jint capacity) {
        return functions->PushLocalFrame(this,capacity);
    }
    jobject PopLocalFrame(jobject result) {
        return functions->PopLocalFrame(this,result);
    }

    jobject NewGlobalRef(jobject lobj) {
        return functions->NewGlobalRef(this,lobj);
    }
    void DeleteGlobalRef(jobject gref) {
        functions->DeleteGlobalRef(this,gref);
    }
    void DeleteLocalRef(jobject obj) {
        functions->DeleteLocalRef(this, obj);
    }

    jboolean IsSameObject(jobject obj1, jobject obj2) {
        return functions->IsSameObject(this,obj1,obj2);
    }

    jobject NewLocalRef(jobject ref) {
        return functions->NewLocalRef(this,ref);
    }
    jint EnsureLocalCapacity(jint capacity) {
        return functions->EnsureLocalCapacity(this,capacity);
    }

    jobject AllocObject(jclass clazz) {
        return functions->AllocObject(this,clazz);
    }
    jobject NewObject(jclass clazz, jmethodID methodID, ...) {
        va_list args;
        jobject result;
        va_start(args, methodID);
        result = functions->NewObjectV(this,clazz,methodID,args);
        va_end(args);
        return result;
    }
    jobject NewObjectV(jclass clazz, jmethodID methodID,
                       va_list args) {
        return functions->NewObjectV(this,clazz,methodID,args);
    }
    jobject NewObjectA(jclass clazz, jmethodID methodID,
                       const jvalue *args) {
        return functions->NewObjectA(this,clazz,methodID,args);
    }

    jclass GetObjectClass(jobject obj) {
        return functions->GetObjectClass(this,obj);
    }
    jboolean IsInstanceOf(jobject obj, jclass clazz) {
        return functions->IsInstanceOf(this,obj,clazz);
    }

    jmethodID GetMethodID(jclass clazz, const char *name,
                          const char *sig) {
        return functions->GetMethodID(this,clazz,name,sig);
    }


    jfieldID GetFieldID(jclass clazz, const char *name,
                        const char *sig) {
        return functions->GetFieldID(this,clazz,name,sig);
    }

    jmethodID GetStaticMethodID(jclass clazz, const char *name,
                                const char *sig) {
        return functions->GetStaticMethodID(this,clazz,name,sig);
    }

    jfieldID GetStaticFieldID(jclass clazz, const char *name,
                              const char *sig) {
        return functions->GetStaticFieldID(this,clazz,name,sig);
    }

    jstring NewString(const jchar *unicode, jsize len) {
        return functions->NewString(this,unicode,len);
    }
    jsize GetStringLength(jstring str) {
        return functions->GetStringLength(this,str);
    }
    const jchar *GetStringChars(jstring str, jboolean *isCopy) {
        return functions->GetStringChars(this,str,isCopy);
    }
    void ReleaseStringChars(jstring str, const jchar *chars) {
        functions->ReleaseStringChars(this,str,chars);
    }

    jstring NewStringUTF(const char *utf) {
        return functions->NewStringUTF(this,utf);
    }
    jsize GetStringUTFLength(jstring str) {
        return functions->GetStringUTFLength(this,str);
    }
    const char* GetStringUTFChars(jstring str, jboolean *isCopy) {
        return functions->GetStringUTFChars(this,str,isCopy);
    }
    void ReleaseStringUTFChars(jstring str, const char* chars) {
        functions->ReleaseStringUTFChars(this,str,chars);
    }

    jsize GetArrayLength(jarray array) {
        return functions->GetArrayLength(this,array);
    }

    jobjectArray NewObjectArray(jsize len, jclass clazz,
                                jobject init) {
        return functions->NewObjectArray(this,len,clazz,init);
    }
    jobject GetObjectArrayElement(jobjectArray array, jsize index) {
        return functions->GetObjectArrayElement(this,array,index);
    }
    void SetObjectArrayElement(jobjectArray array, jsize index,
                               jobject val) {
        functions->SetObjectArrayElement(this,array,index,val);
    }

    jbooleanArray NewBooleanArray(jsize len) {
        return functions->NewBooleanArray(this,len);
    }
    jbyteArray NewByteArray(jsize len) {
        return functions->NewByteArray(this,len);
    }
    jcharArray NewCharArray(jsize len) {
        return functions->NewCharArray(this,len);
    }
    jshortArray NewShortArray(jsize len) {
        return functions->NewShortArray(this,len);
    }
    jintArray NewIntArray(jsize len) {
        return functions->NewIntArray(this,len);
    }
    jlongArray NewLongArray(jsize len) {
        return functions->NewLongArray(this,len);
    }
    jfloatArray NewFloatArray(jsize len) {
        return functions->NewFloatArray(this,len);
    }
    jdoubleArray NewDoubleArray(jsize len) {
        return functions->NewDoubleArray(this,len);
    }

    jboolean * GetBooleanArrayElements(jbooleanArray array, jboolean *isCopy) {
        return functions->GetBooleanArrayElements(this,array,isCopy);
    }
    jbyte * GetByteArrayElements(jbyteArray array, jboolean *isCopy) {
        return functions->GetByteArrayElements(this,array,isCopy);
    }
    jchar * GetCharArrayElements(jcharArray array, jboolean *isCopy) {
        return functions->GetCharArrayElements(this,array,isCopy);
    }
    jshort * GetShortArrayElements(jshortArray array, jboolean *isCopy) {
        return functions->GetShortArrayElements(this,array,isCopy);
    }
    jint * GetIntArrayElements(jintArray array, jboolean *isCopy) {
        return functions->GetIntArrayElements(this,array,isCopy);
    }
    jlong * GetLongArrayElements(jlongArray array, jboolean *isCopy) {
        return functions->GetLongArrayElements(this,array,isCopy);
    }
    jfloat * GetFloatArrayElements(jfloatArray array, jboolean *isCopy) {
        return functions->GetFloatArrayElements(this,array,isCopy);
    }
    jdouble * GetDoubleArrayElements(jdoubleArray array, jboolean *isCopy) {
        return functions->GetDoubleArrayElements(this,array,isCopy);
    }

    void ReleaseBooleanArrayElements(jbooleanArray array,
                                     jboolean *elems,
                                     jint mode) {
        functions->ReleaseBooleanArrayElements(this,array,elems,mode);
    }
    void ReleaseByteArrayElements(jbyteArray array,
                                  jbyte *elems,
                                  jint mode) {
        functions->ReleaseByteArrayElements(this,array,elems,mode);
    }
    void ReleaseCharArrayElements(jcharArray array,
                                  jchar *elems,
                                  jint mode) {
        functions->ReleaseCharArrayElements(this,array,elems,mode);
    }
    void ReleaseShortArrayElements(jshortArray array,
                                   jshort *elems,
                                   jint mode) {
        functions->ReleaseShortArrayElements(this,array,elems,mode);
    }
    void ReleaseIntArrayElements(jintArray array,
                                 jint *elems,
                                 jint mode) {
        functions->ReleaseIntArrayElements(this,array,elems,mode);
    }
    void ReleaseLongArrayElements(jlongArray array,
                                  jlong *elems,
                                  jint mode) {
        functions->ReleaseLongArrayElements(this,array,elems,mode);
    }
    void ReleaseFloatArrayElements(jfloatArray array,
                                   jfloat *elems,
                                   jint mode) {
        functions->ReleaseFloatArrayElements(this,array,elems,mode);
    }
    void ReleaseDoubleArrayElements(jdoubleArray array,
                                    jdouble *elems,
                                    jint mode) {
        functions->ReleaseDoubleArrayElements(this,array,elems,mode);
    }

    void GetBooleanArrayRegion(jbooleanArray array,
                               jsize start, jsize len, jboolean *buf) {
        functions->GetBooleanArrayRegion(this,array,start,len,buf);
    }
    void GetByteArrayRegion(jbyteArray array,
                            jsize start, jsize len, jbyte *buf) {
        functions->GetByteArrayRegion(this,array,start,len,buf);
    }
    void GetCharArrayRegion(jcharArray array,
                            jsize start, jsize len, jchar *buf) {
        functions->GetCharArrayRegion(this,array,start,len,buf);
    }
    void GetShortArrayRegion(jshortArray array,
                             jsize start, jsize len, jshort *buf) {
        functions->GetShortArrayRegion(this,array,start,len,buf);
    }
    void GetIntArrayRegion(jintArray array,
                           jsize start, jsize len, jint *buf) {
        functions->GetIntArrayRegion(this,array,start,len,buf);
    }
    void GetLongArrayRegion(jlongArray array,
                            jsize start, jsize len, jlong *buf) {
        functions->GetLongArrayRegion(this,array,start,len,buf);
    }
    void GetFloatArrayRegion(jfloatArray array,
                             jsize start, jsize len, jfloat *buf) {
        functions->GetFloatArrayRegion(this,array,start,len,buf);
    }
    void GetDoubleArrayRegion(jdoubleArray array,
                              jsize start, jsize len, jdouble *buf) {
        functions->GetDoubleArrayRegion(this,array,start,len,buf);
    }

    void SetBooleanArrayRegion(jbooleanArray array, jsize start, jsize len,
                               const jboolean *buf) {
        functions->SetBooleanArrayRegion(this,array,start,len,buf);
    }
    void SetByteArrayRegion(jbyteArray array, jsize start, jsize len,
                            const jbyte *buf) {
        functions->SetByteArrayRegion(this,array,start,len,buf);
    }
    void SetCharArrayRegion(jcharArray array, jsize start, jsize len,
                            const jchar *buf) {
        functions->SetCharArrayRegion(this,array,start,len,buf);
    }
    void SetShortArrayRegion(jshortArray array, jsize start, jsize len,
                             const jshort *buf) {
        functions->SetShortArrayRegion(this,array,start,len,buf);
    }
    void SetIntArrayRegion(jintArray array, jsize start, jsize len,
                           const jint *buf) {
        functions->SetIntArrayRegion(this,array,start,len,buf);
    }
    void SetLongArrayRegion(jlongArray array, jsize start, jsize len,
                            const jlong *buf) {
        functions->SetLongArrayRegion(this,array,start,len,buf);
    }
    void SetFloatArrayRegion(jfloatArray array, jsize start, jsize len,
                             const jfloat *buf) {
        functions->SetFloatArrayRegion(this,array,start,len,buf);
    }
    void SetDoubleArrayRegion(jdoubleArray array, jsize start, jsize len,
                              const jdouble *buf) {
        functions->SetDoubleArrayRegion(this,array,start,len,buf);
    }

    jint RegisterNatives(jclass clazz, const JNINativeMethod *methods,
                         jint nMethods) {
        return functions->RegisterNatives(this,clazz,methods,nMethods);
    }
    jint UnregisterNatives(jclass clazz) {
        return functions->UnregisterNatives(this,clazz);
    }

    jint MonitorEnter(jobject obj) {
        return functions->MonitorEnter(this,obj);
    }
    jint MonitorExit(jobject obj) {
        return functions->MonitorExit(this,obj);
    }

    jint GetJavaVM(JavaVM **vm) {
        return functions->GetJavaVM(this,vm);
    }

    void GetStringRegion(jstring str, jsize start, jsize len, jchar *buf) {
        functions->GetStringRegion(this,str,start,len,buf);
    }
    void GetStringUTFRegion(jstring str, jsize start, jsize len, char *buf) {
        functions->GetStringUTFRegion(this,str,start,len,buf);
    }

    void * GetPrimitiveArrayCritical(jarray array, jboolean *isCopy) {
        return functions->GetPrimitiveArrayCritical(this,array,isCopy);
    }
    void ReleasePrimitiveArrayCritical(jarray array, void *carray, jint mode) {
        functions->ReleasePrimitiveArrayCritical(this,array,carray,mode);
    }

    const jchar * GetStringCritical(jstring string, jboolean *isCopy) {
        return functions->GetStringCritical(this,string,isCopy);
    }
    void ReleaseStringCritical(jstring string, const jchar *cstring) {
        functions->ReleaseStringCritical(this,string,cstring);
    }

    jweak NewWeakGlobalRef(jobject obj) {
        return functions->NewWeakGlobalRef(this,obj);
    }
    void DeleteWeakGlobalRef(jweak ref) {
        functions->DeleteWeakGlobalRef(this,ref);
    }

    jboolean ExceptionCheck() {
        return functions->ExceptionCheck(this);
    }

    jobject NewDirectByteBuffer(void* address, jlong capacity) {
        return functions->NewDirectByteBuffer(this, address, capacity);
    }
    void* GetDirectBufferAddress(jobject buf) {
        return functions->GetDirectBufferAddress(this, buf);
    }
    jlong GetDirectBufferCapacity(jobject buf) {
        return functions->GetDirectBufferCapacity(this, buf);
    }
    jobjectRefType GetObjectRefType(jobject obj) {
        return functions->GetObjectRefType(this, obj);
    }

};
*/