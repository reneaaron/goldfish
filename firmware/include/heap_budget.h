#pragma once
// The heap the app may count on: on the board, what's left of its 512 KB RAM
// after the static data, less what the Arduino core and newlib keep.
// Measured on a board: 78-80 KB free at Read in Scan seed, of 88.7 KB.
//
// The simulator gives the app exactly APP_HEAP_BYTES (src/sim/heap.cpp), so a
// step that doesn't fit fails there too. The board build checks that its heap
// still holds APP_HEAP_BYTES + CORE_HEAP_BYTES (heap_check.py): when static
// data grows past that, lower APP_HEAP_BYTES and see what the simulator says.
#define APP_HEAP_BYTES (92 * 1024)
// What the core keeps: in use at the idle home screen, measured on a board
// with mallinfo() (2026-11-24): 8,828 bytes, so 80 KB free of 90,788. Rounded
// up for malloc's per-block overhead. The core is pinned in platformio.ini:
// after bumping it, measure again (mallinfo().uordblks on the home screen).
#define CORE_HEAP_BYTES (10 * 1024)
