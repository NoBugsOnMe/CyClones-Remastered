# Reverse-engineering CYCLONE.EXE

1. Convert the executable to a relocated ELF:

       python tools/le2elf.py <install>/CYCLONE.EXE cyclone.elf

2. Install Ghidra 11.4.2 and JDK 21.

3. Back up `Ghidra/Processors/x86/data/languages/x86gcc.cspec`, then replace it
   with `x86gcc.cspec.watcom`.

   This file is the GCC spec with its default prototype changed to Watcom
   `__watcall`:
   - arguments in EAX, EDX, EBX, ECX, then the stack
   - result in EAX
   - EBX, ECX, EDX, ESI, EDI and EBP preserved

   The prototype keeps the name `__cdecl`, because other parts of the spec
   refer to the default by that name.

4. Run the analysis:

       analyzeHeadless <projdir> proj -import cyclone.elf -processor x86:LE:32:default -cspec gcc \
           -scriptPath re -preScript PreWatcom.java -postScript DecompAll.java <outdir>

   This writes `decomp.c` (all functions) and `strings.txt` (strings with their
   referencing functions).

Key functions:

| Function | What it does |
|---|---|
| `FUN_00014179` | Map loader |
| `FUN_00014106` | Cube record decode |
| `FUN_00020aa2` | Texture section lookup |
| `FUN_0002c974` | Floor/ceiling renderer |
| `FUN_0002e61e` | Slope polygon builder |
| `FUN_00016afa` | Wall column shading |
| `FUN_00015ca7` | Fade-table row map |
| `FUN_000121a2` | Opens `cyclone.glb` |
