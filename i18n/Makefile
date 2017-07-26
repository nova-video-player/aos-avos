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

all: ../Source/i18n_936.c ../Source/i18n_950.c ../Source/i18n_932.c ../Source/i18n_949.c

########################################
#
# Simplified Chinese - CP936
#
########################################

cp936: cp936.c
	gcc -o cp936 cp936.c
	
../Source/i18n_936.c: cp936
	./cp936 > ../Source/i18n_936.c 
	
########################################
#
# Traditional Chinese - BIG 5 - CP950
#
########################################
cp950: cp950.c
	gcc -o cp950 cp950.c
	
../Source/i18n_950.c: cp950
	./cp950 > ../Source/i18n_950.c 
	
########################################
#
# Japanese - JIS0208 - CP932
#
########################################
cp932: cp932.c
	gcc -o cp932 cp932.c
	
../Source/i18n_932.c: cp932
	./cp932 > ../Source/i18n_932.c

########################################
#
# Korean - unihan - CP949
#
########################################
cp949: cp949.c
	gcc -o cp949 cp949.c
	
../Source/i18n_949.c: cp949
	./cp949 > ../Source/i18n_949.c
	
clean: 
	rm -f cp950 cp936 cp932 cp949

