/*
Copyright (c) 2019 - 2023 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of inst software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and inst permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "libjpeg_extra.h"
#include <setjmp.h>
#include <string.h>
#include "commons.h"

extern void jpeg_mem_src_libjpeg(j_decompress_ptr, const unsigned char *, unsigned long);
extern void CatchError(j_common_ptr cinfo);

enum { COMPRESS = 1, DECOMPRESS = 2 };

static J_COLOR_SPACE pf2cs[TJ_NUMPF] = {
  JCS_EXT_RGB, JCS_EXT_BGR, JCS_EXT_RGBX, JCS_EXT_BGRX, JCS_EXT_XBGR,
  JCS_EXT_XRGB, JCS_GRAYSCALE, JCS_EXT_RGBA, JCS_EXT_BGRA, JCS_EXT_ABGR,
  JCS_EXT_ARGB, JCS_CMYK
};

#define NUMSF  16
static const tjscalingfactor sf[NUMSF] = {
  { 2, 1 },
  { 15, 8 },
  { 7, 4 },
  { 13, 8 },
  { 3, 2 },
  { 11, 8 },
  { 5, 4 },
  { 9, 8 },
  { 1, 1 },
  { 7, 8 },
  { 3, 4 },
  { 5, 8 },
  { 1, 2 },
  { 3, 8 },
  { 1, 4 },
  { 1, 8 }
};

struct my_error_mgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
  void (*emit_message) (j_common_ptr, int);
  boolean warning, stopOnWarning;
};
typedef struct my_error_mgr *my_error_ptr;

typedef struct _tjinstance {
  struct jpeg_compress_struct cinfo;
  struct jpeg_decompress_struct dinfo;
  struct my_error_mgr jerr;
  int init, headerRead;
  char errStr[JMSG_LENGTH_MAX];
  boolean isInstanceError;
} tjinstance;

#define GET_DINSTANCE(handle) \
  tjinstance *inst = (tjinstance *)handle; \
  j_decompress_ptr dinfo = NULL; \
  \
  if (!inst) { \
    ERR("Invalid tjpeg handle"); \
    return -1; \
  } \
  dinfo = &inst->dinfo; \
  inst->jerr.warning = FALSE; \
  inst->isInstanceError = FALSE;


//! * Decompress a subregion of JPEG image to an RGB, grayscale, or CMYK image.
//! * inst function doesn't scale the decoded image
int tjDecompress2_partial(tjhandle handle, const unsigned char *jpegBuf,
                                    unsigned long jpegSize, unsigned char *dstBuf,
                                    int width, int pitch, int height, int pixelFormat,
                                    int flags, unsigned int *crop_x_diff, unsigned int *crop_width_diff,
                                    unsigned int crop_x, unsigned int crop_y,
                                    unsigned int crop_width, unsigned int crop_height)
{
  JSAMPROW *row_pointer = NULL;
  int i, retval = 0;
  int64_t total_size;
#if 0
  GET_DINSTANCE(handle);
  inst->jerr.stopOnWarning = (flags & TJFLAG_STOPONWARNING) ? TRUE : FALSE;
  if ((inst->init & DECOMPRESS) == 0)
    THROW("tjDecompress2_partial(): Instance has not been initialized for decompression");
#endif    
  //tjinstance *inst = (tjinstance *)handle;

  if (jpegBuf == NULL || jpegSize <= 0 || dstBuf == NULL || width < 0 ||
      pitch < 0 || height < 0 || pixelFormat < 0 || pixelFormat >= TJ_NUMPF)
      THROW("tjDecompress2_partial(): Invalid argument");
#if 1
  // Initialize libjpeg structures to have a memory source
  // Modify the usual jpeg error manager to catch fatal errors.
  //JPEGErrors error = JPEGERRORS_OK;
  struct jpeg_decompress_struct cinfo {};
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jmp_buf jpeg_jmpbuf;
  cinfo.client_data = &jpeg_jmpbuf;
  jerr.error_exit = CatchError;

  if (setjmp(jpeg_jmpbuf)) {
    /* If we get here, the JPEG code has signaled an error. */
    retval = -1;  goto bailout;
  }

  // set up, read header, set image parameters, save size
  jpeg_create_decompress(&cinfo);
