// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
#include "sub_language.h"
#include <atomic>
#include <mutex>
namespace { std::mutex probeMutex; std::atomic<int> players(0); }
extern "C" int sub_language_probe_try_lock(void) { return probeMutex.try_lock(); }
extern "C" void sub_language_probe_unlock(void) { probeMutex.unlock(); }
extern "C" void sub_language_playback_active(int delta) { players.fetch_add(delta); }
extern "C" int sub_language_playback_count(void) { return players.load(); }
