#pragma once
// LVGL's heap zeroes every block it frees: label text (seed words read from a
// page) would otherwise stay in freed memory. lv_conf.h includes this through
// LV_MEM_POOL_INCLUDE, which only LVGL's builtin allocator
// (lv_mem_core_builtin.c) includes, after lv_tlsf.h: the macros below then
// route its free and realloc through src/app/ram_wipe.cpp.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void lv_wipe_tlsf_free(void *tlsf, void *p);
void *lv_wipe_tlsf_realloc(void *tlsf, void *p, size_t size);
#ifdef __cplusplus
}
#endif
#ifndef LV_MEM_WIPE_IMPL
#define lv_tlsf_free(t, p) lv_wipe_tlsf_free((t), (p))
#define lv_tlsf_realloc(t, p, s) lv_wipe_tlsf_realloc((t), (p), (s))
#endif
