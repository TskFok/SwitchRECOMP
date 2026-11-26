# Native Horizon Adapter 多阶段开发计划

**目标**　在 SwitchRecomp v190 内逐项引入可选的 Windows/Linux 系统服务适配，提供可观测、可比较的 Native 与 Legacy 路由。

**架构**　在 `swr_svc` 和 `ipc_handle` 的服务分派处加薄调度层，保留原 ABI 和 guest 状态模型。先验证可独立切换的时钟与只读文件，再扩展到使用现有线程、事件和 mutex 原语的有状态接口。

**技术栈**　C99 与项目允许的 GNU 扩展、Python 3、GCC/MinGW-w64、Win32、POSIX，沿用 `LOG(...)` 和现有目标构建入口。

**依据**　[Phase 0 架构审计](2026-11-18-native-horizon-adapter-audit.md) · [开发指南](../../../DEVELOPER_GUIDE.md) · [回归流程](../../../REGRESSION.md)

## 全局约束

- 保持 `SwrModule.abi == 1`、`CPU` / `RtAPI` 布局、AArch64→C→GCC 路径和原模块加载。从既有 `plat.c`、内核、服务对象和 GPU 后端复用实装原语。启动时确定路由配置，运行中保持稳定。
- 首发路由默认 Legacy。已选择 Native 的操作以准确的 guest 结果结束，host 错误可追踪。Native 一旦开始执行，所有结果均由该路径返回。路由切换以调用前、已验证的条件为界。
- 以独立语义测试及已确认授权的 homebrew 或自有程序为验收输入。记录接口规范与实现来源，保存开发与测试证据。涉及未授权 SDK、固件、密钥、访问控制绕过、泄漏源码或未经授权商业内容的工作排除在范围外。授权存疑标记 `LEGAL_REVIEW_REQUIRED`，暂停受影响的集成或发布。
- 保留现有许可证、NOTICE、署名与启动画面。新增源码沿用项目头部约定，关键改动写入英文 `CHANGELOG_v190.txt`。如提交 GitHub，提交消息使用简体中文。沿用当前分支。

## 路由与验证设计

| 位置 | 计划中的责任 |
| --- | --- |
| 新增 `runtime/native_hos.h`、`runtime/native_hos.c` | 内部注册表、启动配置、路由决策和 trace。以 `svc.0x1e` 等编号唯一索引 SVC，以 `ipc.<服务族>.<接口>.<命令>` 索引 IPC。IPC 对象创建时携带规范化来源，domain、克隆及别名沿用同一身份。注册表维护操作组、允许的能力条件和 Native 处理函数。 |
| 修改 `runtime/kernel.c`、`runtime/ipc.c`、`runtime/main.c` | 复用现有入口。`swr_svc` 负责寄存器级回调和 SVC 结果类型，`ipc_handle` 保留控制命令、对象解析、响应封包和引用回收，在服务命令调用前选择实现。启动初始化配置。 |
| 修改 `runtime/plat.c`、`runtime/svc_core.c`、`runtime/svc_fs.c` | 时钟、打开/读取/查询大小/关闭的 host 入口及服务语义。必要的 Win32/POSIX 差异保留在平台层。Native `IFile` 对象持有单一后端资源，并在各命令和销毁时使用相同 owner。 |
| 修改 `switchrecomp.py`、`CHANGELOG_v190.txt` | Runtime 入口加入适配器源文件，覆盖四种目标。生成代码和动态模块链接沿用原流程。 |
| 新增 `tests/nha/` | `run.sh` 及 `run.ps1` 驱动宿主合同测试与自有 guest 集成测试。以 C stubs 注入时间、I/O 与线程失败，搭配真实 Win32/POSIX 原语和 ABI 1 模块验证。 |

建议的内部接口是 `int nha_init(void)`、`int nha_try_svc(CPU *c, u32 imm)` 和 `int nha_try_ipc(SvcObj *obj, IpcCtx *ctx)`。`nha_init` 返回 `0` 表示配置有效。两个 `try` 返回 `0` 代表由既有实现接手，返回 `1` 代表已写入 SVC 寄存器或 `IpcCtx.rc`。后者包含 Native 成功和 Native 失败。单元测试提供 host 调用计数，验证一次 guest 调用只有一个实现负责。IPC 错误写入服务回包，传输层 `x[0]` 沿用现有契约。对无错误位的调用如 `GetSystemTick`，在配置或能力校验时排除无实现的 Native 选择，保留值返回约定。

