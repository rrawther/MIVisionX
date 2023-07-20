/*
Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
*/

#include "vcndecode.hpp"
#include "scale_colconvert.hpp"


// ffmpeg helper functions for custom AVIOContex for bitstream reading
static int ReadFunc(void* ptr, uint8_t* buf, int buf_size) {
    ioBufferData *bd = (ioBufferData *)ptr;
    buf_size = FFMIN(buf_size, bd->size);

    if (!buf_size)
        return AVERROR_EOF;

    /* copy internal buffer data to buf */
    memcpy(buf, bd->ptr, buf_size);
    bd->ptr  += buf_size;
    bd->size -= buf_size;

    return buf_size;
}


VCNDecode::VCNDecode(int deviceID) : filename_{""}, numDevices_{0}, deviceID_ {deviceID}, hipStream_ {0},
                     av_stream_{-1}, av_fmt_input_ctx_{nullptr}, av_decoder_{nullptr}, av_decoder_ctx_{nullptr},
                     av_bufref_hw_dev_ctx_{nullptr}, frame_{nullptr}, packet_{{}}, va_display_{nullptr}, va_surfaceID_{0},
                     vaDrmPrimeSurfaceDesc_{{}}, isJPEGHWDecoderSuppoerted_{false}, va_profiles_{{}}, num_va_profiles_{-1},
                     externalMemoryHandleDesc_{{}}, externalMemBufferDesc_{{}}, pYUVdevMem_{nullptr}{

    if (!initHIP(deviceID_)) {
        std::cerr << "Failed to initilize the HIP" << std::endl;
        throw std::runtime_error("Failed to initilize the HIP");
    }

    initDRMnodes();
}

VCNDecode::~VCNDecode() {
    if (hipStream_) {
        hipError_t hipStatus = hipSuccess;
        hipStatus = hipStreamDestroy(hipStream_);
        if (hipStatus != hipSuccess) {
            std::cout << "ERROR: hipStream_Destroy failed! (" << hipStatus << ")" << std::endl;
        }
    }
    if (av_decoder_ctx_) {
        avcodec_free_context(&av_decoder_ctx_);
        av_decoder_ctx_ = nullptr;
    }
    if (av_fmt_input_ctx_) {
        avformat_close_input(&av_fmt_input_ctx_);
        av_fmt_input_ctx_ = nullptr;
    }
    if (av_bufref_hw_dev_ctx_) {
        av_buffer_unref(&av_bufref_hw_dev_ctx_);
        av_bufref_hw_dev_ctx_ = nullptr;
        va_display_ = nullptr;
    }
    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }
}

bool VCNDecode::initHIP(int deviceID) {
    hipError_t hipStatus = hipSuccess;
    hipStatus = hipGetDeviceCount(&numDevices_);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipGetDeviceCount failed! (" << hipStatus << ")" << std::endl;
        return false;
    }
    if (numDevices_ < 1) {
        std::cout << "ERROR: didn't find any GPU!" << std::endl;
        return false;
    }
    if (deviceID >= numDevices_) {
        std::cout << "ERROR: the requested deviceID is not found! " << std::endl;
        return false;
    }
    hipStatus = hipSetDevice(deviceID);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipSetDevice(" << deviceID << ") failed! (" << hipStatus << ")" << std::endl;
        return false;
    }

    hipStatus = hipGetDeviceProperties(&hipDevProp_, deviceID);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipGetDeviceProperties for device (" << deviceID << ") failed! (" << hipStatus << ")" << std::endl;
        return false;
    }

    hipStatus = hipStreamCreate(&hipStream_);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipStream_Create failed! (" << hipStatus << ")" << std::endl;
        return false;
    }
    return true;
}

void VCNDecode::initDRMnodes() {
    // build the DRM render node names
    for (int i = 0; i < numDevices_; i++) {
        drmNodes_.push_back("/dev/dri/renderD" + std::to_string(128 + i));
    }
}

bool VCNDecode::initHWDevCtx() {

    if (av_bufref_hw_dev_ctx_ == nullptr) {
        if (av_hwdevice_ctx_create(&av_bufref_hw_dev_ctx_, AV_HWDEVICE_TYPE_VAAPI, drmNodes_[deviceID_].c_str(), NULL, 0) < 0) {
            std::cerr << "ERROR: av_hwdevice_ctx_create failed!" << std::endl;
            return false;
        }
        AVHWDeviceContext *av_hw_dev_ctx_ = reinterpret_cast<AVHWDeviceContext *>(av_bufref_hw_dev_ctx_->data);
        AVVAAPIDeviceContext *av_vaapi_dev_ctx_ = reinterpret_cast<AVVAAPIDeviceContext *>(av_hw_dev_ctx_->hwctx);
        va_display_ = av_vaapi_dev_ctx_->display;
    }

    return true;
}

