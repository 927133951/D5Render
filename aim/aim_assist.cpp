#include "aim_assist.h"
#include "gui/app_log.h"
#include "utils/obfstr.h"
#include <algorithm>
#include <cmath>

static float getAimPosYForClass(int classId, const D5RenderConfig& config) {
	if (config.per_class_aim_enabled) {
		for (const auto& o : config.class_aim_positions) {
			if (o.enabled && o.classId == classId) {
				return o.aim_position;
			}
		}
	}
	return config.aim_position;
}

AutoTrigger::AutoTrigger() : m_rng(std::random_device{}()) {}

void AutoTrigger::update(bool aimActive, bool hasTarget, float lockedX, float lockedY, const D5RenderConfig& config, float dt) {
	m_shouldHold = false;
	if (!config.auto_trigger_enabled || !aimActive || !hasTarget) {
		m_wasInZone = false;
		m_delaying = false;
		m_delayTimerMs = 0;
		return;
	}
	if (m_wasInZone) {
		m_shouldHold = true;
		return;
	}
	float dist = std::sqrt(lockedX * lockedX + lockedY * lockedY);
	float threshold = config.auto_trigger_threshold;
	if (config.auto_trigger_humanize) {
		std::uniform_real_distribution<float> thJitter(-0.5f, 0.5f);
		threshold += thJitter(m_rng);
	}
	bool inZone = (dist <= threshold);
	if (!inZone) return;
	float delayMs = (float)config.auto_trigger_delay_ms;
	if (config.auto_trigger_humanize) {
		std::uniform_real_distribution<float> delayJitter(0.0f, 15.0f);
		delayMs += delayJitter(m_rng);
	}
	m_delayTimerMs = delayMs;
	m_delaying = true;
	if (m_delaying) {
		m_delayTimerMs -= dt * 1000.0f;
		if (m_delayTimerMs <= 0) {
			m_delaying = false;
		} else {
			return;
		}
	}
	m_shouldHold = true;
	m_wasInZone = true;
}

void AutoTrigger::reset() {
	m_shouldHold = false;
	m_wasInZone = false;
	m_delaying = false;
	m_delayTimerMs = 0;
}

AimAssist::AimAssist() : m_rng(std::random_device{}()) {
	m_lastUpdateTime = std::chrono::steady_clock::now();
}

void AimAssist::updateFovOnly(const D5RenderConfig& config) {
	m_aimFov = config.aim_fov;
}

float AimAssist::computeTrackScore(const TrackedTarget& track, int captureCX, int captureCY, int captureW, int captureH, const D5RenderConfig& config) const {
	// 用检测框中心(rawCx/rawCy)计算距离：
	// - 不用aimpoint(含aim_position偏移→距离算错)
	// - 不用Kalman cx/cy(含被视角旋转污染的预测)
	float dx = track.rawCx - captureCX;
	float dy = track.rawCy - captureCY;
	float dist = std::sqrt(dx * dx + dy * dy);
	float fovRadius = captureW * m_aimFov / 2.0f;
	bool isLockedTarget = (track.trackId == m_lockedTrackId && m_lockedTrackId >= 0);
	if (dist > fovRadius && !isLockedTarget) return 1e9f;

	float score;
	// 距离优先：只用原始距离（v106原版逻辑）
	score = dist;

	// 锁定目标迟滞：已锁定的目标分数大幅降低，优先保持
	if (isLockedTarget) {
		float lockStrength = 0.5f + config.track_switch_hysteresis * 0.4f;
		score *= (1.0f - lockStrength);
	} else if (m_lockedTrackId >= 0) {
		score *= 1.5f;
	}
	return score;
}

int AimAssist::selectBestTrack(const std::vector<Detection>& detections, int captureCX, int captureCY, int captureW, int captureH, const D5RenderConfig& config) {
	const auto& tracks = m_tracker.getTracks();
	if (tracks.empty()) return -1;
	float bestScore = 1e9f;
	int bestTrackId = -1;
	for (const auto& track : tracks) {
		if (track.lostFrames > 0) continue;
		// [v231] 新轨迹确认门限: 连续检出≥N帧才可被选为锁定目标。
		// 拦截枪口火焰/烟雾/特效等单帧干扰框抢锁(它们通常只出现1-2帧)。
		// 锁定保持走 findTrackById 路径不经过此处, 不受影响; 宽网捞回/重捕走身份验证路径也不受影响。
		// track_confirm_frames=1 时等于旧版行为(向后兼容)
		if (track.hitFrames < std::max(1, config.track_confirm_frames)) continue;
		float score = computeTrackScore(track, captureCX, captureCY, captureW, captureH, config);
		if (score < bestScore) {
			bestScore = score;
			bestTrackId = track.trackId;
		}
	}
	return bestTrackId;
}

