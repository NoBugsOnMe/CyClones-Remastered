#!/bin/bash
# play.sh name level start frames every count cols [extra args]  -> /tmp/claude-0/eng/name.png contact sheet
N=$1 L=$2 ST=$3 S=$4 E=$5 C=$6 COLS=$7; shift 7
mkdir -p /tmp/claude-0/eng/$N && cd /tmp/claude-0/eng/$N && rm -f *.bmp
timeout 300 xvfb-run -a -s "-screen 0 1280x960x24" /home/claude/cyclones/engine/build/linux/cyclones-engine --disc /home/claude/cyclones/disc/CYCLONES.cue --hd /home/claude/cyclones/install/CYCLONE.HD --size 640x400 --level $L $([ "$ST" != "-" ] && echo --start $ST) --shot f.bmp --frames $S --every $E --burst $C --fast "$@" 2>&1 | grep -iv "^INFO: *$" | grep -i "error\|warn\|AI\|dbg\|killed\|died" | head -40
python3 /home/claude/cyclones/tools/sheet.py /tmp/claude-0/eng/$N.png $COLS - 1 $(for i in $(seq 0 $((C-1))); do echo f_$i.bmp; done)