bool VCNDecode::queryVaProfiles() {
    if (va_display_ == nullptr) {
        if (!initHWDevCtx()) {
            return false;
        }
    }
    if (num_va_profiles_ > -1) {
        //already queried available profiles
        return true;
    }

    num_va_profiles_ = vaMaxNumProfiles(va_display_);
    vaQueryConfigProfiles(va_display_, va_profiles_, &num_va_profiles_);
    for (int i = 0; i < num_va_profiles_; ++i) {
        if (va_profiles_[i] == VAProfileJPEGBaseline) {
            isJPEGHWDecoderSuppoerted_ = true;
            break;
        }
    }
    return true;
}

bool VCNDecode::openInputFile(const std::string filePath) {

    if (av_fmt_input_ctx_) {
        avformat_close_input(&av_fmt_input_ctx_);
    }

    if (avformat_open_input(&av_fmt_input_ctx_, filePath.c_str(), NULL, NULL) != 0) {
        std::cerr << "ERROR: avformat_open_input failed!\nplease make sure that the input file path is correct!" << std::endl;
        return false;
    }
    if (avformat_find_stream_info(av_fmt_input_ctx_, NULL) < 0) {
        std::cerr << "ERROR: avformat_find_stream_info failed!" << std::endl;
        return false;
    }
    av_stream_ = av_find_best_stream(av_fmt_input_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &av_decoder_, 0);
    if (av_stream_ < 0) {
        std::cerr << "ERROR: av_find_best_stream failed!" << std::endl;
        return false;
    }
    if (av_decoder_ctx_ == nullptr) {
        av_decoder_ctx_ = avcodec_alloc_context3(av_decoder_);
        if (!av_decoder_ctx_) {
            std::cerr << "ERROR: avcodec_alloc_context3 failed!" << std::endl;
            return false;
        }
    }
    if (avcodec_parameters_to_context(av_decoder_ctx_, av_fmt_input_ctx_->streams[av_stream_]->codecpar) < 0) {
        std::cerr << "ERROR: avcodec_parameters_to_context failed!" << std::endl;
        return false;
    }

    return true;
}

// helper to open input using io_context
bool VCNDecode::openInputStream(uint8_t *pData, int nSize, int* width, int* height) {

    io_buffer_data_ = { 0 };

    av_log_set_level(AV_LOG_QUIET);

    if (avio_ctx_buffer_) {
        av_free(avio_ctx_buffer_);
        avio_ctx_buffer_ = nullptr;
    }
    if (io_ctx_) {
        avio_context_free(&io_ctx_);
        io_ctx_ = nullptr;
    }

    if (av_fmt_input_ctx_) {
        avformat_close_input(&av_fmt_input_ctx_);
        av_fmt_input_ctx_ = nullptr;
    }

    if (!(av_fmt_input_ctx_ = avformat_alloc_context())) {
        std::cerr << "ERROR: avformat_alloc_context failed!" << std::endl;
        return false;
    }

    avio_ctx_buffer_ = (uint8_t *)av_malloc(AVIO_CONTEXT_BUF_SIZE);
    if (!avio_ctx_buffer_) {
        std::cerr << "ERROR: avio_ctx_buffer_ allocation failed!" << std::endl;
        return false;
    }
    io_ctx_ = avio_alloc_context(avio_ctx_buffer_, AVIO_CONTEXT_BUF_SIZE,
                                0, &io_buffer_data_, &ReadFunc, NULL, NULL);
    if (!io_ctx_) {
        std::cerr << "ERROR: avio_alloc_context failed!" << std::endl;
        return false;
    }
    
    io_buffer_data_.ptr  = (uint8_t *)pData;
    io_buffer_data_.size = nSize;
    av_fmt_input_ctx_->pb = io_ctx_;
    av_fmt_input_ctx_->flags |= AVFMT_FLAG_CUSTOM_IO;

    if (avformat_open_input(&av_fmt_input_ctx_, NULL, NULL, NULL) < 0) {
        std::cerr << "ERROR: avformat_open_input failed!\nplease make sure that the input file path is correct!" << std::endl;
        return false;
    }
    if (avformat_find_stream_info(av_fmt_input_ctx_, NULL) < 0) {
        std::cerr << "ERROR: avformat_find_stream_info failed!" << std::endl;
        return false;
    }

    av_stream_ = av_find_best_stream(av_fmt_input_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, &av_decoder_, 0);
    if (av_stream_ < 0) {
        std::cerr << "ERROR: av_find_best_stream failed!" << std::endl;
        return false;
    }

    if (av_decoder_ctx_ == nullptr) {
        av_decoder_ctx_ = avcodec_alloc_context3(av_decoder_);
        if (!av_decoder_ctx_) {
            std::cerr << "ERROR: avcodec_alloc_context3 failed!" << std::endl;
            return false;
        }
    }
    if (avcodec_parameters_to_context(av_decoder_ctx_, av_fmt_input_ctx_->streams[av_stream_]->codecpar) < 0) {
        std::cerr << "ERROR: avcodec_parameters_to_context failed!" << std::endl;
        return false;
    }

    nWidth_ = av_decoder_ctx_->width;
    nHeight_ = av_decoder_ctx_->height;
    nComponents_ = av_pix_fmt_count_planes(av_decoder_ctx_->pix_fmt);
    eVideoCodec_ = av_fmt_input_ctx_->streams[av_stream_]->codecpar->codec_id;

    switch (av_decoder_ctx_->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            subsampling_ = VCN_FMT_YUV420;
            break;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            subsampling_ = VCN_FMT_YUV444;
            break;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
            subsampling_ = VCN_FMT_YUV422;
            break;
        case AV_PIX_FMT_GRAY8:
            subsampling_ = VCN_FMT_YUV400;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            subsampling_ = VCN_FMT_YUV420P10;
            nBitDepth_ = 10;
            nBPP_ = 2;
            break;
        case AV_PIX_FMT_YUV420P12LE:
            subsampling_ = VCN_FMT_YUV420P12;
            nBitDepth_ = 12;
            nBPP_ = 2;
            break;

        default:
            std::cerr << "ERROR: " << av_get_pix_fmt_name(av_decoder_ctx_->pix_fmt) << " subsampling format is not supported!" << std::endl;
            return false;
    }

    if (!getImageSizeHintInternal(subsampling_, nWidth_, nHeight_, &nSurfaceStride_, &nSurfaceSize_)) {
        std::cerr << "failed to get the frame size!" << std::endl;
    }
    *width = nWidth_;
    *height = nHeight_;

    return true;
}


