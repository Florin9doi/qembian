/* Host shim for unit tests of src/qemu-ngage board files that use only these QEMU helpers. */
#ifndef NGAGE_SHIM_OSDEP_H
#define NGAGE_SHIM_OSDEP_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
static inline uint32_t le32_to_cpu(uint32_t v) { return v; }
static inline uint32_t ldl_le_p(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
#endif
