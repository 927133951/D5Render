#pragma once
#include <vector>
#include <cstdint>
#include "config/config.h"
struct Detection {
	float cx, cy, w, h;
	float confidence;
	int classId;
	float x1() const { return cx - w / 2; }
	float y1() const { return cy - h / 2; }
	float x2() const { return cx + w / 2; }
	float y2() const { return cy + h / 2; }
};
class YOLOPostProcess {
public:
	static std::vector<Detection> process(const float* output, const std::vector<int64_t>& dims, YoloVersion version, float confThresh = 0.25f, float nmsThresh = 0.45f, int maxDet = 100, const std::vector<int>& selectedClasses = {});
	static int getNumClasses(const std::vector<int64_t>& dims, YoloVersion version);
	static bool isEndToEnd(const std::vector<int64_t>& dims);
	static const char* getCocoClassName(int classId);
	static const char* getCocoClassDisplayName(int classId);
private:
	static std::vector<Detection> parseEndToEnd(const float* output, const std::vector<int64_t>& dims, float confThresh);
	static std::vector<Detection> parseV5V7(const float* output, const std::vector<int64_t>& dims, float confThresh);
	static std::vector<Detection> parseV8V26(const float* output, const std::vector<int64_t>& dims, float confThresh);
	static float iou(const Detection& a, const Detection& b);
	static std::vector<Detection> nms(std::vector<Detection>& dets, float nmsThresh, int maxDet);
};