启动读取 `SWR_NATIVE_HOS` 总开关与 `SWR_NATIVE_TIME`、`SWR_NATIVE_FS`、`SWR_NATIVE_THREAD`、`SWR_NATIVE_SYNC` 类别开关，值为 `0` 或 `1`，缺省为 `0`。总开关启用后，类别开关选择注册表中的已验证条目。用 `SWR_NATIVE_APIS=svc.0x1e=native,ipc.time.time.300=legacy` 做精确覆盖。未知键或无法一致切换的对象序列在启动时给出配置错误。带状态的命令共享 `KObj` / `FileData`，需要共同切换的生命周期操作按同一注册组处理。Native 已选中而能力不支持时返回该协议的明确错误。只读挂载之外的 FS 命令走事先登记的 Legacy 规则，trace 记录原因。

`SWR_NATIVE_TRACE=1` 独立于路由开关，默认关闭。通过现有 `LOG` 输出版本化 JSON Lines 记录，字段至少包含 `schema_version`、`call_id`、`phase`、`api_id`、`route`、`reason`、`guest_thread`、`guest_pc`、`outcome`、`guest_result`、`host_error` 和 `duration_ns`。`guest_result` 按寄存器返回、传输返回和服务 RC 区分；`host_error` 带 Win32 或 `errno` 域。处理 `ExitThread` 等无返回调用的终止事件，记录 trace 丢失与截断。未迁移调用标记 Legacy，已识别占位标记 `legacy_stub`，未确认的旧实现保留待审核状态。日志用编号、长度和状态等可比较字段，排除文件原始路径、guest 缓冲内容和用户数据。

## Phase 0　代码、构建与许可审计 · 已完成

- [x] 固定 `713c861`，形成 Horizon 依赖图、SVC/IPC 链路、模块 ABI、替换清单、保留机制、许可矩阵和风险登记。
- [x] 验证 CLI 与现有独立录制时钟用例，记录四目标构建环境及 GPU 样本缺口。

**交付及验收**　[Phase 0 架构审计](2026-11-18-native-horizon-adapter-audit.md) 能按文件定位候选接口、平台原语和许可证据。宿主测试结果与目标系统未执行项在审计中分别记录。

## Phase 1　最小架构及接口接入

依赖 Phase 0。先完成调度与时间，随后做只读 FS，再接入线程和同步。时间是第一个端到端验证点。

- [x] **1.1 建立可独立构建的调度层。** 新增 `native_hos.h`、`native_hos.c` 和 `tests/nha/test_dispatch.c`。先用假处理函数断言默认、组开关、精确覆盖、重复配置、未知 ID、未支持操作和后端失败。接入 SVC、IPC 和启动入口，两个 headless 目标先通过构建。guest 收到接口定义的结果，Win32 或 `errno` 原值进入 trace。用 `713c861` 的重编译入口构建自有最小 guest 模块，保存源码、编译命令、模块校验和及输入树，供三路复用。
- [x] **1.2 接入 `svc.0x1e`、`svc.0x0b` 及 `ipc.time.time.300`。** 复用 `rt_ticks`、`plat_ns`、`plat_sleep_ns`、`plat_yield`、`plat_unix_time`，保持 19.2 MHz、guest 时间缩放、睡眠输入与寄存器副作用、Unix 秒的 64 位 IPC 回包。`test_time.c` 用可控时钟固定输入值，断言 `1` 秒未经缩放对应 `19,200,000` ticks，wall clock 回拨不影响 monotonic wait。子时钟对象、共享内存、时区及 CNTVCT 接法进入兼容性回归。
- [ ] **1.3 接入只读 `IFileSystem.8`、`IFile.0`、`IFile.4` 与关闭。** 覆盖已确认根路径内的只读普通文件。Win32 使用 `CreateFileW`、`ReadFile`、`GetFileSizeEx` 和 `CloseHandle`，在同步句柄下将文件定位和读操作串行化。Linux 使用 `openat`、`pread`、`fstat` 和 `close`。继续使用 SvcObj 及 domain 关闭路径。`test_fs.c` 断言 64 位偏移、短读、EOF、无效范围、权限及并发读，路径解析对符号链接/reparse point 和改名竞争具备经测试的根目录约束。guest buffer 验证在读前完成。写入、目录、RomFS 和存档迁移使用已登记的原路径。
- [x] **1.4 接入线程、Event 和 Mutex。** 线程首批 `0x08/09/0a`，复用 `_beginthreadex` / `pthread_create` 及原 guest trampoline，失败时回滚 guest 可见状态并释放本次已拥有资源。事件覆盖 `0x11/12/17/18/19/45` 的建、发、清/复位、等、取消，继续通过原 `0x16` 释放 handle。Mutex 首批 `0x1a/1b`，在保留 guest owner/waiter 字与仲裁队列的前提下复用 SRWLOCK/condition variable 和 pthread。`test_thread.c` / `test_sync.c` 固定测试重复启动、等待与关闭竞争、取消、复位、mutex 移交和跨 Legacy 路由的同一对象。首批只适配已证明的 Event 模式，其他模式记录兼容性差距。

