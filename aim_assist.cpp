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
	float actualAimY = track.cy - track.h / 2.0f + track.h * getAimPosYForClass(track.classId, config);
	float actualAimX = track.cx - track.w / 2.0f + track.w * config.aim_position_x;
	float dx = actualAimX - captureCX;
	float dy = actualAimY - captureCY;
	float dist = std::sqrt(dx * dx + dy * dy);
	float fovRadius = captureW * m_aimFov / 2.0f;
	bool isLockedTarget = (track.trackId == m_lockedTrackId && m_lockedTrackId >= 0);
	if (dist > fovRadius && !isLockedTarget) return 1e9f;
	float area = track.w * track.h;
	float confPenalty = (1.0f - track.confidence) * 30.0f;
	float maxFovDist = fovRadius > 0 ? fovRadius : 1.0f;
	float normDist = dist / maxFovDist;
	float maxArea = (float)(captureW * captureH);
	float normArea = (area > 0 && maxArea > 0) ? area / maxArea : 0.0f;
	float moveDirPenalty = 0.0f;
	if (dist > 1.0f && track.lastSpeed > 5.0f) {
		float normDx = dx / dist;
		float normDy = dy / dist;
		float moveDirX = (track.lastSpeed > 0.1f && track.kalman.isInitialized()) ? track.kalman.getVx() / track.lastSpeed : 0;
		float moveDirY = (track.lastSpeed > 0.1f && track.kalman.isInitialized()) ? track.kalman.getVy() / track.lastSpeed : 0;
		float dotProduct = normDx * moveDirX + normDy * moveDirY;
		if (dotProduct > 0) moveDirPenalty = dotProduct * 0.3f;
	}
	float score;
	if (config.target_strategy == TargetStrategy::Distance) {
		score = normDist * 0.85f + (1.0f - normArea) * 0.15f + confPenalty * 0.01f + moveDirPenalty;
	} else {
		score = (1.0f - normArea) * 0.85f + normDist * 0.15f + confPenalty * 0.01f + moveDirPenalty;
	}
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
		m_lockedClassId = -1;
		m_lockConfidence = 0;
		m_lockFrames = 0;
		m_hasTarget = false;
		m_moveX = 0; m_moveY = 0;
		m_smoothInit = false;
		m_smoothW = 0; m_smoothH = 0;
		m_prevMoveX = 0; m_prevMoveY = 0;
		m_prevErrorX = 0; m_prevErrorY = 0;
		m_integralX = 0; m_integralY = 0;
		m_accelFactor = 1.0f;
		m_subPixelX = 0; m_subPixelY = 0;
		m_shootFrames = 0; m_stopFireFrames = 0;
		m_filteredErrorX = 0; m_filteredErrorY = 0;
		m_lockedTargetSpeed = 0;
		m_lockedTargetSpeedX = 0; m_lockedTargetSpeedY = 0;
		m_prevErrorDist = 0;
		m_ffBrakeActive = false;
		m_userSuppress = 0;
		m_switchCooldown = 0;
	}

	if (hasNewDetections) {
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
			m_tracker.setKalmanParams(config.kalman_process_noise, config.kalman_measurement_noise, dynamicAhead);
		}
		m_tracker.update(detections, dt, captureW, captureH);
	}

	if (m_lockedTrackId >= 0) {
		TrackedTarget locked;
		bool found = m_tracker.findTrackDataById(m_lockedTrackId, locked);

		if (!found && hasNewDetections) {
			float predCenterX = captureCX + m_lockedX;
			float predCenterY = captureCY + m_lockedY;
			const auto& tracks = m_tracker.getTracks();
			float bestDist = 1e9f;
			int bestId = -1;
			for (const auto& t : tracks) {
				if (t.lostFrames > 0) continue;
				float dx = t.cx - predCenterX;
				float dy = t.cy - predCenterY;
				float d = dx * dx + dy * dy;
				if (d < bestDist) { bestDist = d; bestId = t.trackId; }
			}
			float posMatchRadius = m_smoothW * 0.5f + 30.0f;
			if (m_lockedTargetSpeed > 200.0f) {
				posMatchRadius += std::min((m_lockedTargetSpeed - 200.0f) * 0.1f, 120.0f);
			}
			if (bestId >= 0 && bestDist < posMatchRadius * posMatchRadius) {
				TrackedTarget bestTrack;
				m_tracker.findTrackDataById(bestId, bestTrack);
				if (bestTrack.classId == m_lockedClassId) {
					m_tracker.findTrackDataById(bestId, locked);
					found = true;
					m_lockedTrackId = bestId;
				}
			}
		}

		const int aimLockLostTimeout = 15;
		if (found && locked.lostFrames <= aimLockLostTimeout) {
			m_lostFrames = 0;
			m_lockFrames++;
			if (m_lockLogCooldown > 0) m_lockLogCooldown--;
			m_lockedClassId = locked.classId;
			m_lockedW = locked.w;
			m_lockedH = locked.h;
			m_lockConfidence = locked.lockConfidence;
			m_lockedTargetSpeed = locked.lastSpeed;
			if (locked.kalman.isInitialized()) {
				m_lockedTargetSpeedX = locked.kalman.getVx();
				m_lockedTargetSpeedY = locked.kalman.getVy();
			} else {
				m_lockedTargetSpeedX = 0;
				m_lockedTargetSpeedY = 0;
			}

			bool inProtectionPeriod = (m_lockFrames <= 10);

			if (m_switchCooldown <= 0 && locked.lostFrames > 0 && hasNewDetections && !inProtectionPeriod) {
				m_selectBestFrame++;
				bool doSelect = (m_selectBestFrame >= 3);
				if (doSelect) m_selectBestFrame = 0;
				if (doSelect) {
					int bestTrackId = selectBestTrack(detections, captureCX, captureCY, captureW, captureH, config);
					if (bestTrackId >= 0 && bestTrackId != m_lockedTrackId) {
						TrackedTarget newTarget;
						if (m_tracker.findTrackDataById(bestTrackId, newTarget)) {
							float curDist = std::sqrt(m_lockedX * m_lockedX + m_lockedY * m_lockedY);
							float newDx = (newTarget.cx - captureCX);
							float newDy = (newTarget.cy - captureCY);
							float newDist = std::sqrt(newDx * newDx + newDy * newDy);
							float switchThreshold = (newTarget.classId == m_lockedClassId) ? 0.7f : 0.5f;
							if (newDist < curDist * switchThreshold) {
								m_lockedTrackId = bestTrackId;
								m_lockedClassId = newTarget.classId;
								m_lockedW = newTarget.w;
								m_lockedH = newTarget.h;
								m_lockConfidence = newTarget.lockConfidence;
								m_lockFrames = 1;
								m_lockedTargetSpeed = newTarget.lastSpeed;
								if (newTarget.kalman.isInitialized()) {
									m_lockedTargetSpeedX = newTarget.kalman.getVx();
									m_lockedTargetSpeedY = newTarget.kalman.getVy();
								} else {
									m_lockedTargetSpeedX = 0;
									m_lockedTargetSpeedY = 0;
								}
								m_prevMoveX = 0; m_prevMoveY = 0;
								m_prevErrorX = 0; m_prevErrorY = 0;
								m_integralX = 0; m_integralY = 0;
								m_shootFrames = 0; m_stopFireFrames = 0;
								m_filteredErrorX = 0; m_filteredErrorY = 0;
								m_smoothH = newTarget.h;
								m_smoothW = newTarget.w;
								m_smoothInit = true;
								locked = newTarget;
								m_switchCooldown = 30;
								if (m_lockLogCooldown <= 0) {
									m_lockLogCooldown = 30;
									LOG_INFO(OBS1(u8"切换目标 ID:%d 类:%d").c_str(), bestTrackId, newTarget.classId);
								}
							}
						}
					}
				}
			}

			if (locked.lostFrames > 0) {
				m_hasTarget = true;
				if (locked.lostFrames <= 8 && config.kalman_enabled && locked.kalman.isInitialized()) {
					float predX = locked.kalman.getFastPredictedX();
					float predY = locked.kalman.getFastPredictedY();
					float decayFactor = std::max(0.0f, 1.0f - locked.lostFrames * 0.2f);
					float targetX = (predX - m_smoothW / 2.0f + m_smoothW * config.aim_position_x) - captureCX;
					float targetY = (predY - m_smoothH / 2.0f + m_smoothH * getAimPosYForClass(m_lockedClassId, config)) - captureCY;
					m_lockedX = m_lockedX * 0.4f + targetX * 0.6f * decayFactor;
					m_lockedY = m_lockedY * 0.4f + targetY * 0.6f * decayFactor;
				} else if (hasNewDetections) {
					float decayFactor = std::max(0.0f, 1.0f - locked.lostFrames * 0.2f);
					float predX = ((locked.rawCx - m_smoothW / 2.0f + m_smoothW * config.aim_position_x) - captureCX) * decayFactor;
					float predY = ((locked.rawCy - m_smoothH / 2.0f + m_smoothH * getAimPosYForClass(m_lockedClassId, config)) - captureCY) * decayFactor;
					m_lockedX = predX;
					m_lockedY = predY;
				} else {
					m_lockedX *= 0.75f;
					m_lockedY *= 0.75f;
				}
				if (aimActive) {
					computeMove(m_lockedX, m_lockedY, dt, config, recoilVelX, recoilVelY);
				} else {
					m_moveX = 0; m_moveY = 0;
				}
				return;
			}

			if (hasNewDetections) {
				float rawX = locked.rawCx;
				float rawY = locked.rawCy;
				if (!m_smoothInit) {
					m_smoothH = locked.h;
					m_smoothW = locked.w;
					m_smoothInit = true;
				} else {
					m_smoothH = m_smoothH * 0.7f + locked.h * 0.3f;
					m_smoothW = m_smoothW * 0.7f + locked.w * 0.3f;
				}
				float targetX = (rawX - m_smoothW / 2.0f + m_smoothW * config.aim_position_x) - captureCX;
				float targetY = (rawY - m_smoothH / 2.0f + m_smoothH * getAimPosYForClass(m_lockedClassId, config)) - captureCY;
				m_lockedX = targetX;
				m_lockedY = targetY;
			} else {
				// 推理延迟补偿: 无新检测期间目标仍在移动, 误差按Kalman速度外推
				// (直播开启时推理帧率掉到40-66fps, 旧代码误差冻结在旧画面位置→吸不住)
				// 阈值20px/s过滤Kalman速度噪声; 单帧外推限幅12px防过冲; 有新检测时被真实检测覆盖, 无累积风险
				if (std::abs(m_lockedTargetSpeedX) > 20.0f || std::abs(m_lockedTargetSpeedY) > 20.0f) {
					float exX = m_lockedTargetSpeedX * dt;
					float exY = m_lockedTargetSpeedY * dt;
					float exMag = std::sqrt(exX * exX + exY * exY);
					if (exMag > 12.0f) { float s = 12.0f / exMag; exX *= s; exY *= s; }
					m_lockedX += exX;
					m_lockedY += exY;
				}
			}
			if (aimActive) {
				computeMove(m_lockedX, m_lockedY, dt, config, recoilVelX, recoilVelY);
			} else {
				m_moveX = 0; m_moveY = 0;
			}
			m_hasTarget = true;
			return;
		}

		m_hasTarget = false;
		m_lockedTrackId = -1;
		m_lockedClassId = -1;
		m_lockConfidence = 0;
		m_lockFrames = 0;
		m_moveX = 0; m_moveY = 0;
		m_smoothInit = false;
		m_smoothW = 0; m_smoothH = 0;
		m_lockedTargetSpeed = 0;
		m_lockedTargetSpeedX = 0; m_lockedTargetSpeedY = 0;
		m_prevErrorDist = 0;
		m_ffBrakeActive = false;
		m_userSuppress = 0;
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

	float useX = lockedTrack.rawCx;
	float useY = lockedTrack.rawCy;
	if (config.kalman_enabled && lockedTrack.kalman.isInitialized()) {
		useX = lockedTrack.kalman.getPredictedX();
		useY = lockedTrack.kalman.getPredictedY();
	}

	m_lockedTrackId = bestTrackId;
	m_lockedClassId = lockedTrack.classId;
	m_lockedW = lockedTrack.w;
	m_lockedH = lockedTrack.h;
	m_lockConfidence = lockedTrack.lockConfidence;
	m_lockFrames = 1;
	m_moveX = 0; m_moveY = 0;
	m_prevMoveX = 0; m_prevMoveY = 0;
	m_prevErrorX = 0; m_prevErrorY = 0;
	m_integralX = 0; m_integralY = 0;
	m_accelFactor = 1.0f;
	m_subPixelX = 0; m_subPixelY = 0;
	m_shootFrames = 0; m_stopFireFrames = 0;
	m_filteredErrorX = 0; m_filteredErrorY = 0;
	m_lockedTargetSpeed = lockedTrack.lastSpeed;
	if (lockedTrack.kalman.isInitialized()) {
		m_lockedTargetSpeedX = lockedTrack.kalman.getVx();
		m_lockedTargetSpeedY = lockedTrack.kalman.getVy();
	} else {
		m_lockedTargetSpeedX = 0;
		m_lockedTargetSpeedY = 0;
	}
	m_smoothH = lockedTrack.h;
	m_smoothW = lockedTrack.w;
	m_smoothInit = true;

	float targetX = (useX - m_smoothW / 2.0f + m_smoothW * config.aim_position_x) - captureCX;
	float targetY = (useY - m_smoothH / 2.0f + m_smoothH * getAimPosYForClass(lockedTrack.classId, config)) - captureCY;

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
	if (aimActive) {
		computeMove(m_lockedX, m_lockedY, dt, config, recoilVelX, recoilVelY);
	} else {
		m_moveX = 0; m_moveY = 0;
	}
}

