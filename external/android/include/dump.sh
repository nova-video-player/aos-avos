#!/bin/sh

# before dumping header, be sure to have good branches setup
# for each dir in frameworks/av frameworks/native frameworks/native system/core hardware/libhardware
#   git checkout BRANCH
# BRANCH=ics-mr0 for api 14
# BRANCH=jb-mr0-release for api 16
# BRANCH=jb-mr1-release for api 17
# BRANCH=jb-mr2-release for api 18
# BRANCH=android-4.4_r1 for api 19
# BRANCH=lollipop-release for api 21

if [ "x$1" = "x" -o "x$2" = "x" ];then
	echo "usage: $0 <aosp_path> <api>"
	exit 1
fi
aosp_path=$1
api=$2

if [ $api -eq 14 ];then
dir_list="""
	frameworks/base/include/binder
	frameworks/base/include/gui
	frameworks/base/include/ui
	frameworks/base/include/media
	frameworks/base/include/utils
	system/core/include/cutils
	system/core/include/system
	system/core/include/pixelflinger
	hardware/libhardware/include
"""
elif [ $api -eq 16 ];then
dir_list="""
	frameworks/native/include/binder
	frameworks/native/include/gui
	frameworks/native/include/ui
	frameworks/native/include/media
	frameworks/native/include/utils
	frameworks/av/include/media
	system/core/include/cutils
	system/core/include/system
	hardware/libhardware/include
"""
elif [ $api -eq 17 ];then
dir_list="""
	frameworks/native/include/binder
	frameworks/native/include/gui
	frameworks/native/include/ui
	frameworks/native/include/media
	frameworks/native/include/utils
	frameworks/av/include/media
	system/core/include/cutils
	system/core/include/system
	system/core/include/sync
	hardware/libhardware/include
"""
elif [ $api -eq 18 ];then
dir_list="""
	frameworks/native/include/binder
	frameworks/native/include/gui
	frameworks/native/include/ui
	frameworks/native/include/media
	frameworks/native/include/utils
	frameworks/av/include/media
	system/core/include/cutils
	system/core/include/system
	system/core/include/sync
	hardware/libhardware/include
"""
elif [ $api -eq 19 ];then
dir_list="""
	frameworks/native/include/binder
	frameworks/native/include/gui
	frameworks/native/include/ui
	frameworks/native/include/media
	frameworks/av/include/media
	system/core/include/cutils
	system/core/include/utils
	system/core/include/system
	system/core/include/sync
	system/core/include/log
	hardware/libhardware/include
"""
else #21 and highr
dir_list="""
	frameworks/native/include/binder
	frameworks/native/include/gui
	frameworks/native/include/ui
	frameworks/native/include/media
	frameworks/av/include/media
	system/core/include/cutils
	system/core/include/utils
	system/core/include/system
	system/core/libsync/
	system/core/include/log
	hardware/libhardware/include
"""
fi

rm -rf ${api}
for dir in $dir_list;do
	echo $dir
	for header in `find $aosp_path/$dir -type f -name "*.h"`;do
		ext_file=`echo $header|sed -e "s,${aosp_path},${api},g"`
		ext_dir=`dirname $ext_file`
		mkdir -p ${ext_dir}
		cp $header ${ext_dir}
	done
done
