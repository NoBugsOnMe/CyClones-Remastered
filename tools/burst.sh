#!/bin/bash
# burst.sh name level focus weapon fireframes startframe every count cols scale [extra args]
N=$1 L=$2 F=$3 W=$4 FI=$5 S=$6 E=$7 C=$8 COLS=$9 SC=${10}; shift 10
mkdir -p /tmp/claude-0/eng/$N && cd /tmp/claude-0/eng/$N && rm -f *.bmp
DISPLAY=:99 /home/claude/cyclones/engine/build/linux/cyclones-engine --disc /home/claude/cyclones/disc/CYCLONES.cue --hd /home/claude/cyclones/install/CYCLONE.HD --size 640x400 --level $L --focus $F --weapon $W --fire "$FI" --shot f.bmp --frames $S --every $E --burst $C "$@" 2>&1 | grep -i "error\|completed"
python3 /home/claude/cyclones/tools/sheet.py /tmp/claude-0/eng/$N.png $COLS - $SC $(for i in $(seq 0 $((C-1))); do echo f_$i.bmp; done)
