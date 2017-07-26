#ifndef _NEON_DEINTERLACE_H_
#define _NEON_DEINTERLACE_H_

#include <stdint.h>

void neon_XDeint8x8DetectC(int8_t *src, int i_src, int *fr, int *ff);
void neon_XDeint8x8MergeC( uint8_t *dst, uint8_t *src1, uint8_t *src2, int i_src1);
void neon_XDeint8x8FieldEC( uint8_t *dst, uint8_t *src, int i_src);
void neon_XDeint8x8FieldC( uint8_t *dst, uint8_t *src, uint8_t *src2);
int neon_XDeint8x8FieldC_pix( uint8_t *dst, uint8_t *src, uint8_t *src2);

#endif	// _NEON_DEINTERLACE_H_
