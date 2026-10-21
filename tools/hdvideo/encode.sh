#!/bin/bash
# Builds the Enhanced cut-scenes pack (HD Video/): AV1 10-bit in IVF (SVT-AV1, key frame every
# 30 pictures, closed GOP), the cleaned speech as FLAC at 22,050 Hz, and index.txt.
set -e
cd /home/claude/hdvid
OUT=pack
mkdir -p $OUT/speech
ENC=(-c:v libsvtav1 -preset 5 -g 30 -svtav1-params "irefresh-type=2:tune=0:enable-overlays=0" -fps_mode passthrough)
COL=(-vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p10le" -color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv)
: > $OUT/index.new
echo "# CyClones: Remastered - Enhanced cut-scenes (made from the game's own videos)" >> $OUT/index.new
# The CGI videos (cgi.py's 4x frames) are left out: they play as the game's own. To try them:
# CGI="RAVEN INT1 INT2 INT3 4 7 11 14 22" ./encode.sh
for v in $CGI; do
  [ -s $OUT/$v.ivf ] || ffmpeg -v error -y -framerate 10 -i hd/$v/%04d.png "${COL[@]}" "${ENC[@]}" -crf 26 $OUT/$v.ivf
  echo "video $v full $v.ivf" >> $OUT/index.new
done
for v in 1A2A 1A2B 1A3 30 31 32 33 34 35 36A 36B 37 38 39 40; do
  if [ ! -s $OUT/$v.ivf ]; then
    rm -rf tmpseq && mkdir tmpseq
    i=0
    for f in $(ls live/$v/ | sort); do i=$((i+1)); ln -s ../live/$v/$f tmpseq/$(printf %05d $i).png; done
    ffmpeg -v error -y -framerate 10 -i tmpseq/%05d.png "${COL[@]}" "${ENC[@]}" -crf 28 $OUT/$v.ivf
  fi
  echo "video $v window 77 47 164 106 $v.ivf $(cat live/$v.runs)" >> $OUT/index.new
done
for id in 225 226 202 203 204 210 211 212 215 230 231 217 221 223 250; do
  [ -s $OUT/speech/$id.flac ] || ffmpeg -v error -y -i speech/$id.wav -af "aresample=22050:resampler=soxr:precision=28" -c:a flac -compression_level 8 -sample_fmt s16 $OUT/speech/$id.flac
  echo "speech $id speech/$id.flac" >> $OUT/index.new
done
mv $OUT/index.new $OUT/index.txt
du -sh $OUT; ls -la $OUT