bool VCNDecode::openCodec() {
    if (av_decoder_ctx_ == nullptr) {
        return false;
    }
    av_decoder_ctx_->get_format = get_hw_format;
    av_decoder_ctx_->hw_device_ctx = av_buffer_ref(av_bufref_hw_dev_ctx_);
    if (avcodec_open2(av_decoder_ctx_, av_decoder_, NULL) < 0) {
        std::cerr << "ERROR: avcodec_open2 failed!" << std::endl;
        return false;
    }
    nSurfaceWidth_ = av_decoder_ctx_->coded_width;
    nSurfaceHeight_ = av_decoder_ctx_->coded_height;

    return true;
}

bool VCNDecode::initFrame() {
    if (frame_ == nullptr) {
        frame_ = av_frame_alloc();
        if (!frame_) {
            std::cerr << "ERROR: av_frame_alloc failed!" << std::endl;
            return false;
        }
    }
    return true;
}

enum AVPixelFormat VCNDecode::get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    (void)ctx, (void)pix_fmts;
    return AV_PIX_FMT_VAAPI;
}

bool VCNDecode::getImageSizeHintInternal(vcnImageFormat_t subsampling, const uint32_t width, const uint32_t height, uint32_t *output_stride, size_t *output_image_size) {

    if ( output_stride == nullptr || output_image_size == nullptr) {
        std::cerr << "invalid input parameters!" << std::endl;
        return false;
    }
    int alignedHeight = 0;
    switch (subsampling) {
        case VCN_FMT_YUV420:
            *output_stride = align(width, 256);
            alignedHeight = align(height, 16);
            *output_image_size = *output_stride * (alignedHeight + (alignedHeight >> 1));
             break;
        case VCN_FMT_YUV444:
            *output_stride = align(width, 256);
            alignedHeight = align(height, 16);
            *output_image_size = *output_stride * alignedHeight * 3;
            break;
        case VCN_FMT_YUV400:
            *output_stride = align(width, 256);
            alignedHeight = align(height, 16);
            *output_image_size = *output_stride * alignedHeight;
            break;
        case VCN_FMT_RGB:
            *output_stride = align(width, 256) * 3;
            alignedHeight = align(height, 16);
            *output_image_size = *output_stride * alignedHeight;
            break;
        case VCN_FMT_YUV420P10:
            *output_stride = align(width, 128) * 2;
            alignedHeight = align(height, 16);
            *output_image_size = *output_stride * (alignedHeight + (alignedHeight >> 1));
            break;
        case VCN_FMT_YUV420P12:
            *output_stride = align(width, 128) * 2;
            alignedHeight = align(height, 16);
            *output_image_size = *output_stride * (alignedHeight + (alignedHeight >> 1));
            break;
        default:
            std::cerr << "ERROR: "<< getPixFmtName(subsampling) <<" is not supported! " << std::endl;
            return false;
    }

    return true;
}

bool VCNDecode::isJPEGHWDecoderSupported() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initHWDevCtx()) {
        std::cerr << "ERROR: initialization of the HW device context creation failed!" << std::endl;
        return false;
    }
    queryVaProfiles();
    return isJPEGHWDecoderSuppoerted_;
}

