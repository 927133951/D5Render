#pragma once
#include <d3d11.h>
#include <dxgi.h>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include "config/config.h"
#include "capture/screen_capture.h"
#include "inference/trt_engine.h"
#include "inference/dml_engine.h"
#include "inference/yolo_postprocess.h"
#include "inference/cuda_preprocess.h"
#include "inference/onnx_metadata.h"
#include "aim/aim_assist.h"
#include "inference/gpu_perf.h"
#include "input/logitech_driver.h"
#include "gui/app_log.h"
#include "utils/update_checker.h"
class App {
public:
	App();
	~App();
	bool initialize(HINSTANCE hInstance);
	void run();
	void shutdown();
private:
	HWND m_hwnd = nullptr;
	HWND m_previewHwnd = nullptr;
	ID3D11Device* m_d3dDevice = nullptr;
	ID3D11DeviceContext* m_d3dContext = nullptr;
	IDXGISwapChain* m_swapChain = nullptr;
	ID3D11RenderTargetView* m_renderTarget = nullptr;
	D5RenderConfig m_config;
	ScreenCapture m_capture;
	TRTEngine m_engine;
	DMLEngine m_dmlEngine;
	AimAssist m_aim;
	LogitechDriver m_driver;
	std::atomic<bool> m_running{false};
	std::atomic<bool> m_modelLoaded{false};
	std::atomic<bool> m_aimActive{false};
	std::atomic<bool> m_aimToggled{false};
	bool m_toggleKeyWasPressed = false;
	bool m_homeKeyWasPressed = false;  // VK_HOME 显隐切换边沿检测
	int m_keyDebounce = 0;             // [v190] 热键去抖计数: 连续3帧无键才真正失活(滤GHUB宏导致的25Hz闪烁)
	std::atomic<bool> m_captureInited{false};
	std::atomic<bool> m_modelLoading{false};
	std::atomic<bool> m_loadCancel{false};
	std::atomic<bool> m_loadSuccess{false};
	std::atomic<int> m_loadProgress{0};
	std::string m_loadStatus;
	std::mutex m_loadMutex;
	std::thread m_loadThread;
	float m_fps = 0.0f;
	std::atomic<int> m_detectionCount{0};
	std::atomic<float> m_inferMs{0.0f};
	std::vector<Detection> m_lastDetections;
	std::mutex m_detectMutex;
	cudaStream_t m_cudaStream = nullptr;
	// GPU调度心跳: 每2ms一次微型kernel提交, 迫使WDDM在游戏长命令流中制造调度间隙
	// (实测: 直播助手窗口可见时其渲染提交意外产生同样效果, 此线程将该效果显式化)
	std::atomic<bool> m_heartbeatRunning{false};
	std::thread m_heartbeatThread;
	void* m_heartbeatBuf = nullptr;
	cudaStream_t m_heartbeatStream = nullptr;
	void startHeartbeat();
	void stopHeartbeat();
	// 自适应推理帧率目标(推理线程写, 捕获线程读)
	std::atomic<int> m_inferTargetFps{120};
	std::atomic<int> m_inferAutoFps{120};
	std::atomic<int> m_inferOverloadCount{0};
	std::atomic<float> m_inferCostEma{0.0f};
	cudaEvent_t m_gpuStartEvent = nullptr;
	cudaEvent_t m_gpuEndEvent = nullptr;
	cudaEvent_t m_preEndEvent = nullptr;
	// [v184] m_h2dEvent已删除: 共享事件有跨槽歧义, 改为FrameSlot内每槽独立h2dEvent