void AimAssist::update(const std::vector<Detection>& detections, int captureCX, int captureCY, int captureW, int captureH, const D5RenderConfig& config, bool aimActive, bool hasNewDetections, float recoilVelX, float recoilVelY, bool shooting, float frameDt) {
	(void)frameDt;
	m_aimActive = aimActive;
	m_shooting = shooting;
	m_aimFov = config.aim_fov;
	if (m_switchCooldown > 0) m_switchCooldown--;
	auto now = std::chrono::steady_clock::now();
	float dt = (float)std::chrono::duration<double>(now - m_lastUpdateTime).count();
	m_lastUpdateTime = now;
	if (dt <= 0.0001f) dt = 0.001f;
	if (dt > 0.1f) dt = 0.1f;

	if (!aimActive && m_lockedTrackId >= 0) {
		m_lockedTrackId = -1;
		m_pinned = false;  // [v225]
		m_lockedClassId = -1;
		m_lockConfidence = 0;
		m_lockFrames = 0;
		m_hasTarget = false;
		m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
		m_smoothInit = false;
		m_smoothW = 0; m_smoothH = 0;
		m_lockedTargetSpeed = 0;
		m_lockedTargetSpeedX = 0; m_lockedTargetSpeedY = 0; m_lastLockedRawX = 0; m_lastLockedRawY = 0;
		m_ffVelX = 0; m_ffVelY = 0; m_sentNowX = 0; m_sentNowY = 0;
		m_sentHistX[0] = 0; m_sentHistX[1] = 0; m_sentHistY[0] = 0; m_sentHistY[1] = 0;  // [v228]
		m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0; m_freshCompute = false;  // [v229] 清插值态
		m_switchCooldown = 0;
	}

	if (hasNewDetections) {
		// [v187] 检测流到达率统计: 实战数据实测到达率仅~16fps而推理FPS>100,
		// 差值=丢弃/排队损耗——这是实战摇晃与"卡住不动"的主因, 持续输出定位数据
		m_streamCount++;
		{
			auto nowS = std::chrono::steady_clock::now();
			if (m_streamLastReport.time_since_epoch().count() == 0) m_streamLastReport = nowS;
			double elS = std::chrono::duration<double>(nowS - m_streamLastReport).count();
			if (elS >= 2.0) {
				LOG_INFO(OBS1(u8"[检测流] 到达率=%.0f fps 输出率=%.0f fps (输出率>到达率=插值连续输出生效)").c_str(),
					m_streamCount / elS, m_outEmitCount / elS);
				m_streamCount = 0;
				m_outEmitCount = 0;
				m_streamLastReport = nowS;
			}
		}
		m_diagInferSkipCount = 0;
		float avgSpeed = 0;
		const auto& existingTracks = m_tracker.getTracks();
		if (!existingTracks.empty()) {
			float totalSpeed = 0;
			for (const auto& t : existingTracks) totalSpeed += t.lastSpeed;
			avgSpeed = totalSpeed / existingTracks.size();
		}
		m_prevAvgSpeed = avgSpeed;
		m_sceneAnalyzeFrame++;
		if (m_sceneAnalyzeFrame >= 3) {
			m_sceneMetrics = m_sceneAnalyzer.analyze(detections, captureW, captureH, avgSpeed);
			m_sceneAnalyzeFrame = 0;
		}
		float baseMatchThresh = config.track_match_threshold;
		if (config.scene_adapt_enabled) {
			m_tracker.setMatchThreshold(baseMatchThresh * 0.6f + m_sceneAnalyzer.getSmoothedComplexity() * baseMatchThresh * 0.8f);
		} else {
			m_tracker.setMatchThreshold(baseMatchThresh);
		}
		m_tracker.setMaxLostFrames(config.track_max_lost);
		m_tracker.setKalmanEnabled(config.kalman_enabled);
		if (config.kalman_enabled) {
			float dynamicAhead = m_latencyMs / 1000.0f * 1.2f;
			dynamicAhead = std::clamp(dynamicAhead, 0.010f, 0.200f);
			float qProc = std::clamp(config.kalman_process_noise, config.kalman_q_min, config.kalman_q_dyn);  // [v230] CA刻度防护旧配置值
			m_tracker.setKalmanParams(qProc, config.kalman_measurement_noise, dynamicAhead);
			m_tracker.setImmParams(config.imm_cv_q, config.imm_lam);
		}
		// dt基准修复：tracker/Kalman要用"距上次有检测帧"的真实间隔。
		// 旧代码用渲染帧间隔——高刷渲染下6.9/13.9ms交替传入，速度估计带±40%噪声
		// → 匹配门限抖动 → 闪断 → 停手卡顿。只修测量基准，不动输出节拍。
		auto detectNow = std::chrono::steady_clock::now();
		float detectDt = 0.008f;
		if (m_lastDetectTime.time_since_epoch().count() > 0) {
			detectDt = (float)std::chrono::duration<double>(detectNow - m_lastDetectTime).count();
			if (detectDt < 0.001f) detectDt = 0.001f;
			if (detectDt > 0.050f) detectDt = 0.050f;
		}
		m_lastDetectTime = detectNow;
		// [v192] dt改EMA平滑(α=0.03, τ≈0.6秒): dtNorm需要稳定的检测间隔基准。
		// 此前m_lastDetectDt从未被赋值(恒0.012)→dtNorm恒=0.6→钳位0.7→输出恒定
		// 打七折(隐藏的力道损失)。EMA后dtNorm跟随到达率慢变化, 单帧抖动被滤除
		m_lastDetectDt = (m_lastDetectDt > 0.0001f)
			? (m_lastDetectDt * 0.97f + detectDt * 0.03f)
			: detectDt;
		m_tracker.update(detections, detectDt, captureW, captureH);
	}

	if (m_lockedTrackId >= 0) {
		TrackedTarget locked;
		bool found = m_tracker.findTrackDataById(m_lockedTrackId, locked);

		// 重新获取：丢检≥2帧才启动。1帧闪断交给tracker自行重匹配——
		// 立刻重捕获会在过期位置附近抓到相邻目标=飞过最近目标跳到别人(主 bug)
		bool needReacquire = !found || locked.lostFrames >= 2;
		if (needReacquire && hasNewDetections) {
			// ── 纯位置重捕(不再用Kalman速度外推) ──
			// 旧版: 搜索中心 = 最后位置 + Kalman速度×时间。速度被我们自己的甩枪旋转污染,
			// 外推中心跟着旋转方向飘 → 半径内抓到相邻目标 → 乱锁切换(实测主因)。
			// 新版: 搜索中心 = 最后已知原始位置, 半径=半框+70px(覆盖丢检期间目标自身移动
			// +检测噪声), 同类检查兜底。目标真实快速横移时重捕成功率略降, 但不会乱切。
			float predX = m_lastLockedRawX;
			float predY = m_lastLockedRawY;
			const auto& tracks = m_tracker.getTracks();
			float bestDist = 1e9f;
			int bestId = -1;
			for (const auto& t : tracks) {
				if (t.lostFrames > 0) continue;  // 只搜新鲜检测
				float dx = t.rawCx - predX;
				float dy = t.rawCy - predY;
				float d = dx * dx + dy * dy;
				if (d < bestDist) { bestDist = d; bestId = t.trackId; }
			}
			// [v222] 开枪时重捕半径+40px: 后座抖动+击中后仰使检测框跳变加大, 原半径重捕失败→脱离
			float posMatchRadius = m_smoothW * 0.5f + (m_shooting ? 110.0f : 70.0f);
			if (bestId >= 0 && bestDist < posMatchRadius * posMatchRadius) {
				TrackedTarget bestTrack;
				m_tracker.findTrackDataById(bestId, bestTrack);
				if (bestTrack.classId == m_lockedClassId) {
					m_tracker.findTrackDataById(bestId, locked);
					found = true;
					m_lockedTrackId = bestId;
					// 不重置m_prevMoveX/Y——保持动量，准星继续平滑移动不停顿
				}
			}
		}

		// 锁保持：容忍较长丢检(Kalman预测补位)，超过才释放。
		// 切换原则：锁定目标存活(lostFrames==0)时绝不切换——像人一样打一个到底。
		// 只有锁定目标丢检12帧以上(确认消失而非闪断)才切到最近的活目标。
		// 8→12：小/远目标缺检成串(~67ms+)是常态，8帧太短→闪断时乱切=红框跳目标
		// [v215] 12→5: 目标消失(击杀)后100ms内瞬间切换; 安全网=下方宽网捞回(含半死轨迹)，
		// 5帧触发时先在原位置±90px找原目标，找到即回锁不丢目标，找不到(真死)才切最近
		// [v222] 开枪状态动态化: 枪口火焰/烟雾/击中特效使检测闪断成串(到达率50→32fps实测),
		// 8帧即释放锁定→目标还在视野内却脱离吸附。开枪时容忍20帧(400ms@50fps)让特效遮挡过去;
		// 不开枪保持8帧维持击杀后快速换目标
		const int aimLockLostTimeout = m_shooting ? 20 : 8;
		// [v221] 5→3: 目标死亡丢检3帧即进入切换评估, 检测率50fps时60ms切换(原100ms)
		// [v222] 开枪时3→6: 开枪期间检测抖动闪断是常态, 3帧即切会被枪口火焰诱骗跳目标
		const int switchLostThreshold = m_shooting ? 6 : 3;
		if (found && locked.lostFrames <= aimLockLostTimeout) {
			m_lostFrames = 0;
			m_lockFrames++;
			if (m_lockLogCooldown > 0) m_lockLogCooldown--;
			m_lockedClassId = locked.classId;
			// [v227] 检测框突变拒绝: 枪口火焰/烟雾把锁定框膨胀数倍(boxH 64→406实测),
			// 该帧位置是假目标点, 用它算误差/速度/重捕中心=准星被拽走→开枪后锁不上主因
			float boxRatio = (m_smoothH > 1.0f) ? (locked.h / m_smoothH) : 1.0f;
			bool boxBurst = (boxRatio > 2.5f);
			// [v231] 位置突变拒绝: 框尺寸不变但中心大跳(遮挡偏移框/误检框)——该帧中心是假目标点。
			// 仅在非甩枪场景生效: 目标Kalman速度<800px/s说明相机与目标均未大幅移动,
			// 此时位置单帧大跳(>max(3倍框宽,180px))必为检测干扰。与boxBurst同路径冻结本帧,
			// 只影响当前帧、无跨帧状态, 结构上不可能产生振荡
			bool posBurst = false;
			float rawJump = 0.0f, jumpLimit = 0.0f;
			if (!boxBurst && m_lockFrames > 5) {
				float jumpDx = locked.rawCx - m_lastLockedRawX;
				float jumpDy = locked.rawCy - m_lastLockedRawY;
				rawJump = std::sqrt(jumpDx * jumpDx + jumpDy * jumpDy);
				jumpLimit = std::max(m_smoothW * 3.0f, 180.0f);
				float tgtSpeed = locked.kalman.isInitialized() ? locked.kalman.getSpeed() : 0.0f;
				if (tgtSpeed < 800.0f && rawJump > jumpLimit) posBurst = true;
			}
			bool burstNow = (boxBurst || posBurst);
			if (posBurst && m_posBurstLogCooldown <= 0) {
				LOG_INFO(u8"[抗干扰] 锁定目标位置突变已拦截(跳变%.0fpx>限值%.0fpx, 冻结1帧)", rawJump, jumpLimit);
				m_posBurstLogCooldown = 100;
			}
			if (m_posBurstLogCooldown > 0) m_posBurstLogCooldown--;
			if (!burstNow) {
				m_lockedW = locked.w;
				m_lockedH = locked.h;
			}
			m_lockConfidence = locked.lockConfidence;
			if (!burstNow) m_lockedTargetSpeed = locked.lastSpeed;
			// 记录最后已知原始位置，用于丢检时重新搜索(突变帧冻结防污染重捕中心)
			if (!burstNow) {
				m_lastLockedRawX = locked.rawCx;
				m_lastLockedRawY = locked.rawCy;
			}
			if (!burstNow) {
				if (locked.kalman.isInitialized()) {
					m_lockedTargetSpeedX = locked.kalman.getVx();
					m_lockedTargetSpeedY = locked.kalman.getVy();
				} else {
					m_lockedTargetSpeedX = 0;
					m_lockedTargetSpeedY = 0;
				}
			}

			bool inProtectionPeriod = (m_lockFrames <= 5);

			// ── 切换的两条路径 ──
			// A) 锁定目标丢检≥8帧(确认消失) → 切到最近的活目标 (v106原版)
			// B) 途中纠偏：还在接近阶段(距锁定目标>60px未安定)，出现"明显更近"
			//    (至少近40px、或近当前距离35%以上)的活目标 → 改锁它。
			//    → 准心永远奔向最近的目标，不会一路飞过近目标去抓远的。
			//    安定后(≤60px)纯粘滞不换目标 → 保持v106"锁一个到底"的手感。
			bool lostEnough = (locked.lostFrames >= switchLostThreshold);
			float curDist = 1e9f;
			if (locked.lostFrames == 0) {
				float cdx = locked.rawCx - captureCX;
				float cdy = locked.rawCy - captureCY;
				curDist = std::sqrt(cdx * cdx + cdy * cdy);
			}
			// [v222] 开枪时禁用途中纠偏: 连续开枪期间准星必须咬死当前目标, 严禁被更近目标诱骗横跳
			bool approaching = (locked.lostFrames == 0 && curDist > 60.0f && !m_shooting);
			int bestTrackId = -1;
			if (m_switchCooldown <= 0 && hasNewDetections && !inProtectionPeriod && (lostEnough || approaching)) {
				if (lostEnough) {
				// ── 切走前先"宽网捞回原目标" ──
				// 旧版: 以Kalman预测位置(locked.cx/cy, 被甩枪旋转污染)为中心。
				// 新版: 以最后已知原始位置为中心——不用污染预测, 同类检查兜底。
				// 半框+90px内找同类新鲜轨迹：原目标重现(哪怕位置略偏)优先回锁；
				// 捞不到才切到最近的其他目标。
				// 专治：小目标缺检成串→8帧就切走→红框跳到别人身上
					float rcx = m_lastLockedRawX;
					float rcy = m_lastLockedRawY;
					// [v222] 开枪时捞回半径90→130: 击中后仰+后座抖动使原目标重现位置偏移更大
					float wideR = m_smoothW * 0.5f + (m_shooting ? 130.0f : 90.0f);
					const auto& tracks = m_tracker.getTracks();
					float bd = wideR * wideR;
					int regrabId = -1;
					for (const auto& t : tracks) {
						if (t.lostFrames > 12 || t.trackId == m_lockedTrackId) continue;  // [v215] ≤12帧半死轨迹可捞回
						if (t.classId != m_lockedClassId) continue;
						float dx = t.rawCx - rcx;
						float dy = t.rawCy - rcy;
						float d = dx * dx + dy * dy;
						if (d < bd) { bd = d; regrabId = t.trackId; }
					}
					if (regrabId >= 0) bestTrackId = regrabId;
					else bestTrackId = selectBestTrack(detections, captureCX, captureCY, captureW, captureH, config);
				} else {
					// 途中纠偏：按原始距离(不带迟滞加权)找"明显更近"的活目标
					// [v221] 40px/35%→28px/30%: 途中出现更近目标时纠偏门槛降低, 响应更快
					float margin = std::max(28.0f, curDist * 0.30f);
					const auto& tracks = m_tracker.getTracks();
					float bestD = curDist - margin;
					for (const auto& t : tracks) {
						if (t.lostFrames > 0 || t.trackId == m_lockedTrackId) continue;
						// [v231] 纠偏同样遵守确认门限: 未确认的1帧干扰框不得成为纠偏目标
						if (t.hitFrames < std::max(1, config.track_confirm_frames)) continue;
						float dx = t.rawCx - captureCX;
						float dy = t.rawCy - captureCY;
						float d = std::sqrt(dx * dx + dy * dy);
						if (d < bestD) { bestD = d; bestTrackId = t.trackId; }
					}
				}
				if (bestTrackId >= 0 && bestTrackId != m_lockedTrackId) {
					TrackedTarget newTarget;
					if (m_tracker.findTrackDataById(bestTrackId, newTarget)) {
						float newDx = (newTarget.cx - captureCX);
						float newDy = (newTarget.cy - captureCY);
						float newDist = std::sqrt(newDx * newDx + newDy * newDy);
						float fovRadius = captureW * m_aimFov / 2.0f;
						if (newDist < fovRadius) {
							m_lockedTrackId = bestTrackId;
							m_lockedClassId = newTarget.classId;
							m_lockedW = newTarget.w;
							m_lockedH = newTarget.h;
							m_lockConfidence = newTarget.lockConfidence;
							m_lockFrames = 1;
							m_lockedTargetSpeed = newTarget.lastSpeed;
							m_lastLockedRawX = newTarget.rawCx;
							m_lastLockedRawY = newTarget.rawCy;
							if (newTarget.kalman.isInitialized()) {
								m_lockedTargetSpeedX = newTarget.kalman.getVx();
								m_lockedTargetSpeedY = newTarget.kalman.getVy();
							} else {
								m_lockedTargetSpeedX = 0;
								m_lockedTargetSpeedY = 0;
							}
							m_smoothH = newTarget.h;
							m_smoothW = newTarget.w;
							m_smoothInit = true;
							m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
							m_pinned = false;  // [v225] 切换目标解除锁死
							m_ffVelX = 0; m_ffVelY = 0; m_sentNowX = 0; m_sentNowY = 0;
							m_sentHistX[0] = 0; m_sentHistX[1] = 0; m_sentHistY[0] = 0; m_sentHistY[1] = 0;  // [v228] 清3帧在途链: 新目标误差不含旧目标在途移动
							m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0; m_freshCompute = false;  // [v229] 清插值态
							locked = newTarget;
							m_switchCooldown = 6;  // [v221] 8→6: 切换后120ms短冷却, 仍防闪断连锁
							if (m_lockLogCooldown <= 0) {
								m_lockLogCooldown = 30;
								LOG_INFO(OBS1(u8"切换目标 ID:%d 类:%d").c_str(), bestTrackId, newTarget.classId);
							}
						}
					}
				}
			}

			// ── 核心修复：目标丢失 = 立即停手，不追幽灵位置 ──
			// 旧逻辑：lostFrames 1~15 期间仍朝 rawCx/rawCy（过期位置）移动
			// → 目标若隐若现时准星在"幽灵"与"真实"间反复横跳 = 摇晃
			// 新逻辑：lostFrames > 0（重获取也失败）→ 立即零移动，保持准星不动
			//        lostFrames == 0（目标鲜活或重获取成功）→ 正常追踪，瞬间锁回
			if (locked.lostFrames == 0 && hasNewDetections) {
				// 直接用rawCx(原始检测位置)，不做EMA平滑
				// EMA会造成误差信号滞后→高增益过冲/低增益无力，是"毫无力气"的根因
				// 检测噪声由死区+安全钳位(getMoveDelta)处理，不需要EMA
				float rawX = locked.rawCx;
				float rawY = locked.rawCy;
				// ── [v196] 在线自标定: 已停用 ──
				// 采样公式 c=dRaw/N 存在符号级错误(自瞄移动时目标屏幕位移与发送反向),
				// 学出负c被限幅丢弃, 偶发正样本全是目标移动噪声→换算比被污染成随机值
				// 直接调制所有输出幅度=严重摇晃(v197实战实锤)。恢复标定需改用
				// "误差收缩量/发送量"公式并限静止靶, 暂以手填counts360为唯一标定途径
				m_calibPrevTrackId = m_lockedTrackId;
				m_calibPrevRawX = rawX;
				m_calibPrevRawY = rawY;
				if (!burstNow) {
					if (!m_smoothInit) {
						m_smoothH = locked.h;
						m_smoothW = locked.w;
						m_smoothInit = true;
					} else {
						m_smoothH = m_smoothH * 0.85f + locked.h * 0.15f;
						m_smoothW = m_smoothW * 0.85f + locked.w * 0.15f;
					}
				}
				// [v227] 突变帧: 沿用上帧目标点继续按旧误差修正, 位置/框尺寸全冻结
				float targetX, targetY;
				if (burstNow) {
					targetX = m_lockedX;
					targetY = m_lockedY;
				} else {
					targetX = (rawX - m_smoothW / 2.0f + m_smoothW * config.aim_position_x) - captureCX;
					targetY = (rawY - m_smoothH / 2.0f + m_smoothH * getAimPosYForClass(m_lockedClassId, config)) - captureCY;
				}
				// ── [v199] 贴附抖动遥测: 误差<25px(贴附区)时统计帧间Δ距离 ──
				// 目标静止+准星静止时Δ≈0, Δ全由检测噪声+输出抖动贡献
				// 每60检测帧(≈1.2秒)输出平均帧间跳动, 量化摇晃幅度、验证死区/滤波效果
				{
					float distNow = std::sqrt(targetX * targetX + targetY * targetY);
					if (distNow < 25.0f) {
						if (m_jitPrevDist >= 0.0f) {
							m_jitAbsSum += std::abs(distNow - m_jitPrevDist);
							m_jitFrames++;
						}
						m_jitPrevDist = distNow;
					} else {
						m_jitPrevDist = -1.0f;  // 离开贴附区, 重置采样
						m_jitAbsSum = 0;
						m_jitFrames = 0;
					}
					if (m_jitReportCd > 0) m_jitReportCd--;
					if (m_jitFrames >= 60 && m_jitReportCd <= 0) {
						m_jitReportCd = 300;  // ≈6秒限频
						LOG_INFO(OBS1(u8"[贴附抖动] 平均帧间跳动=%.2fpx (采样%d帧, 目标静止贴附期; >2px=肉眼可见晃动)").c_str(),
							m_jitAbsSum / m_jitFrames, m_jitFrames);
						m_jitAbsSum = 0;
						m_jitFrames = 0;
					}
				}
				// [v186] 弹跳捕获诊断器(只记录不动控制): 环形缓冲64帧, 检测到误差反号
				// (准星穿越目标=弹跳瞬间)自动dump前后64帧的误差/输出/增益/框高到日志,
				// 用数据区分根因: 换算比放大(mv持续>err) / 检测框剧变(boxH与err跳变) / os摆动(gain锯齿)
				{
					if (m_diagBounceCd > 0) m_diagBounceCd--;
					float* r = m_diagRing[m_diagRingIdx];
					r[0] = targetX; r[1] = targetY;
					r[2] = m_moveX; r[3] = m_moveY;
					r[4] = m_gainScale; r[5] = m_smoothH;
					int prevIdx = (m_diagRingIdx + 63) % 64;
					if (m_diagRingFull && m_diagBounceCd <= 0) {
					const float* p = m_diagRing[prevIdx];
					// [v188] 输出失效检测: 误差几乎不动(|Δerr|<0.8px)但输出显著(|mv|>2)
					// 连续8检测帧→鼠标输出未被游戏应用(驱动间歇失灵/输入被吞), 记日志取证
					float dErr = std::sqrt((r[0]-p[0])*(r[0]-p[0]) + (r[1]-p[1])*(r[1]-p[1]));
					float mvMag = std::sqrt(r[2]*r[2] + r[3]*r[3]);
					if (dErr < 0.8f && mvMag > 2.0f) {
						if (++m_frozenCnt == 8) {
							m_frozenCnt = 4;  // 每8帧续告警一次, 防刷屏
							LOG_WARN(OBS1(u8"[输出失效] 误差冻结%dx但持续输出%.1f计数/帧, 鼠标移动未被游戏应用(驱动/输入侧)").c_str(),
								8, mvMag);
						}
					} else {
						m_frozenCnt = 0;
					}
					bool crossedNow = (std::abs(r[0]) > 3.0f && std::abs(p[0]) > 3.0f && r[0] * p[0] < 0.0f) ||
						(std::abs(r[1]) > 3.0f && std::abs(p[1]) > 3.0f && r[1] * p[1] < 0.0f);
						if (crossedNow) {
							m_diagBounceCd = 600;  // 限频≈12秒一次
							// [v187] 降载: 只dump反号前后各8帧(共16行), 64行刷屏会阻塞主循环
							LOG_WARN(OBS1(u8"[弹跳捕获] 误差反号 boxH=%.0f err=(%.1f,%.1f)").c_str(), r[5], r[0], r[1]);
							int start = (m_diagRingIdx + 64 - 8) % 64;
							for (int k = 0; k < 16; k++) {
								const float* d = m_diagRing[(start + k) % 64];
								LOG_WARN(OBS2(u8"[弹跳] %02d err(%7.1f,%7.1f) mv(%6.2f,%6.2f) gain=%.2f boxH=%.0f").c_str(),
									k, d[0], d[1], d[2], d[3], d[4], d[5]);
							}
						}
					}
					m_diagRingIdx = (m_diagRingIdx + 1) % 64;
					if (m_diagRingIdx == 0) m_diagRingFull = true;
				}
				m_lockedX = targetX;
				m_lockedY = targetY;
				if (aimActive) {
					computeMove(m_lockedX, m_lockedY, dt, config, recoilVelX, recoilVelY);
					m_prevMoveX = m_moveX;
					m_prevMoveY = m_moveY;
				} else {
					m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
				}
			} else if (locked.lostFrames == 0) {
				// [v226] 跳帧期间不再计算移动: getMoveDelta只在新检测帧取走输出,
				// 此处computeMove的输出永不发出, 反而会提前消费在途记账m_sentNow
				//(下个检测帧扣除丢失=欠修正)并污染m_prevMove → 空过, 保持上帧输出
			} else {
				// 目标真丢失(lostFrames>0)：立即停手
				// 不追幽灵位置 → 消除若隐若现时的摇晃；目标重现后lostFrames归零→瞬间锁回
				m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0;
				m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0;  // [v229] 停手同步清插值态, 防旧速率继续发射
			}
			m_hasTarget = true;
			return;
		}

		m_hasTarget = false;
		m_lockedTrackId = -1;
		m_lockedClassId = -1;
		m_lockConfidence = 0;
		m_lockFrames = 0;
		m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
		m_pinned = false;  // [v225] 释放锁定解除锁死
		m_smoothInit = false;
		m_smoothW = 0; m_smoothH = 0;
		m_lockedTargetSpeed = 0;
		m_lockedTargetSpeedX = 0; m_lockedTargetSpeedY = 0; m_lastLockedRawX = 0; m_lastLockedRawY = 0;
		m_ffVelX = 0; m_ffVelY = 0; m_sentNowX = 0; m_sentNowY = 0;
		m_sentHistX[0] = 0; m_sentHistX[1] = 0; m_sentHistY[0] = 0; m_sentHistY[1] = 0;  // [v228]
		m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0; m_freshCompute = false;  // [v229] 清插值态
		if (m_lockLogCooldown <= 0) {
			m_lockLogCooldown = 60;
			LOG_INFO(OBS2(u8"目标丢失，释放锁定").c_str());
		}
	}

	if (!hasNewDetections) {
		m_diagInferSkipCount++;
		return;
	}

	if (detections.empty() && m_tracker.getTracks().empty()) {
		m_lostFrames++;
		m_hasTarget = false;
		return;
	}

	m_lostFrames = 0;
	int bestTrackId = selectBestTrack(detections, captureCX, captureCY, captureW, captureH, config);
	if (bestTrackId < 0) {
		m_hasTarget = false;
		return;
	}

	TrackedTarget lockedTrack;
	if (!m_tracker.findTrackDataById(bestTrackId, lockedTrack)) {
		m_hasTarget = false;
		return;
	}

	// 用rawCx(原始检测位置)，不用cx(Kalman平滑位置含被污染的预测成分)
	float useX = lockedTrack.rawCx;
	float useY = lockedTrack.rawCy;

	m_lockedTrackId = bestTrackId;
	m_lockedClassId = lockedTrack.classId;
	m_lockedW = lockedTrack.w;
	m_lockedH = lockedTrack.h;
	m_lockConfidence = lockedTrack.lockConfidence;
	m_lockFrames = 1;
	m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
	m_prevServoDist = 0.0f;  // [v196] 重置拉动判定基准
	m_ffVelX = 0; m_ffVelY = 0; m_sentNowX = 0; m_sentNowY = 0;
	m_sentHistX[0] = 0; m_sentHistX[1] = 0; m_sentHistY[0] = 0; m_sentHistY[1] = 0;  // [v228] 清3帧在途链: 新锁定从头记账
	m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0; m_freshCompute = false;  // [v229] 清插值态
	m_smoothH = lockedTrack.h;
	m_smoothW = lockedTrack.w;
	m_smoothInit = true;
	m_humanizeEnabled = config.move_randomize;  // [v217] 同步拟人层开关
	m_lockedTargetSpeed = lockedTrack.lastSpeed;
	m_lastLockedRawX = lockedTrack.rawCx;
	m_lastLockedRawY = lockedTrack.rawCy;
	if (lockedTrack.kalman.isInitialized()) {
		m_lockedTargetSpeedX = lockedTrack.kalman.getVx();
		m_lockedTargetSpeedY = lockedTrack.kalman.getVy();
	} else {
		m_lockedTargetSpeedX = 0;
		m_lockedTargetSpeedY = 0;
	}

	float targetX = (useX - m_smoothW / 2.0f + m_smoothW * config.aim_position_x) - captureCX;
	float targetY = (useY - m_smoothH / 2.0f + m_smoothH * getAimPosYForClass(lockedTrack.classId, config)) - captureCY;

	// ── [v177] 过冲自抑制(闭环): 纯P按"1计数=1像素"假设发力, 真实换算比>1.11时
	// 90%钳位失效→过冲。此处每帧观测: 误差反号且比上帧更近=我方大步穿过→增益系数×0.70
	// 强压; 误差反号但比上帧更远=目标自己跑过准星(外部)→不罚; 连续扩大→×0.90;
	// 长期收缩<25%(太慢)→×1.10回升但永不超过1.0。范围[0.10,1.0]保证可观测无死锁
	{
		float prevErrDist = std::sqrt(m_lockedX * m_lockedX + m_lockedY * m_lockedY);
		float newDist = std::sqrt(targetX * targetX + targetY * targetY);
		float sentMag = std::sqrt(m_moveX * m_moveX + m_moveY * m_moveY);
		bool crossed = (std::abs(targetX) > 3.0f && std::abs(m_lockedX) > 3.0f && targetX * m_lockedX < 0.0f) ||
			(std::abs(targetY) > 3.0f && std::abs(m_lockedY) > 3.0f && targetY * m_lockedY < 0.0f);
		// [v185] 评估门槛 8→15px: 贴附期(误差<15px)检测噪声即可让误差偶发反号→
		// crossed误判→gainScale×0.70强压→回升×1.10→再压, 力道大幅周期摆动=锁上后冲冲拉拉。
		// 贴附期os退场(力道冻结), 远场防穿越功能照常。
		if (sentMag > 0.5f && prevErrDist > 15.0f) {
			float shrink = (prevErrDist - newDist) / prevErrDist;
			m_shrinkEma = (m_shrinkEma < -0.5f) ? shrink : (m_shrinkEma * 0.7f + shrink * 0.3f);
			bool external = std::abs(newDist - prevErrDist) > sentMag * 3.0f;
			if (crossed && newDist < prevErrDist && !external) {
				m_gainScale *= config.os_damp;
				m_shrinkEma = -1.0f;
			} else if (!external && m_shrinkEma < -0.05f) {
				m_gainScale *= 0.90f;
			} else if (!external && m_shrinkEma > 0.0f && m_shrinkEma < 0.25f) {
				m_gainScale *= 1.10f;
			}
			m_gainScale = std::clamp(m_gainScale, 0.10f, 1.0f);
		}
	}

	if (m_lockLogCooldown > 0) m_lockLogCooldown--;
	if (m_lockLogCooldown <= 0) {
		m_lockLogCooldown = 60;
		LOG_INFO(OBS2(u8"锁定新目标 ID:%d 类:%d 尺寸:%.0fx%.0f 置信度:%.2f").c_str(),
			bestTrackId, lockedTrack.classId,
			lockedTrack.w, lockedTrack.h, lockedTrack.confidence);
	}

	m_hasTarget = true;
	m_lockedX = targetX;
	m_lockedY = targetY;
	m_prevErrorX = targetX;
	m_prevErrorY = targetY;
	if (aimActive) {
		computeMove(m_lockedX, m_lockedY, dt, config, recoilVelX, recoilVelY);
	} else {
		m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
	}
}

