#pragma once
// GPU时钟锁定: 防止空载降频导致推理延迟虚高
// (软件开着但游戏没全速跑时, NVIDIA驱动把核心降到低功耗档, 640模型推理从2-4ms涨到7ms+)
namespace gpu_clock {
	// 锁定最低频率为最大SM频率的指定百分比(默认85%: 低于温度墙避免拉锯震荡, 显著快于70%)
	// 返回true=锁定成功; 已锁定则返回true
	bool apply(int gpuId, int minPercent = 85);
	// 清除锁定并关闭NVML
	void clear();
	bool isActive();
}