void AimAssist::computeMove(float errorX, float errorY, float dt, const D5RenderConfig& config, float recoilVelX, float recoilVelY) {
	// 完整PID + 前馈 + 惯性系统（来自备份版本）
	// 只添加了过冲保护钳位

	float targetSpeed = m_lockedTargetSpeed;
	float dist = std::sqrt(errorX * errorX + errorY * errorY);
	float deadzone = config.pid_deadzone;
	m_ffBrakeActive = false;

	if (dist < deadzone) {
		m_moveX = 0; m_moveY = 0;
		m_currentGain = 0;
		m_accelFactor = 1.0f;
		m_prevMoveX = 0; m_prevMoveY = 0;
		m_prevErrorX = 0; m_prevErrorY = 0;
		m_integralX = 0; m_integralY = 0;
		m_shootFrames = 0; m_stopFireFrames = 0;
		m_filteredErrorX = 0; m_filteredErrorY = 0;
		m_prevErrorDist = dist;
		return;
	}

	if (m_shooting) {
		m_shootFrames++;
		m_stopFireFrames = 0;
	} else {
		m_stopFireFrames++;
		if (m_stopFireFrames > 30) m_shootFrames = 0;
	}

	float emaAlpha = 0.3f;
	if (m_shooting && m_shootFrames > 1) {
		emaAlpha = 0.12f;
		if (targetSpeed > config.ff_target_speed) {
			emaAlpha = 0.9f;
		}
	}
	if (m_shootFrames <= 1) {
		m_filteredErrorX = errorX;
		m_filteredErrorY = errorY;
	} else {
		m_filteredErrorX += (errorX - m_filteredErrorX) * emaAlpha;
		m_filteredErrorY += (errorY - m_filteredErrorY) * emaAlpha;
	}
	float useErrorX = m_shooting ? m_filteredErrorX : errorX;
	float useErrorY = m_shooting ? m_filteredErrorY : errorY;

	float maxMove = config.move_max_step;
	float inertia = config.move_inertia;
	float jitterRange = config.move_jitter_range;
	float jitterScaleBase = config.move_jitter_scale;
	float accelNear = config.move_accel_near;
	float accelFar = config.move_accel_far;
	float fineDist = config.move_fine_dist;
	if (accelNear >= accelFar) accelFar = accelNear + 10.0f;

	float moveBoost = std::clamp(targetSpeed / 250.0f, 0.0f, 1.0f) * 0.5f;
	maxMove *= (1.0f + moveBoost);
	if (moveBoost > 0) {
		float nearBoost = std::clamp(targetSpeed / 250.0f, 0.0f, 1.0f) * 12.0f;
		accelNear += nearBoost;
		accelFar += nearBoost * 2.0f;
	}

	if (config.move_randomize) {
		std::uniform_real_distribution<float> maxStepJitter(-0.8f, 0.8f);
		std::uniform_real_distribution<float> inertiaJitter(-0.03f, 0.03f);
		std::uniform_real_distribution<float> jitterRJitter(-0.05f, 0.05f);
		std::uniform_real_distribution<float> jitterSJitter(-0.02f, 0.02f);
		maxMove = std::max(1.0f, maxMove + maxStepJitter(m_rng));
		inertia = std::clamp(inertia + inertiaJitter(m_rng), 0.0f, 0.95f);
		jitterRange = std::max(0.0f, jitterRange + jitterRJitter(m_rng));
		jitterScaleBase = std::max(0.0f, jitterScaleBase + jitterSJitter(m_rng));
	}

	float gain = config.pid_kp * config.speed_multiplier;
	float kdX = config.pid_kd;
	float kdY = config.pid_kd;

	if (m_shooting && m_shootFrames <= 6) {
		kdY *= 1.8f;
		gain *= 0.85f;
	} else if (m_shooting && m_shootFrames > 6) {
		kdX *= 1.5f;
		kdY *= 1.3f;
	}

	float effectiveInertia = inertia * config.aim_smoothness;
	effectiveInertia = std::clamp(effectiveInertia, 0.0f, 0.92f);

	if (m_lockFrames >= 1 && m_lockFrames <= 6) {
		float ramp = std::min((float)(m_lockFrames - 1) / 5.0f, 1.0f);
		effectiveInertia = effectiveInertia + (1.0f - effectiveInertia) * (1.0f - ramp * ramp);
	}

	float prevDist = std::sqrt(m_prevErrorX * m_prevErrorX + m_prevErrorY * m_prevErrorY);
	if (prevDist > 1.0f && dist < prevDist * 0.9f) {
		float shrinkRatio = dist / prevDist;
		effectiveInertia *= shrinkRatio;
	}
	if (dist < fineDist * 2.0f) {
		float brakeFactor = dist / (fineDist * 2.0f);
		effectiveInertia *= brakeFactor * brakeFactor;
	}

	float desiredX = useErrorX * gain;
	float desiredY = useErrorY * gain;

	if (dist < fineDist) {
		m_integralX = 0;
		m_integralY = 0;
	}

	float rawIntegralX = m_integralX + useErrorX * config.pid_ki * config.speed_multiplier * dt;
	float rawIntegralY = m_integralY + useErrorY * config.pid_ki * config.speed_multiplier * dt;
	float integralLimit = maxMove * 0.3f;
	m_integralX = std::clamp(rawIntegralX, -integralLimit, integralLimit);
	m_integralY = std::clamp(rawIntegralY, -integralLimit, integralLimit);
	desiredX += m_integralX;
	desiredY += m_integralY;

	float dErrorX = useErrorX - m_prevErrorX;
	float dErrorY = useErrorY - m_prevErrorY;
	float maxDError = std::max(std::abs(useErrorX), std::abs(useErrorY)) * 0.5f + 5.0f;
	dErrorX = std::clamp(dErrorX, -maxDError, maxDError);
	dErrorY = std::clamp(dErrorY, -maxDError, maxDError);
	desiredX += dErrorX * kdX;
	desiredY += dErrorY * kdY;

	if (m_shooting && std::abs(recoilVelY) > 0.01f) {
		desiredY += recoilVelY * kdY * 0.8f;
	}

	m_prevErrorX = useErrorX;
	m_prevErrorY = useErrorY;

	float ffScale = 1.0f + 0.8f * std::clamp(targetSpeed / 350.0f, 0.0f, 1.0f);
	float accelTarget;
	if (dist > accelFar) {
		accelTarget = ffScale;
	} else if (dist > accelNear) {
		accelTarget = (0.25f + 0.75f * (dist - accelNear) / (accelFar - accelNear)) * ffScale;
	} else {
		accelTarget = (0.3f + 0.4f * dist / accelNear) * ffScale;
	}
	m_accelFactor += (accelTarget - m_accelFactor) * 0.15f;
	m_accelFactor = std::clamp(m_accelFactor, 0.2f, 1.8f);
	desiredX *= m_accelFactor;
	desiredY *= m_accelFactor;

	float errShrinkRatio = 1.0f;
	if (dist < 80.0f && m_prevErrorDist > dist + 1.0f) {
		errShrinkRatio = std::max(0.25f, dist / std::max(m_prevErrorDist, 1.0f));
	}
	if (errShrinkRatio < 0.6f || dist < fineDist) {
		float brake = std::min(errShrinkRatio, std::max(0.25f, dist / std::max(fineDist, 1.0f)));
		effectiveInertia = std::min(effectiveInertia, brake * 0.5f);
		m_ffBrakeActive = true;
	}

	if (config.ff_enabled && targetSpeed > config.ff_target_speed) {
		float ffGain = config.ff_gain;
		if (dist < fineDist) {
			ffGain *= std::max(0.0f, (dist / fineDist) * 0.4f);
		} else if (dist < fineDist * 2.0f) {
			ffGain *= 0.5f;
		}
		if (!m_ffBrakeActive && ffGain > 0 && errorX * m_lockedTargetSpeedX >= 0) {
			desiredX += m_lockedTargetSpeedX * dt * ffGain;
		}
		if (!m_ffBrakeActive && ffGain > 0 && errorY * m_lockedTargetSpeedY >= 0) {
			desiredY += m_lockedTargetSpeedY * dt * ffGain;
		}
	}

	// [FIX] 方向反转时清零prevMove，防止回弹
	if (m_prevMoveX * errorX < 0.0f) m_prevMoveX = 0;
	if (m_prevMoveY * errorY < 0.0f) m_prevMoveY = 0;

	desiredX = m_prevMoveX * effectiveInertia + desiredX * (1.0f - effectiveInertia);
	desiredY = m_prevMoveY * effectiveInertia + desiredY * (1.0f - effectiveInertia);

	// [FIX] 过冲保护：移动量不超过剩余误差的95%
	float maxAllowedX = std::abs(errorX) * 0.95f;
	float maxAllowedY = std::abs(errorY) * 0.95f;
	if (std::abs(desiredX) > maxAllowedX) desiredX = (desiredX > 0 ? 1.0f : -1.0f) * maxAllowedX;
	if (std::abs(desiredY) > maxAllowedY) desiredY = (desiredY > 0 ? 1.0f : -1.0f) * maxAllowedY;

	m_prevMoveX = desiredX;
	m_prevMoveY = desiredY;

	if (errShrinkRatio < 0.6f || dist < fineDist) {
		float tail = std::max(0.25f, dist / std::max(fineDist, 1.0f));
		desiredX = errorX * tail * 0.8f;
		desiredY = errorY * tail * 0.8f;
		m_prevMoveX = desiredX;
		m_prevMoveY = desiredY;
	}

	m_prevErrorDist = dist;

	float moveDist = std::sqrt(desiredX * desiredX + desiredY * desiredY);
	if (moveDist > maxMove) {
		float scale = maxMove / moveDist;
		desiredX *= scale;
		desiredY *= scale;
	}

	// [FIX] 最终钳位
	maxAllowedX = std::abs(errorX) * 0.95f;
	maxAllowedY = std::abs(errorY) * 0.95f;
	if (maxAllowedX < 0.5f) maxAllowedX = 0.5f;
	if (maxAllowedY < 0.5f) maxAllowedY = 0.5f;
	desiredX = std::clamp(desiredX, -maxAllowedX, maxAllowedX);
	desiredY = std::clamp(desiredY, -maxAllowedY, maxAllowedY);

	if (!m_shooting && m_stopFireFrames > 0 && m_stopFireFrames <= 5) {
		desiredX *= 0.3f;
		desiredY *= 0.3f;
	}

	if (dist < fineDist) {
		float fineScale = dist / fineDist;
		fineScale = fineScale * fineScale;
		fineScale = std::max(fineScale, 0.3f);
		desiredX *= fineScale;
		desiredY *= fineScale;
	}

	// [FIX] 移除随机抖动，它导致振荡
	// float jitterAmp = jitterRange * (0.3f + 0.7f * std::min(dist / 80.0f, 1.0f));
	// if (dist < fineDist) jitterAmp *= 0.2f;
	// std::uniform_real_distribution<float> jitterDist2(-jitterAmp, jitterAmp);
	// desiredX += jitterDist2(m_rng) * jitterScaleBase;
	// desiredY += jitterDist2(m_rng) * jitterScaleBase;

	m_currentGain = gain;
	m_moveX = desiredX;
	m_moveY = desiredY;
}

