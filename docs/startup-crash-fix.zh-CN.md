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
