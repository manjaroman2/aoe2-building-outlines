#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
tool="$repo_dir/sld"
source_file="$repo_dir/sld.c"
header_file="$repo_dir/stb_image_write.h"
export_pngs=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --export-pngs)
            export_pngs=1
            shift
            ;;
        *)
            echo "usage: $0 [--export-pngs]" >&2
            exit 1
            ;;
    esac
done

source_graphics_dir="/home/marc/.steam/steam/steamapps/common/AoE2DE/resources/_common/drs/graphics"
mod_root="/home/marc/.steam/steam/steamapps/compatdata/813780/pfx/drive_c/users/steamuser/Games/Age of Empires 2 DE/76561198315622133/mods/local/BuildingOutlines"
mod_graphics_dir="$mod_root/resources/_common/drs/graphics"

if [[ ! -x "$tool" || "$source_file" -nt "$tool" || "$header_file" -nt "$tool" ]]; then
    gcc -O2 -Wall -Wextra -o "$tool" "$source_file"
fi

mkdir -p "$mod_graphics_dir"
cp "$repo_dir/info.json" "$mod_root/info.json"

shopt -s nullglob
inputs=(
    "$source_graphics_dir"/b_*x1.sld
)

if [[ ${#inputs[@]} -eq 0 ]]; then
    echo "no matching SLD files found" >&2
    exit 1
fi

for input_path in "${inputs[@]}"; do
    base_name="$(basename "$input_path")"
    case "$base_name" in
        *waypoint*|*destr*|*rubble*|*_shadow_*|*flag*)
            continue
            ;;
    esac
    stem_name="${base_name%.sld}"
    outlined_path="$repo_dir/out/$stem_name/${stem_name}_outlined.sld"
    destination_path="$mod_graphics_dir/$base_name"

    cmd=("$tool" --add-outlines --resize-layers --outline-width 4)
    if (( export_pngs )); then
        cmd+=(--export-pngs)
    fi
    cmd+=("$input_path")

    "${cmd[@]}"
    if [[ ! -f "$outlined_path" ]]; then
        printf 'skipped %s (no outlined output)\n' "$base_name"
        continue
    fi
    cp "$outlined_path" "$destination_path"
    printf 'installed %s\n' "$destination_path"
done
