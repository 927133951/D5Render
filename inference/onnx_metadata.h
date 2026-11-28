#pragma once
#include <string>
#include <vector>
#include "config/config.h"
struct ClassInfo {
	int id = 0;
	std::string name;
};
struct YoloVersionInfo {
	YoloVersion version = YoloVersion::V8;
	std::string versionName;
	std::string description;
	bool detected = false;
};
std::vector<ClassInfo> extractOnnxClassNames(const std::string& onnxPath);
YoloVersionInfo detectYoloVersion(const std::string& onnxPath);
