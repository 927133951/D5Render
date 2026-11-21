# -*- coding: utf-8 -*-
# 自瞄跟随算法Python仿真主程序
# 移植v229a控制律 + 真实游戏链路仿真 + 无限迭代调优至完美跟随
import os
import sys
import copy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aim_sim.game_model import run_scenario
from aim_sim.scenarios import ALL
from aim_sim.metrics import compute_metrics, fmt_metrics
from aim_sim.visualize import plot_scenario, plot_summary
from aim_sim.aim_assist import SimConfig

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'aim_sim', '输出')


def tune_config():
    # 调优参数覆盖层(迭代修改这里)
    cfg = SimConfig()
    return cfg


def run_suite(label, color, config=None, seed=100):
    results = []
    for maker in ALL:
        sc = maker()
        res = run_scenario(sc, config=config, seed=seed)
        m = compute_metrics(res, sc)
        print('[%s] %s: %s' % (label, sc.name, fmt_metrics(m)))
        results.append((sc, res, m))
    return results


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print('=' * 80)
    print('基线(C++ v229a默认参数)')
    print('=' * 80)
    base = run_suite('基线', '#d62728', SimConfig())
    for sc, res, m in base:
        plot_scenario(sc, [('基线', res, '#d62728')], os.path.join(OUT_DIR, '基线_%s.png' % sc.name))
    print('基线图已保存至', OUT_DIR)


if __name__ == '__main__':
    main()
