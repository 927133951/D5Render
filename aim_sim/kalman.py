# -*- coding: utf-8 -*-
# KalmanFilter2D: CA恒加速模型(每轴3状态: 位置/速度/加速度), 世界系滤波
# 原型: g:\A_Develop\DD\aim\kalman_filter.h CV版, 仿真调优升级为CA(v229a无加速度状态)
# 状态语义: 最后一次成功update对应的测量时刻(检测帧拍摄时刻), 外推到当前用predict_ext
# IMM扩展: CA主模型并联CV副模型, 按单帧innov似然递推模型概率mu, 输出按mu加权融合
#   - 平滑段(CV似然高)输出CV平滑速度, 机动段(CA似然高)输出CA快速响应
import math


def clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


class KalmanAxisCV:
    # 单轴CV滤波器 [x, vx], 加速度白噪声模型(q_cv为加速度方差密度px²/s³)
    def __init__(self):
        self.initialized = False
        self.x = 0.0
        self.vx = 0.0
        self.p00 = 1.0
        self.p01 = 0.0
        self.p11 = 1.0
        self.q_cv = 0.0
        self.measurement_noise = KalmanFilter2D.default_r
        self.last_innov = 0.0

    def init(self, z):
        self.x = z
        self.vx = 0.0
        self.p00 = 100.0
        self.p01 = 0.0
        self.p11 = 10000.0
        self.initialized = True

    def predict(self, dt):
        if not self.initialized:
            return
        self.x += self.vx * dt
        q = self.q_cv
        dt2 = dt * dt
        dt3 = dt2 * dt
        dt4 = dt3 * dt
        p00 = self.p00 + 2.0 * dt * self.p01 + dt2 * self.p11
        p01 = self.p01 + dt * self.p11
        self.p00 = p00 + q * dt4 / 4.0
        self.p01 = p01 + q * dt3 / 2.0
        self.p11 = self.p11 + q * dt2

    def update(self, z):
        if not self.initialized:
            self.init(z)
            return
        r = self.measurement_noise
        s = self.p00 + r
        k0 = self.p00 / s
        k1 = self.p01 / s
        dy = z - self.x
        self.last_innov = dy
        self.x += k0 * dy
        self.vx += k1 * dy
        self.vx = clamp(self.vx, -1500.0, 1500.0)
        self.p00 = max(0.001, self.p00 * (1.0 - k0))
        self.p01 = self.p01 * (1.0 - k0)
        self.p11 = max(0.001, self.p11 - k1 * self.p01)


