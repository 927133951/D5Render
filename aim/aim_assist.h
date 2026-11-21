#pragma once
#include <vector>
#include <chrono>
#include <random>
#include "config/config.h"
#include "inference/yolo_postprocess.h"
#include "aim/kalman_filter.h"
#include "aim/target_tracker.h"
#include "aim/scene_analyzer.h"

struct AimTarget {
	int screenX;
	int screenY;
	float confidence;
	float distance;
};

class AutoTrigger {
public:
	AutoTrigger();
	void update(bool aimActive, bool hasTarget, float lockedX, float lockedY, const D5RenderConfig& config, float dt);
	bool shouldHold() const { return m_shouldHold; }
	void reset();
private:
	bool m_shouldHold = false;
	bool m_wasInZone = false;
	float m_delayTimerMs = 0;
	bool m_delaying = false;
	std::mt19937 m_rng{std::random_device{}()};
};

class AimAssist {
public:
	AimAssist();
	void update(const std::vector<Detection>& detections, int captureCX, int captureCY, int captureW, int captureH, const D5RenderConfig& config, bool aimActive, bool hasNewDetections, float recoilVelX = 0, float recoilVelY = 0, bool shooting = false, float frameDt = 0.016f);
	AimTarget getMoveDelta(float dt = 0.016f);
	bool hasTarget() const { return m_hasTarget; }
	float getLockedX() const { return m_lockedX; }
	float getLockedY() const { return m_lockedY; }
	float getCurrentGain() const { return m_currentGain; }
	float getSensRatioX() const { return m_sensRatioX; }
	float getSensRatioY() const { return m_sensRatioY; }
	float getSceneComplexity() const { return m_sceneAnalyzer.getSmoothedComplexity(); }
	int getLockedTrackId() const { return m_lockedTrackId; }
	// 预览显示用：暴露tracker轨迹(与update()同线程调用,无需加锁)。
	// 画轨迹而非原始检测→短暂缺检时Kalman预测补位,框不再闪烁
	const std::vector<TrackedTarget>& getTracksForDisplay() const { return m_tracker.getTracks(); }
	float getLockConfidence() const { return m_lockConfidence; }
	float getEffectiveFov() const { return m_aimFov; }
	float getLockedTargetSpeed() const { return m_lockedTargetSpeed; }
	float getLockedTargetSpeedX() const { return m_lockedTargetSpeedX; }
	float getLockedTargetSpeedY() const { return m_lockedTargetSpeedY; }
	bool checkQuickscopeTrigger(bool triggerKeyDown);
	void startQuickscopeFireDelay(int delayFrames);
	bool isQuickscopeFireReady() const { return m_qsFireCountdown == 0; }
	void clearQuickscopeFire() { m_qsFireCountdown = -1; }
	void resetAfterQuickscope();
	void resetAfterQuickscopeSnap(float snapX, float snapY);
	void tickQuickscopeDelay();
	void getHeadSnapPos(float& outX, float& outY, float aimPositionY) const;
	void updateFovOnly(const D5RenderConfig& config);
	void reset();
	void setLatencyMs(float ms) { m_latencyMs = ms; }
	void noteSent(int dx, int dy) { m_sentNowX += (float)dx; m_sentNowY += (float)dy; }
	AutoTrigger m_autoTrigger;
	AutoTrigger& getAutoTrigger() { return m_autoTrigger; }
private:
	bool m_hasTarget = false;
	bool m_aimActive = false;
	int m_lockedTrackId = -1;
	float m_lockedX = 0;
	float m_lockedY = 0;
	int m_lostFrames = 0;
	float m_lockConfidence = 0;
	int m_lockedClassId = -1;
	float m_lockedW = 0;
	float m_lockedH = 0;
	float m_moveX = 0;
	float m_moveY = 0;
	float m_prevMoveX = 0;
	float m_prevMoveY = 0;
	float m_prevErrorX = 0;
	float m_prevErrorY = 0;
	float m_currentGain = 0.20f;
	bool m_humanizeEnabled = false;  // [v217] 拟人层开关, 从move_randomize同步
	float m_aimFov = 0.5f;
	int m_lockFrames = 0;
	float m_smoothH = 0;
	float m_smoothW = 0;
	bool m_smoothInit = false;
	std::chrono::steady_clock::time_point m_lastUpdateTime;
	TargetTracker m_tracker;
	SceneAnalyzer m_sceneAnalyzer;
	SceneMetrics m_sceneMetrics;
	float m_prevAvgSpeed = 0;
	std::mt19937 m_rng{std::random_device{}()};
	float m_subPixelX = 0;
	float m_subPixelY = 0;
	float m_diagPrevMoveX = 0;
	float m_diagPrevMoveY = 0;
	float m_diagPrevDt = 0.016f;
	int m_diagInferSkipCount = 0;
	int m_diagStutterLogCooldown = 0;
	int m_lockLogCooldown = 0;
	int m_posBurstLogCooldown = 0;  // [v231] 位置突变拦截日志限频
	bool m_shooting = false;
	bool m_pinned = false;  // [v225] 滞回锁死态: 贴附后纹丝不动, 拉离1.8×死区才解锁
	float m_lockedTargetSpeed = 0;
	float m_lockedTargetSpeedX = 0;
	float m_lockedTargetSpeedY = 0;
	float m_lastLockedRawX = 0;
	float m_lastLockedRawY = 0;
	float m_sensRatioX = 1.0f;  // 游戏灵敏度估计：屏幕像素/鼠标计数(实测标定)
	float m_sensRatioY = 1.0f;
	// ── 换算比自标定(v152): 用"发送计数→下帧raw位移"实测c, 控制律按c归一 ──
	float m_sentNowX = 0;
	float m_sentNowY = 0;
	float m_sentHistX[3] = { 0, 0, 0 };  // [v228] Smith预估3帧在途链: [0]=上一帧 [1]=上上帧
	float m_sentHistY[3] = { 0, 0, 0 };
	float m_calibPrevRawX = 0;
	float m_calibPrevRawY = 0;
	int m_calibPrevTrackId = -1;
	int m_ratioSamples = 0;
	int m_ratioLogCooldown = 0;
	float m_lastDetectDt = 0.012f;
	float m_ffVelX = 0;  // [v226] 前馈速度(EMA平滑后的Kalman速度, 防估计噪声直通输出)
	float m_ffVelY = 0;
	// ── [v230] CA+IMM动态调度状态(aim_sim仿真最优参数移植) ──
	float m_dynState = 0;        // 动态门限状态(尖峰瞬时开, dyn_hold_s缓关)
	float m_dynAccX = 0;         // 锁定目标融合加速度(强减速刹ff用)
	float m_dynAccY = 0;
	float m_prevMeasErrX = 0;    // 上次computeMove测量误差(误差发散应急EMA用)
	float m_prevMeasErrY = 0;
	bool m_prevMeasSet = false;
	// ── [v229] 连续输出插值: 检测帧整步折算速率+预算, 主循环每帧按速率×时间片摊薄发射 ──
	// 总发送量与v228整步1:1(预算守恒), 分布从"1帧脉冲"变"连续流"→消除16~29fps步进感
	float m_ctrlRateX = 0;          // 控制速率(counts/s)=本检测帧整步/检测间隔
	float m_ctrlRateY = 0;
	bool m_freshCompute = false;    // 本检测帧computeMove刚产出新步(拟人层只按整步周期施加)
	float m_interpBudgetX = 0;      // 本检测周期剩余插值预算(counts), 累计发射≤预算防超发
	float m_interpBudgetY = 0;
	std::chrono::steady_clock::time_point m_lastOutputTime{};  // 上次输出时刻(实测主循环帧间dt)
	int m_outEmitCount = 0;         // 非零输出发射计数(输出率诊断)
	// ── [v186] 弹跳捕获诊断器: 环形缓冲64帧, 误差反号时dump到日志定位根因 ──
	float m_diagRing[64][6];        // errX errY mvX mvY gainScale boxH
	int m_diagRingIdx = 0;
	bool m_diagRingFull = false;
	int m_diagBounceCd = 0;
	int m_frozenCnt = 0;            // [v188] 误差冻结+输出非零 的连续检测帧计数(输出失效告警)
	// ── [v187] 检测流到达率诊断: 定位实战检测帧掉到16fps的根因 ──
	int m_streamCount = 0;
	std::chrono::steady_clock::time_point m_streamLastReport{};
	bool m_ffBrakeActive = false;
	float m_userSuppress = 0;
	int m_switchCooldown = 0;
	// ── 锁定粘滞(v120): "锁一个到底"三件套 ──
	int m_regrabOldId = -1;      // 重捕弹回: 被换掉的原轨迹ID(疑似抓错人)
	int m_regrabGrace = 0;       // 弹回剩余帧数(3帧内原轨迹恢复检测→弹回原目标)
	float m_regrabOldRawX = 0;   // 换人瞬间原目标最后已知位置(弹回位置比对基准)
	float m_regrabOldRawY = 0;
	std::chrono::steady_clock::time_point m_lastDetectTime{};  // 上次"有检测结果"的时刻——tracker/Kalman的真实dt基准(仅修测量，不动输出节拍)
	int m_selectBestFrame = 0;
	int m_sceneAnalyzeFrame = 0;
	bool m_quickscopeLastKey = false;
	int m_qsFireCountdown = -1;
	float m_latencyMs = 30.0f;
	// ── P0 拟人化输出层状态（防行为统计检测）──
	// 纯P控制输出是完美几何衰减曲线(move∝0.9^n)——统计分类器一眼假。
	// 这些字段驱动输出层扰动，不进入控制环。
	float m_arcCurv = 0.04f;        // 当前弧线曲率（新锁定时重掷，带随机符号）
	float m_arcLateral = 0;         // 弧线累积侧偏量（±5px 封顶防漂移）
	float m_wobblePhase = 0;        // 正弦微扰相位（随机推进打破帧对齐）
	int m_prevHumanLockFrames = 0;  // 检测"新锁定"（lockFrames 回落=换目标/重锁）
	int m_oscCooldown = 0;          // 过冲强阻尼剩余帧数：误差反号时触发，移动量×0.35+清惯性
	float m_flyZoneDist = 62.0f;    // [v155] 匀速飞行区边界(px)=fineDist×2.5，区外满速直线飞行
	std::chrono::steady_clock::time_point m_lastMoveTime{};  // [v155] 上次getMoveDelta调用时刻（真实dt速度补偿）
	bool m_moveTimeInit = false;
	float m_aimSmRawX = 0;          // [v160] 锁定目标aimpoint轻EMA(α=0.6), 削跑动检测框高频跳动
	float m_aimSmRawY = 0;
	bool m_aimSmRawInit = false;
	int m_stallCount = 0;           // [v161] 步长保险丝: 连续停滞帧数(发送counts但误差几乎不收缩)
	// ── [v162] 极简控制核: 唯一控制变量 = 回路增益g(计数/像素), 闭环自适应 ──
	float m_loopGain = 0.30f;       // 回路增益: move_counts = g×error; [v173] 0.12→0.30补偿钳位减半带来的力道损失
	float m_gainAtSend = 0.30f;     // 上次发送时的g(闭环比值计算基准)
	float m_ratioEma = -1.0f;       // 实测收缩/预期收缩 的EMA(<0=未初始化)
	int m_overCount = 0;            // [v163] 连续越界(误差反号)计数
	int m_overCooldown = 0;         // [v165] 越界回归阻尼剩余帧数(移动量×0.4, 斩断连续穿越链)
	float m_prevServoDist = 0;      // [v170] 上帧伺服误差距离(刚度调度的|Δ误差|基准)
	float m_integX = 0;             // [v197] 积分项(抗饱和钳位±3px·s, Ki消贴附稳态偏差)
	float m_integY = 0;
	// ── [v199] 贴附抖动遥测: 误差距离的滑动统计, 量化贴附摇晃幅度 ──
	float m_jitPrevDist = -1.0f;    // 上检测帧误差距离(<0=未初始化)
	float m_jitAbsSum = 0;          // |帧间Δ距离|累计(抖动总量)
	int m_jitFrames = 0;            // 采样帧数
	int m_jitReportCd = 0;          // 报告冷却
	// ── [v177] 纯P控制核的过冲自抑制 ──
	float m_gainScale = 1.0f;       // 过冲自抑制系数: 误差反号→×0.70, 范围[0.10,1.0], 零过冲最终保证
	float m_shrinkEma = -1.0f;      // 误差收缩率EMA(<-0.5=未初始化)

	int selectBestTrack(const std::vector<Detection>& detections, int captureCX, int captureCY, int captureW, int captureH, const D5RenderConfig& config);
	float computeTrackScore(const TrackedTarget& track, int captureCX, int captureCY, int captureW, int captureH, const D5RenderConfig& config) const;
	void computeMove(float errorX, float errorY, float dt, const D5RenderConfig& config, float recoilVelX = 0, float recoilVelY = 0);
};
