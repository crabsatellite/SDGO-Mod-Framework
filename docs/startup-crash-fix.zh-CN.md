# 开游戏随机崩溃修复

修复游戏刚启动时偶尔在读取界面闪退的问题。游戏会随机挑一组读取界面，其中个别组引用的机体在客户端的
机体数据里不存在，抽到就会闪退；现在这种组会换成最近一组机体存在的读取界面。
English: [startup-crash-fix.md](startup-crash-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `startup_crash_fix` | 客户端 | 整个功能 |

## 使用

无需操作，客户端安装框架后自动生效。它在 SDGO Mod Manager 的“修复归档”中列出。排查问题需要临时关闭时，
点“显示开发信息”，再关闭它的开关并重启游戏。

## 设置

无。

## 日志与排错

第一次进入读取界面时，mod 会列出机体缺失的读取组（具体编号取决于客户端数据）：

```
startup_crash_fix: loading set <组编号>: unit <机体编号> is not in this client's unit data
startup_crash_fix: <N> loading sets, <M> with a missing unit
startup_crash_fix: loading set <组编号> picked: its unit <机体编号> is missing, set <组编号> (unit <机体编号>) shown instead
```

| 日志 | 含义 |
|---|---|
| `N loading sets, M with a missing unit` | 共 N 组，其中 M 组被抽中时原本会闪退 |
| `loading set ... picked: its unit ... is missing, set ... shown instead` | 这次启动原本会闪退；改为显示日志里写的那一组 |
| `... no set has a known unit; shown as the game would` | 当前客户端数据中没有可替换的组 |
| 点登录时闪退，`access violation at 008FEE6B` | 安装的是 1.0.0 版本 mod，请更新框架 |
| `required symbol gate.show_set_unit ...` | 客户端版本不受支持，本 mod 不启用 |

## 原理

显示带机体的读取界面时，游戏会查找这台机体，但不检查是否找到就直接使用。本 mod 先做检查：机体缺失时，
改用它之前最近一组机体存在的读取界面（没有则取之后最近的一组），就像随机本来就抽到了那一组，
因此读取界面和登录动作始终有可用的机体。

## 开发者说明

游戏启动时（`CGameApp::SetState`，`SERVICE_LOGO`），读取界面（`0x0090C320`）用 `srand(timeGetTime())`
随机挑一组（`0x00904180`：类型 5 的数据，从编号 27102 起，范围等于有机体的组数），编号存到 `+0x20E0`。
然后从 27101 起逐组遍历，对每个没有机体的组和被抽中的组调用显示函数（`0x008FE380`，符号
`gate.show_set_unit`）。对有机体的组，该函数到机体数据中查找（类型 2，`0x0059D2B0`，符号 `grid.find`），
在 `+0x207C` 创建机体模型，并且不检查是否找到就读取机体记录 `+0x264` 处的名字（`0x008FE610` 访问冲突，
游戏自己的 `zoa_log\Exp_*.txt` 中也有记录）。

点登录时，`0x008FEE30` 取编号为 `+0x20E0` 的组，在 `+0x207C` 的模型上播放动作，两者都不检查。
因此 mod 挂钩 `gate.show_set_unit`，并把替换组的编号写入 `+0x20E0`。替换组在被抽中组之前时，遍历已经走过，
当场显示；在之后时，由遍历走到时显示。两种情况都恰好设置一台机体。

1.0.0 版的做法是不显示机体，导致 `+0x207C` 未设置，只要抽中机体缺失的组，点登录就会闪退（`0x008FEE6B`）。

参考客户端的 9 组读取界面中有 2 组机体缺失：第 27106 组（机体 25452）和第 27107 组（机体 25454）。
