#include "gpu_perf.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <cctype>
#include <dxgi.h>
#include "utils/obfstr.h"
#pragma comment(lib, "dxgi.lib")
static std::string toLower(const std::string& s) {
	std::string r = s;
	for (auto& c : r) c = (char)std::tolower((unsigned char)c);
	return r;
}
static bool nameContains(const std::string& name, const std::string& keyword) {
	return toLower(name).find(toLower(keyword)) != std::string::npos;
}
bool GpuPerf::detect(int gpuId) {
	int deviceCount = 0;
	cudaError_t err = cudaGetDeviceCount(&deviceCount);
	if (err == cudaSuccess && deviceCount > gpuId && gpuId >= 0) {
		cudaDeviceProp prop;
		err = cudaGetDeviceProperties(&prop, gpuId);
		if (err == cudaSuccess) {
			m_info.name = prop.name;
			m_info.vramBytes = (int64_t)prop.totalGlobalMem;
			m_info.computeMajor = prop.major;
			m_info.computeMinor = prop.minor;
			m_info.smCount = prop.multiProcessorCount;
			m_info.clockRateKHz = 0;
			if (nameContains(m_info.name, OBS1("nvidia")) || nameContains(m_info.name, OBS2("geforce")) ||
				nameContains(m_info.name, OBS3("rtx")) || nameContains(m_info.name, OBS4("gtx"))) {
				m_info.vendor = GpuVendor::NVIDIA;
				m_info.autoTargetFps = classifyNvidia();
			} else if (nameContains(m_info.name, OBS1("amd")) || nameContains(m_info.name, OBS2("radeon")) ||
				nameContains(m_info.name, OBS3("rx "))) {
				m_info.vendor = GpuVendor::AMD;
				m_info.autoTargetFps = classifyAmd();
			} else {
				m_info.vendor = GpuVendor::Unknown;
				m_info.autoTargetFps = classifyByVram();
			}
			return true;
		}
	}
	IDXGIFactory* factory = nullptr;
	if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory))) return false;
	IDXGIAdapter* adapter = nullptr;
	if (FAILED(factory->EnumAdapters(gpuId, &adapter))) {
		factory->Release();
		return false;
	}
	DXGI_ADAPTER_DESC desc;
	adapter->GetDesc(&desc);
	char nameA[128] = {};
	wcstombs(nameA, desc.Description, sizeof(nameA) - 1);
	m_info.name = nameA;
	m_info.vramBytes = (int64_t)desc.DedicatedVideoMemory;
	if (desc.VendorId == 0x10DE) m_info.vendor = GpuVendor::NVIDIA;
	else if (desc.VendorId == 0x1002) m_info.vendor = GpuVendor::AMD;
	else if (desc.VendorId == 0x8086) m_info.vendor = GpuVendor::Intel;
	if (m_info.vendor == GpuVendor::NVIDIA) m_info.autoTargetFps = classifyNvidia();
	else if (m_info.vendor == GpuVendor::AMD) m_info.autoTargetFps = classifyAmd();
	else m_info.autoTargetFps = classifyByVram();
	adapter->Release();
	factory->Release();
	return true;
}
int GpuPerf::classifyNvidia() {
	int cc = m_info.computeMajor * 10 + m_info.computeMinor;
	float vram = m_info.vramGB();
	int sm = m_info.smCount;
	// Ada Lovelace (RTX 40系列): CC 8.9
	// 帧率需考虑游戏占用GPU，保守估计：推理独占帧率 × 0.6
	if (cc >= 89) {
		if (sm >= 128) { m_info.tier = GpuTier::High; return 90; }       // 4090: 144→90
		if (sm >= 80) { m_info.tier = GpuTier::MidHigh; return 75; }     // 4080: 120→75
		if (sm >= 46) { m_info.tier = GpuTier::Mid; return 55; }         // 4070: 90→55
		if (sm >= 36) { m_info.tier = GpuTier::Low; return 45; }         // 4060: 60→45
		m_info.tier = GpuTier::Low; return 40;
	}
	// Ampere (RTX 30系列): CC 8.6
	if (cc >= 86) {
		if (sm >= 84 && vram >= 10) { m_info.tier = GpuTier::High; return 75; }    // 3090: 120→75
		if (sm >= 56) { m_info.tier = GpuTier::MidHigh; return 60; }               // 3080: 100→60
		if (sm >= 28) { m_info.tier = GpuTier::Mid; return 50; }                   // 3070: 75→50
		m_info.tier = GpuTier::Low; return 40;                                      // 3060: 55→40
	}
	// Turing (RTX 20系列): CC 7.5
	if (cc >= 75) {
		if (sm >= 68) { m_info.tier = GpuTier::MidHigh; return 55; }    // 2080Ti: 90→55
		if (sm >= 36) { m_info.tier = GpuTier::Mid; return 45; }        // 2070/2060: 70→45
		m_info.tier = GpuTier::Low; return 35;
	}
	// Pascal (GTX 10系列): CC 6.0-6.2
	if (cc >= 60) {
		if (sm >= 40) { m_info.tier = GpuTier::Mid; return 40; }
		m_info.tier = GpuTier::Low; return 30;
	}
	// Maxwell及更早
	m_info.tier = GpuTier::VeryLow;
	return 25;
}
int GpuPerf::classifyAmd() {
	const std::string& n = m_info.name;
	// 帧率需考虑游戏占用GPU，保守估计
	if (nameContains(n, OBS1("9070"))) { m_info.tier = GpuTier::MidHigh; return 60; }
	if (nameContains(n, OBS2("7900"))) { m_info.tier = GpuTier::High; return 55; }
	if (nameContains(n, OBS3("7800"))) { m_info.tier = GpuTier::MidHigh; return 50; }
	if (nameContains(n, OBS4("7700"))) { m_info.tier = GpuTier::Mid; return 45; }
	if (nameContains(n, OBS1("7600"))) { m_info.tier = GpuTier::Low; return 40; }
	if (nameContains(n, OBS2("7500"))) { m_info.tier = GpuTier::VeryLow; return 35; }
	if (nameContains(n, OBS3("6950"))) { m_info.tier = GpuTier::High; return 55; }
	if (nameContains(n, OBS4("6900"))) { m_info.tier = GpuTier::High; return 50; }
	if (nameContains(n, OBS1("6800"))) { m_info.tier = GpuTier::MidHigh; return 45; }
	if (nameContains(n, OBS2("6750"))) { m_info.tier = GpuTier::Mid; return 40; }
	if (nameContains(n, OBS3("6700"))) { m_info.tier = GpuTier::Mid; return 40; }
	if (nameContains(n, OBS4("6600"))) { m_info.tier = GpuTier::Low; return 35; }
	if (nameContains(n, OBS1("6500"))) { m_info.tier = GpuTier::VeryLow; return 30; }
	if (nameContains(n, OBS2("5700"))) { m_info.tier = GpuTier::Mid; return 35; }
	if (nameContains(n, OBS3("5600"))) { m_info.tier = GpuTier::Low; return 30; }
	if (nameContains(n, OBS4("5500"))) { m_info.tier = GpuTier::VeryLow; return 25; }
	if (nameContains(n, OBS1("580")) || nameContains(n, OBS2("590"))) { m_info.tier = GpuTier::Low; return 30; }
	if (nameContains(n, OBS3("480")) || nameContains(n, OBS4("470"))) { m_info.tier = GpuTier::VeryLow; return 25; }
	if (nameContains(n, OBS1("vega"))) { m_info.tier = GpuTier::Mid; return 35; }
	return classifyByVram();
}
int GpuPerf::classifyByVram() {
	float vram = m_info.vramGB();
	if (vram >= 20) { m_info.tier = GpuTier::High; return 55; }
	if (vram >= 12) { m_info.tier = GpuTier::MidHigh; return 45; }
	if (vram >= 8) { m_info.tier = GpuTier::Mid; return 40; }
	if (vram >= 6) { m_info.tier = GpuTier::Low; return 30; }
	if (vram >= 4) { m_info.tier = GpuTier::VeryLow; return 25; }
	m_info.tier = GpuTier::VeryLow;
	return 20;
}
int GpuPerf::adjustTargetFps(float inferMs) {
	if (inferMs <= 0.1f) return m_info.autoTargetFps;
	// headroom从1.3提高到1.8，给游戏渲染留更多GPU余量
	float headroom = 1.8f;
	int calculated = (int)(1000.0f / (inferMs * headroom));
	calculated = std::clamp(calculated, 20, 144);
	calculated = std::min(calculated, m_info.autoTargetFps);
	m_info.autoTargetFps = calculated;
	return calculated;
}
void GpuPerf::addFrameTime(float ms) {
	m_frameTimes.push_back(ms);
	if ((int)m_frameTimes.size() > HISTORY_FRAMES) m_frameTimes.pop_front();
	if (!m_calibrated && (int)m_frameTimes.size() >= CALIBRATE_FRAMES) {
		float sum = 0;
		for (int i = 0; i < CALIBRATE_FRAMES; ++i) sum += m_frameTimes[i];
		float avg = sum / CALIBRATE_FRAMES;
		m_calibratedFps = adjustTargetFps(avg);
		m_calibrated = true;
	}
	if (m_calibrated && (int)m_frameTimes.size() % 60 == 0) {
		int n = std::min((int)m_frameTimes.size(), 60);
		float sum = 0;
		for (int i = (int)m_frameTimes.size() - n; i < (int)m_frameTimes.size(); ++i) sum += m_frameTimes[i];
		float avg = sum / n;
		m_calibratedFps = adjustTargetFps(avg);
	}
}
float GpuPerf::getAvgFrameTime() const {
	if (m_frameTimes.empty()) return 0;
	float sum = 0;
	for (float t : m_frameTimes) sum += t;
	return sum / m_frameTimes.size();
}
const char* GpuPerf::getTierName() const {
	switch (m_info.tier) {
	case GpuTier::High: { static std::string s = OBS1(u8"旗舰级"); return s.c_str(); }
	case GpuTier::MidHigh: { static std::string s = OBS2(u8"高性能"); return s.c_str(); }
	case GpuTier::Mid: { static std::string s = OBS3(u8"中端"); return s.c_str(); }
	case GpuTier::Low: { static std::string s = OBS4(u8"入门级"); return s.c_str(); }
	case GpuTier::VeryLow: { static std::string s = OBS1(u8"低端"); return s.c_str(); }
	default: { static std::string s = OBS2(u8"未知"); return s.c_str(); }
	}
}
const char* GpuPerf::getVendorName() const {
	switch (m_info.vendor) {
	case GpuVendor::NVIDIA: { static std::string s = OBS3("NVIDIA"); return s.c_str(); }
	case GpuVendor::AMD: { static std::string s = OBS4("AMD"); return s.c_str(); }
	case GpuVendor::Intel: { static std::string s = OBS1("Intel"); return s.c_str(); }
	default: { static std::string s = OBS2(u8"未知"); return s.c_str(); }
	}
}
