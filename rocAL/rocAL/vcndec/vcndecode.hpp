/*
Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
*/

#pragma once

#include <iostream>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <cstring>
#include <queue>
#include <iomanip>
#include <hip/hip_runtime.h>
#include <mutex>

extern "C" {
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/pixdesc.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/hwcontext_vaapi.h>
    #include <va/va.h>
    #include <va/va_drmcommon.h>
}

#define AVIO_CONTEXT_BUF_SIZE   256*1024     //256K
#define NUM_BUFFERS_IN_POOL     2

static bool getEnv(const char *name, char *value, size_t valueSize) {
    const char *v = getenv(name);
    if (v) {
        strncpy(value, v, valueSize);
        value[valueSize - 1] = 0;
    }
    return v ? true : false;
}

static inline int align(int value, int alignment) {
   return (value + alignment - 1) & ~(alignment - 1);
}

//! \enum vcnVideoCodec_t
//! Video codec enums
typedef enum vcnVideoCodec_enum {
    vcnVideoCodec_MPEG1 = 0,                                              /**<  MPEG1             */
    vcnVideoCodec_MPEG2 = 1,                                              /**<  MPEG2             */
    vcnVideoCodec_MPEG4 = 2,                                              /**<  MPEG4             */
    vcnVideoCodec_VC1 = 3,                                                /**<  VC1               */
    vcnVideoCodec_H264 = 4,                                               /**<  H264              */
    vcnVideoCodec_JPEG = 5,                                               /**<  JPEG              */
    vcnVideoCodec_H264_SVC = 6,                                           /**<  H264-SVC          */
    vcnVideoCodec_H264_MVC = 7,                                           /**<  H264-MVC          */
    vcnVideoCodec_HEVC = 8,                                               /**<  HEVC              */
    vcnVideoCodec_VP8 = 9,                                                /**<  VP8               */
    vcnVideoCodec_VP9 = 10,                                               /**<  VP9               */
    vcnVideoCodec_AV1 = 11,                                               /**<  AV1               */
    vcnVideoCodec_NumCodecs = 12,                                         /**<  Max codecs        */
} vcnVideoCodec_t;

typedef enum {
    VCN_FMT_YUV420 = 0,
    VCN_FMT_YUV444 = 1,
    VCN_FMT_YUV422 = 2,
    VCN_FMT_YUV400 = 3,
    VCN_FMT_YUV420P10 = 4,
    VCN_FMT_YUV420P12 = 5,   
    VCN_FMT_RGB24 = 6,
    VCN_FMT_BGR24 = 7,
    VCN_FMT_MAX = 8
} vcnImageFormat_t;

typedef struct _ioBufferData {
    uint8_t *ptr;
    size_t size; ///< size left in the buffer
} ioBufferData;

typedef struct _decFrameBuffer {
    AVFrame *pFrame;                                        /**< AVFrame ptr for the decoded frame buffer */
    hipExternalMemory_t hipExtMem;                          /**< interop hip memory for the decoded surface */
    VADRMPRIMESurfaceDescriptor vaDrmPrimeSurfaceDesc;      /**< DRM surface descriptor */
} decFrameBuffer;

typedef struct _outputImageInfo {
    unsigned int nOutputWidth;        /**< Output width of decoded image*/
    unsigned int nOutputHeight;       /**< Output height of decoded image*/
    unsigned int nOutputHStride;      /**< Output horizontal stride in bytes of luma plane, chroma hstride can be inferred based on chromaFormat*/
    unsigned int nOutputVStride;      /**< Output vertical stride in number of columns of luma plane,  chroma vstride can be inferred based on chromaFormat */
    unsigned int nBytesPerPixel;      /**< Output BytesPerPixel of decoded image*/
    unsigned int nBitDepth;           /**< Output BitDepth of the image*/
    unsigned long lOutputImageSizeInBytes;      /**< Output Image Size in Bytes; including both luma and chroma planes*/ 
    vcnImageFormat_t chromaFormat;     /**< Chroma format of the decoded image*/
}outputImageInfo;

// for returning more meaningful error status
typedef enum VCNDecodeStatus_enum {
    VCNDEC_FAILURE = -1,
    VCNDEC_DEVICE_INVALID = -2,
    VCNDEC_CONTEXT_INVALID = -3,
    VCNDEC_RUNTIME_ERROR  = -4,
    VCNDEC_OUTOF_MEMORY = -5,
    VCNDEC_INVALID_PARAMETER = -5,
    VCNDEC_SUCCESS = 0,
} VCNDecodeStatus;


