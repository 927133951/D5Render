#include "app.h"
#include "theme.h"
#include "app_log.h"
#include "utils/obfstr.h"
#include "utils/anti_debug.h"
#include "utils/gpu_clock.h"
#include <intrin.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <cuda_runtime.h>
#include <cuda.h>
#include <d3d11.h>
#include <mmsystem.h>
#include <tchar.h>
#include <commdlg.h>
#include <cstdio>
#include <algorithm>
// 物理左键状态追踪（已移除WH_MOUSE_LL钩子，避免全局鼠标事件延迟）
static constexpr ULONG_PTR AT_MAGIC = 0x41545244; // "ATRD"
volatile DWORD s_ignorePhysUntil = 0; // 虚拟事件时间窗口（logitech_driver仍设置，保留兼容）
#include <cmath>
#include <shellapi.h>
#include <random>
// ── P0 防检测：窗口标识随机化 ──
// 反作弊通过 EnumWindows + GetClassName 枚举窗口特征。
// 固定类名/标题 = 签名靶子。每次启动重新生成，每次不同。
static std::wstring randomHexW(size_t len) {
	static const wchar_t hex[] = L"0123456789abcdef";
	static std::mt19937 rng{ std::random_device{}() };
	std::uniform_int_distribution<size_t> dist(0, 15);
	std::wstring s;
	for (size_t i = 0; i < len; ++i) s += hex[dist(rng)];
	return s;
}
static const std::wstring mainClassName = randomHexW(8);
static const std::wstring mainTitle = randomHexW(8);
static const std::wstring previewClassName = randomHexW(8);
static const std::wstring previewTitle = randomHexW(8);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
App::App() {
	m_config.loadFromFile();
	// GPU锁频: 防止空载降频导致推理延迟虚高(空载7ms+的根因)
	if (m_config.gpu_lock_clocks) gpu_clock::apply(m_config.gpu_id);
	strncpy_s(m_modelPathBuf, m_config.model_path.c_str(), sizeof(m_modelPathBuf) - 1);
}
App::~App() { shutdown(); }
bool App::initialize(HINSTANCE hInstance) {
	m_hInstance = hInstance;
	// [v185] CUDA同步模式=自旋: 必须在primary context激活之前设置。
	// 默认模式cudaEventSynchronize在GPU完成后要经过OS线程调度唤醒(0.5~2ms抖动),
	// 游戏满载时唤醒延迟更大; 自旋模式忙等, GPU完成→CPU继续的延迟降到微秒级
	// [v185] 关键: cudaSetDevice/cudaSetDeviceFlags在CUDA 12+都会立即激活primary
	// context, 使随后的cudaD3D11SetDirect3DDevice报708(device active)。
	// 必须用Driver API的cuDevicePrimaryCtxSetFlags(纯设置, 不激活)
	{
		CUresult initErr = cuInit(0);   // [v185] cuDeviceGet要求driver先初始化(不激活context)
		CUdevice cuDev = 0;
		CUresult devErr = initErr == CUDA_SUCCESS ? cuDeviceGet(&cuDev, m_config.gpu_id) : (CUresult)-1;
		CUresult flagErr = devErr == CUDA_SUCCESS ? cuDevicePrimaryCtxSetFlags(cuDev, CU_CTX_SCHED_SPIN) : (CUresult)-1;
		if (devErr == CUDA_SUCCESS && flagErr == CUDA_SUCCESS) {
			LOG_INFO(OBS1(u8"CUDA自旋同步模式已启用(消除推理完成后的唤醒抖动)").c_str());
		} else {
			LOG_WARN(OBS2(u8"CUDA自旋同步设置失败(init=%d dev=%d flags=%d), 使用默认同步模式").c_str(), (int)initErr, (int)devErr, (int)flagErr);
		}
		// [v218] 推理独立卡: 对推理卡同样设置自旋调度(纯设置不激活context)
		if (m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id) {
			CUdevice cuDev2 = 0;
			if (cuDeviceGet(&cuDev2, m_config.infer_gpu_id) == CUDA_SUCCESS &&
				cuDevicePrimaryCtxSetFlags(cuDev2, CU_CTX_SCHED_SPIN) == CUDA_SUCCESS) {
				LOG_INFO(OBS1(u8"推理独立卡自旋同步已启用").c_str());
			}
		}
	}
	// [v185] 捕获D3D设备早期绑定: cudaD3D11SetDirect3DDevice必须在CUDA设备激活
	// (TRT引擎加载/任何cudaSetDevice)之前调用, 否则报708。D3D设备创建与模型尺寸
	// 无关, 此处先行完成, 之后捕获初始化直接复用该设备 → 互操作零拷贝路径真正可用。
	// 该调用是进程内首次CUDA激活, 完成primary context创建与D3D设备绑定
	m_config.gpu_id = 0;  // [v218] 捕获恒定主屏卡(适配器0), 防老配置gpu_id=1/2残留绑错卡; 推理卡由infer_gpu_id独立控制
	if (!m_capture.initEarlyDevice(m_config.gpu_id)) {
		LOG_WARN(OBS3(u8"捕获设备早期绑定未完成, 互操作可能不可用(见上方日志)").c_str());
	}
	if (!createWindow(hInstance)) return false;
	if (!createDeviceD3D()) return false;
	createRenderTarget();
	setupImGui();
	enumGPUs();
	m_gpuPerf.detect(m_config.gpu_id);
	const auto& gi = m_gpuPerf.getInfo();
	LOG_INFO(OBS1(u8"GPU检测: %s | %s | 显存:%.1fGB | 性能等级:%s | 推荐帧率:%dFPS").c_str(),
		m_gpuPerf.getVendorName(), gi.name.c_str(), gi.vramGB(), m_gpuPerf.getTierName(), gi.autoTargetFps);
	LOG_INFO(OBS1(u8"D5 Render 视觉辅助软件启动").c_str());
	BOOL isElevated = FALSE;
	HANDLE hToken = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
		TOKEN_ELEVATION te = {};
		DWORD size = 0;
		if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &size)) {
			isElevated = te.TokenIsElevated;
		}
		CloseHandle(hToken);
	}
	if (!isElevated) {
		LOG_WARN(OBS2(u8"未以管理员权限运行! 鼠标驱动在游戏中可能无法工作，请右键选择[以管理员身份运行]").c_str());
	}
	switchDriver(m_config.driver_mode);
	// 进程优先级设为AboveNormal（而非HIGH），避免抢占游戏CPU时间
	// 推理和捕获线程通过线程级优先级保证及时性
	SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
	// 禁用Windows后台电源节流（Win10+）
	// POWER_THROTTLING_THREAD_STATE = 1 表示禁用节流
	typedef struct { ULONG Version; ULONG ControlMask; ULONG StateMask; } PWR_THROTTLE_STATE;
	HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
	if (hKernel32) {
		typedef BOOL(WINAPI* pfnSetProcessInformation)(HANDLE, ULONG, void*, ULONG);
		auto fn = (pfnSetProcessInformation)GetProcAddress(hKernel32, "SetProcessInformation");
		if (fn) {
			PWR_THROTTLE_STATE state = {};
			state.Version = 1;
			state.ControlMask = 1;
			state.StateMask = 0;
			fn(GetCurrentProcess(), 4/*ProcessPowerThrottling*/, &state, sizeof(state));
		}
	}
	// 设置1ms定时器精度，确保Sleep(1)真的只睡1ms
	typedef NTSTATUS(NTAPI* pfnNtSetTimerResolution)(ULONG, BOOLEAN, PULONG);
	HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
	if (hNtdll) {
		auto fnNt = (pfnNtSetTimerResolution)GetProcAddress(hNtdll, "NtSetTimerResolution");
		if (fnNt) {
			ULONG currentRes = 0;
			fnNt(10000, TRUE, &currentRes); // 10000单位=1ms
		}
	}
	if (m_config.show_preview) createPreviewWindow();
	if (m_config.anti_capture) setAntiCapture(true);
		antidebug::runPeriodicCheck();
		m_securityRunning.store(true);
		m_securityThread = std::thread([this]() {
			antidebug::hideThreadFromDebugger();
			while (m_securityRunning.load()) {
				antidebug::runFullCheck();
				Sleep(5000 + (int)(__rdtsc() % 5000));
			}
		});
	// 异步检查软件更新
	m_updateChecker.checkAsync([this](const UpdateInfo& info) {
		if (info.hasUpdate) {
			m_updateInfo = info;
			m_showUpdatePopup = true;
			LOG_INFO(OBS1(u8"检测到新版本: v%s，当前版本: v%s").c_str(), info.latestVersion.c_str(), D5_VERSION_STRING);
		}
	});
	return true;
}
const char* App::getClassName(int classId) const {
	if (classId >= 0 && classId < (int)m_modelClasses.size() && !m_modelClasses[classId].name.empty())
		return m_modelClasses[classId].name.c_str();
	return YOLOPostProcess::getCocoClassName(classId);
}
const char* App::getClassDisplayName(int classId) const {
	if (classId >= 0 && classId < (int)m_modelClasses.size() && !m_modelClasses[classId].name.empty())
		return m_modelClasses[classId].name.c_str();
	return YOLOPostProcess::getCocoClassDisplayName(classId);
}
bool App::checkAimKey() const {
	if (!m_config.aim_key_slots.empty()) {
		int active = m_activeAimSlot.load();
		if (active >= 0 && active < (int)m_config.aim_key_slots.size()) {
			const auto& slot = m_config.aim_key_slots[active];
			bool held = false;
			if (slot.key_mode == KeyMode::Single) {
				held = (GetAsyncKeyState(slot.vkCode) & 0x8000) != 0;
			} else {
				held = (GetAsyncKeyState(slot.vkCode) & 0x8000) != 0 &&
					   (GetAsyncKeyState(slot.combo_vkCode) & 0x8000) != 0;
			}
			if (held) return true;
			m_activeAimSlot.store(-1);
		}
		for (int i = 0; i < (int)m_config.aim_key_slots.size(); ++i) {
			const auto& slot = m_config.aim_key_slots[i];
			if (slot.key_mode == KeyMode::Single) {
				if ((GetAsyncKeyState(slot.vkCode) & 0x8000) != 0) {
					m_activeAimSlot.store(i);
					return true;
				}
			} else {
				if ((GetAsyncKeyState(slot.vkCode) & 0x8000) != 0 &&
					(GetAsyncKeyState(slot.combo_vkCode) & 0x8000) != 0) {
					m_activeAimSlot.store(i);
					return true;
				}
			}
		}
		return false;
	}
	if (m_config.key_mode == KeyMode::Single) {
		return (GetAsyncKeyState(m_config.aim_key1.vkCode) & 0x8000) != 0;
	}
	bool k1 = (GetAsyncKeyState(m_config.aim_key1.vkCode) & 0x8000) != 0;
	bool k2 = (GetAsyncKeyState(m_config.aim_key2.vkCode) & 0x8000) != 0;
	return k1 && k2;
}
bool App::validateKeys() {
	m_keyError.clear();
	if (!m_config.aim_key_slots.empty()) {
		for (int i = 0; i < (int)m_config.aim_key_slots.size(); ++i) {
			auto& slot = m_config.aim_key_slots[i];
			if (slot.key_mode == KeyMode::Combo && slot.vkCode == slot.combo_vkCode) {
				m_keyError = OBS1(u8"按键槽") + std::to_string(i + 1) + OBS2(u8": 组合键的两个按键不能相同!");
				return false;
			}
		}
		for (int i = 0; i < (int)m_config.aim_key_slots.size(); ++i) {
			for (int j = i + 1; j < (int)m_config.aim_key_slots.size(); ++j) {
				auto& a = m_config.aim_key_slots[i];
				auto& b = m_config.aim_key_slots[j];
				bool dup = false;
				if (a.key_mode == KeyMode::Single && b.key_mode == KeyMode::Single) {
					dup = (a.vkCode == b.vkCode);
				} else if (a.key_mode == KeyMode::Single && b.key_mode == KeyMode::Combo) {
					dup = (a.vkCode == b.vkCode || a.vkCode == b.combo_vkCode);
				} else if (a.key_mode == KeyMode::Combo && b.key_mode == KeyMode::Single) {
					dup = (a.vkCode == b.vkCode || a.combo_vkCode == b.vkCode);
				} else {
					dup = (a.vkCode == b.vkCode || a.vkCode == b.combo_vkCode || a.combo_vkCode == b.vkCode || a.combo_vkCode == b.combo_vkCode);
				}
				if (dup) {
					m_keyError = OBS3(u8"按键槽") + std::to_string(i + 1) + OBS4(u8"和") + std::to_string(j + 1) + OBS5(u8"存在重复按键，请修改!");
					return false;
				}
			}
		}
		return true;
	}
	if (m_config.key_mode == KeyMode::Combo) {
		if (m_config.aim_key1.vkCode == m_config.aim_key2.vkCode) {
			m_keyError = OBS2(u8"组合键模式: 两个按键不能相同!");
			return false;
		}
	}
	return true;
}
void App::fakeMinimize() {
	if (!m_hwnd || m_fakeMinimized) return;
	GetWindowRect(m_hwnd, &m_restoreRect);
	// 临时去掉标题栏(否则Windows标题栏高度卡住窗口最小高度), 才能压成超细条
	SetWindowLongW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
	int w = 72, h = 6;
	int screenW = GetSystemMetrics(SM_CXSCREEN);
	int screenH = GetSystemMetrics(SM_CYSCREEN);
	RECT work;
	if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
		screenW = work.right;
		screenH = work.bottom;
	}
	int x = screenW - w - 8;
	int y = screenH - h - 4;
	SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED);
	m_fakeMinimized = true;
}
void App::restoreFromFakeMinimize() {
	if (!m_hwnd || !m_fakeMinimized) return;
	// 恢复原样式(带标题栏)与原位置
	SetWindowLongW(m_hwnd, GWL_STYLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME | WS_VISIBLE);
	RECT rc = m_restoreRect;
	if (rc.right > rc.left && rc.bottom > rc.top) {
		SetWindowPos(m_hwnd, HWND_TOPMOST, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}
	m_fakeMinimized = false;
	SetForegroundWindow(m_hwnd);
}
void App::setAntiCapture(bool enable) {
	if (enable) {
		SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE);
		if (m_previewHwnd) SetWindowDisplayAffinity(m_previewHwnd, WDA_EXCLUDEFROMCAPTURE);
	} else {
		SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
		if (m_previewHwnd) SetWindowDisplayAffinity(m_previewHwnd, WDA_NONE);
	}
}
	bool App::createWindow(HINSTANCE hInstance) {
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.style = CS_CLASSDC;
		wc.lpfnWndProc = WndProc;
		wc.hInstance = hInstance;
		wc.lpszClassName = mainClassName.c_str();
		wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		RegisterClassExW(&wc);
		int screenW = GetSystemMetrics(SM_CXSCREEN);
		int screenH = GetSystemMetrics(SM_CYSCREEN);
		int winW = (int)(screenW * 0.42f);
		int winH = (int)(screenH * 0.48f);
		if (winW < 780) winW = 780;
		if (winH < 520) winH = 520;
		if (winW > screenW - 80) winW = screenW - 80;
		if (winH > screenH - 80) winH = screenH - 80;
		int posX = (screenW - winW) / 2;
		int posY = (screenH - winH) / 2;
		m_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, mainClassName.c_str(), mainTitle.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME, posX, posY, winW, winH, nullptr, nullptr, hInstance, this);
	if (!m_hwnd) return false;
	ShowWindow(m_hwnd, SW_SHOWDEFAULT);
	UpdateWindow(m_hwnd);
	return true;
}
	bool App::createPreviewWindow() {
		if (m_previewHwnd) return true;
		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = PreviewWndProc;
		wc.hInstance = m_hInstance;
		wc.lpszClassName = previewClassName.c_str();
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hIcon = LoadIconW(m_hInstance, MAKEINTRESOURCEW(101));
		RegisterClassExW(&wc);
		int px = m_config.preview_x, py = m_config.preview_y;
		int screenW = GetSystemMetrics(SM_CXSCREEN);
		int screenH = GetSystemMetrics(SM_CYSCREEN);
		if (px < 0 || py < 0 || px >= screenW || py >= screenH) {
			px = (screenW - m_config.preview_w) / 2 + 300;
			py = (screenH - m_config.preview_h) / 2;
		}
		RECT rc = {0, 0, m_config.preview_w, m_config.preview_h};
		AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, FALSE);
		m_previewHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, previewClassName.c_str(), previewTitle.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, px, py, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, m_hInstance, this);
	if (!m_previewHwnd) return false;
	SetWindowLongPtrW(m_previewHwnd, GWLP_USERDATA, (LONG_PTR)this);
	ShowWindow(m_previewHwnd, SW_SHOWDEFAULT);
	UpdateWindow(m_previewHwnd);
	return true;
}
	void App::destroyPreviewWindow() {
		if (m_previewHwnd) {
			savePreviewPos();
			DestroyWindow(m_previewHwnd);
			m_previewHwnd = nullptr;
			UnregisterClassW(previewClassName.c_str(), m_hInstance);
		}
	}
