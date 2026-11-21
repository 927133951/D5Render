#pragma once
#include <windows.h>
#include <winternl.h>
#include <TlHelp32.h>
#include <intrin.h>
#include <iphlpapi.h>
#include <psapi.h>
#include "utils/obfstr.h"
namespace antidebug {
// ========== CRC32 ==========
inline uint32_t crc32Table() {
	static uint32_t table[256] = {};
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int j = 0; j < 8; ++j) c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
			table[i] = c;
		}
		init = true;
	}
	return 0;
}
inline uint32_t computeCRC(const uint8_t* data, size_t len) {
	(void)crc32Table();
	static uint32_t* tbl = nullptr;
	if (!tbl) {
		static uint32_t table[256] = {};
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int j = 0; j < 8; ++j) c = (c >> 1) ^ (0xEDB88320 & (-(c & 1)));
			table[i] = c;
		}
		tbl = table;
	}
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < len; ++i) crc = (crc >> 8) ^ tbl[(crc ^ data[i]) & 0xFF];
	return ~crc;
}
// ========== 基础调试检测 ==========
inline bool checkDebuggerPresent() {
	if (IsDebuggerPresent()) return true;
	BOOL remoteDebugger = FALSE;
	CheckRemoteDebuggerPresent(GetCurrentProcess(), &remoteDebugger);
	if (remoteDebugger) return true;
	return false;
}
inline DWORD readNtGlobalFlag(PVOID pebBase) {
	DWORD flag = 0;
	__try {
		flag = *(DWORD*)((BYTE*)pebBase + 0xBC);
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
	return flag;
}
inline bool checkNtGlobalFlag() {
	static std::wstring ntdllStr = OBSWS1(L"ntdll.dll");
	HMODULE ntdll = GetModuleHandleW(ntdllStr.c_str());
	if (!ntdll) return false;
	typedef NTSTATUS(NTAPI* PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	auto pfn = (PFN_NtQueryInformationProcess)GetProcAddress(ntdll, OBS1("NtQueryInformationProcess").c_str());
	if (!pfn) return false;
	DWORD_PTR debugPort = 0;
	NTSTATUS status = pfn(GetCurrentProcess(), 7, &debugPort, sizeof(debugPort), nullptr);
	if (NT_SUCCESS(status) && debugPort != 0) return true;
	HANDLE debugObjHandle = nullptr;
	status = pfn(GetCurrentProcess(), 30, &debugObjHandle, sizeof(debugObjHandle), nullptr);
	if (NT_SUCCESS(status) && debugObjHandle != nullptr) return true;
	DWORD debugFlags = 0;
	status = pfn(GetCurrentProcess(), 31, &debugFlags, sizeof(debugFlags), nullptr);
	if (NT_SUCCESS(status) && debugFlags == 0) return true;
	typedef struct _PBI1 { NTSTATUS es; PVOID peb; ULONG_PTR am; LONG bp; ULONG_PTR pid; ULONG_PTR ppid; } PBI1;
	PBI1 pbi = {};
	status = pfn(GetCurrentProcess(), 0, &pbi, sizeof(pbi), nullptr);
	if (!NT_SUCCESS(status) || !pbi.peb) return false;
	DWORD ntGlobalFlag = readNtGlobalFlag(pbi.peb);
	if ((ntGlobalFlag & 0x70) != 0) return true;
	return false;
}
inline bool checkDebugHooks() {
	static std::wstring ntdllStr = OBSWS2(L"ntdll.dll");
	HMODULE ntdll = GetModuleHandleW(ntdllStr.c_str());
	if (!ntdll) return false;
	static std::wstring kernel32Str = OBSWS1(L"kernel32.dll");
	HMODULE kernel32 = GetModuleHandleW(kernel32Str.c_str());
	if (!kernel32) return false;
	FARPROC pIsDebug = GetProcAddress(kernel32, OBS2("IsDebuggerPresent").c_str());
	if (!pIsDebug) return false;
	BYTE code[2];
	memcpy(code, pIsDebug, 2);
	if (code[0] == 0xEB || code[0] == 0xE9 || code[0] == 0x90) return true;
	return false;
}
inline bool checkHardwareBreakpoints() {
	CONTEXT ctx = {};
	ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if (GetThreadContext(GetCurrentThread(), &ctx)) {
		if (ctx.Dr0 != 0 || ctx.Dr1 != 0 || ctx.Dr2 != 0 || ctx.Dr3 != 0) return true;
	}
	return false;
}
inline bool checkThreadHideFromDebugger() {
	static std::wstring ntdllStr = OBSWS3(L"ntdll.dll");
	HMODULE ntdll = GetModuleHandleW(ntdllStr.c_str());
	if (!ntdll) return false;
	typedef NTSTATUS(NTAPI* PFN_NtSetInformationThread)(HANDLE, ULONG, PVOID, ULONG);
	auto pfn = (PFN_NtSetInformationThread)GetProcAddress(ntdll, OBS3("NtSetInformationThread").c_str());
	if (!pfn) return false;
	NTSTATUS status = pfn(GetCurrentThread(), 0x11, nullptr, 0);
	return !NT_SUCCESS(status);
}
// ========== 增强RDTSC检测（多次采样取中位数） ==========
inline bool checkRdtscTiming() {
	unsigned long long deltas[5] = {};
	for (int s = 0; s < 5; ++s) {
		unsigned long long tsc1 = __rdtsc();
		volatile int dummy = 0;
		for (int i = 0; i < 10; ++i) dummy += i;
		unsigned long long tsc2 = __rdtsc();
		deltas[s] = tsc2 - tsc1;
	}
	for (int i = 0; i < 4; ++i) for (int j = i + 1; j < 5; ++j) if (deltas[i] > deltas[j]) { auto t = deltas[i]; deltas[i] = deltas[j]; deltas[j] = t; }
	if (deltas[2] > 1000000) return true;
	return false;
}
// ========== 增强父进程检测（扩展黑名单+进程名比较） ==========
inline bool checkParentProcess() {
	static std::wstring ntdllStr = OBSWS4(L"ntdll.dll");
	HMODULE ntdll = GetModuleHandleW(ntdllStr.c_str());
	if (!ntdll) return false;
	typedef NTSTATUS(NTAPI* PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);
	auto pfn = (PFN_NtQueryInformationProcess)GetProcAddress(ntdll, OBS4("NtQueryInformationProcess").c_str());
	if (!pfn) return false;
	typedef struct _PBI2 { NTSTATUS es; PVOID peb; ULONG_PTR am; LONG bp; ULONG_PTR pid; ULONG_PTR ppid; } PBI2;
	PBI2 pbi = {};
	NTSTATUS status = pfn(GetCurrentProcess(), 0, &pbi, sizeof(pbi), nullptr);
	if (!NT_SUCCESS(status)) return false;
	DWORD parentPid = (DWORD)pbi.ppid;
	HANDLE hParent = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
	if (!hParent) return false;
	WCHAR parentName[MAX_PATH] = {};
	DWORD nameLen = MAX_PATH;
	bool suspicious = false;
	if (QueryFullProcessImageNameW(hParent, 0, parentName, &nameLen)) {
		std::wstring name(parentName);
		static const std::wstring b1 = OBSWS1(L"x64dbg"), b2 = OBSWS2(L"x32dbg"), b3 = OBSWS3(L"ollydbg");
		static const std::wstring b4 = OBSWS4(L"ida"), b5 = OBSWS1(L"IDA"), b6 = OBSWS2(L"cheatengine");
		static const std::wstring b7 = OBSWS3(L"Cheat Engine"), b8 = OBSWS4(L"processhacker"), b9 = OBSWS1(L"ProcessHacker");
		static const std::wstring b10 = OBSWS2(L"de4dot"), b11 = OBSWS3(L"dnSpy"), b12 = OBSWS4(L"Scylla");
		static const std::wstring b13 = OBSWS1(L"ScyllaHide"), b14 = OBSWS2(L"TitanEngine"), b15 = OBSWS3(L"ReClass");
		static const std::wstring b16 = OBSWS4(L"API Monitor"), b17 = OBSWS1(L"frida"), b18 = OBSWS2(L"Frida");
		static const std::wstring b19 = OBSWS3(L"x96dbg"), b20 = OBSWS4(L"Windbg"), b21 = OBSWS1(L"WinDbg");
		static const std::wstring b22 = OBSWS2(L"pestudio"), b23 = OBSWS3(L"die.exe"), b24 = OBSWS4(L"DIE.exe");
		static const std::wstring checks[] = {b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11,b12,b13,b14,b15,b16,b17,b18,b19,b20,b21,b22,b23,b24};
		for (int i = 0; i < 24; ++i) {
			if (name.find(checks[i]) != std::wstring::npos) { suspicious = true; break; }
		}
	}
	CloseHandle(hParent);
	return suspicious;
}
// ========== 运行中进程黑名单扫描（扩展） ==========
inline bool checkRunningDebuggers() {
	static std::string procNames[] = {
		OBS1("x64dbg.exe"), OBS2("x32dbg.exe"), OBS3("ollydbg.exe"),
		OBS4("ida.exe"), OBS5("ida64.exe"), OBS6("idaq.exe"), OBS7("idaq64.exe"),
		OBS8("cheatengine.exe"), OBS1("Cheat Engine.exe"),
		OBS2("processhacker.exe"), OBS3("ProcessHacker.exe"),
		OBS4("de4dot.exe"), OBS5("dnSpy.exe"), OBS6("Scylla.exe"),
		OBS7("x96dbg.exe"), OBS8("windbg.exe"),
		OBS1("pestudio.exe"), OBS2("die.exe"), OBS3("HxD.exe"),
		OBS4("API Monitor.exe"), OBS5("apimonitor-x64.exe"),
		OBS6("frida-server.exe"), OBS7("frida.exe"),
		OBS8("HttpDebugger.exe"), OBS1("Fiddler.exe"),
		OBS2("Charles.exe"), OBS3("Wireshark.exe")
	};
	const int nameCount = 28;
	wchar_t wNames[28][MAX_PATH] = {};
	for (int i = 0; i < nameCount; ++i) MultiByteToWideChar(CP_ACP, 0, procNames[i].c_str(), -1, wNames[i], MAX_PATH);
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	bool found = false;
	if (Process32FirstW(hSnap, &pe)) {
		do {
			for (int i = 0; i < nameCount; ++i) {
				if (_wcsicmp(pe.szExeFile, wNames[i]) == 0) { found = true; break; }
			}
			if (found) break;
		} while (Process32NextW(hSnap, &pe));
	}
	CloseHandle(hSnap);
	return found;
}
// ========== 虚拟机检测 ==========
inline bool checkVirtualMachine() {
	PIP_ADAPTER_INFO adapterInfo = nullptr;
	ULONG bufLen = 0;
	if (GetAdaptersInfo(nullptr, &bufLen) == ERROR_BUFFER_OVERFLOW) {
		adapterInfo = (PIP_ADAPTER_INFO)malloc(bufLen);
		if (adapterInfo && GetAdaptersInfo(adapterInfo, &bufLen) == ERROR_SUCCESS) {
			PIP_ADAPTER_INFO adapter = adapterInfo;
			while (adapter) {
				if (adapter->AddressLength >= 3) {
					BYTE b0 = adapter->Address[0], b1 = adapter->Address[1], b2 = adapter->Address[2];
					if ((b0 == 0x00 && b1 == 0x0C && b2 == 0x29) ||
						(b0 == 0x00 && b1 == 0x50 && b2 == 0x56) ||
						(b0 == 0x00 && b1 == 0x05 && b2 == 0x69) ||
						(b0 == 0x08 && b1 == 0x00 && b2 == 0x27)) {
						free(adapterInfo);
						return true;
					}
				}
				adapter = adapter->Next;
			}
		}
		if (adapterInfo) free(adapterInfo);
	}
	HKEY hKey = nullptr;
	static std::string vmwareKey = OBS1("SOFTWARE\\VMware, Inc.\\VMware Tools");
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, vmwareKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		RegCloseKey(hKey);
		return true;
	}
	static std::string vboxKey = OBS2("SOFTWARE\\Oracle\\VirtualBox Guest Additions");
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, vboxKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		RegCloseKey(hKey);
		return true;
	}
	static std::string vmProcNames[] = {
		OBS3("vmtoolsd.exe"), OBS4("vmwaretray.exe"),
		OBS1("vmwareuser.exe"), OBS2("VBoxService.exe"), OBS3("VBoxTray.exe")
	};
	wchar_t wNames[5][MAX_PATH] = {};
	for (int i = 0; i < 5; ++i) MultiByteToWideChar(CP_ACP, 0, vmProcNames[i].c_str(), -1, wNames[i], MAX_PATH);
	HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (hSnap == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	bool found = false;
	if (Process32FirstW(hSnap, &pe)) {
		do {
			for (int i = 0; i < 5; ++i) {
				if (_wcsicmp(pe.szExeFile, wNames[i]) == 0) { found = true; break; }
			}
			if (found) break;
		} while (Process32NextW(hSnap, &pe));
	}
	CloseHandle(hSnap);
	return found;
}
// ========== 内存完整性自校验（防止内存补丁） ==========
inline uint32_t computeCodeSectionCRC() {
	HMODULE hMod = GetModuleHandle(nullptr);
	if (!hMod) return 0;
	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)hMod;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE*)hMod + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
	PIMAGE_SECTION_HEADER sec = IMAGE_FIRST_SECTION(nt);
	BYTE* codeBase = nullptr;
	DWORD codeSize = 0;
	for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
		if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
			codeBase = (BYTE*)hMod + sec[i].VirtualAddress;
			codeSize = sec[i].Misc.VirtualSize;
			break;
		}
	}
	if (!codeBase || codeSize == 0 || codeSize > 100 * 1024 * 1024) return 0;
	return computeCRC(codeBase, codeSize);
}
inline bool checkCodeIntegrity() {
	static uint32_t expectedCRC = 0;
	static bool initialized = false;
	if (!initialized) {
		expectedCRC = computeCodeSectionCRC();
		initialized = true;
		return false;
	}
	uint32_t currentCRC = computeCodeSectionCRC();
	if (currentCRC != 0 && currentCRC != expectedCRC) return true;
	return false;
}
// ========== 反内存Dump ==========
inline bool checkMemoryDump() {
	HMODULE hMod = GetModuleHandle(nullptr);
	if (!hMod) return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (VirtualQuery(hMod, &mbi, sizeof(mbi)) == 0) return false;
	if (mbi.Protect == PAGE_NOACCESS || mbi.Protect == PAGE_GUARD) return true;
	return false;
}
// ========== 反API Hook检测（检测关键NT函数是否被Hook） ==========
inline bool checkApiHooks() {
	static std::wstring ntdllStr = OBSWS5(L"ntdll.dll");
	HMODULE ntdll = GetModuleHandleW(ntdllStr.c_str());
	if (!ntdll) return false;
	static std::string funcNames[] = {
		OBS1("NtQueryInformationProcess"), OBS2("NtSetInformationThread"),
		OBS3("NtClose"), OBS4("NtReadVirtualMemory")
	};
	for (int i = 0; i < 4; ++i) {
		FARPROC pfn = GetProcAddress(ntdll, funcNames[i].c_str());
		if (!pfn) continue;
		BYTE* ptr = (BYTE*)pfn;
		if (ptr[0] == 0xE9 || ptr[0] == 0xEB || (ptr[0] == 0xFF && ptr[1] == 0x25)) return true;
		if (ptr[0] == 0x90 && ptr[1] == 0x90) return true;
	}
	return false;
}
// ========== 控制流平坦化混淆宏（编译期生成随机switch分发器） ==========
#define OBF_BEGIN \
	{ volatile int _obf_state = __LINE__; \
		volatile int _obf_seed = (__LINE__ * 2654435761u) & 0x7FFFFFFF; \
		(void)_obf_seed; \
		switch(_obf_state) { case __LINE__:
#define OBF_STEP \
		_obf_state = (__LINE__ ^ _obf_seed) & 0x7FFFFFFF; break; \
		case __LINE__:
#define OBF_END \
		break; } }
