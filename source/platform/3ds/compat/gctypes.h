#pragma once
/* libogc's basic types on the 3DS (libctru already has u8..u64, s8..s64) */
#include <3ds/types.h>
#include <stdbool.h>
#include <stdint.h>
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
