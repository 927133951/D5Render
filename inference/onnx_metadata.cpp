#include "onnx_metadata.h"
#include "utils/obfstr.h"
#include <fstream>
#include <cstring>
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// 检查字符串是否为有效UTF-8
static bool isValidUtf8(const std::string& str) {
	for (size_t i = 0; i < str.size(); ) {
		unsigned char c = (unsigned char)str[i];
		int len;
		if (c < 0x80) len = 1;
		else if ((c & 0xE0) == 0xC0) len = 2;
		else if ((c & 0xF0) == 0xE0) len = 3;
		else if ((c & 0xF8) == 0xF0) len = 4;
		else return false;
		if (i + len > str.size()) return false;
		for (int j = 1; j < len; j++) {
			if (((unsigned char)str[i + j] & 0xC0) != 0x80) return false;
		}
		i += len;
	}
	return true;
}

// 从系统ANSI编码(中文Windows=GBK)转换为UTF-8
static std::string ansiToUtf8(const std::string& ansi) {
	int wlen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), nullptr, 0);
	if (wlen <= 0) return ansi;
	std::wstring wstr(wlen, 0);
	MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), (int)ansi.size(), &wstr[0], wlen);
	int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
	if (ulen <= 0) return ansi;
	std::string utf8(ulen, 0);
	WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &utf8[0], ulen, nullptr, nullptr);
	return utf8;
}

