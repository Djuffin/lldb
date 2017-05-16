#pragma once
#include "jni.h"
#include "jvmti.h"

#include "common.h"

jobject unwrap_ref(jobject ref);
jobject wrap_ref(jobject ref);

jvmtiError RegisterNewJniTable(jvmtiEnv *ti);