# MHGU MouseLookDirect v1.0.3

适用于 Windows 重编译版《MONSTER HUNTER GENERATIONS ULTIMATE》PC 版。

本包已经内置 Forge PC 0.7.0，不需要预先安装 Forge。插件在启动和主界面阶段
不安装相机钩子；进入实际游戏并按 **F8** 后，才会定位当前 `uQuestCamera` 并启用
原生鼠标视角。

鼠标直接控制游戏相机，不经过右摇杆模拟，也不会改变人物移动方向。

---

## 主要功能

- 鼠标原生控制镜头 Yaw/Pitch。
- 鼠标移动不会把人物一起转向。
- 启动和标题/主菜单阶段零相机钩子。
- 只在 F8 开启且找到稳定的任务相机后写入。
- 窗口失焦、退出菜单或关闭 F8 时停止写入。
- 开启期间隐藏 Windows 系统光标。
- 支持鼠标动作键映射、滚轮映射和按键保持，避免短点击被游戏漏掉。
- 安装时自动备份配置，并隔离旧版 `MouseLook`，避免两个插件同时加载。

---

## 支持版本

安装器会校验游戏 EXE 和共同游戏模块，仅支持以下版本：

### 旧版外层 EXE

```text
053CAE60DA1BFF123E391E5582A650C1B25E200A0F09A0DD265E321225F38FE5
```

### MHGU-0.10.0 外层 EXE

```text
D7CC9B32187CA255EF9224BD3FDCF295BB3788D0474440434C11030650BCADCA
```

两个版本必须具有相同的游戏模块：

```text
exefs\main
B3935415499549ADAF4C0E89822831DAA175CFD456A7990791C79BE3997645EF
```

不符合上述哈希时，安装器会拒绝安装，避免把插件装到不兼容的版本。

---

## 安装方法一：干净游戏版本一键安装

适合没有安装 Forge，也没有旧版 MouseLook 的游戏目录。

1. 完全关闭游戏。
2. 解压本安装包。
3. 把游戏目录里的：

```text
MONSTER HUNTER GENERATIONS ULTIMATE.exe
```

拖到：

```text
install-oneclick.bat
```

4. 安装器完成后，通过游戏目录中新生成的：

```text
launch.bat
```

启动游戏。

安装器会自动：

- 安装 Forge PC 0.7.0。
- 安装 MouseLookDirect。
- 启用 Forge loader。
- 关闭模拟器触屏鼠标层。
- 启用玩家 0 的 PC 键盘映射。
- 关闭可能与 F8 冲突的 `[Dev] order_probe`。
- 备份会被修改的配置文件。

---

## 安装方法二：已有 Forge 或想保留自己的键位

适合已有 Forge、已有自定义 SDL 键位，或者只想更新 MouseLookDirect 的用户。

关闭游戏后，在本包目录打开 PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 `
  -GamePath "D:\Games\MHGU" `
  -ApplyKeymap:$false
```

把 `D:\Games\MHGU` 替换成包含以下文件的游戏目录：

```text
MONSTER HUNTER GENERATIONS ULTIMATE.exe
game_settings.ini
exefs\
mods\
user\
```

使用 `-ApplyKeymap:$false` 时：

- 仍会安装 Forge 和 MouseLookDirect。
- 仍会关闭触屏鼠标层并启用 `keyboard_enabled`。
- 不会覆盖你现有的玩家 0 键位。
- 鼠标动作映射仍按本包内置映射工作。

如果希望使用推荐键位，去掉 `-ApplyKeymap:$false` 再运行一次即可。

### 从旧版 MouseLook 升级

如果游戏目录存在：

```text
mods\0100770008DD8000\MouseLook
```

安装器会把它移动到游戏根目录：

```text
MouseLook.legacy-日期-时间
```

这样可以避免旧版和 MouseLookDirect 同时挂钩造成卡死。旧文件不会被直接删除。

---

## 使用方法

1. 必须通过游戏目录里的 `launch.bat` 启动。
2. 进入任务、村庄或其他存在玩家镜头的场景。
3. 等待约 2 秒，让任务相机完成加载。
4. 按 **F8** 开启鼠标视角。
5. 再按一次 **F8** 关闭。

不要在标题或主菜单中反复按 F8。主界面本身不需要 MouseLook。

开启后移动鼠标应看到镜头转动，而不是人物原地转身。

---

## 键鼠映射

### 鼠标动作映射

以下映射只在 F8 开启且游戏窗口聚焦时生效：

| 鼠标输入 | 输出按键 | 对应手柄动作 |
| --- | --- | --- |
| 左键 | `Z` | X / 主攻击 |
| 右键 | `E` | A |
| 滚轮下 | `E` | A |
| 滚轮上 | `Q` | Y |
| Mouse4 / X1 | `Left Shift` | 按下左摇杆 / L3 |
| Mouse5 / X2 | `C` | R |
| 中键 | `Left Ctrl` | L |

鼠标动作键和滚轮默认保持至少 `100 ms`，避免游戏轮询漏掉快速点击。

### SDL 玩家 0 键位

安装器默认应用以下映射：

| 手柄 | 键盘 |
| --- | --- |
| A | `E` |
| B | `Space` |
| X | `Z` |
| Y | `Q` |
| L3 / 按下左摇杆 | `Left Shift` |
| R | `C` |
| L | `Left Ctrl` |
| ZL | `R` |
| ZR | `R` |

`launch.bat` 每次启动都会重新应用这些设置，避免新版游戏在退出时把它们重写。

---

## 相机模式

配置文件：

```text
mods\0100770008DD8000\MouseLookDirect\mouselook_direct.ini
```

修改后需要重启游戏。

### 默认模式：向量写入

```ini
angle_mode = false
```

这是推荐模式。插件在游戏相机计算前写入：

```text
eye    +0x40
up     +0x50
target +0x60
```

使用老版验证过的两个相机调用点：

```text
main+0x1C33C
main+0x1C358
```

### 备用模式：角度字段

```ini
angle_mode = true
```

使用以下相机角度字段：

```text
Yaw:   0xAE, 0xEA, 0xD2
Pitch: 0x196
单位:  65536 / 圈
```

该模式仅用于字段适配或排查。如果画面表现不正确，请改回：

```ini
angle_mode = false
```

---

## 主要配置

```ini
[mouse]
sens_x_deg_per_count = 0.06
sens_y_deg_per_count = 0.06
invert_x = true
invert_y = true
toggle_key = F8
settle_ms = 1500
hide_cursor = true
center_cursor = true
button_hold_ms = 100
wheel_hold_ms = 100