AimTarget AimAssist::getMoveDelta(float dt) {
	AimTarget delta = { 0, 0, 0, 0 };
	if (!m_hasTarget) {
		m_subPixelX = 0;
		m_subPixelY = 0;
		m_diagPrevMoveX = 0;
		m_diagPrevMoveY = 0;
		m_diagPrevDt = dt;
		return delta;
	}

	// [FIX] timeScale按真实dt补偿, 钳位[0.2,1.8]: 移动每秒累计量恒定≈62.5拍
	// (旧[0.9,1.1]在检测间隔25ms+时移动量缺口45% → 低帧率下吸不住; 主循环144Hz时0.43、
	//  隐藏态120Hz时0.52、卡顿25ms时1.56, 全范围正确补偿, 只有极端异常帧被钳住)
	float timeScale = (dt > 0.001f) ? (dt / 0.016f) : 1.0f;
	timeScale = std::clamp(timeScale, 0.2f, 1.8f);
	float scaledMoveX = m_moveX * timeScale;
	float scaledMoveY = m_moveY * timeScale;

	// [FIX] 去掉maxMoveX钳位，不再限制跟踪能力
	// float maxMoveX = std::abs(m_lockedX) * 0.85f;
	// float maxMoveY = std::abs(m_lockedY) * 0.85f;

	// [FIX] 过冲预判：如果移动后会越过目标，钳位到恰好抵达
	float afterX = m_lockedX - scaledMoveX;
	float afterY = m_lockedY - scaledMoveY;
	if (m_lockedX > 0.0f && afterX < 0.0f) scaledMoveX = m_lockedX;
	else if (m_lockedX < 0.0f && afterX > 0.0f) scaledMoveX = m_lockedX;
	if (m_lockedY > 0.0f && afterY < 0.0f) scaledMoveY = m_lockedY;
	else if (m_lockedY < 0.0f && afterY > 0.0f) scaledMoveY = m_lockedY;

	float prevLockedX = m_lockedX;
	float prevLockedY = m_lockedY;
	m_lockedX -= scaledMoveX;
	m_lockedY -= scaledMoveY;

	m_moveX *= 0.92f;
	m_moveY *= 0.92f;

	if (prevLockedX * m_lockedX < 0) {
		m_lockedX = 0;
		m_prevMoveX = 0;
		m_integralX = 0;
	}
	if (prevLockedY * m_lockedY < 0) {
		m_lockedY = 0;
		m_prevMoveY = 0;
		m_integralY = 0;
	}

	if (m_diagStutterLogCooldown > 0) m_diagStutterLogCooldown--;
	bool stutterDetected = false;
	if (dt > 0.040f) stutterDetected = true;
	float prevMag = std::sqrt(m_diagPrevMoveX * m_diagPrevMoveX + m_diagPrevMoveY * m_diagPrevMoveY);
	float curMag = std::sqrt(scaledMoveX * scaledMoveX + scaledMoveY * scaledMoveY);
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
			m_diagPrevMoveX, m_diagPrevMoveY, scaledMoveX, scaledMoveY);
	}
	m_diagPrevMoveX = scaledMoveX;
	m_diagPrevMoveY = scaledMoveY;
	m_diagPrevDt = dt;

	m_subPixelX += scaledMoveX;
	m_subPixelY += scaledMoveY;
	delta.screenX = (int)m_subPixelX;
	delta.screenY = (int)m_subPixelY;
	m_subPixelX -= delta.screenX;
	m_subPixelY -= delta.screenY;
	delta.confidence = m_lockConfidence;
	delta.distance = std::sqrt(m_lockedX * m_lockedX + m_lockedY * m_lockedY);
	return delta;
}

