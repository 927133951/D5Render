# -*- coding: utf-8 -*-
# 跟随性能指标计算
import math


def compute_metrics(res, scenario):
    t0 = scenario.eval_start
    ts = res.t
    ex = res.err_x
    ey = res.err_y
    idx0 = 0
    for i, t in enumerate(ts):
        if t >= t0:
            idx0 = i
            break
    # 收敛时间: 首次|err|<10px并保持0.3s
    conv_t = None
    hold_start = None
    for i in range(idx0, len(ts)):
        e = math.hypot(ex[i], ey[i])
        if e < 10.0:
            if hold_start is None:
                hold_start = ts[i]
        else:
            hold_start = None
        if hold_start is not None and ts[i] - hold_start >= 0.3:
            conv_t = hold_start - t0
            break
    steady_i0 = idx0
    if conv_t is not None:
        st = t0 + conv_t
        for i in range(idx0, len(ts)):
            if ts[i] >= st:
                steady_i0 = i
                break
    errs = [math.hypot(ex[i], ey[i]) for i in range(steady_i0, len(ts))]
    n = len(errs)
    m = {
        '收敛时间ms': (conv_t * 1000.0) if conv_t is not None else -1.0,
        '稳态平均误差px': sum(errs) / n if n else -1.0,
        '稳态最大误差px': max(errs) if n else -1.0,
        '稳态RMS误差px': math.sqrt(sum(e * e for e in errs) / n) if n else -1.0,
        '超20px占比%': (sum(1 for e in errs if e > 20.0) / n * 100.0) if n else -1.0,
        '超调次数': count_overshoot(ex, ey, steady_i0),
        '换目标次数': res.lock_switches,
        '从未收敛': conv_t is None,
    }
    return m


def count_overshoot(ex, ey, i0):
    # 误差x符号翻转计数(幅度>5px才算有效翻转, 吞掉噪声抖动)
    signs = []
    for i in range(i0, len(ex)):
        if abs(ex[i]) > 5.0:
            s = 1 if ex[i] > 0 else -1
            if not signs or signs[-1] != s:
                signs.append(s)
    return max(0, len(signs) - 1)


def fmt_metrics(m):
    conv = m['收敛时间ms']
    conv_s = ("%.0f" % conv) if conv >= 0 else "未收敛"
    return ("收敛=%sms 稳态均值=%.1fpx 稳态最大=%.1fpx RMS=%.1fpx "
            ">20px占比=%.1f%% 超调=%d次 换目标=%d次") % (
        conv_s, m['稳态平均误差px'], m['稳态最大误差px'], m['稳态RMS误差px'],
        m['超20px占比%'], m['超调次数'], m['换目标次数'])