void AimAssist::computeMove(float errorX, float errorY, float dt, const D5RenderConfig& config, float recoilVelX, float recoilVelY) {
	// ============================================================
	// [v226] 权威2DOF控制: 速度前馈 + 指数收敛反馈 + 在途移动记账
	// ============================================================

	(void)recoilVelX;
	(void)recoilVelY;
	(void)dt;
	// ── [v230] CA+IMM动态调度(aim_sim仿真10.7分最优): 创新量/加速度软门限→dyn_state,
	// 尖峰瞬时开Q+0.25s缓关(消bootstrap死锁); IMM副模型lam机动段加快翻转 ──
	float dyn = 0.0f;
	TrackedTarget dynTrack;
	if (m_tracker.findTrackDataById(m_lockedTrackId, dynTrack) && dynTrack.kalman.isInitialized()) {
		float innov = dynTrack.kalman.getInnovMag();
		float amag = dynTrack.kalman.getAMag();
		float inst = std::clamp(std::max((innov - config.dyn_innov_px) / config.dyn_innov_div,
			(amag - config.dyn_acc_px) / config.dyn_amag_div), 0.0f, 1.0f);
		float dtsDyn = m_lastDetectDt;
		if (dtsDyn < 0.008f) dtsDyn = 0.008f;
		if (dtsDyn > 0.05f) dtsDyn = 0.05f;
		m_dynState *= std::exp(-dtsDyn / config.dyn_hold_s);
		if (inst > m_dynState) m_dynState = inst;
		dyn = m_dynState;
		float qDyn = config.kalman_q_min + (config.kalman_q_dyn - config.kalman_q_min) * dyn;
		float lamDyn = std::min(0.9f, config.imm_lam + config.imm_lam_dyn * dyn);
		m_tracker.setTrackDynamic(m_lockedTrackId, qDyn, config.imm_cv_q, lamDyn);
		m_dynAccX = dynTrack.kalman.getAX();
		m_dynAccY = dynTrack.kalman.getAY();
	} else {
		m_dynAccX = 0;
		m_dynAccY = 0;
	}

	float dist = std::sqrt(errorX * errorX + errorY * errorY);
	float deadzone = config.pid_deadzone;
	if (deadzone < 2.0f) deadzone = 2.0f;

	// [v225] 滞回锁死(硬磁吸核心): 慢速目标(<120px/s)贴附后进入锁死态,
	// 维持区1.8×死区内绝对零输出——检测噪声±5px全部吞掉, 准星纹丝不动;
	// 手动拉鼠标/目标急移超出维持区→解锁, 反馈全速回弹(体感=拉不动)。
	// 移动目标不进锁死(避免周期滞后弹回), 由前馈+指数反馈自然咬合跟枪。
	bool slowTarget = m_lockedTargetSpeed < 120.0f;
	if (m_pinned && slowTarget) {
		if (dist < deadzone * 1.8f) {
			m_moveX = 0;
			m_moveY = 0;
			m_currentGain = 0;
			m_prevMoveX = 0;
			m_prevMoveY = 0;
			m_prevErrorX = errorX;
			m_prevErrorY = errorY;
			m_prevServoDist = dist;
			m_integX = 0;
			m_integY = 0;
			// [v228] 在途链轮转不清零: 死区前发出的移动仍在画面延迟中, 清零会漏扣→出死区过冲
			m_sentHistX[1] = m_sentHistX[0]; m_sentHistX[0] = m_sentNowX; m_sentNowX = 0;
			m_sentHistY[1] = m_sentHistY[0]; m_sentHistY[0] = m_sentNowY; m_sentNowY = 0;
			m_freshCompute = true;  // [v229] 零步登记: getMoveDelta从零整步算出速率0/预算0, 本周期发0
			return;
		}
		m_pinned = false;
	}
	if (dist < deadzone) {
		m_pinned = slowTarget;
		m_moveX = 0;
		m_moveY = 0;
		m_currentGain = 0;
		m_prevMoveX = 0;
		m_prevMoveY = 0;
		m_prevErrorX = errorX;
		m_prevErrorY = errorY;
		m_prevServoDist = dist;  // [v196] 死区内也跟踪距离基准, 防恢复帧误判"用户拉动"
		// [v228] 在途链轮转不清零(同上), 出死区时窗口内发送量照常扣除
		m_sentHistX[1] = m_sentHistX[0]; m_sentHistX[0] = m_sentNowX; m_sentNowX = 0;
		m_sentHistY[1] = m_sentHistY[0]; m_sentHistY[0] = m_sentNowY; m_sentNowY = 0;
		m_freshCompute = true;  // [v229] 零步登记(同上)
		return;
	}

	// ── 1) 在途移动记账: 扣除发出后画面尚未反映的全部真实移动量 ──
	// [v228] 核心修复(移动过冲终极根因): 画面延迟40~70ms > 检测间隔20~33ms,
	// 同一笔移动停留在2~3帧误差里, 旧版只扣m_sentNow这1帧→每个移动被修1.5~2次
	// →过冲与移速成正比(速度越快扣得越少过冲越大)。这是控制论经典纯滞后(dead-time)
	// 问题, 权威解法=Smith预估器: 扣除整个延迟窗口(3帧覆盖60~100ms)内的全部发送量。
	// 多扣=欠修正(慢5%无感知), 少扣=过冲(反复回修), 宁欠勿过
	float inflightX = m_sentNowX + m_sentHistX[0] + m_sentHistX[1];
	float inflightY = m_sentNowY + m_sentHistY[0] + m_sentHistY[1];
	float effErrX = errorX - inflightX;
	float effErrY = errorY - inflightY;
	m_sentHistX[1] = m_sentHistX[0]; m_sentHistX[0] = m_sentNowX; m_sentNowX = 0;
	m_sentHistY[1] = m_sentHistY[0]; m_sentHistY[0] = m_sentNowY; m_sentNowY = 0;
	// ── 2) 速度前馈(EMA平滑Kalman融合速度, 防估计噪声直通输出) ──
	// [v230] ema_k动态(仿真最优): 基准0.70, 机动段dyn升向1.0消前馈滞后;
	// 误差单拍发散>6px(折返/急转)应急+0.15; 开枪保留v227降噪0.05
	float ffAlpha;
	if (m_shooting) {
		ffAlpha = 0.05f;
	} else {
		ffAlpha = config.ff_ema_k;
		ffAlpha += (1.0f - ffAlpha) * dyn;
		if (m_prevMeasSet && (std::abs(errorX) - std::abs(m_prevMeasErrX) > 6.0f || std::abs(errorY) - std::abs(m_prevMeasErrY) > 6.0f)) {
			ffAlpha = std::min(1.0f, ffAlpha + 0.15f);
		}
	}
	m_ffVelX = m_ffVelX * (1.0f - ffAlpha) + m_lockedTargetSpeedX * ffAlpha;
	m_ffVelY = m_ffVelY * (1.0f - ffAlpha) + m_lockedTargetSpeedY * ffAlpha;
	// 画面年龄补偿: 检测看到的是~30ms前的目标, 它已移动vel×age(Smith预估)
	float predErrX = effErrX + m_ffVelX * 0.030f;
	float predErrY = effErrY + m_ffVelY * 0.030f;
	// ── 3) 指数收敛律: alpha=1-exp(-dt/τ), 分段τ: 近区快回咬, 远区防过冲 ──
	// [v227] τ=100ms对后座连续冲击恢复太慢(30fps每帧28%, 200ms才修60%误差),
	// 后座每100ms踢一次准星→误差越修越大→脱落。近区(<50px)τ=50ms每帧修~48%,
	// 在途记账兜底不会叠加; 远区τ=100ms保持平滑接近无过冲
	float errMag0 = std::sqrt(errorX * errorX + errorY * errorY);
	float tau = (errMag0 < 50.0f) ? 0.050f : 0.100f;
	float dts = m_lastDetectDt;
	if (dts < 0.008f) dts = 0.008f;
	if (dts > 0.05f) dts = 0.05f;
	float alpha = 1.0f - std::exp(-dts / tau);
	alpha *= config.pid_kp * 1.5f * config.speed_multiplier;
	if (alpha > 0.90f) alpha = 0.90f;
	if (alpha < 0.02f) alpha = 0.02f;
	alpha += config.fb_boost * dyn;  // [v230] 反馈增益动态加成(机动段加力, 仿真fb_boost=0.06)
	if (alpha > 0.90f) alpha = 0.90f;
	// 反馈修预测误差 + 前馈补一帧目标位移(跟枪主导项, 移动目标零滞后死咬)
	float ffX = m_ffVelX * dts;
	float ffY = m_ffVelY * dts;
	// [v230] 强减速刹ff: CA加速度与目标速度反号且|ax|大(急减速/折返), 滤波滞后+画面延迟
	// 使ff按旧速度继续推→过冲; 按比例收缩ff, 交由反馈(经在途记账修正)纠偏
	if (m_lockedTargetSpeedX * m_dynAccX < 0.0f && std::abs(m_dynAccX) > config.decel_ax_px) ffX *= config.ff_decel_ratio;
	if (m_lockedTargetSpeedY * m_dynAccY < 0.0f && std::abs(m_dynAccY) > config.decel_ax_px) ffY *= config.ff_decel_ratio;
	// [v228] 前馈安全带: 前馈最多为有效误差的60%(同向才钳)。正常追踪时ff/effErr
	// ≈0.4不会触发; 只有Kalman速度虚高(噪声/急停/反向上仰)时把它拽回安全范围,
	// 防前馈把准星拉过头(前馈不经过90%硬钳位, 是最后没看住的输出通道)
	float capX = std::abs(effErrX) * 0.60f;
	float capY = std::abs(effErrY) * 0.60f;
	if (std::abs(ffX) > capX && ffX * effErrX > 0) ffX = (ffX > 0 ? capX : -capX);
	if (std::abs(ffY) > capY && ffY * effErrY > 0) ffY = (ffY > 0 ? capY : -capY);
	float moveX = predErrX * alpha + ffX;
	float moveY = predErrY * alpha + ffY;
	m_currentGain = alpha;
	// ── 反馈项分段限幅(前馈不受限: 钳前馈=人为制造滞后咬不住) ──
	float fbX = moveX - ffX;
	float fbY = moveY - ffY;
	float maxMove = config.move_max_step;
	if (maxMove < 1.0f) maxMove = 1.0f;
	float fbMag = std::sqrt(fbX * fbX + fbY * fbY);
	if (fbMag > maxMove) {
		float s = maxMove / fbMag;
		fbX *= s;
		fbY *= s;
	}
	moveX = fbX + ffX;
	moveY = fbY + ffY;

	// [v226] 旧Ki/D/惯性/最小速度/接近限速已随结构改造删除:
	// - Ki在噪声下累积随机游走(晃动源), 指数律对静止目标误差自然归零
	// - 惯性是延迟拖尾(贴附区0.15地板强制引入15%上帧拖尾=过冲帧反推)
	// - 最小速度2px在误差2.3px时强拉2px跨过目标→反向2px→贴附打摆
	// - dtNorm在30fps时×1.5把kp0.85放大到1.28/帧=叠加过冲直接元凶
	// 新结构防过冲三重保证: 在途记账(每误差只修一次)+指数律(一阶无过冲)+硬钳位

	// --- 硬钳位：移动量绝不超过预测误差的90% ---
	// 零过冲的数学保证：move < error → 不可能越过目标
	float maxAllowedX = std::abs(predErrX) * 0.90f;
	float maxAllowedY = std::abs(predErrY) * 0.90f;
	if (maxAllowedX < 0.5f) maxAllowedX = 0.5f;
	if (maxAllowedY < 0.5f) maxAllowedY = 0.5f;
	if (std::abs(moveX) > maxAllowedX) moveX = (moveX > 0 ? 1.0f : -1.0f) * maxAllowedX;
	if (std::abs(moveY) > maxAllowedY) moveY = (moveY > 0 ? 1.0f : -1.0f) * maxAllowedY;

	m_prevMoveX = moveX;
	m_prevMoveY = moveY;
	m_moveX = moveX;
	m_moveY = moveY;
	m_freshCompute = true;  // [v229] 新整步已产出: getMoveDelta把它折算为速率+预算, 帧间匀速摊薄
	m_prevMeasErrX = errorX;
	m_prevMeasErrY = errorY;
	m_prevMeasSet = true;  // [v230] 误差发散应急EMA的测量基准
}

