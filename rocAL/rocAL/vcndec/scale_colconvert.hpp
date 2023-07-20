/*
Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.

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

#include <hip/hip_runtime.h>

extern "C" {
int HipExec_ColorConvert_NV12_to_RGB(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes,
    const uint8_t *pHipSrcLumaImage, uint32_t srcLumaImageStrideInBytes,
    const uint8_t *pHipSrcChromaImage, uint32_t srcChromaImageStrideInBytes);

int HipExec_ColorConvert_YUV444_to_RGB(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes, const uint8_t *pHipSrcYUVImage,
    uint32_t srcYUVImageStrideInBytes, uint32_t srcUImageOffset);

int HipExec_ColorConvert_YUV800_to_RGB(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes, const uint8_t *pHipSrcYUVImage, 
    uint32_t srcYUVImageStrideInBytes);

int HipExec_ScaleImage_YUV420_Nearest(hipStream_t stream, uint32_t scaledYWidth, uint32_t scaledYHeight,
    uint8_t *pHipScaledYImage, uint32_t scaledYImageStrideInBytes, uint32_t srcYWidth, uint32_t srcYHeight,
    const uint8_t *pHipSrcYImage, uint32_t srcYImageStrideInBytes, uint8_t *pHipScaledUImage, uint8_t *pHipScaledVImage,
    const uint8_t *pHipSrcUImage, const uint8_t *pHipSrcVImage);

int HipExec_ScaleImage_NV12_Nearest(hipStream_t stream, uint32_t scaledYWidth, uint32_t scaledYHeight,
    uint8_t *pHipScaledYImage, uint32_t scaledYImageStrideInBytes, uint32_t srcYWidth, uint32_t srcYHeight,
    const uint8_t *pHipSrcYImage, uint32_t srcYImageStrideInBytes, uint8_t *pHipScaledUVImage,
    const uint8_t *pHipSrcUVImage);
    
int HipExec_ChannelExtract_U8U8_U16(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage1, uint8_t *pHipDstImage2, uint32_t dstImageStrideInBytes,
    const uint8_t *pHipSrcImage1, uint32_t srcImage1StrideInBytes);

int HipExec_ChannelCombine_U16_U8U8(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes,
    const uint8_t *pHipSrcImage1, uint32_t srcImage1StrideInBytes,
    const uint8_t *pHipSrcImage2, uint32_t srcImage2StrideInBytes);

int HipExec_ScaleImage_U8_U8_Nearest(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes, uint32_t srcWidth, uint32_t srcHeight,
    const uint8_t *pHpSrcImage, uint32_t srcImageStrideInBytes);

int HipExec_ScaleImage_YUV444_Nearest(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstYUVImage, uint32_t dstImageStrideInBytes, uint32_t dstUImageOffset,
    uint32_t srcWidth, uint32_t srcHeight, const uint8_t *pHipSrcYUVImage,
    uint32_t srcImageStrideInBytes, uint32_t srcUImageOffset);
}

typedef struct d_uint6 {
  uint data[6];
} d_uint6;

__device__ __forceinline__ uint hip_pack(float4 src) {
    return __builtin_amdgcn_cvt_pk_u8_f32(src.w, 3,
           __builtin_amdgcn_cvt_pk_u8_f32(src.z, 2,
           __builtin_amdgcn_cvt_pk_u8_f32(src.y, 1,
           __builtin_amdgcn_cvt_pk_u8_f32(src.x, 0, 0))));
}

__device__ __forceinline__ float hip_unpack0(uint src) {
    return (float)(src & 0xFF);
}

__device__ __forceinline__ float hip_unpack1(uint src) {
    return (float)((src >> 8) & 0xFF);
}

__device__ __forceinline__ float hip_unpack2(uint src) {
    return (float)((src >> 16) & 0xFF);
}

__device__ __forceinline__ float hip_unpack3(uint src) {
    return (float)((src >> 24) & 0xFF);
}

__device__ __forceinline__ float4 hip_unpack(uint src) {
    return make_float4(hip_unpack0(src), hip_unpack1(src), hip_unpack2(src), hip_unpack3(src));
}

__global__ void __attribute__((visibility("default")))
Hip_ColorConvert_NV12_to_RGB(uint dstWidth, uint dstHeight,
    uchar *pDstImage, uint dstImageStrideInBytes, uint dstImageStrideInBytesComp,
    const uchar *pSrcLumaImage, uint srcLumaImageStrideInBytes,
    const uchar *pSrcChromaImage, uint srcChromaImageStrideInBytes,
    uint dstWidthComp, uint dstHeightComp, uint srcLumaImageStrideInBytesComp) {

    int x = hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if ((x < dstWidthComp) && (y < dstHeightComp)) {
        uint srcY0Idx = y * srcLumaImageStrideInBytesComp + (x << 3);
        uint srcY1Idx = srcY0Idx + srcLumaImageStrideInBytes;
        uint srcUVIdx = y * srcChromaImageStrideInBytes + (x << 3);
        uint2 pY0 = *((uint2 *)(&pSrcLumaImage[srcY0Idx]));
        uint2 pY1 = *((uint2 *)(&pSrcLumaImage[srcY1Idx]));
        uint2 pUV = *((uint2 *)(&pSrcChromaImage[srcUVIdx]));

        uint RGB0Idx = y * dstImageStrideInBytesComp + (x * 24);
        uint RGB1Idx = RGB0Idx + dstImageStrideInBytes;

        float4 f;
        uint2 pU0, pU1;
        uint2 pV0, pV1;

        f.x = hip_unpack0(pUV.x);
        f.y = f.x;
        f.z = hip_unpack2(pUV.x);
        f.w = f.z;
        pU0.x = hip_pack(f);

        f.x = hip_unpack0(pUV.y);
        f.y = f.x;
        f.z = hip_unpack2(pUV.y);
        f.w = f.z;
        pU0.y = hip_pack(f);

        pU1.x = pU0.x;
        pU1.y = pU0.y;

        f.x = hip_unpack1(pUV.x);
        f.y = f.x;
        f.z = hip_unpack3(pUV.x);
        f.w = f.z;
        pV0.x = hip_pack(f);

        f.x = hip_unpack1(pUV.y);
        f.y = f.x;
        f.z = hip_unpack3(pUV.y);
        f.w = f.z;
        pV0.y = hip_pack(f);

        pV1.x = pV0.x;
        pV1.y = pV0.y;

        float2 cR = make_float2( 0.0000f,  1.5748f);
        float2 cG = make_float2(-0.1873f, -0.4681f);
        float2 cB = make_float2( 1.8556f,  0.0000f);
        float3 yuv;
        d_uint6 pRGB0, pRGB1;

        yuv.x = hip_unpack0(pY0.x);
        yuv.y = hip_unpack0(pU0.x);
        yuv.z = hip_unpack0(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY0.x);
        yuv.y = hip_unpack1(pU0.x);
        yuv.z = hip_unpack1(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB0.data[0] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY0.x);
        yuv.y = hip_unpack2(pU0.x);
        yuv.z = hip_unpack2(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB0.data[1] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY0.x);
        yuv.y = hip_unpack3(pU0.x);
        yuv.z = hip_unpack3(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB0.data[2] = hip_pack(f);

        yuv.x = hip_unpack0(pY0.y);
        yuv.y = hip_unpack0(pU0.y);
        yuv.z = hip_unpack0(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY0.y);
        yuv.y = hip_unpack1(pU0.y);
        yuv.z = hip_unpack1(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB0.data[3] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY0.y);
        yuv.y = hip_unpack2(pU0.y);
        yuv.z = hip_unpack2(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB0.data[4] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY0.y);
        yuv.y = hip_unpack3(pU0.y);
        yuv.z = hip_unpack3(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB0.data[5] = hip_pack(f);

        yuv.x = hip_unpack0(pY1.x);
        yuv.y = hip_unpack0(pU1.x);
        yuv.z = hip_unpack0(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY1.x);
        yuv.y = hip_unpack1(pU1.x);
        yuv.z = hip_unpack1(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB1.data[0] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY1.x);
        yuv.y = hip_unpack2(pU1.x);
        yuv.z = hip_unpack2(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB1.data[1] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY1.x);
        yuv.y = hip_unpack3(pU1.x);
        yuv.z = hip_unpack3(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB1.data[2] = hip_pack(f);

        yuv.x = hip_unpack0(pY1.y);
        yuv.y = hip_unpack0(pU1.y);
        yuv.z = hip_unpack0(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY1.y);
        yuv.y = hip_unpack1(pU1.y);
        yuv.z = hip_unpack1(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB1.data[3] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY1.y);
        yuv.y = hip_unpack2(pU1.y);
        yuv.z = hip_unpack2(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB1.data[4] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY1.y);
        yuv.y = hip_unpack3(pU1.y);
        yuv.z = hip_unpack3(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB1.data[5] = hip_pack(f);

        *((d_uint6 *)(&pDstImage[RGB0Idx])) = pRGB0;
        *((d_uint6 *)(&pDstImage[RGB1Idx])) = pRGB1;
    }
}

int HipExec_ColorConvert_NV12_to_RGB(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes,
    const uint8_t *pHipSrcLumaImage, uint32_t srcLumaImageStrideInBytes,
    const uint8_t *pHipSrcChromaImage, uint32_t srcChromaImageStrideInBytes) {
    int localThreads_x = 16;
    int localThreads_y = 4;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = (dstHeight + 1) >> 1;

    uint32_t dstWidthComp = (dstWidth + 7) / 8;
    uint32_t dstHeightComp = (dstHeight + 1) / 2;
    uint32_t dstImageStrideInBytesComp = dstImageStrideInBytes * 2;
    uint32_t srcLumaImageStrideInBytesComp = srcLumaImageStrideInBytes * 2;

    hipLaunchKernelGGL(Hip_ColorConvert_NV12_to_RGB, dim3(ceil((float)globalThreads_x / localThreads_x), ceil((float)globalThreads_y / localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight, (uchar *)pHipDstImage, dstImageStrideInBytes, dstImageStrideInBytesComp,
                        (const uchar *)pHipSrcLumaImage, srcLumaImageStrideInBytes, (const uchar *)pHipSrcChromaImage, srcChromaImageStrideInBytes,
                        dstWidthComp, dstHeightComp, srcLumaImageStrideInBytesComp);

    return 0;
}

__global__ void __attribute__((visibility("default")))
Hip_ColorConvert_YUV444_to_RGB(uint dstWidth, uint dstHeight,
    uchar *pDstImage, uint dstImageStrideInBytes, uint dstImageStrideInBytesComp,
    const uchar *pSrcYImage, const uchar *pSrcUImage, const uchar *pSrcVImage, uint srcYUVImageStrideInBytes,
    uint dstWidthComp, uint dstHeightComp, uint srcYUVImageStrideInBytesComp) {

    int x = hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if ((x < dstWidthComp) && (y < dstHeightComp)) {
        uint srcY0Idx = y * srcYUVImageStrideInBytesComp + (x << 3);
        uint srcY1Idx = srcY0Idx + srcYUVImageStrideInBytes;


        uint2 pY0 = *((uint2 *)(&pSrcYImage[srcY0Idx]));
        uint2 pY1 = *((uint2 *)(&pSrcYImage[srcY1Idx]));

        uint2 pU0 = *((uint2 *)(&pSrcUImage[srcY0Idx]));
        uint2 pU1 = *((uint2 *)(&pSrcUImage[srcY1Idx]));

        uint2 pV0 = *((uint2 *)(&pSrcVImage[srcY0Idx]));
        uint2 pV1 = *((uint2 *)(&pSrcVImage[srcY1Idx]));

        uint RGB0Idx = y * dstImageStrideInBytesComp + (x * 24);
        uint RGB1Idx = RGB0Idx + dstImageStrideInBytes;

        float2 cR = make_float2( 0.0000f,  1.5748f);
        float2 cG = make_float2(-0.1873f, -0.4681f);
        float2 cB = make_float2( 1.8556f,  0.0000f);
        float3 yuv;
        d_uint6 pRGB0, pRGB1;
        float4 f;

        yuv.x = hip_unpack0(pY0.x);
        yuv.y = hip_unpack0(pU0.x);
        yuv.z = hip_unpack0(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY0.x);
        yuv.y = hip_unpack1(pU0.x);
        yuv.z = hip_unpack1(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB0.data[0] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY0.x);
        yuv.y = hip_unpack2(pU0.x);
        yuv.z = hip_unpack2(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB0.data[1] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY0.x);
        yuv.y = hip_unpack3(pU0.x);
        yuv.z = hip_unpack3(pV0.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB0.data[2] = hip_pack(f);

        yuv.x = hip_unpack0(pY0.y);
        yuv.y = hip_unpack0(pU0.y);
        yuv.z = hip_unpack0(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY0.y);
        yuv.y = hip_unpack1(pU0.y);
        yuv.z = hip_unpack1(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB0.data[3] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY0.y);
        yuv.y = hip_unpack2(pU0.y);
        yuv.z = hip_unpack2(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB0.data[4] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY0.y);
        yuv.y = hip_unpack3(pU0.y);
        yuv.z = hip_unpack3(pV0.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB0.data[5] = hip_pack(f);

        yuv.x = hip_unpack0(pY1.x);
        yuv.y = hip_unpack0(pU1.x);
        yuv.z = hip_unpack0(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY1.x);
        yuv.y = hip_unpack1(pU1.x);
        yuv.z = hip_unpack1(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB1.data[0] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY1.x);
        yuv.y = hip_unpack2(pU1.x);
        yuv.z = hip_unpack2(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB1.data[1] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY1.x);
        yuv.y = hip_unpack3(pU1.x);
        yuv.z = hip_unpack3(pV1.x);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB1.data[2] = hip_pack(f);

        yuv.x = hip_unpack0(pY1.y);
        yuv.y = hip_unpack0(pU1.y);
        yuv.z = hip_unpack0(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.x = fmaf(cR.y, yuv.z, yuv.x);
        f.y = fmaf(cG.x, yuv.y, yuv.x);
        f.y = fmaf(cG.y, yuv.z, f.y);
        f.z = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack1(pY1.y);
        yuv.y = hip_unpack1(pU1.y);
        yuv.z = hip_unpack1(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.w = fmaf(cR.y, yuv.z, yuv.x);
        pRGB1.data[3] = hip_pack(f);

        f.x = fmaf(cG.x, yuv.y, yuv.x);
        f.x = fmaf(cG.y, yuv.z, f.x);
        f.y = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack2(pY1.y);
        yuv.y = hip_unpack2(pU1.y);
        yuv.z = hip_unpack2(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.z = fmaf(cR.y, yuv.z, yuv.x);
        f.w = fmaf(cG.x, yuv.y, yuv.x);
        f.w = fmaf(cG.y, yuv.z, f.w);
        pRGB1.data[4] = hip_pack(f);

        f.x = fmaf(cB.x, yuv.y, yuv.x);
        yuv.x = hip_unpack3(pY1.y);
        yuv.y = hip_unpack3(pU1.y);
        yuv.z = hip_unpack3(pV1.y);
        yuv.y -= 128.0f;
        yuv.z -= 128.0f;
        f.y = fmaf(cR.y, yuv.z, yuv.x);
        f.z = fmaf(cG.x, yuv.y, yuv.x);
        f.z = fmaf(cG.y, yuv.z, f.z);
        f.w = fmaf(cB.x, yuv.y, yuv.x);
        pRGB1.data[5] = hip_pack(f);

        *((d_uint6 *)(&pDstImage[RGB0Idx])) = pRGB0;
        *((d_uint6 *)(&pDstImage[RGB1Idx])) = pRGB1;
    }
}

int HipExec_ColorConvert_YUV444_to_RGB(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes, const uint8_t *pHipSrcYUVImage,
    uint32_t srcYUVImageStrideInBytes, uint32_t srcUImageOffset) {

    int localThreads_x = 16;
    int localThreads_y = 4;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = (dstHeight + 1) >> 1;

    uint32_t dstWidthComp = (dstWidth + 7) / 8;
    uint32_t dstHeightComp = (dstHeight + 1) / 2;
    uint32_t dstImageStrideInBytesComp = dstImageStrideInBytes * 2;
    uint32_t srcYUVImageStrideInBytesComp = srcYUVImageStrideInBytes * 2;

    hipLaunchKernelGGL(Hip_ColorConvert_YUV444_to_RGB, dim3(ceil((float)globalThreads_x / localThreads_x), ceil((float)globalThreads_y / localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight, (uchar *)pHipDstImage, dstImageStrideInBytes, dstImageStrideInBytesComp,
                        (const uchar *)pHipSrcYUVImage,
                        (const uchar *)pHipSrcYUVImage + srcUImageOffset,
                        (const uchar *)pHipSrcYUVImage + (srcUImageOffset * 2),
                        srcYUVImageStrideInBytes,
                        dstWidthComp, dstHeightComp, srcYUVImageStrideInBytesComp);

    return 0;
}

__global__ void __attribute__((visibility("default")))
Hip_ColorConvert_YUV800_to_RGB(uint dstWidth, uint dstHeight,
    uchar *pDstImage, uint dstImageStrideInBytes, uint dstImageStrideInBytesComp,
    const uchar *pSrcLumaImage, uint srcLumaImageStrideInBytes,
    uint dstWidthComp, uint dstHeightComp, uint srcLumaImageStrideInBytesComp) {

    int x = hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if ((x < dstWidthComp) && (y < dstHeightComp)) {
        uint srcY0Idx = y * srcLumaImageStrideInBytesComp + (x << 3);
        uint srcY1Idx = srcY0Idx + srcLumaImageStrideInBytes;
        uint2 pY0 = *((uint2 *)(&pSrcLumaImage[srcY0Idx]));
        uint2 pY1 = *((uint2 *)(&pSrcLumaImage[srcY1Idx]));

        uint RGB0Idx = y * dstImageStrideInBytesComp + (x * 24);
        uint RGB1Idx = RGB0Idx + dstImageStrideInBytes;
        d_uint6 pRGB0, pRGB1;
        u_int8_t y0, y1, y2, y3, y4, y5, y6, y7;
        y0 = (pY0.x & 0xFF), y1 = (pY0.x >> 8) & 0xFF, y2 =  (pY0.x >> 16) & 0xFF, y3 = (pY0.x >> 24) & 0xFF;  
        y4 = (pY0.y & 0xFF), y5 = (pY0.y >> 8) & 0xFF, y6 =  (pY0.y >> 16) & 0xFF, y7 = (pY0.y >> 24) & 0xFF;  
        pRGB0.data[0] = y0 | (y0 << 8) | (y0 << 16) | (y1 << 24); 
        pRGB0.data[1] = y1 | (y1 << 8) | (y2 << 16) | (y2 << 24); 
        pRGB0.data[2] = y2 | (y3 << 8) | (y3 << 16) | (y3 << 24); 
        pRGB0.data[3] = y4 | (y4 << 8) | (y4 << 16) | (y5 << 24); 
        pRGB0.data[4] = y5 | (y5 << 8) | (y6 << 16) | (y6 << 24); 
        pRGB0.data[5] = y6 | (y7 << 8) | (y7 << 16) | (y7 << 24); 

        y0 = (pY1.x & 0xFF), y1 = (pY1.x >> 8) & 0xFF, y2 =  (pY1.x >> 16) & 0xFF, y3 = (pY1.x >> 24) & 0xFF;  
        y4 = (pY1.y & 0xFF), y5 = (pY1.y >> 8) & 0xFF, y6 =  (pY1.y >> 16) & 0xFF, y7 = (pY1.y >> 24) & 0xFF;  
        pRGB1.data[0] = y0 | (y0 << 8) | (y0 << 16) | (y1 << 24); 
        pRGB1.data[1] = y1 | (y1 << 8) | (y2 << 16) | (y2 << 24); 
        pRGB1.data[2] = y2 | (y3 << 8) | (y3 << 16) | (y3 << 24); 
        pRGB1.data[3] = y4 | (y4 << 8) | (y4 << 16) | (y5 << 24); 
        pRGB1.data[4] = y5 | (y5 << 8) | (y6 << 16) | (y6 << 24); 
        pRGB1.data[5] = y6 | (y7 << 8) | (y7 << 16) | (y7 << 24); 

        *((d_uint6 *)(&pDstImage[RGB0Idx])) = pRGB0;
        *((d_uint6 *)(&pDstImage[RGB1Idx])) = pRGB1;
    }
}

int HipExec_ColorConvert_YUV800_to_RGB(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes, const uint8_t *pHipSrcYUVImage, 
    uint32_t srcYUVImageStrideInBytes) {

    int localThreads_x = 16;
    int localThreads_y = 4;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = (dstHeight + 1) >> 1;

    uint32_t dstWidthComp = (dstWidth + 7) / 8;
    uint32_t dstHeightComp = (dstHeight + 1) / 2;
    uint32_t dstImageStrideInBytesComp = dstImageStrideInBytes * 2;
    uint32_t srcYUVImageStrideInBytesComp = srcYUVImageStrideInBytes * 2;

    hipLaunchKernelGGL(Hip_ColorConvert_YUV800_to_RGB, dim3(ceil((float)globalThreads_x / localThreads_x), ceil((float)globalThreads_y / localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight, (uchar *)pHipDstImage, dstImageStrideInBytes, dstImageStrideInBytesComp,
                        (const uchar *)pHipSrcYUVImage,
                        (const uchar *)pHipSrcYUVImage + srcUImageOffset,
                        (const uchar *)pHipSrcYUVImage + (srcUImageOffset * 2),
                        srcYUVImageStrideInBytes,
                        dstWidthComp, dstHeightComp, srcYUVImageStrideInBytesComp);

    return 0;
}


__global__ void __attribute__((visibility("default")))
Hip_ScaleImage_YUV420_Nearest(uint scaledYWidth, uint scaledYHeight, uchar *pScaledYImage, uint scaledYImageStrideInBytes,
    const uchar *pSrcYImage, uint srcYImageStrideInBytes, float xscaleY, float yscaleY, float xoffsetY, float yoffsetY,
    uint scaledUVWidth, uint scaledUVHeight, uchar *pScaledUImage, uchar *pScaledVImage, uint scaledUVImageStrideInBytes,
    const uchar *pSrcUImage, const uchar *pSrcVImage, uint srcUVImageStrideInBytes,
    float xscaleUV, float yscaleUV, float xoffsetUV, float yoffsetUV) {

    int x = (hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x) * 8;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if (x >= scaledYWidth || y >= scaledYHeight) {
        return;
    }

    uint scaledYIdx = y * scaledYImageStrideInBytes + x;

    float4 scaleInfo = make_float4(xscaleY, yscaleY, xoffsetY, yoffsetY);

    uint2 scaledYdst;
    pSrcYImage += srcYImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    float fx = fmaf((float)x, scaleInfo.x, scaleInfo.z);

    scaledYdst.x  = pSrcYImage[(int)fx];
    fx += scaleInfo.x;
    scaledYdst.x |= pSrcYImage[(int)fx] << 8;
    fx += scaleInfo.x;
    scaledYdst.x |= pSrcYImage[(int)fx] << 16;
    fx += scaleInfo.x;
    scaledYdst.x |= pSrcYImage[(int)fx] << 24;

    fx += scaleInfo.x;

    scaledYdst.y  = pSrcYImage[(int)fx];
    fx += scaleInfo.x;
    scaledYdst.y |= pSrcYImage[(int)fx] << 8;
    fx += scaleInfo.x;
    scaledYdst.y |= pSrcYImage[(int)fx] << 16;
    fx += scaleInfo.x;
    scaledYdst.y |= pSrcYImage[(int)fx] << 24;

    *((uint2 *)(&pScaledYImage[scaledYIdx])) = scaledYdst;

    //scale the U and V components here
    if (x >= scaledUVWidth || y >= scaledUVHeight) {
        return;
    }

    uint scaledUVIdx = y * scaledUVImageStrideInBytes + x;

    scaleInfo = make_float4(xscaleUV, yscaleUV, xoffsetUV, yoffsetUV);

    uint2 scaledUdst, scaledVdst;
    pSrcUImage += srcUVImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    pSrcVImage += srcUVImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    fx = fmaf((float)x, scaleInfo.x, scaleInfo.z);

    scaledUdst.x  = pSrcUImage[(int)fx];
    scaledVdst.x  = pSrcVImage[(int)fx];
    fx += scaleInfo.x;
    scaledUdst.x |= pSrcUImage[(int)fx] << 8;
    scaledVdst.x |= pSrcVImage[(int)fx] << 8;
    fx += scaleInfo.x;
    scaledUdst.x |= pSrcUImage[(int)fx] << 16;
    scaledVdst.x |= pSrcVImage[(int)fx] << 16;
    fx += scaleInfo.x;
    scaledUdst.x |= pSrcUImage[(int)fx] << 24;
    scaledVdst.x |= pSrcVImage[(int)fx] << 24;

    fx += scaleInfo.x;

    scaledUdst.y  = pSrcUImage[(int)fx];
    scaledVdst.y  = pSrcVImage[(int)fx];
    fx += scaleInfo.x;
    scaledUdst.y |= pSrcUImage[(int)fx] << 8;
    scaledVdst.y |= pSrcVImage[(int)fx] << 8;
    fx += scaleInfo.x;
    scaledUdst.y |= pSrcUImage[(int)fx] << 16;
    scaledVdst.y |= pSrcVImage[(int)fx] << 16;
    fx += scaleInfo.x;
    scaledUdst.y |= pSrcUImage[(int)fx] << 24;
    scaledVdst.y |= pSrcVImage[(int)fx] << 24;

    *((uint2 *)(&pScaledUImage[scaledUVIdx])) = scaledUdst;
    *((uint2 *)(&pScaledVImage[scaledUVIdx])) = scaledVdst;

}

__global__ void __attribute__((visibility("default")))
Hip_ScaleImage_NV12_Nearest(uint scaledYWidth, uint scaledYHeight, uchar *pScaledYImage, uint scaledYImageStrideInBytes,
    const uchar *pSrcYImage, uint srcYImageStrideInBytes, float xscale, float yscale, float xoffset, float yoffset,
    uint scaledUVWidth, uint scaledUVHeight, uchar *pScaledUVImage, uint scaledUVImageStrideInBytes,
    const uchar *pSrcUVImage, uint srcUVImageStrideInBytes) {

    int x = (hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x) * 8;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if (x >= scaledYWidth || y >= scaledYHeight) {
        return;
    }

    uint scaledYIdx0 = y * scaledYImageStrideInBytes + x;

    float4 scaleInfo = make_float4(xscale, yscale, xoffset, yoffset);

    uint2 scaledYdst;
    pSrcYImage += srcYImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    float fx = fmaf((float)x, scaleInfo.x, scaleInfo.z);

    scaledYdst.x  = pSrcYImage[(int)fx];
    fx += scaleInfo.x;
    scaledYdst.x |= pSrcYImage[(int)fx] << 8;
    fx += scaleInfo.x;
    scaledYdst.x |= pSrcYImage[(int)fx] << 16;
    fx += scaleInfo.x;
    scaledYdst.x |= pSrcYImage[(int)fx] << 24;

    fx += scaleInfo.x;

    scaledYdst.y  = pSrcYImage[(int)fx];
    fx += scaleInfo.x;
    scaledYdst.y |= pSrcYImage[(int)fx] << 8;
    fx += scaleInfo.x;
    scaledYdst.y |= pSrcYImage[(int)fx] << 16;
    fx += scaleInfo.x;
    scaledYdst.y |= pSrcYImage[(int)fx] << 24;

    *((uint2 *)(&pScaledYImage[scaledYIdx])) = scaledYdst;
     //scale the U and V components here, skip scaling for odd lines for NV12
    if (x >= scaledUVWidth || (y>>1) >= scaledUVHeight || (y&1)) {
        return;
    }
   

    uint scaledUVIdx = (y>>1) * scaledUVImageStrideInBytes + x;

    uint2 scaledUVdst;
    pSrcUVImage += srcUVImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    //pSrcVImage += srcUVImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    fx = fmaf((float)x, scaleInfo.x, scaleInfo.z);

    scaledUVdst.x  = pSrcUVImage[(int)fx];
    scaledUVdst.x  |= pSrcUVImage[(int)fx+1] << 8;
    fx += scaleInfo.x;
    scaledUVdst.x |= pSrcUVImage[(int)fx] << 16;
    scaledUVdst.x |= pSrcUVImage[(int)fx+1] << 24;
    fx += scaleInfo.x;
    scaledUVdst.y |= pSrcUVImage[(int)fx] ;
    scaledUVdst.y |= pSrcUVImage[(int)fx+1] << 8;
    fx += scaleInfo.x;
    scaledUVdst.y |= pSrcUVImage[(int)fx] << 16;
    scaledUVdst.y |= pSrcUVImage[(int)fx] << 24;
    *((uint2 *)(&pScaledUVImage[scaledUVIdx])) = scaledUVdst;

}


int HipExec_ScaleImage_YUV420_Nearest(hipStream_t stream, uint32_t scaledYWidth, uint32_t scaledYHeight,
    uint8_t *pHipScaledYImage, uint32_t scaledYImageStrideInBytes, uint32_t srcYWidth, uint32_t srcYHeight,
    const uint8_t *pHipSrcYImage, uint32_t srcYImageStrideInBytes, uint8_t *pHipScaledUImage, uint8_t *pHipScaledVImage,
    const uint8_t *pHipSrcUImage, const uint8_t *pHipSrcVImage) {

    int localThreads_x = 16;
    int localThreads_y = 16;
    int globalThreads_x = (scaledYWidth + 7) >> 3;
    int globalThreads_y = scaledYHeight;

    uint32_t srcUVWidth = srcYWidth / 2;
    uint32_t srcUVHeight = srcYHeight / 2;
    uint32_t srcUVImageStrideInBytes = srcYImageStrideInBytes / 2;

    uint32_t scaledUVWidth = scaledYWidth / 2;
    uint32_t scaledUVHeight = scaledYHeight / 2;
    uint32_t scaledUVImageStrideInBytes = scaledYImageStrideInBytes / 2;

    float xscaleY = (float)((double)srcYWidth / (double)scaledYWidth);
    float yscaleY = (float)((double)srcYHeight / (double)scaledYHeight);
    float xoffsetY = (float)((double)srcYWidth / (double)scaledYWidth * 0.5);
    float yoffsetY = (float)((double)srcYHeight / (double)scaledYHeight * 0.5);

    float xscaleUV = (float)((double)srcUVWidth / (double)scaledUVWidth);
    float yscaleUV = (float)((double)srcUVHeight / (double)scaledUVHeight);
    float xoffsetUV = (float)((double)srcUVWidth / (double)scaledUVWidth * 0.5);
    float yoffsetUV = (float)((double)srcUVHeight / (double)scaledUVHeight * 0.5);

    hipLaunchKernelGGL(Hip_ScaleImage_YUV420_Nearest, dim3(ceil((float)globalThreads_x/localThreads_x), ceil((float)globalThreads_y/localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, scaledYWidth, scaledYHeight, (uchar *)pHipScaledYImage , scaledYImageStrideInBytes,
                        (const uchar *)pHipSrcYImage, srcYImageStrideInBytes, xscaleY, yscaleY, xoffsetY, yoffsetY,
                        scaledUVWidth, scaledUVHeight, (uchar *)pHipScaledUImage, (uchar *)pHipScaledVImage, scaledUVImageStrideInBytes,
                        (const uchar *)pHipSrcUImage, (const uchar *)pHipSrcVImage, srcUVImageStrideInBytes, xscaleUV, yscaleUV, xoffsetUV, yoffsetUV);

    return 0;
}

int HipExec_ScaleImage_NV12_Nearest(hipStream_t stream, uint32_t scaledYWidth, uint32_t scaledYHeight,
    uint8_t *pHipScaledYImage, uint32_t scaledYImageStrideInBytes, uint32_t srcYWidth, uint32_t srcYHeight,
    const uint8_t *pHipSrcYImage, uint32_t srcYImageStrideInBytes, uint8_t *pHipScaledUVImage
    const uint8_t *pHipSrcUVImage) {

    int localThreads_x = 16;
    int localThreads_y = 16;
    int globalThreads_x = (scaledYWidth + 7) >> 3;
    int globalThreads_y = (scaledYHeight + 1) >> 1;

    uint32_t srcUVWidth = srcYWidth / 2;
    uint32_t srcUVHeight = srcYHeight / 2;
    uint32_t srcUVImageStrideInBytes = srcYImageStrideInBytes;

    uint32_t scaledUVWidth = scaledYWidth / 2;
    uint32_t scaledUVHeight = scaledYHeight / 2;
    uint32_t scaledUVImageStrideInBytes = scaledYImageStrideInBytes;

    float xscale = (float)((double)srcYWidth / (double)scaledYWidth);
    float yscale = (float)((double)srcYHeight / (double)scaledYHeight);
    float xoffset = (float)((double)srcYWidth / (double)scaledYWidth * 0.5);
    float yoffset = (float)((double)srcYHeight / (double)scaledYHeight * 0.5);

    hipLaunchKernelGGL(Hip_ScaleImage_NV12_Nearest, dim3(ceil((float)globalThreads_x/localThreads_x), ceil((float)globalThreads_y/localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, scaledYWidth, scaledYHeight, (uchar *)pHipScaledYImage , scaledYImageStrideInBytes,
                        (const uchar *)pHipSrcYImage, srcYImageStrideInBytes, xscale, yscale, xoffset, yoffset,
                        scaledUVWidth, scaledUVHeight, (uchar *)pHipScaledUVImage,  scaledUVImageStrideInBytes,
                        (const uchar *)pHipSrcUVImage, srcUVImageStrideInBytes);

    return 0;
}


__global__ void __attribute__((visibility("default")))
Hip_ChannelExtract_U8U8_U16(uint dstWidth, uint dstHeight,
    uchar *pDstImage1, uchar *pDstImage2, uint dstImageStrideInBytes,
    const uchar *pSrcImage, uint srcImageStrideInBytes) {

    int x = (hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x) * 8;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    unsigned int srcIdx = y * srcImageStrideInBytes + x + x;
    unsigned int dstIdx = y * dstImageStrideInBytes + x;

    uint4 src = *((uint4 *)(&pSrcImage[srcIdx]));
    uint2 dst1, dst2;

    dst1.x = hip_pack(make_float4(hip_unpack0(src.x), hip_unpack2(src.x), hip_unpack0(src.y), hip_unpack2(src.y)));
    dst1.y = hip_pack(make_float4(hip_unpack0(src.z), hip_unpack2(src.z), hip_unpack0(src.w), hip_unpack2(src.w)));
    dst2.x = hip_pack(make_float4(hip_unpack1(src.x), hip_unpack3(src.x), hip_unpack1(src.y), hip_unpack3(src.y)));
    dst2.y = hip_pack(make_float4(hip_unpack1(src.z), hip_unpack3(src.z), hip_unpack1(src.w), hip_unpack3(src.w)));

    *((uint2 *)(&pDstImage1[dstIdx])) = dst1;
    *((uint2 *)(&pDstImage2[dstIdx])) = dst2;

}
int HipExec_ChannelExtract_U8U8_U16(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage1, uint8_t *pHipDstImage2, uint32_t dstImageStrideInBytes,
    const uint8_t *pHipSrcImage1, uint32_t srcImage1StrideInBytes) {
    int localThreads_x = 16, localThreads_y = 16;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = dstHeight;

    hipLaunchKernelGGL(Hip_ChannelExtract_U8U8_U16, dim3(ceil((float)globalThreads_x / localThreads_x), ceil((float)globalThreads_y / localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight, (uchar *)pHipDstImage1, (uchar *)pHipDstImage2, dstImageStrideInBytes,
                        (const uchar *)pHipSrcImage1, srcImage1StrideInBytes);

    return 0;
}

__global__ void __attribute__((visibility("default")))
Hip_ChannelCombine_U16_U8U8(uint dstWidth, uint dstHeight,
    uchar *pDstImage, uint dstImageStrideInBytes,
    const uchar *pSrcImage1, uint srcImage1StrideInBytes,
    const uchar *pSrcImage2, uint srcImage2StrideInBytes) {

    int x = (hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x) * 8;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    uint src1Idx = y * srcImage1StrideInBytes + x;
    uint src2Idx = y * srcImage2StrideInBytes + x;
    uint dstIdx =  y * dstImageStrideInBytes + x + x;

    uint2 src1 = *((uint2 *)(&pSrcImage1[src1Idx]));
    uint2 src2 = *((uint2 *)(&pSrcImage2[src2Idx]));
    uint4 dst;

    dst.x = hip_pack(make_float4(hip_unpack0(src1.x), hip_unpack0(src2.x), hip_unpack1(src1.x), hip_unpack1(src2.x)));
    dst.y = hip_pack(make_float4(hip_unpack2(src1.x), hip_unpack2(src2.x), hip_unpack3(src1.x), hip_unpack3(src2.x)));
    dst.z = hip_pack(make_float4(hip_unpack0(src1.y), hip_unpack0(src2.y), hip_unpack1(src1.y), hip_unpack1(src2.y)));
    dst.w = hip_pack(make_float4(hip_unpack2(src1.y), hip_unpack2(src2.y), hip_unpack3(src1.y), hip_unpack3(src2.y)));

    *((uint4 *)(&pDstImage[dstIdx])) = dst;
}
int HipExec_ChannelCombine_U16_U8U8(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes,
    const uint8_t *pHipSrcImage1, uint32_t srcImage1StrideInBytes,
    const uint8_t *pHipSrcImage2, uint32_t srcImage2StrideInBytes) {
    int localThreads_x = 16;
    int localThreads_y = 16;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = dstHeight;

    hipLaunchKernelGGL(Hip_ChannelCombine_U16_U8U8, dim3(ceil((float)globalThreads_x/localThreads_x), ceil((float)globalThreads_y/localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight, (uchar *)pHipDstImage , dstImageStrideInBytes,
                        (const uchar *)pHipSrcImage1, srcImage1StrideInBytes, (const uchar *)pHipSrcImage2, srcImage2StrideInBytes);

    return 0;
}

__global__ void __attribute__((visibility("default")))
Hip_ScaleImage_U8_U8_Nearest(uint dstWidth, uint dstHeight,
    uchar *pDstImage, uint dstImageStrideInBytes,
    const uchar *pSrcImage, uint srcImageStrideInBytes,
    float xscale, float yscale, float xoffset, float yoffset) {

    int x = (hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x) * 8;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    uint dstIdx = y * dstImageStrideInBytes + x;

    float4 scaleInfo = make_float4(xscale, yscale, xoffset, yoffset);

    uint2 dst;
    pSrcImage += srcImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    float fx = fmaf((float)x, scaleInfo.x, scaleInfo.z);

    dst.x  = pSrcImage[(int)fx];
    fx += scaleInfo.x;
    dst.x |= pSrcImage[(int)fx] << 8;
    fx += scaleInfo.x;
    dst.x |= pSrcImage[(int)fx] << 16;
    fx += scaleInfo.x;
    dst.x |= pSrcImage[(int)fx] << 24;

    fx += scaleInfo.x;

    dst.y  = pSrcImage[(int)fx];
    fx += scaleInfo.x;
    dst.y |= pSrcImage[(int)fx] << 8;
    fx += scaleInfo.x;
    dst.y |= pSrcImage[(int)fx] << 16;
    fx += scaleInfo.x;
    dst.y |= pSrcImage[(int)fx] << 24;

    *((uint2 *)(&pDstImage[dstIdx])) = dst;
}

int HipExec_ScaleImage_U8_U8_Nearest(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstImage, uint32_t dstImageStrideInBytes,
    uint32_t srcWidth, uint32_t srcHeight,
    const uint8_t *pHipSrcImage, uint32_t srcImageStrideInBytes) {
    int localThreads_x = 16;
    int localThreads_y = 16;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = dstHeight;

    float xscale = (float)((double)srcWidth / (double)dstWidth);
    float yscale = (float)((double)srcHeight / (double)dstHeight);
    float xoffset = (float)((double)srcWidth / (double)dstWidth * 0.5);
    float yoffset = (float)((double)srcHeight / (double)dstHeight * 0.5);

    hipLaunchKernelGGL(Hip_ScaleImage_U8_U8_Nearest, dim3(ceil((float)globalThreads_x/localThreads_x), ceil((float)globalThreads_y/localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight, (uchar *)pHipDstImage , dstImageStrideInBytes,
                        (const uchar *)pHipSrcImage, srcImageStrideInBytes,
                        xscale, yscale, xoffset, yoffset);

    return 0;
}

__global__ void __attribute__((visibility("default")))
Hip_ScaleImage_YUV444_Nearest(uint dstWidth, uint dstHeight, uchar *pDstYImage, uchar *pDstUImage, uchar *pDstVImage, uint dstImageStrideInBytes,
    const uchar *pSrcYImage, const uchar *pSrcUImage, const uchar *pSrcVImage, uint srcImageStrideInBytes,
    float xscale, float yscale, float xoffset, float yoffset) {

    int x = (hipBlockDim_x * hipBlockIdx_x + hipThreadIdx_x) * 8;
    int y = hipBlockDim_y * hipBlockIdx_y + hipThreadIdx_y;

    if (x >= dstWidth || y >= dstHeight) {
        return;
    }

    uint dstIdx = y * dstImageStrideInBytes + x;

    float4 scaleInfo = make_float4(xscale, yscale, xoffset, yoffset);

    uint2 yDst, uDst, vDst;
    pSrcYImage += srcImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    pSrcUImage += srcImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);
    pSrcVImage += srcImageStrideInBytes * (uint)fmaf((float)y, scaleInfo.y, scaleInfo.w);

    float fx = fmaf((float)x, scaleInfo.x, scaleInfo.z);

    yDst.x  = pSrcYImage[(int)fx];
    uDst.x  = pSrcUImage[(int)fx];
    vDst.x  = pSrcVImage[(int)fx];
    fx += scaleInfo.x;
    yDst.x |= pSrcYImage[(int)fx] << 8;
    uDst.x |= pSrcUImage[(int)fx] << 8;
    vDst.x |= pSrcVImage[(int)fx] << 8;
    fx += scaleInfo.x;
    yDst.x |= pSrcYImage[(int)fx] << 16;
    uDst.x |= pSrcUImage[(int)fx] << 16;
    vDst.x |= pSrcVImage[(int)fx] << 16;
    fx += scaleInfo.x;
    yDst.x |= pSrcYImage[(int)fx] << 24;
    uDst.x |= pSrcUImage[(int)fx] << 24;
    vDst.x |= pSrcVImage[(int)fx] << 24;
    fx += scaleInfo.x;

    yDst.y  = pSrcYImage[(int)fx];
    uDst.y  = pSrcUImage[(int)fx];
    vDst.y  = pSrcVImage[(int)fx];
    fx += scaleInfo.x;
    yDst.y |= pSrcYImage[(int)fx] << 8;
    uDst.y |= pSrcUImage[(int)fx] << 8;
    vDst.y |= pSrcVImage[(int)fx] << 8;
    fx += scaleInfo.x;
    yDst.y |= pSrcYImage[(int)fx] << 16;
    uDst.y |= pSrcUImage[(int)fx] << 16;
    vDst.y |= pSrcVImage[(int)fx] << 16;
    fx += scaleInfo.x;
    yDst.y |= pSrcYImage[(int)fx] << 24;
    uDst.y |= pSrcUImage[(int)fx] << 24;
    vDst.y |= pSrcVImage[(int)fx] << 24;

    *((uint2 *)(&pDstYImage[dstIdx])) = yDst;
    *((uint2 *)(&pDstUImage[dstIdx])) = uDst;
    *((uint2 *)(&pDstVImage[dstIdx])) = vDst;
}

int HipExec_ScaleImage_YUV444_Nearest(hipStream_t stream, uint32_t dstWidth, uint32_t dstHeight,
    uint8_t *pHipDstYUVImage, uint32_t dstImageStrideInBytes, uint32_t dstUImageOffset,
    uint32_t srcWidth, uint32_t srcHeight, const uint8_t *pHipSrcYUVImage, uint32_t srcImageStrideInBytes, uint32_t srcUImageOffset) {

    int localThreads_x = 16;
    int localThreads_y = 16;
    int globalThreads_x = (dstWidth + 7) >> 3;
    int globalThreads_y = dstHeight;

    float xscale = (float)((double)srcWidth / (double)dstWidth);
    float yscale = (float)((double)srcHeight / (double)dstHeight);
    float xoffset = (float)((double)srcWidth / (double)dstWidth * 0.5);
    float yoffset = (float)((double)srcHeight / (double)dstHeight * 0.5);

    hipLaunchKernelGGL(Hip_ScaleImage_YUV444_Nearest, dim3(ceil((float)globalThreads_x/localThreads_x), ceil((float)globalThreads_y/localThreads_y)),
                        dim3(localThreads_x, localThreads_y), 0, stream, dstWidth, dstHeight,
                        (uchar *)pHipDstYUVImage,
                        (uchar *)pHipDstYUVImage + dstUImageOffset,
                        (uchar *)pHipDstYUVImage + (dstUImageOffset * 2), dstImageStrideInBytes,
                        (const uchar *)pHipSrcYUVImage,
                        (const uchar *)pHipSrcYUVImage + srcUImageOffset,
                        (const uchar *)pHipSrcYUVImage + (srcUImageOffset * 2),
                        srcImageStrideInBytes, xscale, yscale, xoffset, yoffset);

    return 0;
}

