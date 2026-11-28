#pragma once
#include "inference_types.h"
#include <string>
#include <vector>
#include <atomic>
#include <memory>
namespace ov {
class Core;
class CompiledModel;
class InferRequest;
}
enum class OVDevice { Auto = 0, CPU = 1, GPU = 2, NPU = 3 };
class OVEngine {
public:
	OVEngine();
	~OVEngine();
	bool loadModel(const std::string& onnxPath, OVDevice device = OVDevice::Auto, ProgressCallback progress = nullptr, std::atomic<bool>* cancelFlag = nullptr);
	InferenceResult inference(float* inputData, int inputSize);
	InferenceResult inferenceDirect();
	int getInputSize() const { return m_inputSize; }
	std::vector<int64_t> getInputDims() const { return m_inputShape; }
	int getOutputSize() const { return m_outputSize; }
	std::vector<int64_t> getOutputDims() const { return m_outputDims; }
	bool isLoaded() const { return m_compiledModel != nullptr; }
	float* getInputHostBuf() { return m_tensorDataPtr ? m_tensorDataPtr : (m_inputBufPinned ? m_inputBufPinned : m_inputBuf.data()); }
	void* getInputGPUBuffer() const { return m_inputGPUBuffer; }
	void release();
	static std::vector<std::string> getAvailableDevices();
private:
	void* m_core = nullptr;
	void* m_compiledModel = nullptr;
	void* m_inferRequest = nullptr;
	std::vector<float> m_inputBuf;
	float* m_inputBufPinned = nullptr;
	void* m_inputGPUBuffer = nullptr;
	int m_inputSize = 0;
	int m_outputSize = 0;
	std::vector<int64_t> m_inputShape;
	std::vector<int64_t> m_outputDims;
	std::string m_inputName;
	std::string m_outputName;
	float* m_tensorDataPtr = nullptr;
	bool m_warmupDone = false;
};