AimTarget AimAssist::getMoveDelta(float dt) {
	AimTarget delta = { 0, 0, 0, 0 };
	// [v229] 连续输出: 主循环每帧调用, outDt=距上次输出的真实间隔(时钟实测, 与检测节拍解耦)
	auto nowOut = std::chrono::steady_clock::now();
	float outDt = 0.004f;
	if (m_lastOutputTime.time_since_epoch().count() > 0) {
		outDt = (float)std::chrono::duration<double>(nowOut - m_lastOutputTime).count();
		if (outDt < 0.0005f) outDt = 0.0005f;
		if (outDt > 0.100f) outDt = 0.100f;
	}
	m_lastOutputTime = nowOut;
	if (!m_hasTarget) {
		m_subPixelX = 0;
		m_subPixelY = 0;
		m_diagPrevMoveX = 0;
		m_diagPrevMoveY = 0;
		m_diagPrevDt = dt;
		m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0; m_freshCompute = false;  // [v229] 清插值态
		return delta;
	}
	// [v229] fresh=本检测帧computeMove刚产出新整步(或死区零步); 非fresh=检测帧间按速率插值
	bool freshStep = m_freshCompute;
	m_freshCompute = false;
	float rawMoveX = 0, rawMoveY = 0;
	if (freshStep) {
		// 检测帧: 整步经安全网+拟人+防穿越后折算为速率, 登记本周期预算(总量=整步, 与v228 1:1)
		float moveX = m_moveX;
		float moveY = m_moveY;
		// 安全网：移动量不超过误差的90%
		if (m_lockedX > 0.0f && moveX > m_lockedX * 0.90f) moveX = m_lockedX * 0.90f;
		else if (m_lockedX < 0.0f && moveX < m_lockedX * 0.90f) moveX = m_lockedX * 0.90f;
		if (m_lockedY > 0.0f && moveY > m_lockedY * 0.90f) moveY = m_lockedY * 0.90f;
		else if (m_lockedY < 0.0f && moveY < m_lockedY * 0.90f) moveY = m_lockedY * 0.90f;
		rawMoveX = moveX;
		rawMoveY = moveY;

	// ── P0 拟人化输出层（对抗行为统计检测）──
	// 位置：安全钳位之后、折算速率之前。只污染输出序列，不进入控制环。
	// 人手特征：幅度抖动+弧线轨迹+连续微扰+传感器噪声。
	// 机器特征：固定衰减比的完美指数曲线+笔直直线。
	// 全部扰动零均值或受限漂移，检测帧重算误差的闭环自动吸收残余偏差。
	// [v229] 按整步设计(扰动脉宽=检测周期), 只在fresh帧施加一次, 结果折算进速率摊薄发射
	if (m_aimActive && m_humanizeEnabled) {  // [v217] 接上move_randomize开关(之前是摆设, 拟人层无条件运行)
		float md = std::sqrt(moveX * moveX + moveY * moveY);
		// [v225] 贴附区(误差<20px)关闭拟人扰动: ±25%幅度抖动+σ0.5噪声在贴附边缘
		// =随机游走("松/晃"帮凶), 硬磁吸要求修正纯净。追击段(≥20px)拟人全开保反检测
		float errMag = std::sqrt(m_lockedX * m_lockedX + m_lockedY * m_lockedY);
		if (md > 0.3f && errMag >= 20.0f) {
			// 新锁定判定：lockFrames 回落说明换了目标/重锁 -> 重掷弧线参数
			if (m_lockFrames < m_prevHumanLockFrames) {
				std::uniform_real_distribution<float> curv(0.02f, 0.08f);
				m_arcCurv = curv(m_rng) * ((m_rng() & 1) ? 1.0f : -1.0f);
				m_arcLateral = 0;
				m_wobblePhase = (float)(m_rng() % 628) * 0.01f;
			}
			m_prevHumanLockFrames = m_lockFrames;

			// 1) 幅度乘性抖动：±25%，打破固定衰减比（对数正态手感）
			std::normal_distribution<float> magJ(0.0f, 0.12f);
			float magScale = 1.0f + magJ(m_rng);
			if (magScale > 1.30f) magScale = 1.30f;
			if (magScale < 0.75f) magScale = 0.75f;
			moveX *= magScale;
			moveY *= magScale;

			// 侧偏/微扰沿移动方向的法线（弧线由侧偏累积形成）
			float invMd = 1.0f / md;
			float perpX = -moveY * invMd;
			float perpY = moveX * invMd;

			// 2) 弧线侧偏：曲率×步长，单帧≤2px、总漂移±5px封顶，接近目标(20px内)渐隐
			float arcFade = (md < 20.0f) ? (md / 20.0f) : 1.0f;
			float latStep = m_arcCurv * md * arcFade;
			if (latStep > 2.0f) latStep = 2.0f;
			if (latStep < -2.0f) latStep = -2.0f;
			m_arcLateral += latStep;
			if (m_arcLateral > 5.0f) { latStep -= (m_arcLateral - 5.0f); m_arcLateral = 5.0f; }
			if (m_arcLateral < -5.0f) { latStep -= (m_arcLateral + 5.0f); m_arcLateral = -5.0f; }
			moveX += perpX * latStep;
			moveY += perpY * latStep;

			// 3) 正弦微扰：±0.3px，相位随机推进（打破固定帧对齐）
			m_wobblePhase += 0.7f + (float)(m_rng() % 100) * 0.006f;
			float wob = std::sin(m_wobblePhase) * 0.3f;
			moveX += perpX * wob;
			moveY += perpY * wob;

			// 4) 高斯微噪声：σ=0.5px（模拟手部生理抖动+传感器噪声），
			//    小步长时按比例缩放，静止时无噪声（避免死区内空转）
			float noiseScale = (md < 3.0f) ? (md / 3.0f) : 1.0f;
			std::normal_distribution<float> gn(0.0f, 0.5f * noiseScale);
			float nx = gn(m_rng), ny = gn(m_rng);
			if (nx > 0.9f) nx = 0.9f; else if (nx < -0.9f) nx = -0.9f;
			if (ny > 0.9f) ny = 0.9f; else if (ny < -0.9f) ny = -0.9f;
			moveX += nx;
			moveY += ny;
		}
	}

		// ── 终极防穿越 ──
		// 拟人化扰动(±25%幅度+弧线+噪声)发生在90%钳位之后，最大可放大到
		// 误差的1.3倍 → 近距离单帧越过目标。输出前按当前误差再钳一次(98%)：
		// 每帧移动严格小于到目标的剩余误差 → 物理上不可能跨过要去的目标。
		{
			float limX = std::abs(m_lockedX) * 0.98f;
			float limY = std::abs(m_lockedY) * 0.98f;
			if (std::abs(moveX) > limX) moveX = (moveX > 0.0f ? limX : -limX);
			if (std::abs(moveY) > limY) moveY = (moveY > 0.0f ? limY : -limY);
		}
		// [v229] 折算速率+登记预算: 本周期匀速摊薄发射, 累计≤整步(预算守恒→宁欠勿过)
		float dtsI = m_lastDetectDt;
		if (dtsI < 0.001f) dtsI = 0.001f;
		m_ctrlRateX = moveX / dtsI;
		m_ctrlRateY = moveY / dtsI;
		m_interpBudgetX = std::abs(moveX);
		m_interpBudgetY = std::abs(moveY);
	}
	// [v229] 所有帧统一发射: 速率×实际时间片, 预算钳制防超发(检测变慢时停手等下帧)
	float emitX = m_ctrlRateX * outDt;
	float emitY = m_ctrlRateY * outDt;
	if (std::abs(emitX) > m_interpBudgetX) emitX = (emitX > 0.0f ? m_interpBudgetX : -m_interpBudgetX);
	if (std::abs(emitY) > m_interpBudgetY) emitY = (emitY > 0.0f ? m_interpBudgetY : -m_interpBudgetY);
	m_interpBudgetX -= std::abs(emitX);
	m_interpBudgetY -= std::abs(emitY);
	if (m_interpBudgetX < 0.0f) m_interpBudgetX = 0.0f;
	if (m_interpBudgetY < 0.0f) m_interpBudgetY = 0.0f;
	if (emitX != 0.0f || emitY != 0.0f) m_outEmitCount++;  // [v229] 非零发射计数(输出率诊断)
	// 累积亚像素
	m_subPixelX += emitX;
	m_subPixelY += emitY;
	delta.screenX = (int)m_subPixelX;
	delta.screenY = (int)m_subPixelY;
	m_subPixelX -= delta.screenX;
	m_subPixelY -= delta.screenY;

	// [v229] 卡顿诊断只在检测帧评估(插值帧整步=0, 与上帧整步比较无意义)
	if (freshStep) {
		if (m_diagStutterLogCooldown > 0) m_diagStutterLogCooldown--;
		bool stutterDetected = false;
		if (dt > 0.040f) stutterDetected = true;
		float prevMag = std::sqrt(m_diagPrevMoveX * m_diagPrevMoveX + m_diagPrevMoveY * m_diagPrevMoveY);
		float curMag = std::sqrt(rawMoveX * rawMoveX + rawMoveY * rawMoveY);
		float magRatio = (prevMag > 0.01f) ? (curMag / prevMag) : 1.0f;
		if (prevMag > 3.0f && curMag > 3.0f && std::abs(curMag - prevMag) > 5.0f && (magRatio > 5.0f || magRatio < 0.2f)) stutterDetected = true;
		if (m_diagInferSkipCount > 5) stutterDetected = true;
		if (stutterDetected && m_diagStutterLogCooldown <= 0) {
			m_diagStutterLogCooldown = 3600;
			static std::string causeUnknown = OBS1(u8"未知");
			static std::string causeStutter = OBS2(u8"主循环卡顿");
			static std::string causeInfer = OBS3(u8"推理延迟");
			static std::string causeSpike = OBS4(u8"移动量突变");
			const char* cause = causeUnknown.c_str();
			if (dt > 0.040f) cause = causeStutter.c_str();
			else if (m_diagInferSkipCount > 5) cause = causeInfer.c_str();
			else if (curMag > prevMag * 3.0f || curMag < prevMag * 0.33f) cause = causeSpike.c_str();
			LOG_WARN(OBS1(u8"[诊断] 原因:%s 帧间隔:%.1fms 跳帧:%d 移动:(%.1f,%.1f)->(%.1f,%.1f)").c_str(),
				cause, dt * 1000.0f, m_diagInferSkipCount,
				m_diagPrevMoveX, m_diagPrevMoveY, rawMoveX, rawMoveY);
		}
		m_diagPrevMoveX = rawMoveX;
		m_diagPrevMoveY = rawMoveY;
	}
	m_diagPrevDt = dt;
	// [v195] 标定上下文清理: 学习值m_sensRatioX/Y跨重置保留(已收敛不必重学)
	m_calibPrevTrackId = -1;
	// [v226] 删除旧自标定累加: 与noteSent重复记账(m_sentNow=2×发送量→反向过修=震荡),
	// 现在m_sentNow只由noteSent累加实际发送计数, 由computeMove消费后自清
	delta.confidence = m_lockConfidence;
	delta.distance = std::sqrt(m_lockedX * m_lockedX + m_lockedY * m_lockedY);
	return delta;
}

