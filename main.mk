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

ifeq ($(MASTER_OS),ANDROID)
ifeq (YES,$(LIBAVOS))
CSRC += avos_common.c avos_mp.c avos_mp_video.c avos_mp_audio.c avos_mr.c android_config.c android_codecs.c
INCLUDES += -I$(LOCAL_PATH)/public
AVOS_SHARED_LIBS += -lavos_android
endif
DEFINES += -DCONFIG_ANDROID
endif

ifeq ($(TTY),AVOS)
DEFINES += -DCONFIG_TTY
CSRC += log_avos.c tty.c console.c
endif
ifeq ($(TTY),STDOUT)
CSRC += log_stdout.c
endif
ifeq ($(TTY),ANDROID)
CSRC += log_android.c
endif

ifeq ($(LIBYUV),ON)
LIBYUV_DIR := $(AVOS_DIR)/../libyuv
DEFINES += -DCONFIG_LIBYUV
endif