[camera]
root = sFestaCamera
angle_mode = false
```

说明：

- `sens_x_deg_per_count`：水平灵敏度，每鼠标计数对应角度。
- `sens_y_deg_per_count`：垂直灵敏度。
- `invert_x` / `invert_y`：水平、垂直反转。
- `toggle_key`：开关 MouseLook，默认 F8。
- `settle_ms`：相机稳定等待时间。
- `hide_cursor`：开启时隐藏 Windows 系统光标。
- `center_cursor`：把光标保持在窗口中心。
- `button_hold_ms`：鼠标动作键最短保持时间。
- `wheel_hold_ms`：滚轮映射键最短保持时间。

---

## 安装配置备份

安装器会尽量保留原始文件：

```text
game_settings.ini.oneclick-backup
launch.bat.oneclick-backup
user\config\sdl2-config.ini.oneclick-backup
user\config\sdl2-config.ini.mouselook-backup
mods\0100770008DD8000\Forge\forge.ini.oneclick-backup
mods\0100770008DD8000\MouseLookDirect\mouselook_direct.ini.oneclick-backup
```

---

## 日志与正常状态

日志文件：

```text
mods\0100770008DD8000\Forge\forge.log
```

启动时应先看到：

```text
[MouseLookDirect] loaded; no hooks, no camera reads and no mouse capture at startup
```

按 F8 并进入稳定游戏场景后，应看到类似：

```text
[MouseLookDirect] uQuestCamera found at root+0x54 -> ...
[MouseLookDirect] camera direct-write locked: eye ... target ...
[MouseLookDirect] camera write hook main+0x1C33C: installed
[MouseLookDirect] camera write hook main+0x1C358: installed
```

---

## 常见问题

### F8 没有反应

- 确认游戏窗口处于前台。
- 进入实际任务后等待约 2 秒再按 F8。
- 确认没有改过 `toggle_key`。
- 检查日志是否找到 `uQuestCamera`。

### 主界面卡死

- 确认使用的是 MouseLookDirect v1.0.3，而不是旧 MouseLook。
- 确认启动阶段日志没有出现相机钩子。
- 确认旧 `MouseLook` 已被移动到 `MouseLook.legacy-*`。
- 不要在标题或主菜单反复按 F8。

### 鼠标只让人物转向

- 确认 `angle_mode = false`。
- 确认日志中两个相机写入点都已安装：
  `main+0x1C33C` 和 `main+0x1C358`。

### 鼠标按键或滚轮偶尔失效

- 保持 `button_hold_ms` 和 `wheel_hold_ms` 为 `100`。
- 确认 F8 已开启且游戏窗口聚焦。
- 确认 `launch.bat` 已重新应用玩家 0 键位。

### F8 与其他功能冲突

安装器已设置：

```ini
[Dev]
order_probe = false
```

如果仍然冲突，可在 `mouselook_direct.ini` 把 `toggle_key` 改为 `F6`、`F9` 或
`HOME`，然后重启游戏。

---

## 卸载

运行：

```text
uninstall-oneclick.bat
```

卸载器会把插件目录移动为：

```text
MouseLookDirect.removed-日期-时间
```

并尽量恢复安装前的 SDL 配置和启动脚本。被移动的插件目录不会直接删除。

---

## 已知边界

- 当前支持旧版外层 EXE 和 MHGU-0.10.0 外层 EXE。
- 其他 EXE 或不同 `exefs\main` 需要重新适配。
- 相机写入只在存在 `uQuestCamera` 的实际游戏场景使用。
- 联机时直接修改本地相机状态的兼容性和同步风险不作为验收目标。
- 建议用于单机或本地体验。
- 修改游戏内存和加载第三方 MOD 存在风险，请自行保留原始游戏与存档备份。

---

## 授权

MouseLookDirect 使用 MIT License。

包内 Forge PC、Dear ImGui、Roboto 字体和第三方组件分别遵循各自许可证，
详见 `payload\Forge\licenses`。
