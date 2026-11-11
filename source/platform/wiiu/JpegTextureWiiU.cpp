/* JpegTexture on the Wii U: a JPEG (posters, backdrops, photos) into an
 * RGBA8 texture, at most 1024 pixels a side (libjpeg's own scaling, 1/2,
 * 1/4...): the TV's 1280x720 never shows a picture bigger than that. */
#include "../../ui/JpegTexture.h"
#include "GfxWiiU.h"
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
    unsigned char* px = nullptr;
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
    cinfo.out_color_space = JCS_EXT_RGBA;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;
    while ((cinfo.image_width / cinfo.scale_denom > 1024 || cinfo.image_height / cinfo.scale_denom > 1024) &&
           cinfo.scale_denom < 8)
        cinfo.scale_denom *= 2;
    jpeg_start_decompress(&cinfo);
    const int w = cinfo.output_width, h = cinfo.output_height;
    if (w <= 0 || h <= 0 || cinfo.output_components != 4) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    px = (unsigned char*)malloc((size_t)w * h * 4);
    if (!px) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW r = px + (size_t)cinfo.output_scanline * w * 4;
        jpeg_read_scanlines(&cinfo, &r, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    GRRLIB_texImg* tex = GfxWiiU::wrap(GfxWiiU::make(GfxWiiU::Fmt::RGBA8, w, h, px, w * 4));
    free(px);
    return tex;
}