// ========== 轻量级调试检测（主线程安全，无系统调用开销） ==========
	inline bool isDebuggerDetectedLight() {
		bool result = false;
		if (checkDebuggerPresent()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		if (checkDebugHooks()) result = true;
		return result;
	}
	// ========== 综合检测入口（随机化检测顺序） ==========
	inline bool isDebuggerDetected() {
	bool result = false;
	unsigned long long tsc = __rdtsc();
	int order = (int)(tsc & 0x7);
	switch (order) {
	case 0:
		if (checkDebuggerPresent()) result = true;
		if (checkNtGlobalFlag()) result = true;
		if (checkDebugHooks()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		if (checkParentProcess()) result = true;
		if (checkRunningDebuggers()) result = true;
		break;
	case 1:
		if (checkHardwareBreakpoints()) result = true;
		if (checkParentProcess()) result = true;
		if (checkRunningDebuggers()) result = true;
		if (checkDebuggerPresent()) result = true;
		if (checkNtGlobalFlag()) result = true;
		if (checkDebugHooks()) result = true;
		break;
	case 2:
		if (checkRunningDebuggers()) result = true;
		if (checkNtGlobalFlag()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		if (checkDebugHooks()) result = true;
		if (checkParentProcess()) result = true;
		if (checkDebuggerPresent()) result = true;
		break;
	case 3:
		if (checkParentProcess()) result = true;
		if (checkDebugHooks()) result = true;
		if (checkDebuggerPresent()) result = true;
		if (checkRunningDebuggers()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		if (checkNtGlobalFlag()) result = true;
		break;
	case 4:
		if (checkNtGlobalFlag()) result = true;
		if (checkRunningDebuggers()) result = true;
		if (checkParentProcess()) result = true;
		if (checkDebuggerPresent()) result = true;
		if (checkDebugHooks()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		break;
	case 5:
		if (checkDebugHooks()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		if (checkDebuggerPresent()) result = true;
		if (checkNtGlobalFlag()) result = true;
		if (checkParentProcess()) result = true;
		if (checkRunningDebuggers()) result = true;
		break;
	default:
		if (checkDebuggerPresent()) result = true;
		if (checkRunningDebuggers()) result = true;
		if (checkHardwareBreakpoints()) result = true;
		if (checkNtGlobalFlag()) result = true;
		if (checkDebugHooks()) result = true;
		if (checkParentProcess()) result = true;
		break;
	}
	return result;
}
inline void hideThreadFromDebugger() {
	static std::wstring ntdllStr = OBSWS1(L"ntdll.dll");
	HMODULE ntdll = GetModuleHandleW(ntdllStr.c_str());
	if (!ntdll) return;
	typedef NTSTATUS(NTAPI* PFN_NtSetInformationThread)(HANDLE, ULONG, PVOID, ULONG);
	auto pfn = (PFN_NtSetInformationThread)GetProcAddress(ntdll, OBS5("NtSetInformationThread").c_str());
	if (pfn) pfn(GetCurrentThread(), 0x11, nullptr, 0);
}
// ========== 主线程快速检测（零开销，每帧可调用） ==========
	inline void runPeriodicCheck() {
		if (isDebuggerDetectedLight()) {
			TerminateProcess(GetCurrentProcess(), 0);
		}
	}
	// ========== 后台线程完整检测（重开销，仅后台线程调用） ==========
	inline void runFullCheck() {
		if (isDebuggerDetected()) {
			TerminateProcess(GetCurrentProcess(), 0);
		}
		if (checkCodeIntegrity()) {
			TerminateProcess(GetCurrentProcess(), 0);
		}
		if (checkApiHooks()) {
			TerminateProcess(GetCurrentProcess(), 0);
		}
	}
	}
