// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
// Host integration entry point linked against production AVOS objects.
#include "types.h"
#include "global.h"
#include "file_info.h"
#include "sub_language.h"
#include <libavutil/time.h>
#include <stdio.h>
#include <stdlib.h>
static int abort_scan(void *p) { return p && av_gettime_relative() >= *(int64_t *)p; }
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    sub_language_set_model_path(argv[2]);
    if (argc > 3) sub_language_set_cache_path(argv[3]);
    STREAM_URL src = {0}; FILE_INFO *info = calloc(1,sizeof(*info));
    stream_url_cpy_url(&src, argv[1]);
    int64_t start = av_gettime_relative(), deadline = start;
    if (argc > 4) info->abort_opaque = &deadline;
    int rc = ffmpeg_guess_subtitle_languages(&src, info, abort_scan);
    fprintf(stderr, "RESULT status=%d elapsed_ms=%lld tracks=%d\n",rc,(long long)((av_gettime_relative()-start)/1000),info->av.subs_max);
    for (int i=0;i<info->av.subs_max;i++) fprintf(stderr, "TRACK %d %s %d %d\n",i+1,info->av.sub[i].lang,info->av.sub[i].language_source,info->av.sub[i].language_confidence);
    stream_url_clear(&src); free(info); return rc;
}
