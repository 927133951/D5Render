#include "gui/app.h"
#include "utils/obfstr.h"
#include <Windows.h>
#include <dbghelp.h>
#include <psapi.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
	FILE* f = nullptr;
	fopen_s(&f, "crash.log", "a");
	if (f) {
		fseek(f, 0, SEEK_END);
		fprintf(f, "=== Crash ===\n");
		fprintf(f, "Code: 0x%08X\n", ep->ExceptionRecord->ExceptionCode);
		fprintf(f, "Address: 0x%016llX\n", (unsigned long long)ep->ExceptionRecord->ExceptionAddress);
		if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
			fprintf(f, "Access Violation at: 0x%016llX\n", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
		}
		CONTEXT* ctx = ep->ContextRecord;
		fprintf(f, "RIP: 0x%016llX RSP: 0x%016llX\n", (unsigned long long)ctx->Rip, (unsigned long long)ctx->Rsp);
		fprintf(f, "RAX: 0x%016llX RBX: 0x%016llX RCX: 0x%016llX\n", (unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx);
		fprintf(f, "RDX: 0x%016llX RSI: 0x%016llX RDI: 0x%016llX\n", (unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi);
		MODULEINFO mi = {};
		HMODULE hMod = nullptr;
		DWORD pid = GetCurrentProcessId();
		HANDLE hProc = GetCurrentProcess();
		HMODULE modules[1024];
		DWORD needed = 0;
		if (EnumProcessModules(hProc, modules, sizeof(modules), &needed)) {
			DWORD64 crashAddr = (DWORD64)ep->ExceptionRecord->ExceptionAddress;
			for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
				if (GetModuleInformation(hProc, modules[i], &mi, sizeof(mi))) {
					DWORD64 start = (DWORD64)mi.lpBaseOfDll;
					DWORD64 end = start + mi.SizeOfImage;
					if (crashAddr >= start && crashAddr < end) {
						char modName[MAX_PATH];
						GetModuleFileNameA(modules[i], modName, MAX_PATH);
						fprintf(f, "Module: %s\n", modName);
						fprintf(f, "RVA: 0x%016llX\n", (unsigned long long)(crashAddr - start));
						fprintf(f, "Base: 0x%016llX Size: 0x%X\n", (unsigned long long)start, mi.SizeOfImage);
						hMod = modules[i];
						break;
					}
				}
			}
		}
		if (hMod) {
			SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
			SymInitialize(hProc, nullptr, TRUE);
			DWORD64 disp = 0;
			IMAGEHLP_SYMBOL64 sym;
			memset(&sym, 0, sizeof(sym));
			sym.SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
			sym.MaxNameLength = MAX_SYM_NAME;
			if (SymGetSymFromAddr64(hProc, (DWORD64)ep->ExceptionRecord->ExceptionAddress, &disp, &sym)) {
				fprintf(f, "Symbol: %s+%llu\n", sym.Name, (unsigned long long)disp);
			} else {
				fprintf(f, "SymGetSymFromAddr64 failed: %d\n", GetLastError());
			}
			IMAGEHLP_LINE64 line;
			memset(&line, 0, sizeof(line));
			line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
			DWORD lineDisp = 0;
			if (SymGetLineFromAddr64(hProc, (DWORD64)ep->ExceptionRecord->ExceptionAddress, &lineDisp, &line)) {
				fprintf(f, "File: %s Line: %d\n", line.FileName, line.LineNumber);
			}
			SymCleanup(hProc);
		}
		fprintf(f, "\n");
		fclose(f);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
	SetProcessDPIAware();
	SetUnhandledExceptionFilter(CrashHandler);
	App app;
	if (!app.initialize(hInstance)) {
		MessageBoxW(nullptr, OBSWS1(L"Initialization failed. Check CUDA/TensorRT environment.").c_str(), OBSWS2(L"D5 Render - Error").c_str(), MB_OK | MB_ICONERROR);
		return 1;
	}
	app.run();
	app.shutdown();
	return 0;
}