**交付及验收**　Runtime 四目标可构建，适配器也能由契约测试目标独立构建。自有 ABI 1 样本验证时间、文件、线程和同步接口可按配置路由，关闭时复现 Legacy 基线。每个新增接口的失败有明确 guest 结果和可追踪记录。Windows 和 Linux 至少各有一次目标宿主运行证据。完整语义矩阵在 Phase 2 收口。


**2026-11-18 验证记录**

- Linux aarch64 容器使用 GCC 12.2.0。Linux headless 和 x11 通过构建，Windows headless 和 win32 通过 MinGW-w64 12 交叉构建，关闭可选 SNXVK 链接。
- Linux headless 使用同一个基线 ABI 1 模块通过基线、Legacy、Native 和混合路由。三种新 Runtime 配置各有 65 条完整调用 trace，完整 Native 配置命中 18 个接口。ASan/UBSan 的 5 个宿主合同测试、11 个调度场景及仓库音频与录制时钟用例通过。
- Linux x11 的自有样本通过了 guest 断言。软件 OpenGL 环境偶发在 guest 退出后异常，已在 `713c861` 和本次 Legacy Runtime 上复现，留待后续图形回归处理。
- Windows headless ABI 1 样本的四种配置在 Wine 下通过，时间、文件 IPC、线程、同步宿主用例通过。Wine 无法创建文件测试使用的目录联接，原生 Windows 运行和 NTFS 路径约束验收仍待完成。1.3 的代码与合同测试已接入。

[选项及重跑命令](../../../tests/nha/README.md)

## Phase 2　语义测试与接口契约

依赖 Phase 1。为实际接入的每个 `api_id` 建立规范条目和测试索引，预期值来自有授权的公开材料、自有测试或独立实验，Legacy 输出用于标注兼容性。

- [x] 建立同输入对照。以未改基线 Runtime、改后 Legacy 和改后 Native 三路执行同一 ABI 1 模块、初始文件树与测试种子。固定 guest 可观察输出、寄存器/响应字段、状态变化和资源计数。对无确定 wall-clock 数值的条件比较范围、顺序与差值。
- [x] 实现每个接口适用的正常、错误、释放、边界、并发、失败/超时和状态变化测试。纯值接口覆盖单位、边界和并发读取。文件覆盖无效/越界地址、超大偏移、EOF、短读、共享读、拒绝访问和路径范围。线程覆盖启动失败、抢占竞争、TLS、栈、guest 优先级/亲和性。同步覆盖零/有限/无限等待、纳秒向 host 时间换算、同时 signal/cancel/close、多等待者、自动/手动复位、内存可见性和 mutex 条件变量交叉调用。记录首批接口的进程内范围，共享内存与跨进程行为保留原路径并记录后续验收条件。
- [x] 做可重复的故障注入。Host 创建失败、I/O 失败及 wait timeout 分别验证 guest 结果、状态、资源释放、诊断 trace 和 Legacy 调用计数。在 Linux 运行 ASan/UBSan。
- [ ] 在原生 Windows 留存测试结果和资源计数，验证 NTFS 路径范围。

**交付及验收**　`tests/nha/` 含每项接口的命令、数据来源、预期和实测记录。每个差异有归因及处理决定；证据不足的条目保持实验性或留在 Legacy。

**2026-11-18 验证记录**　[Phase 2 接口契约与实测](../../../tests/nha/CONTRACTS.md) 覆盖 18 个已接入接口。Linux 的同输入对照、故障注入及 ASan/UBSan 验证通过。Win64 交叉构建及 Wine 常规语义用例通过，原生 Windows 的进程句柄计数和 NTFS 路径验证仍待完成。

## Phase 3　接口依赖与 Runtime 最小化报告

依赖 Phase 2 的接口契约及 trace 样本。

- [x] 在 `docs/nha/interfaces.json` 固化版本化接口清单。每条含 `api_id`、名称、SVC 或服务来源、参数和返回值、错误码、调用关系、handle 类型和寿命、线程安全和内存可见性、行为、Windows/Linux 原语、兼容差异、实现/测试状态、来源及授权状态。静态枚举 SVC 与已注册 IPC，显式列出 IPC/时间、文件、线程同步、VM、输入、音频、显示和 GPU 提交。补记直接 `RtAPI.ticks` 和与依赖相关的 `nvdrv` IOCTL 子命令，动态对象与不可静态恢复的调用单列。
- [x] 新增 Python 报告工具 `tests/nha/report.py`，输入接口清单、trace 与样本清单，输出 JSON 调用次数与关系、所需 Native 模块、未知或占位操作、剩余 Legacy 依赖及可审查的裁剪候选。将未调用、未覆盖、证据不足和许可待审分别记录。
- [x] 用固定 trace 验证成功、失败、两个同名来源对象、丢失记录、截断和未知命令。用至少一个自有 ABI 1 样本生成可复现的完整报告。裁剪候选仅输出文件/符号和证据，任何实际裁剪另行验证直接调用、跨模块调用、动态对象及可选配置。

