# -*- coding: utf-8 -*-
# AimAssist 核心控制律逐条移植自 g:\A_Develop\DD\aim\aim_assist.cpp (v229a)
# 覆盖: 锁定保持/重捕/切换 + computeMove(v226权威2DOF) + getMoveDelta(v229连续输出插值)
# 省略: AutoTrigger/quickscope/诊断遥测/scene_analyzer(仿真默认关)/v226后已无效的gainScale
import math
import random
from aim_sim.kalman import clamp
from aim_sim.tracker import TargetTracker


class SimConfig:
    # 参数默认值取自 g:\A_Develop\DD\config\config.h D5RenderConfig
    def __init__(self):
        self.aim_position = 0.0          # 纵向瞄准点比例(0=框顶边=爆头线)
        self.aim_position_x = 0.5        # 横向瞄准点比例(0.5=框中心)
        self.aim_fov = 0.5
        self.speed_multiplier = 1.0
        self.track_match_threshold = 150.0
        self.track_max_lost = 75
        self.track_switch_hysteresis = 0.45
        self.kalman_enabled = True
        self.kalman_process_noise = 8.0e7    # CA jerk方差密度(px²/s⁵, 动态基准)
        self.kalman_measurement_noise = 4.0  # 测量方差(px², σ=2px)
        self.pid_kp = 0.30
        self.pid_deadzone = 6.0
        self.move_max_step = 14.0
        self.move_randomize = False      # 拟人层(跟随性能测试默认关)
        self.scene_adapt_enabled = False


