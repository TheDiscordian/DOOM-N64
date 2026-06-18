# Project Summary
Software port of original DOOM game for PC modified to run on original N64 game console

> ⚠️ **STALE upstream doc.** The "Building The Project" section below is the original
> upstream guidance and is **WRONG for this fork** — we do NOT build via WSL/README, and
> the `DEBUG`/`UPLOAD` flags are not how this fork is built. The real build is Docker
> (`doom-n64:tc`); see **CLAUDE.md → Build** for the authoritative workflow. This file is
> still correct for the **Target Platform Hardware** notes below (RDRAM, FPU, cache).

# Building The Project
WSL (Windows Subsystem for LInux) instead of developing natively on Linux. Use "WSL" command in terminal to enter linux subsystem. Once buld runs on N64, debug.log file is created in project root
- `README.md` contains build instructions

## Build Flags
- DEBUG=1 — Debug mode allows on screen printing and writes logging data to debug.log file to be generated
- UPLOAD=1 — Invoke upload script to load built ROM onto N64 Flash Cart (not yet implemented)
- 'Readme.md' — IN project root, Contains build instructions

# Target Platform Hardware
N64 game console witha RISC based MIPS R4300i-series processor. 8KB of L1 cache, no L2 cache. Lacks branch prediction and will always executes first instruction after any IF or IF ELSE statement, so make first instruction simple, no calling into DRAM
Graphic processors: SGI 62.5 MHz 64-bit RCP (Reality Co-Processor), with 2 sub-processors:
- RSP (Reality Signal Processor) Controls 3D graphics and audio functionalities
- RDP (Reality Drawing Processor) Rasterizer handles all pixel drawing operations in hardware

## Platform Bottleneck
Bottleneck is 250MB/s memory bandwidth of Rambus DRAM (RDRAM). It is expensive to fetch data from RAM with latency of around 640ns for single call. When possible, use Direct Memory Accees (DMA), bandwidth can be as high as 562.5 MB/s

## Asset Pipeline
Built assets consist of original .WAD file (IWAD, PWAD) staged at: `filesystem/`

## Data Type Considerations
N64 FPU flushes denormalized floats to zero instead of following IEEE 754, meaning very small values near zero get silently discarded. This can cause division by zero, NaN propagation, or values snapping unexpectedly, especially in physics and audio calculations. The fix is to clamp small values, add epsilon guards before division, or use fixed-point math instead

## Key Project Paths
- `linuxdoom-1.10/` — Core DOOM project files
- `libdragon/` — External lib (DO NOT modify)
- `tiny3d/` — External lib (DO NOT modify)
- `Docs/` — Project related documents and instructions

# Coding Libraries
- Libdragon — https://github.com/DragonMinded/libdragon.git
- Tiny3D — https://github.com/HailToDodongo/tiny3d.git

## Reference examples for best practice
- `libdragon/examples/` — Audio, input, rendering, filesystem demos
- `tiny3d/examples/` — 3D models, animation, lighting demos
*Never include any example content in main project*

# "Sparse Speaker" Mode
## Core Rules
Respond like you are "Sparse Speaker". Cut all articles, fillers, pleasantries. But keep all technical substance

## Grammar rules
- Drop articles (a, an, the)
- Drop fillers (just, really, basically, actually, simply)
- Drop pleasantries (sure, basically, actually, simply)
- Strip leading self-correction or hesitation marker (e.g. "wait," "actually," "scratch that")
- Short synonyms (big not extensive, fix not "implement solution for")
- Code blocks unchanged. "Sparse Speaker" speak around code, not in code
- Error message quoted exactly. "Sparse Speaker" only for explanation
- Fragments fine. No need full sentence