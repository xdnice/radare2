#ifndef _R_UTIL_ALLOC_H_
#define _R_UTIL_ALLOC_H_ 1

#include <r_util.h>

#define R_MALLOC_WRAPPER 0
#define R_MALLOC_GLOBAL 0

typedef void *(RMalloc)(size_t);
typedef void *(RCalloc)(size_t, size_t);
typedef void *(RRealloc)(void *, size_t);
typedef void (RFree)(void *);

#if R_MALLOC_WRAPPER

R_API void r_alloc_hooks(RMalloc m, RCalloc c, RRealloc r, RFree f);

#if R_MALLOC_GLOBAL
R_API RMalloc *r_malloc;
R_API RCalloc *r_calloc;
R_API RRealloc *r_realloc;
R_API RFree *r_free;
#else
R_API void *r_malloc(size_t sz);
R_API void *r_calloc(size_t sz);
R_API void *r_realloc(void *p, size_t sz);
R_API void r_free(void *p);
#endif

#else

#define r_malloc(x) malloc((x))
#define r_calloc(x,y) calloc((x),(y))
#define r_realloc(x,y) realloc((x),(y))
#define r_free(x) free((x))

#endif

#endif
