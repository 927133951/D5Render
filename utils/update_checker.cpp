#include "update_checker.h"
#include "obfstr.h"
#include <windows.h>
#include <winhttp.h>
#include <cstring>
#include <cstdlib>
// 从JSON字符串中提取指定key的值（简单解析，无需第三方JSON库）
static std::string extractJsonValue(const std::string& json, const std::string& key) {
	std::string searchKey = "\"" + key + "\"";
	size_t pos = json.find(searchKey);
	if (pos == std::string::npos) return "";
	pos += searchKey.size();
	// 跳过空白和冒号
	while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == ':')) pos++;
	if (pos >= json.size()) return "";
	if (json[pos] == '"') {
		pos++;
		std::string result;
		while (pos < json.size() && json[pos] != '"') {
			if (json[pos] == '\\' && pos + 1 < json.size()) {
				pos++;
				switch (json[pos]) {
					case 'n': result += '\n'; break;
					case 'r': result += '\r'; break;
					case 't': result += '\t'; break;
					case '"': result += '"'; break;
					case '\\': result += '\\'; break;
					case '/': result += '/'; break;
					case 'b': result += '\b'; break;
					case 'f': result += '\f'; break;
					case 'u': {
						if (pos + 4 < json.size()) {
							char hex[5] = { json[pos+1], json[pos+2], json[pos+3], json[pos+4], 0 };
							unsigned int cp = (unsigned int)strtol(hex, nullptr, 16);
							pos += 4;
							if (cp < 0x80) {
								result += (char)cp;
							} else if (cp < 0x800) {
								result += (char)(0xC0 | (cp >> 6));
								result += (char)(0x80 | (cp & 0x3F));
							} else {
								result += (char)(0xE0 | (cp >> 12));
								result += (char)(0x80 | ((cp >> 6) & 0x3F));
								result += (char)(0x80 | (cp & 0x3F));
							}
						}
						break;
					}
					default: result += json[pos]; break;
				}
				pos++;
			} else {
				result += json[pos];
				pos++;
			}
		}
		return result;
	} else if (json[pos] == '[' || json[pos] == '{') {
		// 数组或对象，提取到匹配的结束符
		char open = json[pos], close = (open == '[') ? ']' : '}';
		int depth = 1;
		size_t end = pos + 1;
		while (end < json.size() && depth > 0) {
			if (json[end] == open) depth++;
			else if (json[end] == close) depth--;
			else if (json[end] == '\\') end++;
			end++;
		}
		return json.substr(pos, end - pos);
	} else {
		// 数字或布尔值
		size_t end = pos;
		while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']') end++;
		std::string val = json.substr(pos, end - pos);
		// 去除首尾空白
		while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\n' || val.back() == '\r')) val.pop_back();
		return val;
	}
}
// URL解码（处理中文等非ASCII字符）
static std::string urlDecode(const std::string& str) {
	std::string result;
	for (size_t i = 0; i < str.size(); ++i) {
		if (str[i] == '%' && i + 2 < str.size()) {
			char hex[3] = { str[i + 1], str[i + 2], 0 };
			result += (char)strtol(hex, nullptr, 16);
			i += 2;
		} else if (str[i] == '+') {
			result += ' ';
		} else {
			result += str[i];
		}
	}
	return result;
}
UpdateInfo UpdateChecker::checkSync() {
	UpdateInfo info;
	info.hasUpdate = false;
	HINTERNET hSession = WinHttpOpen(L"D5Render/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (!hSession) return info;
	HINTERNET hConnect = WinHttpConnect(hSession, L"gitee.com", 443, 0);
	if (!hConnect) { WinHttpCloseHandle(hSession); return info; }
	// Gitee API: 获取最新Release
	static std::wstring apiPath = OBSWS1(L"/api/v5/repos/wdg927/d5-render-update/releases/latest");
	HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", apiPath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return info; }
	// 设置超时：连接5秒，请求10秒
	DWORD timeout = 5000;
	WinHttpSetOption(hRequest, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
	timeout = 10000;
	WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
	BOOL bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
	if (!bResult) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return info; }
	bResult = WinHttpReceiveResponse(hRequest, nullptr);
	if (!bResult) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return info; }
	// 检查HTTP状态码
	DWORD statusCode = 0;
	DWORD statusCodeSize = sizeof(statusCode);
	WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &statusCode, &statusCodeSize, nullptr);
	if (statusCode != 200) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return info; }
	// 读取响应体
	std::string responseBody;
	DWORD bytesAvailable = 0;
	while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
		std::vector<char> buffer(bytesAvailable + 1);
		DWORD bytesRead = 0;
		if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
			responseBody.append(buffer.data(), bytesRead);
		}
	}
	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);
	// 解析JSON响应
	std::string tagName = extractJsonValue(responseBody, "tag_name");
	std::string body = extractJsonValue(responseBody, "body");
	// 去掉tag_name中的v前缀
	std::string remoteVersion = tagName;
	if (!remoteVersion.empty() && (remoteVersion[0] == 'v' || remoteVersion[0] == 'V')) {
		remoteVersion = remoteVersion.substr(1);
	}
	if (remoteVersion.empty()) return info;
	// 比较版本号
	int cmp = compareVersions(D5_VERSION_STRING, remoteVersion);
	if (cmp < 0) {
		info.hasUpdate = true;
		info.latestVersion = remoteVersion;
		info.releaseNotes = urlDecode(body);
		// 下载链接固定为夸克网盘，不从Gitee release body解析
		info.downloadUrl = "https://pan.quark.cn/s/0777e734f179";
	}
	return info;
}
void UpdateChecker::checkAsync(std::function<void(const UpdateInfo&)> callback) {
	if (m_checking.load()) return;
	m_checking.store(true);
	std::thread([this, callback]() {
		UpdateInfo info = checkSync();
		m_checking.store(false);
		if (callback) callback(info);
	}).detach();
}
int UpdateChecker::compareVersions(const std::string& v1, const std::string& v2) {
	// 解析版本号为数字数组
	auto parseVersion = [](const std::string& v) -> std::vector<int> {
		std::vector<int> parts;
		std::string num;
		for (char c : v) {
			if (c == '.') {
				if (!num.empty()) parts.push_back(std::atoi(num.c_str()));
				num.clear();
			} else if (c >= '0' && c <= '9') {
				num += c;
			}
		}
		if (!num.empty()) parts.push_back(std::atoi(num.c_str()));
		return parts;
	};
	auto p1 = parseVersion(v1);
	auto p2 = parseVersion(v2);
	size_t maxLen = std::max(p1.size(), p2.size());
	for (size_t i = 0; i < maxLen; ++i) {
		int n1 = (i < p1.size()) ? p1[i] : 0;
		int n2 = (i < p2.size()) ? p2[i] : 0;
		if (n1 < n2) return -1;
		if (n1 > n2) return 1;
	}
	return 0;
}
