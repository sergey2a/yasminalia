#!/usr/bin/env bash
# Собирает и запускает хост-превью лица. Результат: PNG-портреты состояний
# и preview.gif с анимацией — в каталоге, переданном первым аргументом.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/preview-out}"
mkdir -p "$out"

g++ -O2 -std=c++17 -ffast-math \
    -DBOARD_ESP32_WROOM \
    -I"$root/tools/preview/shim" -I"$root/include" -I"$root/src" \
    "$root/tools/preview/preview.cpp" "$root/src/face.cpp" "$root/src/shapes.cpp" \
    -o "$out/preview"

"$out/preview" "$out"

if command -v magick >/dev/null; then
  for f in "$out"/face_*.ppm "$out"/talk_*.ppm; do magick "$f" "${f%.ppm}.png"; done
fi
if command -v ffmpeg >/dev/null; then
  ffmpeg -y -loglevel error -framerate 25 -i "$out/anim_%04d.ppm" \
         -vf "scale=240:240:flags=neighbor,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
         "$out/preview.gif"
fi
python3 "$root/tools/shapes.py" export "$out" >/dev/null
rm -f "$out"/anim_*.ppm "$out"/face_*.ppm "$out"/talk_*.ppm
echo "готово: $out"
