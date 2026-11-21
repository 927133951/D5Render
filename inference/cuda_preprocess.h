#pragma once
#include <cstdint>
void cuda_preprocess(const uint8_t* src, int srcWidth, int srcHeight, int srcPitch, float* dst, int dstWidth, int dstHeight, cudaStream_t stream);