class AimAssist:
    def __init__(self):
        self.rng = random.Random(12345)
        self.has_target = False
        self.aim_active = False
        self.locked_track_id = -1
        self.locked_x = 0.0
        self.locked_y = 0.0
        self.lost_frames = 0
        self.lock_confidence = 0.0
        self.locked_class_id = -1
        self.locked_w = 0.0
        self.locked_h = 0.0
        self.move_x = 0.0
        self.move_y = 0.0
        self.prev_move_x = 0.0
        self.prev_move_y = 0.0
        self.prev_error_x = 0.0
        self.prev_error_y = 0.0
        self.current_gain = 0.20
        self.humanize_enabled = False
        self.aim_fov = 0.5
        self.lock_frames = 0
        self.smooth_h = 0.0
        self.smooth_w = 0.0
        self.tracker = TargetTracker()
        self.shooting = False
        self.pinned = False
        self.locked_target_speed = 0.0
        self.locked_target_speed_x = 0.0
        self.locked_target_speed_y = 0.0
        self.last_locked_raw_x = 0.0
        self.last_locked_raw_y = 0.0
        self.sub_pixel_x = 0.0
        self.sub_pixel_y = 0.0
        self.last_detect_dt = 0.012
        self.ff_vel_x = 0.0
        self.ff_vel_y = 0.0
        self.ls_span_smooth = 0.22       # LS二次拟合平滑段窗口(s, innov小时用)
        self.ls_span_fast = 0.10         # LS二次拟合机动段窗口(s, innov大时用)
        self.ls_weight = 0.0             # LS速度/加速度混合权重(0=纯kalman)
        self.sent_log = []               # (t,dx,dy) 发射时间戳记账(精确补偿+发射率窗口)
        self.now_t = 0.0
        self.ctrl_rate_x = 0.0
        self.ctrl_rate_y = 0.0
        self.fresh_compute = False
        self.interp_budget_x = 0.0
        self.interp_budget_y = 0.0
        self.switch_cooldown = 0
        self.regrab_old_id = -1
        self.regrab_grace = 0
        self.arc_curv = 0.04
        self.arc_lateral = 0.0
        self.wobble_phase = 0.0
        self.prev_human_lock_frames = 0
        self.latency_ms = 60.0           # 仿真真实画面延迟(捕获+推理)
        self.diag_prev_move_x = 0.0
        self.diag_prev_move_y = 0.0
        self.diag_infer_skip_count = 0
        # ── 可调优化参数(仿真调优用) ──
        self.delay_s = 0.060             # 画面延迟(Smith补偿用): 捕获+推理+传输
        self.aim_vel_alpha = 0.6         # 自身准星速度EMA(实测发射率)
        self.aim_vel_x = 0.0
        self.aim_vel_y = 0.0
        self.fb_far = 0.20               # 分段反馈增益K(|e|>far_px)
        self.fb_mid = 0.20               # 中区K
        self.fb_near = 0.22              # 近区K(防贴附抖动)
        self.fb_far_px = 40.0
        self.fb_mid_px = 20.0
        self.fb_cross_ratio = 0.85       # 反馈防穿越比例
        self.os_damp_fb = 0.30           # 误差反号时K乘数
        self.tvel_max = 1500.0           # 目标速度估计限幅(px/s)
        self.safe_net_ratio = 0.90       # getMoveDelta安全网比例(对e_now)
        self.cross_clamp_ratio = 0.98    # 终极防穿越比例(对e_now)
        self.max_step_far = 42.0         # 远区单周期最大步长
        self.max_step_far_px = 80.0      # 远区判据
        self.e_now_x = 0.0               # Smith补偿后的当前真实误差估计
        self.e_now_y = 0.0
        self.last_ff_x = 0.0             # 上帧速度前馈量(安全网ff豁免用)
        self.last_ff_y = 0.0
        self.prev_eff_x = 0.0
        self.prev_eff_y = 0.0
        self.prev_eff_set = False
        self.prev_meas_x = 0.0           # 上拍纯测量误差(折返检测用,不受tvel污染)
        self.prev_meas_y = 0.0
        self.prev_meas_set = False
        self.cross_abs_x = 0.0           # 准星累计绝对位移(世界系重建记账)
        self.cross_abs_y = 0.0
        self.cap_cx = 0.0                # 捕获画面中心(世界系→屏幕误差换算用)
        self.cap_cy = 0.0
        self.q_min = 4.0e6               # 自适应Q下限(静止/匀速收紧降噪)
        self.q_dyn = 8.0e7               # 自适应Q上限(高动态)
        self.dyn_innov_px = 4.0          # 创新量软门限起点(px)
        self.dyn_acc_px = 300.0          # 加速度软门限起点(px/s²)
        self.dyn_innov_div = 16.0        # 创新量软门限斜率分母
        self.dyn_amag_div = 2000.0       # 加速度软门限斜率分母(1e9≈关闭amag项)
        self.dyn_state = 0.0             # 动态门限状态(尖峰瞬时开, 缓关防bootstrap死锁)
        self.dyn_hold_s = 0.25           # 动态保持衰减时间常数(s)
        self.ema_k = 0.70                # ff_vel平滑EMA基准(动态时自适应升向1.0)
        self.fb_boost = 0.06             # 反馈增益动态加成(K += fb_boost*dyn)
        self.decel_ax_px = 2000.0        # 强减速判据(|ax|, ax与tvel反号时生效)
        self.ff_decel_ratio = 0.6        # 强减速ff收缩比例(1.0=关闭)
        self.innov_k = 0.0               # 创新趋势v修正增益(0=关)
        self.innov_win_s = 0.15          # 创新趋势窗口(s)
        self.innov_hist_x = []           # (state_t, innov) 去重历史
        self.innov_hist_y = []
        self.innov_brake_px = 0.0        # 创新尖峰刹ff门限(px, 0=关)
        self.innov_brake_div = 20.0      # 刹ff斜率分母
        self.innov_brake_cut = 0.7       # 满刹车比例(ff乘(1-cut·b))
        self.mix_innov_px = 0.0          # 机动段差分v混合innov门限(px, 0=关)
        self.mix_innov_div = 15.0        # 差分v混合斜率分母
        self.imm_cv_q = 2.0e4            # IMM副模型CV加速度方差密度(px²/s³, 0=关)
        self.imm_cv_dyn = 0.0            # IMM动态q_cv系数(机动段q_cv×(1+dyn·k))
        self.imm_lam = 0.3               # IMM似然EWMA新证据权重
        self.imm_lam_dyn = 0.1           # IMM动态lam增量(机动段lam+dyn·k)
        self.snap_innov_px = 0.0         # 差分v快照重置innov门限(px, 0=关)
        self.snap_dyn_min = 0.6          # 快照重置dyn确认门限
        self.ff_a_gain = 1.0             # tvel的a前瞻增益(1.0=外推v0+a·dt, 0=纯滤波v)
        self.pred_lag_comp = 0.0         # fv滤波滞后补偿(s): 外推dt_el+comp, 补CA对1Hz正弦的32ms速度滞后
        self.ls_pos_w = 0.0              # LS位置外推混合权重(0=纯kalman外推, 独立于ls_weight)
        self.v_lead_s = 0.0              # tvel速度相位超前(s): fv滤波滞后补偿, 只修ff不动位置外推

    def reset(self):
        self.has_target = False
        self.aim_active = False
        self.locked_track_id = -1
        self.pinned = False
        self.locked_class_id = -1
        self.locked_x = 0.0
        self.locked_y = 0.0
        self.lost_frames = 0
        self.lock_confidence = 0.0
        self.lock_frames = 0
        self.locked_w = 0.0
        self.locked_h = 0.0
        self.move_x = 0.0
        self.move_y = 0.0
        self.prev_move_x = 0.0
        self.prev_move_y = 0.0
        self.prev_error_x = 0.0
        self.prev_error_y = 0.0
        self.smooth_h = 0.0
        self.smooth_w = 0.0
        self.ff_vel_x = 0.0
        self.ff_vel_y = 0.0
        self.aim_vel_x = 0.0
        self.aim_vel_y = 0.0
        self.e_now_x = 0.0
        self.e_now_y = 0.0
        self.last_ff_x = 0.0
        self.last_ff_y = 0.0
        self.prev_eff_set = False
        self.prev_meas_set = False
        self.cross_abs_x = 0.0
        self.cross_abs_y = 0.0
        self.cap_cx = 0.0
        self.cap_cy = 0.0
        self.dyn_state = 0.0
        self.pinned = False
        self.sent_log = []
        self.innov_hist_x = []
        self.innov_hist_y = []
        self.now_t = 0.0
        self.ctrl_rate_x = 0.0
        self.ctrl_rate_y = 0.0
        self.interp_budget_x = 0.0
        self.interp_budget_y = 0.0
        self.fresh_compute = False
        self.current_gain = 0.20
        self.sub_pixel_x = 0.0
        self.sub_pixel_y = 0.0
        self.locked_target_speed = 0.0
        self.locked_target_speed_x = 0.0
        self.locked_target_speed_y = 0.0
        self.last_locked_raw_x = 0.0
        self.last_locked_raw_y = 0.0
        self.switch_cooldown = 0
        self.tracker.reset()
        self.diag_infer_skip_count = 0

    def note_sent(self, dx, dy, t_now=0.0):
        self.now_t = t_now
        self.sent_log.append((t_now, dx, dy))
        self.cross_abs_x += dx
        self.cross_abs_y += dy
        cutoff = t_now - 0.6
        while self.sent_log and self.sent_log[0][0] < cutoff:
            self.sent_log.pop(0)

    def compute_track_score(self, track, capture_cx, capture_cy, capture_w, config):
        dx = track.raw_cx - capture_cx
        dy = track.raw_cy - capture_cy
        dist = math.sqrt(dx * dx + dy * dy)
        fov_radius = capture_w * self.aim_fov / 2.0
        is_locked = (track.track_id == self.locked_track_id and self.locked_track_id >= 0)
        if dist > fov_radius and not is_locked:
            return 1e9
        score = dist
        if is_locked:
            lock_strength = 0.5 + config.track_switch_hysteresis * 0.4
            score *= (1.0 - lock_strength)
        elif self.locked_track_id >= 0:
            score *= 1.5
        return score

    def select_best_track(self, capture_cx, capture_cy, capture_w, config):
        tracks = self.tracker.tracks
        if not tracks:
            return -1
        best_score = 1e9
        best_id = -1
        for track in tracks:
            if track.lost_frames > 0:
                continue
            score = self.compute_track_score(track, capture_cx, capture_cy, capture_w, config)
            if score < best_score:
                best_score = score
                best_id = track.track_id
        return best_id

    def _apply_humanize(self, move_x, move_y, md):
        # P0拟人化输出层(默认关, 完整移植供对比测试)
        err_mag = math.sqrt(self.locked_x * self.locked_x + self.locked_y * self.locked_y)
        if md > 0.3 and err_mag >= 20.0:
            if self.lock_frames < self.prev_human_lock_frames:
                self.arc_curv = self.rng.uniform(0.02, 0.08) * (1.0 if self.rng.getrandbits(1) else -1.0)
                self.arc_lateral = 0.0
                self.wobble_phase = (self.rng.randrange(628)) * 0.01
            self.prev_human_lock_frames = self.lock_frames
            mag_scale = 1.0 + self.rng.gauss(0.0, 0.12)
            mag_scale = clamp(mag_scale, 0.75, 1.30)
            move_x *= mag_scale
            move_y *= mag_scale
            inv_md = 1.0 / md
            perp_x = -move_y * inv_md
            perp_y = move_x * inv_md
            arc_fade = (md / 20.0) if md < 20.0 else 1.0
            lat_step = self.arc_curv * md * arc_fade
            lat_step = clamp(lat_step, -2.0, 2.0)
            self.arc_lateral += lat_step
            if self.arc_lateral > 5.0:
                lat_step -= (self.arc_lateral - 5.0)
                self.arc_lateral = 5.0
            if self.arc_lateral < -5.0:
                lat_step -= (self.arc_lateral + 5.0)
                self.arc_lateral = -5.0
            move_x += perp_x * lat_step
            move_y += perp_y * lat_step
            self.wobble_phase += 0.7 + (self.rng.randrange(100)) * 0.006
            wob = math.sin(self.wobble_phase) * 0.3
            move_x += perp_x * wob
            move_y += perp_y * wob
            noise_scale = (md / 3.0) if md < 3.0 else 1.0
            nx = clamp(self.rng.gauss(0.0, 0.5 * noise_scale), -0.9, 0.9)
            ny = clamp(self.rng.gauss(0.0, 0.5 * noise_scale), -0.9, 0.9)
            move_x += nx
            move_y += ny
        return move_x, move_y

    def get_move_delta(self, dt=0.004, out_dt=0.004):
        delta_x = 0.0
        delta_y = 0.0
        if not self.has_target:
            self.sub_pixel_x = 0.0
            self.sub_pixel_y = 0.0
            self.diag_prev_move_x = 0.0
            self.diag_prev_move_y = 0.0
            self.ctrl_rate_x = 0.0
            self.ctrl_rate_y = 0.0
            self.interp_budget_x = 0.0
            self.interp_budget_y = 0.0
            self.fresh_compute = False
            return 0.0, 0.0
        fresh_step = self.fresh_compute
        self.fresh_compute = False
        raw_move_x = 0.0
        raw_move_y = 0.0
        if fresh_step:
            move_x = self.move_x
            move_y = self.move_y
            # 安全网: 移动量 ≤ 90%×当前真实误差(Smith补偿后) + 速度前馈豁免
            lim_x = abs(self.e_now_x) * self.safe_net_ratio + abs(self.last_ff_x)
            lim_y = abs(self.e_now_y) * self.safe_net_ratio + abs(self.last_ff_y)
            if abs(move_x) > lim_x:
                move_x = math.copysign(lim_x, move_x) if move_x != 0.0 else 0.0
            if abs(move_y) > lim_y:
                move_y = math.copysign(lim_y, move_y) if move_y != 0.0 else 0.0
            raw_move_x = move_x
            raw_move_y = move_y
            if self.aim_active and self.humanize_enabled:
                md = math.sqrt(move_x * move_x + move_y * move_y)
                if md > 0.0:
                    move_x, move_y = self._apply_humanize(move_x, move_y, md)
            # 终极防穿越98% + 速度前馈豁免
            lim_x = abs(self.e_now_x) * self.cross_clamp_ratio + abs(self.last_ff_x)
            lim_y = abs(self.e_now_y) * self.cross_clamp_ratio + abs(self.last_ff_y)
            if abs(move_x) > lim_x:
                move_x = math.copysign(lim_x, move_x) if move_x != 0.0 else 0.0
            if abs(move_y) > lim_y:
                move_y = math.copysign(lim_y, move_y) if move_y != 0.0 else 0.0
            # 折算速率+登记预算
            dts_i = max(self.last_detect_dt, 0.001)
            self.ctrl_rate_x = move_x / dts_i
            self.ctrl_rate_y = move_y / dts_i
            self.interp_budget_x = abs(move_x)
            self.interp_budget_y = abs(move_y)
        # 所有帧统一发射: 速率×outDt, 预算钳制
        emit_x = self.ctrl_rate_x * out_dt
        emit_y = self.ctrl_rate_y * out_dt
        if abs(emit_x) > self.interp_budget_x:
            emit_x = math.copysign(self.interp_budget_x, emit_x) if emit_x != 0.0 else 0.0
        if abs(emit_y) > self.interp_budget_y:
            emit_y = math.copysign(self.interp_budget_y, emit_y) if emit_y != 0.0 else 0.0
        self.interp_budget_x = max(0.0, self.interp_budget_x - abs(emit_x))
        self.interp_budget_y = max(0.0, self.interp_budget_y - abs(emit_y))
        self.sub_pixel_x += emit_x
        self.sub_pixel_y += emit_y
        delta_x = float(int(self.sub_pixel_x))
        delta_y = float(int(self.sub_pixel_y))
        self.sub_pixel_x -= delta_x
        self.sub_pixel_y -= delta_y
        self.diag_prev_move_x = raw_move_x
        self.diag_prev_move_y = raw_move_y
        return delta_x, delta_y

    def _ls_fit(self, hist, span):
        # 局部二次最小二乘: x(t)=c0+c1·τ+c2·τ²(τ中心化), 返回窗口末端(位置,速度,加速度,t_end)
        # 平滑段速度σ≈13px/s(kalman CA对正弦振铃±250); innov大时外部收窗保机动响应
        t_end = hist[-1][0]
        pts = [p for p in hist if p[0] >= t_end - span]
        n = len(pts)
        if n < 4:
            return None
        tc = 0.0
        for p in pts:
            tc += p[0]
        tc /= n
        s0 = s1 = s2 = s3 = s4 = 0.0
        tx0 = tx1 = tx2 = ty0 = ty1 = ty2 = 0.0
        for t, x, y in pts:
            tau = t - tc
            tau2 = tau * tau
            s0 += 1.0
            s1 += tau
            s2 += tau2
            s3 += tau2 * tau
            s4 += tau2 * tau2
            tx0 += x
            tx1 += tau * x
            tx2 += tau2 * x
            ty0 += y
            ty1 += tau * y
            ty2 += tau2 * y
        det_m = s0 * (s2 * s4 - s3 * s3) - s1 * (s1 * s4 - s2 * s3) + s2 * (s1 * s3 - s2 * s2)
        if abs(det_m) < 1e-9:
            return None
        c0x = (tx0 * (s2 * s4 - s3 * s3) - s1 * (tx1 * s4 - tx2 * s3) + s2 * (tx1 * s3 - tx2 * s2)) / det_m
        c1x = (s0 * (tx1 * s4 - tx2 * s3) - tx0 * (s1 * s4 - s2 * s3) + s2 * (s1 * tx2 - s2 * tx1)) / det_m
        c2x = (s0 * (s2 * tx2 - s3 * tx1) - s1 * (s1 * tx2 - s2 * tx1) + tx0 * (s1 * s3 - s2 * s2)) / det_m
        c0y = (ty0 * (s2 * s4 - s3 * s3) - s1 * (ty1 * s4 - ty2 * s3) + s2 * (ty1 * s3 - ty2 * s2)) / det_m
        c1y = (s0 * (ty1 * s4 - ty2 * s3) - ty0 * (s1 * s4 - s2 * s3) + s2 * (s1 * ty2 - s2 * ty1)) / det_m
        c2y = (s0 * (s2 * ty2 - s3 * ty1) - s1 * (s1 * ty2 - s2 * ty1) + ty0 * (s1 * s3 - s2 * s2)) / det_m
        te = t_end - tc
        x_e = c0x + c1x * te + c2x * te * te
        vx_e = c1x + 2.0 * c2x * te
        ax_e = 2.0 * c2x
        y_e = c0y + c1y * te + c2y * te * te
        vy_e = c1y + 2.0 * c2y * te
        ay_e = 2.0 * c2y
        return (x_e, vx_e, ax_e, y_e, vy_e, ay_e, t_end)

    def compute_move(self, error_x, error_y, config):
        dist = math.sqrt(error_x * error_x + error_y * error_y)
        deadzone = max(config.pid_deadzone, 2.0)
        dts = clamp(self.last_detect_dt, 0.008, 0.05)
        # 1) CA恒加速Kalman世界状态外推到当前时刻(状态时间=最近测量拍摄时刻)
        #    tvel=外推世界速度, e_now=外推世界位置+瞄准点偏移-当前准星累计位置-画面中心
        tvel_x = 0.0
        tvel_y = 0.0
        acc_x = 0.0
        acc_y = 0.0
        e_now_x = error_x
        e_now_y = error_y
        dyn = 0.0
        innov_bx = 0.0
        innov_by = 0.0
        locked = self.tracker.find_track_data_by_id(self.locked_track_id)
        if locked is not None and locked.kalman.is_initialized():
            # 动态自适应Q: 软门限+保持衰减(创新尖峰瞬时开q, 0.25s缓关, 消除bootstrap死锁)
            innov = locked.kalman.get_innov_mag()
            innov_bx = abs(locked.kalman.fx.last_innov)
            innov_by = abs(locked.kalman.fy.last_innov)
            amag = locked.kalman.get_a_mag()
            inst = clamp(max((innov - self.dyn_innov_px) / self.dyn_innov_div,
                             (amag - self.dyn_acc_px) / self.dyn_amag_div), 0.0, 1.0)
            self.dyn_state *= math.exp(-dts / self.dyn_hold_s)
            self.dyn_state = max(self.dyn_state, inst)
            dyn = self.dyn_state
            locked.kalman.set_process_noise(self.q_min + (self.q_dyn - self.q_min) * dyn)
            locked.kalman.set_cv_q(self.imm_cv_q * (1.0 + self.imm_cv_dyn * dyn))
            locked.kalman.set_cv_lam(min(0.9, self.imm_lam + self.imm_lam_dyn * dyn))
            dt_el = clamp(self.now_t - locked.state_t, 0.0, 0.25) + self.pred_lag_comp
            x_pred, y_pred, vx_now, vy_now, ax_now, ay_now = locked.kalman.predict_ext(dt_el)
            acc_x = clamp(ax_now, -8000.0, 8000.0)
            acc_y = clamp(ay_now, -8000.0, 8000.0)
            if self.ff_a_gain < 1.0:
                vx_now = locked.kalman.get_vx() + ax_now * dt_el * self.ff_a_gain
                vy_now = locked.kalman.get_vy() + ay_now * dt_el * self.ff_a_gain
            if self.v_lead_s > 0.0:
                vx_now += ax_now * self.v_lead_s
                vy_now += ay_now * self.v_lead_s
            tvel_x = clamp(vx_now, -self.tvel_max, self.tvel_max)
            tvel_y = clamp(vy_now, -self.tvel_max, self.tvel_max)
            # LS二次拟合(v2): hist时间戳回归替代kalman速度(消CA对正弦的振铃)
            # innov大→收窗保机动响应; 点不足回退kalman
            ls_span = clamp(self.ls_span_smooth - (innov - self.dyn_innov_px) * 0.02,
                            self.ls_span_fast, self.ls_span_smooth)
            ls = self._ls_fit(locked.hist, ls_span)
            if ls is not None and self.ls_weight > 0.0:
                x_e, vx_e, ax_e, y_e, vy_e, ay_e, t_end = ls
                w_ls = self.ls_weight
                v_bx = (1.0 - w_ls) * tvel_x + w_ls * vx_e
                v_by = (1.0 - w_ls) * tvel_y + w_ls * vy_e
                a_bx = (1.0 - w_ls) * acc_x + w_ls * ax_e
                a_by = (1.0 - w_ls) * acc_y + w_ls * ay_e
                x_pred = locked.kalman.get_x() + v_bx * dt_el + 0.5 * a_bx * dt_el * dt_el
                y_pred = locked.kalman.get_y() + v_by * dt_el + 0.5 * a_by * dt_el * dt_el
                acc_x = clamp(a_bx, -8000.0, 8000.0)
                acc_y = clamp(a_by, -8000.0, 8000.0)
                tvel_x = clamp(v_bx, -self.tvel_max, self.tvel_max)
                tvel_y = clamp(v_by, -self.tvel_max, self.tvel_max)
            if ls is not None and self.ls_pos_w > 0.0:
                x_e, vx_e, ax_e, y_e, vy_e, ay_e, t_end = ls
                w_p = self.ls_pos_w
                x_ls = x_e + vx_e * dt_el + 0.5 * ax_e * dt_el * dt_el
                y_ls = y_e + vy_e * dt_el + 0.5 * ay_e * dt_el * dt_el
                x_pred = (1.0 - w_p) * x_pred + w_p * x_ls
                y_pred = (1.0 - w_p) * y_pred + w_p * y_ls
            off_x = self.smooth_w * config.aim_position_x - self.smooth_w * 0.5
            off_y = self.smooth_h * self._aim_pos_y_for_class(self.locked_class_id, config) - self.smooth_h * 0.5
            e_now_x = x_pred + off_x - self.cap_cx - self.cross_abs_x
            e_now_y = y_pred + off_y - self.cap_cy - self.cross_abs_y
            # 创新趋势v修正: 折返时innovation单向累积(目标比预测慢/已反向), kalman k1增益校正太慢
            # trend = 窗口innov和/时间跨度 = 平均速度亏缺(px/s), 直接补偿外推速度与e_now
            if self.innov_k > 0.0:
                st_t = locked.state_t
                if locked.kalman.fx.time_since_update < 1e-6 and \
                        (not self.innov_hist_x or self.innov_hist_x[-1][0] != st_t):
                    self.innov_hist_x.append((st_t, locked.kalman.fx.last_innov))
                    self.innov_hist_y.append((st_t, locked.kalman.fy.last_innov))
                cut = st_t - self.innov_win_s
                while self.innov_hist_x and self.innov_hist_x[0][0] < cut:
                    self.innov_hist_x.pop(0)
                    self.innov_hist_y.pop(0)
                if len(self.innov_hist_x) >= 2:
                    dt_i = self.innov_hist_x[-1][0] - self.innov_hist_x[0][0]
                    if dt_i > 0.015:
                        tr_x = sum(h[1] for h in self.innov_hist_x) / dt_i * self.innov_k
                        tr_y = sum(h[1] for h in self.innov_hist_y) / dt_i * self.innov_k
                        vx_now = clamp(vx_now + tr_x, -self.tvel_max, self.tvel_max)
                        vy_now = clamp(vy_now + tr_y, -self.tvel_max, self.tvel_max)
                        tvel_x = vx_now
                        tvel_y = vy_now
                        x_pred = locked.kalman.get_x() + vx_now * dt_el + 0.5 * acc_x * dt_el * dt_el
                        y_pred = locked.kalman.get_y() + vy_now * dt_el + 0.5 * acc_y * dt_el * dt_el
                        e_now_x = x_pred + off_x - self.cap_cx - self.cross_abs_x
                        e_now_y = y_pred + off_y - self.cap_cy - self.cross_abs_y
            # 机动段差分v混合: innov大=CA模型失配(瞬间反向/急停), kalman k1校正需2-3帧;
            # hist末两点差分速度零滞后(σ≈86px/s), 按innov门控混入, 平滑段w≈0不受噪声影响
            if self.mix_innov_px > 0.0 and len(locked.hist) >= 2:
                (h1t, h1x, h1y), (h2t, h2x, h2y) = locked.hist[-2], locked.hist[-1]
                dt_h = h2t - h1t
                if dt_h > 0.005:
                    w_m = clamp((innov - self.mix_innov_px) / self.mix_innov_div, 0.0, 1.0)
                    if w_m > 0.0:
                        vdx = (h2x - h1x) / dt_h
                        vdy = (h2y - h1y) / dt_h
                        vx_now = (1.0 - w_m) * vx_now + w_m * vdx
                        vy_now = (1.0 - w_m) * vy_now + w_m * vdy
                        tvel_x = clamp(vx_now, -self.tvel_max, self.tvel_max)
                        tvel_y = clamp(vy_now, -self.tvel_max, self.tvel_max)
                        x_pred = locked.kalman.get_x() + vx_now * dt_el + 0.5 * acc_x * dt_el * dt_el
                        y_pred = locked.kalman.get_y() + vy_now * dt_el + 0.5 * acc_y * dt_el * dt_el
                        e_now_x = x_pred + off_x - self.cap_cx - self.cross_abs_x
                        e_now_y = y_pred + off_y - self.cap_cy - self.cross_abs_y
            # 差分v快照重置: innov>门限且机动确认(dyn高)时, 差分v硬替换输出速度;
            # 离散决策(非连续混合), 折返瞬间差分v立刻反号, 消60ms ff错方向推进
            if self.snap_innov_px > 0.0 and dyn >= self.snap_dyn_min and len(locked.hist) >= 2:
                (h1t, h1x, h1y), (h2t, h2x, h2y) = locked.hist[-2], locked.hist[-1]
                dt_h = h2t - h1t
                if dt_h > 0.005 and innov > self.snap_innov_px:
                    vx_now = clamp((h2x - h1x) / dt_h, -self.tvel_max, self.tvel_max)
                    vy_now = clamp((h2y - h1y) / dt_h, -self.tvel_max, self.tvel_max)
                    tvel_x = vx_now
                    tvel_y = vy_now
                    x_pred = locked.kalman.get_x() + vx_now * dt_el + 0.5 * acc_x * dt_el * dt_el
                    y_pred = locked.kalman.get_y() + vy_now * dt_el + 0.5 * acc_y * dt_el * dt_el
                    e_now_x = x_pred + off_x - self.cap_cx - self.cross_abs_x
                    e_now_y = y_pred + off_y - self.cap_cy - self.cross_abs_y
        tvel_raw_x = tvel_x
        tvel_raw_y = tvel_y
        # 误差发散应急: |e_meas|单拍增长>6px(噪声差最多±4px) → tvel EMA加速收敛
        # (折返/急转时持续生效直到误差受控); 动态时ek_eff自适应逼近1.0消ff滞后
        ema_k = self.ema_k + (1.0 - self.ema_k) * dyn
        if self.prev_meas_set:
            if abs(error_x) - abs(self.prev_meas_x) > 6.0 or abs(error_y) - abs(self.prev_meas_y) > 6.0:
                ema_k = min(1.0, ema_k + 0.15)
        # tvel长平滑: 切断 aimVel→ff→输出→发射→aimVel 自激环(ff承担稳态, fb承担动态)
        self.ff_vel_x += (tvel_x - self.ff_vel_x) * ema_k
        self.ff_vel_y += (tvel_y - self.ff_vel_y) * ema_k
        tvel_x = self.ff_vel_x
        tvel_y = self.ff_vel_y
        tvel_mag = math.sqrt(tvel_x * tvel_x + tvel_y * tvel_y)
        # 2) v225滞回锁死: 仅慢速目标(tvel<120)贴附后零输出防抖
        #    匀速/快速目标不受死区影响(否则前馈被掐→误差累积→出死区猛冲→极限环)
        slow_target = tvel_mag < 120.0
        if self.pinned and slow_target:
            if dist < deadzone * 1.8:
                self.move_x = 0.0
                self.move_y = 0.0
                self.current_gain = 0.0
                self.prev_move_x = 0.0
                self.prev_move_y = 0.0
                self.prev_error_x = error_x
                self.prev_error_y = error_y
                self.fresh_compute = True
                return
            self.pinned = False
        if dist < deadzone and slow_target:
            self.pinned = True
            self.move_x = 0.0
            self.move_y = 0.0
            self.current_gain = 0.0
            self.prev_move_x = 0.0
            self.prev_move_y = 0.0
            self.prev_error_x = error_x
            self.prev_error_y = error_y
            self.fresh_compute = True
            return
        # 3) e_now已由CA世界状态外推得到(当前真实误差), 旧Smith手写补偿不再需要
        self.e_now_x = e_now_x
        self.e_now_y = e_now_y
        # 4) 速度前馈(世界速度→稳态零滞后) + 小幅加速度补偿(仅本拍速度增量) + 分段比例反馈
        ff_x = (tvel_x + acc_x * dts * 0.5) * dts
        ff_y = (tvel_y + acc_y * dts * 0.5) * dts
        # 强减速刹ff: ax与tvel反号且|ax|大(目标急减速/折返), kalman ax滞后+60ms延迟
        # 使ff按旧速度继续推→过冲30-60px; 按比例收缩ff, 交由fb(基于e_now)纠偏
        if tvel_raw_x * acc_x < 0.0 and abs(acc_x) > self.decel_ax_px:
            ff_x *= self.ff_decel_ratio
        if tvel_raw_y * acc_y < 0.0 and abs(acc_y) > self.decel_ax_px:
            ff_y *= self.ff_decel_ratio
        # 创新尖峰刹ff: innov大=目标实际运动与预测严重不符(瞬间反向/急停), 滤波器重锁期间
        # ff按旧v推进是错误方向; 按innov幅度比例刹ff, innov回落后自动释放
        if self.innov_brake_px > 0.0:
            bx = clamp((innov_bx - self.innov_brake_px) / self.innov_brake_div, 0.0, 1.0)
            by = clamp((innov_by - self.innov_brake_px) / self.innov_brake_div, 0.0, 1.0)
            if bx > 0.0:
                ff_x *= 1.0 - self.innov_brake_cut * bx
            if by > 0.0:
                ff_y *= 1.0 - self.innov_brake_cut * by
        e_mag = math.sqrt(e_now_x * e_now_x + e_now_y * e_now_y)
        # 误差快速收缩且已接近目标(e_mag<25): tvel含恢复瞬态过冲, ff减速防冲过
        if e_mag < 25.0 and self.prev_meas_set:
            if abs(error_x) < abs(self.prev_meas_x) - 6.0 or abs(error_y) < abs(self.prev_meas_y) - 6.0:
                ff_x *= 0.7
                ff_y *= 0.7
        self.last_ff_x = ff_x
        self.last_ff_y = ff_y
        if e_mag > self.fb_far_px:
            K = self.fb_far
        elif e_mag > self.fb_mid_px:
            K = self.fb_mid
        else:
            K = self.fb_near
        K += self.fb_boost * dyn
        fb_x = K * e_now_x
        fb_y = K * e_now_y
        # 跨零应急(per-axis): e_meas该轴真跨零(符号相反且单拍跨度>10px, 噪声差最多±4px)
        # 分流: tvel与e_now反号=ff在推错误方向(折返)→置零+fb衰减
        #       tvel与e_now同号=准星冲过但目标仍远离(正弦冲过)→ff快收敛跟随
        if self.prev_meas_set:
            if error_x * self.prev_meas_x < 0.0 and abs(error_x - self.prev_meas_x) > 10.0:
                if tvel_raw_x * e_now_x < 0.0:
                    fb_x *= self.os_damp_fb
                    self.ff_vel_x = 0.0
                    ff_x = 0.0
                else:
                    self.ff_vel_x += (tvel_raw_x - self.ff_vel_x) * 0.7
            if error_y * self.prev_meas_y < 0.0 and abs(error_y - self.prev_meas_y) > 10.0:
                if tvel_raw_y * e_now_y < 0.0:
                    fb_y *= self.os_damp_fb
                    self.ff_vel_y = 0.0
                    ff_y = 0.0
                else:
                    self.ff_vel_y += (tvel_raw_y - self.ff_vel_y) * 0.7
        # 反馈防穿越: fb分量不超过当前真实误差的85%
        cap_x = abs(e_now_x) * self.fb_cross_ratio
        cap_y = abs(e_now_y) * self.fb_cross_ratio
        if abs(fb_x) > cap_x:
            fb_x = math.copysign(cap_x, fb_x) if fb_x != 0.0 else 0.0
        if abs(fb_y) > cap_y:
            fb_y = math.copysign(cap_y, fb_y) if fb_y != 0.0 else 0.0
        move_x = ff_x + fb_x
        move_y = ff_y + fb_y
        self.current_gain = K
        # 5) 总步长限幅: 连续函数防边界跳变振荡 = 前馈需求 + 误差比例项, 钳[10,60]
        step_cap = clamp(abs(ff_x) + abs(ff_y) + 0.35 * e_mag + 6.0, 10.0, self.max_step_far)
        mv_mag = math.sqrt(move_x * move_x + move_y * move_y)
        if mv_mag > step_cap:
            s = step_cap / mv_mag
            move_x *= s
            move_y *= s
        self.prev_eff_x = e_now_x
        self.prev_eff_y = e_now_y
        self.prev_eff_set = True
        self.prev_meas_x = error_x
        self.prev_meas_y = error_y
        self.prev_meas_set = True
        self.prev_move_x = move_x
        self.prev_move_y = move_y
        self.move_x = move_x
        self.move_y = move_y
        self.fresh_compute = True

    def _aim_pos_y_for_class(self, class_id, config):
        return config.aim_position

    def _do_switch(self, best_track_id, config):
        new_target = self.tracker.find_track_data_by_id(best_track_id)
        if new_target is None:
            return False
        self.locked_track_id = best_track_id
        self.locked_class_id = new_target.class_id
        self.locked_w = new_target.w
        self.locked_h = new_target.h
        self.lock_confidence = new_target.lock_confidence
        self.lock_frames = 1
        self.locked_target_speed = new_target.last_speed
        self.last_locked_raw_x = new_target.raw_cx
        self.last_locked_raw_y = new_target.raw_cy
        if new_target.kalman.is_initialized():
            self.locked_target_speed_x = new_target.kalman.get_vx()
            self.locked_target_speed_y = new_target.kalman.get_vy()
        else:
            self.locked_target_speed_x = 0.0
            self.locked_target_speed_y = 0.0
        self.smooth_h = new_target.h
        self.smooth_w = new_target.w
        self.move_x = 0.0
        self.move_y = 0.0
        self.prev_move_x = 0.0
        self.prev_move_y = 0.0
        self.prev_error_x = 0.0
        self.prev_error_y = 0.0
        self.pinned = False
        self.ff_vel_x = 0.0
        self.ff_vel_y = 0.0
        self.ctrl_rate_x = 0.0
        self.ctrl_rate_y = 0.0
        self.interp_budget_x = 0.0
        self.interp_budget_y = 0.0
        self.fresh_compute = False
        self.switch_cooldown = 6
        return True

    def update(self, detections, capture_cx, capture_cy, capture_w, capture_h, config, aim_active, has_new_detections, detect_dt, now_t=0.0):
        self.aim_active = aim_active
        self.aim_fov = config.aim_fov
        self.now_t = now_t
        self.cap_cx = capture_cx
        self.cap_cy = capture_cy
        if self.switch_cooldown > 0:
            self.switch_cooldown -= 1
        dt = detect_dt if detect_dt > 0.0001 else 0.001
        if dt > 0.1:
            dt = 0.1
        if not aim_active and self.locked_track_id >= 0:
            self.locked_track_id = -1
            self.pinned = False
            self.locked_class_id = -1
            self.lock_confidence = 0.0
            self.lock_frames = 0
            self.has_target = False
            self.move_x = 0.0
            self.move_y = 0.0
            self.prev_move_x = 0.0
            self.prev_move_y = 0.0
            self.prev_error_x = 0.0
            self.prev_error_y = 0.0
            self.smooth_init = False
            self.smooth_w = 0.0
            self.smooth_h = 0.0
            self.locked_target_speed = 0.0
            self.locked_target_speed_x = 0.0
            self.locked_target_speed_y = 0.0
            self.last_locked_raw_x = 0.0
            self.last_locked_raw_y = 0.0
            self.ff_vel_x = 0.0
            self.ff_vel_y = 0.0
            self.ctrl_rate_x = 0.0
            self.ctrl_rate_y = 0.0
            self.interp_budget_x = 0.0
            self.interp_budget_y = 0.0
            self.fresh_compute = False
            self.switch_cooldown = 0
        if has_new_detections:
            self.tracker.set_max_lost_frames(config.track_max_lost)
            self.tracker.set_kalman_enabled(config.kalman_enabled)
            if config.kalman_enabled:
                dynamic_ahead = clamp(self.latency_ms / 1000.0 * 1.2, 0.010, 0.200)
                self.tracker.set_kalman_params(config.kalman_process_noise, config.kalman_measurement_noise, dynamic_ahead)
            self.tracker.set_match_threshold(config.track_match_threshold)
            if dt < 0.001:
                dt = 0.001
            if dt > 0.050:
                dt = 0.050
            # 世界系重建: 检测误差 + 拍摄时刻准星累计位置 = 目标世界位置
            # 拍摄时刻准星位置 = 当前累计 - 拍摄以来已发出的位移(sent_log时间戳记账)
            if detections:
                cap_t0 = detections[0].get('cap_t', -1.0)
                ccx = self.cross_abs_x
                ccy = self.cross_abs_y
                for rec in self.sent_log:
                    if rec[0] > cap_t0:
                        ccx -= rec[1]
                        ccy -= rec[2]
                for det in detections:
                    det['cap_cross_x'] = ccx
                    det['cap_cross_y'] = ccy
                    det['world_cx'] = det['cx'] + ccx
                    det['world_cy'] = det['cy'] + ccy
            self.tracker.update(detections, dt, capture_w, capture_h)
            self.last_detect_dt = (self.last_detect_dt * 0.65 + dt * 0.35) if self.last_detect_dt > 0.0001 else dt
        if self.locked_track_id >= 0:
            locked = self.tracker.find_track_data_by_id(self.locked_track_id)
            found = locked is not None
            need_reacquire = (not found) or locked.lost_frames >= 2
            if need_reacquire and has_new_detections:
                pred_x = self.last_locked_raw_x
                pred_y = self.last_locked_raw_y
                best_dist = 1e18
                best_id = -1
                for t in self.tracker.tracks:
                    if t.lost_frames > 0:
                        continue
                    dx = t.raw_cx - pred_x
                    dy = t.raw_cy - pred_y
                    d = dx * dx + dy * dy
                    if d < best_dist:
                        best_dist = d
                        best_id = t.track_id
                pos_match_radius = self.smooth_w * 0.5 + (110.0 if self.shooting else 70.0)
                if best_id >= 0 and best_dist < pos_match_radius * pos_match_radius:
                    best_track = self.tracker.find_track_data_by_id(best_id)
                    if best_track is not None and best_track.class_id == self.locked_class_id:
                        locked = best_track
                        found = True
                        self.locked_track_id = best_id
            aim_lock_lost_timeout = 20 if self.shooting else 8
            switch_lost_threshold = 6 if self.shooting else 3
            if found and locked.lost_frames <= aim_lock_lost_timeout:
                self.lost_frames = 0
                self.lock_frames += 1
                self.locked_class_id = locked.class_id
                box_ratio = (locked.h / self.smooth_h) if self.smooth_h > 1.0 else 1.0
                box_burst = box_ratio > 2.5
                if not box_burst:
                    self.locked_w = locked.w
                    self.locked_h = locked.h
                self.lock_confidence = locked.lock_confidence
                if not box_burst:
                    self.locked_target_speed = locked.last_speed
                    self.last_locked_raw_x = locked.raw_cx
                    self.last_locked_raw_y = locked.raw_cy
                    if locked.kalman.is_initialized():
                        self.locked_target_speed_x = locked.kalman.get_vx()
                        self.locked_target_speed_y = locked.kalman.get_vy()
                    else:
                        self.locked_target_speed_x = 0.0
                        self.locked_target_speed_y = 0.0
                in_protection = self.lock_frames <= 5
                lost_enough = locked.lost_frames >= switch_lost_threshold
                cur_dist = 1e9
                if locked.lost_frames == 0:
                    cdx = locked.raw_cx - capture_cx
                    cdy = locked.raw_cy - capture_cy
                    cur_dist = math.sqrt(cdx * cdx + cdy * cdy)
                approaching = (locked.lost_frames == 0 and cur_dist > 60.0 and not self.shooting)
                if self.switch_cooldown <= 0 and has_new_detections and not in_protection and (lost_enough or approaching):
                    best_id = -1
                    if lost_enough:
                        rcx = self.last_locked_raw_x
                        rcy = self.last_locked_raw_y
                        wide_r = self.smooth_w * 0.5 + (130.0 if self.shooting else 90.0)
                        bd = wide_r * wide_r
                        regrab_id = -1
                        for t in self.tracker.tracks:
                            if t.lost_frames > 12 or t.track_id == self.locked_track_id:
                                continue
                            if t.class_id != self.locked_class_id:
                                continue
                            dx = t.raw_cx - rcx
                            dy = t.raw_cy - rcy
                            d = dx * dx + dy * dy
                            if d < bd:
                                bd = d
                                regrab_id = t.track_id
                        if regrab_id >= 0:
                            best_id = regrab_id
                        else:
                            best_id = self.select_best_track(capture_cx, capture_cy, capture_w, config)
                    else:
                        margin = max(28.0, cur_dist * 0.30)
                        best_d = cur_dist - margin
                        for t in self.tracker.tracks:
                            if t.lost_frames > 0 or t.track_id == self.locked_track_id:
                                continue
                            dx = t.raw_cx - capture_cx
                            dy = t.raw_cy - capture_cy
                            d = math.sqrt(dx * dx + dy * dy)
                            if d < best_d:
                                best_d = d
                                best_id = t.track_id
                    if best_id >= 0 and best_id != self.locked_track_id:
                        new_target = self.tracker.find_track_data_by_id(best_id)
                        if new_target is not None:
                            ndx = new_target.cx - capture_cx
                            ndy = new_target.cy - capture_cy
                            ndist = math.sqrt(ndx * ndx + ndy * ndy)
                            fov_radius = capture_w * self.aim_fov / 2.0
                            if ndist < fov_radius and self._do_switch(best_id, config):
                                locked = new_target
                                found = True
                if found and locked.lost_frames <= 2 and has_new_detections:
                    if locked.lost_frames == 0:
                        raw_x = locked.raw_cx
                        raw_y = locked.raw_cy
                    else:
                        raw_x = locked.cx
                        raw_y = locked.cy
                    if not box_burst:
                        if self.smooth_h <= 0.0:
                            self.smooth_h = locked.h
                            self.smooth_w = locked.w
                        else:
                            self.smooth_h = self.smooth_h * 0.85 + locked.h * 0.15
                            self.smooth_w = self.smooth_w * 0.85 + locked.w * 0.15
                    if box_burst:
                        target_x = self.locked_x
                        target_y = self.locked_y
                    else:
                        target_x = (raw_x - self.smooth_w / 2.0 + self.smooth_w * config.aim_position_x) - capture_cx
                        target_y = (raw_y - self.smooth_h / 2.0 + self.smooth_h * self._aim_pos_y_for_class(self.locked_class_id, config)) - capture_cy
                    self.locked_x = target_x
                    self.locked_y = target_y
                    if self.aim_active:
                        self.compute_move(self.locked_x, self.locked_y, config)
                        self.prev_move_x = self.move_x
                        self.prev_move_y = self.move_y
                    else:
                        self.move_x = 0.0
                        self.move_y = 0.0
                        self.prev_move_x = 0.0
                        self.prev_move_y = 0.0
                        self.prev_error_x = 0.0
                        self.prev_error_y = 0.0
                elif found and locked.lost_frames <= 2:
                    pass
                else:
                    self.move_x = 0.0
                    self.move_y = 0.0
                    self.prev_move_x = 0.0
                    self.prev_move_y = 0.0
                    self.ctrl_rate_x = 0.0
                    self.ctrl_rate_y = 0.0
                    self.interp_budget_x = 0.0
                    self.interp_budget_y = 0.0
                self.has_target = True
                return
            # 超时: 切换评估已做(lost_enough), 这里处理完全释放
            if found and locked.lost_frames > aim_lock_lost_timeout:
                pass
        if has_new_detections:
            if not detections and not self.tracker.tracks:
                self.lost_frames += 1
                self.has_target = False
                return
            self.lost_frames = 0
            best_id = self.select_best_track(capture_cx, capture_cy, capture_w, config)
            if best_id < 0:
                self.has_target = False
                return
            locked_track = self.tracker.find_track_data_by_id(best_id)
            if locked_track is None:
                self.has_target = False
                return
            use_x = locked_track.raw_cx
            use_y = locked_track.raw_cy
            self.locked_track_id = best_id
            self.locked_class_id = locked_track.class_id
            self.locked_w = locked_track.w
            self.locked_h = locked_track.h
            self.lock_confidence = locked_track.lock_confidence
            self.lock_frames = 1
            self.move_x = 0.0
            self.move_y = 0.0
            self.prev_move_x = 0.0
            self.prev_move_y = 0.0
            self.prev_error_x = 0.0
            self.prev_error_y = 0.0
            self.ff_vel_x = 0.0
            self.ff_vel_y = 0.0
            self.ctrl_rate_x = 0.0
            self.ctrl_rate_y = 0.0
            self.interp_budget_x = 0.0
            self.interp_budget_y = 0.0
            self.fresh_compute = False
            self.smooth_h = locked_track.h
            self.smooth_w = locked_track.w
            self.humanize_enabled = config.move_randomize
            self.locked_target_speed = locked_track.last_speed
            self.last_locked_raw_x = locked_track.raw_cx
            self.last_locked_raw_y = locked_track.raw_cy
            if locked_track.kalman.is_initialized():
                self.locked_target_speed_x = locked_track.kalman.get_vx()
                self.locked_target_speed_y = locked_track.kalman.get_vy()
            else:
                self.locked_target_speed_x = 0.0
                self.locked_target_speed_y = 0.0
            target_x = (use_x - self.smooth_w / 2.0 + self.smooth_w * config.aim_position_x) - capture_cx
            target_y = (use_y - self.smooth_h / 2.0 + self.smooth_h * self._aim_pos_y_for_class(self.locked_class_id, config)) - capture_cy
            self.locked_x = target_x
            self.locked_y = target_y
            if self.aim_active:
                self.compute_move(self.locked_x, self.locked_y, config)
                self.prev_move_x = self.move_x
                self.prev_move_y = self.move_y
            else:
                self.move_x = 0.0
                self.move_y = 0.0
                self.prev_move_x = 0.0
                self.prev_move_y = 0.0
                self.prev_error_x = 0.0
                self.prev_error_y = 0.0
            self.has_target = True
            return
        # 无新检测帧: 什么都不做(保持上帧状态)
