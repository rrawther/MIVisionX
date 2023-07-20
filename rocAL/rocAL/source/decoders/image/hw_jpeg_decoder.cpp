/*
Copyright (c) 2019 - 2023 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#ifdef ROCAL_VIDEO

#include <stdio.h>
#include <stdlib.h>
#include <commons.h>
#include <fstream>
#include <string.h>
#include <hip/hip_runtime.h>

#include "hw_jpeg_decoder.h"

#ifndef USE_VCNDECODE_API

#define AVIO_CONTEXT_BUF_SIZE   32768     //32K

struct buffer_data {
    uint8_t *ptr;
    size_t size; ///< size left in the buffer
};

static inline int num_hw_devices() {

    int num_hw_devices = 0;
    FILE *fp = popen("ls -l /dev/dri", "r");
    if (fp == NULL)
      return num_hw_devices;

    char *path = NULL;
    size_t length = 0;
    std::string line;
    while (getline(&path, &length, fp) >= 0)
    {
        line = std::string(path, length);
        if(line.find("renderD") != std::string::npos)
          num_hw_devices++;
    }
    pclose(fp);
    return num_hw_devices;
}

static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }
    WRN("HardwareJpegDecoder::Unable to decode using VA-API");

    return AV_PIX_FMT_NONE;
}

// ffmpeg helper functions for custom AVIOContex for bitstream reading
static int ReadFunc(void* ptr, uint8_t* buf, int buf_size)
{
    struct buffer_data *bd = (struct buffer_data *)ptr;
    buf_size = FFMIN(buf_size, bd->size);

    if (!buf_size)
        return AVERROR_EOF;
    //printf("ptr:%p size:%zu\n", bd->ptr, bd->size);

    /* copy internal buffer data to buf */
    memcpy(buf, bd->ptr, buf_size);
    bd->ptr  += buf_size;
    bd->size -= buf_size;

    return buf_size;
}
#endif

void HWJpegDecoder::initialize(int dev_id){

#ifdef USE_VCNDECODE_API
    _vcn_decoder = new VCNDecode(dev_id);
    if (!_vcn_decoder || !_vcn_decoder->isJPEGHWDecoderSupported()) 
        THROW("HardwareJpegDecoder::Initialize ERROR: vaapi is not supported for this device\n");
#else
    int ret = 0;
    char device[128] = "";
    char* pdevice = NULL;
    int num_devices = 1; // default;
    hipError_t hipStatus;

    hipStatus = hipGetDeviceCount(&num_devices);
    if ((hipStatus != hipSuccess) || num_devices < 1) {
        THROW("HardwareJpegDecoder::Initialize ERROR: Could not find GPU device\n");
    }
    if (dev_id >= 0) {
        snprintf(device, sizeof(device), "/dev/dri/renderD%d", (128 + (dev_id % num_devices)));
        pdevice = device;
    }
    const char* device_name = pdevice? pdevice : NULL;

    if ((ret = av_hwdevice_ctx_create(&_hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, device_name, NULL, 0)) < 0)
        THROW("Couldn't find vaapi device for device_id: " + device_name)
    _hw_type = av_hwdevice_find_type_by_name("vaapi");
    if (_hw_type == AV_HWDEVICE_TYPE_NONE) {
        THROW("HardwareJpegDecoder::Initialize ERROR: vaapi is not supported for this device\n");
    }
    else
        INFO("HardwareJpegDecoder::Initialize : Found vaapi device for the device\n");
#endif        
};


