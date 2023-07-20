/*
Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
*/

#include "../include/vcndecode.hpp"

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

// implementation of the VideoDecode
VideoDecode::VideoDecode(int deviceID) : VCNDecode(deviceID) {}

VideoDecode::~VideoDecode() {
    // close output file
    if (fp_out_) {
        fclose(fp_out_);
        fp_out_ = nullptr;
    }
    // release all resourses for the decoder
    if (io_ctx_) {
        avio_context_free(&io_ctx_);
        io_ctx_ = nullptr;
    }
    if (av_fmt_input_ctx_) {
        avformat_close_input(&av_fmt_input_ctx_);
        av_fmt_input_ctx_ = nullptr;
    }
    if (!decBufferPool_.empty()) {
      for(auto i = 0; i < decBufferPool_.size(); i++) {
         av_frame_free(&decBufferPool_[i]->pFrame);
      }
      decBufferPool_.clear();
    }
}


// helper to open input using io_context
bool VideoDecode::openInputStream(uint8_t *pData, int nSize) {

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

    return true;
}

int VideoDecode::decode(uint8_t *pData, int nSize, int64_t pts, int nFlags) {
    // for the first packet, initialize the ffmpeg decoder_ctx for decoding
    // open input from bitstream ptr if not opened : will be true for the first packet in the stream
    if (av_decoder_ctx_ == nullptr) {
     
        if (!initHWDevCtx()) {
            std::cerr << "ERROR: initialization of the HW device context creation failed!" << std::endl;
            return 0;
        }
        if (!openInputStream(pData, nSize)) {
            std::cerr << "ERROR: opening the input file failed!" << std::endl;
            return 0;
        }

        if (!openCodec()) {
            std::cerr << "ERROR: open codedc failed!" << std::endl;
            return 0;
        }
        // alloc AVFrame pool for decoding
        for (size_t i = 0; i< NUM_BUFFERS_IN_POOL; i++) {
            decFrameBuffer *decBuff = new decFrameBuffer;
            AVFrame *pFrame = av_frame_alloc();
            if (!pFrame) {
                std::cerr << "ERROR: av_frame_alloc failed!" << std::endl;
                return 0;
            }
            decBuff->pFrame = pFrame;
            decBuff->hipExtMem = nullptr;
            decBuff->vaDrmPrimeSurfaceDesc = {};
            decBufferPool_.push_back(decBuff);
        }
        // Initialize outImageInfo_ struct
        outImageInfo_.nOutputWidth = nWidth_;
        outImageInfo_.nOutputHeight = nHeight_;
        outImageInfo_.nOutputHStride = nSurfaceStride_;
        outImageInfo_.nOutputVStride = align(nHeight_, 16);
        outImageInfo_.nBytesPerPixel = nBPP_;
        outImageInfo_.nBitDepth = nBitDepth_;
        outImageInfo_.lOutputImageSizeInBytes = nSurfaceSize_;
        outImageInfo_.chromaFormat = subsampling_;
    }

    VAStatus vastatus;
    nDecodedFrame_ = 0;
    bool eos = false;
    //package input data into packet
    packet_ = {0};
    //initialize valid packet
    if (nSize){
      packet_.stream_index = av_stream_;
      packet_.data = pData;
      packet_.size = nSize;
      packet_.pts  = pts;
      packet_.duration = 0; // unknown
      packet_.pos = -1;
    }

    while (!nDecodedFrame_ || eos ) {
        int ret;
        // Supply raw packet data as input to a decoder
        if (!eos && ((ret = avcodec_send_packet(av_decoder_ctx_, &packet_)) < 0)) {
            // flush last frames and return multiple frames if possible
            if (ret == AVERROR_EOF) {
                // trigger flush for the remainder of frames
                eos = true;
                av_packet_unref(&packet_);
                continue;
            } else {
              std::cout << "ERROR: avcodec_send_packet failed!" << std::endl;
              av_packet_unref(&packet_);    // invalid packet
              return nDecodedFrame_;
            }
        }
        decFrameBuffer *fb = decBufferPool_[nDecodedFrame_];
        AVFrame *frame = fb->pFrame;

        // Return decoded output data from a decoder
        ret = avcodec_receive_frame(av_decoder_ctx_, frame);
        if ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) {
            // There are no more frames ready from the decoder; therfore, request to decode new frames
            // check if EOS received. Then we need to flush
            if (ret == AVERROR_EOF) {
                lastFrameReceived_ = true;
            }
            break;
        } else if (ret < 0) {
            std::cerr << "ERROR: avcodec_receive_frame failed!" << std::endl;
            break;
        }
        // make sure the surface is synced before returing to HIP
        va_surfaceID_ = (uintptr_t)frame->data[3];

        vastatus = vaSyncSurface(va_display_, va_surfaceID_);
        if (vastatus != VA_STATUS_SUCCESS) {
            std::cerr << "ERROR: vaSyncSurface failed! " << AVERROR(vastatus) << std::endl;
            return 0;
        }
        fb->pFrame = frame;
        dec_frame_q_.push(fb);
        av_packet_unref(&packet_);
        nDecodedFrame_++;
    }
    nDecodedFrameReturned_ += nDecodedFrame_;
    return nDecodedFrame_;
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

