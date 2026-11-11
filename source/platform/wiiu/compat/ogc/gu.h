#pragma once
#include "../gctypes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef f32 Mtx[3][4];
typedef f32 Mtx44[4][4];
typedef f32 (*MtxP)[4];
typedef struct { f32 x, y, z; } guVector;
void guMtxIdentity(Mtx mt);
void guMtxCopy(Mtx src, Mtx dst);
void guMtxTransApply(Mtx src, Mtx dst, f32 xT, f32 yT, f32 zT);
void guMtxScaleApply(Mtx src, Mtx dst, f32 xS, f32 yS, f32 zS);
void guOrtho(Mtx44 mt, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);
#ifdef __cplusplus
}
#endif
