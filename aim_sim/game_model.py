# -*- coding: utf-8 -*-
# 游戏闭环+检测链路仿真模型
# 链路: 目标移动→画面→捕获120fps→推理延迟60ms→检测帧30fps到达→主循环240Hz输出→鼠标计数→视角旋转
import math
import random
from bisect import bisect_right
from aim_sim.aim_assist import AimAssist, SimConfig

SIM_DT = 0.001          # 仿真步长1ms
CAPTURE_FPS = 120.0     # 捕获帧率
DETECT_FPS = 30.0       # 检测帧到达率(实测16~50fps取中值)
OUTPUT_FPS = 240.0      # 主循环输出帧率
SCREEN_W = 1920
SCREEN_H = 1080
INFER_DELAY_S = 0.060   # 捕获→检测到达的纯滞后(渲染+捕获+推理+传输)
BOX_W = 32.0            # 目标框尺寸(中距离敌人)
BOX_H = 64.0
NOISE_SIGMA = 2.0       # 检测框中心高斯噪声(px)
MISS_RATE = 0.15        # 漏检概率(开枪时更高, 本仿真不开枪)
PX_PER_COUNT = 1.0      # 1鼠标计数=1屏幕像素(v226控制律标定假设)


class SimResult:
    def __init__(self):
        self.t = []            # 时刻(s)
        self.err_x = []        # 真实x误差(目标aim点-准星, 当前真实值非延迟值)
        self.err_y = []
        self.aim_x = []        # 准星累计位置(px域)
        self.aim_y = []
        self.tgt_x = []        # 当前真实目标aim点屏幕坐标
        self.tgt_y = []
        self.has_target = []
        self.lock_switches = 0

    def finalize(self):
        return self


def run_scenario(scenario, config=None, seed=100, aim_active=True, assist=None):
    rng = random.Random(seed)
    if config is None:
        config = SimConfig()
    if assist is None:
        assist = AimAssist()
    assist.reset()
    assist.humanize_enabled = config.move_randomize
    duration = scenario.duration
    n_steps = int(duration / SIM_DT)
    cap_times = []
    cap_hist = []  # (t, tx, ty, ax, ay) tx/ty=目标框中心在aim=0域的屏幕坐标
    next_cap_t = 0.0
    next_detect_t = 0.05   # 前50ms无检测(链路预热)
    detect_interval = 1.0 / DETECT_FPS
    next_out_t = 0.0
    out_interval = 1.0 / OUTPUT_FPS
    prev_out_t = None
    prev_detect_t = None
    aim_x = 0.0
    aim_y = 0.0
    res = SimResult()
    last_switch_id = -1
    for step in range(n_steps):
        t = step * SIM_DT
        tx, ty = scenario.position(t)
        if t >= next_cap_t:
            cap_times.append(t)
            cap_hist.append((t, tx, ty, aim_x, aim_y))
            next_cap_t += 1.0 / CAPTURE_FPS
        if t >= next_out_t:
            if prev_out_t is None:
                out_dt = out_interval
            else:
                out_dt = t - prev_out_t
            prev_out_t = t
            dx, dy = assist.get_move_delta(dt=out_dt, out_dt=out_dt)
            aim_x += dx * PX_PER_COUNT
            aim_y += dy * PX_PER_COUNT
            assist.note_sent(int(dx), int(dy), t)
            next_out_t += out_interval
        if t >= next_detect_t:
            if prev_detect_t is None:
                dd = detect_interval
            else:
                dd = t - prev_detect_t
            prev_detect_t = t
            next_detect_t += detect_interval * (1.0 + rng.uniform(-0.08, 0.08))
            t_cap = t - INFER_DELAY_S
            idx = bisect_right(cap_times, t_cap) - 1
            detections = []
            if idx >= 0:
                st, stx, sty, sax, say = cap_hist[idx]
                cx = stx - sax
                cy = sty - say
                if -120.0 < cx < SCREEN_W + 120.0 and -120.0 < cy < SCREEN_H + 120.0:
                    if rng.random() >= MISS_RATE:
                        detections.append({
                            'cx': cx + rng.gauss(0.0, NOISE_SIGMA),
                            'cy': cy + rng.gauss(0.0, NOISE_SIGMA),
                            'w': BOX_W, 'h': BOX_H,
                            'confidence': 0.85, 'class_id': 0,
                            'cap_t': st,
                        })
            assist.update(detections, SCREEN_W / 2.0, SCREEN_H / 2.0, SCREEN_W, SCREEN_H,
                          config, aim_active, True, dd, t)
            if assist.locked_track_id >= 0 and last_switch_id >= 0 and assist.locked_track_id != last_switch_id:
                res.lock_switches += 1
            last_switch_id = assist.locked_track_id
        err_x = tx - aim_x - SCREEN_W / 2.0
        err_y = (ty - BOX_H / 2.0) - aim_y - SCREEN_H / 2.0
        res.t.append(t)
        res.err_x.append(err_x)
        res.err_y.append(err_y)
        res.aim_x.append(aim_x)
        res.aim_y.append(aim_y)
        res.tgt_x.append(tx - SCREEN_W / 2.0)
        res.tgt_y.append((ty - BOX_H / 2.0) - SCREEN_H / 2.0)
        res.has_target.append(assist.has_target)
    return res
