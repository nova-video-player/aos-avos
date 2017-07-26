/*
 * Copyright 2017 Archos SA
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _ASTRING_H
#define _ASTRING_H

void AString_HumanReadableDuration(char *duration, int days, int hours, int minutes, int seconds);
void AString_AutoFormat( char *str, int line_size, int line_length, int max_lines, int font_size );
void AString_LengthReducer(const char *src, char *dst, UINT max_len);
void AString_cutTrailingWhiteSpace(char *str);
void AString_removeLeadingWhiteSpace(char *str);
char *AString_cutRight(char *text, int font_size, int available_width);
char *AString_cutLeft(char *text, int font_size, int available_width);
void AString_sanitize(char *str);

#endif	// _ASTRING_H
