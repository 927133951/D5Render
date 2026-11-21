#include "makcu_driver.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#pragma comment(lib, "setupapi.lib")
MakcuDriver::MakcuDriver() {
	InitializeCriticalSection(&m_lock);
}
MakcuDriver::~MakcuDriver() {
	disconnect();
	DeleteCriticalSection(&m_lock);
}
std::vector<std::string> MakcuDriver::enumeratePorts() {
	std::vector<std::string> ports;
	HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_DEVCLASS_PORTS, nullptr, nullptr, DIGCF_PRESENT);
	if (hDevInfo == INVALID_HANDLE_VALUE) return ports;
	SP_DEVINFO_DATA devInfo = {};
	devInfo.cbSize = sizeof(devInfo);
	for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfo); ++i) {
		char friendlyName[256] = {};
		if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &devInfo, SPDRP_FRIENDLYNAME, nullptr, (PBYTE)friendlyName, sizeof(friendlyName), nullptr)) {
			// 从友好名称提取COM端口号，如"Communications Port (COM3)"
			const char* p1 = strstr(friendlyName, "(COM");
			const char* p2 = strstr(friendlyName, ")");
			if (p1 && p2 && p2 > p1) {
				int comNum = atoi(p1 + 4);
				if (comNum > 0) {
					ports.push_back("COM" + std::to_string(comNum));
				}
			}
		}
	}
	SetupDiDestroyDeviceInfoList(hDevInfo);
	return ports;
}
bool MakcuDriver::openPort(const std::string& port, int baudRate) {
	std::string comPart = port;
	if (comPart.find("COM") == 0) comPart = comPart.substr(3);
	std::string fullPath = "\\\\.\\COM" + comPart;
	m_hSerial = CreateFileA(fullPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
	if (m_hSerial == INVALID_HANDLE_VALUE) return false;
	DCB dcb = {};
	dcb.DCBlength = sizeof(dcb);
	if (!GetCommState(m_hSerial, &dcb)) { closePort(); return false; }
	dcb.BaudRate = baudRate;
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = ONESTOPBIT;
	dcb.fDtrControl = DTR_CONTROL_ENABLE;
	dcb.fRtsControl = RTS_CONTROL_ENABLE;
	dcb.fOutX = FALSE;
	dcb.fInX = FALSE;
	dcb.fNull = FALSE;
	if (!SetCommState(m_hSerial, &dcb)) { closePort(); return false; }
	COMMTIMEOUTS timeouts = {};
	timeouts.ReadIntervalTimeout = 10;
	timeouts.ReadTotalTimeoutMultiplier = 1;
	timeouts.ReadTotalTimeoutConstant = 100;
	timeouts.WriteTotalTimeoutMultiplier = 1;
	timeouts.WriteTotalTimeoutConstant = 100;
	if (!SetCommTimeouts(m_hSerial, &timeouts)) { closePort(); return false; }
	purgeBuffers();
	return true;
}
void MakcuDriver::closePort() {
	if (m_hSerial != INVALID_HANDLE_VALUE) {
		CloseHandle(m_hSerial);
		m_hSerial = INVALID_HANDLE_VALUE;
	}
}
void MakcuDriver::purgeBuffers() {
	if (m_hSerial != INVALID_HANDLE_VALUE) {
		PurgeComm(m_hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
	}
}
bool MakcuDriver::sendCommand(const std::string& cmd) {
	if (!m_connected) return false;
	EnterCriticalSection(&m_lock);
	std::string fullCmd = cmd + "\r\n";
	DWORD written = 0;
	BOOL ok = WriteFile(m_hSerial, fullCmd.c_str(), (DWORD)fullCmd.size(), &written, NULL);
	if (!ok || written != fullCmd.size()) {
		m_connected = false;
		LeaveCriticalSection(&m_lock);
		LOG_ERROR(OBS1(u8"Makcu串口写入失败，连接断开").c_str());
		return false;
	}
	LeaveCriticalSection(&m_lock);
	return true;
}
std::string MakcuDriver::readResponse(int timeoutMs) {
	if (m_hSerial == INVALID_HANDLE_VALUE) return "";
	char buf[512] = {};
	DWORD totalRead = 0;
	ULONGLONG startTime = GetTickCount64();
	while ((int)(GetTickCount64() - startTime) < timeoutMs && totalRead < sizeof(buf) - 1) {
		DWORD bytesRead = 0;
		if (ReadFile(m_hSerial, buf + totalRead, 1, &bytesRead, NULL) && bytesRead > 0) {
			totalRead += bytesRead;
			buf[totalRead] = '\0';
			if (totalRead >= 2 && buf[totalRead - 2] == '\r' && buf[totalRead - 1] == '\n') break;
		}
	}
	buf[totalRead] = '\0';
	return std::string(buf);
}
std::string MakcuDriver::sendQuery(const std::string& cmd, int timeoutMs) {
	if (!m_connected) return "";
	EnterCriticalSection(&m_lock);
	purgeBuffers();
	std::string fullCmd = cmd + "\r\n";
	DWORD written = 0;
	WriteFile(m_hSerial, fullCmd.c_str(), (DWORD)fullCmd.size(), &written, NULL);
	std::string response = readResponse(timeoutMs);
	LeaveCriticalSection(&m_lock);
	return response;
}
bool MakcuDriver::detectMakcu() {
	purgeBuffers();
	Sleep(50);
	purgeBuffers();
	std::string ver = sendQuery(OBS2(".version()"), 300);
	if (ver.find(OBS4("km.")) != std::string::npos || ver.find(OBS5("makcu")) != std::string::npos || ver.find(OBS6("MAKCU")) != std::string::npos) {
		size_t kmPos = ver.find(OBS4("km."));
		if (kmPos != std::string::npos) {
			m_firmwareVersion = ver.substr(kmPos + 3);
			size_t crPos = m_firmwareVersion.find('\r');
			if (crPos != std::string::npos) m_firmwareVersion = m_firmwareVersion.substr(0, crPos);
			size_t nlPos = m_firmwareVersion.find('\n');
			if (nlPos != std::string::npos) m_firmwareVersion = m_firmwareVersion.substr(0, nlPos);
			while (!m_firmwareVersion.empty() && (m_firmwareVersion.back() == ' ' || m_firmwareVersion.back() == '\r' || m_firmwareVersion.back() == '\n')) {
				m_firmwareVersion.pop_back();
			}
		}
		return true;
	}
	std::string info = sendQuery(OBS3(".info()"), 300);
	if (info.find(OBS4("km.")) != std::string::npos || info.find(OBS5("makcu")) != std::string::npos || info.find(OBS6("MAKCU")) != std::string::npos) {
		m_firmwareVersion = OBS7("detected");
		return true;
	}
	return false;
}
std::string MakcuDriver::autoDetect() {
	auto ports = enumeratePorts();
	for (const auto& port : ports) {
		MakcuDriver testDriver;
		if (testDriver.openPort(port, 115200)) {
			if (testDriver.detectMakcu()) {
				testDriver.closePort();
				return port;
			}
			testDriver.closePort();
		}
		Sleep(10);
	}
	return "";
}
bool MakcuDriver::connect(const std::string& port, int baudRate) {
	EnterCriticalSection(&m_lock);
	disconnect();
	m_baudRate = baudRate;
	std::string targetPort = port;
	if (targetPort.empty()) {
		LeaveCriticalSection(&m_lock);
		targetPort = autoDetect();
		if (targetPort.empty()) return false;
		EnterCriticalSection(&m_lock);
	}
	m_portName = targetPort;
	if (!openPort(targetPort, baudRate)) {
		LeaveCriticalSection(&m_lock);
		LOG_ERROR(OBS4(u8"Makcu串口打开失败").c_str());
		return false;
	}
	m_connected = true;
	if (!detectMakcu()) {
		LeaveCriticalSection(&m_lock);
		disconnect();
		LOG_WARN(OBS5(u8"Makcu设备检测失败，非Makcu设备").c_str());
		return false;
	}
	LeaveCriticalSection(&m_lock);
	LOG_INFO(OBS6(u8"Makcu盒子连接成功 - 固件:%s").c_str(), m_firmwareVersion.c_str());
	return true;
}
void MakcuDriver::disconnect() {
	EnterCriticalSection(&m_lock);
	if (m_connected) {
		sendCommand(OBS7(".release()"));
	}
	closePort();
	m_connected = false;
	m_portName = "";
	m_firmwareVersion = "";
	LeaveCriticalSection(&m_lock);
}
void MakcuDriver::moveRelative(int dx, int dy) {
	if (!m_connected) return;
	char cmd[64];
	snprintf(cmd, sizeof(cmd), OBS8(".move(%d,%d)").c_str(), dx, dy);
	sendCommand(cmd);
}
void MakcuDriver::moveAbsolute(int x, int y) {
	if (!m_connected) return;
	char cmd[64];
	snprintf(cmd, sizeof(cmd), OBS1(".moveto(%d,%d)").c_str(), x, y);
	sendCommand(cmd);
}
void MakcuDriver::leftDown() {
	if (!m_connected) return;
	sendCommand(OBS2(".left(1)"));
}
void MakcuDriver::leftUp() {
	if (!m_connected) return;
	sendCommand(OBS3(".left(0)"));
}
void MakcuDriver::rightDown() {
	if (!m_connected) return;
	sendCommand(OBS4(".right(1)"));
}
void MakcuDriver::rightUp() {
	if (!m_connected) return;
	sendCommand(OBS5(".right(0)"));
}
void MakcuDriver::middleDown() {
	if (!m_connected) return;
	sendCommand(OBS6(".middle(1)"));
}
void MakcuDriver::middleUp() {
	if (!m_connected) return;
	sendCommand(OBS7(".middle(0)"));
}
void MakcuDriver::wheel(int delta) {
	if (!m_connected) return;
	char cmd[32];
	int d = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
	snprintf(cmd, sizeof(cmd), OBS8(".wheel(%d)").c_str(), d);
	sendCommand(cmd);
}
std::string MakcuDriver::getVersion() {
	if (!m_connected) return "";
	return sendQuery(OBS1(".version()"), 300);
}
std::string MakcuDriver::getDeviceInfo() {
	if (!m_connected) return "";
	return sendQuery(OBS2(".info()"), 300);
}
