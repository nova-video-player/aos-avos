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

CSRC += audio_interface.c audio_interface_null.c

ifeq ($(AUDIO_INTERFACE),OSS)
CSRC += audio_interface_oss.c mixerdev_oss.c
DEFINES += -DCONFIG_SPDIF
CSRC += audio_spdif.c

else ifeq ($(AUDIO_INTERFACE),ALSA)
CSRC += audio_interface_alsa.c
SHARED_LIBS += -lasound

else ifeq ($(AUDIO_INTERFACE),TINYALSA)
CSRC += audio_interface_tinyalsa.c
SHARED_LIBS += -ltinyalsa

else ifeq ($(AUDIO_INTERFACE),ANDROID)
CSRC += audio_interface_opensles.c
CSRC += audio_interface_audiotrack.c
CSRC += audio_interface_audiotrack_new.c
CSRC += audio_interface_audiotrack_java.c
DEFINES += -DCONFIG_SPDIF
CSRC += audio_spdif.c
endif