void VideoDecode::saveImage(std::string outputfileName, void* pdevMem, outputImageInfo *pImageInfo, bool isOutputRGB) {

    uint8_t *hstPtr = nullptr;
    unsigned long outputImageSize = pImageInfo->lOutputImageSizeInBytes;
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
    if (pImageInfo->chromaFormat == VCN_FMT_YUV400 && isOutputRGB) {
        return;
    }
    uint8_t *tmpHstPtr = hstPtr;
    if (fp_out_ == nullptr) {
        fp_out_ = fopen(outputfileName.c_str(), "wb");
    }
    if (fp_out_) {
        int imgWidth = pImageInfo->nOutputWidth;
        int imgHeight = pImageInfo->nOutputHeight;
        int outputImageStride =  pImageInfo->nOutputHStride;
        if (imgWidth * nBPP_ == outputImageStride && imgHeight == pImageInfo->nOutputVStride) {
            fwrite(hstPtr, 1, outputImageSize, fp_out_);
        } else {
            uint32_t width = isOutputRGB ? pImageInfo->nOutputWidth * 3 : pImageInfo->nOutputWidth;
            if (nBitDepth_ == 8) {
                for (int i = 0; i < pImageInfo->nOutputHeight; i++) {
                    fwrite(tmpHstPtr, 1, width, fp_out_);
                    tmpHstPtr += outputImageStride;
                }
                if (!isOutputRGB) {
                    // dump chroma
                    uint8_t *uvHstPtr = hstPtr + outputImageStride * pImageInfo->nOutputVStride;
                    for (int i = 0; i < imgHeight >> 1; i++) {
                        fwrite(uvHstPtr, 1, width, fp_out_);
                        uvHstPtr += outputImageStride;
                    }
                }
            } else if (nBitDepth_ > 8 &&  nBitDepth_ <= 16 ) {
                for (int i = 0; i < imgHeight; i++) {
                    fwrite(tmpHstPtr, 1, width * nBPP_, fp_out_);
                    tmpHstPtr += outputImageStride;
                }
                if (!isOutputRGB) {
                    // dump chroma
                    uint8_t *uvHstPtr = hstPtr + outputImageStride * pImageInfo->nOutputVStride;
                    for (int i = 0; i < imgHeight >> 1; i++) {
                        fwrite(uvHstPtr, 1, width * nBPP_, fp_out_);
                        uvHstPtr += outputImageStride;
                    }
                }
            }
        }
    }

    if (hstPtr != nullptr) {
        delete [] hstPtr;
        hstPtr = nullptr;
        tmpHstPtr = nullptr;
    }
}

vcnVideoCodec_t VideoDecode::getVcnVideoCodecId() {
    switch (eVideoCodec_) {
        case AV_CODEC_ID_MPEG1VIDEO : return vcnVideoCodec_MPEG1;
        case AV_CODEC_ID_MPEG2VIDEO : return vcnVideoCodec_MPEG2;
        case AV_CODEC_ID_MPEG4      : return vcnVideoCodec_MPEG4;
        case AV_CODEC_ID_WMV3       :
        case AV_CODEC_ID_VC1        : return vcnVideoCodec_VC1;
        case AV_CODEC_ID_H264       : return vcnVideoCodec_H264;
        case AV_CODEC_ID_HEVC       : return vcnVideoCodec_HEVC;
        case AV_CODEC_ID_VP8        : return vcnVideoCodec_VP8;
        case AV_CODEC_ID_VP9        : return vcnVideoCodec_VP9;
        case AV_CODEC_ID_MJPEG      : return vcnVideoCodec_JPEG;
        case AV_CODEC_ID_AV1        : return vcnVideoCodec_AV1;
        default                     : return vcnVideoCodec_NumCodecs;
    }
}

std::string VideoDecode::getCodecFmtName(vcnVideoCodec_t vcnVideoCodecId) {
    std::string fmtName = "";
    switch (vcnVideoCodecId) {
        case vcnVideoCodec_MPEG1:
            fmtName = "MPEG1";
            break;
        case vcnVideoCodec_MPEG2:
            fmtName = "MPEG1";
            break;
        case vcnVideoCodec_MPEG4:
            fmtName = "MPEG4";
            break;
        case vcnVideoCodec_VC1:
            fmtName = "VC1";
            break;
       case vcnVideoCodec_H264:
            fmtName = "H264";
            break;
       case vcnVideoCodec_HEVC:
            fmtName = "HEVC";
            break;
        case vcnVideoCodec_VP8:
            fmtName = "VP8";
            break;
        case vcnVideoCodec_VP9:
            fmtName = "VP9";
            break;
        case vcnVideoCodec_JPEG:
            fmtName = "JPEG";
            break;
        case vcnVideoCodec_AV1:
            fmtName = "AV1";
            break;
        default:
            std::cerr << "ERROR: subsampling format is not supported!" << std::endl;
    }
    return fmtName;
}