bool VCNDecode::getImageInfoFile(const std::string filePath, uint8_t *nComponents, vcnImageFormat_t &subsampling, uint32_t *width, uint32_t *height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (width == nullptr || height == nullptr || nComponents == nullptr) {
        return false;
    }

    if (!openInputFile(filePath)) {
        std::cerr << "ERROR: opening the input file failed!" << std::endl;
        return false;
    }

    if (av_decoder_ctx_ == nullptr) {
        return false;
    }
    *width = av_decoder_ctx_->width;
    *height = av_decoder_ctx_->height;
    *nComponents = av_pix_fmt_count_planes(av_decoder_ctx_->pix_fmt);

    switch (av_decoder_ctx_->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            subsampling = VCN_FMT_YUV420;
            break;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            subsampling = VCN_FMT_YUV444;
            break;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
            subsampling = VCN_FMT_YUV422;
            break;
        case AV_PIX_FMT_GRAY8:
            subsampling = VCN_FMT_YUV400;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            subsampling = VCN_FMT_YUV420P10;
            break;
        case AV_PIX_FMT_YUV420P12LE:
            subsampling = VCN_FMT_YUV420P12;
            break;
        default:
            std::cerr << "ERROR: " << av_get_pix_fmt_name(av_decoder_ctx_->pix_fmt) << " subsampling format is not supported!" << std::endl;
            return false;
    }
    return true;
}

bool VCNDecode::getImageInfo(unsigned char* input_buffer, size_t input_size, uint8_t *nComponents, vcnImageFormat_t &subsampling, uint32_t *width, uint32_t *height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (width == nullptr || height == nullptr || nComponents == nullptr) {
        return false;
    }

    if (!openInputStream(input_buffer, input_size)) {
        std::cerr << "ERROR: opening the input file failed!" << std::endl;
        return false;
    }

    if (av_decoder_ctx_ == nullptr) {
        return false;
    }
    *width = av_decoder_ctx_->width;
    *height = av_decoder_ctx_->height;
    *nComponents = av_pix_fmt_count_planes(av_decoder_ctx_->pix_fmt);

    switch (av_decoder_ctx_->pix_fmt) {
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            subsampling = VCN_FMT_YUV420;
            break;
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUVJ444P:
            subsampling = VCN_FMT_YUV444;
            break;
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUVJ422P:
            subsampling = VCN_FMT_YUV422;
            break;
        case AV_PIX_FMT_GRAY8:
            subsampling = VCN_FMT_YUV400;
            break;
        case AV_PIX_FMT_YUV420P10LE:
            subsampling = VCN_FMT_YUV420P10;
            break;
        case AV_PIX_FMT_YUV420P12LE:
            subsampling = VCN_FMT_YUV420P12;
            break;
        default:
            std::cerr << "ERROR: " << av_get_pix_fmt_name(av_decoder_ctx_->pix_fmt) << " subsampling format is not supported!" << std::endl;
            return false;
    }
    return true;
}


bool VCNDecode::getImageSizeHint(vcnImageFormat_t subsampling, const uint32_t output_width, const uint32_t output_height, uint32_t *output_stride, size_t *output_image_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    return getImageSizeHintInternal(subsampling, output_width, output_height, output_stride, output_image_size);
}

