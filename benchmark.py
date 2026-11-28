# -*- coding: utf-8 -*-
# 正式基准测试脚本: 5场景跟随性能评估 + 对比图输出(最终交付)
import os
import sys
import math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from aim_sim.game_model import run_scenario
from aim_sim.scenarios import ALL
from aim_sim.metrics import compute_metrics, fmt_metrics
from aim_sim.aim_assist import SimConfig, AimAssist
plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei', 'SimSun']
plt.rcParams['axes.unicode_minus'] = False
OUT_DIR = os.path.dirname(os.path.abspath(__file__))
# 完美标准: 场景名 -> (指标名, 阈值, 类型stat=稳态指标/event=事件相对计时)
PERF_STD = {'匀速横跑': ('稳态平均误差px', 3.0, 'stat'), '蛇形走位': ('稳态平均误差px', 8.0, 'stat'),
            '急停急转': ('恢复时间ms', 150.0, 'event'), '跳跃起伏': ('稳态平均误差px', 8.0, 'stat'),
            '新目标出现': ('响应时间ms', 200.0, 'event')}
# 事件时刻: 急停急转=急停结束开始反向加速; 新目标出现=目标出现的瞬间
EVENT_T = {'急停急转': 1.1, '新目标出现': 0.8}
WEIGHTS = {'蛇形走位': 0.35, '跳跃起伏': 0.30, '匀速横跑': 0.15, '急停急转': 0.10, '新目标出现': 0.10}
def eval_start_index(res, t0):
    for j, t in enumerate(res.t):
        if t >= t0:
            return j
    return 0
def recovery_after(res, t_event, thr=10.0, disturb=15.0):
    # 事件后恢复时间(ms): 误差先被事件扰动(>disturb)后首次回落<thr; 事件时误差本就<thr且未被扰动返回0
    disturbed = False
    for j, t in enumerate(res.t):
        if t >= t_event:
            e = math.hypot(res.err_x[j], res.err_y[j])
            if e > disturb:
                disturbed = True
            elif e < thr and disturbed:
                return (t - t_event) * 1000.0
    return -1.0 if disturbed else 0.0
def run_bench():
    results = []
    print('=' * 86)
    print('D5Render 自瞄跟随性能基准测试(最终默认参数)')
    print('链路: 仿真步长1ms | 捕获120fps | 推理延迟60ms | 检测30fps(±8%) | 漏检15% | 检测噪声σ=2px | 输出240Hz')
    print('=' * 86)
    for maker in ALL:
        sc = maker()
        res = run_scenario(sc, config=SimConfig(), seed=100, assist=AimAssist())
        m = compute_metrics(res, sc)
        results.append((sc, res, m))
        name, thr, kind = PERF_STD[sc.name]
        if kind == 'event':
            val = recovery_after(res, EVENT_T[sc.name])
            std_txt = '%s(事件t=%.1fs) ≤ %g' % (name, EVENT_T[sc.name], thr)
            val_txt = ('%.1f' % val) if val >= 0 else '未测得'
        else:
            val = m[name]
            std_txt = '%s ≤ %g' % (name, thr)
            val_txt = '%.1f' % val
        ok = '达标' if (0.0 <= val <= thr) else '未达标'
        print('[%s] %s' % (sc.name, sc.desc))
        print('   ' + fmt_metrics(m))
        print('   完美标准: %s | 实测 %s → %s' % (std_txt, val_txt, ok))
    score = 0.0
    for sc, res, m in results:
        score += WEIGHTS[sc.name] * m['稳态平均误差px']
        if m['换目标次数'] > 0:
            score += m['换目标次数'] * 30.0
        if m['收敛时间ms'] < 0 and sc.name not in ('蛇形走位', '跳跃起伏'):
            score += 50.0
    print('-' * 86)
    print('综合评分(加权稳态误差, 越低越好): %.1f' % score)
    print('=' * 86)
    return results, score