void App::savePreviewPos() {
	if (!m_previewHwnd) return;
	RECT rc;
	GetWindowRect(m_previewHwnd, &rc);
	int screenW = GetSystemMetrics(SM_CXSCREEN);
	int screenH = GetSystemMetrics(SM_CYSCREEN);
	if (rc.left >= 0 && rc.left < screenW && rc.top >= 0 && rc.top < screenH) {
		m_config.preview_x = rc.left;
		m_config.preview_y = rc.top;
	}
	RECT clientRc;
	GetClientRect(m_previewHwnd, &clientRc);
	m_config.preview_w = clientRc.right;
	m_config.preview_h = clientRc.bottom;
}
void App::updatePreviewWindow() {
	if (!m_previewHwnd || !IsWindowVisible(m_previewHwnd)) return;
	if (!m_previewReady.load() || m_previewW <= 0 || m_previewH <= 0) return;
	m_previewReady.store(false);
	InvalidateRect(m_previewHwnd, nullptr, FALSE);
}
bool App::createDeviceD3D() {
	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = 0;
	sd.BufferDesc.Height = 0;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = m_hwnd;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	UINT flags = 0;
	D3D_FEATURE_LEVEL featureLevel;
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
	// 优先选择NVIDIA适配器，确保D3D11设备与CUDA在同一GPU上
	IDXGIAdapter* nvidiaAdapter = nullptr;
	IDXGIFactory* factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory))) {
		IDXGIAdapter* adapter = nullptr;
		for (UINT i = 0; SUCCEEDED(factory->EnumAdapters(i, &adapter)); ++i) {
			DXGI_ADAPTER_DESC desc;
			adapter->GetDesc(&desc);
			if (desc.VendorId == 0x10DE) {
				nvidiaAdapter = adapter;
				break;
			}
			adapter->Release();
		}
		factory->Release();
	}
	HRESULT hr = D3D11CreateDeviceAndSwapChain(nvidiaAdapter, nvidiaAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1, D3D11_SDK_VERSION, &sd, &m_swapChain, &m_d3dDevice, &featureLevel, &m_d3dContext);
	if (nvidiaAdapter) nvidiaAdapter->Release();
	if (FAILED(hr)) return false;
	return true;
}
void App::createRenderTarget() {
	ID3D11Texture2D* backBuffer = nullptr;
	m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
	m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &m_renderTarget);
	backBuffer->Release();
}
void App::cleanupRenderTarget() {
	if (m_renderTarget) { m_renderTarget->Release(); m_renderTarget = nullptr; }
}
void App::setupImGui() {
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui_ImplWin32_Init(m_hwnd);
	ImGui_ImplDX11_Init(m_d3dDevice, m_d3dContext);
	Theme::Apply();
	ImFontConfig fontCfg;
	fontCfg.OversampleH = 2;
	fontCfg.OversampleV = 1;
	io.Fonts->AddFontFromFileTTF(OBS1("C:\\Windows\\Fonts\\msyh.ttc").c_str(), 16.0f, &fontCfg, io.Fonts->GetGlyphRangesChineseFull());
}
void App::enumGPUs() {
	m_gpuNames.clear();
	IDXGIFactory* factory = nullptr;
	CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
	if (!factory) return;
	for (UINT i = 0; ; ++i) {
		IDXGIAdapter* adapter = nullptr;
		if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);
		char name[128];
		wcstombs(name, desc.Description, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		char label[160];
		snprintf(label, sizeof(label), "%d: %s", i, name);
		m_gpuNames.push_back(label);
		adapter->Release();
	}
	factory->Release();
	if (m_gpuNames.empty()) m_gpuNames.push_back("0: Default GPU");
}
void App::switchDriver(DriverMode mode) {
	m_mouseTestRunning.store(false);
	Sleep(50);
	m_config.driver_mode = mode;
	MouseDrvType preferred = MouseDrvType::MDT_None;
	if (mode == DriverMode::Logitech) preferred = MouseDrvType::MDT_GHUB;
	else if (mode == DriverMode::Generic) preferred = MouseDrvType::MDT_SendInput;
	else if (mode == DriverMode::Makcu) preferred = MouseDrvType::MDT_Makcu;
	m_driver.initialize(preferred, m_config.makcu_port, m_config.makcu_baud);
	LOG_INFO(OBS3(u8"鼠标驱动: %s").c_str(), m_driver.getMethodName().c_str());
}
void App::updateClassList() {
	bool loaded = (m_config.infer_mode == InferMode::DirectML) ? m_dmlEngine.isLoaded() : m_engine.isLoaded();
	if (!loaded) return;
	m_modelClasses = extractOnnxClassNames(m_config.model_path);
	if (!m_modelClasses.empty()) {
		m_modelNumClasses = (int)m_modelClasses.size();
	} else {
		auto dims = (m_config.infer_mode == InferMode::DirectML) ? m_dmlEngine.getOutputDims() : m_engine.getOutputDims();
		m_modelNumClasses = YOLOPostProcess::getNumClasses(dims, m_config.yolo_version);
	}
	m_classEnabled.clear();
	m_classEnabled.resize(m_modelNumClasses, 1);
	auto it = m_modelClassMemory.find(m_config.model_path);
	if (it != m_modelClassMemory.end()) {
		for (int disabledId : it->second) {
			if (disabledId >= 0 && disabledId < m_modelNumClasses) {
				m_classEnabled[disabledId] = 0;
			}
		}
		m_config.disabled_classes = it->second;
	} else {
		for (int disabledId : m_config.disabled_classes) {
			if (disabledId >= 0 && disabledId < m_modelNumClasses) {
				m_classEnabled[disabledId] = 0;
			}
		}
	}
	// 按类别独立瞄准部位：以 classId 对齐，保留 ini 中已加载的配置
	std::vector<ClassAimPos> capById((size_t)m_modelNumClasses);
	for (const auto& cap : m_config.class_aim_positions) {
		if (cap.classId >= 0 && cap.classId < m_modelNumClasses) capById[cap.classId] = cap;
	}
	for (int i = 0; i < m_modelNumClasses; ++i) capById[i].classId = i;
	m_config.class_aim_positions = std::move(capById);
	syncDisabledClasses();
}
std::vector<int> App::getSelectedClasses() const {
	std::vector<int> sel;
	for (int i = 0; i < (int)m_classEnabled.size(); ++i) {
		if (m_classEnabled[i]) sel.push_back(i);
	}
	if (sel.empty()) {
		for (int i = 0; i < m_modelNumClasses; ++i) {
			sel.push_back(i);
		}
	}
	return sel;
}
void App::syncDisabledClasses() {
	m_config.disabled_classes.clear();
	for (int i = 0; i < (int)m_classEnabled.size(); ++i) {
		// 类别筛选勾选=启用该类别独立瞄准部位，取消=不启用(跟随全局)
		if (i < (int)m_config.class_aim_positions.size()) {
			m_config.class_aim_positions[i].enabled = m_classEnabled[i] != 0;
		}
		if (!m_classEnabled[i]) {
			m_config.disabled_classes.push_back(i);
		}
	}
	if (!m_config.model_path.empty()) {
		m_modelClassMemory[m_config.model_path] = m_config.disabled_classes;
	}
}
void App::resetInference() {
	stopHeartbeat();
	m_modelLoaded.store(false);
	m_captureInited.store(false);
	m_config.aim_enabled = false;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	for (int i = 0; i < 2; ++i) {
		m_frameSlots[i].ready.store(false);
		if (m_frameSlots[i].cudaInterop && m_frameSlots[i].cudaRes) {
			cudaGraphicsUnmapResources(1, &m_frameSlots[i].cudaRes, m_cudaStream);
			m_frameSlots[i].cudaRes = nullptr;
		}
		if (m_frameSlots[i].gpuBuf) {
			cudaFree(m_frameSlots[i].gpuBuf);
			m_frameSlots[i].gpuBuf = nullptr;
		}
	}
	m_capture.release();
	m_engine.release();
	m_dmlEngine.release();
	m_aim.reset();
}
void App::startModelLoad() {
	if (m_modelLoading.load()) return;
	// 等待上一个加载线程完成
	if (m_loadThread.joinable()) m_loadThread.join();
	m_modelLoaded.store(false);
	m_captureInited.store(false);
	m_config.aim_enabled = false;
	m_aim.reset();
	LOG_INFO(OBS4(u8"开始加载模型: %s").c_str(), m_config.model_path.c_str());
	resetInference();
	syncDisabledClasses();
	m_modelNumClasses = 0;
	m_modelClasses.clear();
	m_classEnabled.clear();
	m_config.disabled_classes.clear();
	m_modelLoading.store(true);
	m_loadCancel.store(false);
	m_loadSuccess.store(false);
	m_loadProgress.store(0);
	{
		std::lock_guard<std::mutex> lock(m_loadMutex);
		m_loadStatus = OBS5(u8"准备加载...");
	}
	m_config.model_path = m_modelPathBuf;
	m_loadThread = std::thread([this]() {
		std::string path = m_config.model_path;
		int gpuId = m_config.infer_gpu_id;  // [v218] 推理引擎用独立卡配置(0=与捕获同卡)
		bool ok = false;
		// [v218] 推理卡有效性校验: 越界回退捕获卡
		{
			int devCount = 0;
			if (cudaGetDeviceCount(&devCount) == cudaSuccess && (gpuId < 0 || gpuId >= devCount)) {
				LOG_WARN(OBS2(u8"推理GPU %d 无效(共%d块), 回退到捕获卡").c_str(), gpuId, devCount);
				gpuId = m_config.gpu_id;
				m_config.infer_gpu_id = m_config.gpu_id;
			}
		}
		// CUDA可用性预检测
		int cudaDevCount = 0;
		cudaError_t cudaErr = cudaGetDeviceCount(&cudaDevCount);
		if (cudaErr != cudaSuccess || cudaDevCount <= 0) {
			LOG_ERROR(OBS1(u8"CUDA不可用: %s").c_str(), cudaErr == cudaSuccess ? u8"未检测到NVIDIA GPU" : cudaGetErrorString(cudaErr));
			LOG_ERROR(OBS2(u8"请确认: 1.已安装NVIDIA显卡驱动(最新版) 2.电脑有NVIDIA GPU 3.驱动未被其他程序占用").c_str());
			std::lock_guard<std::mutex> lock(m_loadMutex);
			m_loadStatus = u8"CUDA不可用，请安装NVIDIA驱动";
			m_loadProgress.store(0);
			m_loadSuccess.store(false);
			m_modelLoading.store(false);
			return;
		}
		cudaDeviceProp prop = {};
		cudaGetDeviceProperties(&prop, gpuId < cudaDevCount ? gpuId : 0);
		int driverVer = 0, runtimeVer = 0;
		cudaDriverGetVersion(&driverVer);
		cudaRuntimeGetVersion(&runtimeVer);
		static std::string sDML = OBS1("DirectML"), sTRT = OBS2("TensorRT");
			LOG_INFO(OBS6(u8"加载推理引擎: %s | GPU: %d | %s | 计算能力: %d.%d | 驱动CUDA: %d.%d | 运行时CUDA: %d.%d").c_str(),
				m_config.infer_mode == InferMode::DirectML ? sDML.c_str() : sTRT.c_str(),
			gpuId, prop.name, prop.major, prop.minor,
			driverVer / 1000, (driverVer % 1000) / 10,
			runtimeVer / 1000, (runtimeVer % 1000) / 10);
		if (driverVer < runtimeVer) {
			LOG_ERROR(OBS3(u8"NVIDIA驱动过旧! 需要支持CUDA %d.%d (当前驱动仅支持CUDA %d.%d)").c_str(),
				runtimeVer / 1000, (runtimeVer % 1000) / 10,
				driverVer / 1000, (driverVer % 1000) / 10);
			LOG_ERROR(OBS4(u8"请更新NVIDIA显卡驱动到最新版本(https://www.nvidia.com/Download/index.aspx)").c_str());
			std::lock_guard<std::mutex> lock(m_loadMutex);
			m_loadStatus = u8"NVIDIA驱动过旧，请更新驱动";
			m_loadProgress.store(0);
			m_loadSuccess.store(false);
			m_modelLoading.store(false);
			return;
		}
		if (prop.major < 6) {
			LOG_ERROR(OBS3(u8"GPU计算能力过低(需>=6.0): %s = %d.%d").c_str(), prop.name, prop.major, prop.minor);
			std::lock_guard<std::mutex> lock(m_loadMutex);
			m_loadStatus = u8"GPU计算能力不足";
			m_loadProgress.store(0);
			m_loadSuccess.store(false);
			m_modelLoading.store(false);
			return;
		}
		if (m_config.infer_mode == InferMode::DirectML) {
			ok = m_dmlEngine.loadModel(path, gpuId,
				[this](int progress, const char* status) {
					m_loadProgress.store(progress);
					std::lock_guard<std::mutex> lock(m_loadMutex);
					m_loadStatus = status ? status : "";
				},
				&m_loadCancel
			);
		} else {
			ok = m_engine.loadModel(path, gpuId,
				[this](int progress, const char* status) {
					m_loadProgress.store(progress);
					std::lock_guard<std::mutex> lock(m_loadMutex);
					m_loadStatus = status ? status : "";
				},
				&m_loadCancel,
				m_config.trt_precision,
				m_config.trt_mixed_precision
			);
		}
		m_loadSuccess.store(ok);
		m_modelLoading.store(false);
		if (ok) {
			m_modelLoaded.store(true);
			auto inputDims = (m_config.infer_mode == InferMode::DirectML) ? m_dmlEngine.getInputDims() : m_engine.getInputDims();
			if (inputDims.size() >= 3) {
				int h = (int)inputDims[inputDims.size() - 2];
				int w = (int)inputDims[inputDims.size() - 1];
				if (w > 0 && h > 0) {
					m_config.model_width = w;
					m_config.model_height = h;
				}
			}
			auto outputDims = (m_config.infer_mode == InferMode::DirectML) ? m_dmlEngine.getOutputDims() : m_engine.getOutputDims();
			if (YOLOPostProcess::isEndToEnd(outputDims)) {
				if (m_config.yolo_version != YoloVersion::V10 && m_config.yolo_version != YoloVersion::V26) {
					m_config.yolo_version = YoloVersion::V26;
					m_detectedVersionName = OBS7(u8"YOLOv26 (输出推断)");
					m_versionAutoDetected = true;
				}
			} else if (outputDims.size() >= 3) {
				int d1 = (int)outputDims[1], d2 = (int)outputDims[2];
				if (d1 > d2) std::swap(d1, d2);
				bool isAnchorBased = (d1 == 3);
				if (isAnchorBased && m_config.yolo_version != YoloVersion::V5 && m_config.yolo_version != YoloVersion::V6 && m_config.yolo_version != YoloVersion::V7) {
					m_config.yolo_version = YoloVersion::V5;
					m_detectedVersionName = OBS8(u8"YOLOv5 (输出推断)");
					m_versionAutoDetected = true;
				}
			}
			updateClassList();
			LOG_INFO(OBS1(u8"模型加载成功 (%s %dx%d)").c_str(), m_config.infer_mode == InferMode::DirectML ? "DirectML" : "TensorRT", m_config.model_width, m_config.model_height);
			m_capture.release();
			if (m_capture.initialize(m_config.model_width, m_config.model_height, m_config.gpu_id, (CaptureMode)m_config.capture_mode)) {
				m_captureInited.store(true);
				m_config.capture_mode = (int)m_capture.getCaptureMode();
				m_config.aim_enabled = true;
				// 注册CUDA互操作（TRT模式）
				if (m_config.infer_mode == InferMode::TensorRT) {
					if (m_capture.cudaRegisterSharedTexture()) {
						LOG_INFO(OBS2(u8"CUDA-D3D11互操作已启用，屏幕捕获零CPU拷贝").c_str());
					} else {
						LOG_INFO(OBS3(u8"CUDA互操作不可用，使用Host->Device回退路径").c_str());
					}
					// 启用CUDA Graph优化
					m_engine.enableCudaGraph();
				}
				// GPU调度心跳: 游戏独占GPU时制造调度间隙(直播助手可见时同效)
				startHeartbeat();
			} else {
					// [v183] 启动失败自动回退WGC: DXGI/Hook是排他接口, 被NVIDIA App即时回放/
					// 远程工具占用时启动必失败→捕获死态。回退WGC保证可用, 具体错误码见上方日志
					// [v185] 回退只影响本次运行, 不改写capture_mode: 用户配置保留DXGI,
					// 界面黄字显示实际模式, 重启后自动重试DXGI
					LOG_ERROR(OBS4(u8"屏幕捕获初始化失败, 本次运行自动回退WGC图形捕获(配置保留, 重启将重试)").c_str());
					m_capture.release();
					if (m_capture.initialize(m_config.model_width, m_config.model_height, m_config.gpu_id, CaptureMode::WGC)) {
						m_captureInited.store(true);
						m_config.aim_enabled = true;
					if (m_config.infer_mode == InferMode::TensorRT) {
						if (m_capture.cudaRegisterSharedTexture()) {
							LOG_INFO(OBS2(u8"CUDA-D3D11互操作已启用，屏幕捕获零CPU拷贝").c_str());
						} else {
							LOG_INFO(OBS3(u8"CUDA互操作不可用，使用Host->Device回退路径").c_str());
						}
						m_engine.enableCudaGraph();
					}
					startHeartbeat();
				} else {
					LOG_ERROR(OBS4(u8"WGC回退也失败，捕获不可用，请检查显卡驱动与显示配置").c_str());
				}
			}
		} else {
			LOG_ERROR(OBS5(u8"模型加载失败").c_str());
		}
	});
}
void App::cancelModelLoad() {
	m_loadCancel.store(true);
}
// ── GPU调度心跳 ──
// 现象依据: 直播助手窗口可见时, 其持续的渲染/编码提交把GPU命令队列切成小段,
// 我们的推理kernel能频繁插进间隙 → 延迟低; 助手最小化后游戏独占队列提交
// 整帧长命令流 → 推理只能等帧边界 → 延迟升高。
// 方案: 专用低优先级流每2ms提交一次4字节微型kernel —— 显式复制"助手可见"
// 的调度效果, 迫使WDDM持续制造调度间隙。开销: 微秒级/次, 可忽略。
void App::startHeartbeat() {
	if (m_heartbeatRunning.load()) return;
	m_heartbeatRunning.store(true);
	m_heartbeatThread = std::thread([this]() {
		// [v218] 心跳服务对象=推理卡(制造WDDM调度间隙让推理kernel插入)
		int hbGpu = (m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id) ? m_config.infer_gpu_id : m_config.gpu_id;
		cudaSetDevice(hbGpu);
		int leastPri = 0, greatestPri = 0;
		cudaDeviceGetStreamPriorityRange(&leastPri, &greatestPri);
		// 低优先级流: 心跳永远不与推理kernel竞争, 只负责制造调度事件
		cudaStreamCreateWithPriority(&m_heartbeatStream, cudaStreamNonBlocking, leastPri);
		if (cudaMalloc(&m_heartbeatBuf, 4) != cudaSuccess) m_heartbeatBuf = nullptr;
		LOG_INFO(OBS1(u8"GPU调度心跳已启动(2ms周期, 低优先级)").c_str());
		while (m_heartbeatRunning.load()) {
			if (m_heartbeatBuf) cudaMemsetAsync(m_heartbeatBuf, 0, 4, m_heartbeatStream);
			Sleep(2);
		}
		if (m_heartbeatBuf) { cudaFree(m_heartbeatBuf); m_heartbeatBuf = nullptr; }
		if (m_heartbeatStream) { cudaStreamDestroy(m_heartbeatStream); m_heartbeatStream = nullptr; }
	});
}
void App::stopHeartbeat() {
	if (!m_heartbeatRunning.load()) return;
	m_heartbeatRunning.store(false);
	if (m_heartbeatThread.joinable()) m_heartbeatThread.join();
}
void App::run() {
	m_running.store(true);
	m_inferRunning.store(true);
	m_captureRunning.store(true);
	m_captureThread = std::thread(&App::captureLoop, this);
	m_inferThread = std::thread(&App::inferLoop, this);
	// 线程级优先级：捕获和推理线程设为High，保证及时性但不抢占整个系统
	SetThreadPriority(m_captureThread.native_handle(), THREAD_PRIORITY_HIGHEST);
	SetThreadPriority(m_inferThread.native_handle(), THREAD_PRIORITY_HIGHEST);
	m_lastTime = std::chrono::steady_clock::now();
	m_fpsLastTime = m_lastTime;
	MSG msg = {};
	while (m_running.load()) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
			if (msg.message == WM_QUIT) { m_running.store(false); }
		}
		if (!m_running.load()) break;
		auto frameNow = std::chrono::steady_clock::now();
		float frameDt = (float)std::chrono::duration<double>(frameNow - m_lastTime).count();
		m_lastTime = frameNow;
		if (frameDt > 0.1f) frameDt = 0.016f;
		// (旧: 隐藏时强制frameDt=16ms——那是给已删除的dt依赖控制留的补丁,
		//  现为纯P控制+真实计时,隐藏模式Sleep(8)下用真实dt更准确)
			// 自瞄总开关热键检测
			if (m_config.aim_toggle_enabled && m_modelLoaded.load() && m_captureInited.load() && m_config.aim_enabled) {
					bool togglePressed = (GetAsyncKeyState(m_config.aim_toggle_key) & 0x8000) != 0;
					if (m_config.aim_toggle_key_mode == KeyMode::Combo)
						togglePressed = togglePressed && (GetAsyncKeyState(m_config.aim_toggle_combo_key) & 0x8000) != 0;
				if (togglePressed && !m_toggleKeyWasPressed) {
					m_aimToggled.store(!m_aimToggled.load());
					bool nowOn = m_aimToggled.load();
					if (nowOn) {
						LOG_INFO(OBS6(u8"自瞄总开关: 已开启").c_str());
						if (m_config.toggle_sound) PlaySoundW((LPCWSTR)SND_ALIAS_SYSTEMEXIT, nullptr, SND_ALIAS_ID | SND_ASYNC);  // 开启: 上扬提示音
					} else {
						LOG_INFO(OBS7(u8"自瞄总开关: 已关闭").c_str());
						if (m_config.toggle_sound) PlaySoundW((LPCWSTR)SND_ALIAS_SYSTEMHAND, nullptr, SND_ALIAS_ID | SND_ASYNC);  // 关闭: 低沉提示音
					}
				}
				m_toggleKeyWasPressed = togglePressed;
				} else {
					m_aimToggled.store(false);
					m_toggleKeyWasPressed = false;
				}
				// ── 主窗口显隐切换（VK_HOME）──
				// 全屏游戏下按 Home 触发假最小化: 窗口缩到右下角小条保持置顶,
				// 再按 Home 或点小条恢复原位。隐藏时瞄准逻辑不受影响。
				bool homePressed = (GetAsyncKeyState(m_config.ui_toggle_key) & 0x8000) != 0;
				if (homePressed && !m_homeKeyWasPressed) {
					if (m_fakeMinimized) restoreFromFakeMinimize(); else fakeMinimize();
					LOG_INFO(OBS1(u8"主窗口: %s (Home键)").c_str(), m_fakeMinimized ? OBS2(u8"已缩到右下角") : OBS3(u8"已恢复"));
				}
				m_homeKeyWasPressed = homePressed;
				bool aimKeyPressed = checkAimKey();
			// 总开关逻辑：启用总开关时，需要总开关开启+瞄准键才激活
			bool aimAllowed = m_config.aim_toggle_enabled ? m_aimToggled.load() : true;
			bool wasActive = m_aimActive.load();
			// [v190] 热键去抖: 实战弹跳dump实证aimActive以25Hz方波闪烁(罗技GHUB宏/驱动
			// 拦截热键报告), 每次闪断把prevMove清零→恢复帧全速直发=脱离吸附+过冲+摇晃主源。
			// 连续3帧(≈40ms)无键才真正失活; 40ms内的瞬时丢失被完全滤除, 真实松键停手
			// 延迟≤40ms低于人手感知阈值。根本解决仍需检查GHUB热键是否绑定了宏
			bool rawActive = aimKeyPressed && aimAllowed && m_modelLoaded.load() && m_captureInited.load() && m_config.aim_enabled;
			if (rawActive) {
				m_keyDebounce = 0;
				m_aimActive.store(true);
			} else if (m_keyDebounce < 3) {
				m_keyDebounce++;
				// 保持当前激活状态不变, 滤除瞬时丢失
			} else {
				m_aimActive.store(false);
			}
			if (!wasActive && m_aimActive.load()) {
				LOG_INFO(OBS6(u8"瞄准热键激活").c_str());
			}
			if (wasActive && !m_aimActive.load()) LOG_INFO(OBS7(u8"瞄准热键释放").c_str());
		updateFPS();
		// 从推理线程读取最新结果
		bool hasNewResult = false;
		if (m_resultReady.load()) {
			std::lock_guard<std::mutex> lock(m_detectMutex);
			m_lastDetections = std::move(m_resultDetections);
			m_resultDetections.clear();
			m_detectionCount.store((int)m_lastDetections.size());
			m_resultReady.store(false);
			hasNewResult = true;
			m_inferSkipCount = 0;
			// [v157] 过期结果丢弃: 大厅/选人/加载动画时GPU被游戏抢占, 推理尖峰实测1.7s,
			// 过期画面的检测位置早已失效, 喂给aim会朝旧位置移动=跳目标。帧龄超阈值直接丢弃,
			// 尖峰期间自瞄停手(锁保持), 尖峰过后自动恢复
			// [v229] 120→250ms: 尖峰期饿死输出=锁上但拉不动(体感延迟激增)。
			// v228在途记账+929插值使旧帧安全(误差闭环自动收敛), 宁可慢速修正也不完全断流
			long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
			if (m_resultCapMs.load() > 0 && nowMs - m_resultCapMs.load() > 250) {
				hasNewResult = false;
			}
		} else {
			m_inferSkipCount++;
		}
		// 推理延迟诊断日志(每30秒输出一次)
		if (m_inferDiagCooldown.load() > 0) m_inferDiagCooldown.store(m_inferDiagCooldown.load() - 1);
		// [v180] 统计帧数>300(~2.5s)才输出: 启动首帧有冷启动尖峰(CUDA Graph预热+内核首次
		// 加载, 可达80ms+), 旧条件>TotalFrames>0会让首帧产出"平均=最小=最大=81.1"的单帧
		// 误导日志, 被误判为持续高延迟。300帧后输出的才是稳态数据
		if (m_inferDiagCooldown.load() <= 0 && m_inferTotalFrames.load() > 300) {
			m_inferDiagCooldown.store(1800);
			float avgInferMs = m_inferMsSum.load() / m_inferTotalFrames.load();
			// [v194] 未分类段 = 总耗时-全部已计时分段: 揭示盲区(H2D拷贝/锁等待/驱动调用),
			// 未分类>5ms说明存在未插桩的阻塞, 与"最大"联动定位730ms级尖峰藏身处
			float uncls = m_inferMs.load() - m_inferWaitMs.load() - m_gpuInferMs.load() - m_postprocMs.load();
			if (uncls < 0.0f) uncls = 0.0f;
			LOG_INFO(OBS8(u8"[延迟诊断] 推理:%.1fms(等待:%.1f GPU:%.1f=预处理%.1f+TRT%.1f 后处理:%.1f 未分类:%.1f) 捕获:%.1fms 跳帧:%d 平均:%.1fms 最小:%.1fms 最大:%.1fms FPS:%.0f").c_str(),
				m_inferMs.load(), m_inferWaitMs.load(), m_gpuInferMs.load(), m_preGpuMs.load(), m_gpuInferMs.load() - m_preGpuMs.load(), m_postprocMs.load(), uncls, m_captureMs.load(), m_inferSkipCount,
				avgInferMs, m_inferMsMin.load(), m_inferMsMax.load(), m_fps);
			m_inferMsMin.store(999.0f);
			m_inferMsMax.store(0.0f);
			m_inferMsSum.store(0.0f);
			m_inferTotalFrames.store(0);
		}
		// aim每帧更新，用frameDt驱动
		if (m_modelLoaded.load() && m_captureInited.load() && m_config.aim_enabled) {
				// 动态传递端到端延迟给aim（延迟EMA平滑）
				// 延迟EMA平滑后传给aim：单帧推理/捕获耗时抖动直接喂给aim
				// 会让匹配参考帧间跳变→匹配门限抖→闪断。EMA(α=0.1)后稳定
				{
					static float s_latEma = 0.0f;
					float latNow = m_inferMs.load() + m_captureMs.load() + 8.0f;
					s_latEma = (s_latEma > 0.5f) ? (s_latEma * 0.90f + latNow * 0.10f) : latNow;
					m_aim.setLatencyMs(s_latEma);
				}
				bool shooting = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
			// 自动扳机虚拟射击时不启用后坐力补偿模式，保持正常自瞄移动速度
			if (m_atLeftHeld) shooting = false;
				bool realAimActive = m_aimActive.load();
				// 对狙启用时始终追踪目标（即使未按自瞄键），以便触发键可用
				// 但必须总开关开启才允许对狙
				bool qsAllowed = aimAllowed && m_config.quickscope_enabled && m_config.quickscope_key.vkCode > 0;
				// 自动扳机：必须按住自瞄键且准心吸附到目标才能触发
				bool atKeyActive = aimAllowed && m_config.auto_trigger_enabled && m_config.auto_trigger_key.vkCode > 0 &&
					realAimActive && // 必须按住自瞄键
					(GetAsyncKeyState(m_config.auto_trigger_key.vkCode) & 0x8000) != 0;
				bool trackActive = realAimActive || qsAllowed;
				m_aim.update(m_lastDetections, m_resultAimCX, m_resultAimCY, m_resultFrameW, m_resultFrameH, m_config, trackActive, hasNewResult, m_resultRecoilX, m_resultRecoilY, shooting);
				// 对狙触发检测（独立于自瞄键，但需要总开关开启）
				bool quickscopeFired = false;
				if (qsAllowed) {
					bool triggerKey = (GetAsyncKeyState(m_config.quickscope_key.vkCode) & 0x8000) != 0;
					if (m_aim.checkQuickscopeTrigger(triggerKey) && m_aim.hasTarget()) {
						// 对狙固定瞄头，不受Y轴吸附参数影响
						float snapX, snapY;
						m_aim.getHeadSnapPos(snapX, snapY, m_config.aim_position);
						m_driver.moveRelative((int)std::round(snapX), (int)std::round(snapY));
						// 保留剩余误差让自瞄修正（不清零lockedX/Y）
						m_aim.resetAfterQuickscopeSnap(snapX, snapY);
						// 吸附后开始开枪倒计时
						m_aim.startQuickscopeFireDelay(m_config.quickscope_fire_delay);
						quickscopeFired = true;
					}
					// 开枪倒计时：到0时自动开枪
					m_aim.tickQuickscopeDelay();
					if (m_aim.isQuickscopeFireReady()) {
						m_driver.clickLeft();
						m_aim.clearQuickscopeFire();
					}
				}
				// 常规自瞄移动（自瞄键按住时移动，补偿后坐力）
				// ── [v229] 连续输出: 每主循环帧调用getMoveDelta ──
				// 旧版(929门控hasNewResult): 只在检测帧发整步 → 检测16~29fps时输出呈脉冲
				// 步进, 帧间34~62ms零输出=体感"迟滞/卡顿"。
				// 新逻辑: 检测帧把整步折算为速率+预算(aim内), 之后每主循环帧(~240fps)
				// 发"速率×实际时间片", 预算守恒→总发送量仍与检测帧1:1(防超发), 但分布连续。
				// 主循环不持有移动状态, 全部由AimAssist内部状态机决策
				if (realAimActive && !quickscopeFired && m_aim.hasTarget()) {
					auto delta = m_aim.getMoveDelta(frameDt);
					if (delta.screenX != 0 || delta.screenY != 0) {
						m_driver.moveRelative(delta.screenX, delta.screenY);
						m_aim.noteSent(delta.screenX, delta.screenY);
					}
				}
				// 自动扳机（与对狙互斥）
				// 持续按住模式：首次leftDown + 定期重发leftDownHold维持
				// 每3帧(~48ms)重发一次，避免每帧重发导致事件积压鬼畜
				{
					if (atKeyActive && m_aim.hasTarget()) {
						m_atFireActive = true;
					}
					if (!atKeyActive) {
						m_atFireActive = false;
					}
					if (m_atFireActive) {
						if (!m_atLeftHeld) {
							m_driver.leftDown();
							m_atLeftHeld = true;
							m_atFireTick = 0;
						} else {
							m_atFireTick++;
							if (m_atFireTick >= 3) {
								m_driver.leftDownHold();
								m_atFireTick = 0;
							}
						}
					} else {
						if (m_atLeftHeld) {
							m_driver.leftUp();
							m_atLeftHeld = false;
						}
						m_atFireTick = 0;
					}
				}
			} else {
				m_aim.updateFovOnly(m_config);
				m_atFireActive = false;
				if (m_atLeftHeld) {
					m_driver.leftUp();
					m_atLeftHeld = false;
				}
			}
		// 窗口隐藏时跳过渲染，按~120Hz绝对期限节拍(与检测节奏对齐)
		// 旧Sleep(16)=62.5Hz：检测2:1被丢+反馈陈旧+tracker帧间隔翻倍→甩枪跳目标/过冲(已实测确认)
		// 加固(2026-08-23)：不再依赖定时器精度——Win11会忽略"无可视窗口进程"的
		// NtSetTimerResolution请求，纯Sleep(8)可能静默退化成15.6ms≈64Hz复发。
		// 改用高精度等待计时器(硬件时钟唤醒,1803+)；创建失败的老系统退化到"睡+自旋"。
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
		if (m_hidden) {  // ← 修复:加固时误删此守卫,continue无条件执行导致界面全白(2026-08-23)
			using clk = std::chrono::steady_clock;
			static clk::time_point s_hiddenNext{};
			static bool s_hiddenInited = false;
			static HANDLE s_hrTimer = CreateWaitableTimerExW(nullptr, nullptr,
				CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE);  // 同步计时器(自动复位)
			const auto frameTime = std::chrono::microseconds(8333);  // 120Hz
			auto nowT = clk::now();
			if (!s_hiddenInited) { s_hiddenNext = nowT + frameTime; s_hiddenInited = true; }
			else s_hiddenNext += frameTime;  // 绝对期限累加:长期速率锁定,偶发卡顿自动追赶
			if (s_hiddenNext > nowT) {
				auto us = std::chrono::duration_cast<std::chrono::microseconds>(s_hiddenNext - nowT).count();
				if (s_hrTimer) {
					// 高精度等待:100ns单位负值=相对期限,唤醒精度~亚毫秒级
					LARGE_INTEGER due; due.QuadPart = -10LL * us;
					if (SetWaitableTimer(s_hrTimer, &due, 0, nullptr, nullptr, FALSE)) {
						WaitForSingleObject(s_hrTimer, 20);  // 20ms兜底防漏唤醒
					} else {
						Sleep(8);  // Set失败兜底
					}
				} else {
					// 老系统退化路径:睡掉大部分+末段Sleep(0)自旋补精度
					if (us > 1500) std::this_thread::sleep_for(std::chrono::microseconds(us - 1500));
					while (clk::now() < s_hiddenNext) Sleep(0);
				}
			} else {
				s_hiddenNext = nowT;  // 已超期(卡顿追赶):立即放行并重置基准
			}
			continue;
		}
		if (m_config.show_preview && m_previewHwnd) {
			updatePreviewWindow();
		}
		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
			antidebug::runPeriodicCheck();
			renderUI();
		ImGui::Render();
		const float clear_color[] = { 0.06f, 0.07f, 0.10f, 1.00f };
		m_d3dContext->OMSetRenderTargets(1, &m_renderTarget, nullptr);
		m_d3dContext->ClearRenderTargetView(m_renderTarget, clear_color);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		// [v193] 关闭垂直同步+240fps期限节拍: 原Present(1,0)把主循环锁在显示器刷新率
		// (60/75Hz), 而检测结果的消费在主循环里→检测到达率上限=刷新率(实测42-58)。
		// 关vsync后主循环解锁, 用隐藏模式同款绝对期限节拍(240fps)防CPU空转,
		// 检测消费率提升至捕获上限(120), 吸附修正更连贯
		HRESULT presentHr = m_swapChain->Present(0, 0);
		// 窗口被游戏全屏遮挡时Present立即返回(不再垂直同步) → 主循环失去节拍
		// 空转数十万次/秒(实测195k), 白烧CPU还放大重复发移动的bug → 遮蔽时主动小睡
		if (presentHr == DXGI_STATUS_OCCLUDED) Sleep(4);
		else {
			// [v194] 可见模式240fps硬节拍: v193的高精度计时器分支实测未钳住(跑出957~3746fps),
			// 改为sleep_until绝对期限硬限(timeBeginPeriod(1)已提升sleep精度至~1ms),
			// 检测消费余量仍远超捕获120fps供给, 消除主循环空转烧核
			using clk = std::chrono::steady_clock;
			static clk::time_point s_visNext{};
			static bool s_visInited = false;
			const auto visFrameTime = std::chrono::microseconds(4166);  // 240fps
			auto nowV = clk::now();
			if (!s_visInited) { s_visNext = nowV + visFrameTime; s_visInited = true; }
			else s_visNext += visFrameTime;
			if (s_visNext > nowV) {
				std::this_thread::sleep_until(s_visNext);
			} else {
				s_visNext = nowV;  // 卡顿追赶
			}
		}
	}
	m_captureRunning.store(false);
	m_inferRunning.store(false);
	if (m_captureThread.joinable()) m_captureThread.join();
	if (m_inferThread.joinable()) m_inferThread.join();
}
void App::captureLoop() {
	// 专用H2D拷贝流: 与引擎推理流分离 → 下一帧的Host→Device拷贝可与当前帧
	// 推理并行(原来串在引擎流上等推理+D2H全部完成); 换模型期间也不会用空流(默认流隐式同步)
	if (!m_cudaStream) {
		cudaSetDevice(m_config.gpu_id);
		cudaStreamCreateWithFlags(&m_cudaStream, cudaStreamNonBlocking);
	}
	int frameBufSize = 0;
	int capAntiDbgCounter = 0;
	// 捕获线程帧率限制：不超过推理帧率的1.5倍，减少GPU命令提交量
	auto capFrameLimitNext = std::chrono::steady_clock::now();
	bool capFrameLimitInited = false;
	while (m_captureRunning.load() && m_running.load()) {
		if (++capAntiDbgCounter >= 500) {
			capAntiDbgCounter = 0;
			antidebug::runPeriodicCheck();
		}
		if (!m_modelLoaded.load() || !m_captureInited.load() || !m_config.aim_enabled) {
			Sleep(1);
			continue;
		}
			// 捕获帧率固定120: 与推理硬拍一致, 不跟随自适应拍(拍频打架导致帧槽跳帧58+延迟震荡)
			int capTargetFps = 120;
		{
			auto capFrameTime = std::chrono::microseconds(1000000 / capTargetFps);
			if (!capFrameLimitInited) {
				capFrameLimitNext = std::chrono::steady_clock::now() + capFrameTime;
				capFrameLimitInited = true;
			} else {
				capFrameLimitNext += capFrameTime;
			}
			auto now = std::chrono::steady_clock::now();
			if (capFrameLimitNext > now) {
				auto sleepDur = capFrameLimitNext - now;
				auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(sleepDur);
				if (ms.count() > 1) std::this_thread::sleep_for(ms - std::chrono::milliseconds(1));
				// 捕获线程用Sleep(1)代替忙等待，避免抢占游戏CPU
				while (std::chrono::steady_clock::now() + std::chrono::milliseconds(1) < capFrameLimitNext) Sleep(1);
			} else {
				capFrameLimitNext = now;
			}
		}
		cudaSetDevice(m_config.gpu_id);
		if (m_screenW == 0) {
			m_screenW = GetSystemMetrics(SM_CXSCREEN);
			m_screenH = GetSystemMetrics(SM_CYSCREEN);
		}
		int cx = m_screenW / 2;
		int cy = m_screenH / 2;
		int writeIdx = m_captureWriteSlot.load();
		auto& slot = m_frameSlots[writeIdx];
		// 如果上一帧还没被推理线程消费，跳过此帧（丢帧保实时）
		if (slot.ready.load()) {
			Sleep(1);
			continue;
		}
		// 判断是否需要CPU端数据
		// 预览拷贝节流到10fps(旧为每5帧≈24fps): GPU满载(游戏+直播+录屏)时
		// 少做GPU→CPU staging拷贝=给推理和游戏留带宽; 预览窗口关闭时零开销。
		bool previewVisible = m_config.show_preview && m_previewHwnd && IsWindowVisible(m_previewHwnd);
		static auto s_lastPreviewCopy = std::chrono::steady_clock::now() - std::chrono::milliseconds(1000);
		bool previewDue = false;
		if (previewVisible) {
			auto nowPC = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(nowPC - s_lastPreviewCopy).count() >= 100) {
				s_lastPreviewCopy = nowPC;
				previewDue = true;
			}
		}
		// [v218] 跨卡模式: 推理在另一块卡, 共享纹理互操作不可用, 恒走CPU staging
		bool xdev = m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id;
		bool needCPU = xdev || !m_capture.isCudaInteropEnabled() || previewDue;
		auto capStart = std::chrono::steady_clock::now();
		auto frame = m_capture.capture(cx, cy, writeIdx, needCPU);
		m_captureMs.store((float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - capStart).count());
		if (!frame.valid) continue;
		slot.capMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(capStart.time_since_epoch()).count();
		slot.width = frame.width;
		slot.height = frame.height;
		slot.pitch = frame.pitch;
		slot.cudaRes = frame.cudaResource;
		slot.cudaInterop = !xdev && m_capture.isCudaInteropEnabled() && frame.cudaResource != nullptr;
		slot.aimCX = frame.width / 2;
		slot.aimCY = frame.height / 2;
		slot.recoilX = 0;
		slot.recoilY = 0;
		// 预览数据拷贝（需要CPU端帧数据）
		bool canCopyPreview = needCPU && frame.data && previewVisible;
		if (canCopyPreview) {
			int previewSize = frame.width * frame.height * 4;
			if ((int)m_previewBufWrite.size() < previewSize) m_previewBufWrite.resize(previewSize);
			if (frame.pitch == frame.width * 4) {
				memcpy(m_previewBufWrite.data(), frame.data, previewSize);
			} else {
				uint8_t* dst = m_previewBufWrite.data();
				for (int y = 0; y < frame.height; ++y) {
					memcpy(dst + y * frame.width * 4, frame.data + y * frame.pitch, frame.width * 4);
				}
			}
			{
				std::lock_guard<std::mutex> lock(m_detectMutex);
				m_previewW = frame.width;
				m_previewH = frame.height;
				std::swap(m_previewBuf, m_previewBufWrite);
			}
			m_previewReady.store(true);
		}
		// CUDA互操作模式：跳过Host→Device拷贝，推理线程直接从共享纹理读取
		if (!slot.cudaInterop && frame.data && xdev) {
			// [v218] 跨卡模式: 捕获卡(0)只做 D3D staging→pinned, 显存端H2D由推理卡线程完成
			// pinned 必须关联推理设备上下文(推理线程cudaMemcpyAsync同设备), 分配时临时切换
			int rawSize = frame.pitch * frame.height;
			if (!slot.pinnedBuf || slot.pinnedSize < (size_t)rawSize) {
				cudaSetDevice(m_config.infer_gpu_id);
				if (slot.pinnedBuf) cudaFreeHost(slot.pinnedBuf);
				slot.pinnedBuf = nullptr;
				slot.pinnedSize = 0;
				if (cudaMallocHost(&slot.pinnedBuf, rawSize) == cudaSuccess) {
					slot.pinnedSize = rawSize;
				}
			}
			if (slot.pinnedBuf) {
				// 槽复用前slot.ready=false(推理已消费且其H2D同步完成), pinned覆盖安全
				memcpy(slot.pinnedBuf, frame.data, rawSize);
			}
		} else if (!slot.cudaInterop && frame.data) {
			int rawSize = frame.pitch * frame.height;
			// [v184] 复用本槽前等待上一帧H2D完成, 防止覆盖仍在途的pinned数据
			if (!slot.h2dEvent) cudaEventCreateWithFlags(&slot.h2dEvent, cudaEventDisableTiming);
			if (!slot.preDoneEvent) cudaEventCreateWithFlags(&slot.preDoneEvent, cudaEventDisableTiming);
			if (slot.h2dPending && slot.h2dEvent) {
				cudaEventSynchronize(slot.h2dEvent);
				slot.h2dPending = false;
			}
			if (!slot.gpuBuf || frameBufSize < rawSize) {
				if (slot.gpuBuf) cudaFree(slot.gpuBuf);
				cudaError_t err = cudaMalloc(&slot.gpuBuf, rawSize);
				if (err != cudaSuccess) {
					slot.gpuBuf = nullptr;
					m_capture.unmap();
					continue;
				}
				// [v184] 页锁定中转缓冲: staging Map出的内存是不可分页显存映射,
				// 对它直接cudaMemcpyAsync会退化为驱动内部同步拷贝卡住捕获线程;
				// 先memcpy到pinned内存再异步H2D才是真异步
				if (!slot.pinnedBuf || slot.pinnedSize < (size_t)rawSize) {
					if (slot.pinnedBuf) cudaFreeHost(slot.pinnedBuf);
					slot.pinnedBuf = nullptr;
					slot.pinnedSize = 0;
					if (cudaMallocHost(&slot.pinnedBuf, rawSize) == cudaSuccess) {
						slot.pinnedSize = rawSize;
					}
				}
				frameBufSize = rawSize;
			}
			// 异步H2D拷贝：不在此处同步等待，由推理线程在消费时确保拷贝完成
			cudaStream_t copyStream = m_cudaStream;
			// [v184] GPU侧跨流排序: 推理线程可能已把本帧预处理kernel排入引擎流但尚未执行,
			// 新的H2D必须等它读完gpuBuf——否则copyStream会覆盖正在被读取的显存(偶发花帧/检测乱跳根因)
			if (slot.preDoneRecorded && slot.preDoneEvent) {
				cudaStreamWaitEvent(copyStream, slot.preDoneEvent, 0);
			}
			if (slot.pinnedBuf) {
				memcpy(slot.pinnedBuf, frame.data, rawSize);
				cudaMemcpyAsync(slot.gpuBuf, slot.pinnedBuf, rawSize, cudaMemcpyHostToDevice, copyStream);
			} else {
				cudaMemcpyAsync(slot.gpuBuf, frame.data, rawSize, cudaMemcpyHostToDevice, copyStream);
			}
			cudaEventRecord(slot.h2dEvent, copyStream);
			slot.h2dPending = true;
		}
		m_capture.unmap();
		// 标记此帧就绪，交换槽位（release屏障确保上面的写入对推理线程可见）
		std::atomic_thread_fence(std::memory_order_release);
		slot.ready.store(true);
		m_lastWrittenSlot.store(writeIdx);
		m_captureWriteSlot.store(1 - writeIdx);
	}
	// 释放槽位GPU缓冲区
	// [v218] 跨卡模式下gpuBuf/pinned由推理卡context分配, 释放前必须切到推理卡
	{
		int inferGpu = (m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id) ? m_config.infer_gpu_id : m_config.gpu_id;
		cudaSetDevice(inferGpu);
	}
	for (int i = 0; i < 2; ++i) {
		if (m_frameSlots[i].gpuBuf) {
			cudaFree(m_frameSlots[i].gpuBuf);
			m_frameSlots[i].gpuBuf = nullptr;
		}
		if (m_frameSlots[i].pinnedBuf) {
			cudaFreeHost(m_frameSlots[i].pinnedBuf);
			m_frameSlots[i].pinnedBuf = nullptr;
			m_frameSlots[i].pinnedSize = 0;
		}
		if (m_frameSlots[i].h2dEvent) { cudaEventDestroy(m_frameSlots[i].h2dEvent); m_frameSlots[i].h2dEvent = nullptr; }
		if (m_frameSlots[i].preDoneEvent) { cudaEventDestroy(m_frameSlots[i].preDoneEvent); m_frameSlots[i].preDoneEvent = nullptr; }
		m_frameSlots[i].h2dPending = false;
		m_frameSlots[i].preDoneRecorded = false;
		m_frameSlots[i].ready.store(false);
	}
}
void App::inferLoop() {
	m_frameLimitInited = false;
	int infAntiDbgCounter = 0;
	while (m_inferRunning.load() && m_running.load()) {
		if (++infAntiDbgCounter >= 500) {
			infAntiDbgCounter = 0;
			antidebug::runPeriodicCheck();
		}
		if (!m_modelLoaded.load() || !m_captureInited.load() || !m_config.aim_enabled) {
			m_inferMs.store(0.0f);
			m_gpuInferMs.store(0.0f);
			Sleep(1);
			continue;
		}
		// [v155] 锁频失败自动重试: 启动瞬间驱动未就绪/权限未提升会失败且旧逻辑永不重试,
		// 空载降频会使GPU推理延迟从2ms涨到7ms+。每约5秒重试直到成功
		{
			static int s_lockRetry = 0;
			if (m_config.gpu_lock_clocks && !gpu_clock::isActive() && ++s_lockRetry >= 600) {
				s_lockRetry = 0;
				// [v218] 锁频对象=推理卡(保障推理频率稳定)
				int lockGpu = (m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id) ? m_config.infer_gpu_id : m_config.gpu_id;
				gpu_clock::apply(lockGpu);
			}
		}
		// 查找就绪的帧槽（优先最近写入的，回退检查另一个）
		int readSlot = -1;
		int lastWritten = m_lastWrittenSlot.load();
		if (lastWritten >= 0 && m_frameSlots[lastWritten].ready.load()) {
			readSlot = lastWritten;
		} else if (lastWritten >= 0 && m_frameSlots[1 - lastWritten].ready.load()) {
			readSlot = 1 - lastWritten;
		} else if (lastWritten < 0) {
			if (m_frameSlots[0].ready.load()) readSlot = 0;
			else if (m_frameSlots[1].ready.load()) readSlot = 1;
		}
		if (readSlot < 0) {
			Sleep(1);
			continue;
		}
		auto& slot = m_frameSlots[readSlot];
		// acquire屏障确保捕获线程的写入对此线程可见
		std::atomic_thread_fence(std::memory_order_acquire);
		slot.ready.store(false);
		auto inferStart = std::chrono::steady_clock::now();
		// [v218] 推理设备=独立卡配置(0=与捕获同卡)
		int inferGpu = (m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id) ? m_config.infer_gpu_id : m_config.gpu_id;
		cudaSetDevice(inferGpu);
		processInference(slot);
		m_inferMs.store((float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inferStart).count());
		// 推理延迟统计
		m_inferTotalFrames.store(m_inferTotalFrames.load() + 1);
		m_inferMsSum.store(m_inferMsSum.load() + m_inferMs.load());
		float curMs = m_inferMs.load();
			if (curMs < m_inferMsMin.load()) m_inferMsMin.store(curMs);
			if (curMs > m_inferMsMax.load()) m_inferMsMax.store(curMs);
			static int s_peakLogCd = 0;
			if (s_peakLogCd > 0) s_peakLogCd--;
		// [v194] 尖峰帧即时捕获: 单帧>100ms时立即输出该帧完整分段(四段均为atomic,
		// 此时读取即该帧数据), 让700ms级尖峰的藏身处精确到分段
		if (curMs > 100.0f && s_peakLogCd <= 0) {
			s_peakLogCd = 600;  // ≈10秒限频
			float unclsP = curMs - m_inferWaitMs.load() - m_gpuInferMs.load() - m_postprocMs.load();
			if (unclsP < 0.0f) unclsP = 0.0f;
			LOG_WARN(OBS8(u8"[尖峰捕获] 总:%.1fms 等待:%.1f GPU:%.1f(预处理:%.1f TRT:%.1f) 后处理:%.1f 未分类:%.1f 槽:%d").c_str(),
				curMs, m_inferWaitMs.load(), m_gpuInferMs.load(), m_preGpuMs.load(),
				m_gpuInferMs.load() - m_preGpuMs.load(), m_postprocMs.load(), unclsP, readSlot);
		}
		// 帧率限制: 固定120fps硬拍。服务时间平均2-3ms<<8.3ms预算(利用率0.3)无需降拍,
		// 拍频切换反而让捕获/推理节奏失配(实测跳帧58+延迟震荡1-16ms)。
		// 只有持续超载(EMA>10ms连帧超过60次≈0.5秒)才降拍75兜底, 偶发尖峰不误触发。
		// [v179] 单帧巨尖峰(>150ms)=GPU被游戏场景加载抢占(大厅/选人/进入动画, 实测1.7s),
		// 立即降拍30让路: 游戏加载更快完成、我方恢复更快, 恢复后(EMA<5)自动回120。
		// 旧逻辑EMA衰减50帧才达60次超载计数, 恢复期始终120满拍与游戏争抢=两头都慢。
		{
			float ema = m_inferCostEma.load();
			ema = (ema <= 0.0f) ? curMs : (ema * 0.9f + curMs * 0.1f);
			m_inferCostEma.store(ema);
			int tfps = m_inferAutoFps.load();
			int overload = m_inferOverloadCount.load();
			if (ema > 10.0f) overload++;
			else overload = 0;
			m_inferOverloadCount.store(overload);
			if (curMs > 150.0f) {
				if (tfps > 30) {
					tfps = 30;
					if (s_peakLogCd <= 0) {
						s_peakLogCd = 1800;
						LOG_WARN(OBS1(u8"[尖峰] 单帧推理%.0fms, GPU被游戏场景加载抢占, 已降拍30让路").c_str(), curMs);
					}
				}
			}
			if (overload > 60 && tfps > 75) tfps = 75;
			else if (ema < 5.0f && tfps < 120) tfps = 120;
			m_inferAutoFps.store(tfps);
			m_inferTargetFps.store(tfps);
		}
		int targetFps = m_inferAutoFps.load();
		if (targetFps > 0) {
			auto frameTime = std::chrono::microseconds(1000000 / targetFps);
			if (!m_frameLimitInited) {
				m_frameLimitNext = inferStart + frameTime;
				m_frameLimitInited = true;
			} else {
				m_frameLimitNext += frameTime;
				// 已落后(满载超时)则重新对齐, 否则降拍后仍会连跑不睡眠、降拍不生效
				auto nowAlign = std::chrono::steady_clock::now();
				if (m_frameLimitNext < nowAlign) m_frameLimitNext = nowAlign + frameTime;
			}
			auto now = std::chrono::steady_clock::now();
			if (m_frameLimitNext > now) {
				auto sleepDur = m_frameLimitNext - now;
				auto us = std::chrono::duration_cast<std::chrono::microseconds>(sleepDur);
				// 精确休眠：先sleep到剩余0.5ms，再用短暂自旋补精度
				if (us.count() > 500) {
					std::this_thread::sleep_for(std::chrono::microseconds(us.count() - 500));
				}
				// 仅在最后0.5ms内短暂自旋（而非1ms+），大幅减少CPU占用
				while (std::chrono::steady_clock::now() + std::chrono::microseconds(100) < m_frameLimitNext) {
					Sleep(0);
				}
			} else {
				m_frameLimitNext = now;
			}
		}
	}
}
void App::processInference(FrameSlot& slot) {
	if (!m_modelLoaded.load()) return;
	if (!m_gpuStartEvent) {
		cudaEventCreate(&m_gpuStartEvent);
		cudaEventCreate(&m_gpuEndEvent);
		cudaEventCreate(&m_preEndEvent);
	}
	InferenceResult result;
	int aimCX = slot.aimCX;
	int aimCY = slot.aimCY;
	float recoilVelX = slot.recoilX;
	float recoilVelY = slot.recoilY;
	// ── 分段计时: 定位20ms到底花在哪 ──
	// waitMs = 等待捕获数据就绪(H2D同步 / 互操作Map, Map会与D3D队列同步)
	// gpuMs  = 预处理+推理的真实GPU耗时(cudaEvent测量)
	// 后处理单独计(m_postprocMs已有)
	auto t0 = std::chrono::steady_clock::now();
	// 非互操作模式下，确保捕获线程的H2D异步拷贝已完成
	if (!slot.cudaInterop && slot.h2dEvent && slot.gpuBuf) {
		cudaEventSynchronize(slot.h2dEvent);
	}
	// [v218] 跨卡模式: 捕获卡只填了pinned, 显存端H2D在本线程(推理卡context)完成
	if (!slot.cudaInterop && !slot.h2dEvent && slot.pinnedBuf && slot.pinnedSize > 0) {
		bool xdev = m_config.infer_gpu_id > 0 && m_config.infer_gpu_id != m_config.gpu_id;
		if (xdev) {
			if (!slot.gpuBuf || slot.gpuBufBytes < slot.pinnedSize) {
				if (slot.gpuBuf) cudaFree(slot.gpuBuf);
				slot.gpuBuf = nullptr;
				slot.gpuBufBytes = 0;
				if (cudaMalloc(&slot.gpuBuf, slot.pinnedSize) == cudaSuccess) {
					slot.gpuBufBytes = slot.pinnedSize;
				}
			}
			if (slot.gpuBuf) {
				// [v218] H2D必须用推理卡context的流(engine/dml流), m_cudaStream属于捕获卡0
				// H2D后cudaStreamSynchronize同步等(~0.1ms), 保证pinned复用安全与后续流可见性
				cudaStream_t h2dStream = (m_config.infer_mode == InferMode::DirectML) ? m_dmlEngine.getStream() : m_engine.getStream();
				cudaMemcpyAsync(slot.gpuBuf, slot.pinnedBuf, slot.pinnedSize, cudaMemcpyHostToDevice, h2dStream);
				cudaStreamSynchronize(h2dStream);
			}
		}
	}
	float waitMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (m_config.infer_mode == InferMode::DirectML) {
		if (!m_dmlEngine.isLoaded()) return;
		cudaStream_t stream = m_dmlEngine.getStream();
		cudaEventRecord(m_gpuStartEvent, stream);
		cuda_preprocess((const uint8_t*)slot.gpuBuf, slot.width, slot.height, slot.pitch, (float*)m_dmlEngine.getInputGPUBuffer(), m_config.model_width, m_config.model_height, stream);
		cudaEventRecord(m_preEndEvent, stream);
		result = m_dmlEngine.inferenceFromGPU();
		cudaEventRecord(m_gpuEndEvent, stream);
	} else if (slot.cudaInterop && slot.cudaRes) {
		if (!m_engine.isLoaded()) return;
		auto tMap0 = std::chrono::steady_clock::now();
		cudaStream_t stream = m_engine.getStream();
		cudaError_t err = cudaGraphicsMapResources(1, &slot.cudaRes, stream);
		waitMs += (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tMap0).count();
		if (err == cudaSuccess) {
			cudaArray_t array = nullptr;
			err = cudaGraphicsSubResourceGetMappedArray(&array, slot.cudaRes, 0, 0);
			if (err == cudaSuccess && array) {
				cudaEventRecord(m_gpuStartEvent, stream);
				cuda_preprocess((const uint8_t*)array, slot.width, slot.height, slot.width * 4, (float*)m_engine.getInputGPUBuffer(), m_config.model_width, m_config.model_height, stream);
				cudaEventRecord(m_preEndEvent, stream);
				result = m_engine.inferenceFromGPU();
				cudaEventRecord(m_gpuEndEvent, stream);
				cudaGraphicsUnmapResources(1, &slot.cudaRes, stream);
			} else {
				cudaGraphicsUnmapResources(1, &slot.cudaRes, stream);
				if (slot.gpuBuf) {
					cudaStream_t stream2 = m_engine.getStream();
					cudaEventRecord(m_gpuStartEvent, stream2);
					cuda_preprocess((const uint8_t*)slot.gpuBuf, slot.width, slot.height, slot.pitch, (float*)m_engine.getInputGPUBuffer(), m_config.model_width, m_config.model_height, stream2);
					cudaEventRecord(m_preEndEvent, stream2);
					result = m_engine.inferenceFromGPU();
					cudaEventRecord(m_gpuEndEvent, stream2);
				}
			}
		} else {
			if (slot.gpuBuf) {
				cudaStream_t stream2 = m_engine.getStream();
				cudaEventRecord(m_gpuStartEvent, stream2);
				cuda_preprocess((const uint8_t*)slot.gpuBuf, slot.width, slot.height, slot.pitch, (float*)m_engine.getInputGPUBuffer(), m_config.model_width, m_config.model_height, stream2);
				result = m_engine.inferenceFromGPU();
				cudaEventRecord(m_gpuEndEvent, stream2);
			}
		}
	} else {
		if (!m_engine.isLoaded()) return;
		cudaStream_t stream = m_engine.getStream();
		cudaEventRecord(m_gpuStartEvent, stream);
		cuda_preprocess((const uint8_t*)slot.gpuBuf, slot.width, slot.height, slot.pitch, (float*)m_engine.getInputGPUBuffer(), m_config.model_width, m_config.model_height, stream);
		result = m_engine.inferenceFromGPU();
		cudaEventRecord(m_gpuEndEvent, stream);
	}
	// 真实GPU耗时: D2H同步已在inferenceFromGPU内完成, 两端事件必然完成
	// 分段: 预处理段(gpuStart→preEnd) + TRT推理段(preEnd→gpuEnd), 用于定位5-9ms尖峰来源
	// [v194] 盲区修复: 尖峰帧(GPU被游戏抢占)时事件可能未决, cudaEventElapsedTime返回
	// cudaErrorNotReady被静默跳过→GPU段显示旧值(实测尖峰帧TRT恒显示1.5), 真实700ms
	// 藏进"总-分段"裂缝。同步m_gpuEndEvent后读取(已完成的同步成本≈0), 尖峰如实入账
	{
		cudaEventSynchronize(m_gpuEndEvent);
		float gpuMs = 0.0f, preMs = 0.0f;
		if (cudaEventElapsedTime(&gpuMs, m_gpuStartEvent, m_gpuEndEvent) == cudaSuccess && gpuMs > 0.0f) {
			m_gpuInferMs.store(gpuMs);
		}
		if (cudaEventElapsedTime(&preMs, m_gpuStartEvent, m_preEndEvent) == cudaSuccess && preMs > 0.0f) {
			m_preGpuMs.store(preMs);
		}
	}
	// [v184] gpuBuf消费完成事件: 捕获线程复用本槽的H2D会等待此事件(跨流排序),
	// 保证copyStream的新拷贝排在引擎流预处理读取之后, 消除显存读写竞态
	if (!slot.preDoneEvent) cudaEventCreateWithFlags(&slot.preDoneEvent, cudaEventDisableTiming);
	if (slot.preDoneEvent) {
		cudaStream_t doneStream = (m_config.infer_mode == InferMode::DirectML) ? m_dmlEngine.getStream() : m_engine.getStream();
		if (doneStream) cudaEventRecord(slot.preDoneEvent, doneStream);
		slot.preDoneRecorded = true;
	}
	// 后处理
	auto postprocStart = std::chrono::steady_clock::now();
	std::vector<Detection> detections;
	// [v184] directPtr优先: 指向引擎pinned缓冲, 空output不再表示无结果
	const float* outData = result.directPtr ? result.directPtr : (result.output.empty() ? nullptr : result.output.data());
	if (outData) {
		detections = YOLOPostProcess::process(outData, result.outputDims, m_config.yolo_version, m_config.confidence_threshold, m_config.nms_threshold, m_config.max_detections, getSelectedClasses());
		float scaleX = (float)slot.width / m_config.model_width;
		float scaleY = (float)slot.height / m_config.model_height;
		for (auto& det : detections) {
			det.cx *= scaleX;
			det.cy *= scaleY;
			det.w *= scaleX;
			det.h *= scaleY;
		}
	}
		if (!detections.empty()) {
			// FOV边界滞回：过滤半径放宽8%(R×1.08)——
			// · 目标框中心在圈缘逐帧抖动时不再"在/不在"硬开关弹出(闪框根因之一)
			// · 锁定目标漂出圈后仍被检测/追踪(轨迹存活)，真正远离(R×1.08外)才断
			// · 瞄准侧选新目标仍用严格R(computeTrackScore/switch路径的fovRadius不变)，
			//   环内目标只显示不锁——不会出现"圈外目标被吸走"
			float fovRadius = slot.width * m_config.aim_fov / 2.0f;
			float exitRadius = fovRadius * 1.08f;
			float fovCX = (float)aimCX;
			float fovCY = (float)aimCY;
			detections.erase(std::remove_if(detections.begin(), detections.end(),
				[exitRadius, fovCX, fovCY](const Detection& det) {
					float dx = det.cx - fovCX;
					float dy = det.cy - fovCY;
					return (dx * dx + dy * dy) > (exitRadius * exitRadius);
				}), detections.end());
		}
	// 写入结果供主线程读取（加锁保护）
	{
		std::lock_guard<std::mutex> lock(m_detectMutex);
		m_resultDetections = std::move(detections);
		m_resultAimCX = aimCX;
		m_resultAimCY = aimCY;
		m_resultFrameW = slot.width;
		m_resultFrameH = slot.height;
		m_resultRecoilX = recoilVelX;
		m_resultRecoilY = recoilVelY;
		m_resultCapMs.store(slot.capMs);
		m_resultReady.store(true);
	}
	m_postprocMs.store((float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - postprocStart).count());
	m_inferWaitMs.store(waitMs);
}
void App::updateFPS() {
	m_fpsCounter++;
	auto now = std::chrono::steady_clock::now();
	float elapsed = (float)std::chrono::duration<double>(now - m_fpsLastTime).count();
	if (elapsed >= 0.5f) {
		m_fps = m_fpsCounter / elapsed;
		m_fpsCounter = 0;
		m_fpsLastTime = now;
	}
}
void App::startMouseTest() {
	if (m_mouseTestRunning.load()) return;
	if (!m_driver.isAvailable()) return;
	m_mouseTestRunning.store(true);
	std::thread([this]() {
		POINT origPos;
		GetCursorPos(&origPos);
		int dx[] = {50, -50, 0, 0, -50, 50, 0, 0};
		int dy[] = {0, 0, 50, -50, 0, 0, 50, -50};
		for (int i = 0; i < 8; ++i) {
			if (!m_mouseTestRunning.load()) break;
			if (!m_driver.isAvailable()) break;
			m_driver.moveRelative(dx[i], dy[i]);
			Sleep(200);
		}
		POINT endPos;
		GetCursorPos(&endPos);
		LOG_INFO(OBS8(u8"鼠标测试完成, 偏移: (%d, %d)").c_str(), endPos.x - origPos.x, endPos.y - origPos.y);
		m_mouseTestRunning.store(false);
	}).detach();
}
void App::renderUI() {
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
	ImGui::Begin("##Main", nullptr, flags);
	float totalW = ImGui::GetWindowWidth();
	float totalH = ImGui::GetWindowHeight();
	float logH = 180.0f;
	float topH = totalH - logH - 20.0f;
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.75f, 1.00f, 1.00f));
	std::string d5TextStr = std::string(OBS1("D5 Render")) + " v" + D5_VERSION_STRING;
	float d5W = ImGui::CalcTextSize(d5TextStr.c_str()).x;
	ImGui::SetCursorPosX((totalW - d5W) / 2 - 50);
	ImGui::Text("%s", d5TextStr.c_str());
	ImGui::PopStyleColor();
	ImGui::SameLine();
	float btnW = 100.0f;
	ImGui::SetCursorPosX(totalW - btnW - 10);
	if (ImGui::Button(OBS2(u8"使用说明书").c_str(), ImVec2(btnW, 0))) {
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		std::wstring exeDir(exePath);
		size_t pos = exeDir.find_last_of(L"\\/");
		if (pos != std::wstring::npos) exeDir = exeDir.substr(0, pos);
		std::wstring htmlPath = exeDir + L"\\使用说明书.html";
		ShellExecuteW(NULL, L"open", htmlPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
	}
	ImGui::Spacing();
	float col1W = totalW * 0.30f;
	float col2W = totalW * 0.40f;
	float col3W = totalW - col1W - col2W - 24.0f;
	float colH = topH - 40.0f;
	ImGui::BeginChild("##BasicPanel", ImVec2(col1W, colH), true, ImGuiWindowFlags_None);
	renderBasicPanel();
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##ParamsPanel", ImVec2(col2W, colH), true, ImGuiWindowFlags_None);
	renderParamsPanel();
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("##AdvancedPanel", ImVec2(col3W, colH), true, ImGuiWindowFlags_None);
	renderAdvancedPanel();
	ImGui::EndChild();
	ImGui::Spacing();
	renderLogPanel();
	ImGui::End();
	if (m_showUpdatePopup) showUpdateDialog();
}
void App::renderBasicPanel() {
	if (ImGui::CollapsingHeader(OBS3(u8"模型设置").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		bool inferRunning = m_modelLoaded.load() && m_captureInited.load() && m_config.aim_enabled;
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.60f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.50f, 0.75f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.55f, 0.80f, 1.0f));
		if (inferRunning) {
			ImGui::BeginDisabled();
			ImGui::Button(OBS4(u8"选择模型").c_str(), ImVec2(-1, 28));
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"推理进行中，请先停止检测再切换模型").c_str());
		} else {
			if (ImGui::Button(OBS6(u8"选择模型").c_str(), ImVec2(-1, 28))) {
				OPENFILENAMEA ofn = {};
				char fileBuf[MAX_PATH] = {};
				strncpy_s(fileBuf, m_modelPathBuf, sizeof(fileBuf) - 1);
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = m_hwnd;
				ofn.lpstrFile = fileBuf;
				ofn.nMaxFile = sizeof(fileBuf);
				static std::string filterStr = OBS1("ONNX模型 (*.onnx)") + std::string(1, '\0') + OBS2("*.onnx") + std::string(1, '\0') + OBS3("所有文件 (*.*)") + std::string(1, '\0') + OBS4("*.*") + std::string(2, '\0');
				ofn.lpstrFilter = filterStr.c_str();
				ofn.lpstrTitle = OBS7(u8"选择YOLO模型文件").c_str();
				ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
				static std::string defExt = OBS5("onnx");
				ofn.lpstrDefExt = defExt.c_str();
				if (GetOpenFileNameA(&ofn)) {
					std::string newPath(fileBuf);
					if (newPath != m_config.model_path) {
						syncDisabledClasses();
						m_config.aim_enabled = false;
						m_aim.reset();
						m_modelLoaded.store(false);
						m_captureInited.store(false);
						resetInference();
						m_modelNumClasses = 0;
						m_modelClasses.clear();
						m_classEnabled.clear();
						m_config.disabled_classes.clear();
						strncpy_s(m_modelPathBuf, fileBuf, sizeof(m_modelPathBuf) - 1);
						m_config.model_path = fileBuf;
						YoloVersionInfo verInfo = detectYoloVersion(fileBuf);
						m_config.yolo_version = verInfo.version;
						m_detectedVersionName = verInfo.versionName;
						m_versionAutoDetected = verInfo.detected;
					}
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"点击浏览并选择YOLO模型的ONNX文件\n支持V5至V26版本\n选择后需点击[加载模型]按钮开始推理").c_str());
		}
		ImGui::PopStyleColor(3);
		if (!m_config.model_path.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.75f, 0.9f, 1.0f));
			const char* path = m_config.model_path.c_str();
			const char* fileName = strrchr(path, '\\');
			fileName = fileName ? fileName + 1 : path;
			ImGui::TextWrapped(OBS8(u8"当前: %s").c_str(), fileName);
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"完整路径: %s").c_str(), path);
		}
		ImGui::Spacing();
		ImVec4 verColor = m_versionAutoDetected ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.85f, 0.65f, 0.20f, 1.0f);
		ImGui::TextColored(verColor, OBS1(u8"模型版本: %s").c_str(), m_detectedVersionName.empty() ? OBS2(u8"未选择").c_str() : m_detectedVersionName.c_str());
		if (ImGui::IsItemHovered()) {
			if (m_versionAutoDetected) ImGui::SetTooltip(OBS3(u8"已自动识别模型版本\n系统根据模型元数据和结构特征判断\n绿色=识别成功").c_str());
			else ImGui::SetTooltip(OBS4(u8"未能自动识别模型版本，使用默认V8格式\n黄色=未识别，如检测不准确请手动切换\n点击右侧下拉框可手动覆盖").c_str());
		}
		ImGui::SameLine();
		static std::string ver0 = OBS1("V5"), ver1 = OBS2("V6"), ver2 = OBS3("V7"), ver3 = OBS4("V8");
		static std::string ver4 = OBS5("V9"), ver5 = OBS6("V10"), ver6 = OBS7("V11"), ver7 = OBS8("V26");
		const char* versions[] = { ver0.c_str(), ver1.c_str(), ver2.c_str(), ver3.c_str(), ver4.c_str(), ver5.c_str(), ver6.c_str(), ver7.c_str() };
		int verIdx = (int)m_config.yolo_version;
		ImGui::SetNextItemWidth(60);
		if (ImGui::Combo(OBS2("##ver_manual").c_str(), &verIdx, versions, IM_ARRAYSIZE(versions))) {
			m_config.yolo_version = (YoloVersion)verIdx;
			m_detectedVersionName = versions[verIdx];
			m_versionAutoDetected = true;
			if (m_modelLoaded.load()) updateClassList();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"手动选择YOLO版本\n仅在自动识别不准确时使用\n切换后立即生效").c_str());
		ImGui::Spacing();
		static std::string inf0 = OBS1("TensorRT"), inf1 = OBS2("DirectML");
		const char* inferNames[] = { inf0.c_str(), inf1.c_str() };
		int inferIdx = (int)m_config.infer_mode;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS7(u8"推理模式").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		if (inferRunning) {
			ImGui::BeginDisabled();
			ImGui::Combo(OBS3("##infer_mode").c_str(), &inferIdx, inferNames, IM_ARRAYSIZE(inferNames));
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"推理进行中，请先停止检测再切换推理模式").c_str());
		} else {
			if (ImGui::Combo(OBS4("##infer_mode").c_str(), &inferIdx, inferNames, IM_ARRAYSIZE(inferNames))) {
				if (m_modelLoaded.load()) {
					syncDisabledClasses();
					m_modelLoaded.store(false);
					m_captureInited.store(false);
					m_config.aim_enabled = false;
					m_aim.reset();
					resetInference();
					m_modelNumClasses = 0;
					m_modelClasses.clear();
					m_classEnabled.clear();
					m_config.disabled_classes.clear();
				}
				m_config.infer_mode = (InferMode)inferIdx;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"TensorRT: NVIDIA GPU专用，速度最快\nDirectML: 通用GPU加速，兼容性更好\n推荐: 有NVIDIA显卡选TensorRT\n切换后需重新加载模型").c_str());
		}
		if (m_config.infer_mode == InferMode::TensorRT) {
			static std::string prec0 = OBS3(u8"FP16 半精度"), prec1 = OBS4(u8"FP32 全精度");
			const char* precNames[] = { prec0.c_str(), prec1.c_str() };
			int precIdx = (int)m_config.trt_precision;
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS3(u8"TRT精度").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (inferRunning) {
				ImGui::BeginDisabled();
				ImGui::Combo(OBS5("##trt_precision").c_str(), &precIdx, precNames, IM_ARRAYSIZE(precNames));
				ImGui::EndDisabled();
			} else {
				if (ImGui::Combo(OBS6("##trt_precision").c_str(), &precIdx, precNames, IM_ARRAYSIZE(precNames))) {
					if (precIdx != (int)m_config.trt_precision && m_modelLoaded.load()) {
						syncDisabledClasses();
						m_modelLoaded.store(false);
						m_captureInited.store(false);
						m_config.aim_enabled = false;
						m_aim.reset();
						resetInference();
						m_modelNumClasses = 0;
						m_modelClasses.clear();
						m_classEnabled.clear();
						m_config.disabled_classes.clear();
					}
					m_config.trt_precision = (TrtPrecision)precIdx;
				}
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"FP16: 半精度推理，速度最快，可能丢精度导致目标框抖动\nFP32: 全精度推理，精度最高，速度稍慢\n切换后需重新加载模型").c_str());
			// GPU锁频: 防止空载降频(空载时GPU掉低功耗档→GPU延迟7ms+红色)
			{
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(OBS1(u8"GPU锁频").c_str());
				ImGui::SameLine();
				bool lockOn = m_config.gpu_lock_clocks && gpu_clock::isActive();
				if (ImGui::Checkbox(OBS2("##gpu_lock_clocks").c_str(), &lockOn)) {
					m_config.gpu_lock_clocks = lockOn;
					if (lockOn) gpu_clock::apply(m_config.gpu_id);
					else gpu_clock::clear();
					m_config.saveToFile();
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"锁定GPU最低频率(最大频率的70%%)，防止空闲时降频\n空载GPU延迟高/红色时开启可显著稳定推理延迟\n待机功耗会略增；关闭即恢复系统默认调频").c_str());
			}
			if (m_config.trt_precision == TrtPrecision::FP32) {
				ImGui::Indent(16.0f);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(OBS6(u8"混合精度优化").c_str());
				ImGui::SameLine();
				if (inferRunning) {
					ImGui::BeginDisabled();
					ImGui::Checkbox(OBS7("##mixed_prec").c_str(), &m_config.trt_mixed_precision);
					ImGui::EndDisabled();
				} else {
					if (ImGui::Checkbox(OBS8("##mixed_prec").c_str(), &m_config.trt_mixed_precision)) {
						if (m_modelLoaded.load()) {
							syncDisabledClasses();
							m_modelLoaded.store(false);
							m_captureInited.store(false);
							m_config.aim_enabled = false;
							m_aim.reset();
							resetInference();
							m_modelNumClasses = 0;
							m_modelClasses.clear();
							m_classEnabled.clear();
							m_config.disabled_classes.clear();
						}
					}
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"在FP32基础上允许部分层使用FP16加速\n性能较好的显卡优化后速度和FP16相差无几\n同时保持FP32的精度优势\n勾选后需重新加载模型").c_str());
				ImGui::Unindent(16.0f);
			}
		}
		ImGui::Spacing();
		if (!m_gpuNames.empty()) {
			std::vector<const char*> gpuPtrs;
			for (const auto& n : m_gpuNames) gpuPtrs.push_back(n.c_str());
			int gpuIdx = m_config.infer_gpu_id;  // [v218] 下拉=推理显卡(捕获固定主屏卡gpu_id)
			if (gpuIdx >= (int)m_gpuNames.size()) gpuIdx = 0;
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS5(u8"显卡选择").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (inferRunning) {
				ImGui::BeginDisabled();
				ImGui::Combo(OBS1("##gpu_select").c_str(), &gpuIdx, gpuPtrs.data(), (int)gpuPtrs.size());
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"推理进行中，请先停止检测再切换显卡").c_str());
			} else {
				if (ImGui::Combo(OBS2("##gpu_select").c_str(), &gpuIdx, gpuPtrs.data(), (int)gpuPtrs.size())) {
					if (gpuIdx != m_config.infer_gpu_id && m_modelLoaded.load()) {
						syncDisabledClasses();
						m_modelLoaded.store(false);
						m_captureInited.store(false);
						m_config.aim_enabled = false;
						m_aim.reset();
						resetInference();
						m_modelNumClasses = 0;
						m_modelClasses.clear();
						m_classEnabled.clear();
						m_config.disabled_classes.clear();
					}
					m_config.infer_gpu_id = gpuIdx;  // [v218] 只切推理卡, 捕获卡不动
					m_gpuPerf.detect(gpuIdx);
					const auto& gi2 = m_gpuPerf.getInfo();
					LOG_INFO(OBS3(u8"GPU切换: %s | %s | 显存:%.1fGB | 等级:%s | 推荐帧率:%dFPS").c_str(),
						m_gpuPerf.getVendorName(), gi2.name.c_str(), gi2.vramGB(), m_gpuPerf.getTierName(), gi2.autoTargetFps);
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"推理显卡(重载模型后生效)\n0=与捕获同卡\n1/2=独立卡: 推理与游戏物理解耦, 消除游戏抢占GPU导致的延迟尖峰\n推荐选择与主屏不同的空闲卡").c_str());
			}
		}
		ImGui::Spacing();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS3(u8"置信度阈值").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS3("##conf_thresh").c_str(), &m_config.confidence_threshold, 0.01f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"目标被识别的最低可信度\n越低=识别越多但可能有误判\n越高=识别越准但可能漏检\n推荐: 0.25~0.50").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS5(u8"NMS阈值").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS4("##nms_thresh").c_str(), &m_config.nms_threshold, 0.01f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"非极大值抑制阈值\n越低=重叠框越少\n越高=保留更多重叠框\n推荐: 0.45").c_str());
		ImGui::Unindent(8);
	}
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	float btnW = -1, btnH = 36;
	if (m_modelLoading.load()) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.35f, 0.35f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
		if (ImGui::Button(OBS8(u8"取消加载").c_str(), ImVec2(btnW, btnH))) {
			cancelModelLoad();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"取消正在进行的模型加载").c_str());
		ImGui::PopStyleColor(3);
		ImGui::Spacing();
		int prog = m_loadProgress.load();
		char progText[64];
		snprintf(progText, sizeof(progText), "%d%%", prog);
		ImGui::ProgressBar(prog / 100.0f, ImVec2(-1, 0), progText);
		{
			std::lock_guard<std::mutex> lock(m_loadMutex);
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
			ImGui::TextWrapped("%s", m_loadStatus.c_str());
			ImGui::PopStyleColor();
		}
	} else if (m_modelLoaded.load() && m_captureInited.load() && m_config.aim_enabled) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.20f, 0.15f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.25f, 0.18f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.90f, 0.30f, 0.22f, 1.0f));
		if (ImGui::Button(OBS5(u8"停止推理").c_str(), ImVec2(btnW, btnH))) {
			m_config.aim_enabled = false;
			m_aim.reset();
			LOG_INFO(OBS4(u8"已停止推理").c_str());
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"停止当前推理\n停止后可重新选择模型或再次加载").c_str());
		ImGui::PopStyleColor(3);
	} else {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.40f, 0.70f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.50f, 0.85f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.55f, 0.90f, 1.0f));
		if (m_config.model_path.empty()) {
			ImGui::BeginDisabled();
			ImGui::Button(OBS5(u8"加载模型").c_str(), ImVec2(btnW, btnH));
			ImGui::EndDisabled();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"请先选择一个模型文件").c_str());
		} else {
			if (ImGui::Button(OBS6(u8"加载模型").c_str(), ImVec2(btnW, btnH))) {
				startModelLoad();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"加载模型并开始推理\n首次加载TensorRT需要构建引擎，可能需要几分钟").c_str());
		}
		ImGui::PopStyleColor(3);
	}
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	if (ImGui::CollapsingHeader(OBS4(u8"配置管理").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		float cfgBtnW = (ImGui::GetContentRegionAvail().x - 8.0f) / 2.0f;
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.35f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.60f, 0.42f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.65f, 0.48f, 1.0f));
		if (ImGui::Button(OBS5(u8"保存配置").c_str(), ImVec2(cfgBtnW, 24))) {
			char fileBuf[MAX_PATH] = {};
			OPENFILENAMEA ofn = {};
			ofn.lStructSize = sizeof(ofn);
			ofn.lpstrFilter = OBS6(u8"配置文件 (*.ini)\0*.ini\0所有文件\0*.*\0").c_str();
			ofn.lpstrFile = fileBuf;
			ofn.nMaxFile = MAX_PATH;
			ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
			static std::string defExt = OBS7("ini");
			ofn.lpstrDefExt = defExt.c_str();
			if (GetSaveFileNameA(&ofn)) {
				m_config.saveToPath(fileBuf);
				LOG_INFO(OBS8(u8"配置已保存到: %s").c_str(), fileBuf);
			}
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"将当前所有参数设置保存为配置文件\n可分享给其他用户使用").c_str());
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.35f, 0.20f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.42f, 0.25f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.48f, 0.30f, 1.0f));
		if (ImGui::Button(OBS2(u8"加载配置").c_str(), ImVec2(-1, 24))) {
			char fileBuf[MAX_PATH] = {};
			OPENFILENAMEA ofn = {};
			ofn.lStructSize = sizeof(ofn);
			ofn.lpstrFilter = OBS3(u8"配置文件 (*.ini)\0*.ini\0所有文件\0*.*\0").c_str();
			ofn.lpstrFile = fileBuf;
			ofn.nMaxFile = MAX_PATH;
			ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
			if (GetOpenFileNameA(&ofn)) {
				if (m_config.loadFromPath(fileBuf)) {
					strncpy_s(m_modelPathBuf, m_config.model_path.c_str(), sizeof(m_modelPathBuf) - 1);
					YoloVersionInfo verInfo = detectYoloVersion(m_config.model_path);
					m_config.yolo_version = verInfo.version;
					m_detectedVersionName = verInfo.versionName;
					m_versionAutoDetected = verInfo.detected;
					LOG_INFO(OBS4(u8"配置已加载: %s").c_str(), fileBuf);
				} else {
					LOG_ERROR(OBS5(u8"配置加载失败: %s").c_str(), fileBuf);
				}
			}
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"从配置文件加载参数设置\n加载后所有参数将被替换为文件中的值").c_str());
		ImGui::PopStyleColor(6);
		ImGui::Unindent(8);
	}
	ImGui::Spacing();
	if (ImGui::CollapsingHeader(OBS2(u8"性能监控").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		const auto& gpuInfo = m_gpuPerf.getInfo();
		ImVec4 vendorColor = gpuInfo.vendor == GpuVendor::NVIDIA ? ImVec4(0.35f, 0.75f, 0.35f, 1.0f) :
			gpuInfo.vendor == GpuVendor::AMD ? ImVec4(0.85f, 0.30f, 0.30f, 1.0f) :
			gpuInfo.vendor == GpuVendor::Intel ? ImVec4(0.20f, 0.50f, 0.85f, 1.0f) :
			ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
		ImGui::TextColored(vendorColor, "%s", m_gpuPerf.getVendorName());
		ImGui::SameLine();
		ImGui::Text(OBS1(u8"| %s").c_str(), gpuInfo.name.c_str());
		ImGui::SameLine();
		ImVec4 tierColor = gpuInfo.tier == GpuTier::High ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) :
			gpuInfo.tier == GpuTier::MidHigh ? ImVec4(0.55f, 0.80f, 0.45f, 1.0f) :
			gpuInfo.tier == GpuTier::Mid ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) :
			gpuInfo.tier == GpuTier::Low ? ImVec4(0.90f, 0.50f, 0.30f, 1.0f) :
			ImVec4(0.98f, 0.45f, 0.35f, 1.0f);
		ImGui::TextColored(tierColor, OBS2(u8"| %s").c_str(), m_gpuPerf.getTierName());
		ImGui::SameLine();
		ImGui::Text(OBS3(u8"| %.0fGB").c_str(), gpuInfo.vramGB());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"GPU信息\n厂商: %s\n型号: %s\n显存: %.1fGB\n性能等级: %s").c_str(),
			m_gpuPerf.getVendorName(), gpuInfo.name.c_str(), gpuInfo.vramGB(), m_gpuPerf.getTierName());
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImVec4 fpsColor = m_fps > 100 ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : m_fps > 30 ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) : ImVec4(0.98f, 0.45f, 0.35f, 1.0f);
		ImGui::TextColored(fpsColor, "FPS: %.0f", m_fps);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"每秒处理帧数\n>100=优秀(绿色)\n30~100=正常(黄色)\n<30=卡顿(红色)").c_str());
		ImGui::SameLine(120);
		ImGui::Text(OBS2(u8"检测: %d").c_str(), m_detectionCount.load());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"当前帧检测到的目标数量").c_str());
		ImGui::Spacing();
		ImVec4 inferColor = m_inferMs.load() > 0 && m_inferMs.load() < 5.0f ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : m_inferMs.load() > 0 && m_inferMs.load() < 10.0f ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) : m_inferMs.load() >= 10.0f ? ImVec4(0.98f, 0.45f, 0.35f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
		ImGui::TextColored(inferColor, OBS6(u8"总延迟: %.2fms").c_str(), m_inferMs.load());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"单帧总处理延迟(捕获+预处理+推理+后处理)\n<5ms=优秀(绿色)\n5~10ms=正常(黄色)\n>10ms=较慢(红色)\n灰色=未运行").c_str());
		ImGui::SameLine();
		ImVec4 gpuColor = m_gpuInferMs.load() > 0 && m_gpuInferMs.load() < 3.0f ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : m_gpuInferMs.load() > 0 && m_gpuInferMs.load() < 6.0f ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) : m_gpuInferMs.load() >= 6.0f ? ImVec4(0.98f, 0.45f, 0.35f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
		ImGui::TextColored(gpuColor, OBS7(u8"GPU: %.2fms").c_str(), m_gpuInferMs.load());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"GPU计算延迟(预处理+推理)\n<3ms=优秀(绿色)\n3~6ms=正常(黄色)\n>6ms=较慢(红色)\n灰色=未运行").c_str());
		ImGui::SameLine();
		ImGui::Text(OBS4(u8"捕获: %.2fms").c_str(), m_captureMs.load());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"屏幕捕获延迟\nDXGI Desktop Duplication采集耗时\n通常<2ms").c_str());
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
		ImGui::Text(OBS7(u8"实时参数").c_str());
		ImGui::PopStyleColor();
		ImGui::Spacing();
		const char* engineName = m_config.infer_mode == InferMode::DirectML ? (m_dmlEngine.isDmlAvailable() ? "DirectML" : "DML(CPU)") : "TensorRT";
		ImGui::Text(OBS8(u8"引擎: %s").c_str(), engineName);
		ImGui::SameLine();
		// [v155] GPU锁频状态: 空载降频是低负载时GPU延迟高的主因(2ms→7ms+)
		if (gpu_clock::isActive()) {
			ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.0f), OBS3(u8"| 锁频:ON").c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"GPU最低频率已锁定(最大SM频率的85%%)\n推理延迟不再受空载降频影响").c_str());
		} else if (m_config.gpu_lock_clocks) {
			ImGui::TextColored(ImVec4(0.98f, 0.45f, 0.35f, 1.0f), OBS5(u8"| 锁频:重试中").c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"锁频尚未生效, 每5秒自动重试\n持续失败请检查是否以管理员身份运行").c_str());
		} else {
			ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), OBS7(u8"| 锁频:OFF").c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"锁频开关已关闭\n低负载时GPU空载降频可能导致推理延迟升高").c_str());
		}
		if (m_config.infer_mode == InferMode::TensorRT) {
			ImGui::SameLine();
			if (m_capture.isCudaInteropEnabled()) {
				ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.0f), OBS8(u8"| 零拷贝").c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"CUDA-D3D11互操作已启用\n屏幕数据直接在GPU端传递\n无需CPU中转，延迟降低1-2ms").c_str());
			}
			ImGui::SameLine();
			if (m_engine.isCudaGraphEnabled()) {
				ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.55f, 1.0f), OBS2(u8"| CUDA Graph").c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"CUDA Graph已启用\nTRT推理执行图已预编译\n消除kernel launch开销，延迟降低0.5-1ms").c_str());
			}
		}
		ImGui::SameLine();
		ImVec4 gainColor = m_aim.getCurrentGain() > 0.7f ? ImVec4(0.98f, 0.45f, 0.35f, 1.0f) : m_aim.getCurrentGain() > 0.4f ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) : ImVec4(0.35f, 0.88f, 0.55f, 1.0f);
		ImGui::TextColored(gainColor, OBS1(u8"| 有效增益: %.3f").c_str(), m_aim.getCurrentGain());
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"当前实际生效的移动增益\n=基础增益×速度倍率\n受预设模式和speed_multiplier影响").c_str());
		ImGui::Text(OBS7(u8"速度: %.1f | P:%.3f | 死区:%.1f | FOV:%.2f").c_str(), m_config.speed_multiplier, m_config.pid_kp, m_config.pid_deadzone, m_aim.getEffectiveFov());
		ImGui::SameLine();
		ImVec4 statusColor = m_aimActive.load() ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
			ImGui::TextColored(statusColor, m_aimActive.load() ? OBS5(u8"瞄准中").c_str() : OBS6(u8"检测中").c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"检测中(灰色)=目标检测持续运行\n瞄准中(绿色)=按住热键，鼠标正在移动到目标").c_str());
			if (m_config.aim_toggle_enabled) {
				ImGui::SameLine();
				ImVec4 toggleColor = m_aimToggled.load() ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.98f, 0.45f, 0.35f, 1.0f);
				ImGui::TextColored(toggleColor, m_aimToggled.load() ? OBS8(u8"总开关:ON").c_str() : OBS1(u8"总开关:OFF").c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"自瞄总开关状态\n绿色ON=总开关已开启，按住瞄准键即可锁定\n红色OFF=总开关已关闭，按住瞄准键也不会锁定\n按总开关热键切换状态").c_str());
			}
		ImGui::SameLine();
		static std::string pn0 = OBS3(u8"默认"), pn1 = OBS4(u8"自定义");
		const char* presetNames[] = { pn0.c_str(), pn1.c_str() };
		ImVec4 presetColors[] = { ImVec4(0.35f, 0.65f, 1.00f, 1.0f), ImVec4(0.70f, 0.45f, 0.90f, 1.0f) };
		int pIdx = std::clamp((int)m_config.aim_preset, 0, 1);
		ImGui::TextColored(presetColors[pIdx], OBS2(u8"[%s]").c_str(), presetNames[pIdx]);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"当前预设模式\n默认: 精准锁定+制动区防过冲\n自定义: 用户保存的参数组合").c_str());
		ImGui::Spacing();
		float sceneCplx = m_aim.getSceneComplexity();
		ImVec4 cplxColor = sceneCplx < 0.3f ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : sceneCplx < 0.6f ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) : ImVec4(0.98f, 0.45f, 0.35f, 1.0f);
		ImGui::TextColored(cplxColor, OBS1(u8"场景复杂度: %.2f").c_str(), sceneCplx);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"当前场景的复杂程度(0~1)\n<0.3=简单(绿色): 少量目标，低干扰\n0.3~0.6=中等(黄色): 多目标，有遮挡\n>0.6=复杂(红色): 大量目标，严重遮挡\n影响追踪器匹配阈值的自动调整").c_str());
		ImGui::SameLine();
		ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), OBS5(u8"准心检测: 关").c_str());
		float lockConf = m_aim.getLockConfidence();
		ImVec4 lockColor = lockConf > 0.6f ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : lockConf > 0.3f ? ImVec4(0.85f, 0.65f, 0.20f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
		ImGui::TextColored(lockColor, OBS7(u8"锁定置信度: %.2f").c_str(), lockConf);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"当前锁定目标的可靠程度(0~1)\n>0.6=高置信(绿色): 稳定锁定\n0.3~0.6=中等(黄色): 正在建立锁定\n<0.3=低置信(灰色): 刚锁定或即将丢失\n置信度越高越不容易被其他目标干扰").c_str());
		ImGui::Spacing();
			ImGui::SameLine();
			ImVec4 antiColor = m_config.anti_capture ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
			ImGui::TextColored(antiColor, m_config.anti_capture ? OBS2(u8"防截图: 开").c_str() : OBS3(u8"防截图: 关").c_str());
			if (m_config.aim_toggle_enabled) {
					ImGui::SameLine();
					ImVec4 toggleColor = m_aimToggled.load() ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.98f, 0.45f, 0.35f, 1.0f);
					std::string toggleStr = AimKey::GetName(m_config.aim_toggle_key);
					if (m_config.aim_toggle_key_mode == KeyMode::Combo) toggleStr = toggleStr + "+" + AimKey::GetName(m_config.aim_toggle_combo_key);
					ImGui::TextColored(toggleColor, OBS4(u8"总开关[%s]: %s").c_str(), toggleStr.c_str(), m_aimToggled.load() ? OBS5(u8"ON").c_str() : OBS6(u8"OFF").c_str());
				}
		ImGui::Unindent(8);
	}
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
	char hotkeyText[256];
	if (!m_config.aim_key_slots.empty()) {
		std::string keys;
		for (int i = 0; i < (int)m_config.aim_key_slots.size(); ++i) {
			if (i > 0) keys += "/";
			auto& slot = m_config.aim_key_slots[i];
			if (slot.key_mode == KeyMode::Single) {
				keys += AimKey::GetName(slot.vkCode);
			} else {
				keys += AimKey::GetName(slot.vkCode);
				keys += "+";
				keys += AimKey::GetName(slot.combo_vkCode);
			}
		}
		snprintf(hotkeyText, sizeof(hotkeyText), OBS3(u8"多按键: %s").c_str(), keys.c_str());
	} else if (m_config.key_mode == KeyMode::Single) {
		snprintf(hotkeyText, sizeof(hotkeyText), OBS1(u8"按键: %s").c_str(), AimKey::GetName(m_config.aim_key1.vkCode));
	} else {
		snprintf(hotkeyText, sizeof(hotkeyText), OBS2(u8"组合键: %s+%s").c_str(), AimKey::GetName(m_config.aim_key1.vkCode), AimKey::GetName(m_config.aim_key2.vkCode));
	}
	float hotkeyW = ImGui::CalcTextSize(hotkeyText).x;
	float panelW = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX((panelW - hotkeyW) / 2);
	ImGui::Text("%s", hotkeyText);
	ImGui::PopStyleColor();
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.25f, 1.0f));
	static std::string disclaimerStr = OBS4(u8"声明:本软件仅限技术研究与单机娱乐用途\n严禁用于任何违反服务条款的在线活动\n违规使用的一切法律后果由用户自行承担\n作者不承担任何连带责任");
	const char* disclaimer = disclaimerStr.c_str();
	float disW = ImGui::CalcTextSize(disclaimer).x;
	ImGui::SetCursorPosX((panelW - disW) / 2);
	ImGui::TextWrapped("%s", disclaimer);
	ImGui::PopStyleColor();
}
void App::renderParamsPanel() {
	if (ImGui::CollapsingHeader(OBS2(u8"自瞄设置").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
		ImGui::Text(OBS6(u8"预设模式").c_str());
		ImGui::PopStyleColor();
		ImGui::Spacing();
		float presetBtnW = (ImGui::GetContentRegionAvail().x - 12.0f) / 3.0f;
		bool isDefPreset = (m_config.aim_preset == AimPreset::Default);
		bool isCustomPreset = (m_config.aim_preset == AimPreset::Custom);
		if (isDefPreset) {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.80f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.85f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.60f, 0.90f, 1.0f));
		} else {
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.30f, 0.40f, 1.0f));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.35f, 0.45f, 1.0f));
		}
		if (ImGui::Button(OBS3(u8"默认").c_str(), ImVec2(presetBtnW, 26))) {
			m_config.aim_preset = AimPreset::Default;
			m_config.aim_position = 0.0f;
			m_config.aim_position_x = 0.5f;
			m_config.aim_smoothness = 0.35f; m_config.speed_multiplier = 1.0f;
		m_config.aim_fov = 0.5f;
			m_config.track_match_threshold = 100.0f; m_config.track_max_lost = 30; m_config.track_switch_hysteresis = 0.45f;
			m_config.scene_adapt_enabled = true;
			m_config.pid_kp = 0.30f;
			m_config.pid_deadzone = 5.0f;
			m_config.custom_preset_idx = -1;
		}
		ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"默认模式 - 精准锁定\n纯跟踪+制动区+死区\n无过冲无回弹，稳定跟踪").c_str());
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.35f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.35f, 0.50f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.40f, 0.55f, 1.0f));
		if (ImGui::Button(OBS3(u8"+").c_str(), ImVec2(presetBtnW, 26))) {
			CustomPreset cp;
			cp.name = OBS4(u8"自定义") + std::to_string(m_config.custom_presets.size() + 1);
			cp.aim_smoothness = m_config.aim_smoothness;
			cp.aim_position = m_config.aim_position;
			cp.aim_position_x = m_config.aim_position_x;
			cp.aim_fov = m_config.aim_fov;
			cp.speed_multiplier = m_config.speed_multiplier;
			cp.track_match_threshold = m_config.track_match_threshold;
			cp.track_max_lost = m_config.track_max_lost;
			cp.track_switch_hysteresis = m_config.track_switch_hysteresis;
			cp.track_confirm_frames = m_config.track_confirm_frames;
			cp.scene_adapt_enabled = m_config.scene_adapt_enabled;
			cp.pid_kp = m_config.pid_kp;
			cp.pid_deadzone = m_config.pid_deadzone;
			cp.quickscope_enabled = m_config.quickscope_enabled;
			cp.quickscope_key = m_config.quickscope_key;
			cp.quickscope_fire_delay = m_config.quickscope_fire_delay;
			cp.auto_trigger_enabled = m_config.auto_trigger_enabled;
			cp.auto_trigger_key = m_config.auto_trigger_key;
			cp.auto_trigger_delay_ms = m_config.auto_trigger_delay_ms;
			cp.auto_trigger_threshold = m_config.auto_trigger_threshold;
			cp.auto_trigger_interval_ms = m_config.auto_trigger_interval_ms;
			cp.auto_trigger_humanize = m_config.auto_trigger_humanize;
			m_config.custom_presets.push_back(cp);
			m_config.custom_preset_idx = (int)m_config.custom_presets.size() - 1;
			m_config.aim_preset = AimPreset::Custom;
		}
		ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"保存当前参数为自定义预设\n下次启动仍可使用").c_str());
		for (int i = 0; i < (int)m_config.custom_presets.size(); ++i) {
			if (i % 3 != 0) ImGui::SameLine();
			bool isActive = (isCustomPreset && m_config.custom_preset_idx == i);
			if (isActive) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.35f, 0.70f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.40f, 0.75f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.45f, 0.80f, 1.0f));
			} else {
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.22f, 0.28f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.28f, 0.38f, 1.0f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.35f, 0.33f, 0.43f, 1.0f));
			}
			float cpBtnW = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
			std::string btnLabel = m_config.custom_presets[i].name + OBS8("##cp").c_str() + std::to_string(i);
			if (ImGui::Button(btnLabel.c_str(), ImVec2(cpBtnW, 24))) {
				const auto& cp = m_config.custom_presets[i];
				m_config.aim_smoothness = cp.aim_smoothness;
				m_config.aim_position = cp.aim_position;
				m_config.aim_position_x = cp.aim_position_x;
				m_config.aim_fov = cp.aim_fov;
				m_config.speed_multiplier = cp.speed_multiplier;
				m_config.track_match_threshold = cp.track_match_threshold;
				m_config.track_max_lost = cp.track_max_lost;
				m_config.track_switch_hysteresis = cp.track_switch_hysteresis;
				m_config.track_confirm_frames = cp.track_confirm_frames;
				m_config.scene_adapt_enabled = cp.scene_adapt_enabled;
				m_config.pid_kp = cp.pid_kp;
				m_config.pid_deadzone = cp.pid_deadzone;
				m_config.quickscope_enabled = cp.quickscope_enabled;
				m_config.quickscope_key = cp.quickscope_key;
				m_config.quickscope_fire_delay = cp.quickscope_fire_delay;
				m_config.auto_trigger_enabled = cp.auto_trigger_enabled;
				m_config.auto_trigger_key = cp.auto_trigger_key;
				m_config.auto_trigger_delay_ms = cp.auto_trigger_delay_ms;
				m_config.auto_trigger_threshold = cp.auto_trigger_threshold;
				m_config.auto_trigger_interval_ms = cp.auto_trigger_interval_ms;
				m_config.auto_trigger_humanize = cp.auto_trigger_humanize;
				m_config.aim_preset = AimPreset::Custom;
				m_config.custom_preset_idx = i;
			}
			ImGui::PopStyleColor(3);
			if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
				int oldIdx = m_config.custom_preset_idx;
				m_config.custom_presets.erase(m_config.custom_presets.begin() + i);
				if (oldIdx == i) {
					m_config.aim_preset = AimPreset::Default;
					m_config.custom_preset_idx = -1;
				} else if (oldIdx > i) {
					m_config.custom_preset_idx--;
				}
				if (m_config.custom_preset_idx >= (int)m_config.custom_presets.size())
					m_config.custom_preset_idx = -1;
				break;
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"左键: 应用此预设\n右键: 删除此预设").c_str());
		}
		ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
				static std::vector<const char*> keyNamesCache;
				static bool keyNamesInit = false;
				if (!keyNamesInit) {
					for (int i = 0; i < 72; ++i) keyNamesCache.push_back(AimKey::GetName(AimKey::FromIndex(i)));
					keyNamesInit = true;
				}
				const char** keyNames = keyNamesCache.data();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
				ImGui::Text(OBS6(u8"自瞄总开关").c_str());
			ImGui::PopStyleColor();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS7(u8"启用总开关").c_str());
			ImGui::SameLine();
			ImGui::Checkbox(OBS2("##aim_toggle_enabled").c_str(), &m_config.aim_toggle_enabled);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"启用后，自瞄需要总开关开启才会生效\n防止局外误锁目标\n按热键切换开/关状态").c_str());
			ImGui::SameLine();
			ImGui::Checkbox(OBS3("##toggle_sound").c_str(), &m_config.toggle_sound);
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"总开关切换提示音效\n打勾: 切换时有提示音\n不打勾: 静默切换").c_str());
			if (m_config.aim_toggle_enabled) {
					ImVec4 toggleStatusColor = m_aimToggled.load() ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.98f, 0.45f, 0.35f, 1.0f);
					ImGui::SameLine();
					ImGui::TextColored(toggleStatusColor, m_aimToggled.load() ? OBS1(u8"[ON]").c_str() : OBS2(u8"[OFF]").c_str());
					// 按键模式选择
					static std::string tm0 = OBS3(u8"单键"), tm1 = OBS4(u8"组合键");
					const char* tgModeNames[] = { tm0.c_str(), tm1.c_str() };
					int tgModeIdx = (int)m_config.aim_toggle_key_mode;
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(OBS5(u8"按键模式").c_str());
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1);
					if (ImGui::Combo(OBS3("##toggle_key_mode").c_str(), &tgModeIdx, tgModeNames, 2)) {
						m_config.aim_toggle_key_mode = (KeyMode)tgModeIdx;
					}
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"单键: 按一个键切换开/关\n组合键: 需同时按住两个键才切换(防误触)\n局内建议组合键，防止误触切换").c_str());
					// 主键选择
					int tgIdx = AimKey::GetIndex(m_config.aim_toggle_key);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(OBS7(u8"按键").c_str());
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1);
					if (ImGui::Combo("##toggle_key1", &tgIdx, keyNames, 72)) {
						m_config.aim_toggle_key = AimKey::FromIndex(tgIdx);
					}
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"总开关的主按键\n按下时切换自瞄开/关状态\n推荐: F10、F11、F12等不常用键").c_str());
					// 组合键选择(仅组合键模式显示)
					if (m_config.aim_toggle_key_mode == KeyMode::Combo) {
						int tg2Idx = AimKey::GetIndex(m_config.aim_toggle_combo_key);
						ImGui::AlignTextToFramePadding();
						ImGui::TextUnformatted(OBS1(u8"组合键").c_str());
						ImGui::SameLine();
						ImGui::SetNextItemWidth(-1);
						if (ImGui::Combo("##toggle_key2", &tg2Idx, keyNames, 72)) {
							m_config.aim_toggle_combo_key = AimKey::FromIndex(tg2Idx);
						}
						if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"总开关的组合键\n需同时按住主键+此键才切换\n推荐: Ctrl、Shift、Alt等修饰键\n示例: 主键Q+组合键Ctrl = Ctrl+Q切换").c_str());
					}
				}
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::Separator();
				ImGui::Spacing();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
					ImGui::Text(OBS7(u8"按键模式").c_str());
					ImGui::PopStyleColor();
				static std::string km0 = OBS7(u8"单键"), km1 = OBS8(u8"组合键");
				const char* keyModeNames[] = { km0.c_str(), km1.c_str() };
		if (m_config.aim_key_slots.empty()) {
			int kmIdx = (int)m_config.key_mode;
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS8(u8"按键类型").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::Combo(OBS4("##key_mode").c_str(), &kmIdx, keyModeNames, IM_ARRAYSIZE(keyModeNames))) {
				m_config.key_mode = (KeyMode)kmIdx;
				validateKeys();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"单键: 按住一个键就激活\n组合键: 需要同时按住两个键才激活").c_str());
			int k1Idx = AimKey::GetIndex(m_config.aim_key1.vkCode);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS2(u8"主按键").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::Combo("##aim_key1", &k1Idx, keyNames, 72)) {
				m_config.aim_key1.vkCode = AimKey::FromIndex(k1Idx);
				validateKeys();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"按住此键激活\n推荐: 鼠标侧键1").c_str());
			if (m_config.key_mode == KeyMode::Combo) {
				int k2Idx = AimKey::GetIndex(m_config.aim_key2.vkCode);
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(OBS4(u8"副按键").c_str());
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-1);
				if (ImGui::Combo("##aim_key2", &k2Idx, keyNames, 72)) {
					m_config.aim_key2.vkCode = AimKey::FromIndex(k2Idx);
					validateKeys();
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"组合键模式的第二个按键").c_str());
			}
		} else {
			ImGui::TextDisabled(OBS6(u8"多按键模式已启用 (下方管理按键槽)").c_str());
		}
		if (!m_keyError.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
			ImGui::TextWrapped("%s", m_keyError.c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
		ImGui::Text(OBS8(u8"多按键管理").c_str());
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"添加多个按键槽，任意一个按键槽按下即可激活\n每个按键槽可独立设置单键或组合键模式").c_str());
		if (m_config.aim_key_slots.empty()) {
			if (ImGui::Button(OBS2(u8"+ 添加按键槽").c_str(), ImVec2(-1, 28))) {
				AimKeySlot newSlot;
				newSlot.vkCode = m_config.aim_key1.vkCode;
				newSlot.key_mode = m_config.key_mode;
				newSlot.combo_vkCode = m_config.aim_key2.vkCode;
				m_config.aim_key_slots.push_back(newSlot);
				validateKeys();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"将当前按键设置转为第一个按键槽\n之后可以继续添加更多按键槽").c_str());
		} else {
			for (int i = 0; i < (int)m_config.aim_key_slots.size(); ++i) {
				auto& slot = m_config.aim_key_slots[i];
				ImGui::PushID(i);
				ImGui::AlignTextToFramePadding();
				std::string slotLabel = OBS4(u8"按键") + std::to_string(i + 1);
				ImGui::Text("%s", slotLabel.c_str());
				ImGui::SameLine();
				int slotModeIdx = (int)slot.key_mode;
				ImGui::PushItemWidth(70);
				if (ImGui::Combo(OBS5(u8"模式").c_str(), &slotModeIdx, keyModeNames, IM_ARRAYSIZE(keyModeNames))) {
					slot.key_mode = (KeyMode)slotModeIdx;
					validateKeys();
				}
				ImGui::PopItemWidth();
				ImGui::SameLine();
				int vkIdx = AimKey::GetIndex(slot.vkCode);
				ImGui::PushItemWidth(90);
				if (ImGui::Combo(OBS6(u8"键").c_str(), &vkIdx, keyNames, 72)) {
					slot.vkCode = AimKey::FromIndex(vkIdx);
					validateKeys();
				}
				ImGui::PopItemWidth();
				if (slot.key_mode == KeyMode::Combo) {
					ImGui::SameLine();
					static std::string plusSign = OBS7(u8"+");
					ImGui::Text("%s", plusSign.c_str());
					ImGui::SameLine();
					int cvkIdx = AimKey::GetIndex(slot.combo_vkCode);
					ImGui::PushItemWidth(90);
					if (ImGui::Combo(OBS8(u8"副键").c_str(), &cvkIdx, keyNames, 72)) {
						slot.combo_vkCode = AimKey::FromIndex(cvkIdx);
						validateKeys();
					}
					ImGui::PopItemWidth();
				}
				ImGui::SameLine();
				if (ImGui::Button(OBS1(u8"删除").c_str(), ImVec2(40, 0))) {
					m_config.aim_key_slots.erase(m_config.aim_key_slots.begin() + i);
					validateKeys();
					ImGui::PopID();
					break;
				}
				ImGui::PopID();
			}
			ImGui::Spacing();
			float btnW2 = (ImGui::GetContentRegionAvail().x - 8.0f) / 2.0f;
			if ((int)m_config.aim_key_slots.size() < 10) {
				if (ImGui::Button(OBS2(u8"+ 添加按键").c_str(), ImVec2(btnW2, 26))) {
					AimKeySlot newSlot;
					m_config.aim_key_slots.push_back(newSlot);
					validateKeys();
				}
			} else {
				ImGui::BeginDisabled();
				ImGui::Button(OBS3(u8"已达上限(10)").c_str(), ImVec2(btnW2, 26));
				ImGui::EndDisabled();
			}
			ImGui::SameLine();
			if (ImGui::Button(OBS4(u8"清空所有按键").c_str(), ImVec2(btnW2, 26))) {
				m_config.aim_key_slots.clear();
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"清空所有按键槽，恢复为单按键模式").c_str());
		}
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
		ImGui::TextWrapped(OBS7(u8"吸附位置: 0=头部 0.5=中心 1=脚部").c_str());
		ImGui::PopStyleColor();
		if (!m_config.per_class_aim_enabled) {
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS8(u8"吸附位置Y").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			ImGui::SliderFloat(OBS6("##aim_position").c_str(), &m_config.aim_position, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"Y轴吸附位置\n0=头部(适合爆头)\n0.22=胸部\n0.5=身体中心(最稳定)\n0.75=膝盖(打腿)\n1=脚部(不推荐)\n可拖滑条微调").c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS5(u8"部位预设").c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(OBS1(u8"头部 0.00").c_str())) m_config.aim_position = 0.00f;
			ImGui::SameLine();
			if (ImGui::SmallButton(OBS2(u8"胸部 0.22").c_str())) m_config.aim_position = 0.22f;
			ImGui::SameLine();
			if (ImGui::SmallButton(OBS3(u8"中心 0.50").c_str())) m_config.aim_position = 0.50f;
			ImGui::SameLine();
			if (ImGui::SmallButton(OBS4(u8"膝盖 0.75").c_str())) m_config.aim_position = 0.75f;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"按真实人体比例, 膝关节约在全身检测框高度75%%处\n站姿最准; 蹲姿膝盖比例上移, 可拖滑条到0.60附近微调\n膝盖命中会截断移动能力, 但伤害低于头/胸").c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(OBS7(u8"脚部 0.95").c_str())) m_config.aim_position = 0.95f;
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS2(u8"吸附位置X").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			ImGui::SliderFloat(OBS7("##aim_position_x").c_str(), &m_config.aim_position_x, 0.0f, 1.0f, "%.2f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"X轴吸附位置\n0=目标左边缘\n0.5=目标中心(默认)\n1=目标右边缘\n推荐: 0.5").c_str());
		} else {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.8f, 1.0f, 1.0f));
			ImGui::TextWrapped(OBS4(u8"类别独立瞄准部位已启用，全局吸附位置已隐藏\n总开关与类别部位位于 高级参数 → 类别筛选 区块内").c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS2(u8"视野范围").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS8("##aim_fov").c_str(), &m_config.aim_fov, 0.1f, 1.0f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"自瞄搜索目标的圆形视野范围\n1.0=整个捕获区域\n0.1=仅屏幕中心附近\n推荐: 0.30~0.60\n圆形范围外的目标不会被检测和瞄准").c_str());
		ImGui::Checkbox(OBS5(u8"启用拟人化抖动").c_str(), &m_config.move_randomize);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"模拟人手特征的输出扰动(幅度抖动/弧线轨迹/正弦微扰/高斯噪声)\n开启后由下方[抖动强度]滑条控制幅度, 拉得越大抖动越厉害\n关闭(默认): 输出纯净, 吸附后完全静止, 开枪压枪稳定不晃").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS1(u8"抖动强度").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS1("##human_str").c_str(), &m_config.humanize_strength, 0.0f, 2.0f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"拟人扰动的总强度: 拉得越大抖动越厉害\n0.0=无扰动(输出纯净, 等效关闭抖动)\n1.0=标准拟人(默认)\n2.0=强力(扰动翻倍, 反检测更强但晃动明显)\n需先开启[启用拟人化抖动]开关").c_str());
		ImGui::Spacing();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS3(u8"速度倍率").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS5("##speed_mult").c_str(), &m_config.speed_multiplier, 0.1f, 3.0f, "%.1f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"鼠标移动速度的倍率\n1.0=正常速度\n<1=更慢更稳\n>1=更快但可能不自然\n推荐: 0.8~1.2").c_str());
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.3f, 1.0f));
		ImGui::Text(OBS1(u8"PID控制").c_str());
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"PID控制\nP=比例(响应速度) 死区=贴附稳定区\n只调这两个参数+曲线平滑+最大步长即可控制自瞄效果").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS3(u8"P增益 横向").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS6("##pid_kp").c_str(), &m_config.pid_kp, 0.01f, 1.00f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"横向比例增益, 控制水平跟枪速度\n越大=越快接近目标\n越小=越慢但更稳\n推荐0.15~0.35").c_str());
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS1(u8"死区").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			ImGui::SliderFloat(OBS1("##pid_deadzone").c_str(), &m_config.pid_deadzone, 0.0f, 15.0f, "%.1f");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"死区范围(像素)\n目标在死区内时不移动鼠标\n0=无死区(始终响应)\n越大=越不容易在接近目标时抖动\n默认3.0，推荐2.0~5.0").c_str());
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			if (ImGui::CollapsingHeader(OBS4(u8"自动扳机").c_str())) {
				ImGui::Indent(8);
				ImGui::Checkbox(OBS5(u8"启用自动扳机").c_str(), &m_config.auto_trigger_enabled);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"准星到达目标时自动开枪\n按住触发键+自瞄键时生效\n与对狙互斥").c_str());
				if (m_config.auto_trigger_enabled) {
					int atIdx = AimKey::GetIndex(m_config.auto_trigger_key.vkCode);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(OBS7(u8"触发键").c_str());
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1);
					if (ImGui::Combo("##at_key", &atIdx, keyNames, 72)) {
						m_config.auto_trigger_key.vkCode = AimKey::FromIndex(atIdx);
					}
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"按住此键激活自动扳机\n推荐: 鼠标侧键2").c_str());
					ImGui::SliderInt(OBS1(u8"开枪延迟(ms)").c_str(), &m_config.auto_trigger_delay_ms, 0, 500);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"准星到位后延迟多久再开枪\n0=立即开枪 50=约50ms(推荐)\n模拟人类反应时间，太短容易被检测").c_str());
					ImGui::SliderFloat(OBS3(u8"触发阈值(px)").c_str(), &m_config.auto_trigger_threshold, 0.5f, 30.0f, "%.1f");
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"准星距目标中心<=此像素数时判定到位\n越小=要求准星越准才开枪 5=推荐\n越大=越容易触发但可能打不中").c_str());
					ImGui::SliderInt(OBS4(u8"射击间隔(ms)").c_str(), &m_config.auto_trigger_interval_ms, 0, 500);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"每次开枪之间的间隔\n0=最快(每帧都开) 50=约20发/秒(推荐)\n根据武器射速调整，太快可能被检测").c_str());
					ImGui::Checkbox(OBS5(u8"拟人化随机").c_str(), &m_config.auto_trigger_humanize);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"随机化触发阈值和开枪延迟\n模拟人类不完美的操作\n推荐: 始终开启").c_str());
				}
				ImGui::Unindent(8);
			}
			if (ImGui::CollapsingHeader(OBS4(u8"对狙设置").c_str())) {
				ImGui::Indent(8);
				ImGui::Checkbox(OBS5(u8"启用对狙").c_str(), &m_config.quickscope_enabled);
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"按下触发键时瞬间吸附目标并自动开枪\n适合狙击枪: 开镜后按触发键即可锁+开枪").c_str());
				if (m_config.quickscope_enabled) {
					int qsIdx = AimKey::GetIndex(m_config.quickscope_key.vkCode);
					ImGui::AlignTextToFramePadding();
					ImGui::TextUnformatted(OBS7(u8"触发键").c_str());
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-1);
					if (ImGui::Combo("##qs_key", &qsIdx, keyNames, 72)) {
						m_config.quickscope_key.vkCode = AimKey::FromIndex(qsIdx);
					}
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"按下此键触发对狙: 瞬间吸附+开枪\n推荐: 鼠标侧键1").c_str());
					ImGui::SliderInt(OBS1(u8"开枪延迟(帧)").c_str(), &m_config.quickscope_fire_delay, 0, 20);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"吸附后等几帧再开枪\n0=立即开枪 3=约50ms延迟(推荐)\n延迟太短容易空枪，太长反应慢").c_str());
				}
				ImGui::Unindent(8);
			}
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.88f, 0.55f, 1.0f));
			ImGui::Text(OBS1(u8"高级优化").c_str());
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"目标追踪与运动预测的高级参数\n这些参数控制锁定稳定性和运动预测精度\n默认值已针对大多数场景优化\n如非必要不建议修改").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS3(u8"场景自适应").c_str());
		ImGui::SameLine();
		ImGui::Checkbox(OBS1("##scene_adapt").c_str(), &m_config.scene_adapt_enabled);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"根据场景复杂度自动调整算法参数\n开启: 复杂场景自动降低增益、增强阻尼(推荐)\n关闭: 使用固定参数\n推荐: 始终开启").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS4(u8"匹配阈值").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS2("##match_thresh").c_str(), &m_config.track_match_threshold, 50.0f, 250.0f, "%.0f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"目标重识别的搜索半径(像素)\n越小=仅匹配近距离目标(更精确但易丢)\n越大=远距离也能匹配(更宽容但可能误匹配)\n推荐: 100~150\n场景自适应开启时会自动调整").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS5(u8"丢失容忍").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderInt(OBS3("##max_lost").c_str(), &m_config.track_max_lost, 5, 40);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"目标消失后保持追踪的帧数\n越小=快速放弃丢失目标\n越大=长时间保持追踪(更稳定但可能锁错)\n推荐: 15~25\n值越大对短暂遮挡越容忍").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS6(u8"切换阻力").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS4("##switch_hyst").c_str(), &m_config.track_switch_hysteresis, 0.0f, 0.5f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"目标切换的阻力系数\n0=随时切换到更近目标(可能乱切)\n0.5=极难切换(非常稳定但可能跟错)\n推荐: 0.10~0.25\n值越大锁定越稳定，越不容易乱甩").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS8(u8"确认帧数").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderInt(OBS5("##track_confirm").c_str(), &m_config.track_confirm_frames, 1, 5);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"目标需连续检出≥N帧才允许成为锁定目标\n拦截枪口火焰/烟雾/特效等单帧干扰框抢锁\n预览中灰框+确认中=尚未可锁定\n1=关闭(旧版行为), 默认2\n击杀后切新目标会多等1帧检测(约20-60ms)").c_str());
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::Spacing();
		if (ImGui::Button(OBS7(u8"重置优化参数").c_str())) {
			// [v215] 重置值=吸附稳不过冲组合: P0.30+D0.20+步长14, 开枪震动不脱离不回拉
			m_config.track_match_threshold = 150.0f;
			m_config.track_max_lost = 75;
			m_config.track_switch_hysteresis = 0.45f;
			m_config.track_confirm_frames = 2;
			m_config.scene_adapt_enabled = true;
			m_config.pid_kp = 0.30f;
			m_config.pid_deadzone = 6.0f;
			m_config.move_max_step = 14.0f;
			m_config.move_inertia = 0.22f;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"恢复为吸附稳不过冲配置\n匹配=150, 丢失=75, 阻力=0.45\nPID: P=0.30\n死区=6, 步长=14, 曲线平滑=0.22\n调乱了就点这个重置").c_str());
		ImGui::Unindent(8);
	}
	ImGui::Spacing();
	if (ImGui::CollapsingHeader(OBS3(u8"捕获设置").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		ImGui::Text(OBS4(u8"捕捉区域: %d x %d").c_str(), m_config.model_width, m_config.model_height);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"捕捉区域自动跟随模型输入尺寸\n模型640x640→捕捉640x640\n模型320x320→捕捉320x320\n无需手动设置").c_str());
		static std::string capMode0 = OBS6(u8"WGC图形捕获(推荐)");
		static std::string capMode1 = OBS7(u8"DXGI桌面复制");
		static std::string capMode2 = OBS8(u8"按需DXGI(Hook)");
		const char* captureModeNames[] = { capMode0.c_str(), capMode1.c_str(), capMode2.c_str() };
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS1(u8"捕获方式").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		if (ImGui::Combo(OBS1("##capture_mode").c_str(), &m_config.capture_mode, captureModeNames, IM_ARRAYSIZE(captureModeNames))) {
			if (m_captureInited.load()) {
				m_captureInited.store(false);
				Sleep(50);
				m_capture.release();
				// 初始化失败自动回退WGC: DXGI桌面复制是排他接口, 被远程工具/
				// 直播软件占用时创建必失败 —— 不回退会让捕获处于死状态
				if (m_capture.initialize(m_config.model_width, m_config.model_height, m_config.gpu_id, (CaptureMode)m_config.capture_mode)) {
					m_config.capture_mode = (int)m_capture.getCaptureMode();
					if (m_config.infer_mode == InferMode::TensorRT) {
						m_capture.cudaRegisterSharedTexture();
					}
				} else {
					LOG_ERROR(OBS8(u8"捕获初始化失败(具体错误码见上方日志), 本次运行自动回退到WGC图形捕获(配置保留, 切换将重试)").c_str());
					m_capture.release();
					m_capture.initialize(m_config.model_width, m_config.model_height, m_config.gpu_id, CaptureMode::WGC);
					// [v185] 回退不改写capture_mode: 用户所选保留, 黄字显示实际模式, 重启/再切换会重试
					if (m_config.infer_mode == InferMode::TensorRT) {
						m_capture.cudaRegisterSharedTexture();
					}
				}
				m_captureInited.store(true);
			}
		}
		CaptureMode actualMode = m_capture.getCaptureMode();
		if (m_captureInited.load() && actualMode != (CaptureMode)m_config.capture_mode) {
				static std::string sWGC = OBS1("WGC"), sHook = OBS2("Hook"), sDXGI = OBS3("DXGI");
				const char* actualName = actualMode == CaptureMode::WGC ? sWGC.c_str() : (actualMode == CaptureMode::Hook ? sHook.c_str() : sDXGI.c_str());
			ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f,0.8f,0.0f,1.0f), OBS3(u8"(%s)").c_str(), actualName);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"WGC: Windows图形捕获，隐秘性高，Win10 2004+\nDXGI: 速度快，支持GPU直传，但可被反作弊检测\n按需DXGI: 每帧创建/释放duplication，避免持久化检测\n失败时自动回退到DXGI模式\n推荐: WGC > 按需DXGI > DXGI").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS2(u8"显示预览窗口").c_str());
		ImGui::SameLine();
		if (ImGui::Checkbox(OBS2("##show_preview").c_str(), &m_config.show_preview)) {
			if (m_config.show_preview) {
				createPreviewWindow();
			} else {
				destroyPreviewWindow();
			}
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"开启后显示独立的预览窗口\n可以看到捕获的画面和检测框\n推荐: 调试时开启，正常使用可关闭\n影响: 开启会占用少量额外资源").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS1(u8"安全防截图").c_str());
		ImGui::SameLine();
		if (ImGui::Checkbox(OBS3("##anti_capture").c_str(), &m_config.anti_capture)) {
			setAntiCapture(m_config.anti_capture);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"开启后外部截图/录屏软件无法捕获本程序窗口\n保护隐私安全，防止被反作弊截图检测").c_str());
		ImGui::Unindent(8);
	}
	ImGui::Spacing();
	if (ImGui::CollapsingHeader(OBS8(u8"驱动设置").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		static std::string drvName0 = OBS3(u8"罗技专用驱动");
		static std::string drvName1 = OBS4(u8"通用驱动");
		static std::string drvName2 = OBS5(u8"Makcu盒子");
		const char* driverNames[] = { drvName0.c_str(), drvName1.c_str(), drvName2.c_str() };
		int drvIdx = (int)m_config.driver_mode;
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS1(u8"鼠标驱动").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		if (ImGui::Combo(OBS4("##driver_mode").c_str(), &drvIdx, driverNames, IM_ARRAYSIZE(driverNames))) {
			switchDriver((DriverMode)drvIdx);
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"罗技专用: 需安装并运行Logitech G HUB客户端\n通用驱动: 使用系统通用鼠标移动(SendInput)\nMakcu盒子: 通过串口连接Makcu硬件驱动\n注意: 所选驱动不可用时不会自动切换").c_str());
		// 罗技驱动状态显示
		if (m_config.driver_mode == DriverMode::Logitech) {
			ImGui::Spacing();
			if (m_driver.isAvailable()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.88f, 0.55f, 1.0f));
				ImGui::TextUnformatted(OBS6(u8"LGHUB客户端: 已连接").c_str());
				ImGui::PopStyleColor();
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.35f, 1.0f));
				ImGui::TextUnformatted(OBS7(u8"LGHUB客户端: 未运行").c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"请先启动Logitech G HUB客户端，然后再重新选择此驱动").c_str());
			}
			ImGui::SameLine();
			if (ImGui::SmallButton(OBS1(u8"刷新").c_str())) {
				switchDriver(DriverMode::Logitech);
			}
		}
		// Makcu驱动设置
		if (m_config.driver_mode == DriverMode::Makcu) {
			ImGui::Spacing();
			static char portBuf[32] = "";
			strncpy(portBuf, m_config.makcu_port.c_str(), sizeof(portBuf) - 1);
			portBuf[sizeof(portBuf) - 1] = '\0';
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS7(u8"串口端口").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText(OBS5("##makcu_port").c_str(), portBuf, sizeof(portBuf));
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS3(u8"Makcu盒子的COM端口号\n留空则自动检测\n手动指定示例: COM3").c_str());
			m_config.makcu_port = portBuf;
			int baudIdx = 0;
			const int baudRates[] = { 115200, 921600, 460800, 230400, 57600 };
			static std::string bn0 = OBS1("115200"), bn1 = OBS2("921600"), bn2 = OBS3("460800"), bn3 = OBS4("230400"), bn4 = OBS5("57600");
			const char* baudNames[] = { bn0.c_str(), bn1.c_str(), bn2.c_str(), bn3.c_str(), bn4.c_str() };
			for (int i = 0; i < 5; ++i) { if (m_config.makcu_baud == baudRates[i]) { baudIdx = i; break; } }
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(OBS8(u8"波特率").c_str());
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1);
			if (ImGui::Combo(OBS6("##makcu_baud").c_str(), &baudIdx, baudNames, 5)) {
				m_config.makcu_baud = baudRates[baudIdx];
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"Makcu串口通信波特率\n默认: 115200\n固件3.7+支持921600高速模式\n如果连接不稳定请降低波特率").c_str());
			ImGui::Spacing();
			if (ImGui::Button(OBS2(u8"重新连接").c_str(), ImVec2(-1, 24))) {
				switchDriver(DriverMode::Makcu);
			}
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"断开并重新连接Makcu盒子\n更换端口或波特率后需点击重新连接").c_str());
			if (m_driver.isAvailable() && m_driver.getCurrentType() == MouseDrvType::MDT_Makcu) {
				auto& mk = m_driver.getMakcuDriver();
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.88f, 0.55f, 1.0f));
				ImGui::Text(OBS3(u8"已连接: %s | 固件: %s").c_str(), mk.getPortName().c_str(), mk.getFirmwareVersion().c_str());
				ImGui::PopStyleColor();
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.45f, 0.35f, 1.0f));
				ImGui::TextWrapped(OBS6(u8"Makcu盒子未连接 - 请检查设备是否已插入并安装CH343驱动").c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"常见问题:\n1. 确认Makcu盒子已通过USB连接到电脑\n2. 确认已安装CH343串口驱动\n3. 确认设备管理器中显示COM端口\n4. 确认没有其他软件占用该COM端口\n5. 尝试手动指定COM端口号").c_str());
			}
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
		ImGui::Text(OBS8(u8"当前驱动: %s").c_str(), m_driver.getMethodName().c_str());
		ImGui::PopStyleColor();
		if (!m_driver.getStatusMessage().empty()) {
			bool drvOk = m_driver.isAvailable();
			ImGui::PushStyleColor(ImGuiCol_Text, drvOk ? ImVec4(0.35f, 0.88f, 0.55f, 1.0f) : ImVec4(0.98f, 0.45f, 0.35f, 1.0f));
			ImGui::TextWrapped("%s", m_driver.getStatusMessage().c_str());
			ImGui::PopStyleColor();
		}
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.60f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.50f, 0.75f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.30f, 0.55f, 0.80f, 1.0f));
		bool testRunning = m_mouseTestRunning.load();
		if (testRunning) ImGui::BeginDisabled();
		if (ImGui::Button(OBS3(u8"鼠标移动测试").c_str(), ImVec2(-1, 28))) {
			startMouseTest();
		}
		if (testRunning) ImGui::EndDisabled();
		ImGui::PopStyleColor(3);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"测试鼠标移动功能是否正常\n点击后鼠标会自动移动8个方向\n测试期间请不要移动鼠标\n用于验证驱动是否正确工作").c_str());
		ImGui::Unindent(8);
	}
}
void App::renderAdvancedPanel() {
	if (!ImGui::CollapsingHeader(OBS4(u8"高级参数").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
	ImGui::Indent(6);
	ImGui::Spacing();
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(OBS3(u8"最大步长").c_str());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::SliderFloat(OBS8("##max_step").c_str(), &m_config.move_max_step, 1.0f, 50.0f, "%.1f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"单帧最大移动像素数\n值越大移动越快，但越不自然\n默认: 25.0\n推荐: 15~40").c_str());
		ImGui::Checkbox(OBS1(u8"启用过冲自抑制").c_str(), &m_config.os_enabled);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"检测到准星冲过目标时自动压低移动增益, 直到零过冲\n关闭后力道完全由基准力道滑条手动控制").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS3(u8"过冲压低").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS4("##os_damp").c_str(), &m_config.os_damp, 0.30f, 0.95f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"每次过冲后保留的力道比例\n越小压得越狠(更稳但变慢)\n默认: 0.70").c_str());
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(OBS6(u8"基准力道").c_str());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1);
		ImGui::SliderFloat(OBS7("##os_base").c_str(), &m_config.os_base, 0.10f, 1.00f, "%.2f");
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"手动基准力道: 1.0=完整增益\n吸附太软调大, 仍有过冲调小\n默认: 1.00").c_str());
	ImGui::TextUnformatted(OBS5(u8"曲线平滑").c_str());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(-1);
	ImGui::SliderFloat(OBS1("##curvesmooth").c_str(), &m_config.move_inertia, 0.0f, 0.95f, "%.2f");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"曲线平滑(smoothstep缓动): 近距轻柔收敛, 远距满力直给\n0=无平滑, 0.9=近距强烈轻柔\n默认: 0.20\n推荐: 0.10~0.35").c_str());
	ImGui::Spacing();
	if (ImGui::Button(OBS1(u8"恢复默认").c_str(), ImVec2(-1, 22))) {
		m_config.move_max_step = 25.0f;
		m_config.move_inertia = 0.20f;
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"将所有高级参数恢复为默认值").c_str());
	ImGui::Unindent(6);
	ImGui::Spacing();
	if (m_modelNumClasses > 0 && ImGui::CollapsingHeader(OBS2(u8"类别筛选").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Indent(8);
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
		bool fromMeta = !m_modelClasses.empty();
		ImGui::Text(fromMeta ? OBS3(u8"模型包含 %d 个类别 (来自模型元数据)").c_str() : OBS4(u8"模型包含 %d 个类别 (来自输出维度)").c_str(), m_modelNumClasses);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS5(u8"模型能识别的目标种类数量\n来自模型元数据=模型训练时定义的类别名\n来自输出维度=自动推算的类别数").c_str());
		if (ImGui::Button(OBS6(u8"全选").c_str())) { for (auto& e : m_classEnabled) e = 1; syncDisabledClasses(); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS6(u8"勾选所有类别，检测所有目标").c_str());
		ImGui::SameLine();
		if (ImGui::Button(OBS7(u8"全不选").c_str())) { for (auto& e : m_classEnabled) e = 0; syncDisabledClasses(); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS7(u8"取消所有类别，不检测任何目标").c_str());
		ImGui::SameLine();
		if (ImGui::Button(OBS8(u8"仅人物").c_str())) { for (int i = 0; i < m_modelNumClasses; ++i) m_classEnabled[i] = (i == 0) ? 1 : 0; syncDisabledClasses(); }
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS8(u8"仅勾选第一个类别(通常是人物)\n适合只需要检测人物的场景").c_str());
		ImGui::Spacing();
		ImGui::BeginChild("##ClassList", ImVec2(-1, std::min((float)m_modelNumClasses * 22.0f + 4.0f, 1200.0f)), true, ImGuiWindowFlags_None);
		for (int i = 0; i < m_modelNumClasses; ++i) {
			char label[128];
			snprintf(label, sizeof(label), "%d: %s", i, getClassName(i));
			bool checked = m_classEnabled[i] != 0;
			if (ImGui::Checkbox(label, &checked)) { m_classEnabled[i] = checked ? 1 : 0; syncDisabledClasses(); }
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS1(u8"勾选=检测此类别目标\n取消=忽略此类别目标").c_str());
		}
		ImGui::EndChild();
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		ImGui::Checkbox(OBS1(u8"按类别独立设置瞄准部位").c_str(), &m_config.per_class_aim_enabled);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"开启后隐藏全局吸附位置\n上方筛选勾选的类别=启用该类别独立瞄准部位\n取消勾选=该类别不显示也不启用(跟随全局)").c_str());
		if (m_config.per_class_aim_enabled) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
			ImGui::Text(OBS3(u8"勾选的类别显示在下，取消勾选即隐藏且不启用\n部位: 0=头 0.5=中心 1=脚").c_str());
			ImGui::PopStyleColor();
			int visCount = 0;
			for (int i = 0; i < m_modelNumClasses; ++i) {
				if (i < (int)m_classEnabled.size() && m_classEnabled[i]) visCount++;
			}
			if (visCount > 0) {
				ImGui::BeginChild(OBS4("##ClassAimList").c_str(), ImVec2(-1, (float)visCount * 34.0f + 10.0f), true, ImGuiWindowFlags_None);
				for (int i = 0; i < m_modelNumClasses; ++i) {
					if (i >= (int)m_config.class_aim_positions.size()) break;
					if (i >= (int)m_classEnabled.size() || !m_classEnabled[i]) continue;
					ClassAimPos& cap = m_config.class_aim_positions[i];
					ImGui::PushID(i);
					char clsLabel[128];
					snprintf(clsLabel, sizeof(clsLabel), "%d: %s", i, getClassName(i));
					ImGui::Text("%s", clsLabel);
					ImGui::SameLine();
					ImGui::SetNextItemWidth(-92);
					ImGui::SliderFloat(OBS5("##cap_y").c_str(), &cap.aim_position, 0.0f, 1.0f, "Y %.2f");
					ImGui::SameLine();
					// [v158] 每类快捷部位: 头(0.00)/膝(0.75), 配合滑条微调
					if (ImGui::SmallButton(OBS1(u8"头").c_str())) cap.aim_position = 0.00f;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS2(u8"该类别锁定头部(0.00)").c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton(OBS3(u8"膝").c_str())) cap.aim_position = 0.75f;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(OBS4(u8"该类别锁定膝盖(0.75)\n按真实人体比例, 蹲姿可拖滑条到0.60附近").c_str());
					ImGui::Spacing();
					ImGui::PopID();
				}
				ImGui::EndChild();
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.6f, 0.3f, 1.0f));
				ImGui::TextWrapped(OBS6(u8"上方类别筛选未勾选任何类别，无类别可设置").c_str());
				ImGui::PopStyleColor();
			}
		}
		ImGui::Unindent(8);
	}
	ImGui::Separator();
}
void App::renderLogPanel() {
	float panelW = ImGui::GetContentRegionAvail().x;
	float panelH = ImGui::GetContentRegionAvail().y;
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.07f, 1.00f));
	ImGui::BeginChild("##LogPanel", ImVec2(panelW, panelH), true, ImGuiWindowFlags_None);
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 1.00f, 1.00f));
	ImGui::Text(OBS3(u8"运行日志").c_str());
	ImGui::PopStyleColor();
	ImGui::SameLine(panelW - 80);
	if (ImGui::SmallButton(OBS2(u8"清空").c_str())) {
		AppLog::instance().clear();
	}
	ImGui::SameLine();
	bool autoScroll = AppLog::instance().getAutoScroll();
	if (autoScroll) {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.80f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.85f, 1.0f));
	} else {
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.18f, 0.22f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.30f, 0.40f, 1.0f));
	}
	if (ImGui::SmallButton(OBS3(u8"自动").c_str())) {
		AppLog::instance().setAutoScroll(!autoScroll);
	}
	ImGui::PopStyleColor(2);
	ImGui::Separator();
	ImGui::BeginChild("##LogScroll", ImVec2(0, 0), false, ImGuiWindowFlags_None);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));
	auto entries = AppLog::instance().getEntriesSnapshot();
	int logIdx = 0;
	for (const auto& entry : entries) {
		ImVec4 color;
		switch (entry.level) {
		case 2: color = ImVec4(0.98f, 0.35f, 0.30f, 1.0f); break;
		case 1: color = ImVec4(0.85f, 0.65f, 0.20f, 1.0f); break;
		default: color = ImVec4(0.60f, 0.65f, 0.72f, 1.0f); break;
		}
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.40f, 0.50f, 1.0f));
		ImGui::Text("[%s]", entry.timestamp.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::PushID(logIdx++);
		ImGui::TextWrapped("%s", entry.message.c_str());
		ImGui::PopStyleColor();
		// 右键日志条目弹出复制菜单(复制整行含时间戳 / 仅正文)
		if (ImGui::BeginPopupContextItem("##logctx")) {
			std::string full = "[" + entry.timestamp + "] " + entry.message;
			if (ImGui::MenuItem(OBS1(u8"复制此条日志").c_str())) ImGui::SetClipboardText(full.c_str());
			if (ImGui::MenuItem(OBS2(u8"复制正文(不含时间)").c_str())) ImGui::SetClipboardText(entry.message.c_str());
			ImGui::EndPopup();
		}
		ImGui::PopID();
	}
	ImGui::PopStyleVar();
	if (AppLog::instance().consumeDirty() && AppLog::instance().getAutoScroll()) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
	ImGui::EndChild();
	ImGui::PopStyleColor();
}
void App::showUpdateDialog() {
	ImGui::OpenPopup(OBS4(u8"发现新版本").c_str());
	ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - 200, ImGui::GetIO().DisplaySize.y / 2 - 100), ImGuiCond_FirstUseEver);
	if (ImGui::BeginPopupModal(OBS4(u8"发现新版本").c_str(), nullptr, ImGuiWindowFlags_NoResize)) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.20f, 0.85f, 1.00f, 1.0f));
		ImGui::Text("D5 Render v%s", m_updateInfo.latestVersion.c_str());
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextDisabled("(%s v%s)", OBS5(u8"当前版本").c_str(), D5_VERSION_STRING);
		ImGui::Separator();
		ImGui::Spacing();
		// 显示更新说明
		ImGui::PushTextWrapPos(380);
		if (!m_updateInfo.releaseNotes.empty()) {
			ImGui::TextWrapped("%s", m_updateInfo.releaseNotes.c_str());
		} else {
			ImGui::TextWrapped("%s", OBS6(u8"有新版本可用，请前往下载。").c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
		// 下载按钮
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.85f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.95f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.70f, 1.00f, 1.0f));
		if (ImGui::Button(OBS7(u8"打开下载页面").c_str(), ImVec2(180, 32))) {
			std::wstring url(m_updateInfo.downloadUrl.begin(), m_updateInfo.downloadUrl.end());
			ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			m_showUpdatePopup = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::PopStyleColor(3);
		ImGui::SameLine();
		ImGui::SetCursorPosX(380 - 80);
		if (ImGui::Button(OBS8(u8"稍后提醒").c_str(), ImVec2(80, 32))) {
			m_showUpdatePopup = false;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}
void App::shutdown() {
	// 恢复GPU默认时钟策略(幂等)
	gpu_clock::clear();
		// 幂等保护: WinMain 和 ~App 析构各会调用一次本函数。
		// 无保护时第二次进入时 ImGui 上下文已销毁 →
		// ImGui_ImplDX11_Shutdown 空指针崩溃(每次退出必崩, crash.log 194条记录)
		static bool s_shutdownDone = false;
		if (s_shutdownDone) return;
		s_shutdownDone = true;
		// 停止GPU调度心跳(CUDA资源销毁前必须停)
		stopHeartbeat();
		// 退出前确保释放自动扳机按住的左键，避免退出后左键卡死
		if (m_atLeftHeld) {
			m_driver.leftUp();
			m_atLeftHeld = false;
		}
		m_atFireActive = false;
		m_running.store(false);
		m_securityRunning.store(false);
		if (m_securityThread.joinable()) m_securityThread.join();
	savePreviewPos();
	m_config.saveToFile();
	if (m_modelLoading.load()) {
		cancelModelLoad();
	}
	if (m_loadThread.joinable()) m_loadThread.join();
	if (m_gpuStartEvent) { cudaEventDestroy(m_gpuStartEvent); m_gpuStartEvent = nullptr; }
	if (m_gpuEndEvent) { cudaEventDestroy(m_gpuEndEvent); m_gpuEndEvent = nullptr; }
	if (m_cudaStream) { cudaStreamDestroy(m_cudaStream); m_cudaStream = nullptr; }
	for (int i = 0; i < 2; ++i) {
		if (m_frameSlots[i].gpuBuf) {
			cudaFree(m_frameSlots[i].gpuBuf);
			m_frameSlots[i].gpuBuf = nullptr;
		}
		m_frameSlots[i].ready.store(false);
	}
	resetInference();
	m_driver.release();
	destroyPreviewWindow();
	ImGui_ImplDX11_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	cleanupRenderTarget();
	if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
	if (m_d3dContext) { m_d3dContext->Release(); m_d3dContext = nullptr; }
	if (m_d3dDevice) { m_d3dDevice->Release(); m_d3dDevice = nullptr; }
	if (m_hwnd) { DestroyWindow(m_hwnd); m_hwnd = nullptr; }
	UnregisterClassW(mainClassName.c_str(), nullptr);
}
LRESULT WINAPI App::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	switch (msg) {
	case WM_CREATE: {
		CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
		SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	case WM_SIZE: {
		App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
		if (app && app->m_d3dDevice) {
			app->cleanupRenderTarget();
			app->m_swapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
			app->createRenderTarget();
		}
		return 0;
	}
	case WM_GETMINMAXINFO: {
		App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
		if (app && app->m_fakeMinimized) return 0;
		MINMAXINFO* mmi = (MINMAXINFO*)lParam;
		mmi->ptMinTrackSize.x = 920;
		mmi->ptMinTrackSize.y = 780;
		return 0;
	}
	case WM_SYSCOMMAND:
		if ((wParam & 0xFFF0) == SC_MINIMIZE) {
			App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
			if (app) { app->fakeMinimize(); return 0; }
		}
		break;
	case WM_ACTIVATE:
		if (wParam != WA_INACTIVE) {
			App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
			if (app) app->restoreFromFakeMinimize();
		}
		break;
	case WM_LBUTTONDOWN: {
		App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
		if (app) app->restoreFromFakeMinimize();
		break;
	}
	case WM_DISPLAYCHANGE: {
			App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
			if (app) { app->m_screenW = 0; app->m_screenH = 0; }
			return 0;
		}
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}
LRESULT WINAPI App::PreviewWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	App* app = (App*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
	switch (msg) {
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hWnd, &ps);
		if (app && app->m_previewW > 0 && app->m_previewH > 0 && !app->m_previewBuf.empty()) {
			RECT clientRc;
			GetClientRect(hWnd, &clientRc);
			int clientW = clientRc.right;
			int clientH = clientRc.bottom;
			HDC memDC = CreateCompatibleDC(hdc);
			HBITMAP hBmp = CreateCompatibleBitmap(hdc, clientW, clientH);
			HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hBmp);
			HBRUSH bgBrush = CreateSolidBrush(RGB(10, 10, 16));
			FillRect(memDC, &clientRc, bgBrush);
			DeleteObject(bgBrush);
			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = app->m_previewW;
			bmi.bmiHeader.biHeight = -app->m_previewH;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			float scaleX = (float)clientW / app->m_previewW;
			float scaleY = (float)clientH / app->m_previewH;
			float scale = (scaleX < scaleY) ? scaleX : scaleY;
			int drawW = (int)(app->m_previewW * scale);
			int drawH = (int)(app->m_previewH * scale);
			int offX = (clientW - drawW) / 2;
			int offY = (clientH - drawH) / 2;
			SetStretchBltMode(memDC, HALFTONE);
			{
				std::lock_guard<std::mutex> lock(app->m_detectMutex);
				StretchDIBits(memDC, offX, offY, drawW, drawH, 0, 0, app->m_previewW, app->m_previewH, app->m_previewBuf.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
				HPEN fovPen = CreatePen(PS_SOLID, 2, RGB(255, 200, 0));
			HPEN oldPen2 = (HPEN)SelectObject(memDC, fovPen);
			HBRUSH oldBrush2 = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
			float fovRadius = (drawW * app->m_aim.getEffectiveFov()) / 2.0f;
			int centerX = offX + drawW / 2;
			int centerY = offY + drawH / 2;
			Ellipse(memDC, (int)(centerX - fovRadius), (int)(centerY - fovRadius), (int)(centerX + fovRadius), (int)(centerY + fovRadius));
			SelectObject(memDC, oldBrush2);
			SelectObject(memDC, oldPen2);
			DeleteObject(fovPen);
			SetTextColor(memDC, RGB(255, 200, 0));
			SetBkMode(memDC, TRANSPARENT);
			char fovLabel[64];
			snprintf(fovLabel, sizeof(fovLabel), OBS4(u8"FOV %.2f").c_str(), app->m_aim.getEffectiveFov());
			TextOutA(memDC, centerX - (int)fovRadius + 4, centerY - (int)fovRadius - 16, fovLabel, (int)strlen(fovLabel));
				HPEN crossPen = CreatePen(PS_SOLID, 1, RGB(255, 80, 80));
				HPEN oldPen3 = (HPEN)SelectObject(memDC, crossPen);
				MoveToEx(memDC, centerX - 12, centerY, nullptr);
				LineTo(memDC, centerX + 12, centerY);
				MoveToEx(memDC, centerX, centerY - 12, nullptr);
				LineTo(memDC, centerX, centerY + 12);
				SelectObject(memDC, oldPen3);
				DeleteObject(crossPen);
				// ── 画tracker轨迹而非原始检测 ──
				// 原始检测逐帧抖动(置信度在阈值边缘振荡/甩动拖影)→框闪烁。
				// 轨迹有30帧记忆:缺检期保持最后位置补位(暗黄"预测"框),锁定目标红框高亮。
				{
					const auto& tracks = app->m_aim.getTracksForDisplay();
					const int lockedId = app->m_aim.getLockedTrackId();
					const int confirmFrames = std::max(1, app->m_config.track_confirm_frames);
					const int DISPLAY_LOST_GRACE = 14;  // 缺检>14帧(~117ms)不再画——盖住12帧切换耐心窗口,回锁时不闪断
					HPEN penLocked = CreatePen(PS_SOLID, 3, RGB(255, 60, 60));
					HPEN penFresh = CreatePen(PS_SOLID, 2, RGB(0, 255, 128));
					HPEN penLost = CreatePen(PS_SOLID, 2, RGB(200, 170, 0));
					HPEN penTentative = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
					HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
					HBRUSH labelBrush = CreateSolidBrush(RGB(0, 0, 0));
					// 创建中文字体用于标签绘制(TextOutW需要CJK字体才能显示中文)
					HFONT labelFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
						DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
						CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
					HFONT oldLabelFont = (HFONT)SelectObject(memDC, labelFont);
					HPEN oldPen = (HPEN)SelectObject(memDC, penFresh);
					SetBkMode(memDC, TRANSPARENT);
					for (const auto& tr : tracks) {
						if (tr.lostFrames > DISPLAY_LOST_GRACE) continue;
						bool isLost = (tr.lostFrames > 0);
						bool isLocked = (tr.trackId == lockedId && lockedId >= 0 && !isLost);
						// [v231] 未确认轨迹(检出<确认帧数)标记为干扰嫌疑, 灰框"确认中"
						bool isTentative = (!isLocked && !isLost && tr.hitFrames < confirmFrames);
						// 新鲜框画显示平滑位置(dispCx/Cy, 仅预览显示不进控制);缺检框画track保持位置(连续补位)
						float bcx = isLost ? tr.cx : (tr.dispCx > 0.5f ? tr.dispCx : tr.rawCx);
						float bcy = isLost ? tr.cy : (tr.dispCy > 0.5f ? tr.dispCy : tr.rawCy);
						// [v231] 尺寸用轨迹smoothW/H(EMA+突变冻结)替代逐帧跳变的原始w/h
						float dispW = (tr.smoothW > 1.0f) ? tr.smoothW : tr.w;
						float dispH = (tr.smoothH > 1.0f) ? tr.smoothH : tr.h;
						float rx = offX + (bcx - dispW / 2.0f) * scale;
						float ry = offY + (bcy - dispH / 2.0f) * scale;
						float rw = dispW * scale;
						float rh = dispH * scale;
						SelectObject(memDC, isLocked ? penLocked : (isLost ? penLost : (isTentative ? penTentative : penFresh)));
						Rectangle(memDC, (int)rx, (int)ry, (int)(rx + rw), (int)(ry + rh));
						// UTF-8标签→wchar_t，用TextOutW正确显示中文
						char label[128];
						if (isLocked)
							snprintf(label, sizeof(label), "%s[锁] %.0f%%", app->getClassDisplayName(tr.classId), tr.confidence * 100);
						else if (isLost)
							snprintf(label, sizeof(label), "%s 预测", app->getClassDisplayName(tr.classId));
						else if (isTentative)
							snprintf(label, sizeof(label), "%s 确认中", app->getClassDisplayName(tr.classId));
						else
							snprintf(label, sizeof(label), "%s %.0f%%", app->getClassDisplayName(tr.classId), tr.confidence * 100);
						wchar_t wlabel[128];
						int wlen = MultiByteToWideChar(CP_UTF8, 0, label, -1, wlabel, 128);
						if (wlen <= 0) { wlabel[0] = L'?'; wlabel[1] = 0; wlen = 1; }
						SIZE textSize = {0, 0};
						GetTextExtentPoint32W(memDC, wlabel, wlen - 1, &textSize);
						int labelW = textSize.cx;
						RECT labelBg = {(int)rx, (int)ry - 16, (int)rx + labelW + 4, (int)ry};
						FillRect(memDC, &labelBg, labelBrush);
						SetTextColor(memDC, isLocked ? RGB(255, 60, 60) : (isLost ? RGB(200, 170, 0) : (isTentative ? RGB(150, 150, 150) : RGB(0, 255, 128))));
						TextOutW(memDC, (int)rx + 2, (int)ry - 15, wlabel, wlen - 1);
					}
					SelectObject(memDC, oldLabelFont);
					DeleteObject(labelFont);
					SelectObject(memDC, oldBrush);
					SelectObject(memDC, oldPen);
					DeleteObject(penLocked);
					DeleteObject(penFresh);
					DeleteObject(penLost);
					DeleteObject(penTentative);
					DeleteObject(labelBrush);
				}
			}
			// 绘制状态信息叠加层
			{
				float inferMs = app->m_inferMs.load();
				float captureMs = app->m_captureMs.load();
				float e2eMs = app->m_e2eMs;
				float fps = app->m_fps;
				int detCount = app->m_detectionCount.load();
				// 创建小字体
				HFONT hFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
					DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
					CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
				HFONT oldFont = (HFONT)SelectObject(memDC, hFont);
				int lineH = 16;
				int infoX = 6;
				int infoY = clientH - lineH * 5 - 6;
				HBRUSH infoBg = CreateSolidBrush(RGB(0, 0, 0));
				RECT infoRc = {0, infoY - 4, clientW, clientH};
				FillRect(memDC, &infoRc, infoBg);
				DeleteObject(infoBg);
				SetBkMode(memDC, TRANSPARENT);
				wchar_t wLine[128];
				SetTextColor(memDC, RGB(180, 220, 255));
				swprintf_s(wLine, L"FPS: %.1f", fps);
				TextOutW(memDC, infoX, infoY, wLine, (int)wcslen(wLine));
				infoY += lineH;
				SetTextColor(memDC, RGB(180, 255, 180));
				swprintf_s(wLine, L"Infer: %.1f ms", inferMs);
				TextOutW(memDC, infoX, infoY, wLine, (int)wcslen(wLine));
				infoY += lineH;
				SetTextColor(memDC, RGB(255, 220, 150));
				swprintf_s(wLine, L"Capture: %.1f ms", captureMs);
				TextOutW(memDC, infoX, infoY, wLine, (int)wcslen(wLine));
				infoY += lineH;
				SetTextColor(memDC, RGB(255, 180, 180));
				swprintf_s(wLine, L"E2E: %.1f ms", e2eMs);
				TextOutW(memDC, infoX, infoY, wLine, (int)wcslen(wLine));
				infoY += lineH;
				SetTextColor(memDC, RGB(200, 200, 200));
				swprintf_s(wLine, L"Targets: %d", detCount);
				TextOutW(memDC, infoX, infoY, wLine, (int)wcslen(wLine));
				SelectObject(memDC, oldFont);
				DeleteObject(hFont);
			}
			BitBlt(hdc, 0, 0, clientW, clientH, memDC, 0, 0, SRCCOPY);
			SelectObject(memDC, oldBmp);
			DeleteObject(hBmp);
			DeleteDC(memDC);
		} else {
			RECT rc;
			GetClientRect(hWnd, &rc);
			HBRUSH bgBrush = CreateSolidBrush(RGB(10, 10, 16));
			FillRect(hdc, &rc, bgBrush);
			DeleteObject(bgBrush);
			SetTextColor(hdc, RGB(128, 128, 128));
			SetBkMode(hdc, TRANSPARENT);
			static std::string waitStr = OBS6("Waiting...");
			const char* waitText = waitStr.c_str();
			TextOutA(hdc, rc.right / 2 - 40, rc.bottom / 2 - 8, waitText, (int)strlen(waitText));
		}
		EndPaint(hWnd, &ps);
		return 0;
	}
	case WM_ERASEBKGND:
		return 1;
	case WM_DESTROY:
		if (app) {
			app->m_previewHwnd = nullptr;
			app->m_config.show_preview = false;
		}
		return 0;
	case WM_MOVE:
		if (app) app->savePreviewPos();
		return 0;
	case WM_SIZE:
		if (app) app->savePreviewPos();
		InvalidateRect(hWnd, nullptr, FALSE);
		return 0;
	case WM_GETMINMAXINFO: {
			MINMAXINFO* mmi = (MINMAXINFO*)lParam;
			mmi->ptMinTrackSize.x = 200;
			mmi->ptMinTrackSize.y = 150;
			return 0;
		}
		}
		return DefWindowProcW(hWnd, msg, wParam, lParam);
	}
