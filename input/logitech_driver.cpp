#include "logitech_driver.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include "utils/dll_guard.h"
#include <cstring>
#include <shlobj.h>
#include <cstdio>
#include <tlhelp32.h>
static bool IsProcessRunning(const char* processName) {
	HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnapshot == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	wchar_t wName[MAX_PATH] = {};
	MultiByteToWideChar(CP_ACP, 0, processName, -1, wName, MAX_PATH);
	bool found = false;
	if (Process32FirstW(hSnapshot, &pe)) {
		do {
			if (_wcsicmp(pe.szExeFile, wName) == 0) {
				found = true;
				break;
			}
		} while (Process32NextW(hSnapshot, &pe));
	}
	CloseHandle(hSnapshot);
	return found;
}
static bool IsElevated() {
	BOOL elevated = FALSE;
	HANDLE hToken = nullptr;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
		TOKEN_ELEVATION te = {};
		DWORD size = 0;
		if (GetTokenInformation(hToken, TokenElevation, &te, sizeof(te), &size)) {
			elevated = te.TokenIsElevated;
		}
		CloseHandle(hToken);
	}
	return elevated != FALSE;
}
LogitechDriver::LogitechDriver() : m_methodName(GetTypeName(MouseDrvType::MDT_None)) {}
LogitechDriver::~LogitechDriver() { release(); }
std::string LogitechDriver::GetTypeName(MouseDrvType t) {
	switch (t) {
	case MouseDrvType::MDT_GHUB: return OBS1(u8"罗技GHUB驱动");
	case MouseDrvType::MDT_SendInput: return OBS2(u8"通用SendInput");
	case MouseDrvType::MDT_Makcu: return OBS3(u8"Makcu盒子");
	default: return OBS4(u8"未初始化");
	}
}
bool LogitechDriver::tryLoadFromPath(const char* fullPath) {
	HMODULE hDll = dllguard::safeLoadLibrary(fullPath);
	if (!hDll) {
		DWORD err = GetLastError();
		LOG_ERROR(OBS5(u8"加载DLL失败: %s (错误码: %lu)").c_str(), fullPath, err);
		return false;
	}
	MoveRFunc moveR = nullptr;
	std::string moveRNamesArr[] = { OBS1("moveR"), OBS2("MoveR"), OBS3("MOVER") };
	const char* moveRNames[] = { moveRNamesArr[0].c_str(), moveRNamesArr[1].c_str(), moveRNamesArr[2].c_str() };
	for (const char* name : moveRNames) {
		moveR = (MoveRFunc)GetProcAddress(hDll, name);
		if (moveR) break;
	}
	if (!moveR) {
		FreeLibrary(hDll);
		return false;
	}
	OpenFunc deviceOpen = (OpenFunc)GetProcAddress(hDll, OBS4("device_open").c_str());
	CloseFunc deviceClose = (CloseFunc)GetProcAddress(hDll, OBS5("device_close").c_str());
	m_keyDown = (KeyDownFunc)GetProcAddress(hDll, OBS6("key_down").c_str());
	m_keyUp = (KeyUpFunc)GetProcAddress(hDll, OBS7("key_up").c_str());
	m_mouseDown = (MouseDownFunc)GetProcAddress(hDll, OBS8("mouse_down").c_str());
	m_mouseUp = (MouseUpFunc)GetProcAddress(hDll, OBS1("mouse_up").c_str());
	m_scroll = (ScrollFunc)GetProcAddress(hDll, OBS2("scroll").c_str());
	if (m_ghubDll && m_deviceClose && m_deviceOpened) {
		m_deviceClose();
		m_deviceOpened = false;
	}
	if (m_ghubDll) {
		FreeLibrary(m_ghubDll);
	}
	m_ghubDll = hDll;
	m_moveR = moveR;
	m_deviceOpen = deviceOpen;
	m_deviceClose = deviceClose;
	if (m_deviceOpen) {
		int openResult = m_deviceOpen();
		if (openResult == 1) {
			m_deviceOpened = true;
		} else {
			LOG_WARN(OBS3(u8"罗技驱动device_open失败(返回:%d)，可能G HUB未运行或版本不兼容").c_str(), openResult);
			m_deviceOpened = false;
		}
	} else {
		OpenFunc initFunc = (OpenFunc)GetProcAddress(m_ghubDll, OBS4("INIT").c_str());
		if (initFunc) {
			int initResult = initFunc();
			if (initResult == 1) {
				m_deviceOpened = true;
			} else {
				m_deviceOpened = false;
			}
		} else {
			m_deviceOpened = true;
		}
	}
	char exeDir[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
	char* lastSlash = strrchr(exeDir, '\\');
	if (lastSlash) {
		*lastSlash = '\0';
		const char* dllFileName = strrchr(fullPath, '\\');
		dllFileName = dllFileName ? dllFileName + 1 : fullPath;
		char destPath[MAX_PATH];
		snprintf(destPath, sizeof(destPath), "%s\\%s", exeDir, dllFileName);
		if (_stricmp(fullPath, destPath) != 0) {
			if (!CopyFileA(fullPath, destPath, FALSE)) {
				LOG_WARN(OBS8(u8"自动复制驱动DLL失败").c_str());
			}
		}
	}
	return true;
}
bool LogitechDriver::loadGHUBDriver() {
	char exeDir[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
	char* lastSlash = strrchr(exeDir, '\\');
	if (lastSlash) *lastSlash = '\0';
	char userProfile[MAX_PATH] = {};
	ExpandEnvironmentStringsA("%USERPROFILE%", userProfile, MAX_PATH);
	char programFiles[MAX_PATH] = {};
	ExpandEnvironmentStringsA("%ProgramFiles%", programFiles, MAX_PATH);
	char programFilesX86[MAX_PATH] = {};
	ExpandEnvironmentStringsA("%ProgramFiles(x86)%", programFilesX86, MAX_PATH);
	char publicDocs[MAX_PATH] = {};
	ExpandEnvironmentStringsA("%PUBLIC%", publicDocs, MAX_PATH);
	char downloadsDir[MAX_PATH];
	snprintf(downloadsDir, MAX_PATH, "%s\\Downloads", userProfile);
	std::string dllNamesArr[] = {
		OBS5("logitech.driver.dll"),
		OBS6("ghub_device.dll"),
		OBS7("LogitechGHUBDevice.dll"),
		OBS8("LGHUBDevice.dll")
	};
	const char* dllNames[] = { dllNamesArr[0].c_str(), dllNamesArr[1].c_str(), dllNamesArr[2].c_str(), dllNamesArr[3].c_str() };
	struct SearchEntry { const char* path; int priority; };
	SearchEntry searchEntries[16];
	int searchCount = 0;
	searchEntries[searchCount++] = { exeDir, 90 };
	char customPath[512];
	snprintf(customPath, sizeof(customPath), OBS2("%s\\Downloads\\logitech.driver").c_str(), userProfile);
	searchEntries[searchCount].path = customPath;
	searchEntries[searchCount].priority = 85;
	searchCount++;
	static std::string path1 = OBS3("C:\\Program Files\\LGHUB");
	static std::string path2 = OBS4("C:\\Program Files (x86)\\LGHUB");
	static std::string path3 = OBS5("C:\\Program Files\\Logitech\\Logitech G HUB");
	static std::string path4 = OBS6("C:\\Program Files (x86)\\Logitech\\Logitech G HUB");
	static std::string path5 = OBS7("C:\\Program Files\\Logitech Gaming Software");
	static std::string path6 = OBS8("C:\\Program Files (x86)\\Logitech Gaming Software");
	searchEntries[searchCount++] = { path1.c_str(), 70 };
	searchEntries[searchCount++] = { path2.c_str(), 65 };
	searchEntries[searchCount++] = { path3.c_str(), 60 };
	searchEntries[searchCount++] = { path4.c_str(), 55 };
	searchEntries[searchCount++] = { path5.c_str(), 40 };
	searchEntries[searchCount++] = { path6.c_str(), 35 };
	bool lghubRunning = IsProcessRunning(OBS2("LGHUB.exe").c_str());
	bool lgsRunning = IsProcessRunning(OBS3("LCore.exe").c_str());
	bool isElevated = IsElevated();
	if (!isElevated) {
		LOG_WARN(OBS5(u8"未以管理员权限运行，SendInput在游戏中可能被UIPI阻止").c_str());
	}
	for (int i = 0; i < searchCount; ++i) {
		const SearchEntry& entry = searchEntries[i];
		for (const char* dllName : dllNames) {
			char fullPath[512];
			snprintf(fullPath, sizeof(fullPath), "%s\\%s", entry.path, dllName);
			DWORD attrs = GetFileAttributesA(fullPath);
			if (attrs == INVALID_FILE_ATTRIBUTES) continue;
			if (tryLoadFromPath(fullPath)) {
				return true;
			}
		}
	}
	return false;
}
bool LogitechDriver::loadMakcuDriver(const std::string& port, int baud) {
	if (m_makcuDriver.isConnected()) {
		m_makcuDriver.disconnect();
	}
	if (m_makcuDriver.connect(port, baud)) {
		return true;
	}
	return false;
}
bool LogitechDriver::initialize(MouseDrvType preferredType, const std::string& makcuPort, int makcuBaud) {
	release();
	m_statusMessage.clear();
	if (preferredType == MouseDrvType::MDT_GHUB) {
		m_currentType = MouseDrvType::MDT_GHUB;
		m_methodName = GetTypeName(MouseDrvType::MDT_GHUB);
		// 第一步: 检测LGHUB客户端是否运行
		bool lghubRunning = IsProcessRunning(OBS2("LGHUB.exe").c_str());
		if (!lghubRunning) {
			m_available = false;
			m_statusMessage = OBS1(u8"LGHUB客户端未运行，请先启动Logitech G HUB");
			LOG_WARN(OBS2(u8"罗技驱动不可用: LGHUB客户端未运行").c_str());
			return false;
		}
		// 第二步: 加载DLL
		if (!loadGHUBDriver()) {
			m_available = false;
			m_statusMessage = OBS3(u8"驱动DLL缺失(请查看日志)");
			LOG_WARN(OBS4(u8"罗技驱动不可用: DLL加载失败").c_str());
			return false;
		}
		// 第三步: device_open初始化
		if (!m_deviceOpened) {
			m_available = false;
			m_statusMessage = OBS5(u8"LGHUB客户端连接失败(版本不兼容?)");
			LOG_WARN(OBS6(u8"罗技驱动不可用: device_open/INIT失败").c_str());
			return false;
		}
		m_available = true;
		m_statusMessage = OBS7(u8"LGHUB客户端已连接，驱动正常");
		LOG_INFO(OBS8(u8"罗技GHUB驱动初始化成功").c_str());
		return true;
	}
	if (preferredType == MouseDrvType::MDT_SendInput) {
		m_available = true;
		m_currentType = MouseDrvType::MDT_SendInput;
		m_methodName = GetTypeName(MouseDrvType::MDT_SendInput);
		m_statusMessage = OBS4(u8"通用SendInput驱动就绪");
		return true;
	}
	if (preferredType == MouseDrvType::MDT_Makcu) {
		m_currentType = MouseDrvType::MDT_Makcu;
		m_methodName = GetTypeName(MouseDrvType::MDT_Makcu);
		if (loadMakcuDriver(makcuPort, makcuBaud)) {
			m_available = true;
			m_methodName += " (" + m_makcuDriver.getPortName() + ")";
			m_statusMessage = OBS5(u8"Makcu盒子已连接 - 固件: ") + m_makcuDriver.getFirmwareVersion();
			return true;
		}
		m_available = false;
		m_statusMessage = OBS6(u8"Makcu盒子未连接，请检查设备");
		LOG_WARN(OBS7(u8"Makcu驱动不可用: 设备未连接").c_str());
		return false;
	}
	m_available = false;
	m_currentType = MouseDrvType::MDT_None;
	m_methodName = GetTypeName(MouseDrvType::MDT_None);
	m_statusMessage = OBS8(u8"未指定驱动类型");
	return false;
}
void LogitechDriver::moveRelative(int dx, int dy) {
	if (m_currentType == MouseDrvType::MDT_GHUB) {
		if (m_available && m_moveR && m_deviceOpened) {
			m_moveR(dx, dy);
			return;
		}
		return;
	}
	if (m_currentType == MouseDrvType::MDT_Makcu) {
		if (m_makcuDriver.isConnected()) {
			m_makcuDriver.moveRelative(dx, dy);
			return;
		}
		return;
	}
	if (m_currentType == MouseDrvType::MDT_SendInput) {
		moveViaSendInput(dx, dy);
		return;
	}
}
void LogitechDriver::clickLeft() {
	leftDown();
	leftUp();
}
extern volatile DWORD s_ignorePhysUntil;
void LogitechDriver::leftDown() {
	if (!m_available) return;
	if (m_currentType == MouseDrvType::MDT_GHUB && m_mouseDown) {
		s_ignorePhysUntil = GetTickCount() + 15;
		m_mouseDown(1);
	} else if (m_currentType == MouseDrvType::MDT_Makcu && m_makcuDriver.isConnected()) {
		s_ignorePhysUntil = GetTickCount() + 15;
		m_makcuDriver.leftDown();
	} else {
		INPUT input = {};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		input.mi.dwExtraInfo = 0; // 不带AT_MAGIC，游戏无法区分虚拟和物理按下
		SendInput(1, &input, sizeof(INPUT));
	}
}
void LogitechDriver::leftDownHold() {
	if (!m_available) return;
	if (m_currentType == MouseDrvType::MDT_GHUB && m_mouseDown) {
		m_mouseDown(1);
	} else if (m_currentType == MouseDrvType::MDT_Makcu && m_makcuDriver.isConnected()) {
		m_makcuDriver.leftDown();
	} else {
		INPUT input = {};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
		input.mi.dwExtraInfo = 0;
		SendInput(1, &input, sizeof(INPUT));
	}
}
void LogitechDriver::leftUp() {
	if (!m_available) return;
	if (m_currentType == MouseDrvType::MDT_GHUB && m_mouseUp) {
		s_ignorePhysUntil = GetTickCount() + 15;
		m_mouseUp(1);
	} else if (m_currentType == MouseDrvType::MDT_Makcu && m_makcuDriver.isConnected()) {
		s_ignorePhysUntil = GetTickCount() + 15;
		m_makcuDriver.leftUp();
	} else {
		INPUT input = {};
		input.type = INPUT_MOUSE;
		input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
		input.mi.dwExtraInfo = 0; // 不带AT_MAGIC
		SendInput(1, &input, sizeof(INPUT));
	}
}
void LogitechDriver::moveViaSendInput(int dx, int dy) {
	typedef UINT(WINAPI* PFN_SendInput)(UINT, LPINPUT, int);
	static PFN_SendInput pfnSendInput = nullptr;
	if (!pfnSendInput) {
		HMODULE hUser32 = GetModuleHandleA(OBS1("user32.dll").c_str());
		if (!hUser32) hUser32 = LoadLibraryA(OBS1("user32.dll").c_str());
		if (hUser32) pfnSendInput = (PFN_SendInput)GetProcAddress(hUser32, OBS2("SendInput").c_str());
	}
	if (!pfnSendInput) return;
	INPUT input = {};
	input.type = INPUT_MOUSE;
	input.mi.dx = dx;
	input.mi.dy = dy;
	input.mi.dwFlags = MOUSEEVENTF_MOVE;
	input.mi.time = 0;
	input.mi.dwExtraInfo = GetMessageExtraInfo();
	pfnSendInput(1, &input, sizeof(INPUT));
}
void LogitechDriver::release() {
	if (m_makcuDriver.isConnected()) {
		m_makcuDriver.disconnect();
	}
	if (m_deviceClose && m_ghubDll && m_deviceOpened) {
		m_deviceClose();
		m_deviceOpened = false;
	}
	if (m_ghubDll) {
		FreeLibrary(m_ghubDll);
		m_ghubDll = nullptr;
	}
	m_moveR = nullptr;
	m_deviceOpen = nullptr;
	m_deviceClose = nullptr;
	m_keyDown = nullptr;
	m_keyUp = nullptr;
	m_mouseDown = nullptr;
	m_mouseUp = nullptr;
	m_scroll = nullptr;
	m_available = false;
	m_currentType = MouseDrvType::MDT_None;
	m_methodName = GetTypeName(MouseDrvType::MDT_None);
	m_statusMessage.clear();
}