Decoder::Status HWJpegDecoder::decode_info(unsigned char* input_buffer, size_t input_size, int* width, int* height, int* color_comps) 
{
#ifdef USE_VCNDECODE_API
    if (!_vcn_decoder) {
        ERR("HardwareJpegDecoder::not initialized");
        return Status::UNSUPPORTED;
    }
    uint8_t nComponents;
    vcnImageFormat_t subsampling;
    if (!_vcn_decoder->getImageInfo(input_buffer, input_size, &nComponents, subsampling, (uint32_t *)width, (uint32_t *)height)) {
        ERR("HardwareJpegDecoder::Failed decoding input stream");
        return Status::CONTENT_DECODE_FAILED;
    }
#else
    struct buffer_data bd = { 0 };
    int ret = 0;
    bd.ptr  = input_buffer;
    bd.size = input_size;
    AVCodec *_decoder = NULL;

    if (!(_fmt_ctx = avformat_alloc_context())) {
        return Status::NO_MEMORY;
    }
    
    uint8_t * _avio_ctx_buffer = (uint8_t *)av_malloc(AVIO_CONTEXT_BUF_SIZE);
    if (!_avio_ctx_buffer) {
        THROW("HardwareJpegDecoder::Initialize ERROR: NO_MEMORY\n");
    }
    _io_ctx = avio_alloc_context(_avio_ctx_buffer, AVIO_CONTEXT_BUF_SIZE,
                                0, &bd, &ReadFunc, NULL, NULL);
    if (!_io_ctx) {
        return Status::NO_MEMORY;
    }
    
    _fmt_ctx->pb = _io_ctx;
    _fmt_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    ret = avformat_open_input(&_fmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        ERR("HardwareJpegDecoder::avformat_open_input failed");
        return Status::HEADER_DECODE_FAILED;
    }
    ret = avformat_find_stream_info(_fmt_ctx, NULL);
    if (ret < 0) {
        ERR("HardwareJpegDecoder::Initialize av_find_stream_info error");
        return Status::HEADER_DECODE_FAILED;
    }
    ret = av_find_best_stream(_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &_decoder, 0);
    if (ret < 0)
    {
        ERR("HardwareJpegDecoder::Initialize Could not find %s stream in input file " + 
            STR(av_get_media_type_string(AVMEDIA_TYPE_VIDEO)));
        return Status::HEADER_DECODE_FAILED;
    }
    _video_stream_idx = ret;

    //if (_video_dec_ctx == NULL) {
        _video_dec_ctx = avcodec_alloc_context3(_decoder);
        if (!_video_dec_ctx)
        {
            ERR("HardwareJpegDecoder::Initialize Failed to allocate the " +
                    STR(av_get_media_type_string(AVMEDIA_TYPE_VIDEO)) + " codec context");
            return Status::NO_MEMORY;
        }
    //}
    _video_stream = _fmt_ctx->streams[_video_stream_idx];

    if (!_video_stream)
    {
        ERR("HardwareJpegDecoder::Initialize Could not find video stream in the input, aborting");
        return Status::HEADER_DECODE_FAILED;
    }
    // Copy codec parameters from input stream to output codec context 
    if ((ret = avcodec_parameters_to_context(_video_dec_ctx, _video_stream->codecpar)) < 0)
    {
        ERR("HardwareJpegDecoder::Initialize Failed to copy " +
                STR(av_get_media_type_string(AVMEDIA_TYPE_VIDEO)) + " codec parameters to decoder context");
        return Status::HEADER_DECODE_FAILED;
    }
    _video_dec_ctx->hw_device_ctx = av_buffer_ref(_hw_device_ctx);
    if (!_video_dec_ctx->hw_device_ctx) {
        ERR("HardwareJpegDecoder:: hardware device reference create failed.\n");
        return Status::NO_MEMORY;
    }
    _video_dec_ctx->get_format = get_vaapi_format;
    if (_video_dec_ctx->pix_fmt == AV_PIX_FMT_YUVJ420P)
      _dec_pix_fmt = AV_PIX_FMT_NV12;    // set non-depracated format, vaapi uses NV12 for YUVJ420P
    else if (_video_dec_ctx->pix_fmt == AV_PIX_FMT_YUVJ422P)
      _dec_pix_fmt = AV_PIX_FMT_YUV422P;    // set non-depracated format
    else if (_video_dec_ctx->pix_fmt == AV_PIX_FMT_YUVJ444P)
      _dec_pix_fmt = AV_PIX_FMT_YUV444P;    // set non-depracated format
    else
      _dec_pix_fmt = _video_dec_ctx->pix_fmt;    // correct format will be set after vaapi initialization for hwdec
       
    // Init the decoders 
    if ((ret = avcodec_open2(_video_dec_ctx, _decoder, NULL)) < 0)
    {
        ERR("HardwareJpegDecoder::Initialize Failed to open " + STR(av_get_media_type_string(AVMEDIA_TYPE_VIDEO)) + " codec");
        return Status::HEADER_DECODE_FAILED;
    }
    _codec_width = _video_stream->codecpar->width;
    _codec_height = _video_stream->codecpar->height;
    *width = _codec_width;
    *height = _codec_height;
#endif
    return Status::OK;
}

