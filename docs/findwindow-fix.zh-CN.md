# 通用输入法修复（FindWindowW）

修复输入法让游戏卡死或崩溃的问题，不区分输入法。整合包的 `SDPlus.dll` 破坏了游戏进程里所有 DLL 的
窗口查找，本修复为游戏本体以外的所有代码恢复正常查找。English: [findwindow-fix.md](findwindow-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `findwindow_fix` | 客户端 | 整个功能 |

与 [`ime_fix`](ime-fix.zh-CN.md)（修正游戏读取已提交文字的长度）相互独立。

![游戏大厅中使用输入法](findwindow-fix-ime.png)

## 使用

无需操作，客户端安装框架后自动生效。本 mod 需要同一版本的 `sdgo\core.dll`（使用 `HookSystemExport`），
安装或更新客户端框架时两者会一起更新。它在 SDGO Mod Manager 的“修复归档”中列出。排查问题需要临时关闭时，
点“显示开发信息”，再关闭它的开关并重启游戏；这会在 `sdgo\sdgo.ini` 的 `[mods]` 下写入 `findwindow_fix=0`。

## 设置

开发设置，在 Manager 点“显示开发信息”后显示。保存在 `sdgo\mods\findwindow_fix\mod.ini`，游戏启动时读取：

| 键 | 默认 | 含义 |
|---|---|---|
| `Trace` | 0 | 1：每次外部窗口查找都记录返回的句柄（不记录窗口名）；0：只记录第一次成功的查找 |

## 日志与排错

```
findwindow_fix: hooked system export user32.dll!FindWindowW
findwindow_fix: ready: normal FindWindowW for every caller except GOnline.exe and SDPlus.dll
findwindow_fix: loaded FindWindowW fix 1.0.0
findwindow_fix: window lookup for an external caller: 000105DC
```

| 日志 | 含义 |
|---|---|
| `window lookup for an external caller: ...` | 游戏以外的某个 DLL 查找窗口，得到了真实句柄 |
| `core lacks HookSystemExport` | `sdgo\core.dll` 比本 mod 旧，请重新安装整个客户端包 |
| `SDPlus.dll is not loaded; nothing to fix` | 该游戏目录没有 `SDPlus.dll`，窗口查找没有被破坏，本 mod 不启用 |
| `hook system export user32.dll!FindWindowW failed: ...` | 其它 mod 已经占用了这个 hook |

本修复只针对窗口查找。如果某个输入法仍然让游戏卡死，原因不在这里。

## 实现原理

整合包的汉化插件 `SDPlus.dll` 把 `user32!FindWindowW` 换成了一段并不真正查找窗口的代码。它只应付游戏自己的
单开检查（因此游戏可以多开）；其它调用方会拿到假句柄，类名为 NULL 或 atom 时还会直接崩溃。

输入法运行在游戏进程里。需要查找窗口的输入法，例如查找后台 `iFlyPlatform.exe` 的讯飞，会把初始化请求发给
假句柄，然后在游戏界面线程上一直等待。从不查找窗口的输入法（例如微软拼音）不受影响。

`findwindow_fix` 通过核心 hook 同一个函数，并按调用方区分：来自 `GOnline.exe` 和 `SDPlus.dll` 的调用保持
SDPlus 的行为，多开不受影响；其它调用方一律恢复正常的顶层窗口查找，不区分是哪个输入法或 DLL。

## 开发者说明

`SDPlus.dll` 由整合包的 `GOnline.exe` 导入，把 `user32!FindWindowW` 改为 `SDPlus.dll+0x3020` 处的代码：

* 类名 `_GONLINE_` 且标题 `Gundam Online` 时返回 NULL（"没有在运行"）；
* 其它任何查询返回 -1 或 +1（字符串比较的符号），不是窗口句柄；
* 类名为 NULL 或 atom 时按字符串读取，直接崩溃。

`GOnline.exe` 只在 `0x004914F6` 调用一次 `FindWindowW`，参数正是这组类名和标题，即单开检查。

`findwindow_fix` 通过核心的 `HookSystemExport`（MinHook）hook 该导出，trampoline 把 SDPlus 的改动保留为上一层。
加载时记录 `GOnline.exe` 和 `SDPlus.dll` 的地址范围，按每次调用的返回地址分派：

* 来自这两个模块的调用交给 SDPlus 的代码；
* 其它所有调用（包括之后加载的 DLL 和动态生成的代码）改用
  `FindWindowExW(GetDesktopWindow(), NULL, class, title)`：只找顶层窗口（父窗口为 NULL 时还会返回
  message-only 窗口），atom 和 NULL 参数含义不变，不改动 last error。

SDPlus 还改了 `MultiByteToWideChar` 和 `WideCharToMultiByte`，只是把代码页 874（泰文）换成另一个，
其它代码页原样通过，不影响输入法。

测试：构建后运行 `tests/findwindow_fix_test.ps1`，在独立桌面上用真实核心和 mod 对照 SDPlus 代码副本，
调用方模拟为 `SDPlus.dll` 和 `generic_input_method.dll`。
