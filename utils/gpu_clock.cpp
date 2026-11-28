#include "gpu_clock.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include <windows.h>
// NVML动态加载, 不引入链接依赖; nvml.dll随NVIDIA驱动安装在System32
typedef int nvmlReturn_t;
typedef void* nvmlDevice_t;
static nvmlReturn_t(*pfn_nvmlInit_v2)() = nullptr;
static nvmlReturn_t(*pfn_nvmlShutdown)() = nullptr;
static nvmlReturn_t(*pfn_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t*) = nullptr;
static nvmlReturn_t(*pfn_nvmlDeviceGetMaxClockInfo)(nvmlDevice_t, int, unsigned int*) = nullptr;
static nvmlReturn_t(*pfn_nvmlDeviceSetGpuLockedClocks)(nvmlDevice_t, unsigned int, unsigned int) = nullptr;
static nvmlReturn_t(*pfn_nvmlDeviceClearGpuLockedClocks)(nvmlDevice_t) = nullptr;
static nvmlDevice_t s_device = nullptr;
static bool s_nvmlInited = false;
static bool s_locked = false;
static HMODULE s_nvmlModule = nullptr;
static bool loadNvml() {
	if (s_nvmlModule) return true;
	const char* candidates[] = { "nvml.dll", "C:\\Windows\\System32\\nvml.dll",
		"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll" };
	for (const char* path : candidates) {
		HMODULE h = LoadLibraryA(path);
		if (!h) continue;
		pfn_nvmlInit_v2 = (nvmlReturn_t(*)())GetProcAddress(h, "nvmlInit_v2");
		pfn_nvmlShutdown = (nvmlReturn_t(*)())GetProcAddress(h, "nvmlShutdown");
		pfn_nvmlDeviceGetHandleByIndex_v2 = (nvmlReturn_t(*)(unsigned int, nvmlDevice_t*))GetProcAddress(h, "nvmlDeviceGetHandleByIndex_v2");
		pfn_nvmlDeviceGetMaxClockInfo = (nvmlReturn_t(*)(nvmlDevice_t, int, unsigned int*))GetProcAddress(h, "nvmlDeviceGetMaxClockInfo");
		pfn_nvmlDeviceSetGpuLockedClocks = (nvmlReturn_t(*)(nvmlDevice_t, unsigned int, unsigned int))GetProcAddress(h, "nvmlDeviceSetGpuLockedClocks");
		// ClearGpuLockedClocks 为可选API(部分驱动版本无此导出), 缺失时用 SetGpuLockedClocks(0,0) 兜底
		pfn_nvmlDeviceClearGpuLockedClocks = (nvmlReturn_t(*)(nvmlDevice_t))GetProcAddress(h, "nvmlDeviceClearGpuLockedClocks");
		if (pfn_nvmlInit_v2 && pfn_nvmlShutdown && pfn_nvmlDeviceGetHandleByIndex_v2 &&
			pfn_nvmlDeviceGetMaxClockInfo && pfn_nvmlDeviceSetGpuLockedClocks) {
			s_nvmlModule = h;
			return true;
		}
		FreeLibrary(h);
	}
	return false;
}
bool gpu_clock::apply(int gpuId, int minPercent) {
	if (s_locked) return true;
	if (!loadNvml()) {
		LOG_WARN(OBS1(u8"GPU锁频: nvml.dll未找到(NVIDIA驱动未安装?), 跳过").c_str());
		return false;
	}
	if (!s_nvmlInited) {
		if (pfn_nvmlInit_v2() != 0) {
			LOG_WARN(OBS2(u8"GPU锁频: NVML初始化失败").c_str());
			return false;
		}
		s_nvmlInited = true;
	}
	if (!s_device) {
		if (pfn_nvmlDeviceGetHandleByIndex_v2((unsigned int)gpuId, &s_device) != 0) {
			LOG_WARN(OBS3(u8"GPU锁频: 设备%d句柄获取失败").c_str(), gpuId);
			return false;
		}
	}
	// NVML_CLOCK_SM = 1
	unsigned int maxSm = 0;
	if (pfn_nvmlDeviceGetMaxClockInfo(s_device, 1, &maxSm) != 0 || maxSm == 0) {
		LOG_WARN(OBS4(u8"GPU锁频: 最大频率查询失败").c_str());
		return false;
	}
	unsigned int minClock = (unsigned int)(maxSm * minPercent / 100);
	// max=0表示不限制最高频率(保留Boost)
	if (pfn_nvmlDeviceSetGpuLockedClocks(s_device, minClock, 0) != 0) {
		// 某些驱动版本不接受max=0, 回退锁死[min, max]
		if (pfn_nvmlDeviceSetGpuLockedClocks(s_device, minClock, maxSm) != 0) {
			LOG_WARN(OBS5(u8"GPU锁频: 锁频调用失败(需管理员权限?)").c_str());
			return false;
		}
	}
	s_locked = true;
	LOG_INFO(OBS6(u8"GPU锁频已生效: 最低%dMHz(最大%uMHz), 推理延迟不再受空载降频影响").c_str(), (int)minClock, maxSm);
	return true;
}
void gpu_clock::clear() {
	if (s_locked && s_device) {
		bool cleared = false;
		if (pfn_nvmlDeviceClearGpuLockedClocks) {
			cleared = (pfn_nvmlDeviceClearGpuLockedClocks(s_device) == 0);
		}
		// [v133] 驱动无Clear导出时, 兜底必须用 (0, maxBoost) 而非 (0,0):
		// 实测部分驱动把(0,0)解读为"锁定到0MHz"→退出软件后GPU被钉死低频→游戏帧率暴跌,
		// 重开软件重新锁85%才恢复。min=0(max不限制)+max=Boost上限 = 等效解除锁频且无副作用。
		if (!cleared && pfn_nvmlDeviceSetGpuLockedClocks) {
			unsigned int maxSm2 = 0;
			if (pfn_nvmlDeviceGetMaxClockInfo(s_device, 1, &maxSm2) == 0 && maxSm2 > 0) {
				cleared = (pfn_nvmlDeviceSetGpuLockedClocks(s_device, 0, maxSm2) == 0);
			}
		}
		if (cleared) LOG_INFO(OBS7(u8"GPU锁频已清除, 恢复系统默认频率").c_str());
		s_locked = false;
	}
	if (s_nvmlInited && pfn_nvmlShutdown) {
		pfn_nvmlShutdown();
		s_nvmlInited = false;
		s_device = nullptr;
	}
}
bool gpu_clock::isActive() { return s_locked; }