Decoder::Status HWJpegDecoder::decode(unsigned char *input_buffer, size_t input_size, unsigned char *output_buffer,
                                  size_t max_decoded_width, size_t max_decoded_height,
                                  size_t original_image_width, size_t original_image_height,
                                  size_t &actual_decoded_width, size_t &actual_decoded_height,
                                  Decoder::ColorFormat desired_decoded_color_format, DecoderConfig config, unsigned int flags)
{
    Decoder::Status status = Status::OK;
#ifdef USE_VCNDECODE_API
    if (!_vcn_decoder) {
        ERR("HardwareJpegDecoder::not initialized");
        return Status::UNSUPPORTED;
    }
    int64_t pts = 0;
    _vcn_decoder->decode(input_buffer, input_size, pts); //kick of decoding
    _vcn_decoder->getOutputImageInfo(&_out_imageinfo);
    _codec_width = _out_imageinfo->nOutputWidth;
    _codec_height = _out_imageinfo->nOutputHeight;

    vcnImageFormat_t out_pix_fmt = VCN_FMT_RGB24;
    int planes = 3;
    switch (desired_decoded_color_format) {
        case Decoder::ColorFormat::GRAY:
            out_pix_fmt = VCN_FMT_RGB24;
            planes = 1;
        break;
        case Decoder::ColorFormat::RGB:
            out_pix_fmt = VCN_FMT_RGB24;
        break;
        default:
            ERR("HardwareJpegDecoder::unsupported output format");
            return Status::CONTENT_DECODE_FAILED;                
        break;
    };

    auto is_scaling = (_codec_width > max_decoded_width) || (_codec_height > max_decoded_height);
    auto is_format_change = (out_pix_fmt != _out_imageinfo->chromaFormat);
    auto is_output_rgb = (out_pix_fmt == VCN_FMT_RGB24);

    if (is_scaling) {
        float scaled_h, scaled_w;
        // calculate the scaling_w and scaling_height
        if (_codec_width > _codec_height) {
            scaled_h = _codec_height * (static_cast<float>(max_decoded_width) / _codec_width);
            scaled_w = _codec_width * (static_cast<float>(scaled_h) / _codec_height);
        } else {
            scaled_w = _codec_width * (static_cast<float>(max_decoded_height) / _codec_height);
            scaled_h = _codec_height * (static_cast<float>(scaled_h) / _codec_width);
        }
        _scale_w = std::lround(scaled_w), _scale_h = std::lround(scaled_h);        
        if (!_scale_hstride) _scale_hstride = ALIGN16(max_decoded_width);     // make it for max resolution to avoid reallocation
        if (!_scale_vstride) _scale_hstride = ALIGN16(max_decoded_height);     // make it for max resolution to avoid reallocation

        allocateDevMemForYUVScaling()
    }
    // convert YUV to RGB format if the requested output frame format is RGB
    if (is_output_rgb) {
        if (!allocateDevMemForRGBConversion(&pRGBdevMem, alignedScalingHeight, scaledYUVstride, isScaling, vaDrmPrimeSurfaceDesc)) {
            std::cout << "ERROR: allocating device memories for RGB conversion failed!" << std::endl;
            return -1;
        }
        if (!colorConvertYUVtoRGB(pYUVdevMem, pScaledYUVdevMem, pRGBdevMem, scaledLumaSize, alignedScalingWidth, alignedScalingHeight, scaledYUVstride,
            isScaling, vaDrmPrimeSurfaceDesc, hipStream)) {
                std::cout << "ERROR: YUV to RGB color conversion failed!" << std::endl;
                return -1;
        }
    }


#else
    AVPixelFormat out_pix_fmt = AV_PIX_FMT_RGB24;
    int planes = 3;
    hipExternalMemory_t hipExtMem;    // for interop
    VADRMPRIMESurfaceDescriptor vaDrmPrimeSurfaceDesc = {};

    switch (desired_decoded_color_format) {
        case Decoder::ColorFormat::GRAY:
            out_pix_fmt = AV_PIX_FMT_GRAY8;
            planes = 1;
        break;
        case Decoder::ColorFormat::RGB:
            out_pix_fmt = AV_PIX_FMT_RGB24;
        break;
        case Decoder::ColorFormat::BGR:
            out_pix_fmt = AV_PIX_FMT_BGR24;
        break;
    };
    SwsContext *swsctx = nullptr;
    AVFrame *sw_frame = nullptr;
    auto is_scaling = (max_decoded_width != _codec_width) || (max_decoded_height != _codec_height);
    auto is_format_change = (out_pix_fmt != _dec_pix_fmt);
    if (!(flags & IMAGE_LOADER_FLAGS_USING_DEVICE_MEM)) {
        // Initialize the SwsContext 
        if (is_scaling || is_format_change) {
            swsctx = sws_getCachedContext(nullptr, _codec_width, _codec_height, _dec_pix_fmt,
                                          max_decoded_width, max_decoded_height, out_pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!swsctx)
            {
                ERR("HardwareJpegDecoder::Decode Failed to get sws_getCachedContext");
                return Status::CONTENT_DECODE_FAILED;
            }
        }
        sw_frame = av_frame_alloc();
        if (!sw_frame) {
            ERR("HardwareJpegDecoder::Decode couldn't allocate sw_frame");
            return Status::NO_MEMORY;
        }
    } 
    AVFrame *dec_frame = av_frame_alloc();
    if ( !dec_frame) {
        ERR("HardwareJpegDecoder::Decode couldn't allocate dec_frame");
        return Status::NO_MEMORY;
    }

    unsigned frame_count = 0;
    bool end_of_stream = false;
    AVPacket pkt = { 0 };
    uint8_t *dst_data[4] = {0};
    int dst_linesize[4] = {0};
    int image_size = max_decoded_height * max_decoded_width * planes * sizeof(unsigned char);

    do
    {
        int ret;
        // read packet from input file
        ret = av_read_frame(_fmt_ctx, &pkt);
        if (ret < 0 && ret != AVERROR_EOF)
        {
            ERR("HardwareJpegDecoder::Decode Failed to read the frame: ret=" + TOSTR(ret));
            status = Status::CONTENT_DECODE_FAILED;
            break;
        }
        if (ret == 0 && pkt.stream_index != _video_stream_idx) continue;
        end_of_stream = (ret == AVERROR_EOF);
        if (end_of_stream)
        {
            // null packet for bumping process
            pkt.data = nullptr;
            pkt.size = 0;
        }

        // submit the packet to the decoder
        ret = avcodec_send_packet(_video_dec_ctx, &pkt);
        if (ret < 0)
        {
            ERR("HardWareJpegDecoder::Decode Error while sending packet to the decoder\n");
            status = Status::CONTENT_DECODE_FAILED;
            break;
        }

        // get all the available frames from the decoder
        while (ret >= 0)
        {
            ret = avcodec_receive_frame(_video_dec_ctx, dec_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) {
                ERR("HardWareJpegDecoder::avcodec_receive_frame failed\n");
                break;
            }
            //retrieve data from GPU to CPU
            if ((av_hwframe_transfer_data(sw_frame, dec_frame, 0)) < 0) {
                ERR("HardWareVideoDecoder::Decode avcodec_receive_frame() failed");
                status = Status::CONTENT_DECODE_FAILED;
                break;
            }
            dst_data[0] = output_buffer;
            dst_linesize[0] = max_decoded_width*planes;
            if (swsctx)
                sws_scale(swsctx, sw_frame->data, sw_frame->linesize, 0, sw_frame->height, dst_data, dst_linesize);
            else
            {
                // copy from frame to out_buffer
                memcpy(output_buffer, sw_frame->data[0], sw_frame->linesize[0] * max_decoded_height);
            }
            av_packet_unref(&pkt);
            output_buffer += image_size;
            frame_count++;
        }
    } while (!end_of_stream);

    av_packet_unref(&pkt);
    av_frame_free(&dec_frame);
    av_frame_free(&sw_frame);
    sws_freeContext(swsctx);
    // release video_dec_context and fmt_context after each file decoding
    avformat_close_input(&_fmt_ctx);
    if (_io_ctx) av_freep(&_io_ctx->buffer);
    avio_context_free(&_io_ctx);
    avcodec_free_context(&_video_dec_ctx);
#endif    
    actual_decoded_width = max_decoded_width;
    actual_decoded_height = max_decoded_height;
    return status;
}

