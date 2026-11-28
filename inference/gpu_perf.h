#pragma once
#include <string>
#include <cstdint>
#include <chrono>
#include <deque>
enum class GpuVendor { Unknown, NVIDIA, AMD, Intel };
enum class GpuTier { Unknown, VeryLow, Low, Mid, MidHigh, High };
struct GpuInfo {
	GpuVendor vendor = GpuVendor::Unknown;
	std::string name;
	int64_t vramBytes = 0;
	int computeMajor = 0;
	int computeMinor = 0;
	int smCount = 0;
	int clockRateKHz = 0;
	GpuTier tier = GpuTier::Unknown;
	int autoTargetFps = 60;
	float vramGB() const { return (float)vramBytes / (1024.0f * 1024.0f * 1024.0f); }
};
class GpuPerf {
public:
	GpuPerf() = default;
	bool detect(int gpuId);
	const GpuInfo& getInfo() const { return m_info; }
	int adjustTargetFps(float inferMs);
	const char* getTierName() const;
	const char* getVendorName() const;
	void addFrameTime(float ms);
	float getAvgFrameTime() const;
	int getCalibratedFps() const { return m_calibratedFps; }
	bool isCalibrated() const { return m_calibrated; }
private:
	GpuInfo m_info;
	bool m_calibrated = false;
	int m_calibratedFps = 0;
	std::deque<float> m_frameTimes;
	static const int CALIBRATE_FRAMES = 30;
	static const int HISTORY_FRAMES = 120;
	int classifyNvidia();
	int classifyAmd();
	int classifyByVram();
};
