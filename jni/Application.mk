# Copyright 2017 Archos SA
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

APP_ALLOW_MISSING_DEPS=true
APP_CFLAGS :=
ifeq (,$(NDK_APP_ABI))
APP_ABI := armeabi-v7a
else
APP_ABI := $(NDK_APP_ABI)
endif
APP_PLATFORM := android-21
APP_STL := c++_static

# Performance build: clang -O3 + ThinLTO for all C/C++ code.
# ARMv8-A baseline with CRC extensions on arm64 (mandatory for Android arm64).
# max-page-size=16384 keeps ELF LOAD segments 16KB aligned: required for
# Android 15+ 16KB-page devices (warnings on 4KB devices otherwise), harmless
# on 4KB-page hardware.
ifneq (1,$(ASAN))
APP_CFLAGS += -O3 -flto=thin
APP_LDFLAGS += -flto=thin -Wl,-z,max-page-size=16384
ifneq (,$(filter arm64-v8a,$(APP_ABI)))
APP_CFLAGS += -march=armv8-a+crc -mtune=generic
endif
endif

ifeq (1,$(ASAN))
APP_CFLAGS := -fsanitize=address -fno-omit-frame-pointer
APP_LDFLAGS := -fsanitize=address -Wl,-z,max-page-size=16384
endif
