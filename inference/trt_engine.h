#pragma once
#include "inference_types.h"
#include "gui/app_log.h"
#include "config/config.h"
#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime.h>
#include <string>
#include <vector>
#include <cstdio>
#include <atomic>
class TRTLogger : public nvinfer1::ILogger {
	void log(Severity severity, const char* msg) noexcept override {
		if (severity <= Severity::kWARNING) {
			if (severity <= Severity::kERROR) LOG_ERROR("[TRT] %s", msg);
			else LOG_WARN("[TRT] %s", msg);
		}
	}
};
class TRTEngine {
public:
	TRTEngine();
	~TRTEngine();
	bool loadModel(const std::string& onnxPath, int gpuId = 0, ProgressCallback progress = nullptr, std::atomic<bool>* cancelFlag = nullptr, TrtPrecision precision = TrtPrecision::FP16, bool mixedPrecision = false);
	bool loadEngine(const std::string& enginePath, int gpuId = 0, ProgressCallback progress = nullptr);
	bool saveEngine(const std::string& enginePath);
	InferenceResult inference(const float* inputData, int inputSize);
	InferenceResult inferenceFromGPU();
	void* getInputGPUBuffer() const { return m_inputBuffer; }
	int getInputSize() const { return m_inputSize; }
	std::vector<int64_t> getInputDims() const { return m_inputDims; }
	int getOutputSize() const { return m_outputSize; }
	std::vector<int64_t> getOutputDims() const { return m_outputDims; }
	cudaStream_t getStream() const { return m_stream; }
	bool isLoaded() const { return m_engine != nullptr; }
	void release();
	void enableCudaGraph();
	bool isCudaGraphEnabled() const { return m_cudaGraphEnabled; }
private:
	TRTLogger m_logger;
	nvinfer1::IRuntime* m_runtime = nullptr;
	nvinfer1::ICudaEngine* m_engine = nullptr;
	nvinfer1::IExecutionContext* m_context = nullptr;
	cudaStream_t m_stream = nullptr;
	cudaEvent_t m_copyEvent = nullptr;
	void* m_inputBuffer = nullptr;
	void* m_outputBuffer = nullptr;
	float* m_outputHostBuf = nullptr;
	int m_outputHostBufSize = 0;
	bool m_outputHostBufPinned = false;
	int m_inputSize = 0;
	int m_outputSize = 0;
	std::vector<int64_t> m_inputDims;
	std::vector<int64_t> m_outputDims;
	std::string m_inputName;
	std::string m_outputName;
	// CUDA Graph优化
	bool m_cudaGraphEnabled = false;
		bool m_cudaGraphCaptured = false;
		int m_graphFailCount = 0;
		cudaGraph_t m_cudaGraph = nullptr;
	cudaGraphExec_t m_cudaGraphExec = nullptr;
	int m_warmupCount = 0;
	static const int WARMUP_FRAMES = 3;
	bool allocateBuffers();
	std::vector<char> readEngineFile(const std::string& path);
	std::string getEngineCachePath(const std::string& onnxPath);
};