bool VCNDecode::decode(const std::string filePath, void *output_image, const size_t output_image_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initHWDevCtx()) {
        std::cerr << "ERROR: initialization of the HW device context creation failed!" << std::endl;
        return false;
    }

    if (!openInputFile(filePath)) {
        std::cerr << "ERROR: opening the input file failed!" << std::endl;
        return false;
    }

    if (!openCodec()) {
        std::cerr << "ERROR: open codedc failed!" << std::endl;
        return false;
    }

    if (!initFrame()) {
        std::cerr << "ERROR: init frame failed!" << std::endl;
        return false;
    }

    bool isPacketValid = false;
    bool getNewPacket = true;
    int frameCount = 0;
    VAStatus vastatus;

    hipError_t hipStatus = hipSuccess;

    while (true) {
        // Unreference the buffer referenced by the packet for reusing
        if (isPacketValid) {
            av_packet_unref(&packet_);
            isPacketValid = false;
        }

        // Read the next frame of a stream
        if (getNewPacket) {
            if (av_read_frame(av_fmt_input_ctx_, &packet_) < 0) {
                // Reached end of stream
                break;
            }
            isPacketValid = true;
            if (packet_.stream_index != av_stream_) {
                // The current packet is not a video packet
                continue;
            }
            // Supply raw packet data as input to a decoder
            if (avcodec_send_packet(av_decoder_ctx_, &packet_) < 0) {
                std::cout << "ERROR: avcodec_send_packet failed!" << std::endl;
                return false;
            }
            getNewPacket = false;
        }

        // Return decoded output data from a decoder
        int ret = avcodec_receive_frame(av_decoder_ctx_, frame_);
        if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
            getNewPacket = true;
            // There are no more frames ready from the decoder; therfore, request to decode new frames
            continue;
        } else if (ret < 0) {
            std::cerr << "ERROR: avcodec_receive_frame failed!" << std::endl;
            return false;
        }

        // Export a handle to a surface for use with the HIP APIs
        va_surfaceID_ = (uintptr_t)frame_->data[3];

        vastatus = vaSyncSurface(va_display_, va_surfaceID_);
        if (vastatus != VA_STATUS_SUCCESS) {
            std::cerr << "ERROR: vaSyncSurface failed! " << AVERROR(vastatus) << std::endl;
            return false;
        }

        vastatus = vaExportSurfaceHandle(va_display_, va_surfaceID_,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY |
            VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &vaDrmPrimeSurfaceDesc_);

        if (vastatus != VA_STATUS_SUCCESS) {
            std::cerr << "ERROR: vaExportSurfaceHandle failed! " << AVERROR(vastatus) << std::endl;
            return false;
        }

        // import the frame (DRM-PRIME FDs) into the HIP
        externalMemoryHandleDesc_.type = hipExternalMemoryHandleTypeOpaqueFd;
        externalMemoryHandleDesc_.handle.fd = vaDrmPrimeSurfaceDesc_.objects[0].fd;
        externalMemoryHandleDesc_.size = vaDrmPrimeSurfaceDesc_.objects[0].size;

        hipStatus = hipImportExternalMemory(&hipExtMem_, &externalMemoryHandleDesc_);
        if (hipStatus != hipSuccess) {
            std::cerr << "ERROR: hipImportExternalMemory failed! (" << hipStatus << ")" << std::endl;
            return false;
        }

        externalMemBufferDesc_.offset = 0;
        externalMemBufferDesc_.size = vaDrmPrimeSurfaceDesc_.objects[0].size;
        externalMemBufferDesc_.flags = 0;
        hipStatus = hipExternalMemoryGetMappedBuffer(&pYUVdevMem_, hipExtMem_, &externalMemBufferDesc_);
        if (hipStatus != hipSuccess) {
            std::cerr << "ERROR: hipExternalMemoryGetMappedBuffer failed! (" << hipStatus << ")" << std::endl;
            return false;
        }

        if (output_image_size == externalMemBufferDesc_.size) {
            hipStatus = hipMemcpyDtoDAsync(output_image, pYUVdevMem_, output_image_size, hipStream_);
            if (hipStatus != hipSuccess) {
                std::cerr << "ERROR: hipMemcpyDtoDAsync failed! (" << hipStatus << ")" << std::endl;
                return false;
            }
            hipStatus = hipStreamSynchronize(hipStream_);
            if (hipStatus != hipSuccess) {
                std::cerr << "ERROR: hipStreamSynchronize failed! (" << hipStatus << ")" << std::endl;
                return false;
            }
        } else {
            std::cerr << "the provided output_image_size is wrong! expeting " << externalMemBufferDesc_.size << " but got " << output_image_size
                << std::endl;
            return false;
        }

        hipStatus = hipDestroyExternalMemory(hipExtMem_);
        if (hipStatus != hipSuccess) {
            std::cerr << "ERROR: hipDestroyExternalMemory failed! (" << hipStatus << ")" << std::endl;
            return false;
        }

        for (int i = 0; i < (int)vaDrmPrimeSurfaceDesc_.num_objects; ++i) {
            close(vaDrmPrimeSurfaceDesc_.objects[i].fd);
        }
    }

    if (isPacketValid) {
        av_packet_unref(&packet_);
    }

    return true;
}

