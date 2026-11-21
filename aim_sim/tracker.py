# -*- coding: utf-8 -*-
# TargetTracker 逐条移植自 g:\A_Develop\DD\aim\target_tracker.h (v229a)
import math
from aim_sim.kalman import KalmanFilter2D, clamp


class TrackedTarget:
    def __init__(self):
        self.track_id = -1
        self.class_id = -1
        self.cx = 0.0
        self.cy = 0.0
        self.raw_cx = 0.0
        self.raw_cy = 0.0
        self.w = 0.0
        self.h = 0.0
        self.confidence = 0.0
        self.kalman = KalmanFilter2D()
        self.lost_frames = 0
        self.hit_frames = 0
        self.lock_confidence = 0.0
        self.last_speed = 0.0
        self.cap_cross_x = 0.0   # 最近匹配检测帧拍摄时刻的准星累计位置(世界系回转用)
        self.cap_cross_y = 0.0
        self.state_t = 0.0       # Kalman状态对应时间(=最近测量拍摄时刻, lost帧predict时推进)
        self.hist = []           # 世界系测量历史(cap_t, wx, wy), LS二次拟合速度用


class TargetTracker:
    def __init__(self):
        self.tracks = []
        self.next_id = 0
        self.match_threshold = 150.0
        self.max_lost_frames = 30
        self.kalman_process_noise = 6.0
        self.kalman_measurement_noise = 6.0
        self.kalman_predict_ahead = 0.018
        self.kalman_enabled = True

    def update(self, detections, dt, capture_w, capture_h):
        n_det = len(detections)
        n_trk = len(self.tracks)
        matched_det = [-1] * n_det
        matched_trk = [-1] * n_trk
        pairs = []
        for d in range(n_det):
            det = detections[d]
            # 世界系坐标: 检测误差 + 拍摄时刻准星位置 = 目标世界位置
            # 匹配/Kalman全部在世界系: 准星甩动不再污染匹配与速度估计
            wcx = det.get('world_cx', det['cx'])
            wcy = det.get('world_cy', det['cy'])
            for t in range(n_trk):
                trk = self.tracks[t]
                match_x = trk.cx + trk.cap_cross_x
                match_y = trk.cy + trk.cap_cross_y
                dx = wcx - match_x
                dy = wcy - match_y
                dist = math.sqrt(dx * dx + dy * dy)
                class_penalty = 500.0 if det['class_id'] != trk.class_id else 0.0
                det_area = det['w'] * det['h']
                trk_area = trk.w * trk.h
                if det_area > 0 and trk_area > 0:
                    size_ratio = det_area / trk_area
                    if size_ratio > 3.0 or size_ratio < 0.33:
                        class_penalty += 200.0
                cost = dist + class_penalty
                if cost < self.match_threshold:
                    pairs.append((d, t, cost))
        pairs.sort(key=lambda p: p[2])
        for d, t, cost in pairs:
            if matched_det[d] < 0 and matched_trk[t] < 0:
                matched_det[d] = t
                matched_trk[t] = d
        for t in range(n_trk):
            trk = self.tracks[t]
            if matched_trk[t] >= 0:
                det = detections[matched_trk[t]]
                wcx = det.get('world_cx', det['cx'])
                wcy = det.get('world_cy', det['cy'])
                trk.cap_cross_x = det.get('cap_cross_x', 0.0)
                trk.cap_cross_y = det.get('cap_cross_y', 0.0)
                trk.state_t = det.get('cap_t', trk.state_t)
                cap_t = det.get('cap_t', 0.0)
                trk.hist.append((cap_t, wcx, wcy))
                cutoff = cap_t - 0.35
                while len(trk.hist) > 14 or (trk.hist and trk.hist[0][0] < cutoff):
                    trk.hist.pop(0)
                trk.raw_cx = det['cx']
                trk.raw_cy = det['cy']
                trk.w = det['w']
                trk.h = det['h']
                trk.confidence = det['confidence']
                trk.class_id = det['class_id']
                if self.kalman_enabled:
                    trk.kalman.predict(dt)
                    trk.kalman.update_with_velocity(wcx, wcy, dt)
                    trk.cx = trk.kalman.get_x() - trk.cap_cross_x
                    trk.cy = trk.kalman.get_y() - trk.cap_cross_y
                    trk.last_speed = trk.kalman.get_speed()
                else:
                    a = 0.6
                    wx_old = trk.cx + trk.cap_cross_x
                    wy_old = trk.cy + trk.cap_cross_y
                    trk.cx = (wx_old * (1.0 - a) + wcx * a) - trk.cap_cross_x
                    trk.cy = (wy_old * (1.0 - a) + wcy * a) - trk.cap_cross_y
                trk.lost_frames = 0
                trk.hit_frames += 1
                trk.lock_confidence = self._compute_confidence(trk)
            else:
                if self.kalman_enabled:
                    trk.kalman.predict(dt)
                    trk.state_t += dt
                trk.lost_frames += 1
                trk.lock_confidence *= 0.92
                if trk.lock_confidence < 0.01:
                    trk.lock_confidence = 0.01
                if self.kalman_enabled:
                    trk.cx = trk.kalman.get_x() - trk.cap_cross_x
                    trk.cy = trk.kalman.get_y() - trk.cap_cross_y
        for d in range(n_det):
            if matched_det[d] >= 0:
                continue
            det = detections[d]
            wcx = det.get('world_cx', det['cx'])
            wcy = det.get('world_cy', det['cy'])
            ccx = det.get('cap_cross_x', 0.0)
            ccy = det.get('cap_cross_y', 0.0)
            nt = TrackedTarget()
            nt.track_id = self.next_id
            self.next_id += 1
            nt.class_id = det['class_id']
            nt.cap_cross_x = ccx
            nt.cap_cross_y = ccy
            nt.state_t = det.get('cap_t', 0.0)
            nt.hist.append((det.get('cap_t', 0.0), wcx, wcy))
            nt.cx = wcx - ccx
            nt.cy = wcy - ccy
            nt.raw_cx = det['cx']
            nt.raw_cy = det['cy']
            nt.w = det['w']
            nt.h = det['h']
            nt.confidence = det['confidence']
            nt.kalman.init(wcx, wcy)
            nt.kalman.set_process_noise(self.kalman_process_noise)
            nt.kalman.set_measurement_noise(self.kalman_measurement_noise)
            nt.kalman.set_predict_ahead(self.kalman_predict_ahead)
            nt.lost_frames = 0
            nt.hit_frames = 1
            nt.lock_confidence = 0.1
            nt.last_speed = 0.0
            self.tracks.append(nt)
        self.tracks = [t for t in self.tracks if t.lost_frames <= self.max_lost_frames]

    def _compute_confidence(self, t):
        conf = 0.3
        conf += min(t.hit_frames / 8.0, 0.35)
        conf += t.confidence * 0.25
        if t.lost_frames > 0:
            conf *= max(0.0, 1.0 - t.lost_frames * 0.05)
        return clamp(conf, 0.0, 1.0)

    def get_tracks(self):
        return self.tracks

    def find_track_data_by_id(self, track_id):
        for t in self.tracks:
            if t.track_id == track_id:
                return t
        return None

    def reset(self):
        self.tracks = []
        self.next_id = 0

    def set_match_threshold(self, v):
        self.match_threshold = v

    def set_max_lost_frames(self, v):
        self.max_lost_frames = v

    def set_kalman_params(self, q, r, ahead):
        self.kalman_process_noise = q
        self.kalman_measurement_noise = r
        self.kalman_predict_ahead = ahead

    def set_kalman_enabled(self, e):
        self.kalman_enabled = e
