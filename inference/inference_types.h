#pragma once
#include <vector>
#include <cstdint>
#include <functional>
struct InferenceResult {
	std::vector<float> output;
	std::vector<int64_t> outputDims;
	// [v184] 直接输出指针: 指向引擎内部pinned缓冲, 省去每帧~1.2MB的堆分配+拷贝。
	// 仅在同步推理调用返回后、下一次推理前有效(推理线程内同步使用)。非空时优先于output
	const float* directPtr = nullptr;
};
using ProgressCallback = std::function<void(int, const char*)>;