	std::atomic<float> m_gpuInferMs{0.0f};
	std::atomic<float> m_preGpuMs{0.0f};
	std::atomic<float> m_inferWaitMs{0.0f};   // 等待数据就绪耗时(H2D同步/互操作Map)
	std::atomic<float> m_captureMs{0.0f};
	GpuPerf m_gpuPerf;
	// 推理延迟诊断
	std::atomic<float> m_postprocMs{0.0f};
	float m_e2eMs = 0.0f;
	int m_inferSkipCount = 0;
	std::atomic<int> m_inferTotalFrames{0};
	std::atomic<float> m_inferMsMin{999.0f};
	std::atomic<float> m_inferMsMax{0.0f};
	std::atomic<float> m_inferMsSum{0.0f};
	std::atomic<int> m_inferDiagCooldown{0};
	std::chrono::steady_clock::time_point m_frameLimitNext;
	bool m_frameLimitInited = false;
	std::string m_detectedVersionName;
	bool m_versionAutoDetected = false;
	std::chrono::steady_clock::time_point m_lastTime;
	std::chrono::steady_clock::time_point m_fpsLastTime;
	int m_fpsCounter = 0;
	char m_modelPathBuf[512] = {};
	int m_modelNumClasses = 0;
	std::vector<ClassInfo> m_modelClasses;
	std::vector<int> m_classEnabled;
	std::map<std::string, std::vector<int>> m_modelClassMemory;
	std::vector<uint8_t> m_previewBuf;
	std::vector<uint8_t> m_previewBufWrite;
	int m_previewW = 0;
	int m_previewH = 0;
	std::atomic<bool> m_previewReady{false};
	std::string m_keyError;
	mutable std::atomic<int> m_activeAimSlot{-1};
	HINSTANCE m_hInstance = nullptr;
	std::atomic<bool> m_mouseTestRunning{false};
	bool m_hidden = false;
	bool m_fakeMinimized = false;
	RECT m_restoreRect = {0, 0, 0, 0};
	bool m_atLeftHeld = false;
	bool m_atFireActive = false;
	int m_atFireTick = 0;
	UpdateChecker m_updateChecker;
	UpdateInfo m_updateInfo;
	bool m_updateChecked = false;
	bool m_showUpdatePopup = false;
	void setAntiCapture(bool enable);
	void fakeMinimize();
	void restoreFromFakeMinimize();
	std::thread m_inferThread;
	std::thread m_captureThread;
		std::atomic<bool> m_inferRunning{false};
		std::atomic<bool> m_captureRunning{false};
		std::thread m_securityThread;
		std::atomic<bool> m_securityRunning{false};
	// 帧管线：捕获线程写→推理线程读
	struct FrameSlot {
		void* gpuBuf = nullptr;
		size_t gpuBufBytes = 0;      // [v218] gpuBuf实际容量(跨卡模式推理线程分配时使用)
		void* pinnedBuf = nullptr;   // [v184] 页锁定H2D中转: staging Map内存不可分页, 直接async拷贝会退化成同步
		size_t pinnedSize = 0;
		cudaEvent_t h2dEvent = nullptr;      // [v184] 每槽H2D完成事件(捕获线程在copyStream上记录)
		bool h2dPending = false;             // 仅捕获线程读写
		cudaEvent_t preDoneEvent = nullptr;  // [v184] gpuBuf消费完成事件(推理线程在引擎流上记录)
		bool preDoneRecorded = false;        // 推理线程写, 捕获线程读(经ready屏障)
		int width = 0;
		int height = 0;
		int pitch = 0;
		cudaGraphicsResource* cudaRes = nullptr;
		bool cudaInterop = false;
		int aimCX = 0;
		int aimCY = 0;
		float recoilX = 0;
		float recoilY = 0;
		long long capMs = 0;   // [v157] 帧捕获时刻(steady_clock毫秒), 用于丢弃过期推理结果
		std::atomic<bool> ready{false};
	};
	FrameSlot m_frameSlots[2];
	std::atomic<int> m_captureWriteSlot{0};
	std::atomic<int> m_lastWrittenSlot{-1};
	// 推理结果：推理线程写→UI线程读
	std::atomic<bool> m_resultReady{false};
	std::atomic<long long> m_resultCapMs{0};   // [v157] 结果对应帧的捕获时刻
	std::vector<Detection> m_resultDetections;
	int m_resultAimCX = 0, m_resultAimCY = 0;
	int m_resultFrameW = 0, m_resultFrameH = 0;
	float m_resultRecoilX = 0, m_resultRecoilY = 0;
	void inferLoop();
	void captureLoop();
	void processInference(FrameSlot& slot);
	std::vector<std::string> m_gpuNames;
	float m_panelSplitRatio = 0.55f;
	bool m_panelDragging = false;
	int m_screenW = 0, m_screenH = 0;
	void enumGPUs();
	const char* getClassName(int classId) const;
	const char* getClassDisplayName(int classId) const;
	bool checkAimKey() const;
	bool validateKeys();
	bool createWindow(HINSTANCE hInstance);
	bool createPreviewWindow();
	void destroyPreviewWindow();
	void updatePreviewWindow();
	void savePreviewPos();
	bool createDeviceD3D();
	void createRenderTarget();
	void cleanupRenderTarget();
	void setupImGui();
	void renderUI();
	void updateFPS();
	void startModelLoad();
	void cancelModelLoad();
	void switchDriver(DriverMode mode);
	void updateClassList();
	std::vector<int> getSelectedClasses() const;
	void syncDisabledClasses();
	void resetInference();
	void startMouseTest();
	void renderBasicPanel();
	void renderParamsPanel();
	void renderAdvancedPanel();
	void renderLogPanel();
	void showUpdateDialog();
	static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
		static LRESULT WINAPI PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
	};