//interface class for jpeg decoding
class VCNDecode {
    public:
        VCNDecode(int deviceID = 0);
        ~VCNDecode();
        bool isJPEGHWDecoderSupported();
        bool closeInputStream();
        bool getImageInfo(const std::string filePath, uint8_t *nComponents, vcnImageFormat_t &subsampling, uint32_t *width, uint32_t *height);
        bool getImageInfo(unsigned char* input_buffer, size_t input_size, uint8_t *nComponents, vcnImageFormat_t &subsampling, uint32_t *width, uint32_t *height);
        bool getImageSizeHint(vcnImageFormat_t subsampling, const uint32_t output_width, const uint32_t output_height, uint32_t *output_stride, size_t *output_image_size);
        bool decode(const std::string filePath, void *output_image, const size_t output_image_size);
        bool decode(uint8_t *pData, int nSize, int64_t pts = 0);      // decode from url
        uint8_t* getFrame(int64_t *pts);
        bool releaseFrame(int64_t pts);
        void saveImage(std::string outputfileName, void* pdevMem, size_t outputImageSize, uint32_t width, uint32_t height, uint32_t outputImageStride, vcnImageFormat_t subsampling, bool isOutputRGB = 0);
        void getDeviceinfo(std::string &deviceName, std::string &gcnArchName, int &pciBusID, int &pciDomainID, int &pciDeviceID, std::string &drmNode);
        std::string getPixFmtName(vcnImageFormat_t subsampling);
        bool getOutputImageInfo(outputImageInfo **pFrame);
        //scaling and colorconversion
        bool scaleYUVimage(void *pYUVdevMem, void *pUdevMem, void *pVdevMem, void *pScaledYUVdevMem, void *pScaledUdevMem, void *pScaledVdevMem,
            size_t scaledYUVimageSize, uint32_t alignedScalingWidth, uint32_t alignedScalingHeight, uint32_t scaledYUVstride, uint32_t scaledLumaSize,
            VADRMPRIMESurfaceDescriptor &vaDrmPrimeSurfaceDesc, hipStream_t &hipStream);     
        bool colorConvertYUVtoRGB(void *pYUVdevMem, void *pScaledYUVdevMem, uint8_t *pRGBdevMem, uint32_t scaledLumaSize, uint32_t alignedScalingWidth,
            uint32_t alignedScalingHeight, uint32_t scaledYUVstride, bool isScaling, VADRMPRIMESurfaceDescriptor &vaDrmPrimeSurfaceDesc, hipStream_t &hipStream);

    protected:
       bool initHIP(int deviceID);
       void initDRMnodes();
       bool initHWDevCtx();
       bool queryVaProfiles();
       bool openInputFile(const std::string filePath);
       bool openInputStream(unsigned char* input_buffer, size_t input_size, int* width, int* height);

       bool openCodec();
       bool initFrame();
       static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
       bool getImageSizeHintInternal(vcnImageFormat_t subsampling, const uint32_t output_width, const uint32_t output_height,
            uint32_t *output_stride, size_t *output_image_size);

       std::string filename_;
       hipDeviceProp_t hipDevProp_;
       int numDevices_;
       int deviceID_;
       hipStream_t hipStream_;
       std::vector<std::string> drmNodes_;
       int av_stream_;
       AVFormatContext *av_fmt_input_ctx_;
       AVCodec *av_decoder_;
       AVCodecContext *av_decoder_ctx_;
       AVBufferRef *av_bufref_hw_dev_ctx_;
       AVFrame *frame_;
       AVPacket packet_;
       VADisplay va_display_;
       VASurfaceID va_surfaceID_;
       VADRMPRIMESurfaceDescriptor vaDrmPrimeSurfaceDesc_;
       VAProfile va_profiles_[36];
       int num_va_profiles_;
       bool isJPEGHWDecoderSuppoerted_;
       hipExternalMemory_t hipExtMem_;
       hipExternalMemoryHandleDesc externalMemoryHandleDesc_;
       hipExternalMemoryBufferDesc externalMemBufferDesc_;
       void *pYUVdevMem_;
       // height of the mapped surface
       int nSurfaceHeight_ = 0;
       int nSurfaceWidth_ = 0;
       outputImageInfo outImageInfo_ = {0};            //output ImageInfo
       std::mutex mutex_;
};

//interface class for video decode
class VideoDecode : public VCNDecode {
    public:
        VideoDecode(int deviceID = 0);
        ~VideoDecode();

        int decode(uint8_t *pData, int nSize, int64_t pts = 0, int nFlags = 0);
        void saveImage(std::string outputfileName, void* pdevMem, outputImageInfo *pImageInfo, bool isOutputRGB = 0);
        uint8_t* getFrame(int64_t *pts);
        bool releaseFrame(int64_t pts);
        int getWidth() { assert(nWidth_); return nWidth_;}
        int getHeight() { assert(nHeight_); return nHeight_; }
        int getBitDepth() { assert(nBitDepth_); return nBitDepth_; }
        int getBPP() { assert(nBPP_); return nBPP_; }
        size_t getSurfaceSize() {assert(nSurfaceSize_); return nSurfaceSize_; }
        uint32_t getSurfaceStride() {assert(nSurfaceStride_); return nSurfaceStride_; }
        vcnImageFormat_t getSubsampling() { return subsampling_; }
        vcnVideoCodec_t getVcnVideoCodecId();
        int getSurfaceWidth() {assert(nSurfaceWidth_); return nSurfaceWidth_;}
        int getSurfaceHeight() {assert(nSurfaceHeight_); return nSurfaceHeight_;}
        std::string getCodecFmtName(vcnVideoCodec_t vcnVideoCodecId);
    private:
        //bool openInputStream(uint8_t *pData, int nSize);
        ioBufferData io_buffer_data_;
        uint8_t * avio_ctx_buffer_ = nullptr ;    // for io context
        AVIOContext *io_ctx_ = nullptr;
        // dimension of the output
        unsigned int nWidth_ = 0, nHeight_ = 0, nChromaHeight_ = 0;
        unsigned int nNumChromaPlanes_ = 0;
        unsigned int nComponents_ = 0;
        vcnImageFormat_t subsampling_ = VCN_FMT_YUV420;
        uint32_t nSurfaceStride_ = 0;
        size_t nSurfaceSize_ = 0;
        int nBitDepth_ = 8;
        int nBPP_ = 1;
        FILE *fp_out_ = nullptr;
        AVCodecID eVideoCodec_ = AV_CODEC_ID_NONE;
        // Q of decoded frame buffers and bufferpool
        std::queue<decFrameBuffer *> dec_frame_q_;
        std::vector<decFrameBuffer *> decBufferPool_;
        int nDecodedFrame_ = 0, nDecodedFrameReturned_ = 0;
        bool lastFrameReceived_ = false;
        std::mutex mtxVPFrame_;
};