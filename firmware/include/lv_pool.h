#pragma once
// LVGL's heap, owned by the app (src/app/ram_wipe.cpp) so free blocks can be
// wiped. Included by LVGL's C allocator via LV_MEM_POOL_INCLUDE in lv_conf.h.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *lv_app_pool(size_t size);
#ifdef __cplusplus
}
#endif
