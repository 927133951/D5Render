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
	bool kalman_enabled = true;
	float kalman_process_noise = 8.0e7f;
	float kalman_measurement_noise = 2.0f;
	bool scene_adapt_enabled = true;
	float pid_kp = 0.30f;
	float pid_ki = 0.01f;
	float pid_kd = 0.20f;   // [v215] 0.15→0.20: 加强末段制动, 压制开枪震动时的过冲回拉
	float pid_deadzone = 6.0f;
	bool move_randomize = false;  // [v217] 默认关闭拟人层(v204用户要求): 抖动=卡帧感+开枪摇晃元凶
	int infer_gpu_id = 0;  // [v218] 推理专用GPU: 0=与捕获同卡(兼容旧行为), 1/2=独立推理卡(与游戏物理解耦根治延迟)
	float move_max_step = 14.0f;  // [v215] 25→14: 单帧位移封顶, 吸附稳不过冲
	float move_inertia = 0.22f;  // [v219] 0.35→0.22: 0.35把前馈输出拖慢35%导致移动目标跟不上
	float move_jitter_range = 1.5f;
	float move_jitter_scale = 0.8f;
	float move_accel_near = 30.0f;
	float move_accel_far = 100.0f;
	float move_fine_dist = 25.0f;
	// 目标速度前馈：追快速移动目标时把Kalman速度直接叠加进鼠标移动量
	bool ff_enabled = true;
	float ff_gain = 0.8f;
	float ff_target_speed = 150.0f; // px/s，超过此速度全额前馈，低于则按比例
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
	bool aim_humanize = false; // [v204] 拟人化输出层开关(默认关): ±25%幅度抖动/弧线/正弦/高斯, 关闭后贴附零抖动
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
	float kalman_process_noise = 8.0e7f;
	float kalman_measurement_noise = 2.0f;
	bool kalman_enabled = true;
	// ── [v230] CA+IMM+动态Q(aim_sim仿真10.7分最优参数移植) ──
	float kalman_q_min = 4.0e6f;       // 自适应Q下限(静止/匀速收紧降噪)
	float kalman_q_dyn = 8.0e7f;       // 自适应Q上限(高动态, jerk方差密度px^2/s^5)
	float dyn_innov_px = 4.0f;         // 创新量软门限起点(px)
	float dyn_innov_div = 16.0f;       // 创新量软门限斜率分母
	float dyn_acc_px = 300.0f;         // 加速度软门限起点(px/s^2)
	float dyn_amag_div = 2000.0f;      // 加速度软门限斜率分母
	float dyn_hold_s = 0.25f;          // 动态保持衰减时间常数(s)
	float imm_cv_q = 2.0e4f;           // IMM副模型CV加速度方差密度(px^2/s^3)
	float imm_lam = 0.3f;              // IMM似然EWMA新证据权重
	float imm_lam_dyn = 0.1f;          // IMM动态lam增量(机动段)
	float fb_boost = 0.06f;            // 反馈增益动态加成(K += fb_boost*dyn)
	float ff_decel_ratio = 0.6f;       // 强减速ff收缩比例(1.0=关)
	float decel_ax_px = 2000.0f;       // 强减速判据(|ax|, 与目标速度反号)
	float ff_ema_k = 0.70f;            // ff_vel EMA基准(动态升向1.0)
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
	float pid_kp_y = 0.30f;  // [v201] 纵向P增益(X/Y分轴: 横向跟枪, 纵向压稳, 手感独立调)
	float pid_ki = 0.01f;
	float pid_kd = 0.20f;   // [v215] 0.15→0.20: 加强末段制动, 压制开枪震动时的过冲回拉
	float pid_deadzone = 6.0f;
	bool move_randomize = false;  // [v217] 默认关闭拟人层(v204用户要求): 抖动=卡帧感+开枪摇晃元凶
	int infer_gpu_id = 0;  // [v218] 推理专用GPU: 0=与捕获同卡(兼容旧行为), 1/2=独立推理卡(与游戏物理解耦根治延迟)
	float move_max_step = 14.0f;  // [v215] 25→14: 单帧位移封顶, 吸附稳不过冲
	// [v178] 过冲自抑制参数(UI可调): 检测到误差反号自动压低增益直到零过冲
	bool os_enabled = true;
	float os_damp = 0.70f;   // 每次过冲后保留的力道比例(越小越狠)
	float os_base = 1.0f;    // 手动基准力道(1.0=完整UI增益)
	float move_inertia = 0.22f;  // [v219] 0.35→0.22: 0.35把前馈输出拖慢35%导致移动目标跟不上
	float move_jitter_range = 1.5f;
	float move_jitter_scale = 0.8f;
	float move_accel_near = 30.0f;
	float move_accel_far = 100.0f;
	float move_fine_dist = 25.0f;
	// 目标速度前馈：追快速移动目标时把Kalman速度直接叠加进鼠标移动量
	bool ff_enabled = true;
	float ff_gain = 0.8f;
	float ff_target_speed = 150.0f; // px/s，超过此速度全额前馈，低于则按比例
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
