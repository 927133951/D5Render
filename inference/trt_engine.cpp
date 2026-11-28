#include "trt_engine.h"
#include "utils/obfstr.h"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <windows.h>
TRTEngine::TRTEngine() {}
TRTEngine::~TRTEngine() { release(); }
bool TRTEngine::loadModel(const std::string& onnxPath, int gpuId, ProgressCallback progress, std::atomic<bool>* cancelFlag, TrtPrecision precision, bool mixedPrecision) {
	release();
	if (!std::filesystem::exists(onnxPath)) {
		LOG_ERROR(OBS1(u8"ONNX模型文件不存在: %s").c_str(), onnxPath.c_str());
		if (progress) progress(0, OBS2("ONNX模型文件不存在").c_str());
		return false;
	}
	std::string engineCache = getEngineCachePath(onnxPath);
		static std::string sFp16 = OBS1(".fp16");
		static std::string sFp32Mix = OBS2(".fp32mixed");
		static std::string sFp32 = OBS3(".fp32");
		const std::string& precSuffix = (precision == TrtPrecision::FP16) ? sFp16 : (mixedPrecision ? sFp32Mix : sFp32);
		engineCache += precSuffix;
		engineCache += OBS4(".sp");  // SPARSE_WEIGHTS标志启用后旧缓存必须重建, 用后缀区分
	if (std::filesystem::exists(engineCache)) {
		auto onnxTime = std::filesystem::last_write_time(onnxPath);
		auto cacheTime = std::filesystem::last_write_time(engineCache);
		if (cacheTime >= onnxTime) {
			if (progress) progress(10, OBS6(u8"正在加载引擎缓存...").c_str());
			if (loadEngine(engineCache, gpuId, progress)) {
				if (progress) progress(100, OBS6(u8"引擎缓存加载完成").c_str());
				return true;
			}
			LOG_WARN(OBS4(u8"TRT缓存加载失败，重新构建引擎").c_str());
		} else {
			LOG_INFO(OBS5(u8"TRT缓存过期，重新构建引擎").c_str());
		}
	}
	if (cancelFlag && cancelFlag->load()) return false;
	if (progress) progress(15, OBS6(u8"正在初始化构建器...").c_str());
	cudaSetDevice(gpuId);
	nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(m_logger);
	if (!builder) { LOG_ERROR(OBS3(u8"创建TRT Builder失败").c_str()); if (progress) progress(0, OBS6(u8"创建Builder失败").c_str()); return false; }
	if (cancelFlag && cancelFlag->load()) { delete builder; return false; }
	if (progress) progress(20, OBS6(u8"正在创建网络定义...").c_str());
	uint32_t flags = 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);
	nvinfer1::INetworkDefinition* network = builder->createNetworkV2(flags);
	if (!network) { delete builder; LOG_ERROR(OBS3(u8"创建TRT Network失败").c_str()); if (progress) progress(0, OBS6(u8"创建Network失败").c_str()); return false; }
	if (cancelFlag && cancelFlag->load()) { delete network; delete builder; return false; }
	if (progress) progress(25, OBS6(u8"正在解析ONNX模型...").c_str());
	nvonnxparser::IParser* parser = nvonnxparser::createParser(*network, m_logger);
	if (!parser) { delete network; delete builder; LOG_ERROR(OBS3(u8"创建TRT Parser失败").c_str()); if (progress) progress(0, OBS6(u8"创建Parser失败").c_str()); return false; }
	if (!parser->parseFromFile(onnxPath.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING))) {
		LOG_ERROR(OBS3(u8"解析ONNX模型失败").c_str());
		delete parser; delete network; delete builder;
		if (progress) progress(0, OBS6(u8"解析ONNX模型失败").c_str());
		return false;
	}
	if (cancelFlag && cancelFlag->load()) { delete parser; delete network; delete builder; return false; }
	if (progress) progress(40, OBS6(u8"正在配置构建参数...").c_str());
	nvinfer1::IBuilderConfig* config = builder->createBuilderConfig();
	if (!config) { delete parser; delete network; delete builder; return false; }
	// 2GB tactic空间: 更大的workspace让卷积/矩阵层搜索到更快的算法实现
	// (512MB时部分层被迫选慢速tactic)。只影响构建期搜索, 运行时显存占用不变。
	config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 2ULL * 1024ULL * 1024ULL * 1024ULL);
	if (precision == TrtPrecision::FP16) {
		if (builder->platformHasFastFp16()) {
			config->setFlag(nvinfer1::BuilderFlag::kFP16);
			LOG_INFO(OBS5(u8"TRT精度: FP16半精度").c_str());
		} else {
			LOG_ERROR(OBS3(u8"GPU不支持FP16，无法构建引擎").c_str());
			delete config; delete parser; delete network; delete builder;
			if (progress) progress(0, OBS6(u8"GPU不支持FP16").c_str());
			return false;
		}
	} else if (precision == TrtPrecision::FP32) {
		if (mixedPrecision && builder->platformHasFastFp16()) {
			config->setFlag(nvinfer1::BuilderFlag::kFP16);
			LOG_INFO(OBS5(u8"TRT精度: FP32+混合精度优化(允许部分层FP16加速)").c_str());
		} else {
			LOG_INFO(OBS5(u8"TRT精度: FP32全精度").c_str());
		}
	}
	config->setFlag(nvinfer1::BuilderFlag::kTF32);
	// Ampere稀疏权重: 结构化剪枝kernel在RTX30系免费提速5-10%, 旧显卡自动忽略
	config->setFlag(nvinfer1::BuilderFlag::kSPARSE_WEIGHTS);
	if (cancelFlag && cancelFlag->load()) { delete config; delete parser; delete network; delete builder; return false; }
	if (progress) progress(50, OBS6(u8"正在构建TensorRT引擎(可能需要数分钟)...").c_str());
	LOG_INFO(OBS5(u8"正在构建TensorRT引擎...").c_str());
	nvinfer1::IHostMemory* serializedEngine = builder->buildSerializedNetwork(*network, *config);
	if (!serializedEngine) {
		LOG_ERROR(OBS3(u8"构建TensorRT引擎失败").c_str());
		delete config; delete parser; delete network; delete builder;
		if (progress) progress(0, OBS6(u8"构建引擎失败").c_str());
		return false;
	}
	if (cancelFlag && cancelFlag->load()) { delete serializedEngine; delete config; delete parser; delete network; delete builder; return false; }
	if (progress) progress(80, OBS6(u8"正在反序列化引擎...").c_str());
	if (!m_runtime) m_runtime = nvinfer1::createInferRuntime(m_logger);
	if (!m_runtime) { delete serializedEngine; delete config; delete parser; delete network; delete builder; if (progress) progress(0, OBS6(u8"创建Runtime失败").c_str()); return false; }
	m_engine = m_runtime->deserializeCudaEngine(serializedEngine->data(), serializedEngine->size());
	if (!m_engine) {
		delete serializedEngine; delete config; delete parser; delete network; delete builder;
		if (progress) progress(0, OBS6(u8"反序列化引擎失败").c_str());
		return false;
	}
	if (progress) progress(90, OBS6(u8"正在保存引擎缓存...").c_str());
	std::ofstream out(engineCache, std::ios::binary);
	if (out.is_open()) {
		out.write(reinterpret_cast<const char*>(serializedEngine->data()), serializedEngine->size());
		out.close();
	}
	delete serializedEngine; delete config; delete parser; delete network; delete builder;
	m_context = m_engine->createExecutionContext();
	if (!m_context) { if (progress) progress(0, OBS6(u8"创建执行上下文失败").c_str()); return false; }
	// 推理流用最高优先级: GPU满载(游戏+直播抢卡)时推理kernel优先调度, 延迟不暴涨
	int leastPri = 0, greatestPri = 0;
	cudaDeviceGetStreamPriorityRange(&leastPri, &greatestPri);
	cudaStreamCreateWithPriority(&m_stream, cudaStreamNonBlocking, greatestPri);
	cudaEventCreateWithFlags(&m_copyEvent, cudaEventDisableTiming);
	if (!allocateBuffers()) { if (progress) progress(0, OBS6(u8"分配显存失败").c_str()); return false; }
	LOG_INFO(OBS5(u8"TRT引擎构建完成，已启用TF32+CUDA Graph优化").c_str());
	if (progress) progress(100, OBS6(u8"模型加载完成").c_str());
	return true;
}
bool TRTEngine::loadEngine(const std::string& enginePath, int gpuId, ProgressCallback progress) {
	auto data = readEngineFile(enginePath);
	if (data.empty()) return false;
	if (progress) progress(30, OBS6(u8"正在反序列化引擎...").c_str());
	cudaSetDevice(gpuId);
	if (!m_runtime) m_runtime = nvinfer1::createInferRuntime(m_logger);
	if (!m_runtime) return false;
	m_engine = m_runtime->deserializeCudaEngine(data.data(), data.size());
	if (!m_engine) return false;
	m_context = m_engine->createExecutionContext();
	if (!m_context) return false;
	// 推理流用最高优先级: GPU满载(游戏+直播抢卡)时推理kernel优先调度, 延迟不暴涨
	int leastPri = 0, greatestPri = 0;
	cudaDeviceGetStreamPriorityRange(&leastPri, &greatestPri);
	cudaStreamCreateWithPriority(&m_stream, cudaStreamNonBlocking, greatestPri);
	cudaEventCreateWithFlags(&m_copyEvent, cudaEventDisableTiming);
	if (!allocateBuffers()) return false;
	if (progress) progress(100, OBS6(u8"引擎加载完成").c_str());
	return true;
}
bool TRTEngine::saveEngine(const std::string& enginePath) {
	if (!m_engine) return false;
	nvinfer1::IHostMemory* serialized = m_engine->serialize();
	if (!serialized) return false;
	std::ofstream out(enginePath, std::ios::binary);
	if (!out.is_open()) { delete serialized; return false; }
	out.write(reinterpret_cast<const char*>(serialized->data()), serialized->size());
	out.close();
	delete serialized;
	return true;
}
bool TRTEngine::allocateBuffers() {
	int nbTensors = m_engine->getNbIOTensors();
	for (int i = 0; i < nbTensors; ++i) {
		const char* name = m_engine->getIOTensorName(i);
		nvinfer1::Dims dims = m_engine->getTensorShape(name);
		int64_t volume = 1;
		std::vector<int64_t> dimVec;
		for (int j = 0; j < dims.nbDims; ++j) {
			if (dims.d[j] <= 0) {
				volume *= 1;
				dimVec.push_back(1);
			} else {
				volume *= dims.d[j];
				dimVec.push_back(dims.d[j]);
			}
		}
		if (m_engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
			m_inputName = name;
			m_inputSize = (int)volume;
			m_inputDims = dimVec;
		} else {
			m_outputName = name;
			m_outputSize = (int)volume;
			m_outputDims = dimVec;
		}
	}
	if (m_inputName.empty() || m_outputName.empty()) return false;
	cudaError_t err1 = cudaMalloc(&m_inputBuffer, m_inputSize * sizeof(float));
	cudaError_t err2 = cudaMalloc(&m_outputBuffer, m_outputSize * sizeof(float));
	if (err1 != cudaSuccess || err2 != cudaSuccess) {
		if (m_inputBuffer) { cudaFree(m_inputBuffer); m_inputBuffer = nullptr; }
		if (m_outputBuffer) { cudaFree(m_outputBuffer); m_outputBuffer = nullptr; }
		return false;
	}
	cudaError_t hostErr = cudaMallocHost(&m_outputHostBuf, m_outputSize * sizeof(float));
	if (hostErr == cudaSuccess) {
		m_outputHostBufPinned = true;
	} else {
		m_outputHostBufPinned = false;
		m_outputHostBuf = new float[m_outputSize]();
	}
	m_outputHostBufSize = m_outputSize;
	return true;
}
void TRTEngine::enableCudaGraph() {
	if (!m_engine || !m_context || !m_stream) return;
	m_cudaGraphEnabled = true;
	m_cudaGraphCaptured = false;
	m_warmupCount = 0;
	m_graphFailCount = 0;
	LOG_INFO(OBS5(u8"CUDA Graph优化已启用，等待预热...").c_str());
}
InferenceResult TRTEngine::inference(const float* inputData, int inputSize) {
	InferenceResult result;
	if (!m_context || !m_inputBuffer || !m_outputBuffer) return result;
	cudaMemcpyAsync(m_inputBuffer, inputData, inputSize * sizeof(float), cudaMemcpyHostToDevice, m_stream);
	m_context->setTensorAddress(m_inputName.c_str(), m_inputBuffer);
	m_context->setTensorAddress(m_outputName.c_str(), m_outputBuffer);
	m_context->enqueueV3(m_stream);
	if (m_outputHostBuf && m_outputHostBufSize >= m_outputSize) {
		cudaMemcpyAsync(m_outputHostBuf, m_outputBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
		cudaEventRecord(m_copyEvent, m_stream);
		cudaEventSynchronize(m_copyEvent);
		result.output.assign(m_outputHostBuf, m_outputHostBuf + m_outputSize);
	} else {
		result.output.resize(m_outputSize);
		cudaMemcpyAsync(result.output.data(), m_outputBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
		cudaEventRecord(m_copyEvent, m_stream);
		cudaEventSynchronize(m_copyEvent);
	}
	result.outputDims = m_outputDims;
	return result;
}
InferenceResult TRTEngine::inferenceFromGPU() {
	InferenceResult result;
	if (!m_context || !m_inputBuffer || !m_outputBuffer) return result;
	m_context->setTensorAddress(m_inputName.c_str(), m_inputBuffer);
	m_context->setTensorAddress(m_outputName.c_str(), m_outputBuffer);
	if (m_cudaGraphEnabled) {
		if (!m_cudaGraphCaptured) {
			m_warmupCount++;
			bool enqueueOk = m_context->enqueueV3(m_stream);
			if (!enqueueOk) {
				LOG_WARN(OBS1(u8"TRT enqueueV3失败，跳过本帧").c_str());
				cudaGetLastError();
				m_graphFailCount++;
				if (m_graphFailCount >= 5) {
					LOG_ERROR(OBS2(u8"TRT推理连续失败5次，禁用CUDA Graph优化").c_str());
					m_cudaGraphEnabled = false;
				}
				return result;
			}
			if (m_outputHostBuf && m_outputHostBufSize >= m_outputSize) {
				cudaMemcpyAsync(m_outputHostBuf, m_outputBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
			}
			cudaEventRecord(m_copyEvent, m_stream);
			cudaEventSynchronize(m_copyEvent);
			if (m_warmupCount >= WARMUP_FRAMES) {
				cudaError_t err = cudaStreamBeginCapture(m_stream, cudaStreamCaptureModeThreadLocal);
				if (err == cudaSuccess) {
					bool captureEnqueueOk = m_context->enqueueV3(m_stream);
					if (!captureEnqueueOk) {
						cudaStreamEndCapture(m_stream, &m_cudaGraph);
						if (m_cudaGraph) { cudaGraphDestroy(m_cudaGraph); m_cudaGraph = nullptr; }
						cudaGetLastError();
						LOG_WARN(OBS4(u8"CUDA Graph内enqueueV3失败，放弃本次捕获").c_str());
						m_warmupCount = 0;
						m_graphFailCount++;
						if (m_graphFailCount >= 5) {
							LOG_ERROR(OBS3(u8"CUDA Graph连续失败5次，禁用Graph优化").c_str());
							m_cudaGraphEnabled = false;
						}
					} else {
						if (m_outputHostBuf && m_outputHostBufSize >= m_outputSize) {
							cudaMemcpyAsync(m_outputHostBuf, m_outputBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
						}
						err = cudaStreamEndCapture(m_stream, &m_cudaGraph);
						if (err == cudaSuccess && m_cudaGraph) {
							err = cudaGraphInstantiate(&m_cudaGraphExec, m_cudaGraph, nullptr, nullptr, 0);
							if (err == cudaSuccess) {
								m_cudaGraphCaptured = true;
								m_graphFailCount = 0;
								LOG_INFO(OBS5(u8"CUDA Graph捕获成功(含D2H拷贝)，推理延迟将大幅降低").c_str());
							} else {
								LOG_WARN(OBS4(u8"CUDA Graph实例化失败(err=%d)，重试中...").c_str(), (int)err);
								m_warmupCount = 0;
								m_graphFailCount++;
								if (m_graphFailCount >= 5) {
									LOG_ERROR(OBS3(u8"CUDA Graph捕获连续失败5次，禁用Graph优化").c_str());
									m_cudaGraphEnabled = false;
								}
								if (m_cudaGraph) { cudaGraphDestroy(m_cudaGraph); m_cudaGraph = nullptr; }
							}
						} else {
							LOG_WARN(OBS4(u8"CUDA Graph捕获失败(err=%d)，重试中...").c_str(), (int)err);
							m_warmupCount = 0;
							m_graphFailCount++;
							if (m_graphFailCount >= 5) {
								LOG_ERROR(OBS3(u8"CUDA Graph连续失败5次，禁用Graph优化").c_str());
								m_cudaGraphEnabled = false;
							}
							if (m_cudaGraph) { cudaGraphDestroy(m_cudaGraph); m_cudaGraph = nullptr; }
						}
					}
				} else {
					LOG_WARN(OBS4(u8"CUDA Graph开始捕获失败(err=%d)，重试中...").c_str(), (int)err);
					m_warmupCount = 0;
					m_graphFailCount++;
					if (m_graphFailCount >= 5) {
						LOG_ERROR(OBS3(u8"CUDA Graph连续失败5次，禁用Graph优化").c_str());
						m_cudaGraphEnabled = false;
					}
				}
			}
		} else {
			cudaGraphLaunch(m_cudaGraphExec, m_stream);
		}
	} else {
		bool ok = m_context->enqueueV3(m_stream);
		if (!ok) {
			LOG_WARN(OBS5(u8"TRT enqueueV3失败，跳过本帧").c_str());
			cudaGetLastError();
			return result;
		}
	}
	if (m_outputHostBuf && m_outputHostBufSize >= m_outputSize) {
		if (!m_cudaGraphCaptured) {
			cudaMemcpyAsync(m_outputHostBuf, m_outputBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
		}
		cudaEventRecord(m_copyEvent, m_stream);
		cudaEventSynchronize(m_copyEvent);
		// [v184] 直读pinned缓冲, 省去每帧~1.2MB assign堆拷贝(后处理在下次推理前同步完成)
		result.directPtr = m_outputHostBuf;
	} else {
		result.output.resize(m_outputSize);
		cudaMemcpyAsync(result.output.data(), m_outputBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
		cudaEventRecord(m_copyEvent, m_stream);
		cudaEventSynchronize(m_copyEvent);
	}
	result.outputDims = m_outputDims;
	return result;
}
void TRTEngine::release() {
	if (m_cudaGraphExec) { cudaGraphExecDestroy(m_cudaGraphExec); m_cudaGraphExec = nullptr; }
	if (m_cudaGraph) { cudaGraphDestroy(m_cudaGraph); m_cudaGraph = nullptr; }
	m_cudaGraphCaptured = false;
	m_cudaGraphEnabled = false;
	m_warmupCount = 0;
	m_graphFailCount = 0;
	if (m_outputHostBuf) {
		if (m_outputHostBufPinned) cudaFreeHost(m_outputHostBuf);
		else delete[] m_outputHostBuf;
		m_outputHostBuf = nullptr;
		m_outputHostBufSize = 0;
		m_outputHostBufPinned = false;
	}
	if (m_inputBuffer) { cudaFree(m_inputBuffer); m_inputBuffer = nullptr; }
	if (m_outputBuffer) { cudaFree(m_outputBuffer); m_outputBuffer = nullptr; }
	if (m_copyEvent) { cudaEventDestroy(m_copyEvent); m_copyEvent = nullptr; }
	if (m_stream) { cudaStreamDestroy(m_stream); m_stream = nullptr; }
	delete m_context; m_context = nullptr;
	delete m_engine; m_engine = nullptr;
	delete m_runtime; m_runtime = nullptr;
	m_inputSize = 0;
	m_outputSize = 0;
	m_outputDims.clear();
}
std::vector<char> TRTEngine::readEngineFile(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open()) return {};
	file.seekg(0, std::ios::end);
	size_t size = file.tellg();
	if (size == 0 || size > 512 * 1024 * 1024) return {};
	file.seekg(0, std::ios::beg);
	std::vector<char> data(size);
	file.read(data.data(), size);
	return data;
}
std::string TRTEngine::getEngineCachePath(const std::string& onnxPath) {
	char exePath[MAX_PATH];
	GetModuleFileNameA(nullptr, exePath, MAX_PATH);
	std::string exeDir(exePath);
	auto pos = exeDir.find_last_of("\\/");
	if (pos != std::string::npos) exeDir = exeDir.substr(0, pos + 1);
	std::string modelName = onnxPath;
	auto slashPos = modelName.find_last_of("\\/");
	if (slashPos != std::string::npos) modelName = modelName.substr(slashPos + 1);
	static std::string sTrtExt = OBS1(".trt10");
		return exeDir + modelName + sTrtExt;
}