#ifndef SHEN_C_TC_CACHE_H
#define SHEN_C_TC_CACHE_H

#include <stdint.h>

/*
 * Load-time shen.typecheck verdict memo (rust interp/tc_cache.rs).
 * Nesting-sound keying. Do not wrap shen.shen->kl — rust measured that
 * stream wall-neutral.
 *
 * Off unless SHEN_C_TC_CACHE=<dir>. SHEN_C_TC_CACHE_STATS=1 logs stderr.
 */

void shen_tc_cache_install (const char* dir, int stats_on,
                            const char* kernel_dir);
void shen_tc_cache_install_from_env (void);
int shen_tc_cache_stats (uint64_t* hits, uint64_t* misses);

#endif
