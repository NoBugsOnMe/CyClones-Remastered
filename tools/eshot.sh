#!/bin/bash
# eshot.sh <name> [engine args...] : render a frame with the Linux engine build and save PNG
N=$1; shift
mkdir -p /tmp/claude-0/eng
DISPLAY=:99 /home/claude/cyclones/engine/build/linux/cyclones-engine --disc /home/claude/cyclones/disc/CYCLONES.cue \
  --hd /home/claude/cyclones/install/CYCLONE.HD --size 1280x800 --shot /tmp/claude-0/eng/$N.bmp --frames 3 "$@" 2>&1 | grep -v '^$' | tail -3
python3 -c "from PIL import Image; Image.open('/tmp/claude-0/eng/$N.bmp').save('/tmp/claude-0/eng/$N.png')" && echo /tmp/claude-0/eng/$N.png