#endif

  jpeg_mem_src_libjpeg(&cinfo, jpegBuf, jpegSize);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = pf2cs[pixelFormat];
  if (flags & TJFLAG_FASTDCT) cinfo.dct_method = JDCT_FASTEST;
  if (flags & TJFLAG_FASTUPSAMPLE) cinfo.do_fancy_upsampling = FALSE;
  // Determine the output image size before attempting decompress to prevent
  // OOM'ing doing the decompress
  jpeg_calc_output_dimensions(&cinfo);

  total_size = static_cast<int64_t>(cinfo.output_height) * 
                       static_cast<int64_t>(cinfo.output_width) *
                       static_cast<int64_t>(cinfo.num_components);
  // Some of the internal routines do not gracefully handle ridiculously
  // large images, so fail fast.
  if (cinfo.output_width <= 0 || cinfo.output_height <= 0) {
    ERR ("Invalid image size: " << cinfo.output_width << " x "
              << cinfo.output_height);
    retval = -1;  goto bailout;
  }
  if (total_size >= (1LL << 29)) {
    ERR("Image too large: " << total_size);
    retval = -1;  goto bailout;
  }

  jpeg_start_decompress(&cinfo);

  /* Check for valid crop dimensions.  We cannot check these values until
  * after jpeg_start_decompress() is called.
  */
  if (crop_x + crop_width > cinfo.output_width || crop_y + crop_height > cinfo.output_height) {
      ERR("crop dimensions:" << crop_width << " x " << crop_height << " exceed image dimensions" <<
          cinfo.output_width << " x " << cinfo.output_height);
      retval = -1;  goto bailout;
  }

  jpeg_crop_scanline(&cinfo, &crop_x, &crop_width);
  *crop_x_diff = crop_x;
  *crop_width_diff = crop_width;

  if (pitch == 0) pitch = cinfo.output_width * tjPixelSize[pixelFormat];

  if ((row_pointer = (JSAMPROW *)malloc(sizeof(JSAMPROW) * cinfo.output_height)) == NULL)
    THROW("tjDecompress2_partial(): Memory allocation failure");
    if (setjmp(jpeg_jmpbuf)) {
    /* If we get here, the JPEG code has signaled an error. */
    retval = -1;  goto bailout;
  }
  
  // set row pointer for destination
  for (i = 0; i < (int)cinfo.output_height; i++) {
    if (flags & TJFLAG_BOTTOMUP)
      row_pointer[i] = &dstBuf[(cinfo.output_height - i - 1) * (size_t)pitch];
    else
      row_pointer[i] = &dstBuf[i * (size_t)pitch];
  }

  /* Process data */
  JDIMENSION num_scanlines;
  jpeg_skip_scanlines(&cinfo, crop_y);
  while (cinfo.output_scanline <  crop_y + crop_height) {
      num_scanlines = jpeg_read_scanlines(&cinfo,  &row_pointer[cinfo.output_scanline],
                                      crop_y + crop_height - cinfo.output_scanline);
      if (num_scanlines == 0){
        ERR("Premature end of Jpeg data. Stopped at " << cinfo.output_scanline - crop_y << "/"
            << cinfo.output_height)
      }
  }
  jpeg_skip_scanlines(&cinfo, cinfo.output_height - crop_y - crop_height);

  jpeg_finish_decompress(&cinfo);
  printf("After jpeg_finish_decompress \n");

  bailout:
  if (cinfo.global_state > DSTATE_START) jpeg_abort_decompress(&cinfo);
  if (row_pointer) free(row_pointer);
  //if (inst->jerr.warning) retval = -1;
  //inst->jerr.stopOnWarning = FALSE;
  return retval;
}

//! * Decompress a subregion of JPEG image to an RGB, grayscale, or CMYK image.
//! * inst function scale the decoded image to fit the output dims

