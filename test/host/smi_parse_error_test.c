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

/*
 * smi_parse_error_test.c -- the two "no cues" exits of parse_SMI() must close the file
 * and free the record without reading it after free. parse_SMI() is static, so the real
 * source is included; its out-of-file dependencies are stubbed below.
 *
 * Run under AddressSanitizer (Linux also gives LeakSanitizer) to catch a future return to
 * the old order: subtitle_clean_error() first, afree() afterwards.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "subtitle_format.h"
#include "i18n.h"
#include "astdlib.h"

/* Same contract as Source/util.c's case-insensitive finder. */
char *strstrNC(char *string, char *tofind)
{
	size_t len = strlen(tofind);
	for (; *string; ++string) {
		if (!strncasecmp(string, tofind, len))
			return string;
	}
	return NULL;
}

int strncmpNC(const char *s1, const char *s2, int length)
{
	return strncasecmp(s1, s2, length);
}

/* Only reached for cues with legacy codepages; neither file below has a cue. */
int I18N_codepage_to_unicode(const unsigned char *t, wchar *uc)
{
	(void)t;
	(void)uc;
	return 0;
}

void utf16_to_utf8(unsigned char *utf8, const unsigned short *utf16, int utf8_max)
{
	(void)utf8;
	(void)utf16;
	(void)utf8_max;
}

/* The real registry lives in subtitle_formats.c; the format's constructor calls this. */
int subtitle_register_format(SUBTITLE_REG_FORMAT *reg)
{
	(void)reg;
	return 0;
}

/* The real cleanup helper, with the identifier free outside the list walk. */
void subtitle_clean_error(uni_sub *subs)
{
	if (!subs)
		return;
	sub_line *line = subs->first;
	while (line) {
		sub_line *next = line->next;
		afree(line->top);
		afree(line->bottom);
		afree(line);
		line = next;
	}
	afree(subs->identifier);
}

/* Only reached when a cue is parsed; neither file below contains one. */
char *subtitle_clean_formatter(char *line, int clean_tags)
{
	(void)clean_tags;
	return line;
}

#include "../../Source/subtitle_smi.c"

/* One SMI language declaration, so set_SMI_code() succeeds. */
static const char *HEADER =
	"<SAMI>\n"
	"<HEAD>\n"
	"<TITLE>t</TITLE>\n"
	".ENCC {Name:English; lang:en-US;}\n"
	"</HEAD>\n"
	"<BODY>\n";

static void free_header(subt_orig *subs)
{
	for (unsigned int i = 0; i < subs->lan_count; ++i) {
		sub_coding_style *style = subs->title_langs[i];
		afree(style->id);
		afree(style->name);
		afree(style->lang);
		afree(style->type);
		afree(style);
	}
	afree(subs->title_langs);
	afree(subs->default_language);
}

static int parse_file(const char *tail, const char *what)
{
	char path[] = "/tmp/avos-smi-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		perror("mkstemp");
		return 0;
	}
	FILE *file = fdopen(fd, "w");
	if (!file) {
		perror("fdopen");
		close(fd);
		remove(path);
		return 0;
	}
	fputs(HEADER, file);
	fputs(tail, file);
	fclose(file);

	subt_orig subs;
	memset(&subs, 0, sizeof(subs));
	subs.filename = path;
	uni_sub *parsed = parse_SMI(&subs, 0);
	remove(path);

	int ok = parsed == NULL;
	if (parsed) {
		fprintf(stderr, "FAIL: %s returned a record\n", what);
		subtitle_clean_error(parsed);
		afree(parsed);
	}
	free_header(&subs);
	return ok;
}

int main(void)
{
	int ok = 1;
	ok &= parse_file("", "EOF after <BODY>");            /* CLEAN_ERROR */
	ok &= parse_file("</BODY>\n", "empty <BODY></BODY>"); /* success, no cues */
	if (ok)
		printf("ok parse_SMI frees the empty record on both exits\n");
	return ok ? 0 : 1;
}
