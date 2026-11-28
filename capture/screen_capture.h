#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cuda_runtime.h>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
enum class CaptureMode { WGC = 0, DXGI = 1, Hook = 2 };
struct CaptureFrame {
	uint8_t* data = nullptr;
	int width = 0;
	int height = 0;
	int pitch = 0;
	bool valid = false;
	cudaGraphicsResource* cudaResource = nullptr;
};
class ScreenCapture {
public:
	ScreenCapture();
	~ScreenCapture();
	bool initialize(int captureWidth, int captureHeight, int cudaGpuId = 0, CaptureMode mode = CaptureMode::WGC);
	bool initEarlyDevice(int cudaGpuId);
	void release();
	CaptureFrame capture(int centerX, int centerY, int bufIdx = 0, bool needCPU = true);
	void unmap();
	void getScreenSize(int& width, int& height);
	bool cudaRegisterSharedTexture();
	bool isCudaInteropEnabled() const { return m_cudaInterop; }
	ID3D11Device* getDevice() const { return m_device; }
	ID3D11DeviceContext* getContext() const { return m_context; }
	CaptureMode getCaptureMode() const { return m_captureMode; }
private:
	ID3D11Device* m_device = nullptr;
	ID3D11DeviceContext* m_context = nullptr;
	IDXGIOutputDuplication* m_duplication = nullptr;
	ID3D11Texture2D* m_stagingTex = nullptr;
	ID3D11Texture2D* m_sharedTex[2] = {};
	cudaGraphicsResource* m_cudaResource[2] = {};
	int m_screenWidth = 0;
	int m_screenHeight = 0;
	int m_captureWidth = 0;
	int m_captureHeight = 0;
	int m_cudaGpuId = 0;
	bool m_initialized = false;
	bool m_mapped = false;
	bool m_cudaInterop = false;
	bool m_earlyBound = false;   // [v185] 设备已早期绑定(cudaD3D11SetDirect3DDevice在CUDA激活前完成)
	int m_boundGpuId = -1;       // [v185] 已绑定设备对应的CUDA GPU id
	D3D11_MAPPED_SUBRESOURCE m_mappedRes = {};
	CaptureMode m_captureMode = CaptureMode::WGC;
	// WGC members
	void* m_wgcDevice = nullptr;
	void* m_wgcFramePool = nullptr;
	void* m_wgcSession = nullptr;
	void* m_wgcItem = nullptr;
	void* m_wgcFrame = nullptr;
	bool m_wgcStarted = false;
	// Hook members
	bool m_hookDupActive = false;
	std::chrono::steady_clock::time_point m_hookCreateTime;
	// WGC黄边隐藏线程
	std::thread m_borderThread;
	std::atomic<bool> m_borderThreadRunning{false};
	HMONITOR m_targetMonitor = nullptr;
	void startBorderHideThread();
	void stopBorderHideThread();
	static DWORD WINAPI borderHideProc(LPVOID param);
	bool createDevice();
	bool createDuplication();
	bool createStagingTexture();
	bool createSharedTextures();
	CaptureFrame captureDXGI(int centerX, int centerY, int bufIdx, bool needCPU);
	CaptureFrame captureWGC(int centerX, int centerY, int bufIdx, bool needCPU);
	CaptureFrame captureHook(int centerX, int centerY, int bufIdx, bool needCPU);
	bool initWGC();
	void releaseWGC();
	bool hookCreateDup();
	void hookReleaseDup();
	void hookRebuildDup();
};
