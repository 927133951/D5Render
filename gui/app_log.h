#pragma once
#include <vector>
#include <deque>
#include <string>
#include <mutex>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdarg>
#include <atomic>
#include <windows.h>
#include "utils/obfstr.h"
struct LogEntry {
	std::string timestamp;
	std::string message;
	int level;
};
class AppLog {
public:
	static AppLog& instance() {
		static AppLog inst;
		return inst;
	}
	void addLog(int level, const char* fmt, ...) {
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		std::string msg(buf);
		// 敏感信息过滤: 移除路径和坐标细节
		filterSensitive(msg);
		auto now = std::chrono::system_clock::now();
		auto timeT = std::chrono::system_clock::to_time_t(now);
		struct tm tmBuf;
		localtime_s(&tmBuf, &timeT);
		char timeStr[32];
		strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmBuf);
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if ((int)m_entries.size() >= m_maxEntries) m_entries.pop_front();
			m_entries.push_back({std::string(timeStr), msg, level});
			m_dirty.store(true, std::memory_order_release);
		}
		// [v185] 日志落盘: exe目录d5render_log.txt, 启动时重置。UI日志无法导出,
		// 落盘后才能定位用户端的捕获初始化失败等环境问题
		appendFile(timeStr, msg, level);
	}
	const char* getLogFilePath() const { return "d5render_log.txt"; }
	void clear() {
		std::lock_guard<std::mutex> lock(m_mutex);
		m_entries.clear();
		m_dirty.store(true, std::memory_order_release);
	}
	std::vector<LogEntry> getEntriesSnapshot() const {
		std::lock_guard<std::mutex> lock(m_mutex);
		return std::vector<LogEntry>(m_entries.begin(), m_entries.end());
	}
	bool consumeDirty() { return m_dirty.exchange(false, std::memory_order_acq_rel); }
	void setAutoScroll(bool v) { m_autoScroll = v; }
	bool getAutoScroll() const { return m_autoScroll; }
private:
	AppLog() = default;
	// [v185] 追加写日志文件: exe目录d5render_log.txt(工作目录不固定, 必须绝对路径), 首次调用清空旧文件
	const char* logFilePath() {
		static char s_path[MAX_PATH] = {};
		if (s_path[0] == '\0') {
			GetModuleFileNameA(nullptr, s_path, MAX_PATH);
			char* slash = strrchr(s_path, '\\');
			if (slash) *(slash + 1) = '\0';
			strcat_s(s_path, "d5render_log.txt");
		}
		return s_path;
	}
	void appendFile(const std::string& timeStr, const std::string& msg, int level) {
		static bool s_reset = false;
		const char* path = logFilePath();
		if (!s_reset) {
			s_reset = true;
			FILE* f = nullptr;
			if (fopen_s(&f, path, "w") == 0 && f) {
				fprintf(f, "===== D5 Render 日志 =====\r\n");
				fclose(f);
			}
		}
		FILE* f = nullptr;
		if (fopen_s(&f, path, "a") == 0 && f) {
			const char* lv = level == 2 ? "[错误]" : (level == 1 ? "[警告]" : "[信息]");
			fprintf(f, "%s %s %s\r\n", timeStr.c_str(), lv, msg.c_str());
			fclose(f);
		}
	}
	void filterSensitive(std::string& msg) {
		// 过滤绝对路径，只保留文件名
		auto replacePath = [&](const std::string& prefix) {
			size_t pos = msg.find(prefix);
			while (pos != std::string::npos) {
				size_t start = pos + prefix.size();
				size_t end = msg.find_first_of(" \t\n\r)", start);
				if (end == std::string::npos) end = msg.size();
				std::string fullPath = msg.substr(start, end - start);
				size_t lastSlash = fullPath.find_last_of("\\/");
				if (lastSlash != std::string::npos) {
					msg.replace(start, end - start, fullPath.substr(lastSlash + 1));
				}
				pos = msg.find(prefix, pos + 1);
			}
		};
		replacePath(OBS1("路径:"));
		replacePath(OBS2("path:"));
		replacePath(OBS3("加载DLL失败: "));
	}
	std::deque<LogEntry> m_entries;
	mutable std::mutex m_mutex;
	int m_maxEntries = 500;
	std::atomic<bool> m_dirty{false};
	bool m_autoScroll = true;
};
#define LOG_INFO(fmt, ...)  AppLog::instance().addLog(0, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  AppLog::instance().addLog(1, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) AppLog::instance().addLog(2, fmt, ##__VA_ARGS__)
