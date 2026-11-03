# MHGU MouseLookDirect v1.0.3

适用于 Windows 重编译版 MHGU，包含 Forge PC 0.7.0，不需要预装 Forge。

这是全新的独立实现。插件启动时只读取配置，不访问相机、不捕获鼠标、不安装
Forge hook，因此不会影响标题和主界面。

## 原理

- 按 F8 后，通过 `singleton_getByName("sFestaCamera")` 找到相机单例。
- 只在单例前 4 KB 中查找真正的 `uQuestCamera` 对象指针。
- 等待对象稳定 1.5 秒后，安装老版验证过的两个相机写入点：
  `main+0x1C33C` 和 `main+0x1C358`。
- 鼠标移动直接改变相机 Eye/Up，不经过右摇杆。
- 写入点位于游戏相机计算之前，避免直接更新时被解释成人物转向。
- 可选的角度字段模式可修改 INI 中的 `angle_mode`、偏移、方向和限位。
- 相机不存在、窗口失焦或按 F8 关闭时立即停止写内存。

## 安装

1. 完全关闭游戏。
2. 把游戏 EXE 拖到 `install-oneclick.bat`，或者双击后输入游戏目录。
3. 安装完成后必须通过游戏目录里的 `launch.bat` 启动。

安装器会：

- 安装 Forge PC 0.7.0。
- 安装全新的 `MouseLookDirect` 插件。
- 把旧 `MouseLook` 移到游戏根目录的 `MouseLook.legacy-*`，避免两个插件同时加载。
- 备份并应用玩家 0 的 PC 键位。
- 关闭模拟器鼠标层和触屏层。

## 使用

1. 使用 `launch.bat` 进入游戏。
2. 进入实际任务、村庄或其他有玩家镜头的场景。
3. 等待约 2 秒。
4. 按 **F8** 开启或关闭鼠标视角。

安装器会关闭模拟器的 `[Dev] order_probe`，避免 F8 诊断与插件切换冲突。

## 键鼠动作映射

| 鼠标输入 | 输出按键 | 游戏手柄 |
| --- | --- | --- |
| 左键 | Z | X / 主攻击 |
| 右键 | E | A |
| 滚轮下 | E | A |
| 滚轮上 | Q | Y |
| Mouse4 / X1 | Left Shift | 按下左摇杆 / L3 |
| Mouse5 / X2 | C | R |
| 中键 | Left Ctrl | L |

鼠标按键会保持至少 100 ms，避免游戏轮询漏掉点击。SDL 玩家 0 键位仍为
`A=E, B=Space, X=Z, Y=Q, L3=Left Shift, R=C, L=Left Ctrl, ZL=R`。

## 配置

```text
mods\0100770008DD8000\MouseLookDirect\mouselook_direct.ini
```

- `sens_x_deg_per_count` / `sens_y_deg_per_count`：灵敏度。
- `invert_x` / `invert_y`：水平、垂直反转。
- `toggle_key`：开关键，默认 F8。
- `button_hold_ms` / `wheel_hold_ms`：鼠标映射按键的最短保持时间。
- `settle_ms`：任务加载后等待相机稳定的时间。
- `angle_mode = false`：默认使用经过验证的相机向量写入。
- `angle_mode = true`：测试角度字段模式。
- `hide_cursor`：开启时隐藏系统光标。
- `center_cursor`：每帧把光标放回窗口中心。

## 日志

```text
mods\0100770008DD8000\Forge\forge.log
```

正常开启应依次看到：

```text
uQuestCamera found at root+0x54 -> ...
camera direct-write locked: eye ... target ...
camera write hook main+0x1C33C: installed
camera write hook main+0x1C358: installed
```

## 卸载

运行 `uninstall-oneclick.bat`。被移除的插件目录会保留为
`MouseLookDirect.removed-时间`，不会直接删除。