void AimAssist::reset() {
	m_hasTarget = false;
	m_aimActive = false;
	m_lockedTrackId = -1;
	m_pinned = false;  // [v225]
	m_lockedClassId = -1;
	m_lockedX = 0; m_lockedY = 0;
	m_lostFrames = 0;
	m_lockConfidence = 0;
	m_lockFrames = 0;
	m_lockedW = 0; m_lockedH = 0;
	m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
	m_smoothH = 0; m_smoothW = 0;
	m_ffVelX = 0; m_ffVelY = 0; m_sentNowX = 0; m_sentNowY = 0;
	m_sentHistX[0] = 0; m_sentHistX[1] = 0; m_sentHistY[0] = 0; m_sentHistY[1] = 0;  // [v228]
	m_ctrlRateX = 0; m_ctrlRateY = 0; m_interpBudgetX = 0; m_interpBudgetY = 0; m_freshCompute = false;  // [v229] 清插值态
	m_dynState = 0; m_dynAccX = 0; m_dynAccY = 0; m_prevMeasErrX = 0; m_prevMeasErrY = 0; m_prevMeasSet = false;  // [v230]
	m_smoothInit = false;
	m_currentGain = 0.20f;
	m_aimFov = 0.5f;
	m_subPixelX = 0; m_subPixelY = 0;
	m_lockedTargetSpeed = 0;
	m_lockedTargetSpeedX = 0; m_lockedTargetSpeedY = 0; m_lastLockedRawX = 0; m_lastLockedRawY = 0;
	m_ffBrakeActive = false;
	m_userSuppress = 0;
	m_arcCurv = 0.04f;
	m_arcLateral = 0;
	m_wobblePhase = 0;
	m_prevHumanLockFrames = 0;
	m_gainScale = 1.0f; m_shrinkEma = -1.0f;
	m_prevServoDist = 0.0f;  // [v196]
	m_integX = 0.0f; m_integY = 0.0f;  // [v197]
	m_switchCooldown = 0;
	m_lastDetectTime = {};
	m_shooting = false;
	m_diagInferSkipCount = 0;
	m_diagStutterLogCooldown = 0;
	m_lockLogCooldown = 0;
	m_prevAvgSpeed = 0;
	m_tracker.reset();
	m_sceneAnalyzer.reset();
	m_lastUpdateTime = std::chrono::steady_clock::now();
}

