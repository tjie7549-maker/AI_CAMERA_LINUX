#!/usr/bin/env python3
"""无需板卡的运行配置契约检查。"""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
config = json.loads((root / "configs" / "config.json").read_text(encoding="utf-8"))
required_strings = [
    "socket_path", "log_path", "sensor_subdev", "sender_path", "npu_path",
    "iq_dir", "isp_control_socket", "rkipc_path", "rkipc_socket",
]
required_booleans = ["start_pipeline", "start_npu", "auto_ae"]

for key in required_strings:
    assert isinstance(config.get(key), str) and config[key], key
for key in required_booleans:
    assert isinstance(config.get(key), bool), key
assert config["socket_path"].endswith(".sock")
assert config["sender_path"].endswith("/media-sender")
assert config["restart_after_failures"] >= 1
assert config["low_light_frames"] >= 1
assert config["recover_frames"] >= 1
print("config contract passed")
