#pragma once
// LVGL keeps copies of what was on screen after it is gone: label text in
// freed heap blocks, pixels in the draw buffers. ram_wipe_lvgl() zeroes both.
// Call it outside rendering (the draw buffers are scratch in partial mode).
void ram_wipe_lvgl();
