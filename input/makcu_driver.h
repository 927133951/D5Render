#pragma once
#include <string>
#include <vector>
#include <windows.h>
class MakcuDriver {
public:
	MakcuDriver();
	~MakcuDriver();
	bool connect(const std::string& port = "", int baudRate = 115200);
	void disconnect();
	bool isConnected() const { return m_connected; }
	void moveRelative(int dx, int dy);
	void moveAbsolute(int x, int y);
	void leftDown();
	void leftUp();
	void rightDown();
	void rightUp();
	void middleDown();
	void middleUp();
	void wheel(int delta);
	std::string getVersion();
	std::string getDeviceInfo();
	std::string getPortName() const { return m_portName; }
	int getBaudRate() const { return m_baudRate; }
	std::string getFirmwareVersion() const { return m_firmwareVersion; }
	static std::vector<std::string> enumeratePorts();
	static std::string autoDetect();
private:
	HANDLE m_hSerial = INVALID_HANDLE_VALUE;
	bool m_connected = false;
	std::string m_portName;
	int m_baudRate = 115200;
	std::string m_firmwareVersion;
	CRITICAL_SECTION m_lock;
	bool openPort(const std::string& port, int baudRate);
	void closePort();
	bool sendCommand(const std::string& cmd);
	std::string sendQuery(const std::string& cmd, int timeoutMs = 200);
	std::string readResponse(int timeoutMs = 200);
	void purgeBuffers();
	bool detectMakcu();
};
