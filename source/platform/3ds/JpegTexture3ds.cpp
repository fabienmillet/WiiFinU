/* JpegTexture on the 3DS: a JPEG (posters, backdrops, photos) into an
 * RGB565 texture (half the memory of RGBA8).  The screens are half the
 * Wii's frame: pictures are decoded at most 512 pixels a side (libjpeg's
 * own scaling, 1/2, 1/4...), no sharper than what is shown. */
#include "../../ui/JpegTexture.h"
#include "Gfx3ds.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <jpeglib.h>

namespace {
struct ErrMgr { jpeg_error_mgr pub; jmp_buf buf; };
void errExit(j_common_ptr c) { longjmp(((ErrMgr*)c->err)->buf, 1); }
void noOutput(j_common_ptr) {}
}

GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size)
{
    if (size < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF) return nullptr;
    jpeg_decompress_struct cinfo;
    ErrMgr err;
    u16* px = nullptr;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = errExit;
    err.pub.output_message = noOutput;
    if (setjmp(err.buf)) {
        jpeg_destroy_decompress(&cinfo);
        free(px);
        return nullptr;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_RGB;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    while ((cinfo.image_width / cinfo.scale_denom > 512 || cinfo.image_height / cinfo.scale_denom > 512) &&
           cinfo.scale_denom < 8)
        cinfo.scale_denom *= 2;
    jpeg_start_decompress(&cinfo);
    const int w = cinfo.output_width, h = cinfo.output_height;
    if (w <= 0 || h <= 0 || cinfo.output_components != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    px = (u16*)malloc((size_t)w * h * 2);
    unsigned char* row = (unsigned char*)malloc((size_t)w * 3);
    if (!px || !row) {
        free(row);
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        free(px);
        return nullptr;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        const int y = cinfo.output_scanline;
        JSAMPROW r = row;
        jpeg_read_scanlines(&cinfo, &r, 1);
        u16* o = px + (size_t)y * w;
        for (int x = 0; x < w; ++x) {
            const unsigned char* p = row + x * 3;
            o[x] = (u16)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
        }
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    GRRLIB_texImg* tex = Gfx3ds::wrap(Gfx3ds::make(Gfx3ds::Fmt::RGB565, w, h, px, w * 2));
    free(px);
    return tex;
}
