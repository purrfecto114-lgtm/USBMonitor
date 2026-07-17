# Markdown 报告 2026-06-30 修正落地说明

本版本按用户提供的 Markdown 诊断报告做全量落地，并对少数高风险建议采用等价安全实现。

## 已修改

1. **设备通知注册**：`DeviceWindowThread._register_notification()` 改为返回布尔健康状态；保留 Win32 官方 fixed struct 注册方式，并增加 260 wchar 宽缓冲 fallback。服务启动时会等待监听线程完成初始化，失败会停止 reconciler 并抛出明确错误。
2. **Toast 事件过滤器**：`ToastWindow` 对自身及子控件安装本地 event filter，用户点击、滚轮、触摸、按键时重置自动隐藏倒计时；关闭事件明确停止两个 `QTimer`。
3. **Hook 匹配**：路径和卷标 glob 改为 `casefold()` 后匹配，避免 Windows 盘符/卷标大小写差异导致规则不触发。
4. **入口死代码**：删除 `__main__.py` 中未使用的 `_print_usage()` 和 `sys` 导入。
5. **监听线程失败处理**：`UsbMonitorService.start()` 现在确认监听线程 ready；缺少 pywin32、窗口创建失败或设备通知注册失败都会作为启动失败上抛。
6. **日志清理**：`LoggingManager.reset_files()` 默认只删除 events/actions/errors 日志，保留 `crash.log`；需要删除崩溃日志时可传 `include_crash=True`。
7. **开机启动静默模式**：启动项 payload 增加 `--silent`，`--startup` 或 `--silent` 未显式指定 GUI backend 时使用 `tray-only`。
8. **定时器清理**：Toast `closeEvent()` 明确停止自动隐藏和倒计时定时器；`remainingTime()` 负值会归零显示。
9. **启动项复制**：standalone 目录复制时过滤 `__pycache__`、测试缓存、临时文件和日志文件；删除旧目录时遇到只读文件会尝试改写权限重试。
10. **缓存统计可观测性**：`DriveScanner.cache_stats` 被写入 `drive_scan_completed` 日志及事件 details。
11. **导入清理**：`app.py` 改为标准包内导入 `.core`，减少双路径 fallback 维护成本。
12. **单实例 Mutex**：新增 `single_instance_mutex_name()`，集中生成并清洗 Mutex 名称。

## 保留安全边界

- `parse_device_change()` 仍使用 `header.size` 做边界读取，没有改成无限制 `ctypes.wstring_at(ptr)`，避免恶意或损坏的 `lparam` 造成越界读取。
- 没有把 3600+ 行 `app.py` 机械拆成多个模块；本轮以行为修复和回归测试为主，避免一次性重构扩大风险。

## 验证

```bash
python -m py_compile usb_monitor/app.py usb_monitor/core.py usb_monitor/hooks.py usb_monitor/__init__.py usb_monitor/__main__.py
python -m pytest -q
# 129 passed
```
