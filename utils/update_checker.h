#pragma once
#include <string>
#include <functional>
#include <atomic>
#include <thread>
// 当前软件版本号（每次发版时修改这里）
#define D5_VERSION_MAJOR 1
#define D5_VERSION_MINOR 0
#define D5_VERSION_PATCH 9
#define D5_VERSION_STRING "1.0.9-v235"
struct UpdateInfo {
	bool hasUpdate = false;
	std::string latestVersion;
	std::string downloadUrl;
	std::string releaseNotes;
};
class UpdateChecker {
public:
	// 异步检查更新，结果通过callback返回（在调用线程执行callback）
	void checkAsync(std::function<void(const UpdateInfo&)> callback);
	// 同步检查更新（阻塞调用）
	UpdateInfo checkSync();
	// 比较版本号：v1 < v2返回-1，相等返回0，v1 > v2返回1
	static int compareVersions(const std::string& v1, const std::string& v2);
	// 获取当前版本号
	static std::string getCurrentVersion() { return D5_VERSION_STRING; }
private:
	std::atomic<bool> m_checking{false};
};
