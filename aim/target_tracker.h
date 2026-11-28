#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <deque>
#include "inference/yolo_postprocess.h"
struct TrackedTarget {
	int trackId = -1;
	int classId = -1;
	float cx = 0, cy = 0;
	float rawCx = 0, rawCy = 0;
	float w = 0, h = 0;
	float confidence = 0;
	int lostFrames = 0;
	int hitFrames = 0;
	float lockConfidence = 0;
	float lastSpeed = 0;
	// [v231] 检测框稳定器: smoothW/H=尺寸EMA(突变帧冻结), dispCx/Cy=显示专用位置EMA(仅预览,不进控制)
	float smoothW = 0, smoothH = 0;
	float dispCx = 0, dispCy = 0;
	bool operator<(const TrackedTarget& o) const { return lockConfidence > o.lockConfidence; }
};
class TargetTracker {
public:
	TargetTracker() : m_nextId(0) {}
	void update(const std::vector<Detection>& detections, float dt, int captureW, int captureH) {
		// [v231] 干扰源预过滤: 噪声点/全屏火焰误检/细长拖影不进匹配、不建轨迹
		// · 尺寸<5px = 检测噪声散点; 全屏框(w/h>90%画面)=烟雾/闪光误检; 长宽比>12=火焰条纹
		std::vector<Detection> validDets;
		validDets.reserve(detections.size());
		for (const auto& d : detections) {
			if (d.w < 5.0f || d.h < 5.0f) continue;
			if (d.w > captureW * 0.9f || d.h > captureH * 0.9f) continue;
			float maxSide = std::max(d.w, d.h);
			float minSide = std::min(d.w, d.h);
			if (minSide > 1.0f && maxSide / minSide > 12.0f) continue;
			validDets.push_back(d);
		}
		const std::vector<Detection>& dets = validDets;
		std::vector<int> matchedDetIdx(dets.size(), -1);
		std::vector<int> matchedTrackIdx(m_tracks.size(), -1);
		int nDet = (int)dets.size();
		int nTrk = (int)m_tracks.size();
		struct MatchPair { int det; int trk; float cost; };
		std::vector<MatchPair> pairs;
		pairs.reserve(nDet * nTrk);
			for (int d = 0; d < nDet; ++d) {
				for (int t = 0; t < nTrk; ++t) {
					// 用上次已知平滑位置(cx)做匹配，不用getFastPredictedX
					// (预测位置含被视角旋转污染的速度→匹配位置偏→误关联→aimpoint跳变)
					float matchX = m_tracks[t].cx;
					float matchY = m_tracks[t].cy;
					float dx = dets[d].cx - matchX;
					float dy = dets[d].cy - matchY;
					float dist = std::sqrt(dx * dx + dy * dy);
					float classPenalty = (dets[d].classId != m_tracks[t].classId) ? 500.0f : 0.0f;
					float sizeRatio = 1.0f;
					float detArea = dets[d].w * dets[d].h;
					float trkArea = m_tracks[t].w * m_tracks[t].h;
					if (detArea > 0 && trkArea > 0) {
						sizeRatio = detArea / trkArea;
						if (sizeRatio > 3.0f || sizeRatio < 0.33f) classPenalty += 200.0f;
					}
					float cost = dist + classPenalty;
					// 不再用speedBoost：速度被视角旋转污染→匹配半径膨胀→误关联→aimpoint跳变
					float effThreshold = m_matchThreshold;
					if (cost < effThreshold) pairs.push_back({d, t, cost});
			}
		}
		std::sort(pairs.begin(), pairs.end(), [](const MatchPair& a, const MatchPair& b) { return a.cost < b.cost; });
		for (const auto& p : pairs) {
			if (matchedDetIdx[p.det] >= 0 || matchedTrackIdx[p.trk] >= 0) continue;
			matchedDetIdx[p.det] = p.trk;
			matchedTrackIdx[p.trk] = p.det;
		}
		for (int t = 0; t < nTrk; ++t) {
			if (matchedTrackIdx[t] >= 0) {
				int d = matchedTrackIdx[t];
				float pRawX = m_tracks[t].rawCx, pRawY = m_tracks[t].rawCy;
				m_tracks[t].rawCx = dets[d].cx;
				m_tracks[t].rawCy = dets[d].cy;
				m_tracks[t].w = dets[d].w;
				m_tracks[t].h = dets[d].h;
				// [v231] 置信度EMA: 置信度在阈值边缘振荡(0.26↔0.51)导致标签%与lockConfidence逐帧跳变
				m_tracks[t].confidence = m_tracks[t].confidence * 0.5f + dets[d].confidence * 0.5f;
				m_tracks[t].classId = dets[d].classId;
				// [v231] 显示尺寸EMA+突变冻结: 遮挡/火焰膨胀使尺寸逐帧跳动(64→406实测)→框闪。
				// 突变帧(比值>2.5或<0.4)冻结显示尺寸沿用上帧; 正常帧EMA(α=0.5)平滑;
				// 原始w/h照常更新——aim层boxBurst检测与匹配sizeRatio惩罚仍用原始值
				if (m_tracks[t].smoothH > 1.0f) {
					float dispRatio = dets[d].h / m_tracks[t].smoothH;
					if (dispRatio > 2.5f || dispRatio < 0.4f) {
						// 突变帧: 冻结显示尺寸
					} else {
						m_tracks[t].smoothW = m_tracks[t].smoothW * 0.5f + dets[d].w * 0.5f;
						m_tracks[t].smoothH = m_tracks[t].smoothH * 0.5f + dets[d].h * 0.5f;
					}
				} else {
					m_tracks[t].smoothW = dets[d].w;
					m_tracks[t].smoothH = dets[d].h;
				}
				// [v231] 显示位置EMA(仅预览显示, 控制链仍用rawCx/rawCy不受影响)
				m_tracks[t].dispCx = m_tracks[t].dispCx * 0.55f + dets[d].cx * 0.45f;
				m_tracks[t].dispCy = m_tracks[t].dispCy * 0.55f + dets[d].cy * 0.45f;
				{  // [v234] 纯跟踪: 原始检测位置直通+有限差分速度, 零预测
					m_tracks[t].cx = dets[d].cx;
					m_tracks[t].cy = dets[d].cy;
					float spdX = dets[d].cx - pRawX, spdY = dets[d].cy - pRawY;
					m_tracks[t].lastSpeed = (dt > 0.0001f) ? std::sqrt(spdX * spdX + spdY * spdY) / dt : 0.0f;
				}
				m_tracks[t].lostFrames = 0;
				m_tracks[t].hitFrames++;
				m_tracks[t].lockConfidence = computeTrackConfidence(m_tracks[t]);
			} else {
				m_tracks[t].lostFrames++;
				m_tracks[t].lockConfidence *= 0.92f;
				if (m_tracks[t].lockConfidence < 0.01f) m_tracks[t].lockConfidence = 0.01f;
			}
		}
		for (int d = 0; d < nDet; ++d) {
			if (matchedDetIdx[d] >= 0) continue;
			TrackedTarget nt;
			nt.trackId = m_nextId++;
			nt.classId = dets[d].classId;
			nt.cx = dets[d].cx;
			nt.cy = dets[d].cy;
			nt.rawCx = dets[d].cx;
			nt.rawCy = dets[d].cy;
			nt.w = dets[d].w;
			nt.h = dets[d].h;
			nt.confidence = dets[d].confidence;
			nt.smoothW = dets[d].w;
			nt.smoothH = dets[d].h;
			nt.dispCx = dets[d].cx;
			nt.dispCy = dets[d].cy;
			nt.lostFrames = 0;
			nt.hitFrames = 1;
			nt.lockConfidence = 0.1f;
			nt.lastSpeed = 0;
			m_tracks.push_back(nt);
		}
		m_tracks.erase(std::remove_if(m_tracks.begin(), m_tracks.end(),
			[this](const TrackedTarget& t) { return t.lostFrames > m_maxLostFrames; }),
			m_tracks.end());
	}
	const std::vector<TrackedTarget>& getTracks() const { return m_tracks; }
	bool findTrackDataById(int trackId, TrackedTarget& out) const {
		for (const auto& t : m_tracks) { if (t.trackId == trackId) { out = t; return true; } }
		return false;
	}
	void reset() {
		m_tracks.clear();
		m_nextId = 0;
	}
	void setMatchThreshold(float t) { m_matchThreshold = t; }
	void setMaxLostFrames(int n) { m_maxLostFrames = n; }
private:
	std::vector<TrackedTarget> m_tracks;
	int m_nextId;
	float m_matchThreshold = 150.0f;
	int m_maxLostFrames = 30;
	float computeTrackConfidence(const TrackedTarget& t) const {
		float conf = 0.3f;
		conf += std::min(t.hitFrames / 8.0f, 0.35f);
		conf += t.confidence * 0.25f;
		if (t.lostFrames > 0) conf *= std::max(0.0f, 1.0f - t.lostFrames * 0.05f);
		return std::clamp(conf, 0.0f, 1.0f);
	}
};
