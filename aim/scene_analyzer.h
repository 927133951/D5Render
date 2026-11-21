#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "inference/yolo_postprocess.h"
struct SceneMetrics {
	float complexity = 0;
	int targetCount = 0;
	float avgConfidence = 0;
	float overlapRatio = 0;
	float density = 0;
	float motionLevel = 0;
};
class SceneAnalyzer {
public:
	SceneMetrics analyze(const std::vector<Detection>& detections, int captureW, int captureH, float prevAvgSpeed) {
		SceneMetrics m;
		m.targetCount = (int)detections.size();
		if (m.targetCount == 0) {
			m.complexity = 0;
			return m;
		}
		float totalConf = 0;
		float totalOverlap = 0;
		int overlapPairs = 0;
		for (int i = 0; i < m.targetCount; ++i) {
			totalConf += detections[i].confidence;
			for (int j = i + 1; j < m.targetCount; ++j) {
				float ox = std::max(0.0f, std::min(detections[i].x2(), detections[j].x2()) - std::max(detections[i].x1(), detections[j].x1()));
				float oy = std::max(0.0f, std::min(detections[i].y2(), detections[j].y2()) - std::max(detections[i].y1(), detections[j].y1()));
				float overlapArea = ox * oy;
				float areaI = detections[i].w * detections[i].h;
				float areaJ = detections[j].w * detections[j].h;
				float minArea = std::min(areaI, areaJ);
				if (minArea > 0) {
					totalOverlap += overlapArea / minArea;
					overlapPairs++;
				}
			}
		}
		m.avgConfidence = totalConf / m.targetCount;
		m.overlapRatio = overlapPairs > 0 ? totalOverlap / overlapPairs : 0;
		float sceneArea = (float)(captureW * captureH);
		float targetArea = 0;
		for (const auto& d : detections) targetArea += d.w * d.h;
		m.density = sceneArea > 0 ? targetArea / sceneArea : 0;
		m.motionLevel = prevAvgSpeed;
		m.complexity = 0;
		m.complexity += std::min(m.targetCount / 8.0f, 1.0f) * 0.3f;
		m.complexity += (1.0f - m.avgConfidence) * 0.2f;
		m.complexity += std::min(m.overlapRatio, 1.0f) * 0.25f;
		m.complexity += std::min(m.density * 5.0f, 1.0f) * 0.1f;
		m.complexity += std::min(m.motionLevel / 200.0f, 1.0f) * 0.15f;
		m.complexity = std::clamp(m.complexity, 0.0f, 1.0f);
		m_history.push_back(m.complexity);
		if ((int)m_history.size() > 30) m_history.pop_front();
		return m;
	}
	float getSmoothedComplexity() const {
		if (m_history.empty()) return 0;
		float sum = 0;
		for (float v : m_history) sum += v;
		return sum / m_history.size();
	}
	void reset() { m_history.clear(); }
private:
	std::deque<float> m_history;
};
