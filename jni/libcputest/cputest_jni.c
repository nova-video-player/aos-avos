/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cpu-features.h>

#include <jni.h>

jboolean
Java_com_archos_medialib_CpuTest_nativeIsArm(JNIEnv *jenv)
{
    return android_getCpuFamily() == ANDROID_CPU_FAMILY_ARM;
}

jboolean
Java_com_archos_medialib_CpuTest_nativeIsX86(JNIEnv *jenv)
{
    return android_getCpuFamily() == ANDROID_CPU_FAMILY_X86;
}

jboolean
Java_com_archos_medialib_CpuTest_nativeHasNeon(JNIEnv *jenv)
{
    return android_getCpuFeatures() & ANDROID_CPU_ARM_FEATURE_NEON;
}

jboolean
Java_com_archos_medialib_CpuTest_nativeIsArmV7a(JNIEnv *jenv)
{
    return android_getCpuFeatures() & ANDROID_CPU_ARM_FEATURE_ARMv7;
}

jint JNI_OnLoad(JavaVM* vm, void* reserved)
{

    return JNI_VERSION_1_4;
}

void JNI_OnUnload(JavaVM* vm, void* reserved)
{
}
