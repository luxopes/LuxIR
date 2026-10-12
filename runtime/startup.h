#ifndef LUXRT_STARTUP_H
#define LUXRT_STARTUP_H
#include <stdint.h>
/* Linux kernel entry stack. Valid kernel-provided argc/argv/envp/auxv only. */
uint64_t luxrt_startup_argc(const uint64_t *stack);
char **luxrt_startup_argv(const uint64_t *stack);
char **luxrt_startup_env(const uint64_t *stack);
uint64_t luxrt_startup_pagesize(const uint64_t *stack); /* 0 if AT_PAGESZ absent */
const char *luxrt_getenv(char *const *env,const char *name); /* NULL if absent/invalid */
#endif