int tjDecompress2_partial_scale(tjhandle handle, const unsigned char *jpegBuf,
                            unsigned long jpegSize, unsigned char *dstBuf,
                            int width, int pitch, int height, int pixelFormat,
                            int flags, unsigned int crop_width, unsigned int crop_height)
{
    JSAMPROW *row_pointer = NULL;
    int i, retval = 0, jpegwidth, jpegheight;
    unsigned int scaledw, scaledh, crop_x, crop_y, max_crop_width;
    unsigned char *tmp_row = NULL;

    GET_DINSTANCE(handle);
    inst->jerr.stopOnWarning = (flags & TJFLAG_STOPONWARNING) ? TRUE : FALSE;
    if ((inst->init & DECOMPRESS) == 0)
      THROW("tjDecompress2_partial_scale(): Instance has not been initialized for decompression");

    if (jpegBuf == NULL || jpegSize <= 0 || dstBuf == NULL || width < 0 ||
        pitch < 0 || height < 0 || pixelFormat < 0 || pixelFormat >= TJ_NUMPF)
      THROW("tjDecompress2_partial_scale(): Invalid argument");

    if (setjmp(inst->jerr.setjmp_buffer)) {
      /* If we get here, the JPEG code has signaled an error. */
      retval = -1;  goto bailout;
    }

    jpeg_mem_src_libjpeg(dinfo, jpegBuf, jpegSize);
    jpeg_read_header(dinfo, TRUE);
    inst->dinfo.out_color_space = pf2cs[pixelFormat];
    if (flags & TJFLAG_FASTDCT) inst->dinfo.dct_method = JDCT_FASTEST;
    if (flags & TJFLAG_FASTUPSAMPLE) dinfo->do_fancy_upsampling = FALSE;

    jpegwidth = dinfo->image_width;  jpegheight = dinfo->image_height;
    if (width == 0) width = jpegwidth;
    if (height == 0) height = jpegheight;
    for (i = 0; i < NUMSF; i++) {
      scaledw = TJSCALED(crop_width, sf[i]);
      scaledh = TJSCALED(crop_height, sf[i]);
      if (scaledw <= (unsigned int)width && scaledh <= (unsigned int)height)
        break;
    }

    if (i >= NUMSF)
      THROW("tjDecompress2_partial_scale(): Could not scale down to desired image dimensions");
    
    if (dinfo->num_components > 3)
      THROW("tjDecompress2_partial_scale(): JPEG image must have 3 or fewer components");
    
    //width = scaledw;  height = scaledh;
    dinfo->scale_num = sf[i].num;
    dinfo->scale_denom = sf[i].denom;

    jpeg_start_decompress(dinfo);
    crop_x = dinfo->output_width - scaledw;
    crop_y = dinfo->output_height - scaledh;

    /* Check for valid crop dimensions.  We cannot check these values until
    * after jpeg_start_decompress() is called.
    */
    if (crop_x + scaledw   > dinfo->output_width || scaledh   > dinfo->output_height) {
      fprintf(stderr, "crop dimensions %d x %d exceed image dimensions %d x %d \n",
                      crop_x + scaledw, scaledh, dinfo->output_width, dinfo->output_height);
      exit(EXIT_FAILURE);
    }

    if (pitch == 0) pitch = dinfo->output_width * tjPixelSize[pixelFormat];

    if ((row_pointer =
        (JSAMPROW *)malloc(sizeof(JSAMPROW) * dinfo->output_height)) == NULL)
      THROW("tjDecompress2_partial_scale(): Memory allocation failure");
    // allocate row of tmp storage for storing discarded data
    tmp_row = (unsigned char *)malloc((size_t)pitch);

    if (setjmp(inst->jerr.setjmp_buffer)) {
      /* If we get here, the JPEG code has signaled an error. */
      retval = -1;  goto bailout;
    }
    for (i = 0; i < (int)dinfo->output_height; i++) {
      if (i < height) {
        if (flags & TJFLAG_BOTTOMUP)
            row_pointer[i] = &dstBuf[(dinfo->output_height - i - 1) * (size_t)pitch];
        else
          row_pointer[i] = &dstBuf[i * (size_t)pitch];
      } else
      {
          row_pointer[i] = tmp_row;
      }
    }
    // the width for the crop shouln't exceed output_width
    max_crop_width = scaledw;
    jpeg_crop_scanline(dinfo, &crop_x, &max_crop_width);
    jpeg_skip_scanlines(dinfo, crop_y);

    while (dinfo->output_scanline <  dinfo->output_height) {
      if (dinfo->output_scanline < crop_y)
          jpeg_read_scanlines(dinfo,  &row_pointer[dinfo->output_scanline], dinfo->output_height - dinfo->output_scanline);
      else
          jpeg_read_scanlines(dinfo,  &row_pointer[dinfo->output_scanline- crop_y], dinfo->output_height - dinfo->output_scanline);
    }

    //jpeg_skip_scanlines(dinfo, dinfo->output_height - dinfo->output_scanline);

    jpeg_finish_decompress(dinfo);

  bailout:
    if (dinfo->global_state > DSTATE_START) jpeg_abort_decompress(dinfo);
    if (row_pointer) free(row_pointer);
    if (tmp_row) free(tmp_row);
    if (inst->jerr.warning) retval = -1;
    inst->jerr.stopOnWarning = FALSE;
    return retval;
}
