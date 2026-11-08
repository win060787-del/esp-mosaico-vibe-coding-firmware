# 验证记录 / Validation

## 2026-11-09：固件 3.2.8

- **目标设备**：ESP-Iris Mosaico，ESP32-S31，Device ID `4553502d4952495301001c2904d0a33a`，通过 Mac 的 USB Highspeed 连接。
- **构建**：使用固定 ESP-IDF 修订 `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` 构建 3.2.8。官方 `iris system-update` 重新生成包含应用、分区表和 GSP UI 资源的 `.irisfw` 包。
- **写入**：`python mosaico.py iris system-update --project projects/codex_pet --device-id 4553502d4952495301001c2904d0a33a` 回报 `iris system-update: succeeded`，并回报目标固件与系统分区健康。未直接刷写或擦除整片闪存。
- **设备读回**：更新前应用版本 3.2.7、Boot ID `14930927598924319032`；更新后同一 Device ID 回报应用版本 **3.2.8**、新的 Boot ID `12954969968245731427`、`firmware_mode=normal`、无 crash-loop 标记。
- **界面截图**：[截图目录](screenshots)为 480 × 480 原生 GSP 模拟器生成。主屏、任务、语音、设置、语言切换、终端和 Wi-Fi 页面已重新截取，使用演示数据。截图不是实物照片，也不证明实机触控或联网状态。

## 仍需实物验收

- 在实物屏幕上切换中文与 English，重启后确认语言保留；逐页检查文字是否截断。
- 在当前 Wi-Fi 上观察一次真实的 Mac 任务与额度同步、完成提醒和语音输入。
- 若需支持更多电脑或路由器，还应分别在第二台 Mac 与另一种 Wi-Fi 环境中验证配对和自动重连。

电脑检测到 USB 端口和模拟器运行通过，并不等同于上述真实使用场景已经验收。