void AimAssist::reset() {
	m_hasTarget = false;
	m_aimActive = false;
	m_lockedTrackId = -1;
	m_lockedClassId = -1;
	m_lockedX = 0; m_lockedY = 0;
	m_lostFrames = 0;
	m_lockConfidence = 0;
	m_lockFrames = 0;
	m_lockedW = 0; m_lockedH = 0;
	m_moveX = 0; m_moveY = 0;
	m_smoothH = 0; m_smoothW = 0;
	m_smoothInit = false;
	m_currentGain = 0.20f;
	m_aimFov = 0.5f;
	m_prevMoveX = 0; m_prevMoveY = 0;
	m_prevErrorX = 0; m_prevErrorY = 0;
	m_integralX = 0; m_integralY = 0;
	m_accelFactor = 1.0f;
	m_subPixelX = 0; m_subPixelY = 0;
	m_shootFrames = 0; m_stopFireFrames = 0;
	m_filteredErrorX = 0; m_filteredErrorY = 0;
	m_lockedTargetSpeed = 0;
	m_lockedTargetSpeedX = 0; m_lockedTargetSpeedY = 0;
	m_prevErrorDist = 0;
	m_ffBrakeActive = false;
	m_userSuppress = 0;
	m_switchCooldown = 0;
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
	m_moveX = 0; m_moveY = 0;
	m_prevMoveX = 0; m_prevMoveY = 0;
}

void AimAssist::resetAfterQuickscopeSnap(float snapX, float snapY) {
	m_lockedX -= snapX;
	m_lockedY -= snapY;
	m_moveX = 0; m_moveY = 0;
	m_prevMoveX = 0; m_prevMoveY = 0;
	m_prevErrorX = m_lockedX;
	m_prevErrorY = m_lockedY;
}

void AimAssist::getHeadSnapPos(float& outX, float& outY, float aimPositionY) const {
	const float headRatio = 0.1f;
	const float snapScale = 0.8f;
	outX = m_lockedX * snapScale;
	outY = (m_lockedY - m_smoothH * (aimPositionY - headRatio)) * snapScale;
}
