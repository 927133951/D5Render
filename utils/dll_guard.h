#pragma once
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "utils/obfstr.h"
namespace dllguard {
inline uint32_t computeCRC32(const uint8_t* data, size_t length) {
	uint32_t crc = 0xFFFFFFFF;
	for (size_t i = 0; i < length; ++i) {
		crc ^= data[i];
		for (int j = 0; j < 8; ++j) {
			crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
		}
	}
	return ~crc;
}
inline bool verifyDLLSignature(const char* dllPath) {
	HANDLE hFile = CreateFileA(dllPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER fileSize;
	if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart > 64 * 1024 * 1024 || fileSize.QuadPart < 1024) {
		CloseHandle(hFile);
		return false;
	}
	size_t size = (size_t)fileSize.QuadPart;
	uint8_t* buf = (uint8_t*)VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!buf) { CloseHandle(hFile); return false; }
	DWORD bytesRead = 0;
	BOOL readOk = ReadFile(hFile, buf, (DWORD)size, &bytesRead, nullptr);
	CloseHandle(hFile);
	if (!readOk || bytesRead != size) { VirtualFree(buf, 0, MEM_RELEASE); return false; }
	bool valid = false;
	if (size >= 0x40) {
		uint32_t peOffset = *(uint32_t*)(buf + 0x3C);
		if (peOffset + 4 <= size) {
			uint32_t sig = *(uint32_t*)(buf + peOffset);
			if (sig == 0x00004550) {
				valid = true;
			}
		}
	}
	if (valid && size >= 0x80) {
		uint32_t peOffset = *(uint32_t*)(buf + 0x3C);
		// 越界检查: peOffset + 22 必须在文件范围内
		if (peOffset + 22 > size) { valid = false; }
		else {
			uint16_t optSize = *(uint16_t*)(buf + peOffset + 20);
			uint16_t numSections = *(uint16_t*)(buf + peOffset + 6);
			if (optSize < 64 || numSections == 0 || numSections > 96) valid = false;
		}
	}
	VirtualFree(buf, 0, MEM_RELEASE);
	return valid;
}
inline bool verifyDLLAuthenticode(const char* dllPath) {
	static std::wstring wintrustStr = OBSWS1(L"wintrust.dll");
	HMODULE wintrust = LoadLibraryExW(wintrustStr.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	// fail-close: 无法加载wintrust.dll则拒绝加载
	if (!wintrust) return false;
	typedef LONG(WINAPI* PFN_WinVerifyTrust)(HWND, GUID*, void*);
	auto pfnWVT = (PFN_WinVerifyTrust)GetProcAddress(wintrust, OBS1("WinVerifyTrust").c_str());
	// fail-close: 无法获取WinVerifyTrust则拒绝加载
	if (!pfnWVT) return false;
	int wPathLen = MultiByteToWideChar(CP_ACP, 0, dllPath, -1, nullptr, 0);
	if (wPathLen <= 0) return false;
	std::vector<wchar_t> wPath(wPathLen);
	MultiByteToWideChar(CP_ACP, 0, dllPath, -1, wPath.data(), wPathLen);
	GUID actionId = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	WINTRUST_FILE_INFO fi = {};
	fi.cbStruct = sizeof(fi);
	fi.pcwszFilePath = wPath.data();
	WINTRUST_DATA wd = {};
	wd.cbStruct = sizeof(wd);
	wd.dwUIChoice = WTD_UI_NONE;
	wd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wd.dwUnionChoice = WTD_CHOICE_FILE;
	wd.pFile = &fi;
	LONG status = pfnWVT(nullptr, &actionId, &wd);
	FreeLibrary(wintrust);
	return (status == 0);
}
inline HMODULE safeLoadLibrary(const char* path, bool checkAuthenticode = false) {
	if (!verifyDLLSignature(path)) return nullptr;
	if (checkAuthenticode && !verifyDLLAuthenticode(path)) return nullptr;
	return LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}
}
