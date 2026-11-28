#include "cuda_preprocess.h"
void cpu_preprocess(const uint8_t* src, int srcWidth, int srcHeight, int srcPitch, float* dst, int dstWidth, int dstHeight) {
	float scaleX = (float)srcWidth / dstWidth;
	float scaleY = (float)srcHeight / dstHeight;
	int chOffset = dstWidth * dstHeight;
	for (int y = 0; y < dstHeight; y++) {
		float srcYf = y * scaleY;
		int srcY0 = (int)srcYf;
		int srcY1 = srcY0 + 1 < srcHeight ? srcY0 + 1 : srcY0;
		float fy = srcYf - srcY0;
		float fy0 = 1.0f - fy;
		for (int x = 0; x < dstWidth; x++) {
			float srcXf = x * scaleX;
			int srcX0 = (int)srcXf;
			int srcX1 = srcX0 + 1 < srcWidth ? srcX0 + 1 : srcX0;
			float fx = srcXf - srcX0;
			float fx0 = 1.0f - fx;
			int idx00 = srcY0 * srcPitch + srcX0 * 4;
			int idx10 = srcY0 * srcPitch + srcX1 * 4;
			int idx01 = srcY1 * srcPitch + srcX0 * 4;
			int idx11 = srcY1 * srcPitch + srcX1 * 4;
			float w00 = fx0 * fy0, w10 = fx * fy0, w01 = fx0 * fy, w11 = fx * fy;
			float r = w00 * src[idx00 + 2] + w10 * src[idx10 + 2] + w01 * src[idx01 + 2] + w11 * src[idx11 + 2];
			float g = w00 * src[idx00 + 1] + w10 * src[idx10 + 1] + w01 * src[idx01 + 1] + w11 * src[idx11 + 1];
			float b = w00 * src[idx00 + 0] + w10 * src[idx10 + 0] + w01 * src[idx01 + 0] + w11 * src[idx11 + 0];
			int pixIdx = y * dstWidth + x;
			dst[0 * chOffset + pixIdx] = r * 0.00392156862f;
			dst[1 * chOffset + pixIdx] = g * 0.00392156862f;
			dst[2 * chOffset + pixIdx] = b * 0.00392156862f;
		}
	}
}
