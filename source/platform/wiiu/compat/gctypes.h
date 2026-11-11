#pragma once
/* libogc's basic types on the Wii U (wut has none of them) */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef volatile u8  vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;
typedef volatile u64 vu64;
typedef volatile s8  vs8;
typedef volatile s16 vs16;
typedef volatile s32 vs32;
typedef volatile s64 vs64;
typedef float  f32;
typedef double f64;
#ifndef ATTRIBUTE_ALIGN
#define ATTRIBUTE_ALIGN(v) __attribute__((aligned(v)))
#endif
#ifndef ATTRIBUTE_PACKED
#define ATTRIBUTE_PACKED __attribute__((packed))
#endif
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
