#pragma once
#include <string>
#include <cstdint>
#include <vector>
enum class YoloVersion { V5, V6, V7, V8, V9, V10, V11, V26 };
enum class DriverMode { Logitech = 0, Generic = 1, Makcu = 2 };
enum class KeyMode { Single = 0, Combo = 1 };
enum class InferMode { TensorRT = 0, DirectML = 1 };
enum class AimPreset { Default = 0, Custom = 1 };
enum class TrtPrecision { FP16 = 0, FP32 = 1 };
struct AimKey {
	int vkCode = 0x04;
	static const char* GetName(int vk);
	static int FromIndex(int idx);
	static int GetIndex(int vk);
};
struct AimKeySlot {
	int vkCode = 0x04;
	KeyMode key_mode = KeyMode::Single;
	int combo_vkCode = 0x04;
};
struct CustomPreset {
	std::string name;
	float aim_smoothness = 0.35f;
	float aim_position = 0.0f;
	float aim_position_x = 0.5f;
	float aim_fov = 0.5f;
	float speed_multiplier = 1.0f;
	float track_match_threshold = 150.0f;
	int track_max_lost = 75;   // [v215] 30→75: 1.5秒容忍枪口火焰导致的检测中断, 防轨迹判死换ID横跳
	float track_switch_hysteresis = 0.45f;
	int track_confirm_frames = 2;  // [v231] 新轨迹确认帧数(自定义预设同步)
	bool scene_adapt_enabled = true;
	float pid_kp = 0.30f;
	float pid_deadzone = 6.0f;
	bool move_randomize = false;  // [v217] 默认关闭拟人层(v204用户要求): 抖动=卡帧感+开枪摇晃元凶
	int infer_gpu_id = 0;  // [v218] 推理专用GPU: 0=与捕获同卡(兼容旧行为), 1/2=独立推理卡(与游戏物理解耦根治延迟)
	float move_max_step = 14.0f;  // [v215] 25→14: 单帧位移封顶, 吸附稳不过冲
	float move_inertia = 0.22f;  // [v234] 曲线平滑: smoothstep缓动, 近距轻柔远距直给
	float humanize_strength = 1.0f;  // [v235] 拟人抖动强度: 拉得越大抖动越厉害(0=无扰动 1=标准 2=强力)
	float move_accel_near = 30.0f;
	float move_accel_far = 100.0f;
	float move_fine_dist = 25.0f;
	bool quickscope_enabled = false;
	AimKey quickscope_key;
	int quickscope_fire_delay = 3;
	bool auto_trigger_enabled = false;
	AimKey auto_trigger_key;
	int auto_trigger_delay_ms = 50;
	float auto_trigger_threshold = 5.0f;
	int auto_trigger_interval_ms = 50;
	bool auto_trigger_humanize = true;
};
struct ClassAimPos {
	int classId = 0;
	float aim_position = 0.0f;
	float aim_position_x = 0.5f;
	bool enabled = false;
};
struct D5RenderConfig {
	std::string model_path = "";
	YoloVersion yolo_version = YoloVersion::V26;
	float confidence_threshold = 0.25f;
	float nms_threshold = 0.45f;
	int model_width = 640;
	int model_height = 640;
	float aim_smoothness = 0.35f;
	float aim_position = 0.0f;
	float aim_position_x = 0.5f;
	float aim_fov = 0.5f;
	bool aim_enabled = false;
		bool aim_toggle_enabled = false;
			bool toggle_sound = true; // 总开关切换提示音效开关
			KeyMode aim_toggle_key_mode = KeyMode::Single;
			int aim_toggle_key = 0x79; // VK_F10
			int aim_toggle_combo_key = 0xA2; // VK_LCONTROL
			bool show_overlay = true;
	int ui_toggle_key = 0x24; // VK_HOME — 主窗口显隐切换热键（全屏游戏下找回窗口）
	bool show_preview = true;
	KeyMode key_mode = KeyMode::Single;
	AimKey aim_key1;
	AimKey aim_key2;
	std::vector<AimKeySlot> aim_key_slots;
	int gpu_id = 0;
	int capture_mode = 0;
	int max_detections = 100;
	float speed_multiplier = 1.0f;
	float track_match_threshold = 150.0f;
	int track_max_lost = 75;   // [v215] 30→75: 1.5秒容忍枪口火焰导致的检测中断, 防轨迹判死换ID横跳
	float track_switch_hysteresis = 0.45f;
	int track_confirm_frames = 2;  // [v231] 新轨迹确认帧数: 连续检出≥N帧才允许成为锁定目标, 拦截单帧干扰框抢锁(1=关与旧版一致)
	bool scene_adapt_enabled = true;
	AimPreset aim_preset = AimPreset::Default;
	std::string makcu_port = "";
	int makcu_baud = 115200;
	DriverMode driver_mode = DriverMode::Generic;
	InferMode infer_mode = InferMode::TensorRT;
	int preview_x = -1;
	int preview_y = -1;
	int preview_w = 640;
	int preview_h = 640;
	std::vector<int> disabled_classes;
	bool per_class_aim_enabled = false;
	std::vector<ClassAimPos> class_aim_positions;
	bool anti_capture = false;
	TrtPrecision trt_precision = TrtPrecision::FP16;
	bool trt_mixed_precision = false;
	bool gpu_lock_clocks = true;
	float pid_kp = 0.30f;
	float pid_deadzone = 6.0f;
	bool move_randomize = false;  // [v217] 默认关闭拟人层(v204用户要求): 抖动=卡帧感+开枪摇晃元凶
	int infer_gpu_id = 0;  // [v218] 推理专用GPU: 0=与捕获同卡(兼容旧行为), 1/2=独立推理卡(与游戏物理解耦根治延迟)
	float move_max_step = 14.0f;  // [v215] 25→14: 单帧位移封顶, 吸附稳不过冲
	// [v178] 过冲自抑制参数(UI可调): 检测到误差反号自动压低增益直到零过冲
	bool os_enabled = true;
	float os_damp = 0.70f;   // 每次过冲后保留的力道比例(越小越狠)
	float os_base = 1.0f;    // 手动基准力道(1.0=完整UI增益)
	float move_inertia = 0.22f;  // [v234] 曲线平滑: smoothstep缓动, 近距轻柔远距直给
	float humanize_strength = 1.0f;  // [v235] 拟人抖动强度: 拉得越大抖动越厉害(0=无扰动 1=标准 2=强力)
	float move_accel_near = 30.0f;
	float move_accel_far = 100.0f;
	float move_fine_dist = 25.0f;
	bool quickscope_enabled = false;
	AimKey quickscope_key;
	int quickscope_fire_delay = 3;
	bool auto_trigger_enabled = false;
	AimKey auto_trigger_key;
	int auto_trigger_delay_ms = 50;
	float auto_trigger_threshold = 5.0f;
	int auto_trigger_interval_ms = 50;
	bool auto_trigger_humanize = true;
	std::vector<CustomPreset> custom_presets;
	int custom_preset_idx = -1;
	std::string getConfigPath() const;
	void saveToFile() const;
	bool loadFromFile();
	void saveToPath(const std::string& path) const;
	bool loadFromPath(const std::string& path);
};
