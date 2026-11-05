# 好友列表修复

[English](friend-list-fix.md) · [返回功能目录](../README.zh-CN.md)

修复原生好友名单和聊天窗口的问题：离线好友从名单中消失、显示旧的角色名、无法对离线好友打开聊天窗口，
以及聊天窗口加载了错误的布局、缺少输入框而出错。
加好友、查看名单和发消息仍使用游戏原有的好友页面。主页面右上角 **USER LIST** 的右键操作也显示为中文，
有对应游戏角色资料时可显示好友军衔。

| mod | 运行在 | 负责 |
|---|---|---|
| `friend_list_fix` | WorldServer | 返回完整名单、当前角色名，并为缺失的会员军衔记录读取游戏军衔 |
| `friend_list_client_fix` | 客户端 | 恢复聊天布局，保留完整好友名，汉化主页面右键菜单，让已确认的离线好友打开聊天窗口 |

## 使用

两部分都是自动修复，在 SDGO Mod Manager 的“修复归档”中列出，不需要设置。安装后重启 WorldServer，
再用更新后的客户端重新登录。游戏每次登录只读取一次好友名单，重新打开好友页不会刷新已经出错的名单。

**本修复没有增加离线私信。** 对离线对象发消息时，服务器仍返回原生错误 `0x1407`（不存在的名字为 `0x1406`），
不会假装已经送达。需要对方之后阅读的内容请使用游戏原有的邮件。

WorldServer 的数据库账号需要能创建 schema、存储过程并读取元数据。
补全缺失军衔还需要能读取原生 INI `[DB Game] DBName` 指定的游戏库；已有会员军衔优先。
游戏军衔不可读取时，仍使用会员库返回完整好友名单。

服主开启[好友机体赠送](friend-unit-gift.zh-CN.md)后，在主页面 USER LIST 右键已确认的离线好友也能发起赠送。
其他原生操作保持原有的可用或禁用状态。

## 日志

| 日志 | 含义 |
|---|---|
| `friend list fix registered; owned SQL query initializes on the first list request ...` | 服务端已加载，第一次请求名单时初始化 |
| `friend list policy unavailable; native database failure returned, retry is allowed` | 初始化失败，本次返回原生失败答复，之后可重试 |
| `accepted offline friends may open the native conversation UI ...` | 客户端部分已加载 |

## 开发者说明

程序为聊天窗口请求的 `uirs/1364.uir` 实际是零件交换布局，没有聊天输入框，原生初始化因此读到空控件。
`friend_list_client_fix` 只在两个经过签名核对的聊天布局调用点改用归档中已有的 `uirs/0553.uir`，零件交换界面照常使用 `1364`；
公共字符串赋值入口不受影响，仍供商城汉化等其他 Mod 使用，EXE 和资源归档保持原样。

`friend_list_fix` 按角色名列的实际排序规则处理当前名字与回退值，再转换为原生 ANSI 结果，
因此数据库默认排序为 Thai、角色名 Unicode 列为 Chinese 的混合布局也能正确显示，不改列定义。

`friend_list_fix` 还通过已认证的原生 Unicode 绑定读取受损的角色名，按名字列的代码页填回名字字段，
处理原生 ANSI 绑定造成的问号，不改原存储过程和玩家数据。军衔数字或阶段缺失时，它只读取对应游戏角色的值，
不新增会员资料，也不覆盖已保存的军衔。

`friend_list_client_fix` 收到同一角色的问号名字时，保留已经读取到的完整名字；原生更新函数继续更新在线状态、
位置和好友关系，并重新生成显示文字，正常的新名字仍走原生更新流程。主页面社区名单和聊天好友页各自的右键菜单
都由它汉化，保留原生操作含义和按钮素材。

原查询用 LEFT JOIN 读取在线状态、公会等附加资料，离线或缺少资料时产生 SQL NULL。原生 OLE DB 行绑定没有状态缓冲区，
读取警告会让行循环提前结束，因而截断整份名单。`friend_list_fix` 返回原类型的非空默认值，优先读当前角色名、缺失时用好友关系中
保存的名字；好友关系、排序、分页以及五个参数 / 17 列结果的原生接口保持不变。

服务端借用已认证的会员库会话，按原生 INI 的 `[DB Member] DBName` 核对数据库，不复制凭据。首次请求时在自有的
`sdgo_friend_list_fix` schema 中创建带所有权标记的查询副本，原 `dbo.sp_FriendsList` 和玩家数据保持不变。来源结构不兼容、
schema 不属于本 Mod、自有过程被外部修改或 SQL 失败时，返回原生失败答复。原生查询改变后在下次启动时重新核对；
禁用后重启恢复原路由，自有查询对象保留供检查。

`friend_list_client_fix` 只放行资料完整、关系已确认的离线好友，保留真实的离线状态、选择检查、在线好友流程和原生发信错误。

协议、缓存和函数地址见[客户端记录](investigations/friend-list-client.md)和[服务端记录](investigations/friend-list-server.md)。
构建与测试命令见[英文说明](friend-list-fix.md#developer-tests)。
