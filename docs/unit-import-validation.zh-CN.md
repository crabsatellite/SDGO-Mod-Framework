# 机体导入的开发验证

导入检查、原生表登记、实际画面和出机是不同范围。资源生成图不被记录为原生游戏验收，
调用成功也不能证明模型外观、动作或武器行为正确。

## 静态与事务检查

使用 Win32 MSVC 构建，运行本模块、Manager 和资源生成器的相关检查：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure -R "unit_(catalog|preview|import|presentation)|mod_manager"
```

检查应覆盖记录长度与身份、跨类型编号冲突、引用、资源路径、缺失字段、永久机体类别、
图片版本、显式技能移除、SQL 原子性与安装所有权。已有收集扭蛋时，机体族根形态声明也属于同一事务。
模型、动作、纹理等松散资源必须和 DLL、目录、原生包一起封存哈希；运行后修改输入视为失败。
同时校验 `resource-overlay.ini` 对 ZDX/ZPK 的封存、条目与所有权清单的对应，以及原生归档读取的逐字节结果。
需要分别覆盖机体目录独立启用、与扭蛋目录同时启用、旧接口拒绝、重复登记和跨包路径冲突。
文件存在或原始 GDT 登记成功不能代替游戏的资源流读取和真实模型显示。

客户端目录包含以下原生类型：机体 `2`（4976 字节）、武器 `10`（1871 字节）、
物品类别 `0x22`（614 字节）和机体一览 `0x55`（614 字节）。一览编号保持原生枚举所需的连续性，
每个库存根形态一条；没有对应设计图时不借用其他机体的配方。

## 原生客户端检查

使用当前仓库的 [原生测试 guide](../skills/sdgo-native-testing/SKILL.md)。
复用已有本地 test 及其账号，测试使用独立原生 SQL、回环地址和客户端副本，不写真实玩家库存。
不使用 Docker。仅在并发冲突时复制 test，给副本分配独立端点和数据库文件；同一任务连续复用自己的副本。
缺少机体时只在测试数据库中通过原生存储过程补齐必要资产，保留已有身份和库存 UID。

在候选 stage 的 `game_test_client/mod.ini` 中启用 `UnitPresentations=1`；
需要截图时再启用 `CaptureNativeFrames=1`。设置必须在 `prepare` 封存输入前完成。
绑定测试账号实际持有的机体 UID，运行：

```powershell
python tools/game_test.py prepare --client-template <local-test-client> --stage <candidate-stage> --run-dir build/game-test-runs/unit-check --target-host <test-loopback-ip> --login-port <test-login-port> --account-profile <private-existing-account.ini> --play-unit-uid <owned-unit-uid> --include-mod unit_catalog
python tools/game_test.py run --run-dir build/game-test-runs/unit-check --timeout 600
```

场景通过原始登录、机库选择与成功回包读取实际 UID，打开原生机体一览，搜索并选择对应条目，
等待详情模型及实际变形动作状态。普通对战继续执行建房、开局和正常结束返回；页面检查不等于对战通过。

需要制作机体图标时，在同一候选 stage 中额外启用 `UnitPortraits=1`，
保留 `UnitPresentations=1` 和 `CaptureNativeFrames=1`，然后执行上述 `prepare/run`。
运行报告生成后导出：

```powershell
python -m tools.game_test_portraits --run-dir build/game-test-runs/unit-check --unit <root-unit-id> --output build/native-unit-portraits/new-unit
```

它在已初始化的原生详情实体上调用 `wait_01` / `choice_01`，让游戏正常更新并渲染，
在 Present 前读取真实 backbuffer，按原生模型视口裁剪 PNG。图片保留模型、来源帧与输入哈希。
这些 PNG 是服主请求的本地制作素材；不进入通用工具默认内容，也不将截图成功报告为完整实战通过。
生成后应实际查看模型、姿势、裁剪边界和武器挂点，再上传到 Manager 的图片通道。

读取本次原生断言、运行与清理结果，缺项、超时、异常和未完成的原生回包均按失败处理。

若只验收已安装收藏扭蛋的原生海报和确认框，在封存的测试组件中启用
`CollectionPreviewOnly=1`，然后使用 `--pages-only`，不要添加 `--collection-draw`。
此设置仅追加现有 `pages.collection.preview`，验证真实活动机台、价格、原生确认框及未提交购买请求；
不会追加抽取和领取场景。活动与收藏共用原生机台海报页面，该页面不枚举奖池中每台机体的头像。
奖池根形态登记、机台海报与固定／自定义候选头像需要分别核验，不能用一张海报证明整个奖池。

检查指定候选时，在封存前启用 `UnitPresentations=1`，设置 `UnitAcquisitionKind=1`（固定）或 `3`（自定义）、
`UnitAcquisitionMachine` 和 `UnitAcquisitionUnit`，并绑定该玩家实际持有的机体 UID，使用 `--pages-only`。
`pages.unit_acquisition` 通过原生机台／候选控件打开机体视图，核验真实 `0x01F8` 消费事件和可见模型实体；
不购买，也不修改自定义机台的已选槽位。配合 `CaptureNativeFrames=1` 检查实际卡面与模型截图。
独立候选配置可以明确声明空表，但仍逐表核对注册范围、条目身份和表末边界。

成功记录按 guide 在验证及必要交付读回后删除；失败证据保留于临时运行目录之外以便排错。
不得停止其他活跃环境，结束前核对本次环境已回收。

## 部署读回

确认数据库定义与机体族元数据、客户端目录和资源、安装收据、现有商城／扭蛋模块登记的版本一致。
获取途径仍由对应模块配置。原生服务缓存和客户端重启状态单独核对，不能从构建或安装成功推断已经生效。
多人同步、全部武器技能、各模式、复活与结算只在对应真实场景完成后声明通过。
