// Copyright 2026 Courville Software
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
/*
 * Font selection shared by the libass-based tests. No font is bundled and none is assumed: the
 * harness (test_host_native.py) finds a real TrueType/OpenType file on the machine (an explicit
 * override, a well-known system font, or fontconfig's sans-serif) and passes it through
 *
 *   HOST_TEST_FONT          path of the font file
 *   HOST_TEST_FONT_FAMILY   its family name, as ASS styles must spell it
 *   HOST_TEST_FONT_DIR      a folder holding just that file (for the backend's fonts_dir)
 *
 * Every check in these tests compares text rendered with the SAME font against itself (a reference
 * render, a control ordering, a ratio), so which font it is does not matter -- only that real glyphs
 * render. Test scripts therefore carry the placeholder "@FONT@" where a family name goes, and
 * with_font() fills it in.
 */
#include <stdlib.h>
#include <string.h>

#define FONT_PLACEHOLDER "@FONT@"

/* Family to use in styles; "DejaVu Sans" only when a test is run by hand without the harness. */
static inline const char *test_font_family(void) {
    const char *f = getenv("HOST_TEST_FONT_FAMILY");
    return (f && f[0]) ? f : "DejaVu Sans";
}

/* Returns a malloc'd copy of `tmpl` with every "@FONT@" replaced by the test font family. */
static inline char *with_font(const char *tmpl) {
    const char *fam = test_font_family();
    size_t flen = strlen(fam), plen = strlen(FONT_PLACEHOLDER), n = 0;
    for (const char *p = tmpl; (p = strstr(p, FONT_PLACEHOLDER)); p += plen) n++;
    char *out = malloc(strlen(tmpl) + n * flen + 1), *w = out;
    const char *p = tmpl, *hit;
    while ((hit = strstr(p, FONT_PLACEHOLDER))) {
        memcpy(w, p, (size_t)(hit - p)); w += hit - p;
        memcpy(w, fam, flen); w += flen;
        p = hit + plen;
    }
    strcpy(w, p);
    return out;
}
