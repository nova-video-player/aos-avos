/*
 * Copyright (C) 2026 The Nova Video Player Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Empty C++ translation unit. Its only purpose is to make ndk-build treat
 * libavos as a C++ module so that libc++_static/libc++abi get linked in.
 * The statically linked libplacebo contains C++ objects (convert.cc) that
 * require the C++ runtime (std::terminate, __gxx_personality_v0, ...).
 */

extern "C" int avos_libplacebo_cxx_runtime_stub(void);
extern "C" int avos_libplacebo_cxx_runtime_stub(void)
{
	return 0;
}
