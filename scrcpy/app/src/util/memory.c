#include "memory.h"

#include <stdlib.h>
#include <errno.h>
#include <stdbool.h>

#if __GNUC__ >= 5
#define add_overflow __builtin_add_overflow
#define mul_overflow __builtin_mul_overflow
#else
static inline bool add_overflow(size_t a, size_t b, size_t* out) {
    *out = a + b;
    return *out < a;
}

static inline bool mul_overflow(size_t a, size_t b, size_t* out) {
    *out = a * b;
    return a > SIZE_MAX / b;
}
#endif

void *
sc_allocarray(size_t nmemb, size_t size) {
    size_t bytes;
    if (mul_overflow(nmemb, size, &bytes)) {
      errno = ENOMEM;
      return NULL;
    }
    return malloc(bytes);
}
