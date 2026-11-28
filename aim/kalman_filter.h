#pragma once
#include <cmath>
#include <algorithm>
// [v230] CA恒加速+IMM双模型Kalman: 移植自aim_sim/kalman.py(仿真10.7分最优)
// 每轴3状态[x,vx,ax](jerk方差密度Q), IMM并联CV副模型, innov对数似然EWMA递推mu, 输出按mu加权融合
class KalmanAxisCA {
public:
	bool initialized = false;
	float x = 0, vx = 0, ax = 0;
	float p00 = 1, p01 = 0, p02 = 0, p11 = 1, p12 = 0, p22 = 1;
	float processNoise = 8.0e7f;   // jerk方差密度(px^2/s^5)
	float measurementNoise = 4.0f;
	float lastInnov = 0;
	float cvQ = 0;                 // IMM副模型q_cv(px^2/s^3, 0=关IMM)
	float immLam = 0.3f;           // 似然EWMA新证据权重
	float llCa = 0, llCv = 0;      // 窗口对数似然(EWMA)
	float muCa = 1.0f;
	float cvX = 0, cvVx = 0, cvP00 = 1, cvP01 = 0, cvP11 = 1;
	float cvLastInnov = 0;
	bool cvInit = false;
	bool immOn() const { return cvQ > 0.0f; }
	void init(float z) {
		x = z; vx = 0; ax = 0;
		p00 = 100; p01 = 0; p02 = 0; p11 = 10000; p12 = 0; p22 = 100000;
		initialized = true;
		if (immOn()) {
			cvX = z; cvVx = 0; cvP00 = 100; cvP01 = 0; cvP11 = 10000; cvInit = true;
			muCa = 0.5f; llCa = 0; llCv = 0;
		}
	}
	void predict(float dt) {
		if (!initialized) return;
		if (immOn()) {
			cvX += cvVx * dt;
			float q = cvQ;
			float dt2 = dt * dt, dt3 = dt2 * dt, dt4 = dt3 * dt;
			float sp00 = cvP00 + 2.0f * dt * cvP01 + dt2 * cvP11;
			float sp01 = cvP01 + dt * cvP11;
			cvP00 = sp00 + q * dt4 / 4.0f;
			cvP01 = sp01 + q * dt3 / 2.0f;
			cvP11 = cvP11 + q * dt2;
		}
		float h = 0.5f * dt * dt;
		x += vx * dt + ax * h;
		vx += ax * dt;
		float q = processNoise;
		float dt2 = dt * dt, dt3 = dt2 * dt, dt4 = dt3 * dt, dt5 = dt4 * dt;
		float n00 = p00 + 2.0f * dt * p01 + 2.0f * h * p02 + dt2 * p11 + 2.0f * dt * h * p12 + h * h * p22;
		float n01 = p01 + dt * p11 + h * p12 + dt * p02 + dt2 * p12 + dt * h * p22;
		float n02 = p02 + dt * p12 + h * p22;
		float n11 = p11 + 2.0f * dt * p12 + dt2 * p22;
		float n12 = p12 + dt * p22;
		p00 = n00 + q * dt5 / 20.0f;
		p01 = n01 + q * dt4 / 8.0f;
		p02 = n02 + q * dt3 / 6.0f;
		p11 = n11 + q * dt3 / 3.0f;
		p12 = n12 + q * dt2 / 2.0f;
		p22 = p22 + q * dt;
	}
	void update(float z) {
		if (!initialized) { init(z); return; }
		float r = measurementNoise;
		float s = p00 + r;
		float k0 = p00 / s;
		float k1 = p01 / s;
		float k2 = p02 / s;
		float dy = z - x;
		lastInnov = dy;
		x += k0 * dy;
		vx += k1 * dy;
		ax += k2 * dy;
		vx = std::clamp(vx, -1500.0f, 1500.0f);
		ax = std::clamp(ax, -8000.0f, 8000.0f);
		float n00 = p00 * (1.0f - k0);
		float n01 = p01 * (1.0f - k0);
		float n02 = p02 * (1.0f - k0);
		float n11 = p11 - k1 * p01;
		float n12 = p12 - k1 * p02;
		float n22 = p22 - k2 * p02;
		p00 = std::max(0.001f, n00);
		p01 = n01;
		p02 = n02;
		p11 = std::max(0.001f, n11);
		p12 = n12;
		p22 = std::max(0.001f, n22);
		if (immOn()) {
			if (!cvInit) { cvX = z; cvVx = 0; cvP00 = 100; cvP01 = 0; cvP11 = 10000; cvInit = true; }
			float sc = cvP00 + r;
			float kc0 = cvP00 / sc;
			float kc1 = cvP01 / sc;
			float dyc = z - cvX;
			cvLastInnov = dyc;
			cvX += kc0 * dyc;
			cvVx += kc1 * dyc;
			cvVx = std::clamp(cvVx, -1500.0f, 1500.0f);
			cvP00 = std::max(0.001f, cvP00 * (1.0f - kc0));
			cvP01 = cvP01 * (1.0f - kc0);
			cvP11 = std::max(0.001f, cvP11 - kc1 * cvP01);
			float sCa = p00 + r;
			float sCv = cvP00 + r;
			float illCa = -0.5f * (lastInnov * lastInnov / sCa + std::log(sCa));
			float illCv = -0.5f * (cvLastInnov * cvLastInnov / sCv + std::log(sCv));
			llCa = (1.0f - immLam) * llCa + immLam * illCa;
			llCv = (1.0f - immLam) * llCv + immLam * illCv;
			float m = std::clamp(llCa - llCv, -30.0f, 30.0f);
			muCa = 1.0f / (1.0f + std::exp(-m));
			muCa = std::clamp(muCa, 0.05f, 0.95f);
		}
	}
	float getFx() const { return (immOn() && cvInit) ? muCa * x + (1.0f - muCa) * cvX : x; }
	float getFv() const { return (immOn() && cvInit) ? muCa * vx + (1.0f - muCa) * cvVx : vx; }
	float getFa() const { return (immOn() && cvInit) ? muCa * ax : ax; }
	void setImm(float qCv, float lam) { cvQ = qCv; immLam = lam; }
};
class KalmanFilter2D {
public:
	KalmanFilter2D() { reset(); }
	void reset() {
		m_fx = KalmanAxisCA();
		m_fy = KalmanAxisCA();
		m_fx.measurementNoise = m_measurementNoise;
		m_fy.measurementNoise = m_measurementNoise;
		m_age = 0;
	}
	void init(float x, float y) {
		m_fx.init(x);
		m_fy.init(y);
		m_age = 0;
	}
	void predict(float dt) {
		m_fx.predict(dt);
		m_fy.predict(dt);
		m_age++;
	}
	void update(float zx, float zy) {
		m_fx.update(zx);
		m_fy.update(zy);
		m_age = 0;
	}
	void updateWithVelocity(float zx, float zy, float dt) {
		(void)dt;  // [v230] CA模型Q传播内部已含dt, 接口保留兼容
		m_fx.update(zx);
		m_fy.update(zy);
		m_age = 0;
	}
	float getX() const { return m_fx.getFx(); }
	float getY() const { return m_fy.getFx(); }
	float getVx() const { return m_fx.getFv(); }
	float getVy() const { return m_fy.getFv(); }
	float getAX() const { return m_fx.getFa(); }
	float getAY() const { return m_fy.getFa(); }
	float getAMag() const { return std::sqrt(m_fx.ax * m_fx.ax + m_fy.ax * m_fy.ax); }
	float getInnovMag() const { return std::sqrt(m_fx.lastInnov * m_fx.lastInnov + m_fy.lastInnov * m_fy.lastInnov); }
	float getSpeed() const { return std::sqrt(m_fx.vx * m_fx.vx + m_fy.vx * m_fy.vx); }  // 原CA速度(与aim_sim一致)
	bool isInitialized() const { return m_fx.initialized; }
	int getAge() const { return m_age; }
	void setProcessNoise(float q) { m_fx.processNoise = q; m_fy.processNoise = q; }
	void setMeasurementNoise(float r) { m_measurementNoise = r; m_fx.measurementNoise = r; m_fy.measurementNoise = r; }
	void setImm(float qCv, float lam) { m_fx.setImm(qCv, lam); m_fy.setImm(qCv, lam); }
	void setPredictAhead(float t) { m_predictAhead = t; }
	float getPredictedX() const { return getX() + getVx() * m_predictAhead; }
	float getPredictedY() const { return getY() + getVy() * m_predictAhead; }
	// 快速目标自动加大预测提前量：纯延迟预测追不上高速移动，最高+50ms
	float getSpeedAdaptiveAhead() const {
		float speed = getSpeed();
		float extra = std::clamp(speed / 2000.0f, 0.0f, 0.05f);
		return m_predictAhead + extra;
	}
	float getFastPredictedX() const { return getX() + getVx() * getSpeedAdaptiveAhead(); }
	float getFastPredictedY() const { return getY() + getVy() * getSpeedAdaptiveAhead(); }
private:
	KalmanAxisCA m_fx;
	KalmanAxisCA m_fy;
	int m_age = 0;
	float m_measurementNoise = 4.0f;
	float m_predictAhead = 0.018f;
};
