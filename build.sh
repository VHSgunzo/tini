#!/bin/sh
set -e
HERE="$(dirname "$(readlink -f "$0")")"
cd "$HERE"

WITH_UPX=1
VENDOR_UPX=1

platform="$(uname -s)"
platform_arch="$(uname -m)"
export MAKEFLAGS="-j$(nproc)"

if [ "$platform" == "Linux" ]
    then
        export CFLAGS="-static"
        export LDFLAGS='--static'
    else
        echo "= WARNING: your platform does not support static binaries."
        echo "= (This is mainly due to non-static libc availability.)"
        exit 1
fi

if [ -x "$(which apk 2>/dev/null)" ]
    then
        apk add git gcc musl-dev upx cmake make patch
fi

if [ "$WITH_UPX" == 1 ]
    then
        if [[ "$VENDOR_UPX" == 1 || ! -x "$(which upx 2>/dev/null)" ]]
            then
                upx_ver=4.2.4
                case "$platform_arch" in
                   x86_64) upx_arch=amd64 ;;
                   aarch64) upx_arch=arm64 ;;
                esac
                wget https://github.com/upx/upx/releases/download/v${upx_ver}/upx-${upx_ver}-${upx_arch}_linux.tar.xz
                tar xvf upx-${upx_ver}-${upx_arch}_linux.tar.xz
                mv upx-${upx_ver}-${upx_arch}_linux/upx /usr/bin/
                rm -rf upx-${upx_ver}-${upx_arch}_linux*
        fi
fi

if [ -d build ]
    then
        echo "= removing previous build directory"
        rm -rf build
fi

# if [ -d release ]
#     then
#         echo "= removing previous release directory"
#         rm -rf release
# fi

echo "=  create build and release directory"
mkdir -p build
mkdir -p release

(cd build

export CFLAGS="$CFLAGS -Os -g0 -ffunction-sections -fdata-sections -fvisibility=hidden -fmerge-all-constants"
export LDFLAGS="$LDFLAGS -Wl,--gc-sections -Wl,--strip-all"

tini_version="$(git describe --long --tags|sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g')"
tini_dir="${HERE}"
echo "= tini v${tini_version}"

echo "= build tini"
(export CC=gcc
cmake .. && make)

echo "= extracting tini binaries and libraries"
mv -fv "${tini_dir}"/build/tini "$HERE"/release/tini-${platform_arch}
)

echo "= build super-strip"
(cd build && git clone https://github.com/aunali1/super-strip.git && cd super-strip
make
mv -fv sstrip /usr/bin/)

echo "= super-strip release binaries"
sstrip release/*-"${platform_arch}"

if [[ "$WITH_UPX" == 1 && -x "$(which upx 2>/dev/null)" ]]
    then
        echo "= upx compressing"
        find release -name "*-${platform_arch}"|\
        xargs -I {} upx --force-overwrite -9 --best {} -o {}-upx
fi

if [ "$NO_CLEANUP" != 1 ]
    then
        echo "= cleanup"
        rm -rfv build
fi

echo "= tini done"
