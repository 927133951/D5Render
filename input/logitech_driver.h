#pragma once
#include <string>
#include <cstdint>
#include <Windows.h>
#include "input/makcu_driver.h"
enum class MouseDrvType { MDT_None = 0, MDT_GHUB = 1, MDT_SendInput = 2, MDT_Makcu = 3 };
class LogitechDriver {
public:
	LogitechDriver();
	~LogitechDriver();
	bool initialize(MouseDrvType preferredType = MouseDrvType::MDT_None, const std::string& makcuPort = "", int makcuBaud = 115200);
	void release();
	void moveRelative(int dx, int dy);
	void clickLeft();
	void leftDown();
	void leftDownHold(); // 持续重发按下，不刷新s_ignorePhysUntil
	void leftUp();
	bool isAvailable() const { return m_available; }
	std::string getMethodName() const { return m_methodName; }
	MouseDrvType getCurrentType() const { return m_currentType; }
	std::string getStatusMessage() const { return m_statusMessage; }
	MakcuDriver& getMakcuDriver() { return m_makcuDriver; }
	const MakcuDriver& getMakcuDriver() const { return m_makcuDriver; }
	static std::string GetTypeName(MouseDrvType t);
private:
	bool m_available = false;
	MouseDrvType m_currentType = MouseDrvType::MDT_None;
	std::string m_methodName;
	std::string m_statusMessage;
	bool loadGHUBDriver();
	bool tryLoadFromPath(const char* fullPath);
	void moveViaSendInput(int dx, int dy);
	bool loadMakcuDriver(const std::string& port, int baud);
	HMODULE m_ghubDll = nullptr;
	typedef int (*MoveRFunc)(int, int);
	typedef int (*OpenFunc)();
	typedef int (*CloseFunc)();
	typedef void (*KeyDownFunc)(int);
	typedef void (*KeyUpFunc)(int);
	typedef void (*MouseDownFunc)(int);
	typedef void (*MouseUpFunc)(int);
	typedef void (*ScrollFunc)(int);
	MoveRFunc m_moveR = nullptr;
	OpenFunc m_deviceOpen = nullptr;
	CloseFunc m_deviceClose = nullptr;
	KeyDownFunc m_keyDown = nullptr;
	KeyUpFunc m_keyUp = nullptr;
	MouseDownFunc m_mouseDown = nullptr;
	MouseUpFunc m_mouseUp = nullptr;
	ScrollFunc m_scroll = nullptr;
	bool m_deviceOpened = false;
	MakcuDriver m_makcuDriver;
};
