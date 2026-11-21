#include "screen_capture.h"
#include <cuda_runtime.h>
#include <cuda.h>
#include <cuda_d3d11_interop.h>
#include <cstring>
#include <random>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>
#include <eventtoken.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include "utils/dll_guard.h"
#include "utils/obfstr.h"
#include "gui/app_log.h"
// ========== WGC GUIDs ==========
static const GUID IID_IGraphicsCaptureItemInterop = {0x3628E81B, 0x3CAC, 0x4C60, {0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56}};
static const GUID IID_IGraphicsCaptureItem = {0x79C3F95B, 0x31F7, 0x4EC2, {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60}};
static const GUID IID_IDirect3D11CaptureFramePool = {0x24EB6D22, 0x1975, 0x422E, {0x82, 0xE7, 0x78, 0x0D, 0xBD, 0x8D, 0xDF, 0x24}};
static const GUID IID_IDirect3D11CaptureFramePoolStatics = {0x7784056A, 0x67AA, 0x4D53, {0xAE, 0x54, 0x10, 0x88, 0xD5, 0xA8, 0xCA, 0x21}};
static const GUID IID_IDirect3D11CaptureFramePoolStatics2 = {0x589B103F, 0x6BBC, 0x5DF5, {0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5}};
static const GUID IID_IGraphicsCaptureSession = {0x814E42A9, 0xF70F, 0x4AD7, {0x93, 0x9B, 0xFD, 0xDC, 0xC6, 0xEB, 0x88, 0x0D}};
static const GUID IID_IGraphicsCaptureSession2 = {0x2C39AE40, 0x7D2E, 0x5044, {0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E}};
static const GUID IID_IGraphicsCaptureSession3 = {0xF2CDD966, 0x22AE, 0x5EA1, {0x95, 0x96, 0x3A, 0x28, 0x93, 0x44, 0xC3, 0xBE}};
static const GUID IID_IDirect3DDxgiInterfaceAccess = {0xA9B3D012, 0x3DF2, 0x4EE3, {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};
struct WGCSizeInt32 { INT32 Width; INT32 Height; };
// ========== ScreenCapture 实现 ==========
ScreenCapture::ScreenCapture() {}
ScreenCapture::~ScreenCapture() { release(); }
bool ScreenCapture::initEarlyDevice(int cudaGpuId) {
	// [v185] 早期设备绑定: cudaD3D11SetDirect3DDevice要求在CUDA设备激活前调用
	// (TRT引擎加载会激活primary context, 之后必报708), 因此在App启动、模型加载前执行。
	// D3D设备创建与模型尺寸无关, 可安全提前
	m_cudaGpuId = cudaGpuId;
	if (m_device && m_boundGpuId == cudaGpuId && m_earlyBound) return true;
	if (m_device) { if (m_context) { m_context->Release(); m_context = nullptr; } m_device->Release(); m_device = nullptr; m_earlyBound = false; m_boundGpuId = -1; }
	if (!createDevice()) {
		LOG_WARN(OBS1(u8"捕获D3D设备早期创建失败, 捕获初始化时将重试").c_str());
		return false;
	}
	cudaError_t err = cudaD3D11SetDirect3DDevice(m_device, cudaGpuId);
	if (err != cudaSuccess) {
		LOG_WARN(OBS2(u8"CUDA互操作设备绑定失败: %s(%d) —— 互操作将不可用, 使用CPU回退路径").c_str(), cudaGetErrorString(err), (int)err);
		return false;
	}
	m_boundGpuId = cudaGpuId;
	m_earlyBound = true;
	LOG_INFO(OBS3(u8"CUDA-D3D11设备早期绑定成功(互操作路径可用)").c_str());
	return true;
}
bool ScreenCapture::initialize(int captureWidth, int captureHeight, int cudaGpuId, CaptureMode mode) {
	if (m_initialized) release();
	m_captureWidth = captureWidth;
	m_captureHeight = captureHeight;
	m_cudaGpuId = cudaGpuId;
	m_captureMode = mode;
	m_screenWidth = GetSystemMetrics(SM_CXSCREEN);
	m_screenHeight = GetSystemMetrics(SM_CYSCREEN);
	LOG_INFO(OBS1(u8"捕获初始化: 模式=%d 分辨率=%dx%d").c_str(), (int)mode, captureWidth, captureHeight);
	if (!createDevice()) {
		LOG_ERROR(OBS2(u8"D3D11设备创建失败").c_str());
		return false;
	}
	if (m_captureMode == CaptureMode::WGC) {
		if (!createStagingTexture()) { LOG_ERROR(OBS1(u8"Staging纹理创建失败").c_str()); release(); return false; }
		if (!createSharedTextures()) { LOG_ERROR(OBS2(u8"共享纹理创建失败").c_str()); release(); return false; }
		if (!initWGC()) {
			LOG_ERROR(OBS3(u8"WGC初始化失败，尝试DXGI回退").c_str());
			releaseWGC();
			m_captureMode = CaptureMode::DXGI;
			if (!createDuplication()) { release(); return false; }
			if (!createStagingTexture()) { release(); return false; }
		}
		m_initialized = true;
		LOG_INFO(OBS4(u8"捕获初始化成功: %s").c_str(), m_captureMode == CaptureMode::WGC ? "WGC" : "DXGI");
		return true;
	}
	if (m_captureMode == CaptureMode::Hook) {
		if (!createStagingTexture()) { release(); return false; }
		if (!createSharedTextures()) { release(); return false; }
		if (!hookCreateDup()) {
			LOG_WARN(OBS5(u8"Hook模式Duplication创建失败，回退到DXGI模式").c_str());
			m_captureMode = CaptureMode::DXGI;
		}
		m_initialized = true;
		LOG_INFO(OBS6(u8"捕获初始化成功: %s").c_str(), m_captureMode == CaptureMode::Hook ? "按需DXGI(Hook)" : "DXGI(回退)");
		return true;
	}
	// DXGI模式
	if (!createDuplication()) { release(); return false; }
	if (!createSharedTextures()) { release(); return false; }
	if (!createStagingTexture()) { release(); return false; }
	m_initialized = true;
	LOG_INFO(OBS7(u8"捕获初始化成功: DXGI").c_str());
	return true;
}
bool ScreenCapture::createDevice() {
	D3D_FEATURE_LEVEL featureLevel;
	// [v185] 早期绑定复用: 设备已在启动时创建并cudaD3D11SetDirect3DDevice绑定, 直接复用。
	// 重复创建会丢失绑定(重新绑定需CUDA未激活, 此时必失败)
	if (m_device && m_boundGpuId == m_cudaGpuId) return true;
	if (m_device) { if (m_context) { m_context->Release(); m_context = nullptr; } m_device->Release(); m_device = nullptr; m_earlyBound = false; m_boundGpuId = -1; }
	// [v185] CUDA互操作硬性要求(官方文档): "A CUDA context may interoperate only with
	// Direct3D11 devices created with DriverType set to D3D_DRIVER_TYPE_HARDWARE"。
	// 旧代码用 UNKNOWN+显式适配器创建 → cudaGraphicsD3D11RegisterResource 必返回
	// invalid argument → 互操作永远不可用 → 所有模式掉进CPU回退路径(10-20ms)。
	// 策略: 先用HARDWARE默认适配器(=主显示适配器)创建并校验LUID与CUDA设备一致;
	// 一致即用(互操作可用); 不一致才退回显式适配器+UNKNOWN(捕获可用, 互操作不可用)
	char luidTag[64] = "LUID不可用";
	unsigned int cudaLow = 0, cudaHigh = 0;
	bool luidValid = false;
	{
		// [v185] 必须用Driver API查询LUID: cudaGetDeviceProperties会激活primary context,
		// 导致随后的cudaD3D11SetDirect3DDevice报708(device active in this process)
		CUdevice cuDev = 0;
		char luidBytes[8] = {};
		unsigned int nodeMask = 0;
		if (cuDeviceGet(&cuDev, m_cudaGpuId) == CUDA_SUCCESS &&
			cuDeviceGetLuid(luidBytes, &nodeMask, cuDev) == CUDA_SUCCESS) {
			cudaLow = *(unsigned int*)(luidBytes);
			cudaHigh = *(unsigned int*)(luidBytes + 4);
			luidValid = (cudaLow != 0 || cudaHigh != 0);
			if (luidValid) sprintf_s(luidTag, "LUID=%08X-%08X", cudaHigh, cudaLow);
		}
	}
	auto adapterLuidMatches = [&](ID3D11Device* dev, IDXGIAdapter** outAdapter) -> bool {
		*outAdapter = nullptr;
		IDXGIDevice* dxgiDev = nullptr;
		if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) return false;
		IDXGIAdapter* ad = nullptr;
		if (FAILED(dxgiDev->GetAdapter(&ad))) { dxgiDev->Release(); return false; }
		dxgiDev->Release();
		DXGI_ADAPTER_DESC desc;
		if (FAILED(ad->GetDesc(&desc))) { ad->Release(); return false; }
		bool match = luidValid && desc.AdapterLuid.LowPart == cudaLow && (unsigned int)desc.AdapterLuid.HighPart == cudaHigh;
		if (match) *outAdapter = ad;
		else ad->Release();
		return match;
	};
	// 路径1: HARDWARE默认适配器(互操作可用)
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
	if (SUCCEEDED(hr)) {
		IDXGIAdapter* matched = nullptr;
		if (adapterLuidMatches(m_device, &matched) && matched) {
			matched->Release();
			DXGI_ADAPTER_DESC desc;
			IDXGIDevice* dxgiDev = nullptr;
			if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
				IDXGIAdapter* ad = nullptr;
				if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
					DXGI_ADAPTER_DESC d2;
					ad->GetDesc(&d2);
					LOG_INFO(OBS1(u8"D3D设备: CUDA LUID精确绑定 %ls (%s) [HARDWARE,互操作可用]").c_str(), d2.Description, luidTag);
					ad->Release();
				}
				dxgiDev->Release();
			}
			return true;
		}
		// LUID不匹配: 释放, 走路径2
		if (m_context) { m_context->Release(); m_context = nullptr; }
		m_device->Release(); m_device = nullptr;
	}
	// 路径2: LUID精确匹配的显式适配器+UNKNOWN(捕获可用, 互操作不可用)
	IDXGIAdapter* nvidiaAdapter = nullptr;
	IDXGIFactory1* factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
		IDXGIAdapter1* adapter = nullptr;
		for (int i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc;
			adapter->GetDesc1(&desc);
			if (desc.VendorId == 0x10DE && luidValid && desc.AdapterLuid.LowPart == cudaLow && (unsigned int)desc.AdapterLuid.HighPart == cudaHigh) {
				nvidiaAdapter = adapter;
				LOG_INFO(OBS2(u8"D3D设备: LUID绑定适配器[%d] %ls (%s) [UNKNOWN模式,CUDA互操作将不可用]").c_str(), i, desc.Description, luidTag);
				break;
			}
			adapter->Release();
		}
		if (!nvidiaAdapter) {
			// 兼容回退: 按N卡序号匹配
			IDXGIAdapter1* adapter2 = nullptr;
			int nvidiaIdx = 0;
			for (int i = 0; factory->EnumAdapters1(i, &adapter2) != DXGI_ERROR_NOT_FOUND; ++i) {
				DXGI_ADAPTER_DESC1 desc;
				adapter2->GetDesc1(&desc);
				if (desc.VendorId == 0x10DE) {
					if (nvidiaIdx == m_cudaGpuId) { nvidiaAdapter = adapter2; break; }
					nvidiaIdx++;
				}
				adapter2->Release();
			}
		}
		factory->Release();
	}
	hr = D3D11CreateDevice(nvidiaAdapter, nvidiaAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
	if (nvidiaAdapter) nvidiaAdapter->Release();
	if (FAILED(hr)) {
		LOG_WARN(OBS3(u8"D3D设备: NVIDIA适配器创建失败(0x%X)，回退默认设备").c_str(), hr);
		hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &m_device, &featureLevel, &m_context);
	}
	return SUCCEEDED(hr);
}
bool ScreenCapture::createDuplication() {
	IDXGIDevice* dxgiDevice = nullptr;
	if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) return false;
	IDXGIAdapter* adapter = nullptr;
	if (FAILED(dxgiDevice->GetAdapter(&adapter))) { dxgiDevice->Release(); return false; }
	// [v184] 主显示器匹配: 旧代码固定EnumOutputs(0)——多显示器或核显带主屏(混合GPU笔记本)时,
	// NVIDIA适配器可能根本没有输出(枚举失败=静默"DXGI捕获失败"), 或输出0是副屏(捕获到错误画面=检测不到目标)。
	// 枚举全部输出, 选择与主显示器矩形有交集的那个
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	HMONITOR primaryMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
	GetMonitorInfoW(primaryMon, &mi);
	IDXGIOutput* output = nullptr;
	IDXGIOutput* chosen = nullptr;
	int chosenIdx = -1;
	int totalOutputs = 0;
	for (int i = 0; SUCCEEDED(adapter->EnumOutputs(i, &output)); ++i) {
		totalOutputs++;
		DXGI_OUTPUT_DESC od;
		if (SUCCEEDED(output->GetDesc(&od))) {
			bool intersect = !(od.DesktopCoordinates.right <= mi.rcMonitor.left ||
				od.DesktopCoordinates.left >= mi.rcMonitor.right ||
				od.DesktopCoordinates.bottom <= mi.rcMonitor.top ||
				od.DesktopCoordinates.top >= mi.rcMonitor.bottom);
			if (intersect && !chosen) { chosen = output; chosenIdx = i; continue; }
		}
		output->Release();
	}
	if (!chosen) {
		// [v184] NVIDIA适配器上没有任何输出与主屏相交: DXGI复制接口无法从此设备捕获主屏
		char monName[128];
		WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, monName, 128, nullptr, nullptr);
		LOG_ERROR(OBS1(u8"DXGI捕获失败根因: 主显示器(%s)不在NVIDIA适配器上(该适配器共%d个输出)——"
			u8"游戏画面由其他GPU(核显/另一块显卡)输出, DXGI桌面复制无法跨适配器工作。"
			u8"将自动回退WGC图形捕获").c_str(), monName, totalOutputs);
		adapter->Release(); dxgiDevice->Release();
		return false;
	}
	IDXGIOutput1* output1 = nullptr;
	if (FAILED(chosen->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) {
		LOG_ERROR(OBS2(u8"DXGI捕获失败: 输出#%d不支持IDXGIOutput1(驱动过旧?)").c_str(), chosenIdx);
		chosen->Release(); adapter->Release(); dxgiDevice->Release(); return false;
	}
	HRESULT hr = E_FAIL;
	// [v185] 失败自动重试2次: 排他接口有释放窗口——占用者(远程工具/回放)正在退出、
	// 或上一个duplication正在销毁时, 立即创建会E_ACCESSDENIED, 间隔重试常可成功
	for (int attempt = 0; attempt < 3; ++attempt) {
		hr = output1->DuplicateOutput(m_device, &m_duplication);
		if (SUCCEEDED(hr)) break;
		if (attempt < 2) Sleep(300);
	}
	if (FAILED(hr)) {
		// 桌面复制是排他接口: 远程控制软件(ToDesk/向日葵)或直播软件占用时创建失败
		LOG_WARN(OBS3(u8"DXGI桌面复制创建失败(0x%08X) 输出#%d —— 常见原因: ToDesk/向日葵等远程工具或NVIDIA App即时回放正占用桌面复制接口").c_str(), (unsigned)hr, chosenIdx);
	}
	DXGI_OUTPUT_DESC outputDesc;
	output1->GetDesc(&outputDesc);
	// [v181] 打印绑定的输出: 多显示器绑错屏会直接导致捕获"不生效"(黑屏/检测不到)
	{
		char devName[128];
		WideCharToMultiByte(CP_UTF8, 0, outputDesc.DeviceName, -1, devName, 128, nullptr, nullptr);
		LOG_INFO(OBS4(u8"DXGI绑定输出#%d: %s 桌面区域=(%ld,%ld)-(%ld,%ld) 主屏=%dx%d").c_str(), chosenIdx, devName,
			outputDesc.DesktopCoordinates.left, outputDesc.DesktopCoordinates.top,
			outputDesc.DesktopCoordinates.right, outputDesc.DesktopCoordinates.bottom,
			GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
	}
	// [v184] 创建期即检测HDR: 桌面复制描述符里的桌面帧格式, 非BGRA8说明HDR开启
	// 注意: DuplicateOutput失败(被占用)时m_duplication为空, 必须先判hr否则空指针崩溃
	if (SUCCEEDED(hr) && m_duplication) {
		DXGI_OUTDUPL_DESC dupDesc = {};
		m_duplication->GetDesc(&dupDesc);
		if (dupDesc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
			LOG_WARN(OBS5(u8"[捕获诊断] 桌面帧格式=%d(非BGRA8)——系统已开启HDR! DXGI/Hook捕获会黑屏失效, 请关闭: 设置→系统→屏幕→HDR(或改用WGC模式)").c_str(), (int)dupDesc.ModeDesc.Format);
		}
	}
	m_screenWidth = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
	m_screenHeight = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
	output1->Release(); chosen->Release(); adapter->Release(); dxgiDevice->Release();
	return SUCCEEDED(hr);
}
bool ScreenCapture::createSharedTextures() {
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = m_captureWidth;
	desc.Height = m_captureHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	// [v185] 不带MISC_SHARED: CUDA官方sample(simpleD3D11Texture)的互操作纹理无共享标志,
	// cudaGraphicsD3D11RegisterResource通过设备绑定直接访问资源(经cudaD3D11SetDirect3DDevice),
	// 不需要OS级共享——实测legacy MISC_SHARED导致注册返回invalid argument
	for (int i = 0; i < 2; ++i) {
		HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_sharedTex[i]);
		if (FAILED(hr)) m_sharedTex[i] = nullptr;
	}
	return m_sharedTex[0] != nullptr && m_sharedTex[1] != nullptr;
}
bool ScreenCapture::createStagingTexture() {
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = m_captureWidth;
	desc.Height = m_captureHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_stagingTex);
	return SUCCEEDED(hr);
}
bool ScreenCapture::cudaRegisterSharedTexture() {
	if (!m_sharedTex[0] && !m_sharedTex[1]) {
		LOG_WARN(OBS1(u8"CUDA互操作: 共享纹理未创建").c_str());
		return false;
	}
	cudaError_t err = cudaSetDevice(m_cudaGpuId);
	if (err != cudaSuccess) {
		LOG_WARN(OBS2(u8"CUDA互操作: cudaSetDevice(%d)失败: %s").c_str(), m_cudaGpuId, cudaGetErrorString(err));
		return false;
	}
	// [v185] D3D设备绑定: 早期绑定(initEarlyDevice)已完成则直接用;
	// 未绑定才现场调用(运行中CUDA已激活会708, 属预期失败并回退CPU路径)
	if (!m_earlyBound) {
		err = cudaD3D11SetDirect3DDevice(m_device, m_cudaGpuId);
		if (err != cudaSuccess) {
			LOG_WARN(OBS3(u8"CUDA互操作: cudaD3D11SetDirect3DDevice失败: %s(%d)").c_str(), cudaGetErrorString(err), (int)err);
			m_cudaInterop = false;
			return false;
		}
	}
	// [v185] 清除残留异步错误: CUDA文档明示注册API"may also return error codes
	// from previous, asynchronous launches"——TRT引擎加载期间的遗留错误会被注册继承
	{
		cudaError_t stale = cudaGetLastError();
		if (stale != cudaSuccess) {
			LOG_WARN(OBS4(u8"CUDA互操作: 清除遗留CUDA错误: %s(%d)").c_str(), cudaGetErrorString(stale), (int)stale);
		}
	}
	// [v185] 根因修复: 删除nvcuda.dll入口守卫。nvcuda.dll是驱动API库, 从未导出
	// cudaGraphicsD3D11RegisterResource(运行时API, 在cudart里)——旧守卫GetProcAddress
	// 永远失败→提前return false→互操作从未启用过, 所有模式都在CPU回退路径跑(10-20ms)。
	// 项目已链接cudart, 直接调用即可
	bool anyOk = false;
	for (int i = 0; i < 2; ++i) {
		if (!m_sharedTex[i]) continue;
		err = cudaGraphicsD3D11RegisterResource(&m_cudaResource[i], m_sharedTex[i], cudaGraphicsRegisterFlagsReadOnly);
		if (err == cudaSuccess) anyOk = true;
		else {
			LOG_WARN(OBS8(u8"CUDA互操作: 注册共享纹理[%d]失败: %s(%d)").c_str(), i, cudaGetErrorString(err), (int)err);
			m_cudaResource[i] = nullptr;
		}
	}
	m_cudaInterop = anyOk;
	if (anyOk) {
		LOG_INFO(OBS1(u8"CUDA-D3D11互操作注册成功").c_str());
	}
	return anyOk;
}
// ========== WGC (Windows Graphics Capture) 实现 ==========
// 使用vtable直接调用，与诊断测试验证过的方式一致
bool ScreenCapture::initWGC() {
	HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
		LOG_WARN(OBS8(u8"WGC: RoInitialize失败 0x%X").c_str(), hr);
		return false;
	}
	// 创建WinRT IDirect3DDevice
	IDXGIDevice* dxgiDevice = nullptr;
	hr = m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
	if (FAILED(hr)) return false;
	IInspectable* wgcDeviceIns = nullptr;
	hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, &wgcDeviceIns);
	dxgiDevice->Release();
	if (FAILED(hr)) {
		LOG_WARN(OBS1(u8"WGC: WinRT D3D设备创建失败 0x%X").c_str(), hr);
		return false;
	}
	m_wgcDevice = wgcDeviceIns;
	// 获取IGraphicsCaptureItemInterop工厂
	HSTRING_HEADER itemHeader;
	HSTRING itemClassName;
	static std::wstring itemStr = OBSWS1(L"Windows.Graphics.Capture.GraphicsCaptureItem");
	hr = WindowsCreateStringReference(itemStr.c_str(), (UINT32)itemStr.size(), &itemHeader, &itemClassName);
	if (FAILED(hr)) return false;
	IInspectable* itemFactory = nullptr;
	hr = RoGetActivationFactory(itemClassName, IID_IGraphicsCaptureItemInterop, (void**)&itemFactory);
	if (FAILED(hr) || !itemFactory) {
		LOG_WARN(OBS2(u8"WGC: ItemInterop工厂获取失败 0x%X").c_str(), hr);
		return false;
	}
	// 通过vtable[4]调用CreateForMonitor
	void** itemVt = *(void***)itemFactory;
	typedef HRESULT (__stdcall *PFN_CreateForMonitor)(IInspectable*, HMONITOR, REFIID, void**);
	PFN_CreateForMonitor pfnCreateForMonitor = (PFN_CreateForMonitor)itemVt[4];
	HMONITOR monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
	IInspectable* captureItem = nullptr;
	hr = pfnCreateForMonitor(itemFactory, monitor, IID_IGraphicsCaptureItem, (void**)&captureItem);
	itemFactory->Release();
	if (FAILED(hr) || !captureItem) {
		LOG_WARN(OBS3(u8"WGC: CreateForMonitor失败 0x%X").c_str(), hr);
		return false;
	}
	m_wgcItem = captureItem;
	// 获取FramePool工厂 (优先CreateFreeThreaded)
	HSTRING_HEADER poolHeader;
	HSTRING poolClassName;
	static std::wstring poolStr = OBSWS2(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool");
	hr = WindowsCreateStringReference(poolStr.c_str(), (UINT32)poolStr.size(), &poolHeader, &poolClassName);
	if (FAILED(hr)) return false;
	IInspectable* framePool = nullptr;
	// 尝试Statics2 (CreateFreeThreaded)
	IInspectable* poolFactory2 = nullptr;
	hr = RoGetActivationFactory(poolClassName, IID_IDirect3D11CaptureFramePoolStatics2, (void**)&poolFactory2);
	if (SUCCEEDED(hr) && poolFactory2) {
		void** vt = *(void***)poolFactory2;
		// vtable[6] = CreateFreeThreaded, SizeInt32按值传递
		typedef HRESULT (__stdcall *PFN_CreateFreeThreaded)(IInspectable*, IInspectable*, INT32, INT32, WGCSizeInt32, IInspectable**);
		PFN_CreateFreeThreaded pfn = (PFN_CreateFreeThreaded)vt[6];
		WGCSizeInt32 size = {m_screenWidth, m_screenHeight};
		hr = pfn(poolFactory2, wgcDeviceIns, 87, 3, size, &framePool);
		poolFactory2->Release();
		LOG_INFO(OBS4(u8"WGC: CreateFreeThreaded=0x%X pool=%p").c_str(), hr, framePool);
	}
	if (!framePool) {
		// 回退到Statics (Create)
		IInspectable* poolFactory = nullptr;
		hr = RoGetActivationFactory(poolClassName, IID_IDirect3D11CaptureFramePoolStatics, (void**)&poolFactory);
		if (SUCCEEDED(hr) && poolFactory) {
			void** vt = *(void***)poolFactory;
			typedef HRESULT (__stdcall *PFN_Create)(IInspectable*, IInspectable*, INT32, INT32, WGCSizeInt32, IInspectable**);
			PFN_Create pfn = (PFN_Create)vt[6];
			WGCSizeInt32 size = {m_screenWidth, m_screenHeight};
			hr = pfn(poolFactory, wgcDeviceIns, 87, 3, size, &framePool);
			poolFactory->Release();
			LOG_INFO(OBS5(u8"WGC: Create=0x%X pool=%p").c_str(), hr, framePool);
		}
	}
	if (!framePool) {
		LOG_WARN(OBS6(u8"WGC: FramePool创建失败").c_str());
		return false;
	}
	m_wgcFramePool = framePool;
	// 通过vtable[10]调用CreateCaptureSession
	void** poolVt = *(void***)framePool;
	typedef HRESULT (__stdcall *PFN_CreateSession)(IInspectable*, IInspectable*, IInspectable**);
	PFN_CreateSession pfnCreateSession = (PFN_CreateSession)poolVt[10];
	IInspectable* session = nullptr;
	hr = pfnCreateSession(framePool, captureItem, &session);
	if (FAILED(hr) || !session) {
		LOG_WARN(OBS7(u8"WGC: CreateCaptureSession失败 0x%X").c_str(), hr);
		return false;
	}
	m_wgcSession = session;
	// 隐藏边框: 优先通过Session3 API, 失败则启动后台线程隐藏
	bool borderHidden = false;
	void* session3 = nullptr;
	if (SUCCEEDED(session->QueryInterface(IID_IGraphicsCaptureSession3, &session3)) && session3) {
		void** s3vt = *(void***)session3;
		typedef HRESULT (__stdcall *PFN_putBool)(void*, boolean);
		HRESULT borderHr = ((PFN_putBool)s3vt[7])(session3, false);
		((IUnknown*)session3)->Release();
		if (SUCCEEDED(borderHr)) {
			borderHidden = true;
			LOG_INFO(OBS8(u8"WGC: 已通过API隐藏边框").c_str());
		} else {
			LOG_WARN(OBS8(u8"WGC: put_IsBorderRequired失败 0x%X").c_str(), borderHr);
		}
	} else {
		LOG_WARN(OBS8(u8"WGC: Session3不可用(需Win10 2004+)，将使用后台线程隐藏黄边").c_str());
	}
	// 隐藏光标 (Session2 vtable[7] = put_IsCursorCaptureEnabled)
	void* session2 = nullptr;
	if (SUCCEEDED(session->QueryInterface(IID_IGraphicsCaptureSession2, &session2)) && session2) {
		void** s2vt = *(void***)session2;
		typedef HRESULT (__stdcall *PFN_putBool)(void*, boolean);
		HRESULT cursorHr = ((PFN_putBool)s2vt[7])(session2, false);
		((IUnknown*)session2)->Release();
		if (SUCCEEDED(cursorHr))
			LOG_INFO(OBS1(u8"WGC: 已隐藏光标").c_str());
		else
			LOG_WARN(OBS1(u8"WGC: 隐藏光标失败 0x%X").c_str(), cursorHr);
	}
	// 启动捕获 (Session vtable[6] = StartCapture)
	void** sessionVt = *(void***)session;
	typedef HRESULT (__stdcall *PFN_StartCapture)(IInspectable*);
	hr = ((PFN_StartCapture)sessionVt[6])(session);
	if (FAILED(hr)) {
		LOG_WARN(OBS2(u8"WGC: StartCapture失败 0x%X").c_str(), hr);
		return false;
	}
	m_wgcStarted = true;
	// 如果API隐藏失败，启动后台线程持续查找并隐藏黄边窗口
	if (!borderHidden) {
		m_targetMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
		startBorderHideThread();
	}
	LOG_INFO(OBS3(u8"WGC: 初始化完成，屏幕%d x %d %s").c_str(), m_screenWidth, m_screenHeight,
		borderHidden ? u8"[边框已隐藏]" : u8"[后台隐藏黄边中]");
	return true;
}
// ========== WGC黄边窗口隐藏线程 ==========
struct BorderHideContext {
	ScreenCapture* self;
	HMONITOR targetMonitor;
	MONITORINFOEXW monitorInfo;
	std::vector<HWND> knownWindows;
};
static BOOL CALLBACK snapshotWindowsProc(HWND hwnd, LPARAM lParam) {
	auto* known = reinterpret_cast<std::vector<HWND>*>(lParam);
	known->push_back(hwnd);
	return TRUE;
}
static BOOL CALLBACK hideBorderWindowProc(HWND hwnd, LPARAM lParam) {
	auto* ctx = reinterpret_cast<BorderHideContext*>(lParam);
	if (!IsWindowVisible(hwnd)) return TRUE;
	// 跳过已知窗口
	for (HWND known : ctx->knownWindows) {
		if (hwnd == known) return TRUE;
	}
	// 检查窗口是否在目标显示器上
	HMONITOR hmon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
	if (hmon != ctx->targetMonitor) return TRUE;
	// 获取窗口类名
	wchar_t className[256] = {};
	GetClassNameW(hwnd, className, 256);
	// WGC黄边窗口特征: Windows.UI.Core.CoreWindow类, 由系统进程创建
	// 也可能是ApplicationFrameWindow
	bool isBorderWindow = false;
	if (wcsstr(className, L"CoreWindow") || wcsstr(className, L"ApplicationFrame")) {
		// 获取窗口矩形, 黄边窗口大小接近屏幕尺寸
		RECT rc;
		GetWindowRect(hwnd, &rc);
		int w = rc.right - rc.left;
		int h = rc.bottom - rc.top;
		int sw = ctx->monitorInfo.rcMonitor.right - ctx->monitorInfo.rcMonitor.left;
		int sh = ctx->monitorInfo.rcMonitor.bottom - ctx->monitorInfo.rcMonitor.top;
		// 黄边窗口覆盖整个显示器或接近显示器尺寸
		if (w >= sw - 10 && h >= sh - 10) {
			// 检查是否是进程自身的窗口(避免隐藏自己的窗口)
			DWORD pid;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid != GetCurrentProcessId()) {
				isBorderWindow = true;
			}
		}
	}
	if (isBorderWindow) {
		// 方法1: 设置为不可见
		ShowWindow(hwnd, SW_HIDE);
		// 方法2: 设置为分层窗口并完全透明
		LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
		SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED | WS_EX_TRANSPARENT);
		SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
		LOG_INFO(OBS4(u8"WGC: 已隐藏黄边窗口(hwnd=%p, 类名=%ls)").c_str(), hwnd, className);
	}
	return TRUE;
}
void ScreenCapture::startBorderHideThread() {
	stopBorderHideThread();
	m_borderThreadRunning.store(true);
	m_borderThread = std::thread([this]() {
		// 先快照已知窗口
		BorderHideContext ctx = {};
		ctx.self = this;
		ctx.targetMonitor = m_targetMonitor;
		ctx.monitorInfo.cbSize = sizeof(ctx.monitorInfo);
		GetMonitorInfoW(m_targetMonitor, &ctx.monitorInfo);
		// 快照当前所有窗口
		EnumWindows(snapshotWindowsProc, reinterpret_cast<LPARAM>(&ctx.knownWindows));
		LOG_INFO(OBS5(u8"WGC: 黄边隐藏线程启动, 已快照%d个窗口").c_str(), (int)ctx.knownWindows.size());
		int attempt = 0;
		while (m_borderThreadRunning.load()) {
			Sleep(200);
			attempt++;
			EnumWindows(hideBorderWindowProc, reinterpret_cast<LPARAM>(&ctx));
			// 5秒后降低检测频率
			if (attempt > 25) Sleep(800);
			// 30秒后停止(黄边应该在几秒内被隐藏)
			if (attempt > 150) break;
		}
		LOG_INFO(OBS6(u8"WGC: 黄边隐藏线程退出").c_str());
	});
}
void ScreenCapture::stopBorderHideThread() {
	m_borderThreadRunning.store(false);
	if (m_borderThread.joinable()) {
		m_borderThread.join();
	}
}
void ScreenCapture::releaseWGC() {
	m_wgcStarted = false;
	stopBorderHideThread();
	if (m_wgcFrame) {
		((IInspectable*)m_wgcFrame)->Release();
		m_wgcFrame = nullptr;
	}
	if (m_wgcSession) {
		((IInspectable*)m_wgcSession)->Release();
		m_wgcSession = nullptr;
	}
	if (m_wgcFramePool) {
		((IInspectable*)m_wgcFramePool)->Release();
		m_wgcFramePool = nullptr;
	}
	if (m_wgcItem) {
		((IInspectable*)m_wgcItem)->Release();
		m_wgcItem = nullptr;
	}
	if (m_wgcDevice) {
		((IInspectable*)m_wgcDevice)->Release();
		m_wgcDevice = nullptr;
	}
}
CaptureFrame ScreenCapture::captureWGC(int centerX, int centerY, int bufIdx, bool needCPU) {
	CaptureFrame frame;
	if (!m_wgcStarted || !m_wgcFramePool) return frame;
	frame.width = m_captureWidth;
	frame.height = m_captureHeight;
	bufIdx = bufIdx & 1;
	if (m_mapped) unmap();
	// 释放上一帧
	if (m_wgcFrame) {
		((IInspectable*)m_wgcFrame)->Release();
		m_wgcFrame = nullptr;
	}
	// 通过vtable[7]调用TryGetNextFrame
	IInspectable* pool = (IInspectable*)m_wgcFramePool;
	void** poolVt = *(void***)pool;
	typedef HRESULT (__stdcall *PFN_TryGetNextFrame)(IInspectable*, IInspectable**);
	IInspectable* wgcFrame = nullptr;
	HRESULT hr = ((PFN_TryGetNextFrame)poolVt[7])(pool, &wgcFrame);
	if (FAILED(hr) || !wgcFrame) return frame;
	// ── 排空帧池到最新帧(延迟关键修复) ──
	// TryGetNextFrame是FIFO队列: 捕获线程一旦卡顿(游戏/直播占满GPU时必发生),
	// 帧池里积压2~3帧旧画面, 按顺序处理会把过期帧当当前帧 → 实测延迟多16~33ms。
	// 循环取到队尾(最新帧), 中间的旧帧立即释放 → 永远处理最新画面。
	for (;;) {
		IInspectable* newerFrame = nullptr;
		HRESULT hrNewer = ((PFN_TryGetNextFrame)poolVt[7])(pool, &newerFrame);
		if (FAILED(hrNewer) || !newerFrame) break;
		((IInspectable*)wgcFrame)->Release();
		wgcFrame = newerFrame;
	}
	m_wgcFrame = wgcFrame;
	// 通过Frame vtable[6]调用get_Surface
	void** frameVt = *(void***)wgcFrame;
	typedef HRESULT (__stdcall *PFN_getSurface)(IInspectable*, IInspectable**);
	IInspectable* surface = nullptr;
	hr = ((PFN_getSurface)frameVt[6])(wgcFrame, &surface);
	if (FAILED(hr) || !surface) { wgcFrame->Release(); m_wgcFrame = nullptr; return frame; }
	// 从Surface获取D3D11 Texture2D (通过IDirect3DDxgiInterfaceAccess vtable[3] = GetInterface)
	void* dxgiAccess = nullptr;
	hr = surface->QueryInterface(IID_IDirect3DDxgiInterfaceAccess, &dxgiAccess);
	surface->Release();
	if (FAILED(hr) || !dxgiAccess) { wgcFrame->Release(); m_wgcFrame = nullptr; return frame; }
	void** daVt = *(void***)dxgiAccess;
	typedef HRESULT (__stdcall *PFN_GetInterface)(void*, REFIID, void**);
	ID3D11Texture2D* desktopTexture = nullptr;
	hr = ((PFN_GetInterface)daVt[3])(dxgiAccess, __uuidof(ID3D11Texture2D), (void**)&desktopTexture);
	((IUnknown*)dxgiAccess)->Release();
	if (FAILED(hr) || !desktopTexture) { wgcFrame->Release(); m_wgcFrame = nullptr; return frame; }
	// 裁剪并复制到staging/shared纹理
	D3D11_TEXTURE2D_DESC desc;
	desktopTexture->GetDesc(&desc);
	int left = std::clamp(centerX - m_captureWidth / 2, 0, std::max(0, (int)desc.Width - m_captureWidth));
	int top = std::clamp(centerY - m_captureHeight / 2, 0, std::max(0, (int)desc.Height - m_captureHeight));
	D3D11_BOX box = {};
	box.left = left; box.top = top;
	box.right = left + m_captureWidth; box.bottom = top + m_captureHeight;
	box.front = 0; box.back = 1;
	if (m_sharedTex[bufIdx]) {
		m_context->CopySubresourceRegion(m_sharedTex[bufIdx], 0, 0, 0, 0, desktopTexture, 0, &box);
		frame.cudaResource = m_cudaInterop ? m_cudaResource[bufIdx] : nullptr;
	}
	if (needCPU && m_stagingTex) {
		m_context->CopySubresourceRegion(m_stagingTex, 0, 0, 0, 0, desktopTexture, 0, &box);
		hr = m_context->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &m_mappedRes);
		if (SUCCEEDED(hr)) {
			frame.data = (uint8_t*)m_mappedRes.pData;
			frame.pitch = m_mappedRes.RowPitch;
			m_mapped = true;
		}
	}
	frame.valid = true;
	desktopTexture->Release();
	return frame;
}
// ========== Hook (持久DXGI + 定期重建) 实现 ==========
bool ScreenCapture::hookCreateDup() {
	if (m_duplication) { m_hookDupActive = true; return true; }
	if (!m_device) return false;
	IDXGIDevice* dxgiDevice = nullptr;
	if (FAILED(m_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) return false;
	IDXGIAdapter* adapter = nullptr;
	if (FAILED(dxgiDevice->GetAdapter(&adapter))) { dxgiDevice->Release(); return false; }
	IDXGIOutput* output = nullptr;
	if (FAILED(adapter->EnumOutputs(0, &output))) { adapter->Release(); dxgiDevice->Release(); return false; }
	IDXGIOutput1* output1 = nullptr;
	if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) { output->Release(); adapter->Release(); dxgiDevice->Release(); return false; }
	HRESULT hr = output1->DuplicateOutput(m_device, &m_duplication);
	if (SUCCEEDED(hr)) {
		DXGI_OUTPUT_DESC desc;
		output1->GetDesc(&desc);
		m_screenWidth = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
		m_screenHeight = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
		m_hookCreateTime = std::chrono::steady_clock::now();
	}
	output1->Release(); output->Release(); adapter->Release(); dxgiDevice->Release();
	m_hookDupActive = SUCCEEDED(hr);
	return m_hookDupActive;
}
void ScreenCapture::hookReleaseDup() {
	if (m_duplication) {
		m_duplication->Release();
		m_duplication = nullptr;
	}
	m_hookDupActive = false;
}
void ScreenCapture::hookRebuildDup() {
	hookReleaseDup();
	hookCreateDup();
}
CaptureFrame ScreenCapture::captureHook(int centerX, int centerY, int bufIdx, bool needCPU) {
	CaptureFrame frame;
	if (!m_device || !m_context) return frame;
	if (m_mapped) unmap();
	if (m_hookDupActive) {
		auto elapsed = std::chrono::steady_clock::now() - m_hookCreateTime;
		// 随机抖动: 3~8秒之间随机重建，避免固定间隔被检测
		thread_local std::mt19937 rng(std::random_device{}());
		thread_local std::uniform_int_distribution<int> dist(3, 8);
		thread_local int nextRebuildSec = dist(rng);
		if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= nextRebuildSec) {
			nextRebuildSec = dist(rng);
			hookRebuildDup();
		}
	}
	if (!m_hookDupActive && !hookCreateDup()) return frame;
	bufIdx = bufIdx & 1;
	// [v184] 排空队列取最新帧(与captureDXGI一致): 避免FIFO积压把过期帧当当前帧
	bool gotNew = false;
	D3D11_TEXTURE2D_DESC desc = {};
	for (;;) {
		DXGI_OUTDUPL_FRAME_INFO frameInfo;
		IDXGIResource* resource = nullptr;
		HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &resource);
		if (hr == DXGI_ERROR_WAIT_TIMEOUT || FAILED(hr)) {
			if (hr == DXGI_ERROR_ACCESS_LOST) hookRebuildDup();
			break;
		}
		ID3D11Texture2D* desktopTexture = nullptr;
		if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture))) {
			resource->Release(); m_duplication->ReleaseFrame(); break;
		}
		desktopTexture->GetDesc(&desc);
		int left = std::clamp(centerX - m_captureWidth / 2, 0, std::max(0, (int)desc.Width - m_captureWidth));
		int top = std::clamp(centerY - m_captureHeight / 2, 0, std::max(0, (int)desc.Height - m_captureHeight));
		D3D11_BOX box = {};
		box.left = left; box.top = top;
		box.right = left + m_captureWidth; box.bottom = top + m_captureHeight;
		box.front = 0; box.back = 1;
		if (m_sharedTex[bufIdx]) {
			m_context->CopySubresourceRegion(m_sharedTex[bufIdx], 0, 0, 0, 0, desktopTexture, 0, &box);
		}
		if (needCPU && m_stagingTex) {
			m_context->CopySubresourceRegion(m_stagingTex, 0, 0, 0, 0, desktopTexture, 0, &box);
		}
		gotNew = true;
		desktopTexture->Release();
		resource->Release();
		m_duplication->ReleaseFrame();
	}
	if (!gotNew) return frame;
	frame.cudaResource = (m_sharedTex[bufIdx] && m_cudaInterop) ? m_cudaResource[bufIdx] : nullptr;
	if (needCPU && m_stagingTex) {
		HRESULT hr = m_context->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &m_mappedRes);
		if (SUCCEEDED(hr)) {
			frame.data = (uint8_t*)m_mappedRes.pData;
			frame.pitch = m_mappedRes.RowPitch;
			m_mapped = true;
		}
	}
	frame.width = m_captureWidth;
	frame.height = m_captureHeight;
	frame.valid = true;
	return frame;
}
// ========== DXGI 标准捕获 ==========
CaptureFrame ScreenCapture::captureDXGI(int centerX, int centerY, int bufIdx, bool needCPU) {
	CaptureFrame frame;
	if (!m_initialized || !m_duplication) return frame;
	if (m_mapped) unmap();
	bufIdx = bufIdx & 1;
	// [v184] 排空队列取最新帧: AcquireNextFrame是FIFO队列, 捕获线程一旦慢于游戏帧率
	// (游戏+直播占满GPU时必发生), 队列里积压2~3帧旧画面, 按序单帧取用会把过期帧当当前帧。
	// 循环"采集→拷贝→释放"直到超时, 最后一次拷贝的就是最新画面(与WGC排空逻辑一致)
	bool gotNew = false;
	D3D11_TEXTURE2D_DESC desc = {};
	for (;;) {
		DXGI_OUTDUPL_FRAME_INFO frameInfo;
		IDXGIResource* resource = nullptr;
		HRESULT hr = m_duplication->AcquireNextFrame(0, &frameInfo, &resource);
		if (hr == DXGI_ERROR_ACCESS_LOST) {
			if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
			createDuplication();
			return frame;
		}
		if (hr == DXGI_ERROR_WAIT_TIMEOUT || FAILED(hr)) break;
		ID3D11Texture2D* desktopTexture = nullptr;
		if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture))) {
			resource->Release(); m_duplication->ReleaseFrame(); break;
		}
		desktopTexture->GetDesc(&desc);
		// [v181] 帧格式诊断: HDR开启时桌面帧是R10G10B10A2(10bit), 向B8G8R8A8共享纹理拷贝
		// 会静默失败→捕获黑屏"不生效"。检测到即告警
		{
			static int s_fmtLogCd = 0;
			if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && s_fmtLogCd <= 0) {
				s_fmtLogCd = 3600;
				LOG_WARN(OBS2(u8"[捕获诊断] 桌面帧格式=%d(非BGRA8)——很可能系统开启了HDR! 请关闭: 设置→系统→屏幕→HDR。HDR下DXGI/Hook捕获会黑屏失效").c_str(), (int)desc.Format);
			}
			if (s_fmtLogCd > 0) s_fmtLogCd--;
		}
		int left = std::clamp(centerX - m_captureWidth / 2, 0, std::max(0, (int)desc.Width - m_captureWidth));
		int top = std::clamp(centerY - m_captureHeight / 2, 0, std::max(0, (int)desc.Height - m_captureHeight));
		D3D11_BOX box = {};
		box.left = left; box.top = top;
		box.right = left + m_captureWidth; box.bottom = top + m_captureHeight;
		box.front = 0; box.back = 1;
		if (m_sharedTex[bufIdx]) {
			m_context->CopySubresourceRegion(m_sharedTex[bufIdx], 0, 0, 0, 0, desktopTexture, 0, &box);
		}
		if (needCPU && m_stagingTex) {
			m_context->CopySubresourceRegion(m_stagingTex, 0, 0, 0, 0, desktopTexture, 0, &box);
		}
		gotNew = true;
		desktopTexture->Release();
		resource->Release();
		m_duplication->ReleaseFrame();
	}
	if (!gotNew) return frame;
	frame.cudaResource = (m_sharedTex[bufIdx] && m_cudaInterop) ? m_cudaResource[bufIdx] : nullptr;
	if (needCPU && m_stagingTex) {
		HRESULT hr = m_context->Map(m_stagingTex, 0, D3D11_MAP_READ, 0, &m_mappedRes);
		if (SUCCEEDED(hr)) {
			frame.data = (uint8_t*)m_mappedRes.pData;
			frame.pitch = m_mappedRes.RowPitch;
			m_mapped = true;
		}
	}
	frame.width = m_captureWidth;
	frame.height = m_captureHeight;
	frame.valid = true;
	return frame;
}
// ========== 统一捕获入口 ==========
CaptureFrame ScreenCapture::capture(int centerX, int centerY, int bufIdx, bool needCPU) {
	switch (m_captureMode) {
	case CaptureMode::WGC: return captureWGC(centerX, centerY, bufIdx, needCPU);
	case CaptureMode::Hook: return captureHook(centerX, centerY, bufIdx, needCPU);
	default: return captureDXGI(centerX, centerY, bufIdx, needCPU);
	}
}
void ScreenCapture::unmap() {
	if (m_mapped && m_stagingTex && m_context) {
		m_context->Unmap(m_stagingTex, 0);
		m_mapped = false;
	}
}
void ScreenCapture::release() {
	if (m_cudaInterop) {
		for (int i = 0; i < 2; ++i) {
			if (m_cudaResource[i]) {
				cudaGraphicsUnregisterResource(m_cudaResource[i]);
				m_cudaResource[i] = nullptr;
			}
		}
		m_cudaInterop = false;
	}
	if (m_mapped) unmap();
	releaseWGC();
	if (m_hookDupActive) hookReleaseDup();
	if (m_duplication) { m_duplication->Release(); m_duplication = nullptr; }
	for (int i = 0; i < 2; ++i) {
		if (m_sharedTex[i]) { m_sharedTex[i]->Release(); m_sharedTex[i] = nullptr; }
	}
	if (m_stagingTex) { m_stagingTex->Release(); m_stagingTex = nullptr; }
	// [v185] 早期绑定的设备保留不释放: 重新绑定需CUDA未激活(运行中必失败),
	// 保留到下次initialize复用, 程序退出时统一释放
	if (!m_earlyBound) {
		if (m_context) { m_context->Release(); m_context = nullptr; }
		if (m_device) { m_device->Release(); m_device = nullptr; }
	}
	m_initialized = false;
	m_hookDupActive = false;
}
void ScreenCapture::getScreenSize(int& width, int& height) {
	width = m_screenWidth;
	height = m_screenHeight;
}