class KalmanAxisCA:
    # 单轴CA滤波器 [x, vx, ax], P为对称3x3(6独立量)
    def __init__(self):
        self.initialized = False
        self.x = 0.0
        self.vx = 0.0
        self.ax = 0.0
        self.p00 = 1.0
        self.p01 = 0.0
        self.p02 = 0.0
        self.p11 = 1.0
        self.p12 = 0.0
        self.p22 = 1.0
        self.process_noise = KalmanFilter2D.default_q   # jerk方差密度(px²/s⁵)
        self.measurement_noise = KalmanFilter2D.default_r
        self.time_since_update = 0.0
        self.last_innov = 0.0
        self.cv = KalmanAxisCV()         # IMM副模型(q_cv>0时启用)
        self.mu_ca = 1.0                 # CA模型概率
        self.mu_cv = 0.0
        self.ll_ca = 0.0                 # 窗口对数似然(EWMA, 0=中性)
        self.ll_cv = 0.0
        self.imm_lam = 0.3               # EWMA新证据权重(大=快翻转, 小=强平滑)

    def imm_on(self):
        return self.cv.q_cv > 0.0

    def init(self, z):
        self.x = z
        self.vx = 0.0
        self.ax = 0.0
        self.p00 = 100.0
        self.p01 = 0.0
        self.p02 = 0.0
        self.p11 = 10000.0
        self.p12 = 0.0
        self.p22 = 100000.0
        self.initialized = True
        self.time_since_update = 0.0
        if self.imm_on():
            self.cv.init(z)
            self.mu_ca = 0.5
            self.mu_cv = 0.5
            self.ll_ca = 0.0
            self.ll_cv = 0.0

    def predict(self, dt):
        if not self.initialized:
            return
        if self.imm_on():
            self.cv.predict(dt)
        h = 0.5 * dt * dt
        self.x += self.vx * dt + self.ax * h
        self.vx += self.ax * dt
        q = self.process_noise
        dt2 = dt * dt
        dt3 = dt2 * dt
        dt4 = dt3 * dt
        dt5 = dt4 * dt
        p00 = self.p00 + 2.0 * dt * self.p01 + 2.0 * h * self.p02 + dt2 * self.p11 + 2.0 * dt * h * self.p12 + h * h * self.p22
        p01 = self.p01 + dt * self.p11 + h * self.p12 + dt * self.p02 + dt2 * self.p12 + dt * h * self.p22
        p02 = self.p02 + dt * self.p12 + h * self.p22
        p11 = self.p11 + 2.0 * dt * self.p12 + dt2 * self.p22
        p12 = self.p12 + dt * self.p22
        p22 = self.p22
        self.p00 = p00 + q * dt5 / 20.0
        self.p01 = p01 + q * dt4 / 8.0
        self.p02 = p02 + q * dt3 / 6.0
        self.p11 = p11 + q * dt3 / 3.0
        self.p12 = p12 + q * dt2 / 2.0
        self.p22 = p22 + q * dt
        self.time_since_update += dt

    def update(self, z):
        if not self.initialized:
            self.init(z)
            return
        r = self.measurement_noise
        s = self.p00 + r
        k0 = self.p00 / s
        k1 = self.p01 / s
        k2 = self.p02 / s
        dy = z - self.x
        self.last_innov = dy
        self.x += k0 * dy
        self.vx += k1 * dy
        self.ax += k2 * dy
        self.vx = clamp(self.vx, -1500.0, 1500.0)
        self.ax = clamp(self.ax, -8000.0, 8000.0)
        p00 = self.p00 * (1.0 - k0)
        p01 = self.p01 * (1.0 - k0)
        p02 = self.p02 * (1.0 - k0)
        p11 = self.p11 - k1 * self.p01
        p12 = self.p12 - k1 * self.p02
        p22 = self.p22 - k2 * self.p02
        self.p00 = max(0.001, p00)
        self.p01 = p01
        self.p02 = p02
        self.p11 = max(0.001, p11)
        self.p12 = p12
        self.p22 = max(0.001, p22)
        if self.imm_on():
            self.cv.update(z)
            s_ca = self.p00 + r
            s_cv = self.cv.p00 + r
            # 窗口对数似然(EWMA): 累积多帧证据, 单帧伪平滑不足以翻盘
            ill_ca = -0.5 * (self.last_innov * self.last_innov / s_ca + math.log(s_ca))
            ill_cv = -0.5 * (self.cv.last_innov * self.cv.last_innov / s_cv + math.log(s_cv))
            lam = self.imm_lam
            self.ll_ca = (1.0 - lam) * self.ll_ca + lam * ill_ca
            self.ll_cv = (1.0 - lam) * self.ll_cv + lam * ill_cv
            m = clamp(self.ll_ca - self.ll_cv, -30.0, 30.0)
            self.mu_ca = 1.0 / (1.0 + math.exp(-m))
            self.mu_ca = clamp(self.mu_ca, 0.05, 0.95)
            self.mu_cv = 1.0 - self.mu_ca
        self.time_since_update = 0.0

    def get_fx(self):
        # IMM融合位置: 按模型概率加权(imm关时纯CA)
        if self.imm_on() and self.cv.initialized:
            return self.mu_ca * self.x + self.mu_cv * self.cv.x
        return self.x

    def get_fv(self):
        if self.imm_on() and self.cv.initialized:
            return self.mu_ca * self.vx + self.mu_cv * self.cv.vx
        return self.vx

    def get_fa(self):
        if self.imm_on() and self.cv.initialized:
            return self.mu_ca * self.ax
        return self.ax