void HWJpegDecoder::release()
{
#ifdef USE_VCNDECODE_API
    // release all allocated buffers
#else
    avformat_close_input(&_fmt_ctx);
    _fmt_ctx = NULL;
    if (_io_ctx) av_freep(&_io_ctx->buffer);
    avio_context_free(&_io_ctx);
    _io_ctx = NULL;
    if (_video_dec_ctx) {
        avcodec_free_context(&_video_dec_ctx);
        _video_dec_ctx = NULL;
    }
    av_buffer_unref(&_hw_device_ctx);
#endif
}


HWJpegDecoder::~HWJpegDecoder() {
    release();
}

bool HWJpegDecoder::allocate_scaled_yuv_devMem(void **pScaledYUVdevMem, void **pScaledUdevMem, void **pScaledVdevMem, size_t &scaledYUVimageSize,
    uint32_t scaledLumaSize, uint32_t scaledYUVstride, uint32_t alignedScalingHeight) {

    hipError_t hipStatus = hipSuccess;
    uint32_t chromaHeight = 0;
    uint32_t chromaStride = 0;
    size_t chromaImageSize = 0;
    size_t scaledChromaImageSize = 0;

    switch (_out_imageinfo->chromaFormat) {
        case VCN_FMT_YUV420:        // corresponds to NV12
            scaledYUVimageSize = scaledYUVstride * (alignedScalingHeight + (alignedScalingHeight >> 1));
            chromaHeight = _out_imageinfo->nOutputVStride / 2;
            chromaStride = _out_imageinfo->nOutputHStride / 2;
            chromaImageSize = chromaStride * chromaHeight;
            scaledChromaImageSize = scaledYUVimageSize / 4;

            if (*pScaledYUVdevMem == nullptr) {
                hipStatus = hipMalloc(pScaledYUVdevMem, scaledYUVimageSize);
                if (hipStatus != hipSuccess) {
                    std::cout << "ERROR: hipMalloc failed to allocate the device memory for scaled YUV image!" << hipStatus << std::endl;
                    return false;
                }
            }
            if (*pScaledUdevMem == nullptr) {
                hipStatus = hipMalloc(pScaledUdevMem, scaledChromaImageSize);
                if (hipStatus != hipSuccess) {
                    std::cout << "ERROR: hipMalloc failed to allocate the device memory for scaled U image!" << hipStatus << std::endl;
                    return false;
                }
            }
            if (*pScaledVdevMem == nullptr) {
                hipStatus = hipMalloc(pScaledVdevMem, scaledChromaImageSize);
                if (hipStatus != hipSuccess) {
                    std::cout << "ERROR: hipMalloc failed to allocate the device memory for scaled V image!" << hipStatus << std::endl;
                    return false;
                }
            }
            break;
        case VCN_FMT_YUV400:
            if (*pScaledYUVdevMem == nullptr) {
                hipStatus = hipMalloc(pScaledYUVdevMem, scaledLumaSize);
                if (hipStatus != hipSuccess) {
                    std::cout << "ERROR: hipMalloc failed to allocate the device memory for scaled YUV image!" << hipStatus << std::endl;
                    return false;
                }
            }
            break;
        case VCN_FMT_YUV444:
            scaledYUVimageSize = scaledYUVstride * alignedScalingHeight * 3;
            if (*pScaledYUVdevMem == nullptr) {
                hipStatus = hipMalloc(pScaledYUVdevMem, scaledYUVimageSize);
                if (hipStatus != hipSuccess) {
                    std::cout << "ERROR: hipMalloc failed to allocate the device memory for scaled YUV image!" << hipStatus << std::endl;
                    return false;
                }
            }
            break;
        default:
            std::cout << "Error! " << _out_imageinfo->chromaFormat << " format is not supported!" << std::endl;
            return false;
        }

    return true;
}


