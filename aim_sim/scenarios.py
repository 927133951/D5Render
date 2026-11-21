# -*- coding: utf-8 -*-
# 目标运动场景(屏幕像素域, aim=0时目标框中心的屏幕坐标)
import math


class Scenario:
    def __init__(self, name, desc, duration, position_fn, eval_start=0.0):
        self.name = name
        self.desc = desc
        self.duration = duration
        self.position = position_fn
        self.eval_start = eval_start  # 评估起点(新目标场景=出现时刻)


def linear_run():
    # 匀速横跑: 300px/s, 到右边界反向
    def pos(t):
        x = 1200.0
        v = 300.0
        if x + v * t > 1750.0:
            x = 1750.0 - (x + v * t - 1750.0)
        else:
            x = x + v * t
        return x, 650.0
    return Scenario("匀速横跑", "300px/s匀速+边界折返", 6.0, pos, 0.8)


def snake():
    # 蛇形: 正弦横摆250px@0.5Hz(峰速785px/s) + 纵向小幅摆动
    def pos(t):
        x = 960.0 + 250.0 * math.sin(2.0 * math.pi * 0.5 * t)
        y = 700.0 + 60.0 * math.sin(2.0 * math.pi * 0.8 * t)
        return x, y
    return Scenario("蛇形走位", "正弦250px@0.5Hz 峰速785px/s", 8.0, pos, 1.0)


def stop_turn():
    # 急停急转: 400px/s→急停→反向加速900px/s→停→正向
    def pos(t):
        x = 1200.0
        if t < 0.8:
            x = 1200.0 + 400.0 * t
        elif t < 1.1:
            x = 1200.0 + 400.0 * 0.8
        elif t < 1.9:
            x = 1200.0 + 320.0 - 0.5 * 900.0 * (t - 1.1) * (t - 1.1) - 100.0 * (t - 1.1)
        elif t < 2.2:
            x = 1200.0 + 320.0 - 0.5 * 900.0 * 0.64 - 100.0 * 0.8
        elif t < 3.4:
            x = 1200.0 + 320.0 - 448.0 - 80.0 + 350.0 * (t - 2.2)
        else:
            x = 1200.0 + 320.0 - 448.0 - 80.0 + 350.0 * 1.2
        return x, 700.0
    return Scenario("急停急转", "400px/s→急停→反向900px/s加速", 6.0, pos, 1.2)


def jump():
    # 跳跃: 纵向快速起跳下落(峰速880px/s), x小幅横移
    def pos(t):
        x = 1200.0 + 80.0 * math.sin(2.0 * math.pi * 0.3 * t)
        y = 700.0 - 140.0 * math.sin(2.0 * math.pi * 1.0 * t)
        return x, y
    return Scenario("跳跃起伏", "纵向正弦140px@1Hz 峰速880px/s", 6.0, pos, 1.0)


def new_target():
    # 新目标突然出现: 0.8s前无目标, 之后出现在FOV半径内(误差338px<480px)
    def pos(t):
        if t < 0.8:
            return -9999.0, -9999.0
        return 1298.0, 700.0
    return Scenario("新目标出现", "t=0.8s目标出现于(1298,700)", 4.0, pos, 0.8)


ALL = [linear_run, snake, stop_turn, jump, new_target]