bool VCNDecode::decode(uint8_t *pData, int nSize, int64_t pts) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!openInputStream(pData, nSize)) {
        std::cerr << "ERROR: opening the input file failed!" << std::endl;
        return 0;
    }

    if (!openCodec()) {
        std::cerr << "ERROR: open codedc failed!" << std::endl;
        return false;
    }

    if (!initFrame()) {
        std::cerr << "ERROR: init frame failed!" << std::endl;
        return false;
    }

    bool isPacketValid = false;
    bool getNewPacket = true;
    int frameCount = 0;
    VAStatus vastatus;

    hipError_t hipStatus = hipSuccess;

    while (true) {
        // Unreference the buffer referenced by the packet for reusing
        if (isPacketValid) {
            av_packet_unref(&packet_);
            isPacketValid = false;
        }

        // Read the next frame of a stream
        if (getNewPacket) {
            if (av_read_frame(av_fmt_input_ctx_, &packet_) < 0) {
                // Reached end of stream
                break;
            }
            isPacketValid = true;
            if (packet_.stream_index != av_stream_) {
                // The current packet is not a video packet
                continue;
            }
            // Supply raw packet data as input to a decoder
            if (avcodec_send_packet(av_decoder_ctx_, &packet_) < 0) {
                std::cout << "ERROR: avcodec_send_packet failed!" << std::endl;
                return false;
            }
            getNewPacket = false;
        }

        // Return decoded output data from a decoder
        int ret = avcodec_receive_frame(av_decoder_ctx_, frame_);
        if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
            getNewPacket = true;
            // There are no more frames ready from the decoder; therfore, request to decode new frames
            continue;
        } else if (ret < 0) {
            std::cerr << "ERROR: avcodec_receive_frame failed!" << std::endl;
            return false;
        }

        // Export a handle to a surface for use with the HIP APIs
        va_surfaceID_ = (uintptr_t)frame_->data[3];

        vastatus = vaSyncSurface(va_display_, va_surfaceID_);
        if (vastatus != VA_STATUS_SUCCESS) {
            std::cerr << "ERROR: vaSyncSurface failed! " << AVERROR(vastatus) << std::endl;
            return false;
        }
    }

    if (isPacketValid) {
        av_packet_unref(&packet_);
    }

    return true;
}

// scale YUV image
bool VCNDecode::scaleYUVimage(void *pYUVdevMem, void *pUdevMem, void *pVdevMem, void *pScaledYUVdevMem, void *pScaledUdevMem, void *pScaledVdevMem,
    size_t scaledYUVimageSize, uint32_t alignedScalingWidth, uint32_t alignedScalingHeight, uint32_t scaledYUVstride, uint32_t scaledLumaSize,
    VADRMPRIMESurfaceDescriptor &vaDrmPrimeSurfaceDesc, hipStream_t &hipStream) {

    hipError_t hipStatus = hipSuccess;
    size_t lumaSize = vaDrmPrimeSurfaceDesc.layers[0].pitch[0] * vaDrmPrimeSurfaceDesc.height;
    uint32_t chromaWidth = 0;
    uint32_t chromaHeight = 0;
    uint32_t chromaStride = 0;
    size_t chromaImageSize = 0;
    size_t scaledChromaImageSize = 0;
    uint32_t uOffset = 0;

    switch (vaDrmPrimeSurfaceDesc.fourcc) {
        case VA_FOURCC_NV12:
            chromaWidth = vaDrmPrimeSurfaceDesc.width / 2;
            chromaHeight = vaDrmPrimeSurfaceDesc.height / 2;
            chromaStride = vaDrmPrimeSurfaceDesc.layers[1].pitch[0] / 2;
            chromaImageSize = chromaStride * chromaHeight;
            scaledChromaImageSize = scaledYUVimageSize / 4;

            //extract the U and V components
            HipExec_ChannelExtract_U8U8_U16(hipStream, chromaWidth, chromaHeight,
                (uchar *)pUdevMem, (uchar *)pVdevMem, chromaStride,
                (const uchar *)pYUVdevMem + lumaSize, vaDrmPrimeSurfaceDesc.layers[1].pitch[0]);

            //scale the Y, U, and V components of the NV12 image
            HipExec_ScaleImage_NV12_Nearest(hipStream, alignedScalingWidth, alignedScalingHeight, (uchar *)pScaledYUVdevMem, scaledYUVstride,
                vaDrmPrimeSurfaceDesc.width, vaDrmPrimeSurfaceDesc.height, (const uchar *)pYUVdevMem, vaDrmPrimeSurfaceDesc.layers[0].pitch[0],
                (uchar *)pScaledUdevMem, (uchar *)pScaledVdevMem, (const uchar *)pUdevMem, (const uchar *)pVdevMem);

            // combine the scaled U and V components to the final scaled NV12 buffer
            HipExec_ChannelCombine_U16_U8U8(hipStream, alignedScalingWidth / 2, alignedScalingHeight / 2,
                (uchar *)pScaledYUVdevMem + scaledLumaSize, scaledYUVstride,
                (uchar *)pScaledUdevMem, scaledYUVstride / 2,
                (uchar *)pScaledVdevMem, scaledYUVstride / 2);

            break;
        case VA_FOURCC_Y800:
            // if the surface format is YUV400, then there is only one Y component to scale
            HipExec_ScaleImage_U8_U8_Nearest(hipStream, alignedScalingWidth, alignedScalingHeight, (uchar *)pScaledYUVdevMem, scaledYUVstride,
                vaDrmPrimeSurfaceDesc.width, vaDrmPrimeSurfaceDesc.height, (const uchar *)pYUVdevMem, vaDrmPrimeSurfaceDesc.layers[0].pitch[0]);

            break;
        case VA_FOURCC_444P:
            uOffset = alignedScalingWidth * alignedScalingHeight;
            HipExec_ScaleImage_YUV444_Nearest(hipStream, alignedScalingWidth, alignedScalingHeight,
                (uchar *)pScaledYUVdevMem, scaledYUVstride, uOffset,
                vaDrmPrimeSurfaceDesc.width, vaDrmPrimeSurfaceDesc.height, (const uchar *)pYUVdevMem,
                vaDrmPrimeSurfaceDesc.layers[0].pitch[0], vaDrmPrimeSurfaceDesc.layers[1].offset[0]);

            break;
        default:
            std::cout << "Error! " << vaDrmPrimeSurfaceDesc.fourcc << " format is not supported!" << std::endl;
            return false;
        }

        hipStatus = hipStreamSynchronize(hipStream);
        if (hipStatus != hipSuccess) {
            std::cout << "ERROR: hipStreamSynchronize failed! (" << hipStatus << ")" << std::endl;
            return false;
        }

    return true;
}

