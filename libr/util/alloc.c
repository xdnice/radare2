/* radare - LGPL - Copyright 2018 - pancake */

#include <r_util/r_alloc.h>

#if R_MALLOC_WRAPPER

#if R_MALLOC_GLOBAL
R_API void r_alloc_hooks(RMalloc m, RCalloc c, RRealloc r, RFree f) {
	r_malloc = m? m: malloc;
	r_calloc = c? c: calloc;
	r_realloc = r? r: realloc;
	r_free = f? f: free;
}

#else
static RMalloc *_r_malloc = malloc;
static RCalloc *_r_calloc = calloc;
static RRealloc *_r_realloc = realloc;
static RFree *_r_free = free;

R_API void r_alloc_hooks(RMalloc m, RCalloc c, RRealloc r, RFree f) {
	_r_malloc = m? m: malloc;
	_r_calloc = c? c: calloc;
	_r_realloc = r? r: realloc;
	_r_free = f? f: free;
}

R_API void r_alloc_init () {
	r_alloc_hooks (malloc, calloc, realloc, free);
}

R_API void *r_malloc(size_t sz) {
	return _r_malloc? _r_malloc (sz): malloc (sz);
}

R_API void *r_calloc(size_t sz) {
	return _r_malloc? _r_malloc (sz): malloc (sz);
}

R_API void *r_realloc(void *p, size_t sz) {
	if (_r_realloc) {
		return _r_realloc (p, sz);
	}
	return realloc (p, sz);
}

R_API void r_free(void *p) {
	return _r_free? _r_free (p): free (p);
}
#endif
#endif
