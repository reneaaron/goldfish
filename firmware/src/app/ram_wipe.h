#pragma once
// LVGL keeps copies of what was on screen after it is gone. Freed heap blocks
// are zeroed as they are freed (include/lv_mem_wipe.h); ram_wipe_lvgl() zeroes
// the board's partial-mode draw buffers, which keep the last pixels drawn.
// Call it outside rendering.
void ram_wipe_lvgl();
