#!/bin/sh
# Rebuilds src/fonts/ from the OFL fonts on Google Fonts (github.com/google/fonts, ofl/):
# FunnelSans[wght].ttf, FunnelDisplay[wght].ttf, JetBrainsMono[wght].ttf and
# NotoSansDevanagari[wdth,wght].ttf, all in the folder given. Needs fonttools and node.
#   sh tools/fonts.sh ~/Downloads/fonts
set -e
IN=$1
OUT=$(cd "$(dirname "$0")/../src/fonts" && pwd)
TMP=$(mktemp -d)
python3 -m fontTools.varLib.instancer -q -o "$TMP/sans.ttf" "$IN/FunnelSans[wght].ttf" wght=500
python3 -m fontTools.varLib.instancer -q -o "$TMP/display.ttf" "$IN/FunnelDisplay[wght].ttf" wght=700
python3 -m fontTools.varLib.instancer -q -o "$TMP/mono.ttf" "$IN/JetBrainsMono[wght].ttf" wght=500
python3 -m fontTools.varLib.instancer -q -o "$TMP/deva.ttf" "$IN/NotoSansDevanagari[wdth,wght].ttf" wght=500 wdth=100
cd "$TMP"

# Latin-1, Latin Extended-A (Polish, Czech, Hungarian, Turkish…), Romanian ș ț, punctuation.
LATIN=0x20-0x7E,0xA0-0xFF,0x100-0x17F,0x218-0x21B,0x2013-0x2014,0x2018-0x201D,0x2022,0x2026,0x2039-0x203A
ARROWS=0x2191,0x2193  # Funnel has none; JetBrains Mono's
DEVA=0x900-0x97F      # drawn letter by letter: LVGL doesn't shape, so conjuncts show their virama

conv() {  # name size main-font [extra lv_font_conv args]
  name=$1 size=$2 font=$3; shift 3
  npx -y lv_font_conv@1.5.3 --font "$font" -r $LATIN --font mono.ttf -r $ARROWS "$@" \
    --size "$size" --bpp 4 --format lvgl --lv-include lvgl.h --no-compress -o "$OUT/$name.c"
}
# Artists and titles use 16, 22 and 30, so those carry Devanagari. Its taller lines would push the
# Latin text down in every label, so the line height stays Funnel's.
keep_metrics() {  # name
  lh=$(grep -o 'line_height = [0-9]*' "$TMP/$1.c" | head -1)
  bl=$(grep -o 'base_line = [0-9]*' "$TMP/$1.c" | head -1)
  sed -i.bak -e "s/line_height = [0-9]*/$lh/" -e "s/base_line = [0-9]*/$bl/" "$OUT/$1.c" && rm "$OUT/$1.c.bak"
}
for spec in "font_16 16 sans.ttf" "font_22 22 sans.ttf" "font_30 30 display.ttf"; do
  set -- $spec
  OUT_SAVE=$OUT; OUT=$TMP; conv $1 $2 $3; OUT=$OUT_SAVE  # Latin only, for its metrics
  conv $1 $2 $3 --font deva.ttf -r $DEVA
  keep_metrics $1
done
conv font_44 44 display.ttf
npx -y lv_font_conv@1.5.3 --font mono.ttf -r $LATIN,$ARROWS --size 14 --bpp 4 --format lvgl \
  --lv-include lvgl.h --no-compress -o "$OUT/font_mono_14.c"
rm -rf "$TMP"
