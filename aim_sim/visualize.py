# -*- coding: utf-8 -*-
# 仿真结果可视化(中文标注)
import os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

plt.rcParams['font.sans-serif'] = ['Microsoft YaHei', 'SimHei']
plt.rcParams['axes.unicode_minus'] = False


def plot_scenario(sc, runs, out_path):
    # runs: [(label, res, color), ...] 多组对比
    fig, axes = plt.subplots(2, 1, figsize=(14, 8))
    ax1, ax2 = axes
    for label, res, color in runs:
        ax1.plot(res.t, res.err_x, color=color, linewidth=1.2, label=label + ' X误差')
        ax1.plot(res.t, res.err_y, color=color, linewidth=1.0, linestyle='--', alpha=0.6, label=label + ' Y误差')
    ax1.axhline(10, color='gray', linewidth=0.8, linestyle=':')
    ax1.axhline(-10, color='gray', linewidth=0.8, linestyle=':')
    ax1.axhline(0, color='black', linewidth=0.8)
    ax1.set_xlabel('时间 (秒)')
    ax1.set_ylabel('误差 (像素)')
    ax1.set_title('%s — 真实误差曲线 (%s)' % (sc.name, sc.desc))
    ax1.legend(fontsize=9, loc='upper right')
    ax1.grid(True, alpha=0.3)
    for label, res, color in runs:
        ax2.plot(res.tgt_x, res.tgt_y, color='orange', linewidth=1.0, alpha=0.7, label='目标aim点轨迹' if label == runs[0][0] else None)
        ax2.plot(res.aim_x, res.aim_y, color=color, linewidth=1.0, label=label + ' 准星轨迹')
    ax2.set_xlabel('屏幕X (像素, 相对中心)')
    ax2.set_ylabel('屏幕Y (像素, 相对中心)')
    ax2.set_title('准星跟随轨迹')
    ax2.legend(fontsize=9)
    ax2.grid(True, alpha=0.3)
    ax2.invert_yaxis()
    fig.tight_layout()
    fig.savefig(out_path, dpi=110)
    plt.close(fig)


def plot_summary(all_runs, out_path):
    # all_runs: [(场景名, [(label, 指标dict, color), ...]), ...]
    names = [r[0] for r in all_runs]
    conv = []
    rms = []
    over = []
    for _, metrics_list in all_runs:
        conv.append([m['收敛时间ms'] for _, m, _ in metrics_list])
        rms.append([m['稳态RMS误差px'] for _, m, _ in metrics_list])
        over.append([m['超20px占比%'] for _, m, _ in metrics_list])
    n_sc = len(names)
    n_run = len(all_runs[0][1])
    fig, axes = plt.subplots(1, 3, figsize=(16, 5))
    titles = ['收敛时间 (ms)', '稳态RMS误差 (px)', '误差>20px占比 (%)']
    data = [conv, rms, over]
    for k in range(3):
        ax = axes[k]
        width = 0.8 / n_run
        for j in range(n_run):
            xs = [i + (j - (n_run - 1) / 2.0) * width for i in range(n_sc)]
            vals = [data[k][i][j] if data[k][i][j] >= 0 else 0 for i in range(n_sc)]
            label = all_runs[0][1][j][0]
            ax.bar(xs, vals, width, label=label, color=all_runs[0][1][j][2], alpha=0.85)
        ax.set_xticks(range(n_sc))
        ax.set_xticklabels(names, fontsize=9)
        ax.set_title(titles[k])
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3, axis='y')
    fig.suptitle('五种运动模式跟随性能对比', fontsize=14)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    fig.savefig(out_path, dpi=110)
    plt.close(fig)