// helper functions for scaling and color_format conversions
bool HWJpegDecoder::scale_yuv_image(void *pYUVdevMem, void *pUVdevMem, void *pScaledYUVdevMem, void *pScaledUVdevMem, size_t scaledYUVimageSize, 
                    uint32_t alignedScalingWidth, uint32_t alignedScalingHeight, uint32_t scaledYUVstride, uint32_t scaledLumaSize) {

    hipError_t hipStatus = hipSuccess;
    size_t lumaSize = _out_imageinfo->nOutputHStride * _out_imageinfo->nOutputHStride;
    uint32_t chromaWidth = 0;
    uint32_t chromaHeight = 0;
    uint32_t chromaStride = 0;
    size_t chromaImageSize = 0;
    size_t scaledChromaImageSize = 0;
    uint32_t uOffset = 0;

    switch (_out_imageinfo->chromaFormat) {
        case VCN_FMT_YUV420:
            chromaWidth = _out_imageinfo->nOutputWidth / 2;
            chromaHeight = _out_imageinfo->nOutputVStride / 2;
            chromaStride = _out_imageinfo->nOutputHStride / 2;
            chromaImageSize = chromaStride * chromaHeight;
            scaledChromaImageSize = scaledYUVimageSize / 4;

            //scale the Y, U, and V components of the NV12 image
            HipExec_ScaleImage_NV12_Nearest(hipStream, alignedScalingWidth, alignedScalingHeight, reinterpret_cast<uint8_t *>(pScaledYUVdevMem), scaledYUVstride,
                _out_imageinfo->nOutputWidth, _out_imageinfo->nOutputHeight, (const uint8_t *)pYUVdevMem, _out_imageinfo->nOutputHStride,
                reinterpret_cast<uint8_t *>(pScaledUVdevMem), (const uint8_t *)pUVdevMem);

            break;
        case VCN_FMT_YUV400:
            // if the surface format is YUV400, then there is only one Y component to scale
            HipExec_ScaleImage_U8_U8_Nearest(hipStream, alignedScalingWidth, alignedScalingHeight, (uint8_t *)pScaledYUVdevMem, scaledYUVstride,
                _out_imageinfo->nOutputWidth, _out_imageinfo->nOutputHeight, (const uint8_t *)pYUVdevMem, _out_imageinfo->nOutputHStride);

            break;
        case VCN_FMT_YUV444:
            uint32_t uOffset_src = _out_imageinfo->nOutputVStride * _out_imageinfo->nOutputHStride;
            uOffset = alignedScalingWidth * alignedScalingHeight;
            HipExec_ScaleImage_YUV444_Nearest(hipStream, alignedScalingWidth, alignedScalingHeight,
                (uint8_t *)pScaledYUVdevMem, scaledYUVstride, uOffset,
                _out_imageinfo->nOutputWidth, _out_imageinfo->nOutputHeight, (const uint8_t *)pYUVdevMem,
                _out_imageinfo->nOutputHStride, uOffset_src);

            break;
        default:
            std::cout << "Error! " << _out_imageinfo->chromaFormat << " format is not supported!" << std::endl;
            return false;
        }

        hipStatus = hipStreamSynchronize(hipStream);
        if (hipStatus != hipSuccess) {
            std::cout << "ERROR: hipStreamSynchronize failed! (" << hipStatus << ")" << std::endl;
            return false;
        }

    return true;
}

