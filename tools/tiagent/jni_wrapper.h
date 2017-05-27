#pragma once
#include "jni.h"
#include "jvmti.h"

void *unwrap_raw_ref(void *ref);
void *wrap_raw_ref(void *ref);

template <class T> inline T *unwrap_ref(T *ref) {
  return (T *)unwrap_raw_ref((void *)ref);
}

template <class T> inline T *wrap_ref(T *ref) {
  return (T *)wrap_raw_ref((void *)ref);
}

void enter_user_native_code(JNIEnv *env);

void leave_user_native_code(JNIEnv *env);

jvmtiError RegisterNewJniTable(jvmtiEnv *ti);