/* Diagnostic-only nested CPU accounting. Never linked into product builds. */
#ifndef SHEN_PHASE_PROFILE_H
#define SHEN_PHASE_PROFILE_H
void sp_enter(const char *phase, const char *load);
void sp_leave(void);
unsigned sp_mark(void);
void sp_rewind(unsigned mark);
#endif
