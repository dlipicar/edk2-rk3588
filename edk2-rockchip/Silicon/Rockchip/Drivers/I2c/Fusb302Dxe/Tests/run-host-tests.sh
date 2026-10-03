#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(git -C "$script_dir" rev-parse --show-toplevel)
edk2_dir=${EDK2_DIR:-"$repo_dir/edk2"}
cc=${CC:-cc}
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
case "$(uname -m)" in
  arm64|aarch64) include_arch=AArch64 ;;
  x86_64|amd64) include_arch=X64 ;;
  *) echo 'Unsupported host architecture' >&2; exit 1 ;;
esac
source_file="$script_dir/../Fusb302Pd.c"
extra_define=
if [ "$#" -gt 0 ]; then
  git -C "$repo_dir" show "$1:edk2-rockchip/Silicon/Rockchip/Drivers/I2c/Fusb302Dxe/Fusb302Pd.c" > "$test_dir/Fusb302Pd.c"
  git -C "$repo_dir" show "$1:edk2-rockchip/Silicon/Rockchip/Drivers/I2c/Fusb302Dxe/Fusb302Dxe.h" > "$test_dir/Fusb302Dxe.h"
  source_file="$test_dir/Fusb302Pd.c"
  extra_define=-DBASELINE_CODE
fi
"$cc" -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fshort-wchar \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$edk2_dir/MdePkg/Include" -I"$edk2_dir/MdePkg/Include/$include_arch" \
  -I"$repo_dir/edk2-rockchip/Silicon/Rockchip/Include" \
  -DPD_SOURCE_FILE="\"$source_file\"" $extra_define \
  "$script_dir/PdHostTest.c" -o "$test_dir/pd-tests"
"$test_dir/pd-tests"
