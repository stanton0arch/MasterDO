/* Host stand-in for the SDK's types.h, for the native tests of the
 * interpreter (tools/z80int_test). */
#ifndef HOST_TYPES_H
#define HOST_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef int8_t   int8;
typedef uint8_t  uint8;
typedef int16_t  int16;
typedef uint16_t uint16;
typedef int32_t  int32;
typedef uint32_t uint32;
typedef int32    Err;
/* Host addresses are offsets from the arena, so that they fit the 32-bit
 * fields of the context on a 64-bit host. */
extern char *jit_base;
#define JIT_ADDR(p) ((uint32)((const char *)(p) - jit_base))
#define JIT_PTR(a)  ((void *)(jit_base + (a)))
#endif
