# 好友列表修复

[English](friend-list-fix.md) · [返回功能目录](../README.zh-CN.md)

修复原生好友名单的三个问题：离线好友从名单中消失、显示旧的角色名，以及无法对离线好友打开聊天窗口。
加好友、查看名单和发消息仍使用游戏原有的好友页面。

| mod | 运行在 | 负责 |
|---|---|---|
| `friend_list_fix` | WorldServer | 返回完整名单和当前角色名 |
| `friend_list_client_fix` | 客户端 | 让已确认的离线好友也能打开原生聊天窗口 |

## 使用

两部分都是自动修复，在 SDGO Mod Manager 的“修复归档”中列出，不需要设置。安装后重启 WorldServer，
再用更新后的客户端重新登录。游戏每次登录只读取一次好友名单，重新打开好友页不会刷新已经出错的名单。

**本修复没有增加离线私信。** 对离线对象发消息时，服务器仍返回原生错误 `0x1407`（不存在的名字为 `0x1406`），
不会假装已经送达。需要对方之后阅读的内容请使用游戏原有的邮件。

WorldServer 的数据库账号需要能创建 schema、存储过程并读取元数据。

## 日志

| 日志 | 含义 |
|---|---|
| `friend list fix registered; owned SQL query initializes on the first list request ...` | 服务端已加载，第一次请求名单时初始化 |
| `friend list policy unavailable; native database failure returned, retry is allowed` | 初始化失败，本次返回原生失败答复，之后可重试 |
| `accepted offline friends may open the native conversation UI ...` | 客户端部分已加载 |

## 开发者说明

客户端 `friend_list_client_fix` 1.1.1 还会把两个经过签名核对的好友布局调用点
重定向到现有的 `uirs/0553.uir`。程序原先请求的 `uirs/1364.uir` 已是零件交换布局，
缺少聊天控件。公共字符串赋值入口保留给商店本地化等其他 Mod 使用，EXE 和资源归档保持原样。

服务端 0.1.2 按角色名列的实际排序规则处理当前名字与回退值，再转换为原生 ANSI 结果，
兼容默认 Thai、Unicode 角色名列为 Chinese 的混合数据库布局，不改列定义。


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
