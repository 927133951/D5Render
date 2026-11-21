#include "dml_engine.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include <onnxruntime_cxx_api.h>
#include <cuda_runtime.h>
#include <windows.h>
DMLEngine::DMLEngine() {}
DMLEngine::~DMLEngine() { release(); }
static std::wstring utf8ToWstring(const std::string& str) {
	if (str.empty()) return std::wstring();
	int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
	if (len <= 0) {
		len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), (int)str.size(), nullptr, 0);
		if (len <= 0) return std::wstring(str.begin(), str.end());
	}
	std::wstring wstr(len, 0);
	MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], len);
	return wstr;
}
bool DMLEngine::loadModel(const std::string& onnxPath, int gpuId, ProgressCallback progress, std::atomic<bool>* cancelFlag) {
	release();
	// CUDA驱动版本检查
	int driverVer = 0, runtimeVer = 0;
	cudaDriverGetVersion(&driverVer);
	cudaRuntimeGetVersion(&runtimeVer);
	if (driverVer < runtimeVer) {
		int dMajor = driverVer / 1000, dMinor = (driverVer % 1000) / 10;
		int rMajor = runtimeVer / 1000, rMinor = (runtimeVer % 1000) / 10;
		LOG_ERROR(OBS1(u8"CUDA驱动版本过低! 需要: %d.%d (当前: %d.%d)").c_str(), rMajor, rMinor, dMajor, dMinor);
		LOG_ERROR(OBS2(u8"请更新NVIDIA显卡驱动到最新版本").c_str());
		if (progress) progress(0, OBS3(u8"CUDA驱动版本过低，请更新NVIDIA驱动").c_str());
		return false;
	}
	// 关键DLL预检测: 尝试加载ORT CUDA Provider依赖的DLL
	static std::string dllNames[] = {
			OBS1("onnxruntime_providers_cuda.dll"),
			OBS2("onnxruntime_providers_tensorrt.dll"),
			OBS3("onnxruntime_providers_shared.dll"),
			OBS4("cudart64_13.dll"),
			OBS5("cublas64_13.dll"),
			OBS6("cublasLt64_13.dll"),
			OBS7("cufft64_12.dll"),
			OBS8("cudnn64_9.dll")
		};
		for (auto& dllName : dllNames) {
			HMODULE hMod = LoadLibraryExA(dllName.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
		if (!hMod) {
			DWORD err = GetLastError();
			LOG_ERROR(OBS4(u8"缺少关键DLL: %s (错误码: %d)").c_str(), dllName.c_str(), err);
			if (err == 126) {
				LOG_ERROR(OBS5(u8"该DLL或其依赖项不存在，请确认程序目录完整").c_str());
			} else if (err == 127) {
				LOG_ERROR(OBS6(u8"该DLL版本不兼容，请确认所有DLL版本一致").c_str());
			}
		} else {
			FreeLibrary(hMod);
		}
	}
	cudaSetDevice(gpuId);
	// 推理流用最高优先级: GPU满载(游戏300帧+直播编码抢卡)时推理kernel优先调度
	// (旧leastPriority=最低优先级是笔误, 直播开启时推理被排队→延迟暴涨无法吸附)
	int leastPriority = 0, greatestPriority = 0;
	cudaDeviceGetStreamPriorityRange(&leastPriority, &greatestPriority);
	cudaStreamCreateWithPriority(&m_stream, cudaStreamNonBlocking, greatestPriority);
	cudaEventCreateWithFlags(&m_copyEvent, cudaEventDisableTiming);
	if (progress) progress(5, OBS5(u8"初始化ONNX Runtime环境...").c_str());
	try {
		static std::string envName = OBS1("D5Render");
		m_env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, envName.c_str());
		if (progress) progress(10, OBS5(u8"创建会话选项...").c_str());
		m_sessionOptions = new Ort::SessionOptions();
		auto* opts = static_cast<Ort::SessionOptions*>(m_sessionOptions);
		opts->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
		// 禁止CPU EP回退，所有节点必须在GPU上执行
		opts->AddConfigEntry(OBS1("session.disable_cpu_ep_fallback").c_str(), OBS2("1").c_str());
		// 纯GPU推理优化：禁用inter-op线程池，避免CPU线程竞争
		opts->SetInterOpNumThreads(0);
		// 限制intra-op线程数为物理核心数的一半，减少CPU资源占用
		int numCores = 0;
		SYSTEM_INFO sysInfo;
		GetSystemInfo(&sysInfo);
		numCores = sysInfo.dwNumberOfProcessors / 2;
		if (numCores < 1) numCores = 1;
		opts->SetIntraOpNumThreads(numCores);
		// 禁用线程自旋等待，避免与游戏争夺CPU时间片
		opts->AddConfigEntry(OBS3("session.intra_op.allow_spinning").c_str(), OBS4("0").c_str());
			opts->AddConfigEntry(OBS5("session.inter_op.allow_spinning").c_str(), OBS6("0").c_str());
		// 动态成本模型，降低延迟方差
		opts->AddConfigEntry(OBS7("session.dynamic_block_base").c_str(), OBS8("4").c_str());
		// 顺序执行模式，适合单推理流
		opts->SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
		if (cancelFlag && cancelFlag->load()) { release(); return false; }
		// 注册TensorRT EP (FP16 + 引擎缓存 + 512MB工作空间，减少与游戏显存竞争)
		m_gpuAvailable = false;
		if (progress) progress(15, OBS5(u8"初始化TensorRT执行提供程序...").c_str());
		try {
			OrtTensorRTProviderOptionsV2* trtOpts = nullptr;
			Ort::GetApi().CreateTensorRTProviderOptions(&trtOpts);
			std::string kDevId = OBS1("device_id"), kFp16 = OBS2("trt_fp16_enable"), kWorksp = OBS3("trt_max_workspace_size");
			std::string kCacheEn = OBS4("trt_engine_cache_enable"), kCachePath = OBS5("trt_engine_cache_path");
			std::string vOne = OBS6("1");
			std::vector<const char*> keys = { kDevId.c_str(), kFp16.c_str(), kWorksp.c_str(), kCacheEn.c_str(), kCachePath.c_str() };
			std::string deviceIdStr = std::to_string(gpuId);
			std::string workspaceStr = std::to_string(512ULL * 1024ULL * 1024ULL);
			std::string cachePath = OBS7("trt_ep_cache");
			std::vector<const char*> values = { deviceIdStr.c_str(), vOne.c_str(), workspaceStr.c_str(), vOne.c_str(), cachePath.c_str() };
			Ort::GetApi().UpdateTensorRTProviderOptions(trtOpts, keys.data(), values.data(), keys.size());
			opts->AppendExecutionProvider_TensorRT_V2(*trtOpts);
			Ort::GetApi().ReleaseTensorRTProviderOptions(trtOpts);
			m_gpuAvailable = true;
			LOG_INFO(OBS2(u8"TensorRT EP注册成功 (FP16, 引擎缓存已启用)").c_str());
		} catch (const Ort::Exception& e) {
			LOG_ERROR(OBS3(u8"TensorRT EP不可用: %s").c_str(), e.what());
			LOG_ERROR(OBS4(u8"可能原因: 1.未安装NVIDIA显卡驱动 2.驱动版本过旧(需支持CUDA 13.0) 3.目标电脑无NVIDIA GPU").c_str());
		}
		if (!m_gpuAvailable) {
			LOG_ERROR(OBS6(u8"TensorRT执行提供程序不可用，无法初始化").c_str());
			LOG_ERROR(OBS7(u8"请确认: 1.已安装最新NVIDIA驱动 2.显卡支持CUDA 3.不要复制nvcuda.dll到程序目录").c_str());
			if (progress) progress(0, OBS6(u8"TensorRT EP不可用").c_str());
			release();
			return false;
		}
		if (cancelFlag && cancelFlag->load()) { release(); return false; }
		if (progress) progress(20, OBS5(u8"加载ONNX模型...").c_str());
		std::wstring wPath = utf8ToWstring(onnxPath);
		if (wPath.empty()) { release(); return false; }
		m_session = new Ort::Session(*static_cast<Ort::Env*>(m_env), wPath.c_str(), *opts);
		if (progress) progress(60, OBS5(u8"分析模型输入输出...").c_str());
		auto* session = static_cast<Ort::Session*>(m_session);
		Ort::AllocatorWithDefaultOptions allocator;
		auto inputName = session->GetInputNameAllocated(0, allocator);
		m_inputName = inputName.get();
		auto outputName = session->GetOutputNameAllocated(0, allocator);
		m_outputName = outputName.get();
		auto inputTypeInfo = session->GetInputTypeInfo(0);
		auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
		m_inputShape = inputTensorInfo.GetShape();
		m_inputSize = 1;
		for (auto dim : m_inputShape) { if (dim <= 0) dim = 1; m_inputSize *= (int)dim; }
		auto outputTypeInfo = session->GetOutputTypeInfo(0);
		auto outputTensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
		m_outputDims = outputTensorInfo.GetShape();
		m_outputSize = 1;
		for (auto dim : m_outputDims) { if (dim <= 0) dim = 1; m_outputSize *= (int)dim; }
		if (progress) progress(80, OBS5(u8"分配GPU缓冲区...").c_str());
		cudaError_t err = cudaMalloc(&m_inputGPUBuffer, m_inputSize * sizeof(float));
		if (err != cudaSuccess) {
			m_inputGPUBuffer = nullptr;
			LOG_ERROR(OBS2(u8"cudaMalloc输入缓冲失败: %s").c_str(), cudaGetErrorString(err));
			release();
			return false;
		}
		err = cudaMalloc(&m_outputGPUBuffer, m_outputSize * sizeof(float));
		if (err != cudaSuccess) {
			m_outputGPUBuffer = nullptr;
			LOG_ERROR(OBS2(u8"cudaMalloc输出缓冲失败: %s").c_str(), cudaGetErrorString(err));
			release();
			return false;
		}
		cudaError_t hostErr = cudaMallocHost(&m_inputBufPinned, m_inputSize * sizeof(float));
		if (hostErr == cudaSuccess) {
			memset(m_inputBufPinned, 0, m_inputSize * sizeof(float));
		} else {
			m_inputBufPinned = nullptr;
			m_inputBuf.resize(m_inputSize, 0.0f);
		}
		m_outputHostBufSize = m_outputSize;
		cudaError_t outHostErr = cudaMallocHost(&m_outputHostBuf, m_outputSize * sizeof(float));
		if (outHostErr == cudaSuccess) {
			m_outputHostBufPinned = true;
		} else {
			m_outputHostBufPinned = false;
			m_outputHostBuf = new float[m_outputHostBufSize]();
		}
		// 创建IoBinding
		m_binding = new Ort::IoBinding(*session);
		std::string cudaMemName = OBS1("Cuda");
		m_memInfo = new Ort::MemoryInfo(cudaMemName.c_str(), OrtArenaAllocator, gpuId, OrtMemTypeDefault);
		if (progress) progress(100, OBS5(u8"TRT EP模型加载完成").c_str());
		LOG_INFO(OBS4(u8"DML引擎加载成功 (TRT EP + IoBinding, GPU加速)").c_str());
		return true;
	} catch (const Ort::Exception& e) {
		LOG_ERROR(OBS2(u8"DML模型加载失败: %s").c_str(), e.what());
		release();
		return false;
	} catch (const std::exception& e) {
		LOG_ERROR(OBS2(u8"DML模型加载异常: %s").c_str(), e.what());
		release();
		return false;
	} catch (...) {
		LOG_ERROR(OBS2(u8"DML模型加载未知异常").c_str());
		release();
		return false;
	}
}
InferenceResult DMLEngine::inference(float* inputData, int inputSize) {
	InferenceResult result;
	if (!m_session || !m_binding || !m_inputGPUBuffer) return result;
	try {
		auto* session = static_cast<Ort::Session*>(m_session);
		auto* binding = static_cast<Ort::IoBinding*>(m_binding);
		auto* memInfo = static_cast<Ort::MemoryInfo*>(m_memInfo);
		cudaMemcpyAsync(m_inputGPUBuffer, inputData, inputSize * sizeof(float), cudaMemcpyHostToDevice, m_stream);
		std::vector<int64_t> inputShape = m_inputShape;
		for (auto& dim : inputShape) { if (dim <= 0) dim = 1; }
		Ort::Value inputTensor = Ort::Value::CreateTensor<float>(*memInfo, static_cast<float*>(m_inputGPUBuffer), m_inputSize, inputShape.data(), inputShape.size());
		binding->BindInput(m_inputName.c_str(), inputTensor);
		std::vector<int64_t> outputShape = m_outputDims;
		for (auto& dim : outputShape) { if (dim <= 0) dim = 1; }
		Ort::Value outputTensor = Ort::Value::CreateTensor<float>(*memInfo, static_cast<float*>(m_outputGPUBuffer), m_outputSize, outputShape.data(), outputShape.size());
		binding->BindOutput(m_outputName.c_str(), outputTensor);
		session->Run(Ort::RunOptions{}, *binding);
		cudaMemcpyAsync(m_outputHostBuf, m_outputGPUBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
		cudaEventRecord(m_copyEvent, m_stream);
		cudaEventSynchronize(m_copyEvent);
		if (m_outputHostBuf && m_outputHostBufSize >= m_outputSize) {
			// [v184] 直读pinned缓冲, 省去每帧~1.2MB assign堆拷贝(后处理在下次推理前同步完成)
			result.directPtr = m_outputHostBuf;
		}
		result.outputDims = m_outputDims;
	} catch (const Ort::Exception& e) {
		LOG_ERROR(OBS2(u8"TRT EP推理失败: %s").c_str(), e.what());
	} catch (...) {
		LOG_ERROR(OBS2(u8"TRT EP推理未知异常").c_str());
	}
	return result;
}
InferenceResult DMLEngine::inferenceFromGPU() {
	InferenceResult result;
	if (!m_session || !m_binding || !m_inputGPUBuffer) return result;
	try {
		auto* session = static_cast<Ort::Session*>(m_session);
		auto* binding = static_cast<Ort::IoBinding*>(m_binding);
		auto* memInfo = static_cast<Ort::MemoryInfo*>(m_memInfo);
		std::vector<int64_t> inputShape = m_inputShape;
		for (auto& dim : inputShape) { if (dim <= 0) dim = 1; }
		Ort::Value inputTensor = Ort::Value::CreateTensor<float>(*memInfo, static_cast<float*>(m_inputGPUBuffer), m_inputSize, inputShape.data(), inputShape.size());
		binding->BindInput(m_inputName.c_str(), inputTensor);
		std::vector<int64_t> outputShape = m_outputDims;
		for (auto& dim : outputShape) { if (dim <= 0) dim = 1; }
		Ort::Value outputTensor = Ort::Value::CreateTensor<float>(*memInfo, static_cast<float*>(m_outputGPUBuffer), m_outputSize, outputShape.data(), outputShape.size());
		binding->BindOutput(m_outputName.c_str(), outputTensor);
		session->Run(Ort::RunOptions{}, *binding);
		cudaMemcpyAsync(m_outputHostBuf, m_outputGPUBuffer, m_outputSize * sizeof(float), cudaMemcpyDeviceToHost, m_stream);
		cudaEventRecord(m_copyEvent, m_stream);
		cudaEventSynchronize(m_copyEvent);
		if (m_outputHostBuf && m_outputHostBufSize >= m_outputSize) {
			// [v184] 直读pinned缓冲, 省去每帧~1.2MB assign堆拷贝(后处理在下次推理前同步完成)
			result.directPtr = m_outputHostBuf;
		}
		result.outputDims = m_outputDims;
	} catch (const Ort::Exception& e) {
		LOG_ERROR(OBS2(u8"TRT EP推理失败: %s").c_str(), e.what());
	} catch (...) {
		LOG_ERROR(OBS2(u8"TRT EP推理未知异常").c_str());
	}
	return result;
}
void DMLEngine::release() {
	if (m_outputHostBuf) {
		if (m_outputHostBufPinned) cudaFreeHost(m_outputHostBuf);
		else delete[] m_outputHostBuf;
		m_outputHostBuf = nullptr;
		m_outputHostBufSize = 0;
		m_outputHostBufPinned = false;
	}
	if (m_inputBufPinned) { cudaFreeHost(m_inputBufPinned); m_inputBufPinned = nullptr; }
	m_inputBuf.clear();
	if (m_inputGPUBuffer) { cudaFree(m_inputGPUBuffer); m_inputGPUBuffer = nullptr; }
	if (m_outputGPUBuffer) { cudaFree(m_outputGPUBuffer); m_outputGPUBuffer = nullptr; }
	if (m_copyEvent) { cudaEventDestroy(m_copyEvent); m_copyEvent = nullptr; }
	if (m_stream) { cudaStreamDestroy(m_stream); m_stream = nullptr; }
	if (m_memInfo) { delete static_cast<Ort::MemoryInfo*>(m_memInfo); m_memInfo = nullptr; }
	if (m_binding) { delete static_cast<Ort::IoBinding*>(m_binding); m_binding = nullptr; }
	if (m_session) { delete static_cast<Ort::Session*>(m_session); m_session = nullptr; }
	if (m_sessionOptions) { delete static_cast<Ort::SessionOptions*>(m_sessionOptions); m_sessionOptions = nullptr; }
	if (m_env) { delete static_cast<Ort::Env*>(m_env); m_env = nullptr; }
	m_inputSize = 0;
	m_outputSize = 0;
	m_outputDims.clear();
	m_inputName.clear();
	m_inputShape.clear();
	m_gpuAvailable = false;
}
