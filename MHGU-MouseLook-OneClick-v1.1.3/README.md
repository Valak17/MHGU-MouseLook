# MHGU MouseLook 一键完整版 v1.1.3

适用于**干净的重编译版 MHGU**。不需要预先安装 Forge。

安装包内已经包含：

- Forge PC 0.7.0
- MouseLook 原生鼠标视角插件
- 已调好的鼠标方向、灵敏度和光标配置
- 自动 Forge loader 配置
- 触屏指针关闭设置
- Windows 系统光标隐藏（仅在游戏内 MouseLook 激活时拦截）
- 推荐 PC 键位预设

## 支持版本

```text
MONSTER HUNTER GENERATIONS ULTIMATE.exe
SHA-256 053CAE60DA1BFF123E391E5582A650C1B25E200A0F09A0DD265E321225F38FE5
```

安装器会校验 EXE 哈希，不匹配时会停止，避免装到错误版本。

## 一键安装

1. 关闭游戏。
2. 把游戏的 `MONSTER HUNTER GENERATIONS ULTIMATE.exe` 拖到：

```text
install-oneclick.bat
```

也可以双击 `install-oneclick.bat`，然后输入包含游戏 EXE 的目录。

安装器会自动：

- 安装 Forge PC。
- 启用 `game_settings.ini` 中的 Forge loader。
- 安装 MouseLook。
- 关闭仿真器鼠标和触屏指针。
- 应用推荐的 WASD/键鼠映射。
- 备份被修改的配置。

安装完成后请通过游戏目录中的 `launch.bat` 启动。该启动脚本会先关闭仿真器触屏层，再启动游戏，避免游戏退出时把触屏设置写回 `true`。如果直接启动 EXE，可能重新出现触屏指针。

## 使用

- 进入实际任务、村庄或其他有玩家镜头的场景。
- 移动鼠标控制视角。
- F8 开关 MouseLook。
- 不需要按 F7。
- Insert 打开 Forge 菜单。
- 标题、存档选择和菜单中不会接管相机或鼠标按键。

默认鼠标动作：

| 输入 | 对应手柄/功能 |
| --- | --- |
| 鼠标左键 | X / 主攻击 |
| 鼠标右键 | A |
| 滚轮下 | A |
| 滚轮上 | Y |
| Mouse4（X1） | 按下左摇杆 |
| Mouse5（X2） | R |
| 鼠标中键 | L |

键盘映射：

| 手柄 | 键盘 |
| --- | --- |
| A | `E` |
| Y | `Q` |
| R | `C`（由 Mouse5 发送） |
| L3 / 按下左摇杆 | `Left Shift` |
| ZL | `R` |
| ZR | `R` |
| L | `Left Ctrl` |

## 配置备份

安装器会生成：

```text
game_settings.ini.oneclick-backup
launch.bat.oneclick-backup
user\config\sdl2-config.ini.oneclick-backup
user\config\sdl2-config.ini.mouselook-backup
mods\0100770008DD8000\Forge\forge.ini.oneclick-backup
mods\0100770008DD8000\MouseLook\mouselook.ini.oneclick-backup
```

## 只安装 MouseLook、不修改键位

高级用户可运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -GamePath "D:\Games\MHGU" -ApplyKeymap:$false
```

## 卸载

运行 `uninstall-oneclick.bat` 或：

```powershell
powershell -ExecutionPolicy Bypass -File .\uninstall.ps1 -GamePath "D:\Games\MHGU"
```

它会停用并移动 MouseLook 文件夹，恢复安装前备份；Forge 保留供其他模组使用。

## 常见问题

**安装器提示路径错误**

必须选择包含以下内容的游戏目录：

```text
MONSTER HUNTER GENERATIONS ULTIMATE.exe
exefs\
mods\
user\
game_settings.ini
```

不要选择上层文件夹，也不要选择 `MHGU-MouseLook-OneClick` 本身。

**存档界面无法确认**

使用方向键和 `E`，不要使用鼠标左键。

**系统鼠标指针仍然显示**

- v1.1.3 在游戏内使用透明光标，并仅在 MouseLook 激活时通过
  `SetWindowLongPtr` 临时拦截 `WM_SETCURSOR`。
- 离开游戏、打开菜单、失焦或按 F8 关闭时立即移除，不永久替换窗口过程。

**换游戏版本后失效**

MouseLook 与 EXE 版本绑定，需要重新适配。

## 授权

MouseLook 使用 MIT License。包内 Forge PC、Dear ImGui 和 Roboto 字体各自的许可证位于 `payload\Forge\licenses`。