bool HWJpegDecoder::allocateDevMemForRGBConversion(uint8_t **pRGBdevMem, uint32_t alignedHeight, uint32_t YUVstride) {

    hipError_t hipStatus = hipSuccess;
    size_t rgbImageStride = YUVstride * 3;

    switch (_out_imageinfo->chromaFormat) {
        case VCN_FMT_YUV420:
        case VCN_FMT_YUV444:
        case VCN_FMT_YUV400:
            //allocate HIP device memory for RGB frame
            if (*pRGBdevMem == nullptr) {
                size_t rgbImageSize =  alignedHeight * rgbImageStride;
                hipStatus = hipMalloc(pRGBdevMem, rgbImageSize);
                if (hipStatus != hipSuccess) {
                    std::cout << "ERROR: hipMalloc failed!" << hipStatus << std::endl;
                    return false;
                }
            }
            break;
        default:
            std::cout << "Error! " << _out_imageinfo->chromaFormat << " format is not supported!" << std::endl;
            return false;
    }

    return true;
}

bool HWJpegDecoder::colorConvertYUVtoRGB(void *pYUVdevMem,  uint8_t *pRGBdevMem, uint32_t dstWidth, uint32_t dstHeight, uint32_t lumaSize, uint32_t lumaStride, uint32_t chromaStride, hipStream_t &hipStream) {

    hipError_t hipStatus = hipSuccess;
    size_t rgbImageStride = lumaStride * 3;

    switch (_out_imageinfo->chromaFormat) {
        case VCN_FMT_YUV420:
            HipExec_ColorConvert_NV12_to_RGB(hipStream, dstWidth, dstHeight, static_cast<uint8_t *>(pRGBdevMem), rgbImageStride,
            static_cast<const uint8_t *>(pYUVdevMem), lumaStride, (const uint8_t *)(pYUVdevMem) + lumaSize, chromaStride);
            break;
        case VA_FOURCC_444P:
                HipExec_ColorConvert_YUV444_to_RGB(hipStream, dstWidth, dstHeight, (uint8_t *)pRGBdevMem, rgbImageStride,
                    (const uint8_t *)pYUVdevMem, lumaStride, lumaSize);
            break;
        case VCN_FMT_YUV400:
            // if the surface format is YUV400, then there is only one Y component to scale
            HipExec_ColorConvert_YUV800_to_RGB(hipStream, dstWidth, dstHeight, (uint8_t *)pRGBdevMem, rgbImageStride,
                (const uint8_t *)pYUVdevMem, lumaStride);

            break;
        default:
            std::cout << "Error! " << _out_imageinfo->chromaFormat << " format is not supported!" << std::endl;
            return false;
    }

    hipStatus = hipStreamSynchronize(hipStream);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipStreamSynchronize failed! (" << hipStatus << ")" << std::endl;
        return false;
    }

    return true;

  }



#endif