bool AimAssist::checkQuickscopeTrigger(bool triggerKeyDown) {
	bool triggered = (triggerKeyDown && !m_quickscopeLastKey);
	m_quickscopeLastKey = triggerKeyDown;
	return triggered;
}

void AimAssist::startQuickscopeFireDelay(int delayFrames) {
	m_qsFireCountdown = delayFrames;
}

void AimAssist::tickQuickscopeDelay() {
	if (m_qsFireCountdown > 0) m_qsFireCountdown--;
}

void AimAssist::resetAfterQuickscope() {
	m_lockedX = 0; m_lockedY = 0;
	m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0; m_prevErrorX = 0; m_prevErrorY = 0;
}

void AimAssist::resetAfterQuickscopeSnap(float snapX, float snapY) {
	m_lockedX -= snapX;
	m_lockedY -= snapY;
	m_moveX = 0; m_moveY = 0; m_prevMoveX = 0; m_prevMoveY = 0;
	m_prevErrorX = m_lockedX; m_prevErrorY = m_lockedY;
}

void AimAssist::getHeadSnapPos(float& outX, float& outY, float aimPositionY) const {
	const float headRatio = 0.1f;
	const float snapScale = 0.8f;
	outX = m_lockedX * snapScale;
	outY = (m_lockedY - m_smoothH * (aimPositionY - headRatio)) * snapScale;
}
