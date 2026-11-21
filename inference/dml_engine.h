#pragma once
#include "inference_types.h"
#include <cuda_runtime.h>
#include <string>
#include <vector>
#include <atomic>
class DMLEngine {
public:
	DMLEngine();
	~DMLEngine();
	bool loadModel(const std::string& onnxPath, int gpuId = 0, ProgressCallback progress = nullptr, std::atomic<bool>* cancelFlag = nullptr);
	InferenceResult inference(float* inputData, int inputSize);
	InferenceResult inferenceFromGPU();
	int getInputSize() const { return m_inputSize; }
	std::vector<int64_t> getInputDims() const { return m_inputShape; }
	int getOutputSize() const { return m_outputSize; }
	std::vector<int64_t> getOutputDims() const { return m_outputDims; }
	bool isLoaded() const { return m_session != nullptr; }
	bool isGpuAvailable() const { return m_gpuAvailable; }
	bool isDmlAvailable() const { return m_gpuAvailable; }
	void* getInputGPUBuffer() const { return m_inputGPUBuffer; }
	cudaStream_t getStream() const { return m_stream; }
	float* getInputHostBuf() const { return m_inputBufPinned ? m_inputBufPinned : const_cast<float*>(m_inputBuf.data()); }
	void release();
private:
	void* m_env = nullptr;
	void* m_session = nullptr;
	void* m_sessionOptions = nullptr;
	void* m_binding = nullptr;
	void* m_memInfo = nullptr;
	cudaStream_t m_stream = nullptr;
	cudaEvent_t m_copyEvent = nullptr;
	void* m_inputGPUBuffer = nullptr;
	void* m_outputGPUBuffer = nullptr;
	float* m_inputBufPinned = nullptr;
	float* m_outputHostBuf = nullptr;
	int m_outputHostBufSize = 0;
	bool m_outputHostBufPinned = false;
	std::vector<float> m_inputBuf;
	int m_inputSize = 0;
	int m_outputSize = 0;
	bool m_gpuAvailable = false;
	std::vector<int64_t> m_outputDims;
	std::string m_inputName;
	std::string m_outputName;
	std::vector<int64_t> m_inputShape;
};
