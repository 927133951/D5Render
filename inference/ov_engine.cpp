#include "ov_engine.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include <openvino/openvino.hpp>
#include <cuda_runtime.h>
#include <cstring>
OVEngine::OVEngine() {}
OVEngine::~OVEngine() { release(); }
std::vector<std::string> OVEngine::getAvailableDevices() {
	try {
		ov::Core core;
		auto devices = core.get_available_devices();
		std::vector<std::string> result;
		for (const auto& dev : devices) result.push_back(dev);
		return result;
	} catch (...) {
		return {};
	}
}
bool OVEngine::loadModel(const std::string& onnxPath, OVDevice device, ProgressCallback progress, std::atomic<bool>* cancelFlag) {
	release();
	if (progress) progress(5, OBS5(u8"初始化OpenVINO运行时...").c_str());
	try {
		auto* core = new ov::Core();
		m_core = core;
		if (cancelFlag && cancelFlag->load()) { release(); return false; }
		if (progress) progress(10, OBS5(u8"读取ONNX模型...").c_str());
		auto model = core->read_model(onnxPath);
		if (!model) {
			LOG_ERROR(OBS3(u8"OpenVINO读取模型失败").c_str());
			release();
			return false;
		}
		if (cancelFlag && cancelFlag->load()) { release(); return false; }
		// 确定设备：OpenVINO GPU/NPU仅支持Intel，NVIDIA系统自动回退CPU
		std::string deviceName = "CPU";
		if (device == OVDevice::GPU || device == OVDevice::NPU) {
			auto availableDevs = core->get_available_devices();
			bool intelGpuFound = false;
			for (const auto& dev : availableDevs) {
				if (dev.find("GPU") != std::string::npos || dev.find("NPU") != std::string::npos) {
					intelGpuFound = true;
					break;
				}
			}
			if (intelGpuFound && device == OVDevice::GPU) deviceName = "GPU";
			else if (intelGpuFound && device == OVDevice::NPU) deviceName = "NPU";
			else LOG_WARN(OBS3(u8"OpenVINO: Intel GPU/NPU未检测到，使用CPU推理(NVIDIA显卡请选TensorRT)").c_str());
		} else if (device == OVDevice::Auto) {
			auto availableDevs = core->get_available_devices();
			for (const auto& dev : availableDevs) {
				if (dev.find("GPU") != std::string::npos) { deviceName = "GPU"; break; }
				if (dev.find("NPU") != std::string::npos) { deviceName = "NPU"; break; }
			}
		}
		if (progress) progress(25, OBS5(u8"配置预处理管线...").c_str());
		ov::preprocess::PrePostProcessor ppp(model);
		ppp.input().tensor()
			.set_element_type(ov::element::f32)
			.set_layout("NCHW");
		ppp.input().preprocess()
			.convert_element_type(ov::element::f32);
		ppp.input().model().set_layout("NCHW");
		model = ppp.build();
		if (cancelFlag && cancelFlag->load()) { release(); return false; }
		if (progress) progress(35, OBS5(u8"编译模型...").c_str());
		ov::AnyMap compileConfig = {
			ov::hint::performance_mode(ov::hint::PerformanceMode::LATENCY),
			ov::num_streams(1),
			ov::cache_dir("ov_cache"),
		};
		if (deviceName == "CPU") {
			compileConfig.insert(ov::hint::inference_precision(ov::element::f16));
			compileConfig.insert(ov::inference_num_threads(8));
			compileConfig.insert(ov::hint::enable_cpu_pinning(true));
			compileConfig.insert(ov::hint::scheduling_core_type(ov::hint::SchedulingCoreType::PCORE_ONLY));
		}
		auto compiled = core->compile_model(model, deviceName, compileConfig);
		m_compiledModel = new ov::CompiledModel(std::move(compiled));
		if (cancelFlag && cancelFlag->load()) { release(); return false; }
		if (progress) progress(70, OBS5(u8"创建推理请求...").c_str());
		auto* compiledPtr = static_cast<ov::CompiledModel*>(m_compiledModel);
		auto inferReq = compiledPtr->create_infer_request();
		m_inferRequest = new ov::InferRequest(std::move(inferReq));
		auto* req = static_cast<ov::InferRequest*>(m_inferRequest);
		auto inputTensor = req->get_input_tensor();
		auto inputShape = inputTensor.get_shape();
		m_inputShape.assign(inputShape.begin(), inputShape.end());
		m_inputSize = 1;
		for (auto dim : m_inputShape) { if (dim <= 0) dim = 1; m_inputSize *= (int)dim; }
		auto outputTensor = req->get_output_tensor();
		auto outputShape = outputTensor.get_shape();
		m_outputDims.assign(outputShape.begin(), outputShape.end());
		m_outputSize = 1;
		for (auto dim : m_outputDims) { if (dim <= 0) dim = 1; m_outputSize *= (int)dim; }
		m_inputName = compiledPtr->input().get_any_name();
		m_outputName = compiledPtr->output().get_any_name();
		// 缓存tensor数据指针，避免每帧重复获取
		m_tensorDataPtr = inputTensor.data<float>();
		// 分配GPU输入缓冲（CUDA预处理输出目标）
		cudaError_t gpuErr = cudaMalloc(&m_inputGPUBuffer, m_inputSize * sizeof(float));
		if (gpuErr != cudaSuccess) {
			m_inputGPUBuffer = nullptr;
			// 无GPU缓冲时使用pinned memory作为D2H中转
			cudaError_t hostErr = cudaMallocHost(&m_inputBufPinned, m_inputSize * sizeof(float));
			if (hostErr == cudaSuccess) {
				memset(m_inputBufPinned, 0, m_inputSize * sizeof(float));
			} else {
				m_inputBufPinned = nullptr;
				m_inputBuf.resize(m_inputSize, 0.0f);
			}
		}
		// 预热推理：消除首次推理的JIT编译开销
		if (m_tensorDataPtr) {
			memset(m_tensorDataPtr, 0, m_inputSize * sizeof(float));
			req->infer();
			req->infer();
			m_warmupDone = true;
		}
		if (progress) progress(100, OBS5(u8"OpenVINO模型加载完成").c_str());
		LOG_INFO(OBS4(u8"OpenVINO模型加载成功 (设备:%s, 预热:%s)").c_str(), deviceName.c_str(), m_warmupDone ? "是" : "否");
		return true;
	} catch (const ov::Exception& e) {
		LOG_ERROR(OBS2(u8"OpenVINO加载失败: %s").c_str(), e.what());
		release();
		return false;
	} catch (const std::exception& e) {
		LOG_ERROR(OBS2(u8"OpenVINO加载异常: %s").c_str(), e.what());
		release();
		return false;
	} catch (...) {
		LOG_ERROR(OBS2(u8"OpenVINO加载未知异常").c_str());
		release();
		return false;
	}
}
InferenceResult OVEngine::inference(float* inputData, int inputSize) {
	InferenceResult result;
	if (!m_inferRequest) return result;
	try {
		auto* req = static_cast<ov::InferRequest*>(m_inferRequest);
		if (m_tensorDataPtr && inputData && inputSize <= m_inputSize) {
			memcpy(m_tensorDataPtr, inputData, inputSize * sizeof(float));
		}
		req->infer();
		auto outputTensor = req->get_output_tensor();
		const float* outData = outputTensor.data<const float>();
		size_t outCount = outputTensor.get_size();
		result.output.assign(outData, outData + outCount);
		result.outputDims = m_outputDims;
	} catch (const ov::Exception& e) {
		LOG_ERROR(OBS2(u8"OpenVINO推理失败: %s").c_str(), e.what());
	} catch (...) {
		LOG_ERROR(OBS2(u8"OpenVINO推理未知异常").c_str());
	}
	return result;
}
InferenceResult OVEngine::inferenceDirect() {
	InferenceResult result;
	if (!m_inferRequest || !m_tensorDataPtr) return result;
	try {
		auto* req = static_cast<ov::InferRequest*>(m_inferRequest);
		req->infer();
		auto outputTensor = req->get_output_tensor();
		const float* outData = outputTensor.data<const float>();
		size_t outCount = outputTensor.get_size();
		result.output.assign(outData, outData + outCount);
		result.outputDims = m_outputDims;
	} catch (const ov::Exception& e) {
		LOG_ERROR(OBS2(u8"OpenVINO推理失败: %s").c_str(), e.what());
	} catch (...) {
		LOG_ERROR(OBS2(u8"OpenVINO推理未知异常").c_str());
	}
	return result;
}
void OVEngine::release() {
	m_tensorDataPtr = nullptr;
	m_warmupDone = false;
	if (m_inferRequest) { delete static_cast<ov::InferRequest*>(m_inferRequest); m_inferRequest = nullptr; }
	if (m_compiledModel) { delete static_cast<ov::CompiledModel*>(m_compiledModel); m_compiledModel = nullptr; }
	if (m_core) { delete static_cast<ov::Core*>(m_core); m_core = nullptr; }
	if (m_inputBufPinned) { cudaFreeHost(m_inputBufPinned); m_inputBufPinned = nullptr; }
	if (m_inputGPUBuffer) { cudaFree(m_inputGPUBuffer); m_inputGPUBuffer = nullptr; }
	m_inputBuf.clear();
	m_inputSize = 0;
	m_outputSize = 0;
	m_inputShape.clear();
	m_outputDims.clear();
	m_inputName.clear();
	m_outputName.clear();
}