// YUV to RGB format conversion
bool VCNDecode::colorConvertYUVtoRGB(void *pYUVdevMem, void *pScaledYUVdevMem, uint8_t *pRGBdevMem, uint32_t lumaSize, uint32_t alignedLumaWidth, 
                        uint32_t alignedLumaHeight, uint32_t lumastride, vcnImageFormat_t subsampling, hipStream_t &hipStream) {

    hipError_t hipStatus = hipSuccess;
    //size_t lumaSize = isScaling ? scaledLumaSize : (vaDrmPrimeSurfaceDesc.layers[0].pitch[0] * vaDrmPrimeSurfaceDesc.height);
    size_t rgbImageStride = ALIGN16(lumastride * 3);

    switch (subsampling) {
        case VCN_FMT_YUV420:
            HipExec_ColorConvert_NV12_to_RGB(hipStream, alignedScalingWidth, alignedScalingHeight, (uchar *)pRGBdevMem, rgbImageStride,
            (const uchar *)pScaledYUVdevMem, scaledYUVstride, (const uchar *)pScaledYUVdevMem + scaledLumaSize, scaledYUVstride);
            break;
        case VCN_FMT_YUV444:
            HipExec_ColorConvert_YUV444_to_RGB(hipStream, alignedScalingWidth, alignedScalingHeight, (uchar *)pRGBdevMem, rgbImageStride,
                (const uchar *)pScaledYUVdevMem, scaledYUVstride, scaledLumaSize);
            break;
        case VCN_FMT_YUV400:
            // todo::
            break;
        default:
            std::cout << "Error! " << vaDrmPrimeSurfaceDesc.fourcc << " format is not supported!" << std::endl;
            return false;
    }

    hipStatus = hipStreamSynchronize(hipStream);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipStreamSynchronize failed! (" << hipStatus << ")" << std::endl;
        return false;
    }

    return true;
}


uint8_t* VideoDecode::getFrame(int64_t *pts) {
    if (!dec_frame_q_.empty()) {
        VAStatus vastatus;
        hipError_t hipStatus;
        int size = dec_frame_q_.size();
        decFrameBuffer *fb = dec_frame_q_.front();
        AVFrame *frame = fb->pFrame;
        va_surfaceID_ = (uintptr_t)frame->data[3];
        *pts = frame->pts;

        vastatus = vaExportSurfaceHandle(va_display_, va_surfaceID_,
            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
            VA_EXPORT_SURFACE_READ_ONLY |
            VA_EXPORT_SURFACE_SEPARATE_LAYERS,
            &fb->vaDrmPrimeSurfaceDesc);

        if (vastatus != VA_STATUS_SUCCESS) {
            std::cerr << "ERROR: vaExportSurfaceHandle failed! " << AVERROR(vastatus) << std::endl;
            return nullptr;
        }

        // import the frame (DRM-PRIME FDs) into the HIP
        externalMemoryHandleDesc_.type = hipExternalMemoryHandleTypeOpaqueFd;
        externalMemoryHandleDesc_.handle.fd = fb->vaDrmPrimeSurfaceDesc.objects[0].fd;
        externalMemoryHandleDesc_.size = fb->vaDrmPrimeSurfaceDesc.objects[0].size;

        hipStatus = hipImportExternalMemory(&fb->hipExtMem, &externalMemoryHandleDesc_);
        if (hipStatus != hipSuccess) {
            std::cerr << "ERROR: hipImportExternalMemory failed! (" << hipStatus << ")" << std::endl;
            return nullptr;
        }

        externalMemBufferDesc_.offset = 0;
        externalMemBufferDesc_.size = fb->vaDrmPrimeSurfaceDesc.objects[0].size;
        externalMemBufferDesc_.flags = 0;
        hipStatus = hipExternalMemoryGetMappedBuffer(&pYUVdevMem_, fb->hipExtMem, &externalMemBufferDesc_);
        if (hipStatus != hipSuccess) {
            std::cerr << "ERROR: hipExternalMemoryGetMappedBuffer failed! (" << hipStatus << ")" << std::endl;
            return nullptr;
        }
        return static_cast<uint8_t *>(pYUVdevMem_);
    } else {
        return nullptr;
    }
}

