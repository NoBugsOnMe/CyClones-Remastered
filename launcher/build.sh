#!/bin/bash
set -e
cd "$(dirname "$0")"
python3 -I make_icon.py cyclones.ico
x86_64-w64-mingw32-windres resources.rc -O coff -o resources.o
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -municode -mwindows -static \
  main.cpp resources.o -o "CyClones Remastered.exe" -lcomctl32 -lcomdlg32 -lshell32 -luser32 -lgdi32
ls -l "CyClones Remastered.exe"
