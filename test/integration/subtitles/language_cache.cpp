// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
#include "sub_language.h"
#include <cassert>
#include <cstring>
#include <fstream>
#include <string>

int main(int argc, char **argv) {
    assert(argc == 3);
    sub_language_set_cache_path(argv[1]);
    const std::string key(64, 'a'), other(64, 'b');
    SUB_LANGUAGE_RESULT r{}; r.index = 12; r.stream = 14; r.format = 4; r.confidence = 94;
    std::strcpy(r.language, "fr");
    SUB_LANGUAGE_RESULT out[128]; int recent = 0;
    if (std::string(argv[2]) == "write") {
        sub_language_cache_write(key.c_str(), &r, 1, 1);
        return 0;
    }
    assert(sub_language_cache_read(key.c_str(), out, 128, &recent) == 1 && recent);
    assert(out[0].index == 12 && out[0].stream == 14 && out[0].confidence == 94 && std::string(out[0].language) == "fr");
    assert(!sub_language_cache_read(other.c_str(), out, 128, &recent) && !recent);
    r.index = 13; r.stream = 15;
    sub_language_cache_write(key.c_str(), &r, 1, 0);
    assert(sub_language_cache_read(key.c_str(), out, 128, &recent) == 2 && !recent);
    // Invalid results cannot replace previously accepted entries.
    r.confidence = 1;
    sub_language_cache_write(key.c_str(), &r, 1, 1);
    assert(sub_language_cache_read(key.c_str(), out, 128, &recent) == 2);
    assert(!sub_language_cache_read("../outside", out, 128, &recent));
    std::ofstream file(std::string(argv[1]) + "/" + key + ".lid");
    file << "nova-lid-8f3472cf-v2 0 1 0\n"; file.close();
    assert(!sub_language_cache_read(key.c_str(), out, 128, &recent) && !recent);
    file.open(std::string(argv[1]) + "/" + key + ".lid");
    file << "corrupt"; file.close();
    assert(!sub_language_cache_read(key.c_str(), out, 128, &recent) && !recent);
    assert(sub_language_probe_try_lock());
    sub_language_probe_unlock();
    assert(!sub_language_playback_count());
    sub_language_playback_active(1); assert(sub_language_playback_count() == 1);
    sub_language_playback_active(-1); assert(!sub_language_playback_count());
}