// release the frame with associated id
bool VideoDecode::releaseFrame(int64_t pTimestamp) {
    if (!dec_frame_q_.empty()) {
        int size = dec_frame_q_.size();

        decFrameBuffer *fb = dec_frame_q_.front();
        AVFrame *frame = fb->pFrame;

        if (pTimestamp != frame->pts) {
            std::cerr << "Frames is out of order" << std::endl;
            return false;
        }
        hipError_t hipStatus;
        hipStatus = hipDestroyExternalMemory(fb->hipExtMem);
        if (hipStatus != hipSuccess) {
            std::cerr << "ERROR: hipDestroyExternalMemory failed! (" << hipStatus << ")" << std::endl;
            return false;
        }

        for (int i = 0; i < (int)fb->vaDrmPrimeSurfaceDesc.num_objects; ++i) {
            close(fb->vaDrmPrimeSurfaceDesc.objects[i].fd);
        }
        av_frame_unref(frame);
        // pop decoded frame and unref
        dec_frame_q_.pop();
    }
    return true;
}


void VCNDecode::saveImage(std::string outputfileName, void* pdevMem, size_t outputImageSize, uint32_t imgWidth, uint32_t imgHeight, uint32_t outputImageStride,
    vcnImageFormat_t subsampling, bool isOutputRGB) {

    uint8_t *hstPtr = nullptr;
    FILE *fp;
    if (hstPtr == nullptr) {
        hstPtr = new uint8_t [outputImageSize];
    }
    hipError_t hipStatus = hipSuccess;
    hipStatus = hipMemcpyDtoH((void *)hstPtr, pdevMem, outputImageSize);
    if (hipStatus != hipSuccess) {
        std::cout << "ERROR: hipMemcpyDtoH failed! (" << hipStatus << ")" << std::endl;
        delete [] hstPtr;
        return;
    }

    // no RGB dump if the surface type is YUV400
    if (subsampling == VCN_FMT_YUV400 && isOutputRGB) {
        return;
    }
    uint8_t *tmpHstPtr = hstPtr;
    fp = fopen(outputfileName.c_str(), "wb");
    if (fp) {
        if (imgWidth == outputImageStride && imgHeight == align(imgHeight, 16)) {
            fwrite(hstPtr, 1, outputImageSize, fp);
        } else {
            uint32_t width = isOutputRGB ? imgWidth * 3 : imgWidth;
            for (int i = 0; i < imgHeight; i++) {
                fwrite(tmpHstPtr, 1, width, fp);
                tmpHstPtr += outputImageStride;
            }
        }
        fclose(fp);
    }

    if (hstPtr != nullptr) {
        delete [] hstPtr;
        hstPtr = nullptr;
        tmpHstPtr = nullptr;
    }

}

void VCNDecode::getDeviceinfo(std::string &deviceName, std::string &gcnArchName, int &pciBusID, int &pciDomainID,
    int &pciDeviceID, std::string &drmNode) {
    deviceName = hipDevProp_.name;
    gcnArchName = hipDevProp_.gcnArchName;
    pciBusID = hipDevProp_.pciBusID;
    pciDomainID = hipDevProp_.pciDomainID;
    pciDeviceID = hipDevProp_.pciDeviceID;
    drmNode = drmNodes_[deviceID_];
}

std::string VCNDecode::getPixFmtName(vcnImageFormat_t subsampling) {
    std::string fmtName = "";
    switch (subsampling) {
        case VCN_FMT_YUV420:
            fmtName = "YUV420";
            break;
        case VCN_FMT_YUV444:
            fmtName = "YUV444";
            break;
        case VCN_FMT_YUV422:
            fmtName = "YUV422";
            break;
        case VCN_FMT_YUV400:
            fmtName = "YUV400";
            break;
       case VCN_FMT_YUV420P10:
            fmtName = "YUV420P10";
            break;
       case VCN_FMT_YUV420P12:
            fmtName = "YUV420P12";
            break;
        case VCN_FMT_RGB:
            fmtName = "RGB";
            break;
        default:
            std::cerr << "ERROR: subsampling format is not supported!" << std::endl;
    }
    return fmtName;
}

// api to get output image info
bool VideoDecode::getOutputImageInfo(outputImageInfo **pImageInfo) {
    if (!nWidth_ || !nHeight_) {
        std::cerr << "ERROR: Videodeocder is not intialized" << std::endl;
        return false;
    }
    *pImageInfo = &outImageInfo_;
    return true;
}