class KalmanFilter2D:
    default_q = 8.0e7        # 类级默认jerk噪声密度(新track继承, 便于仿真参数扫描)
    default_r = 4.0          # 类级默认测量噪声方差(px², 仿真检测噪声σ=2px)

    def __init__(self):
        self.fx = KalmanAxisCA()
        self.fy = KalmanAxisCA()
        self.process_noise = KalmanFilter2D.default_q
        self.measurement_noise = KalmanFilter2D.default_r
        self.predict_ahead = 0.018
        self.age = 0
        self.time_since_update = 0.0

    def reset(self):
        self.fx = KalmanAxisCA()
        self.fy = KalmanAxisCA()
        self.age = 0
        self.time_since_update = 0.0

    def init(self, x, y):
        self.fx.init(x)
        self.fy.init(y)
        self.age = 0
        self.time_since_update = 0.0

    def predict(self, dt):
        self.fx.predict(dt)
        self.fy.predict(dt)
        self.age += 1
        self.time_since_update += dt

    def update_with_velocity(self, zx, zy, dt):
        # dt参数保留接口兼容(CA模型Q传播内部已含dt)
        self.fx.update(zx)
        self.fy.update(zy)
        self.age = 0
        self.time_since_update = 0.0

    def get_x(self):
        return self.fx.get_fx()

    def get_y(self):
        return self.fy.get_fx()

    def get_vx(self):
        return self.fx.get_fv()

    def get_vy(self):
        return self.fy.get_fv()

    def get_a_x(self):
        return self.fx.get_fa()

    def get_a_y(self):
        return self.fy.get_fa()

    def get_innov_mag(self):
        if not (self.fx.initialized and self.fy.initialized):
            return 0.0
        return math.hypot(self.fx.last_innov, self.fy.last_innov)

    def get_a_mag(self):
        return math.hypot(self.fx.ax, self.fy.ax)

    def get_speed(self):
        return math.sqrt(self.fx.vx * self.fx.vx + self.fy.vx * self.fy.vx)

    def is_initialized(self):
        return self.fx.initialized

    def set_process_noise(self, q):
        self.process_noise = q
        self.fx.process_noise = q
        self.fy.process_noise = q

    def set_cv_q(self, q_cv):
        # IMM副模型Q(0=关闭IMM); 同步测量噪声
        self.fx.cv.q_cv = q_cv
        self.fy.cv.q_cv = q_cv
        self.fx.cv.measurement_noise = self.measurement_noise
        self.fy.cv.measurement_noise = self.measurement_noise

    def set_cv_lam(self, lam):
        self.fx.imm_lam = lam
        self.fy.imm_lam = lam

    def set_measurement_noise(self, r):
        self.measurement_noise = r
        self.fx.measurement_noise = r
        self.fy.measurement_noise = r
        self.fx.cv.measurement_noise = r
        self.fy.cv.measurement_noise = r

    def set_predict_ahead(self, t):
        self.predict_ahead = t

    def predict_ext(self, dt_el):
        # 外推到当前时刻(CA模型匀加速段精确): 返回(位置, 速度, 加速度)
        # IMM开启时用融合状态外推(CV/CA状态同为最后测量时刻基准, 可直接加权)
        x0 = self.fx.get_fx()
        y0 = self.fy.get_fx()
        vx0 = self.fx.get_fv()
        vy0 = self.fy.get_fv()
        ax0 = self.fx.get_fa()
        ay0 = self.fy.get_fa()
        x = x0 + vx0 * dt_el + 0.5 * ax0 * dt_el * dt_el
        y = y0 + vy0 * dt_el + 0.5 * ay0 * dt_el * dt_el
        vx = vx0 + ax0 * dt_el
        vy = vy0 + ay0 * dt_el
        return x, y, vx, vy, ax0, ay0
