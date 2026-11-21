#include "yolo_postprocess.h"
#include "utils/obfstr.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>
static std::string getCocoClassStr(int id) {
	static const std::string names[] = {
		OBS1("person"),OBS2("bicycle"),OBS3("car"),OBS4("motorcycle"),OBS5("airplane"),OBS6("bus"),OBS7("train"),OBS8("truck"),OBS1("boat"),
		OBS2("traffic light"),OBS3("fire hydrant"),OBS4("stop sign"),OBS5("parking meter"),OBS6("bench"),OBS7("bird"),OBS8("cat"),
		OBS1("dog"),OBS2("horse"),OBS3("sheep"),OBS4("cow"),OBS5("elephant"),OBS6("bear"),OBS7("zebra"),OBS8("giraffe"),OBS1("backpack"),
		OBS2("umbrella"),OBS3("handbag"),OBS4("tie"),OBS5("suitcase"),OBS6("frisbee"),OBS7("skis"),OBS8("snowboard"),OBS1("sports ball"),
		OBS2("kite"),OBS3("baseball bat"),OBS4("baseball glove"),OBS5("skateboard"),OBS6("surfboard"),OBS7("tennis racket"),
		OBS8("bottle"),OBS1("wine glass"),OBS2("cup"),OBS3("fork"),OBS4("knife"),OBS5("spoon"),OBS6("bowl"),OBS7("banana"),OBS8("apple"),
		OBS1("sandwich"),OBS2("orange"),OBS3("broccoli"),OBS4("carrot"),OBS5("hot dog"),OBS6("pizza"),OBS7("donut"),OBS8("cake"),OBS1("chair"),
		OBS2("couch"),OBS3("potted plant"),OBS4("bed"),OBS5("dining table"),OBS6("toilet"),OBS7("tv"),OBS8("laptop"),OBS1("mouse"),
		OBS2("remote"),OBS3("keyboard"),OBS4("cell phone"),OBS5("microwave"),OBS6("oven"),OBS7("toaster"),OBS8("sink"),OBS1("refrigerator"),
		OBS2("book"),OBS3("clock"),OBS4("vase"),OBS5("scissors"),OBS6("teddy bear"),OBS7("hair drier"),OBS8("toothbrush")
	};
	if (id >= 0 && id < 80) return names[id];
	return OBS1("unknown");
}
static std::string getCocoDisplayStr(int id) {
	static const std::string names[] = {
		OBS1(u8"人"),OBS2(u8"自行车"),OBS3(u8"汽车"),OBS4(u8"摩托车"),OBS5(u8"飞机"),OBS6(u8"公交车"),OBS7(u8"火车"),OBS8(u8"卡车"),OBS1(u8"船"),
		OBS2(u8"红绿灯"),OBS3(u8"消防栓"),OBS4(u8"停车标志"),OBS5(u8"停车计时器"),OBS6(u8"长椅"),OBS7(u8"鸟"),OBS8(u8"猫"),
		OBS1(u8"狗"),OBS2(u8"马"),OBS3(u8"羊"),OBS4(u8"牛"),OBS5(u8"大象"),OBS6(u8"熊"),OBS7(u8"斑马"),OBS8(u8"长颈鹿"),OBS1(u8"背包"),
		OBS2(u8"雨伞"),OBS3(u8"手提包"),OBS4(u8"领带"),OBS5(u8"行李箱"),OBS6(u8"飞盘"),OBS7(u8"滑雪板"),OBS8(u8"滑雪板"),OBS1(u8"球"),
		OBS2(u8"风筝"),OBS3(u8"棒球棒"),OBS4(u8"棒球手套"),OBS5(u8"滑板"),OBS6(u8"冲浪板"),OBS7(u8"网球拍"),
		OBS8(u8"瓶子"),OBS1(u8"酒杯"),OBS2(u8"杯子"),OBS3(u8"叉子"),OBS4(u8"刀"),OBS5(u8"勺子"),OBS6(u8"碗"),OBS7(u8"香蕉"),OBS8(u8"苹果"),
		OBS1(u8"三明治"),OBS2(u8"橙子"),OBS3(u8"西兰花"),OBS4(u8"胡萝卜"),OBS5(u8"热狗"),OBS6(u8"披萨"),OBS7(u8"甜甜圈"),OBS8(u8"蛋糕"),OBS1(u8"椅子"),
		OBS2(u8"沙发"),OBS3(u8"盆栽"),OBS4(u8"床"),OBS5(u8"餐桌"),OBS6(u8"马桶"),OBS7(u8"电视"),OBS8(u8"笔记本电脑"),OBS1(u8"鼠标"),
		OBS2(u8"遥控器"),OBS3(u8"键盘"),OBS4(u8"手机"),OBS5(u8"微波炉"),OBS6(u8"烤箱"),OBS7(u8"烤面包机"),OBS8(u8"水槽"),OBS1(u8"冰箱"),
		OBS2(u8"书"),OBS3(u8"时钟"),OBS4(u8"花瓶"),OBS5(u8"剪刀"),OBS6(u8"泰迪熊"),OBS7(u8"吹风机"),OBS8(u8"牙刷")
	};
	if (id >= 0 && id < 80) return names[id];
	return OBS1(u8"未知");
}
const char* YOLOPostProcess::getCocoClassName(int classId) {
	static std::string cache[80];
	static bool init = false;
	if (!init) {
		for (int i = 0; i < 80; ++i) cache[i] = getCocoClassStr(i);
		init = true;
	}
	if (classId >= 0 && classId < 80) return cache[classId].c_str();
	static std::string unk = OBS2("unknown");
	return unk.c_str();
}
const char* YOLOPostProcess::getCocoClassDisplayName(int classId) {
	static std::string cache[80];
	static bool init = false;
	if (!init) {
		for (int i = 0; i < 80; ++i) cache[i] = getCocoDisplayStr(i);
		init = true;
	}
	if (classId >= 0 && classId < 80) return cache[classId].c_str();
	static std::string unk = OBS2(u8"未知");
	return unk.c_str();
}
int YOLOPostProcess::getNumClasses(const std::vector<int64_t>& dims, YoloVersion version) {
	if (dims.size() < 3) return 0;
	if (isEndToEnd(dims)) return 0;
	if (version == YoloVersion::V5 || version == YoloVersion::V6 || version == YoloVersion::V7) {
		int detCount = (int)dims[1];
		int detLen = (int)dims[2];
		if (detCount > 0 && detLen > detCount) std::swap(detCount, detLen);
		return detLen - 5;
	}
	int d1 = (int)dims[1], d2 = (int)dims[2];
	if (d1 < 5) return 0;
	return d1 - 4;
}
bool YOLOPostProcess::isEndToEnd(const std::vector<int64_t>& dims) {
	if (dims.size() < 3) return false;
	int d1 = (int)dims[1], d2 = (int)dims[2];
	return (d2 == 6 || d2 == 7) && d1 > d2;
}
std::vector<Detection> YOLOPostProcess::process(const float* output, const std::vector<int64_t>& dims, YoloVersion version, float confThresh, float nmsThresh, int maxDet, const std::vector<int>& selectedClasses) {
	std::vector<Detection> dets;
	if (isEndToEnd(dims)) {
		dets = parseEndToEnd(output, dims, confThresh);
		if (!selectedClasses.empty()) {
			std::unordered_set<int> classSet(selectedClasses.begin(), selectedClasses.end());
			dets.erase(std::remove_if(dets.begin(), dets.end(), [&](const Detection& d) {
				return classSet.find(d.classId) == classSet.end();
			}), dets.end());
		}
		if ((int)dets.size() > maxDet) dets.resize(maxDet);
		return dets;
	}
	if (version == YoloVersion::V5 || version == YoloVersion::V6 || version == YoloVersion::V7) {
		dets = parseV5V7(output, dims, confThresh);
	} else {
		dets = parseV8V26(output, dims, confThresh);
	}
	if (!selectedClasses.empty()) {
		std::unordered_set<int> classSet(selectedClasses.begin(), selectedClasses.end());
		dets.erase(std::remove_if(dets.begin(), dets.end(), [&](const Detection& d) {
			return classSet.find(d.classId) == classSet.end();
		}), dets.end());
	}
	return nms(dets, nmsThresh, maxDet);
}
std::vector<Detection> YOLOPostProcess::parseEndToEnd(const float* output, const std::vector<int64_t>& dims, float confThresh) {
	std::vector<Detection> dets;
	if (dims.size() < 3) return dets;
	int detCount = (int)dims[1];
	int valCount = (int)dims[2];
	for (int i = 0; i < detCount; ++i) {
		const float* row = output + i * valCount;
		float x1 = row[0], y1 = row[1], x2 = row[2], y2 = row[3];
		float conf = row[4];
		int classId = (int)row[5];
		if (conf < confThresh) continue;
		if (x2 <= x1 || y2 <= y1) continue;
		if (classId < 0) continue;
		Detection d;
		d.cx = (x1 + x2) / 2.0f;
		d.cy = (y1 + y2) / 2.0f;
		d.w = x2 - x1;
		d.h = y2 - y1;
		d.confidence = conf;
		d.classId = classId;
		dets.push_back(d);
	}
	std::sort(dets.begin(), dets.end(), [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
	return dets;
}
std::vector<Detection> YOLOPostProcess::parseV5V7(const float* output, const std::vector<int64_t>& dims, float confThresh) {
	std::vector<Detection> dets;
	if (dims.size() < 3) return dets;
	int detCount = (int)dims[1];
	int detLen = (int)dims[2];
	if (detCount > 0 && detLen > detCount) std::swap(detCount, detLen);
	int numClasses = detLen - 5;
	// 边界检查：确保不会越界访问输出缓冲区
	int64_t totalElements = 1;
	for (auto d : dims) totalElements *= d;
	for (int i = 0; i < detCount; ++i) {
		if ((int64_t)(i + 1) * detLen > totalElements) break;
		const float* row = output + i * detLen;
		float objConf = row[4];
		if (objConf < confThresh) continue;
		int bestClass = 0;
		float bestScore = 0;
		for (int c = 0; c < numClasses; ++c) {
			float score = row[5 + c] * objConf;
			if (score > bestScore) { bestScore = score; bestClass = c; }
		}
		if (bestScore < confThresh) continue;
		Detection d;
		d.cx = row[0]; d.cy = row[1]; d.w = row[2]; d.h = row[3];
		d.confidence = bestScore; d.classId = bestClass;
		dets.push_back(d);
	}
	return dets;
}
std::vector<Detection> YOLOPostProcess::parseV8V26(const float* output, const std::vector<int64_t>& dims, float confThresh) {
	std::vector<Detection> dets;
	if (dims.size() < 3) return dets;
	int channels = (int)dims[1];
	int detCount = (int)dims[2];
	if (channels < 5) return dets;
	int numClasses = channels - 4;
	// 边界检查
	int64_t totalElements = 1;
	for (auto d : dims) totalElements *= d;
	for (int i = 0; i < detCount; ++i) {
		if ((int64_t)(4 + numClasses) * detCount > totalElements) break;
		int bestClass = 0;
		float bestScore = 0;
		for (int c = 0; c < numClasses; ++c) {
			float score = output[(4 + c) * detCount + i];
			if (score > bestScore) { bestScore = score; bestClass = c; }
		}
		if (bestScore < confThresh) continue;
		Detection d;
		d.cx = output[0 * detCount + i];
		d.cy = output[1 * detCount + i];
		d.w = output[2 * detCount + i];
		d.h = output[3 * detCount + i];
		d.confidence = bestScore;
		d.classId = bestClass;
		dets.push_back(d);
	}
	return dets;
}
float YOLOPostProcess::iou(const Detection& a, const Detection& b) {
	float ax1 = a.x1(), ay1 = a.y1(), ax2 = a.x2(), ay2 = a.y2();
	float bx1 = b.x1(), by1 = b.y1(), bx2 = b.x2(), by2 = b.y2();
	float ix1 = std::max(ax1, bx1), iy1 = std::max(ay1, by1);
	float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
	float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
	float inter = iw * ih;
	float areaA = a.w * a.h, areaB = b.w * b.h;
	return inter / (areaA + areaB - inter + 1e-6f);
}
std::vector<Detection> YOLOPostProcess::nms(std::vector<Detection>& dets, float nmsThresh, int maxDet) {
	std::sort(dets.begin(), dets.end(), [](const Detection& a, const Detection& b) { return a.confidence > b.confidence; });
	std::vector<bool> suppressed(dets.size(), false);
	std::vector<Detection> result;
	for (size_t i = 0; i < dets.size() && (int)result.size() < maxDet; ++i) {
		if (suppressed[i]) continue;
		result.push_back(dets[i]);
		for (size_t j = i + 1; j < dets.size(); ++j) {
			if (suppressed[j]) continue;
			if (iou(dets[i], dets[j]) > nmsThresh) suppressed[j] = true;
		}
	}
	return result;
}
