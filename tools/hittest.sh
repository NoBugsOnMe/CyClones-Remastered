#!/bin/bash
# hittest.sh level "monsters" "weapons" "distances" [angle] -> damage counts per case
L=$1; MS=$2; WS=$3; DS=$4; ANG=${5:-0}
cd /tmp/claude-0/eng/ht
for m in $MS; do for w in $WS; do for d in $DS; do
  out=$(CYCLONES_TRACE=-2 timeout 120 xvfb-run -a -s "-screen 0 1280x960x24" /home/claude/cyclones/engine/build/linux/cyclones-engine --disc /home/claude/cyclones/disc/CYCLONES.cue --hd /home/claude/cyclones/install/CYCLONE.HD --size 320x200 --level $L --focus m:$m,$d,$ANG,0 --shot h_${m}_${w}_${d}.bmp --frames 150 --fast --allweapons --weapon $w --fire 30,70,110 --peaceful --hold 2>&1 | grep "dbg damage monster $m:")
  n=$(echo "$out" | grep -c "dbg damage")
  echo "m$m w$w d$d hits=$n $(echo "$out" | sed 's/.*: //' | tr '\n' ' ')"
done; done; done