**交付及验收**　同一组样本的静态候选与实际调用可交叉核对，报告能定位每一条缺口或 Legacy 依赖，并带样本、提交、平台、路由开关及 trace 健康状态。

**2026-11-18 验证记录**　[接口清单与实测报告](../../nha/README.md)。Linux headless 的自有 ABI 1 样本三组 trace 各有 71 次完整调用，34 个实际调用的 ID 全部命中静态清单。固定 trace 覆盖同名来源、错误结果、未知命令、记录缺失和截断。

## Phase 4　Runtime 集成与回归

依赖 Phase 2 语义结论与 Phase 3 报告。

- [ ] 按审计文档的四条命令编译并运行 Linux x11/headless、Windows win32/headless。每个目标运行默认、完整 opt-in 和单接口切换。记录编译器、host、构建配置、样本来源、退出码及 trace 差异。Windows 目标在 Windows 实机或虚拟机执行，交叉构建单独记录。用 Phase 1 保存的基线 ABI 1 模块验证可直接替换 Runtime。
- [ ] 执行 `REGRESSION.md` Level 0 的构建和包检查，按适用性运行 `sh tests/run_audio.sh`。重建并验证已授权 GPU 样本数量，以 `regrun.sh BASE_RUNTIME base`、`regrun.sh NEW_RUNTIME new` 和 `python3 tests/regress/cmpreg.py new base` 做 Level 2 对照。对窗口启动、音频就绪、VSync、GL、headless 帧数和 Windows 可选 Vulkan 保留证据。
- [x] Level 3/4 只使用权限已确认的输入和可用硬件。Windows Intel 场景需要实际设备证据；缺授权样本或硬件时明确登记为未覆盖。Windows Vulkan 的许可条件独立核对，保留 `SWR_NO_SNXVK=1` 的基线结果。
- [x] 对 ABI、guest RC、句柄、同步、状态、trace 和性能波动分类，修复或记录偏差，再复跑相关目标。把重要行为及开关写入英文 `CHANGELOG_v190.txt`。

**交付及验收**　提交源码补丁、构建命令、测试源码、真实退出状态、差异报告和未支持接口清单。四目标默认路由和已进入 Native 的接口均完成对照，兼容性结论与单个程序能否启动分别报告。

**2026-11-26 验证记录**　四目标构建通过，Linux x11 和 headless 的 ABI 1 路由矩阵通过。Level 0 的正式版版本标识、原生 Windows 运行、历史 GPU 样本仍有缺口。退出状态、兼容层证据、自有 GPU 样本与剩余验证见 [Phase 4 集成报告](../../nha/phase4.md)。

## Phase 5　发布前审计

依赖 Phase 4 的实际制品与测试记录。

- [x] 生成制品 SBOM 和合规报告，含修改文件、实现来源、依赖版本、许可证变化、版权风险、发布阻断项与待人工复核项。已核对两套 OpenGL/headless Runtime、两个 GUI 和实际静态链接成员。含 SNXVK 的 win32 无实际制品，列入阻断项。
- [x] 核对实物中的 License/NOTICE、署名、版本、包内容和体积，并登记启动画面、修改声明、对应源码及构建脚本的验证缺口。含 Vulkan 的组合按 Phase 0 的 AGPL/GPL 风险登记复核。
- [ ] 检查源码、归档、编译产物、样本与 trace。确认发布物中的数据、外部字体、外部 SDK/DLL、游戏内容与 shader 均有适用权限，排除密钥、未授权固件/SDK、DRM 绕过及未授权商业产物。用发布实际清单复核每个 `LEGAL_REVIEW_REQUIRED` 项，只放行通过的制品。

**交付及验收**　每个制品都有可验证的源码、许可证和授权记录。所有适用回归和合规门槛通过后进入发布；未关闭的阻断条目记录影响范围和待审证据。

**2026-11-26 审计记录**　[发布前审计](../../nha/phase5.md) · [制品 SBOM](../../nha/phase5.sbom.cdx.json)。四个 Runtime 和两个 GUI 重建后与最终测试包逐字节一致。四个 Runtime 仍报告 `v190 test`，Linux GUI 缺少字体且解包后启动失败。各制品的对应源码、许可、授权和适用回归缺口已登记。本批制品无可放行项。

**候选制品复核**　已生成带 `v190` 标识的 Linux 和 Windows OpenGL/headless 候选包，补齐 GUI 字体、对应源码索引、实际依赖通知与打包验证。原生 Windows、尚缺授权的测试和人工复核仍为发布阻断项。 [候选制品复核](../../nha/phase5.md#v190-候选制品复核) · [候选 SBOM](../../nha/phase5-release.sbom.cdx.json)
