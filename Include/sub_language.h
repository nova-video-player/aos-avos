// Copyright 2026 Courville Software
// SPDX-License-Identifier: Apache-2.0
#ifndef SUB_LANGUAGE_H
#define SUB_LANGUAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SUB_LANG_UNKNOWN, SUB_LANG_METADATA, SUB_LANG_DESCRIPTION, SUB_LANG_FASTTEXT };
enum { SUB_LANG_TEXT = 1, SUB_LANG_ASS, SUB_LANG_MOV_TEXT, SUB_LANG_WEBVTT };
typedef struct sub_language_session SUB_LANGUAGE_SESSION;

int sub_language_resolve(const char *metadata, const char *title, char *language, size_t size);
void sub_language_set_model_path(const char *path);
SUB_LANGUAGE_SESSION *sub_language_create(void);
void sub_language_destroy(SUB_LANGUAGE_SESSION *session);
void sub_language_feed(SUB_LANGUAGE_SESSION *session, int track, int format,
                       const void *data, size_t size, int64_t pts_ms);
int sub_language_poll(SUB_LANGUAGE_SESSION *session, int track, char *language,
                      size_t size, int *confidence);
// Protect publication/readout of the mutable language fields in SUB_PROPERTIES.
void sub_language_metadata_lock(void);
void sub_language_metadata_unlock(void);
// Read-only diagnostics also used by the real-model host tests.
int sub_language_model_count(void);
int sub_language_pending(SUB_LANGUAGE_SESSION *session);
int sub_language_finished(SUB_LANGUAGE_SESSION *session, int track);

#ifdef __cplusplus
}
#endif
#endif
