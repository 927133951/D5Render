﻿#include "config.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdio>
static std::string kKeyNamesCache[] = {
	OBS1(u8"鼠标左键"), OBS2(u8"鼠标中键"), OBS3(u8"鼠标右键"), OBS4(u8"鼠标侧键1"), OBS5(u8"鼠标侧键2"),
	OBS6(u8"左Shift"), OBS7(u8"左Ctrl"), OBS8(u8"左Alt"),
	OBS1(u8"右Shift"), OBS2(u8"右Ctrl"), OBS3(u8"右Alt"),
	OBS4(u8"1"), OBS5(u8"2"), OBS6(u8"3"), OBS7(u8"4"), OBS8(u8"5"),
	OBS1(u8"6"), OBS2(u8"7"), OBS3(u8"8"), OBS4(u8"9"), OBS5(u8"0"),
	OBS6(u8"Q"), OBS7(u8"W"), OBS8(u8"E"), OBS1(u8"R"), OBS2(u8"T"),
	OBS3(u8"Y"), OBS4(u8"U"), OBS5(u8"I"), OBS6(u8"O"), OBS7(u8"P"),
	OBS8(u8"A"), OBS1(u8"S"), OBS2(u8"D"), OBS3(u8"F"), OBS4(u8"G"),
	OBS5(u8"H"), OBS6(u8"J"), OBS7(u8"K"), OBS8(u8"L"),
	OBS1(u8"Z"), OBS2(u8"X"), OBS3(u8"C"), OBS4(u8"V"), OBS5(u8"B"),
	OBS6(u8"N"), OBS7(u8"M"),
	OBS8(u8"Tab"), OBS1(u8"CapsLock"), OBS2(u8"Space"), OBS3(u8"Enter"), OBS4(u8"Backspace"),
	OBS5(u8"F1"), OBS6(u8"F2"), OBS7(u8"F3"), OBS8(u8"F4"),
	OBS1(u8"F5"), OBS2(u8"F6"), OBS3(u8"F7"), OBS4(u8"F8"),
	OBS5(u8"F9"), OBS6(u8"F10"), OBS7(u8"F11"), OBS8(u8"F12"),
	OBS1(u8"Ins"), OBS2(u8"Home"), OBS3(u8"End"), OBS4(u8"PgUp"), OBS5(u8"PgDn"),
	OBS6(u8"Scroll"), OBS7(u8"PrtSc"), OBS8(u8"Pause")
};
static const int kKeyCodes[] = {
	0x01, 0x04, 0x02, 0x05, 0x06,
	0xA0, 0xA2, 0xA4,
	0xA1, 0xA3, 0xA5,
	0x31, 0x32, 0x33, 0x34, 0x35,
	0x36, 0x37, 0x38, 0x39, 0x30,
	0x51, 0x57, 0x45, 0x52, 0x54,
	0x59, 0x55, 0x49, 0x4F, 0x50,
	0x41, 0x53, 0x44, 0x46, 0x47,
	0x48, 0x4A, 0x4B, 0x4C,
	0x5A, 0x58, 0x43, 0x56, 0x42,
	0x4E, 0x4D,
	0x09, 0x14, 0x20, 0x0D, 0x08,
	0x70, 0x71, 0x72, 0x73,
	0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7A, 0x7B,
	0x2D, 0x24, 0x23, 0x21, 0x22,
	0x91, 0x2C, 0x13
};
static const int kKeyCount = 72;
const char* AimKey::GetName(int vk) {
	for (int i = 0; i < kKeyCount; ++i) {
		if (kKeyCodes[i] == vk) return kKeyNamesCache[i].c_str();
	}
	static std::string unknown = OBS6(u8"未知按键");
	return unknown.c_str();
}
int AimKey::FromIndex(int idx) {
	if (idx >= 0 && idx < kKeyCount) return kKeyCodes[idx];
	return 0x04;
}
int AimKey::GetIndex(int vk) {
	for (int i = 0; i < kKeyCount; ++i) {
		if (kKeyCodes[i] == vk) return i;
	}
	return 1;
}
std::string D5RenderConfig::getConfigPath() const {
	char path[MAX_PATH];
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string p(path);
	auto pos = p.find_last_of("\\/");
	if (pos != std::string::npos) p = p.substr(0, pos + 1);
	p += OBS1("d5render_config.ini");
	return p;
}
static std::string trim(const std::string& s) {
	size_t start = s.find_first_not_of(" \t\r\n");
	size_t end = s.find_last_not_of(" \t\r\n");
	if (start == std::string::npos) return "";
	return s.substr(start, end - start + 1);
}
static const char b64Table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string encodeB64(const std::string& input) {
	std::string out;
	const uint8_t* d = (const uint8_t*)input.data();
	size_t len = input.size();
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = (uint32_t)d[i] << 16;
		if (i + 1 < len) n |= (uint32_t)d[i + 1] << 8;
		if (i + 2 < len) n |= (uint32_t)d[i + 2];
		out += b64Table[(n >> 18) & 0x3F];
		out += b64Table[(n >> 12) & 0x3F];
		out += (i + 1 < len) ? b64Table[(n >> 6) & 0x3F] : '=';
		out += (i + 2 < len) ? b64Table[n & 0x3F] : '=';
	}
	return out;
}
static inline int b64Val(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
static std::string decodeB64(const std::string& input) {
	std::string out;
	size_t len = input.size();
	if (len % 4 != 0) return out;
	for (size_t i = 0; i < len; i += 4) {
		int a = b64Val(input[i]), b = b64Val(input[i + 1]);
		int c = (input[i + 2] != '=') ? b64Val(input[i + 2]) : 0;
		int d = (input[i + 3] != '=') ? b64Val(input[i + 3]) : 0;
		if (a < 0 || b < 0) break;
		uint32_t n = (a << 18) | (b << 12) | (c << 6) | d;
		out += (char)((n >> 16) & 0xFF);
		if (input[i + 2] != '=') out += (char)((n >> 8) & 0xFF);
		if (input[i + 3] != '=') out += (char)(n & 0xFF);
	}
	return out;
}
static const uint8_t kValXorKey = 0x5A;
static std::string encVal(const std::string& raw) {
	std::string xored;
	xored.resize(raw.size());
	for (size_t i = 0; i < raw.size(); ++i)
		xored[i] = raw[i] ^ (kValXorKey ^ (uint8_t)(i & 0xFF));
	return encodeB64(xored);
}
static std::string decVal(const std::string& enc) {
	std::string xored = decodeB64(enc);
	std::string raw;
	raw.resize(xored.size());
	for (size_t i = 0; i < xored.size(); ++i)
		raw[i] = xored[i] ^ (kValXorKey ^ (uint8_t)(i & 0xFF));
	return raw;
}
static bool isLikelyB64(const std::string& s) {
	if (s.empty() || s.size() % 4 != 0) return false;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) return false;
	}
	return true;
}
static std::string encStr(const std::string& s) { return encVal(s); }
static std::string decStr(const std::string& s) {
	if (!isLikelyB64(s)) return s;
	std::string dec = decVal(s);
	if (dec.empty()) return s;
	return dec;
}
template<typename T>
static std::string encNum(T v) { return encVal(std::string((const char*)&v, sizeof(T))); }
template<typename T>
static T decNum(const std::string& s, T defVal) {
	if (!isLikelyB64(s)) {
		try {
			if constexpr (std::is_same_v<T, float>) return std::stof(s);
			else if constexpr (std::is_same_v<T, double>) return std::stod(s);
			else if constexpr (std::is_same_v<T, int32_t>) return std::stoi(s);
			else if constexpr (std::is_same_v<T, int64_t>) return std::stoll(s);
		} catch (...) { return defVal; }
	}
	std::string raw = decVal(s);
	if (raw.size() != sizeof(T)) return defVal;
	T v;
	memcpy(&v, raw.data(), sizeof(T));
	return v;
}
void D5RenderConfig::saveToFile() const {
	std::string path = getConfigPath();
	std::ofstream out(path);
	if (!out.is_open()) return;
	out << OBS7("[General]") << "\n";
	out << OBS2("model_path") << "=" << encStr(model_path) << "\n";
	out << OBS1("yolo_version") << "=" << encNum((int32_t)yolo_version) << "\n";
	out << OBS2("confidence_threshold") << "=" << encNum(confidence_threshold) << "\n";
	out << OBS3("nms_threshold") << "=" << encNum(nms_threshold) << "\n";
	out << OBS4("model_width") << "=" << encNum((int32_t)model_width) << "\n";
	out << OBS5("model_height") << "=" << encNum((int32_t)model_height) << "\n";
	out << OBS4("aim_smoothness") << "=" << encNum(aim_smoothness) << "\n";
	out << OBS5("aim_position") << "=" << encNum(aim_position) << "\n";
	out << OBS6("aim_position_x") << "=" << encNum(aim_position_x) << "\n";
	out << OBS6("aim_fov") << "=" << encNum(aim_fov) << "\n";
	out << OBS7("aim_humanize") << "=" << encNum((int32_t)(aim_humanize ? 1 : 0)) << "\n";
	out << OBS3("speed_multiplier") << "=" << encNum(speed_multiplier) << "\n";
	out << OBS4("track_match_threshold") << "=" << encNum(track_match_threshold) << "\n";
	out << OBS5("track_max_lost") << "=" << encNum((int32_t)track_max_lost) << "\n";
	out << OBS6("track_switch_hysteresis") << "=" << encNum(track_switch_hysteresis) << "\n";
	out << OBS7("track_confirm_frames") << "=" << encNum((int32_t)track_confirm_frames) << "\n";
	out << OBS7("kalman_process_noise") << "=" << encNum(kalman_process_noise) << "\n";
	out << OBS8("kalman_measurement_noise") << "=" << encNum(kalman_measurement_noise) << "\n";
	out << OBS2("kalman_enabled") << "=" << encNum((int32_t)(kalman_enabled ? 1 : 0)) << "\n";
	out << OBS3("scene_adapt_enabled") << "=" << encNum((int32_t)(scene_adapt_enabled ? 1 : 0)) << "\n";
	out << OBS4("aim_preset") << "=" << encNum((int32_t)aim_preset) << "\n";
	out << OBS5("makcu_port") << "=" << encStr(makcu_port) << "\n";
	out << OBS6("makcu_baud") << "=" << encNum((int32_t)makcu_baud) << "\n";
	out << OBS7("driver_mode") << "=" << encNum((int32_t)driver_mode) << "\n";
	out << OBS8("infer_mode") << "=" << encNum((int32_t)infer_mode) << "\n";
	out << OBS1("key_mode") << "=" << encNum((int32_t)key_mode) << "\n";
	out << OBS2("aim_key1") << "=" << encNum((int32_t)aim_key1.vkCode) << "\n";
	out << OBS3("aim_key2") << "=" << encNum((int32_t)aim_key2.vkCode) << "\n";
	out << OBS4("aim_key_slot_count") << "=" << encNum((int32_t)aim_key_slots.size()) << "\n";
	out << OBS5("capture_mode") << "=" << encNum((int32_t)capture_mode) << "\n";
	out << OBS6("max_detections") << "=" << encNum((int32_t)max_detections) << "\n";
	for (int i = 0; i < (int)aim_key_slots.size(); ++i) {
		char prefix[32];
		snprintf(prefix, sizeof(prefix), OBS5("aks%d_").c_str(), i);
		out << prefix << OBS6("vk") << "=" << encNum((int32_t)aim_key_slots[i].vkCode) << "\n";
		out << prefix << OBS7("mode") << "=" << encNum((int32_t)aim_key_slots[i].key_mode) << "\n";
		out << prefix << OBS8("combo_vk") << "=" << encNum((int32_t)aim_key_slots[i].combo_vkCode) << "\n";
	}
	out << OBS4("show_preview") << "=" << encNum((int32_t)(show_preview ? 1 : 0)) << "\n";
	out << OBS5("preview_x") << "=" << encNum((int32_t)preview_x) << "\n";
	out << OBS6("preview_y") << "=" << encNum((int32_t)preview_y) << "\n";
	out << OBS7("preview_w") << "=" << encNum((int32_t)preview_w) << "\n";
	out << OBS8("preview_h") << "=" << encNum((int32_t)preview_h) << "\n";
	out << OBS1("anti_capture") << "=" << encNum((int32_t)(anti_capture ? 1 : 0)) << "\n";
	out << OBS2("trt_precision") << "=" << encNum((int32_t)trt_precision) << "\n";
	out << OBS3("trt_mixed_precision") << "=" << encNum((int32_t)(trt_mixed_precision ? 1 : 0)) << "\n";
	out << OBS4("gpu_lock_clocks") << "=" << encNum((int32_t)(gpu_lock_clocks ? 1 : 0)) << "\n";
	out << OBS3("pid_kp") << "=" << encNum(pid_kp) << "\n";
	out << OBS4("pid_kp_y") << "=" << encNum(pid_kp_y) << "\n";
	out << OBS4("pid_ki") << "=" << encNum(pid_ki) << "\n";
	out << OBS5("pid_kd") << "=" << encNum(pid_kd) << "\n";
	out << OBS6("pid_deadzone") << "=" << encNum(pid_deadzone) << "\n";
	out << OBS7("move_randomize") << "=" << encNum((int32_t)(move_randomize ? 1 : 0)) << "\n";
	out << OBS8("move_max_step") << "=" << encNum(move_max_step) << "\n";
	out << OBS5("os_enabled") << "=" << encNum((int32_t)(os_enabled ? 1 : 0)) << "\n";
	out << OBS6("os_damp") << "=" << encNum(os_damp) << "\n";
	out << OBS7("os_base") << "=" << encNum(os_base) << "\n";
	out << OBS1("move_inertia") << "=" << encNum(move_inertia) << "\n";
	out << OBS2("move_jitter_range") << "=" << encNum(move_jitter_range) << "\n";
	out << OBS3("move_jitter_scale") << "=" << encNum(move_jitter_scale) << "\n";
	out << OBS4("move_accel_near") << "=" << encNum(move_accel_near) << "\n";
	out << OBS5("move_accel_far") << "=" << encNum(move_accel_far) << "\n";
	out << OBS6("move_fine_dist") << "=" << encNum(move_fine_dist) << "\n";
	out << OBS5("ff_enabled") << "=" << encNum((int32_t)(ff_enabled ? 1 : 0)) << "\n";
	out << OBS6("ff_gain") << "=" << encNum(ff_gain) << "\n";
	out << OBS7("ff_target_speed") << "=" << encNum(ff_target_speed) << "\n";
	out << OBS7("quickscope_enabled") << "=" << encNum((int32_t)(quickscope_enabled ? 1 : 0)) << "\n";
	out << OBS8("quickscope_key") << "=" << encNum((int32_t)quickscope_key.vkCode) << "\n";
	out << OBS4("quickscope_fire_delay") << "=" << encNum((int32_t)quickscope_fire_delay) << "\n";
	out << OBS1("auto_trigger_enabled") << "=" << encNum((int32_t)(auto_trigger_enabled ? 1 : 0)) << "\n";
	out << OBS2("auto_trigger_key") << "=" << encNum((int32_t)auto_trigger_key.vkCode) << "\n";
	out << OBS3("auto_trigger_delay_ms") << "=" << encNum((int32_t)auto_trigger_delay_ms) << "\n";
	out << OBS4("auto_trigger_threshold") << "=" << encNum(auto_trigger_threshold) << "\n";
	out << OBS5("auto_trigger_interval_ms") << "=" << encNum((int32_t)auto_trigger_interval_ms) << "\n";
	out << OBS7("auto_trigger_humanize") << "=" << encNum((int32_t)(auto_trigger_humanize ? 1 : 0)) << "\n";
	out << OBS1("custom_preset_idx") << "=" << encNum((int32_t)custom_preset_idx) << "\n";
	out << OBS2("aim_enabled") << "=" << encNum((int32_t)(aim_enabled ? 1 : 0)) << "\n";
		out << OBS3("aim_toggle_enabled") << "=" << encNum((int32_t)(aim_toggle_enabled ? 1 : 0)) << "\n";
		out << OBS7("toggle_sound") << "=" << encNum((int32_t)(toggle_sound ? 1 : 0)) << "\n";
		out << OBS4("aim_toggle_key_mode") << "=" << encNum((int32_t)aim_toggle_key_mode) << "\n";
		out << OBS5("aim_toggle_key") << "=" << encNum((int32_t)aim_toggle_key) << "\n";
		out << OBS6("aim_toggle_combo_key") << "=" << encNum((int32_t)aim_toggle_combo_key) << "\n";
		out << OBS7("ui_toggle_key") << "=" << encNum((int32_t)ui_toggle_key) << "\n";
		out << OBS3("gpu_id") << "=" << encNum((int32_t)gpu_id) << "\n";
		out << OBS3("infer_gpu_id") << "=" << encNum((int32_t)infer_gpu_id) << "\n";
		for (size_t i = 0; i < custom_presets.size(); ++i) {
		const auto& cp = custom_presets[i];
		out << OBS8("[CustomPreset_") << i << "]" << "\n";
		out << OBS4("cp_name") << "=" << encStr(cp.name) << "\n";
		out << OBS5("cp_aim_smoothness") << "=" << encNum(cp.aim_smoothness) << "\n";
		out << OBS6("cp_aim_position") << "=" << encNum(cp.aim_position) << "\n";
		out << OBS7("cp_aim_position_x") << "=" << encNum(cp.aim_position_x) << "\n";
		out << OBS7("cp_aim_fov") << "=" << encNum(cp.aim_fov) << "\n";
		out << OBS4("cp_speed_multiplier") << "=" << encNum(cp.speed_multiplier) << "\n";
		out << OBS5("cp_track_match_threshold") << "=" << encNum(cp.track_match_threshold) << "\n";
		out << OBS6("cp_track_max_lost") << "=" << encNum((int32_t)cp.track_max_lost) << "\n";
		out << OBS7("cp_track_switch_hysteresis") << "=" << encNum(cp.track_switch_hysteresis) << "\n";
		out << OBS7("cp_track_confirm_frames") << "=" << encNum((int32_t)cp.track_confirm_frames) << "\n";
		out << OBS8("cp_kalman_enabled") << "=" << encNum((int32_t)(cp.kalman_enabled ? 1 : 0)) << "\n";
		out << OBS1("cp_kalman_process_noise") << "=" << encNum(cp.kalman_process_noise) << "\n";
		out << OBS2("cp_kalman_measurement_noise") << "=" << encNum(cp.kalman_measurement_noise) << "\n";
		out << OBS4("cp_scene_adapt_enabled") << "=" << encNum((int32_t)(cp.scene_adapt_enabled ? 1 : 0)) << "\n";
		out << OBS3("cp_pid_kp") << "=" << encNum(cp.pid_kp) << "\n";
		out << OBS4("cp_pid_ki") << "=" << encNum(cp.pid_ki) << "\n";
		out << OBS5("cp_pid_kd") << "=" << encNum(cp.pid_kd) << "\n";
		out << OBS6("cp_pid_deadzone") << "=" << encNum(cp.pid_deadzone) << "\n";
		out << OBS7("cp_move_randomize") << "=" << encNum((int32_t)(cp.move_randomize ? 1 : 0)) << "\n";
		out << OBS8("cp_move_max_step") << "=" << encNum(cp.move_max_step) << "\n";
		out << OBS1("cp_move_inertia") << "=" << encNum(cp.move_inertia) << "\n";
		out << OBS2("cp_move_jitter_range") << "=" << encNum(cp.move_jitter_range) << "\n";
		out << OBS3("cp_move_jitter_scale") << "=" << encNum(cp.move_jitter_scale) << "\n";
		out << OBS4("cp_move_accel_near") << "=" << encNum(cp.move_accel_near) << "\n";
		out << OBS5("cp_move_accel_far") << "=" << encNum(cp.move_accel_far) << "\n";
		out << OBS6("cp_move_fine_dist") << "=" << encNum(cp.move_fine_dist) << "\n";
		out << OBS4("cp_ff_enabled") << "=" << encNum((int32_t)(cp.ff_enabled ? 1 : 0)) << "\n";
		out << OBS5("cp_ff_gain") << "=" << encNum(cp.ff_gain) << "\n";
		out << OBS6("cp_ff_target_speed") << "=" << encNum(cp.ff_target_speed) << "\n";
		out << OBS7("cp_quickscope_enabled") << "=" << encNum((int32_t)(cp.quickscope_enabled ? 1 : 0)) << "\n";
		out << OBS8("cp_quickscope_key") << "=" << encNum((int32_t)cp.quickscope_key.vkCode) << "\n";
		out << OBS4("cp_quickscope_fire_delay") << "=" << encNum((int32_t)cp.quickscope_fire_delay) << "\n";
		out << OBS1("cp_auto_trigger_enabled") << "=" << encNum((int32_t)(cp.auto_trigger_enabled ? 1 : 0)) << "\n";
		out << OBS2("cp_auto_trigger_key") << "=" << encNum((int32_t)cp.auto_trigger_key.vkCode) << "\n";
		out << OBS3("cp_auto_trigger_delay_ms") << "=" << encNum((int32_t)cp.auto_trigger_delay_ms) << "\n";
		out << OBS4("cp_auto_trigger_threshold") << "=" << encNum(cp.auto_trigger_threshold) << "\n";
		out << OBS5("cp_auto_trigger_interval_ms") << "=" << encNum((int32_t)cp.auto_trigger_interval_ms) << "\n";
		out << OBS7("cp_auto_trigger_humanize") << "=" << encNum((int32_t)(cp.auto_trigger_humanize ? 1 : 0)) << "\n";
	}
	if (!disabled_classes.empty()) {
		std::string dcStr;
		for (size_t i = 0; i < disabled_classes.size(); ++i) {
			if (i > 0) dcStr += ",";
			dcStr += std::to_string(disabled_classes[i]);
		}
		out << OBS3("disabled_classes") << "=" << encStr(dcStr) << "\n";
	}
	out << OBS4("per_class_aim_enabled") << "=" << encNum((int32_t)(per_class_aim_enabled ? 1 : 0)) << "\n";
	for (size_t i = 0; i < class_aim_positions.size(); ++i) {
		const auto& cap = class_aim_positions[i];
		out << OBS8("[ClassAimPos_") << i << "]" << "\n";
		out << OBS4("cap_class_id") << "=" << encNum((int32_t)cap.classId) << "\n";
		out << OBS5("cap_aim_position") << "=" << encNum(cap.aim_position) << "\n";
		out << OBS6("cap_aim_position_x") << "=" << encNum(cap.aim_position_x) << "\n";
		out << OBS7("cap_enabled") << "=" << encNum((int32_t)(cap.enabled ? 1 : 0)) << "\n";
	}
	out.close();
}
bool D5RenderConfig::loadFromFile() {
	std::string path = getConfigPath();
	if (!std::filesystem::exists(path)) return false;
	std::ifstream in(path);
	if (!in.is_open()) return false;
	std::string line;
	int m_loadingCpIdx = -1;
	int m_loadingCapIdx = -1;
	while (std::getline(in, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#') continue;
		if (line[0] == '[') {
			if (line.find(OBS8("[CustomPreset_")) == 0) {
				custom_presets.push_back(CustomPreset());
				m_loadingCpIdx = (int)custom_presets.size() - 1;
				m_loadingCapIdx = -1;
			} else if (line.find(OBS8("[ClassAimPos_")) == 0) {
				class_aim_positions.push_back(ClassAimPos());
				m_loadingCapIdx = (int)class_aim_positions.size() - 1;
				m_loadingCpIdx = -1;
			} else {
				m_loadingCpIdx = -1;
				m_loadingCapIdx = -1;
			}
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = trim(line.substr(0, eq));
		std::string val = trim(line.substr(eq + 1));
		try {
		if (key == OBS2("model_path")) model_path = decStr(val);
		if (key == OBS1("yolo_version")) { int v = decNum<int32_t>(val, 7); if (v < 0 || v > 7) v = 7; yolo_version = (YoloVersion)v; }
		if (key == OBS2("confidence_threshold")) { float v = decNum<float>(val, 0.25f); if (v < 0.0f || v > 1.0f) v = 0.25f; confidence_threshold = v; }
		if (key == OBS3("nms_threshold")) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 1.0f) v = 0.45f; nms_threshold = v; }
		if (key == OBS4("model_width")) { int v = decNum<int32_t>(val, 640); if (v < 64 || v > 4096) v = 640; model_width = v; }
		if (key == OBS5("model_height")) { int v = decNum<int32_t>(val, 640); if (v < 64 || v > 4096) v = 640; model_height = v; }
		if (key == OBS4("aim_smoothness")) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 1.0f) v = 0.35f; aim_smoothness = v; }
		if (key == OBS5("aim_position")) { float v = decNum<float>(val, 0.0f); if (v < 0.0f || v > 4.0f) v = 0.0f; aim_position = v; }
		if (key == OBS6("aim_position_x")) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; aim_position_x = v; }
		if (key == OBS7("aim_fov")) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; aim_fov = v; }
		if (key == OBS7("aim_humanize")) aim_humanize = decNum<int32_t>(val, 0) != 0;
		if (key == OBS7("aim_fov_locked")) { }
		if (key == OBS8("fov_shrink_speed")) { }
		if (key == OBS1("fov_expand_speed")) { }
		if (key == OBS2("fov_auto_zoom")) { }
		if (key == OBS3("speed_multiplier")) { float v = decNum<float>(val, 1.0f); if (v < 0.1f || v > 10.0f) v = 1.0f; speed_multiplier = v; }
		if (key == OBS4("track_match_threshold")) { float v = decNum<float>(val, 150.0f); if (v < 0.0f || v > 500.0f) v = 150.0f; track_match_threshold = v; }
		if (key == OBS5("track_max_lost")) { int v = decNum<int32_t>(val, 30); if (v < 0 || v > 100) v = 30; track_max_lost = v; }
		if (key == OBS6("track_switch_hysteresis")) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 100.0f) v = 0.45f; track_switch_hysteresis = v; }
		if (key == OBS7("track_confirm_frames")) { int v = decNum<int32_t>(val, 2); if (v < 1 || v > 10) v = 2; track_confirm_frames = v; }
		if (key == OBS7("kalman_process_noise")) { float v = decNum<float>(val, 12.0f); if (v < 0.001f || v > 100.0f) v = 12.0f; kalman_process_noise = v; }
		if (key == OBS8("kalman_measurement_noise")) { float v = decNum<float>(val, 2.0f); if (v < 0.001f || v > 100.0f) v = 2.0f; kalman_measurement_noise = v; }
		if (key == OBS2("kalman_enabled")) kalman_enabled = decNum<int32_t>(val, 1) != 0;
		if (key == OBS3("scene_adapt_enabled")) scene_adapt_enabled = decNum<int32_t>(val, 1) != 0;
		if (key == OBS4("aim_preset")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; aim_preset = (AimPreset)v; }
		if (key == OBS5("makcu_port")) makcu_port = decStr(val);
		if (key == OBS6("makcu_baud")) { int v = decNum<int32_t>(val, 115200); if (v <= 0) v = 115200; makcu_baud = v; }
		if (key == OBS7("driver_mode")) { int v = decNum<int32_t>(val, 1); if (v < 0 || v > 2) v = 1; driver_mode = (DriverMode)v; }
		if (key == OBS8("infer_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; infer_mode = (InferMode)v; }
		if (key == OBS1("key_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; key_mode = (KeyMode)v; }
		if (key == OBS2("aim_key1")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key1.vkCode = v; }
		if (key == OBS3("aim_key2")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key2.vkCode = v; }
		if (key == OBS5("capture_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 2) v = 0; capture_mode = v; }
		if (key == OBS6("max_detections")) { int v = decNum<int32_t>(val, 100); if (v < 1 || v > 1000) v = 100; max_detections = v; }
		if (key == OBS4("aim_key_slot_count")) {
			int cnt = decNum<int32_t>(val, 0);
			if (cnt > 0 && cnt <= 10) aim_key_slots.resize(cnt);
		}
		else if (key.find(OBS5("aks")) == 0) {
			for (int si = 0; si < (int)aim_key_slots.size(); ++si) {
				char prefix[32];
				snprintf(prefix, sizeof(prefix), OBS5("aks%d_").c_str(), si);
				std::string pfx(prefix);
				if (key == pfx + OBS6("vk")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key_slots[si].vkCode = v; break; }
				if (key == pfx + OBS7("mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; aim_key_slots[si].key_mode = (KeyMode)v; break; }
				if (key == pfx + OBS8("combo_vk")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key_slots[si].combo_vkCode = v; break; }
			}
		}
		if (key == OBS4("show_preview")) show_preview = decNum<int32_t>(val, 1) != 0;
		if (key == OBS5("preview_x")) { int v = decNum<int32_t>(val, -1); if (v < -10000 || v > 10000) v = -1; preview_x = v; }
		if (key == OBS6("preview_y")) { int v = decNum<int32_t>(val, -1); if (v < -10000 || v > 10000) v = -1; preview_y = v; }
		if (key == OBS7("preview_w")) { int v = decNum<int32_t>(val, 640); if (v < 100 || v > 4000) v = 640; preview_w = v; }
		if (key == OBS8("preview_h")) { int v = decNum<int32_t>(val, 640); if (v < 100 || v > 4000) v = 640; preview_h = v; }
		if (key == OBS1("anti_capture")) anti_capture = decNum<int32_t>(val, 0) != 0;
		if (key == OBS2("trt_precision")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; trt_precision = (TrtPrecision)v; }
		if (key == OBS4("gpu_lock_clocks")) gpu_lock_clocks = decNum<int32_t>(val, 1) != 0;
		if (key == OBS3("trt_mixed_precision")) trt_mixed_precision = decNum<int32_t>(val, 0) != 0;
		if (key == OBS3("pid_kp")) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 1.0f) v = 0.45f; pid_kp = v; }
	if (key == OBS4("pid_kp_y")) { float v = decNum<float>(val, 0.30f); if (v < 0.0f || v > 1.0f) v = 0.30f; pid_kp_y = v; }
	if (key == OBS4("pid_ki")) { float v = decNum<float>(val, 0.01f); if (v < 0.0f || v > 0.1f) v = 0.01f; pid_ki = v; }
	if (key == OBS5("pid_kd")) { float v = decNum<float>(val, 0.15f); if (v < 0.0f || v > 2.0f) v = 0.15f; pid_kd = v; }
		if (key == OBS6("pid_deadzone")) { float v = decNum<float>(val, 2.0f); if (v < 0.0f || v > 50.0f) v = 2.0f; pid_deadzone = v; }
		if (key == OBS7("move_randomize")) move_randomize = decNum<int32_t>(val, 1) != 0;
		if (key == OBS8("move_max_step")) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 50.0f) v = 25.0f; move_max_step = v; }
	if (key == OBS5("os_enabled")) os_enabled = decNum<int32_t>(val, 1) != 0;
	if (key == OBS6("os_damp")) { float v = decNum<float>(val, 0.70f); if (v < 0.30f || v > 0.95f) v = 0.70f; os_damp = v; }
	if (key == OBS7("os_base")) { float v = decNum<float>(val, 1.0f); if (v < 0.10f || v > 1.0f) v = 1.0f; os_base = v; }
	if (key == OBS1("move_inertia")) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 0.95f) v = 0.35f; move_inertia = v; }
	if (key == OBS2("move_jitter_range")) { float v = decNum<float>(val, 1.5f); if (v < 0.0f || v > 5.0f) v = 1.5f; move_jitter_range = v; }
	if (key == OBS3("move_jitter_scale")) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 2.0f) v = 0.8f; move_jitter_scale = v; }
	if (key == OBS4("move_accel_near")) { float v = decNum<float>(val, 30.0f); if (v < 5.0f || v > 200.0f) v = 30.0f; move_accel_near = v; }
	if (key == OBS5("move_accel_far")) { float v = decNum<float>(val, 100.0f); if (v < 10.0f || v > 500.0f) v = 100.0f; move_accel_far = v; }
	if (key == OBS6("move_fine_dist")) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 100.0f) v = 25.0f; move_fine_dist = v; }
		if (key == OBS5("ff_enabled")) ff_enabled = decNum<int32_t>(val, 1) != 0;
		if (key == OBS6("ff_gain")) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 3.0f) v = 0.8f; ff_gain = v; }
		if (key == OBS7("ff_target_speed")) { float v = decNum<float>(val, 150.0f); if (v < 20.0f || v > 1000.0f) v = 150.0f; ff_target_speed = v; }
		if (key == OBS7("quickscope_enabled")) quickscope_enabled = decNum<int32_t>(val, 0) != 0;
		if (key == OBS8("quickscope_key")) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; quickscope_key.vkCode = v; }
		if (key == OBS4("quickscope_fire_delay")) { int v = decNum<int32_t>(val, 3); if (v < 0 || v > 20) v = 3; quickscope_fire_delay = v; }
		if (key == OBS1("auto_trigger_enabled")) auto_trigger_enabled = decNum<int32_t>(val, 0) != 0;
		if (key == OBS2("auto_trigger_key")) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; auto_trigger_key.vkCode = v; }
		if (key == OBS3("auto_trigger_delay_ms")) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 1000) v = 50; auto_trigger_delay_ms = v; }
		if (key == OBS4("auto_trigger_threshold")) { float v = decNum<float>(val, 5.0f); if (v < 0.5f || v > 50.0f) v = 5.0f; auto_trigger_threshold = v; }
		if (key == OBS5("auto_trigger_interval_ms")) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 500) v = 50; auto_trigger_interval_ms = v; }
		if (key == OBS7("auto_trigger_humanize")) auto_trigger_humanize = decNum<int32_t>(val, 1) != 0;
		if (key == OBS1("custom_preset_idx")) { int v = decNum<int32_t>(val, -1); if (v < -1 || v > 9) v = -1; custom_preset_idx = v; }
		if (key == OBS2("aim_enabled")) aim_enabled = decNum<int32_t>(val, 0) != 0;
			if (key == OBS3("aim_toggle_enabled")) aim_toggle_enabled = decNum<int32_t>(val, 0) != 0;
				if (key == OBS7("toggle_sound")) toggle_sound = decNum<int32_t>(val, 1) != 0;
					if (key == OBS4("aim_toggle_key_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; aim_toggle_key_mode = (KeyMode)v; }
				if (key == OBS5("aim_toggle_key")) { int v = decNum<int32_t>(val, 0x79); if (v < 0 || v > 254) v = 0x79; aim_toggle_key = v; }
				if (key == OBS6("aim_toggle_combo_key")) { int v = decNum<int32_t>(val, 0xA2); if (v < 0 || v > 254) v = 0xA2; aim_toggle_combo_key = v; }
				if (key == OBS7("ui_toggle_key")) { int v = decNum<int32_t>(val, 0x24); if (v < 0 || v > 254) v = 0x24; ui_toggle_key = v; }
				if (key == OBS3("gpu_id")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 15) v = 0; gpu_id = v; }
				if (key == OBS3("infer_gpu_id")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 15) v = 0; infer_gpu_id = v; }
		if (key == OBS4("cp_name")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].name = decStr(val); }
		if (key == OBS5("cp_aim_smoothness")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 1.0f) v = 0.35f; custom_presets[m_loadingCpIdx].aim_smoothness = v; } }
		if (key == OBS6("cp_aim_position")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.0f); if (v < 0.0f || v > 4.0f) v = 0.0f; custom_presets[m_loadingCpIdx].aim_position = v; } }
		if (key == OBS7("cp_aim_position_x")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; custom_presets[m_loadingCpIdx].aim_position_x = v; } }
		if (key == OBS7("cp_aim_fov")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; custom_presets[m_loadingCpIdx].aim_fov = v; } }
		if (key == OBS8("cp_aim_fov_locked")) { }
		if (key == OBS1("cp_fov_shrink_speed")) { }
		if (key == OBS2("cp_fov_expand_speed")) { }
		if (key == OBS3("cp_fov_auto_zoom")) { }
		if (key == OBS4("cp_speed_multiplier")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 1.0f); if (v < 0.1f || v > 10.0f) v = 1.0f; custom_presets[m_loadingCpIdx].speed_multiplier = v; } }
		if (key == OBS5("cp_track_match_threshold")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 150.0f); if (v < 0.0f || v > 500.0f) v = 150.0f; custom_presets[m_loadingCpIdx].track_match_threshold = v; } }
		if (key == OBS6("cp_track_max_lost")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 30); if (v < 0 || v > 100) v = 30; custom_presets[m_loadingCpIdx].track_max_lost = v; } }
		if (key == OBS7("cp_track_switch_hysteresis")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 100.0f) v = 0.45f; custom_presets[m_loadingCpIdx].track_switch_hysteresis = v; } }
		if (key == OBS7("cp_track_confirm_frames")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 2); if (v < 1 || v > 10) v = 2; custom_presets[m_loadingCpIdx].track_confirm_frames = v; } }
		if (key == OBS8("cp_kalman_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].kalman_enabled = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS1("cp_kalman_process_noise")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 12.0f); if (v < 0.001f || v > 100.0f) v = 12.0f; custom_presets[m_loadingCpIdx].kalman_process_noise = v; } }
		if (key == OBS2("cp_kalman_measurement_noise")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 2.0f); if (v < 0.001f || v > 100.0f) v = 2.0f; custom_presets[m_loadingCpIdx].kalman_measurement_noise = v; } }
		if (key == OBS4("cp_scene_adapt_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].scene_adapt_enabled = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS3("cp_pid_kp")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.20f); if (v < 0.0f || v > 1.0f) v = 0.20f; custom_presets[m_loadingCpIdx].pid_kp = v; } }
	if (key == OBS4("cp_pid_ki")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.01f); if (v < 0.0f || v > 0.1f) v = 0.01f; custom_presets[m_loadingCpIdx].pid_ki = v; } }
	if (key == OBS5("cp_pid_kd")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.15f); if (v < 0.0f || v > 2.0f) v = 0.15f; custom_presets[m_loadingCpIdx].pid_kd = v; } }
		if (key == OBS6("cp_pid_deadzone")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 1.0f); if (v < 0.0f || v > 50.0f) v = 1.0f; custom_presets[m_loadingCpIdx].pid_deadzone = v; } }
		if (key == OBS7("cp_move_randomize")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].move_randomize = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS8("cp_move_max_step")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 50.0f) v = 25.0f; custom_presets[m_loadingCpIdx].move_max_step = v; } }
		if (key == OBS1("cp_move_inertia")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 0.95f) v = 0.35f; custom_presets[m_loadingCpIdx].move_inertia = v; } }
		if (key == OBS2("cp_move_jitter_range")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 1.5f); if (v < 0.0f || v > 10.0f) v = 1.5f; custom_presets[m_loadingCpIdx].move_jitter_range = v; } }
		if (key == OBS3("cp_move_jitter_scale")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 5.0f) v = 0.8f; custom_presets[m_loadingCpIdx].move_jitter_scale = v; } }
		if (key == OBS4("cp_move_accel_near")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 30.0f); if (v < 1.0f || v > 200.0f) v = 30.0f; custom_presets[m_loadingCpIdx].move_accel_near = v; } }
		if (key == OBS1("cp_move_accel_far")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 100.0f); if (v < 1.0f || v > 500.0f) v = 100.0f; custom_presets[m_loadingCpIdx].move_accel_far = v; } }
		if (key == OBS2("cp_move_fine_dist")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 200.0f) v = 25.0f; custom_presets[m_loadingCpIdx].move_fine_dist = v; } }
		if (key == OBS3("cp_ff_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].ff_enabled = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS4("cp_ff_gain")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 3.0f) v = 0.8f; custom_presets[m_loadingCpIdx].ff_gain = v; } }
		if (key == OBS5("cp_ff_target_speed")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 150.0f); if (v < 20.0f || v > 1000.0f) v = 150.0f; custom_presets[m_loadingCpIdx].ff_target_speed = v; } }
		if (key == OBS7("cp_quickscope_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].quickscope_enabled = decNum<int32_t>(val, 0) != 0; }
		if (key == OBS8("cp_quickscope_key")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; custom_presets[m_loadingCpIdx].quickscope_key.vkCode = v; } }
		if (key == OBS4("cp_quickscope_fire_delay")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 3); if (v < 0 || v > 20) v = 3; custom_presets[m_loadingCpIdx].quickscope_fire_delay = v; } }
		if (key == OBS1("cp_auto_trigger_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].auto_trigger_enabled = decNum<int32_t>(val, 0) != 0; }
		if (key == OBS2("cp_auto_trigger_key")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; custom_presets[m_loadingCpIdx].auto_trigger_key.vkCode = v; } }
		if (key == OBS3("cp_auto_trigger_delay_ms")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 1000) v = 50; custom_presets[m_loadingCpIdx].auto_trigger_delay_ms = v; } }
		if (key == OBS4("cp_auto_trigger_threshold")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 5.0f); if (v < 0.5f || v > 50.0f) v = 5.0f; custom_presets[m_loadingCpIdx].auto_trigger_threshold = v; } }
		if (key == OBS5("cp_auto_trigger_interval_ms")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 500) v = 50; custom_presets[m_loadingCpIdx].auto_trigger_interval_ms = v; } }
		if (key == OBS7("cp_auto_trigger_humanize")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].auto_trigger_humanize = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS4("per_class_aim_enabled")) per_class_aim_enabled = decNum<int32_t>(val, 0) != 0;
		if (key == OBS4("cap_class_id")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 999) v = 0; class_aim_positions[m_loadingCapIdx].classId = v; } }
		if (key == OBS5("cap_aim_position")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) { float v = decNum<float>(val, 0.0f); if (v < 0.0f || v > 4.0f) v = 0.0f; class_aim_positions[m_loadingCapIdx].aim_position = v; } }
		if (key == OBS6("cap_aim_position_x")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; class_aim_positions[m_loadingCapIdx].aim_position_x = v; } }
		if (key == OBS7("cap_enabled")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) class_aim_positions[m_loadingCapIdx].enabled = decNum<int32_t>(val, 0) != 0; }
		if (key == OBS3("disabled_classes")) {
			try {
				disabled_classes.clear();
				std::string dcDec = decStr(val);
				std::istringstream ss(dcDec);
				std::string token;
				while (std::getline(ss, token, ',')) {
					int id = std::stoi(trim(token));
					disabled_classes.push_back(id);
				}
			} catch (...) {
				disabled_classes.clear();
			}
		}
		} catch (const std::exception& e) {
			LOG_ERROR(OBS8(u8"配置文件加载异常: %s").c_str(), e.what());
		}
		catch (...) {
			LOG_ERROR(OBS1(u8"配置文件加载未知异常").c_str());
		}
	}
	in.close();
	if (custom_preset_idx >= (int)custom_presets.size() || custom_preset_idx < -1) {
		custom_preset_idx = -1;
		if (aim_preset == AimPreset::Custom) aim_preset = AimPreset::Default;
	}
	return true;
}
void D5RenderConfig::saveToPath(const std::string& path) const {
	std::ofstream out(path);
	if (!out.is_open()) return;
	out << OBS7("[General]") << "\n";
	out << OBS2("model_path") << "=" << encStr(model_path) << "\n";
	out << OBS1("yolo_version") << "=" << encNum((int32_t)yolo_version) << "\n";
	out << OBS2("confidence_threshold") << "=" << encNum(confidence_threshold) << "\n";
	out << OBS3("nms_threshold") << "=" << encNum(nms_threshold) << "\n";
	out << OBS4("model_width") << "=" << encNum((int32_t)model_width) << "\n";
	out << OBS5("model_height") << "=" << encNum((int32_t)model_height) << "\n";
	out << OBS4("aim_smoothness") << "=" << encNum(aim_smoothness) << "\n";
	out << OBS5("aim_position") << "=" << encNum(aim_position) << "\n";
	out << OBS6("aim_position_x") << "=" << encNum(aim_position_x) << "\n";
	out << OBS6("aim_fov") << "=" << encNum(aim_fov) << "\n";
	out << OBS7("aim_humanize") << "=" << encNum((int32_t)(aim_humanize ? 1 : 0)) << "\n";
	out << OBS3("speed_multiplier") << "=" << encNum(speed_multiplier) << "\n";
	out << OBS4("track_match_threshold") << "=" << encNum(track_match_threshold) << "\n";
	out << OBS5("track_max_lost") << "=" << encNum((int32_t)track_max_lost) << "\n";
	out << OBS6("track_switch_hysteresis") << "=" << encNum(track_switch_hysteresis) << "\n";
	out << OBS7("track_confirm_frames") << "=" << encNum((int32_t)track_confirm_frames) << "\n";
	out << OBS7("kalman_process_noise") << "=" << encNum(kalman_process_noise) << "\n";
	out << OBS8("kalman_measurement_noise") << "=" << encNum(kalman_measurement_noise) << "\n";
	out << OBS2("kalman_enabled") << "=" << encNum((int32_t)(kalman_enabled ? 1 : 0)) << "\n";
	out << OBS3("scene_adapt_enabled") << "=" << encNum((int32_t)(scene_adapt_enabled ? 1 : 0)) << "\n";
	out << OBS4("aim_preset") << "=" << encNum((int32_t)aim_preset) << "\n";
	out << OBS5("makcu_port") << "=" << encStr(makcu_port) << "\n";
	out << OBS6("makcu_baud") << "=" << encNum((int32_t)makcu_baud) << "\n";
	out << OBS7("driver_mode") << "=" << encNum((int32_t)driver_mode) << "\n";
	out << OBS8("infer_mode") << "=" << encNum((int32_t)infer_mode) << "\n";
	out << OBS1("key_mode") << "=" << encNum((int32_t)key_mode) << "\n";
	out << OBS2("aim_key1") << "=" << encNum((int32_t)aim_key1.vkCode) << "\n";
	out << OBS3("aim_key2") << "=" << encNum((int32_t)aim_key2.vkCode) << "\n";
	out << OBS4("aim_key_slot_count") << "=" << encNum((int32_t)aim_key_slots.size()) << "\n";
	out << OBS5("capture_mode") << "=" << encNum((int32_t)capture_mode) << "\n";
	out << OBS6("max_detections") << "=" << encNum((int32_t)max_detections) << "\n";
	for (int i = 0; i < (int)aim_key_slots.size(); ++i) {
		char prefix[32];
		snprintf(prefix, sizeof(prefix), OBS5("aks%d_").c_str(), i);
		out << prefix << OBS6("vk") << "=" << encNum((int32_t)aim_key_slots[i].vkCode) << "\n";
		out << prefix << OBS7("mode") << "=" << encNum((int32_t)aim_key_slots[i].key_mode) << "\n";
		out << prefix << OBS8("combo_vk") << "=" << encNum((int32_t)aim_key_slots[i].combo_vkCode) << "\n";
	}
	out << OBS4("show_preview") << "=" << encNum((int32_t)(show_preview ? 1 : 0)) << "\n";
	out << OBS5("preview_x") << "=" << encNum((int32_t)preview_x) << "\n";
	out << OBS6("preview_y") << "=" << encNum((int32_t)preview_y) << "\n";
	out << OBS7("preview_w") << "=" << encNum((int32_t)preview_w) << "\n";
	out << OBS8("preview_h") << "=" << encNum((int32_t)preview_h) << "\n";
	out << OBS1("anti_capture") << "=" << encNum((int32_t)(anti_capture ? 1 : 0)) << "\n";
	out << OBS2("trt_precision") << "=" << encNum((int32_t)trt_precision) << "\n";
	out << OBS3("trt_mixed_precision") << "=" << encNum((int32_t)(trt_mixed_precision ? 1 : 0)) << "\n";
	out << OBS4("gpu_lock_clocks") << "=" << encNum((int32_t)(gpu_lock_clocks ? 1 : 0)) << "\n";
	out << OBS3("pid_kp") << "=" << encNum(pid_kp) << "\n";
	out << OBS4("pid_kp_y") << "=" << encNum(pid_kp_y) << "\n";
	out << OBS4("pid_ki") << "=" << encNum(pid_ki) << "\n";
	out << OBS5("pid_kd") << "=" << encNum(pid_kd) << "\n";
	out << OBS6("pid_deadzone") << "=" << encNum(pid_deadzone) << "\n";
	out << OBS7("move_randomize") << "=" << encNum((int32_t)(move_randomize ? 1 : 0)) << "\n";
	out << OBS8("move_max_step") << "=" << encNum(move_max_step) << "\n";
	out << OBS5("os_enabled") << "=" << encNum((int32_t)(os_enabled ? 1 : 0)) << "\n";
	out << OBS6("os_damp") << "=" << encNum(os_damp) << "\n";
	out << OBS7("os_base") << "=" << encNum(os_base) << "\n";
	out << OBS1("move_inertia") << "=" << encNum(move_inertia) << "\n";
	out << OBS2("move_jitter_range") << "=" << encNum(move_jitter_range) << "\n";
	out << OBS3("move_jitter_scale") << "=" << encNum(move_jitter_scale) << "\n";
	out << OBS4("move_accel_near") << "=" << encNum(move_accel_near) << "\n";
	out << OBS5("move_accel_far") << "=" << encNum(move_accel_far) << "\n";
	out << OBS6("move_fine_dist") << "=" << encNum(move_fine_dist) << "\n";
	out << OBS5("ff_enabled") << "=" << encNum((int32_t)(ff_enabled ? 1 : 0)) << "\n";
	out << OBS6("ff_gain") << "=" << encNum(ff_gain) << "\n";
	out << OBS7("ff_target_speed") << "=" << encNum(ff_target_speed) << "\n";
	out << OBS7("quickscope_enabled") << "=" << encNum((int32_t)(quickscope_enabled ? 1 : 0)) << "\n";
	out << OBS8("quickscope_key") << "=" << encNum((int32_t)quickscope_key.vkCode) << "\n";
	out << OBS4("quickscope_fire_delay") << "=" << encNum((int32_t)quickscope_fire_delay) << "\n";
	out << OBS1("auto_trigger_enabled") << "=" << encNum((int32_t)(auto_trigger_enabled ? 1 : 0)) << "\n";
	out << OBS2("auto_trigger_key") << "=" << encNum((int32_t)auto_trigger_key.vkCode) << "\n";
	out << OBS3("auto_trigger_delay_ms") << "=" << encNum((int32_t)auto_trigger_delay_ms) << "\n";
	out << OBS4("auto_trigger_threshold") << "=" << encNum(auto_trigger_threshold) << "\n";
	out << OBS5("auto_trigger_interval_ms") << "=" << encNum((int32_t)auto_trigger_interval_ms) << "\n";
	out << OBS7("auto_trigger_humanize") << "=" << encNum((int32_t)(auto_trigger_humanize ? 1 : 0)) << "\n";
	out << OBS1("custom_preset_idx") << "=" << encNum((int32_t)custom_preset_idx) << "\n";
	out << OBS2("aim_enabled") << "=" << encNum((int32_t)(aim_enabled ? 1 : 0)) << "\n";
		out << OBS3("aim_toggle_enabled") << "=" << encNum((int32_t)(aim_toggle_enabled ? 1 : 0)) << "\n";
		out << OBS7("toggle_sound") << "=" << encNum((int32_t)(toggle_sound ? 1 : 0)) << "\n";
		out << OBS4("aim_toggle_key_mode") << "=" << encNum((int32_t)aim_toggle_key_mode) << "\n";
		out << OBS5("aim_toggle_key") << "=" << encNum((int32_t)aim_toggle_key) << "\n";
		out << OBS6("aim_toggle_combo_key") << "=" << encNum((int32_t)aim_toggle_combo_key) << "\n";
		out << OBS7("ui_toggle_key") << "=" << encNum((int32_t)ui_toggle_key) << "\n";
		out << OBS3("gpu_id") << "=" << encNum((int32_t)gpu_id) << "\n";
		out << OBS3("infer_gpu_id") << "=" << encNum((int32_t)infer_gpu_id) << "\n";
		for (size_t i = 0; i < custom_presets.size(); ++i) {
		const auto& cp = custom_presets[i];
		out << OBS8("[CustomPreset_") << i << "]" << "\n";
		out << OBS4("cp_name") << "=" << encStr(cp.name) << "\n";
		out << OBS5("cp_aim_smoothness") << "=" << encNum(cp.aim_smoothness) << "\n";
		out << OBS6("cp_aim_position") << "=" << encNum(cp.aim_position) << "\n";
		out << OBS7("cp_aim_position_x") << "=" << encNum(cp.aim_position_x) << "\n";
		out << OBS7("cp_aim_fov") << "=" << encNum(cp.aim_fov) << "\n";
		out << OBS4("cp_speed_multiplier") << "=" << encNum(cp.speed_multiplier) << "\n";
		out << OBS5("cp_track_match_threshold") << "=" << encNum(cp.track_match_threshold) << "\n";
		out << OBS6("cp_track_max_lost") << "=" << encNum((int32_t)cp.track_max_lost) << "\n";
		out << OBS7("cp_track_switch_hysteresis") << "=" << encNum(cp.track_switch_hysteresis) << "\n";
		out << OBS7("cp_track_confirm_frames") << "=" << encNum((int32_t)cp.track_confirm_frames) << "\n";
		out << OBS8("cp_kalman_enabled") << "=" << encNum((int32_t)(cp.kalman_enabled ? 1 : 0)) << "\n";
		out << OBS1("cp_kalman_process_noise") << "=" << encNum(cp.kalman_process_noise) << "\n";
		out << OBS2("cp_kalman_measurement_noise") << "=" << encNum(cp.kalman_measurement_noise) << "\n";
		out << OBS4("cp_scene_adapt_enabled") << "=" << encNum((int32_t)(cp.scene_adapt_enabled ? 1 : 0)) << "\n";
		out << OBS3("cp_pid_kp") << "=" << encNum(cp.pid_kp) << "\n";
		out << OBS4("cp_pid_ki") << "=" << encNum(cp.pid_ki) << "\n";
		out << OBS5("cp_pid_kd") << "=" << encNum(cp.pid_kd) << "\n";
		out << OBS6("cp_pid_deadzone") << "=" << encNum(cp.pid_deadzone) << "\n";
		out << OBS7("cp_move_randomize") << "=" << encNum((int32_t)(cp.move_randomize ? 1 : 0)) << "\n";
		out << OBS8("cp_move_max_step") << "=" << encNum(cp.move_max_step) << "\n";
		out << OBS1("cp_move_inertia") << "=" << encNum(cp.move_inertia) << "\n";
		out << OBS2("cp_move_jitter_range") << "=" << encNum(cp.move_jitter_range) << "\n";
		out << OBS3("cp_move_jitter_scale") << "=" << encNum(cp.move_jitter_scale) << "\n";
		out << OBS4("cp_move_accel_near") << "=" << encNum(cp.move_accel_near) << "\n";
		out << OBS5("cp_move_accel_far") << "=" << encNum(cp.move_accel_far) << "\n";
		out << OBS6("cp_move_fine_dist") << "=" << encNum(cp.move_fine_dist) << "\n";
		out << OBS4("cp_ff_enabled") << "=" << encNum((int32_t)(cp.ff_enabled ? 1 : 0)) << "\n";
		out << OBS5("cp_ff_gain") << "=" << encNum(cp.ff_gain) << "\n";
		out << OBS6("cp_ff_target_speed") << "=" << encNum(cp.ff_target_speed) << "\n";
		out << OBS7("cp_quickscope_enabled") << "=" << encNum((int32_t)(cp.quickscope_enabled ? 1 : 0)) << "\n";
		out << OBS8("cp_quickscope_key") << "=" << encNum((int32_t)cp.quickscope_key.vkCode) << "\n";
		out << OBS4("cp_quickscope_fire_delay") << "=" << encNum((int32_t)cp.quickscope_fire_delay) << "\n";
		out << OBS1("cp_auto_trigger_enabled") << "=" << encNum((int32_t)(cp.auto_trigger_enabled ? 1 : 0)) << "\n";
		out << OBS2("cp_auto_trigger_key") << "=" << encNum((int32_t)cp.auto_trigger_key.vkCode) << "\n";
		out << OBS3("cp_auto_trigger_delay_ms") << "=" << encNum((int32_t)cp.auto_trigger_delay_ms) << "\n";
		out << OBS4("cp_auto_trigger_threshold") << "=" << encNum(cp.auto_trigger_threshold) << "\n";
		out << OBS5("cp_auto_trigger_interval_ms") << "=" << encNum((int32_t)cp.auto_trigger_interval_ms) << "\n";
		out << OBS7("cp_auto_trigger_humanize") << "=" << encNum((int32_t)(cp.auto_trigger_humanize ? 1 : 0)) << "\n";
	}
	if (!disabled_classes.empty()) {
		std::string dcStr;
		for (size_t i = 0; i < disabled_classes.size(); ++i) {
			if (i > 0) dcStr += ",";
			dcStr += std::to_string(disabled_classes[i]);
		}
		out << OBS3("disabled_classes") << "=" << encStr(dcStr) << "\n";
	}
	out << OBS4("per_class_aim_enabled") << "=" << encNum((int32_t)(per_class_aim_enabled ? 1 : 0)) << "\n";
	for (size_t i = 0; i < class_aim_positions.size(); ++i) {
		const auto& cap = class_aim_positions[i];
		out << OBS8("[ClassAimPos_") << i << "]" << "\n";
		out << OBS4("cap_class_id") << "=" << encNum((int32_t)cap.classId) << "\n";
		out << OBS5("cap_aim_position") << "=" << encNum(cap.aim_position) << "\n";
		out << OBS6("cap_aim_position_x") << "=" << encNum(cap.aim_position_x) << "\n";
		out << OBS7("cap_enabled") << "=" << encNum((int32_t)(cap.enabled ? 1 : 0)) << "\n";
	}
	out.close();
}
bool D5RenderConfig::loadFromPath(const std::string& path) {
	if (!std::filesystem::exists(path)) return false;
	std::ifstream in(path);
	if (!in.is_open()) return false;
	std::string line;
	int m_loadingCpIdx = -1;
	int m_loadingCapIdx = -1;
	while (std::getline(in, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#') continue;
		if (line[0] == '[') {
			if (line.find(OBS8("[CustomPreset_")) == 0) {
				custom_presets.push_back(CustomPreset());
				m_loadingCpIdx = (int)custom_presets.size() - 1;
				m_loadingCapIdx = -1;
			} else if (line.find(OBS8("[ClassAimPos_")) == 0) {
				class_aim_positions.push_back(ClassAimPos());
				m_loadingCapIdx = (int)class_aim_positions.size() - 1;
				m_loadingCpIdx = -1;
			} else {
				m_loadingCpIdx = -1;
				m_loadingCapIdx = -1;
			}
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos) continue;
		std::string key = trim(line.substr(0, eq));
		std::string val = trim(line.substr(eq + 1));
		try {
		if (key == OBS2("model_path")) model_path = decStr(val);
		if (key == OBS1("yolo_version")) { int v = decNum<int32_t>(val, 7); if (v < 0 || v > 7) v = 7; yolo_version = (YoloVersion)v; }
		if (key == OBS2("confidence_threshold")) { float v = decNum<float>(val, 0.25f); if (v < 0.0f || v > 1.0f) v = 0.25f; confidence_threshold = v; }
		if (key == OBS3("nms_threshold")) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 1.0f) v = 0.45f; nms_threshold = v; }
		if (key == OBS4("model_width")) { int v = decNum<int32_t>(val, 640); if (v < 64 || v > 4096) v = 640; model_width = v; }
		if (key == OBS5("model_height")) { int v = decNum<int32_t>(val, 640); if (v < 64 || v > 4096) v = 640; model_height = v; }
		if (key == OBS4("aim_smoothness")) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 1.0f) v = 0.35f; aim_smoothness = v; }
		if (key == OBS5("aim_position")) { float v = decNum<float>(val, 0.0f); if (v < 0.0f || v > 4.0f) v = 0.0f; aim_position = v; }
		if (key == OBS6("aim_position_x")) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; aim_position_x = v; }
		if (key == OBS7("aim_fov")) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; aim_fov = v; }
		if (key == OBS7("aim_humanize")) aim_humanize = decNum<int32_t>(val, 0) != 0;
		if (key == OBS7("aim_fov_locked")) { }
		if (key == OBS8("fov_shrink_speed")) { }
		if (key == OBS1("fov_expand_speed")) { }
		if (key == OBS2("fov_auto_zoom")) { }
		if (key == OBS3("speed_multiplier")) { float v = decNum<float>(val, 1.0f); if (v < 0.1f || v > 10.0f) v = 1.0f; speed_multiplier = v; }
		if (key == OBS4("track_match_threshold")) { float v = decNum<float>(val, 150.0f); if (v < 0.0f || v > 500.0f) v = 150.0f; track_match_threshold = v; }
		if (key == OBS5("track_max_lost")) { int v = decNum<int32_t>(val, 30); if (v < 0 || v > 100) v = 30; track_max_lost = v; }
		if (key == OBS6("track_switch_hysteresis")) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 100.0f) v = 0.45f; track_switch_hysteresis = v; }
		if (key == OBS7("track_confirm_frames")) { int v = decNum<int32_t>(val, 2); if (v < 1 || v > 10) v = 2; track_confirm_frames = v; }
		if (key == OBS7("kalman_process_noise")) { float v = decNum<float>(val, 12.0f); if (v < 0.001f || v > 100.0f) v = 12.0f; kalman_process_noise = v; }
		if (key == OBS8("kalman_measurement_noise")) { float v = decNum<float>(val, 2.0f); if (v < 0.001f || v > 100.0f) v = 2.0f; kalman_measurement_noise = v; }
		if (key == OBS2("kalman_enabled")) kalman_enabled = decNum<int32_t>(val, 1) != 0;
		if (key == OBS3("scene_adapt_enabled")) scene_adapt_enabled = decNum<int32_t>(val, 1) != 0;
		if (key == OBS4("aim_preset")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; aim_preset = (AimPreset)v; }
		if (key == OBS5("makcu_port")) makcu_port = decStr(val);
		if (key == OBS6("makcu_baud")) { int v = decNum<int32_t>(val, 115200); if (v <= 0) v = 115200; makcu_baud = v; }
		if (key == OBS7("driver_mode")) { int v = decNum<int32_t>(val, 1); if (v < 0 || v > 2) v = 1; driver_mode = (DriverMode)v; }
		if (key == OBS8("infer_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; infer_mode = (InferMode)v; }
		if (key == OBS1("key_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; key_mode = (KeyMode)v; }
		if (key == OBS2("aim_key1")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key1.vkCode = v; }
		if (key == OBS3("aim_key2")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key2.vkCode = v; }
		if (key == OBS5("capture_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 2) v = 0; capture_mode = v; }
		if (key == OBS6("max_detections")) { int v = decNum<int32_t>(val, 100); if (v < 1 || v > 1000) v = 100; max_detections = v; }
		if (key == OBS4("aim_key_slot_count")) {
			int cnt = decNum<int32_t>(val, 0);
			if (cnt > 0 && cnt <= 10) aim_key_slots.resize(cnt);
		}
		else if (key.find(OBS5("aks")) == 0) {
			for (int si = 0; si < (int)aim_key_slots.size(); ++si) {
				char prefix[32];
				snprintf(prefix, sizeof(prefix), OBS5("aks%d_").c_str(), si);
				std::string pfx(prefix);
				if (key == pfx + OBS6("vk")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key_slots[si].vkCode = v; break; }
				if (key == pfx + OBS7("mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; aim_key_slots[si].key_mode = (KeyMode)v; break; }
				if (key == pfx + OBS8("combo_vk")) { int v = decNum<int32_t>(val, 4); if (v < 1 || v > 254) v = 4; aim_key_slots[si].combo_vkCode = v; break; }
			}
		}
		if (key == OBS4("show_preview")) show_preview = decNum<int32_t>(val, 1) != 0;
		if (key == OBS5("preview_x")) { int v = decNum<int32_t>(val, -1); if (v < -10000 || v > 10000) v = -1; preview_x = v; }
		if (key == OBS6("preview_y")) { int v = decNum<int32_t>(val, -1); if (v < -10000 || v > 10000) v = -1; preview_y = v; }
		if (key == OBS7("preview_w")) { int v = decNum<int32_t>(val, 640); if (v < 100 || v > 4000) v = 640; preview_w = v; }
		if (key == OBS8("preview_h")) { int v = decNum<int32_t>(val, 640); if (v < 100 || v > 4000) v = 640; preview_h = v; }
		if (key == OBS1("anti_capture")) anti_capture = decNum<int32_t>(val, 0) != 0;
		if (key == OBS2("trt_precision")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; trt_precision = (TrtPrecision)v; }
		if (key == OBS4("gpu_lock_clocks")) gpu_lock_clocks = decNum<int32_t>(val, 1) != 0;
		if (key == OBS3("trt_mixed_precision")) trt_mixed_precision = decNum<int32_t>(val, 0) != 0;
		if (key == OBS3("pid_kp")) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 1.0f) v = 0.45f; pid_kp = v; }
	if (key == OBS4("pid_kp_y")) { float v = decNum<float>(val, 0.30f); if (v < 0.0f || v > 1.0f) v = 0.30f; pid_kp_y = v; }
	if (key == OBS4("pid_ki")) { float v = decNum<float>(val, 0.01f); if (v < 0.0f || v > 0.1f) v = 0.01f; pid_ki = v; }
	if (key == OBS5("pid_kd")) { float v = decNum<float>(val, 0.15f); if (v < 0.0f || v > 2.0f) v = 0.15f; pid_kd = v; }
		if (key == OBS6("pid_deadzone")) { float v = decNum<float>(val, 2.0f); if (v < 0.0f || v > 50.0f) v = 2.0f; pid_deadzone = v; }
		if (key == OBS7("move_randomize")) move_randomize = decNum<int32_t>(val, 1) != 0;
		if (key == OBS8("move_max_step")) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 50.0f) v = 25.0f; move_max_step = v; }
	if (key == OBS5("os_enabled")) os_enabled = decNum<int32_t>(val, 1) != 0;
	if (key == OBS6("os_damp")) { float v = decNum<float>(val, 0.70f); if (v < 0.30f || v > 0.95f) v = 0.70f; os_damp = v; }
	if (key == OBS7("os_base")) { float v = decNum<float>(val, 1.0f); if (v < 0.10f || v > 1.0f) v = 1.0f; os_base = v; }
	if (key == OBS1("move_inertia")) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 0.95f) v = 0.35f; move_inertia = v; }
	if (key == OBS2("move_jitter_range")) { float v = decNum<float>(val, 1.5f); if (v < 0.0f || v > 5.0f) v = 1.5f; move_jitter_range = v; }
	if (key == OBS3("move_jitter_scale")) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 2.0f) v = 0.8f; move_jitter_scale = v; }
	if (key == OBS4("move_accel_near")) { float v = decNum<float>(val, 30.0f); if (v < 5.0f || v > 200.0f) v = 30.0f; move_accel_near = v; }
	if (key == OBS5("move_accel_far")) { float v = decNum<float>(val, 100.0f); if (v < 10.0f || v > 500.0f) v = 100.0f; move_accel_far = v; }
	if (key == OBS6("move_fine_dist")) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 100.0f) v = 25.0f; move_fine_dist = v; }
		if (key == OBS5("ff_enabled")) ff_enabled = decNum<int32_t>(val, 1) != 0;
		if (key == OBS6("ff_gain")) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 3.0f) v = 0.8f; ff_gain = v; }
		if (key == OBS7("ff_target_speed")) { float v = decNum<float>(val, 150.0f); if (v < 20.0f || v > 1000.0f) v = 150.0f; ff_target_speed = v; }
		if (key == OBS7("quickscope_enabled")) quickscope_enabled = decNum<int32_t>(val, 0) != 0;
		if (key == OBS8("quickscope_key")) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; quickscope_key.vkCode = v; }
		if (key == OBS4("quickscope_fire_delay")) { int v = decNum<int32_t>(val, 3); if (v < 0 || v > 20) v = 3; quickscope_fire_delay = v; }
		if (key == OBS1("auto_trigger_enabled")) auto_trigger_enabled = decNum<int32_t>(val, 0) != 0;
		if (key == OBS2("auto_trigger_key")) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; auto_trigger_key.vkCode = v; }
		if (key == OBS3("auto_trigger_delay_ms")) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 1000) v = 50; auto_trigger_delay_ms = v; }
		if (key == OBS4("auto_trigger_threshold")) { float v = decNum<float>(val, 5.0f); if (v < 0.5f || v > 50.0f) v = 5.0f; auto_trigger_threshold = v; }
		if (key == OBS5("auto_trigger_interval_ms")) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 500) v = 50; auto_trigger_interval_ms = v; }
		if (key == OBS7("auto_trigger_humanize")) auto_trigger_humanize = decNum<int32_t>(val, 1) != 0;
		if (key == OBS1("custom_preset_idx")) { int v = decNum<int32_t>(val, -1); if (v < -1 || v > 9) v = -1; custom_preset_idx = v; }
		if (key == OBS2("aim_enabled")) aim_enabled = decNum<int32_t>(val, 0) != 0;
			if (key == OBS3("aim_toggle_enabled")) aim_toggle_enabled = decNum<int32_t>(val, 0) != 0;
				if (key == OBS7("toggle_sound")) toggle_sound = decNum<int32_t>(val, 1) != 0;
					if (key == OBS4("aim_toggle_key_mode")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 1) v = 0; aim_toggle_key_mode = (KeyMode)v; }
				if (key == OBS5("aim_toggle_key")) { int v = decNum<int32_t>(val, 0x79); if (v < 0 || v > 254) v = 0x79; aim_toggle_key = v; }
				if (key == OBS6("aim_toggle_combo_key")) { int v = decNum<int32_t>(val, 0xA2); if (v < 0 || v > 254) v = 0xA2; aim_toggle_combo_key = v; }
				if (key == OBS7("ui_toggle_key")) { int v = decNum<int32_t>(val, 0x24); if (v < 0 || v > 254) v = 0x24; ui_toggle_key = v; }
				if (key == OBS3("gpu_id")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 15) v = 0; gpu_id = v; }
				if (key == OBS3("infer_gpu_id")) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 15) v = 0; infer_gpu_id = v; }
		if (key == OBS4("cp_name")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].name = decStr(val); }
		if (key == OBS5("cp_aim_smoothness")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 1.0f) v = 0.35f; custom_presets[m_loadingCpIdx].aim_smoothness = v; } }
		if (key == OBS6("cp_aim_position")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.0f); if (v < 0.0f || v > 4.0f) v = 0.0f; custom_presets[m_loadingCpIdx].aim_position = v; } }
		if (key == OBS7("cp_aim_position_x")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; custom_presets[m_loadingCpIdx].aim_position_x = v; } }
		if (key == OBS7("cp_aim_fov")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; custom_presets[m_loadingCpIdx].aim_fov = v; } }
		if (key == OBS8("cp_aim_fov_locked")) { }
		if (key == OBS1("cp_fov_shrink_speed")) { }
		if (key == OBS2("cp_fov_expand_speed")) { }
		if (key == OBS3("cp_fov_auto_zoom")) { }
		if (key == OBS4("cp_speed_multiplier")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 1.0f); if (v < 0.1f || v > 10.0f) v = 1.0f; custom_presets[m_loadingCpIdx].speed_multiplier = v; } }
		if (key == OBS5("cp_track_match_threshold")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 150.0f); if (v < 0.0f || v > 500.0f) v = 150.0f; custom_presets[m_loadingCpIdx].track_match_threshold = v; } }
		if (key == OBS6("cp_track_max_lost")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 30); if (v < 0 || v > 100) v = 30; custom_presets[m_loadingCpIdx].track_max_lost = v; } }
		if (key == OBS7("cp_track_switch_hysteresis")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.45f); if (v < 0.0f || v > 100.0f) v = 0.45f; custom_presets[m_loadingCpIdx].track_switch_hysteresis = v; } }
		if (key == OBS7("cp_track_confirm_frames")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 2); if (v < 1 || v > 10) v = 2; custom_presets[m_loadingCpIdx].track_confirm_frames = v; } }
		if (key == OBS8("cp_kalman_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].kalman_enabled = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS1("cp_kalman_process_noise")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 12.0f); if (v < 0.001f || v > 100.0f) v = 12.0f; custom_presets[m_loadingCpIdx].kalman_process_noise = v; } }
		if (key == OBS2("cp_kalman_measurement_noise")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 2.0f); if (v < 0.001f || v > 100.0f) v = 2.0f; custom_presets[m_loadingCpIdx].kalman_measurement_noise = v; } }
		if (key == OBS4("cp_scene_adapt_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].scene_adapt_enabled = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS3("cp_pid_kp")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.20f); if (v < 0.0f || v > 1.0f) v = 0.20f; custom_presets[m_loadingCpIdx].pid_kp = v; } }
	if (key == OBS4("cp_pid_ki")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.01f); if (v < 0.0f || v > 0.1f) v = 0.01f; custom_presets[m_loadingCpIdx].pid_ki = v; } }
	if (key == OBS5("cp_pid_kd")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.15f); if (v < 0.0f || v > 2.0f) v = 0.15f; custom_presets[m_loadingCpIdx].pid_kd = v; } }
		if (key == OBS6("cp_pid_deadzone")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 1.0f); if (v < 0.0f || v > 50.0f) v = 1.0f; custom_presets[m_loadingCpIdx].pid_deadzone = v; } }
		if (key == OBS7("cp_move_randomize")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].move_randomize = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS8("cp_move_max_step")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 50.0f) v = 25.0f; custom_presets[m_loadingCpIdx].move_max_step = v; } }
		if (key == OBS1("cp_move_inertia")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.35f); if (v < 0.0f || v > 0.95f) v = 0.35f; custom_presets[m_loadingCpIdx].move_inertia = v; } }
		if (key == OBS2("cp_move_jitter_range")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 1.5f); if (v < 0.0f || v > 10.0f) v = 1.5f; custom_presets[m_loadingCpIdx].move_jitter_range = v; } }
		if (key == OBS3("cp_move_jitter_scale")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 5.0f) v = 0.8f; custom_presets[m_loadingCpIdx].move_jitter_scale = v; } }
		if (key == OBS4("cp_move_accel_near")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 30.0f); if (v < 1.0f || v > 200.0f) v = 30.0f; custom_presets[m_loadingCpIdx].move_accel_near = v; } }
		if (key == OBS1("cp_move_accel_far")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 100.0f); if (v < 1.0f || v > 500.0f) v = 100.0f; custom_presets[m_loadingCpIdx].move_accel_far = v; } }
		if (key == OBS2("cp_move_fine_dist")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 25.0f); if (v < 1.0f || v > 200.0f) v = 25.0f; custom_presets[m_loadingCpIdx].move_fine_dist = v; } }
		if (key == OBS3("cp_ff_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].ff_enabled = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS4("cp_ff_gain")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 0.8f); if (v < 0.0f || v > 3.0f) v = 0.8f; custom_presets[m_loadingCpIdx].ff_gain = v; } }
		if (key == OBS5("cp_ff_target_speed")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 150.0f); if (v < 20.0f || v > 1000.0f) v = 150.0f; custom_presets[m_loadingCpIdx].ff_target_speed = v; } }
		if (key == OBS7("cp_quickscope_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].quickscope_enabled = decNum<int32_t>(val, 0) != 0; }
		if (key == OBS8("cp_quickscope_key")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; custom_presets[m_loadingCpIdx].quickscope_key.vkCode = v; } }
		if (key == OBS4("cp_quickscope_fire_delay")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 3); if (v < 0 || v > 20) v = 3; custom_presets[m_loadingCpIdx].quickscope_fire_delay = v; } }
		if (key == OBS1("cp_auto_trigger_enabled")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].auto_trigger_enabled = decNum<int32_t>(val, 0) != 0; }
		if (key == OBS2("cp_auto_trigger_key")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 0); if (v < 1 || v > 254) v = 0; custom_presets[m_loadingCpIdx].auto_trigger_key.vkCode = v; } }
		if (key == OBS3("cp_auto_trigger_delay_ms")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 1000) v = 50; custom_presets[m_loadingCpIdx].auto_trigger_delay_ms = v; } }
		if (key == OBS4("cp_auto_trigger_threshold")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { float v = decNum<float>(val, 5.0f); if (v < 0.5f || v > 50.0f) v = 5.0f; custom_presets[m_loadingCpIdx].auto_trigger_threshold = v; } }
		if (key == OBS5("cp_auto_trigger_interval_ms")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) { int v = decNum<int32_t>(val, 50); if (v < 0 || v > 500) v = 50; custom_presets[m_loadingCpIdx].auto_trigger_interval_ms = v; } }
		if (key == OBS7("cp_auto_trigger_humanize")) { if (m_loadingCpIdx >= 0 && m_loadingCpIdx < (int)custom_presets.size()) custom_presets[m_loadingCpIdx].auto_trigger_humanize = decNum<int32_t>(val, 1) != 0; }
		if (key == OBS4("per_class_aim_enabled")) per_class_aim_enabled = decNum<int32_t>(val, 0) != 0;
		if (key == OBS4("cap_class_id")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) { int v = decNum<int32_t>(val, 0); if (v < 0 || v > 999) v = 0; class_aim_positions[m_loadingCapIdx].classId = v; } }
		if (key == OBS5("cap_aim_position")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) { float v = decNum<float>(val, 0.0f); if (v < 0.0f || v > 4.0f) v = 0.0f; class_aim_positions[m_loadingCapIdx].aim_position = v; } }
		if (key == OBS6("cap_aim_position_x")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) { float v = decNum<float>(val, 0.5f); if (v < 0.0f || v > 1.0f) v = 0.5f; class_aim_positions[m_loadingCapIdx].aim_position_x = v; } }
		if (key == OBS7("cap_enabled")) { if (m_loadingCapIdx >= 0 && m_loadingCapIdx < (int)class_aim_positions.size()) class_aim_positions[m_loadingCapIdx].enabled = decNum<int32_t>(val, 0) != 0; }
		if (key == OBS3("disabled_classes")) {
			try {
				disabled_classes.clear();
				std::string dcDec = decStr(val);
				std::istringstream ss(dcDec);
				std::string token;
				while (std::getline(ss, token, ',')) {
					int id = std::stoi(trim(token));
					disabled_classes.push_back(id);
				}
			} catch (...) {
				disabled_classes.clear();
			}
		}
		} catch (const std::exception& e) {
			LOG_ERROR(OBS8(u8"配置文件加载异常: %s").c_str(), e.what());
		}
		catch (...) {
			LOG_ERROR(OBS1(u8"配置文件加载未知异常").c_str());
		}
	}
	in.close();
	if (custom_preset_idx >= (int)custom_presets.size() || custom_preset_idx < -1) {
		custom_preset_idx = -1;
		if (aim_preset == AimPreset::Custom) aim_preset = AimPreset::Default;
	}
	return true;
}