def plot_errors(results, score):
    fig, axes = plt.subplots(2, 3, figsize=(16.5, 8.6))
    fig.suptitle('跟随误差曲线 |err|(t) — 红虚线=20px超标线 灰点线=10px收敛判据 绿点划线=收敛时刻', fontsize=14)
    for i, (sc, res, m) in enumerate(results):
        ax = axes[i // 3][i % 3]
        i0 = eval_start_index(res, sc.eval_start)
        ts = np.array(res.t[i0:])
        errs = np.hypot(np.array(res.err_x[i0:]), np.array(res.err_y[i0:]))
        ax.plot(ts, errs, lw=0.9, color='#1f6fd6')
        ax.axhline(20.0, color='#d62728', ls='--', lw=1.0)
        ax.axhline(10.0, color='#888888', ls=':', lw=1.0)
        conv = m['收敛时间ms']
        if conv >= 0:
            ax.axvline(sc.eval_start + conv / 1000.0, color='#2ca02c', ls='-.', lw=1.1)
            ax.set_title('%s | 收敛%.0fms 稳态均值%.1fpx' % (sc.name, conv, m['稳态平均误差px']), fontsize=11)
        else:
            ax.set_title('%s | 未收敛 稳态均值%.1fpx' % (sc.name, m['稳态平均误差px']), fontsize=11)
        ax.set_xlabel('时间(s)')
        ax.set_ylabel('误差(px)')
        ax.grid(alpha=0.3)
        ax.set_xlim(sc.eval_start, sc.duration)
    ax = axes[1][2]
    ax.axis('off')
    rows = ['指标汇总(综合评分 %.1f)' % score, '']
    for sc, res, m in results:
        conv = ('%.0f' % m['收敛时间ms']) if m['收敛时间ms'] >= 0 else '未收敛'
        extra = ''
        if sc.name in EVENT_T:
            rec = recovery_after(res, EVENT_T[sc.name])
            extra = (' 恢复%.0fms' % rec) if rec >= 0 else ' 恢复未测得'
        rows.append('%s' % sc.name)
        rows.append('  收敛%sms 均值%.1fpx 最大%.1fpx >20px:%.1f%% 超调%d%s' % (
            conv, m['稳态平均误差px'], m['稳态最大误差px'], m['超20px占比%'], m['超调次数'], extra))
    ax.text(0.02, 0.98, '\n'.join(rows), va='top', fontsize=11)
    p = os.path.join(OUT_DIR, 'benchmark_error_curves.png')
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(p, dpi=110)
    plt.close(fig)
    return p
def plot_traj(results):
    fig, axes = plt.subplots(2, 3, figsize=(16.5, 9.0))
    fig.suptitle('目标轨迹 vs 准星轨迹(屏幕坐标系, 相对屏幕中心, y轴向下)', fontsize=14)
    for i, (sc, res, m) in enumerate(results):
        ax = axes[i // 3][i % 3]
        i0 = eval_start_index(res, sc.eval_start)
        ts = np.array(res.t)
        tx = np.array(res.tgt_x).astype(float)
        ty = np.array(res.tgt_y).astype(float)
        ax_x = np.array(res.aim_x)
        ax_y = np.array(res.aim_y)
        vis = (ts >= sc.eval_start) & (tx > -5000.0)
        txp = np.where(vis, tx, np.nan)
        typ = np.where(vis, ty, np.nan)
        ax.plot(txp, typ, '--', color='#d62728', lw=1.4, label='目标')
        ax.plot(ax_x[i0:], ax_y[i0:], '-', color='#1f6fd6', lw=1.0, label='准星')
        ax.invert_yaxis()
        ax.set_aspect('equal', adjustable='datalim')
        ax.set_title('%s (%s)' % (sc.name, sc.desc), fontsize=10)
        ax.set_xlabel('x(px)')
        ax.set_ylabel('y(px)')
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8, loc='best')
    axes[1][2].axis('off')
    axes[1][2].text(0.05, 0.9, '说明:\n红虚线=目标运动轨迹\n蓝实线=准星跟随轨迹\n准星越贴合红线的稳态段,\n跟随性能越好', va='top', fontsize=12)
    p = os.path.join(OUT_DIR, 'benchmark_trajectories.png')
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(p, dpi=110)
    plt.close(fig)
    return p
if __name__ == '__main__':
    results, score = run_bench()
    p1 = plot_errors(results, score)
    p2 = plot_traj(results)
    print('对比图已输出:')
    print('  ' + p1)
    print('  ' + p2)