// 确保字符串是UTF-8：有效则原样返回，无效则从ANSI转换
static std::string ensureUtf8(const std::string& str) {
	if (isValidUtf8(str)) return str;
	return ansiToUtf8(str);
}
static uint32_t readVarint(const uint8_t* data, size_t& pos, size_t size) {
	uint32_t result = 0;
	int shift = 0;
	while (pos < size) {
		uint8_t b = data[pos++];
		result |= (b & 0x7F) << shift;
		if ((b & 0x80) == 0) break;
		shift += 7;
	}
	return result;
}
static std::vector<ClassInfo> parsePythonDict(const std::string& str) {
	std::vector<ClassInfo> classes;
	size_t pos = 0;
	while (pos < str.size()) {
		size_t colon = str.find(':', pos);
		if (colon == std::string::npos) break;
		int key = 0;
		bool foundKey = false;
		for (size_t i = pos; i < colon; ++i) {
			if (str[i] >= '0' && str[i] <= '9') {
				key = key * 10 + (str[i] - '0');
				foundKey = true;
			}
		}
		if (!foundKey) { pos = colon + 1; continue; }
		size_t q1 = str.find_first_of("\"'", colon + 1);
		if (q1 == std::string::npos) break;
		char qc = str[q1];
		size_t q2 = q1 + 1;
		while (q2 < str.size()) {
			if (str[q2] == '\\' && q2 + 1 < str.size()) { q2 += 2; continue; }
			if (str[q2] == qc) break;
			q2++;
		}
		if (q2 >= str.size()) break;
		std::string name = str.substr(q1 + 1, q2 - q1 - 1);
		for (size_t k = 0; k < name.size(); ++k) {
			if (name[k] == '\\' && k + 1 < name.size()) {
				char next = name[k + 1];
				if (next == 'n') { name.replace(k, 2, "\n"); }
				else if (next == 't') { name.replace(k, 2, "\t"); }
				else if (next == '\\') { name.replace(k, 2, "\\"); }
				else if (next == '\'') { name.replace(k, 2, "'"); }
				else if (next == '"') { name.replace(k, 2, "\""); }
				else { name.erase(k, 1); }
			}
		}
			if (key >= (int)classes.size()) classes.resize(key + 1);
			classes[key] = {key, ensureUtf8(name)};
		pos = q2 + 1;
	}
	return classes;
}
std::vector<ClassInfo> extractOnnxClassNames(const std::string& onnxPath) {
	std::vector<ClassInfo> classes;
	std::ifstream file(onnxPath, std::ios::binary | std::ios::ate);
	if (!file.is_open()) return classes;
	size_t fileSize = (size_t)file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> buf(fileSize);
	file.read(reinterpret_cast<char*>(buf.data()), fileSize);
	file.close();
	for (size_t i = 0; i + 8 < fileSize; ++i) {
		if (buf[i] == 0x0A && buf[i + 1] == 0x05 &&
			memcmp(buf.data() + i + 2, OBS1("names").c_str(), 5) == 0 &&
			buf[i + 7] == 0x12) {
			size_t pos = i + 8;
			uint32_t valLen = readVarint(buf.data(), pos, fileSize);
			if (pos + valLen > fileSize) continue;
			std::string value(reinterpret_cast<const char*>(buf.data() + pos), valLen);
			classes = parsePythonDict(value);
			if (!classes.empty()) {
				return classes;
			}
		}
	}
	return classes;
}
static std::string extractMetadataField(const std::vector<uint8_t>& buf, const std::string& fieldName) {
	std::string searchKey = fieldName;
	for (size_t i = 0; i + 8 + searchKey.size() < buf.size(); ++i) {
		if (buf[i] == 0x0A && buf[i + 1] == (uint8_t)searchKey.size() &&
			memcmp(buf.data() + i + 2, searchKey.c_str(), searchKey.size()) == 0 &&
			buf[i + 2 + searchKey.size()] == 0x12) {
			size_t pos = i + 3 + searchKey.size();
			uint32_t valLen = readVarint(buf.data(), pos, buf.size());
			if (pos + valLen > buf.size()) continue;
			return std::string(reinterpret_cast<const char*>(buf.data() + pos), valLen);
		}
	}
	return "";
}
static std::vector<std::string> extractNodeNames(const std::vector<uint8_t>& buf) {
	std::vector<std::string> names;
	for (size_t i = 0; i + 4 < buf.size(); ++i) {
		if (buf[i] == 0x0A && buf[i + 1] >= 0x01 && buf[i + 1] <= 0x40) {
			size_t nameLen = buf[i + 1];
			if (i + 2 + nameLen > buf.size()) continue;
			std::string name(reinterpret_cast<const char*>(buf.data() + i + 2), nameLen);
			bool valid = true;
			for (size_t k = 0; k < name.size(); ++k) {
				char c = name[k];
				if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.' || c == '-')) {
					valid = false;
					break;
				}
			}
			if (valid && nameLen >= 3) names.push_back(name);
		}
	}
	return names;
}
YoloVersionInfo detectYoloVersion(const std::string& onnxPath) {
	YoloVersionInfo info;
	std::ifstream file(onnxPath, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		info.versionName = OBS1(u8"无法打开文件");
		return info;
	}
	size_t fileSize = (size_t)file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> buf(fileSize);
	file.read(reinterpret_cast<char*>(buf.data()), fileSize);
	file.close();
	std::string desc = extractMetadataField(buf, OBS2("description"));
	std::string author = extractMetadataField(buf, OBS3("author"));
	std::string version = extractMetadataField(buf, OBS4("version"));
	std::string modelName = extractMetadataField(buf, OBS5("name"));
	std::string combined = desc + " " + author + " " + version + " " + modelName + " " + onnxPath;
	for (auto& c : combined) if (c >= 'A' && c <= 'Z') c = c + 32;
	auto hasKeyword = [&](const std::string& kw) -> bool {
		return combined.find(kw) != std::string::npos;
	};
	if (hasKeyword(OBS1("yolov5")) || hasKeyword(OBS2("yolo_v5")) || hasKeyword(OBS3("v5"))) {
		if (hasKeyword(OBS4("v5u")) || hasKeyword(OBS5("yolov5u"))) {
			info.version = YoloVersion::V8;
			info.versionName = OBS6("YOLOv5u (V8)");
			info.description = OBS7(u8"检测到YOLOv5u，使用V8后处理格式");
		} else {
			info.version = YoloVersion::V5;
			info.versionName = OBS8("YOLOv5");
			info.description = OBS1(u8"Anchor-based检测头，含objectness");
		}
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS2("yolov6")) || hasKeyword(OBS3("yolo_v6"))) {
		info.version = YoloVersion::V6;
		info.versionName = OBS4("YOLOv6");
		info.description = OBS5(u8"Anchor-based检测头，高效重参数化");
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS6("yolov7")) || hasKeyword(OBS7("yolo_v7"))) {
		info.version = YoloVersion::V7;
		info.versionName = OBS8("YOLOv7");
		info.description = OBS1(u8"Anchor-based检测头，辅助头训练");
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS2("yolov9")) || hasKeyword(OBS3("yolo_v9"))) {
		info.version = YoloVersion::V9;
		info.versionName = OBS4("YOLOv9");
		info.description = OBS5(u8"Anchor-free，PGI+GELAN架构");
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS6("yolov10")) || hasKeyword(OBS7("yolo_v10"))) {
		info.version = YoloVersion::V10;
		info.versionName = OBS8("YOLOv10");
		info.description = OBS1(u8"End-to-End，无NMS后处理");
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS2("yolov11")) || hasKeyword(OBS3("yolo_v11")) || hasKeyword(OBS4("yolo11"))) {
		info.version = YoloVersion::V11;
		info.versionName = OBS5("YOLOv11");
		info.description = OBS6(u8"Anchor-free，最新YOLO架构");
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS7("yolov26")) || hasKeyword(OBS8("yolo_v26")) || hasKeyword(OBS1("yolo26")) || hasKeyword(OBS2("v26"))) {
		info.version = YoloVersion::V26;
		info.versionName = OBS3("YOLOv26");
		info.description = OBS4(u8"End-to-End，无NMS后处理");
		info.detected = true;
		return info;
	}
	if (hasKeyword(OBS5("yolov8")) || hasKeyword(OBS6("yolo_v8")) || hasKeyword(OBS7("yolo8"))) {
		info.version = YoloVersion::V8;
		info.versionName = OBS8("YOLOv8");
		info.description = OBS1(u8"Anchor-free检测头，解耦头");
		info.detected = true;
		return info;
	}
	auto nodeNames = extractNodeNames(buf);
	bool hasV8Nodes = false, hasV5Nodes = false;
	for (auto& n : nodeNames) {
		if (n.find(OBS2("/conv")) != std::string::npos || n.find(OBS3("Conv")) != std::string::npos) hasV5Nodes = true;
		if (n.find(OBS4("dfL")) != std::string::npos || n.find(OBS5("dfl")) != std::string::npos) hasV8Nodes = true;
		if (n.find(OBS6("/bboxes")) != std::string::npos || n.find(OBS7("/scores")) != std::string::npos) hasV8Nodes = true;
	}
	if (hasV8Nodes && !hasV5Nodes) {
		info.version = YoloVersion::V8;
		info.versionName = OBS8(u8"YOLOv8 (节点推断)");
		info.description = OBS1(u8"检测到DFL/解耦头节点，判定为V8+格式");
		info.detected = true;
		return info;
	}
	info.version = YoloVersion::V8;
	info.versionName = OBS2(u8"YOLOv8 (默认)");
	info.description = OBS3(u8"无法从元数据识别版本，使用默认V8格式\n如检测不准确请手动切换版本");
	info.detected = false;
	return info;
}
