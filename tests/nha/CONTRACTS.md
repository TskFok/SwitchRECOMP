# Native Horizon Adapter Phase 2 接口契约

下面记录当前实验性 Runtime 所承诺的 guest 行为。参数宽度、缓冲区和状态来自仓库 ABI 1 模块、源码及独立测试条件。测试使用同属仓库许可的原创 AArch64 汇编、自建文件和 C 故障注入。基线日志用于定位兼容差异。按 [验证命令](README.md#重跑验证) 运行可生成每项用例的构建记录、原始日志和 JSON 结果。

`RC(m,d)` 表示 IPC 服务结果 `(d << 9) | m`。IPC 的传输结果始终单独检查。`IFile.close` 是最后一个对象引用释放时的内部 trace 标识，guest 用 `CloseHandle` 或 domain close 操作。

| `api_id` | 接口及期望 | 语义证据与用例 |
| --- | --- | --- |
| `svc.0x1e` | GetSystemTick。`x0` 直接返回虚拟 tick，频率 19.2 MHz，固定时钟的 52/53 ns 分别落在 tick 0/1。并发同值读取保留 `x1`。 | [`main.c`](../../runtime/main.c) · [`test_time.c`](test_time.c) · [`guest.S`](guest.S) 的 CNTFRQ/CNTVCT 顺序 |
| `svc.0x0b` | SleepThread。`x0` 输入为有符号纳秒，正数交给 host 延时，零或负数让出调度，保留 `x0` 并更新线程睡眠诊断。 | [`kernel.c`](../../runtime/kernel.c) · [`test_time.c`](test_time.c) · [`guest.S`](guest.S) 的正数延时及负数寄存器 |
| `ipc.time.time.300` | `time` 的当前 Unix 秒，为 64 位回包。时钟回拨仍返回新值，monotonic tick 独立。并发读取各自获得完整回包。`-1` 且 `errno` 已置位时返回 `RC(116,200)`，保留原错误并无数据体。 | [`svc_core.c`](../../runtime/svc_core.c) · [`test_time.c`](test_time.c) · [`guest.S`](guest.S) 的 `time:a`、子时钟、时区和共享内存 |
| `ipc.fsp-srv.IFileSystem.8` | OpenFile。只读 `sdmc` 普通文件返回 `IFile`，路径描述符须可读且包含终止零字节，否则 `RC(2,6002)`。禁止跨根目录解析。拒绝访问为 `RC(2,6400)`。回包无法发放句柄时关闭刚打开的文件，服务结果为 `RC(2,6709)`。 | [`svc_fs.c`](../../runtime/svc_fs.c) · [`test_ipc_file.c`](test_ipc_file.c) · [`test_fs.c`](test_fs.c) · [`guest.S`](guest.S) 的 domain 克隆 |
| `ipc.fsp-srv.IFile.0` | Read。64 位偏移与长度，零长度及 EOF 返回 64 位计数 0，短读只写实际前缀。无效目标区或容量不足为 `RC(2,6001)`，负偏移为 `RC(2,6061)`，偏移加长度超界为 `RC(2,3005)`。注入的 I/O 错误为 `RC(2,5000)`。 | [`svc_fs.c`](../../runtime/svc_fs.c) · [`test_ipc_file.c`](test_ipc_file.c) · [`test_fs.c`](test_fs.c) 的 4 GiB 以上稀疏文件、同句柄并发偏移读和两句柄共享读 |
| `ipc.fsp-srv.IFile.4` | GetSize，读取完整 64 位文件大小，注入查询失败为 `RC(2,5000)`。失败后可用原对象重试。 | [`svc_fs.c`](../../runtime/svc_fs.c) · [`test_fs.c`](test_fs.c) · [`test_ipc_file.c`](test_ipc_file.c) · [`guest.S`](guest.S) |
| `ipc.fsp-srv.IFile.close` | 最后一个引用释放时关闭所属 host 资源一次，domain ID 随后查询返回 `0x1D60A`。模拟 host 已消费所有权但报告失败时，析构 trace 记录 `RC(2,5000)` 和 host 错误，guest 句柄仍然释放。 | [`ipc.c`](../../runtime/ipc.c) · [`svc_fs.c`](../../runtime/svc_fs.c) · [`test_ipc_file.c`](test_ipc_file.c) · [`guest.S`](guest.S) |
| `svc.0x08` | CreateThread。成功时 `x1` 返回未启动线程的句柄，初始化 arg、SP、独立 TLS、guest 优先级与核掩码。无效枚举、对齐或句柄用尽分别为 `0xF001`、`0xCC01`、`0xD201`，句柄分配失败会退回新 TLS 页。 | [`kernel.c`](../../runtime/kernel.c) · [`test_thread.c`](test_thread.c) · [`guest.S`](guest.S) 的栈、TLS 和 `0x0c/0x0e` |
| `svc.0x09` | StartThread。只启动一次。并发启动中一次成功，另一次为 `0xFA01`。host 创建失败为 `0xD001`，保留未启动状态、句柄、TLS 和再次启动的能力。失效句柄为 `0xE401`。 | [`kernel.c`](../../runtime/kernel.c) · [`test_thread.c`](test_thread.c) · [`guest.S`](guest.S) 的退出后重复启动 |
| `svc.0x0a` | ExitThread。无返回，trace 标记 `terminated`。退出使线程对象可等待，并向等待者发布退出前写入。宿主用例另外检查退出 trampoline 释放线程诊断用 host handle 的调用次数。 | [`kernel.c`](../../runtime/kernel.c) · [`guest.S`](guest.S) 的 seed 回读 · [`test_thread.c`](test_thread.c) |
| `svc.0x11` | SignalEvent。写端置位并唤醒全部等待者。读端 signal 返回 `0xE401`。置位后手动复位对象可被反复等待，唤醒后可见 signal 前的写入。 | [`kernel.c`](../../runtime/kernel.c) · [`test_sync.c`](test_sync.c) · [`guest.S`](guest.S) |
| `svc.0x12` | ClearEvent。已接入的手动复位对象清除信号，操作可重复，失效句柄为 `0xE401`。自动复位对象返回 `0xF601` 且保留信号。 | [`kernel.c`](../../runtime/kernel.c) · [`test_sync.c`](test_sync.c) 的清除、轮询与已置位自动事件 |
| `svc.0x17` | ResetSignal。读端复位已置位对象成功，已清除时为 `0xFA01`，写端为 `0xE401`。 | [`kernel.c`](../../runtime/kernel.c) · [`test_sync.c`](test_sync.c) · [`guest.S`](guest.S) |
| `svc.0x18` | WaitSynchronization。`x1` 返回首个就绪下标。轮询与有限期超时为 `0xEA01`，负期限无限等待，可 signal 或 cancel。取消为 `0xEC01`。未映射、越界或未对齐数组为 `0xCC01`，超过 64 个为 `0xEE01`，写端或无效句柄为 `0xE401`，自动复位对象为 `0xF601`。已经登记的对象在关闭后由等待引用保活。 | [`kernel.c`](../../runtime/kernel.c) · [`plat.c`](../../runtime/plat.c) · [`test_sync.c`](test_sync.c) 的多等待者与竞争 · [`test_time.c`](test_time.c) 的 37 ns 超时和 Win32 单位换算 |
| `svc.0x19` | CancelSynchronization。一次取消当前的同步等待，或留给下一次等待消费。已取消的等待清除状态，非线程句柄为 `0xE401`。与 signal 并发时根据争用次序返回成功或取消。 | [`kernel.c`](../../runtime/kernel.c) · [`guest.S`](guest.S) 的预取消 · [`test_sync.c`](test_sync.c) 的 signal/cancel/close |
| `svc.0x45` | CreateEvent。`x1/x2` 返回同一未置位手动事件的写端/读端，分别关闭。仅剩一个槽时返回 `0xD201`，两个返回寄存器归零，可继续复用剩余槽。 | [`kernel.c`](../../runtime/kernel.c) · [`test_sync.c`](test_sync.c) · [`guest.S`](guest.S) |
| `svc.0x1a` | ArbitrateLock。guest 原子字保留 owner/waiter 协议。未匹配时立即成功，已排队时在移交后返回并看见已发布数据。越界地址为 `0xCC01`，匹配时无效 owner 为 `0xE401`。 | [`kernel.c`](../../runtime/kernel.c) · [`test_sync.c`](test_sync.c) 的 Native 与混合互斥移交 · [`guest.S`](guest.S) 的零竞争路径 |
| `svc.0x1b` | ArbitrateUnlock。把互斥字交给合适等待者，无等待者时清零。未对齐地址为 `0xCC01`。与 Legacy `0x1c/0x1d` 共享进程内队列，条件变量唤醒者可排入互斥等待队列。 | [`kernel.c`](../../runtime/kernel.c) · [`test_sync.c`](test_sync.c) 的 Native/Mixed 交叉调用 · [`guest.S`](guest.S) |

## 已测兼容差异和支持边界

| 条件 | 归因及处理 |
| --- | --- |
| 成功退出后再调用 `StartThread` | 固定样本的 `713c861` 与当前 Legacy 返回 `0`，Native 与混合返回 `0xFA01`。保留 Native 的状态检查和 Legacy 的既有结果。 |
| 错误文件缓冲区、偏移、拒绝访问或宿主 I/O 失败 | Native 在访问前验证 guest 区间及权限并报告服务错误。Legacy 对长度做截断，并使用 `fseek`/`fread`。文件后端继续按对象 owner 绑定。故障注入测得传输数据前的读失败保留缓冲区及句柄，分段读取中途失败的缓冲区状态仍待验证。 |
| Event 端点权限和自动复位 | Native 拒绝非法端点及自动复位模式。Legacy 可接受额外端点组合，等待不会消耗自动复位信号。仅手动复位模式进入已验证范围。自动复位后续需验证多等待者点名唤醒、重复置位、取消竞争及信号消费。 |
| 时间和等待 | Unix 秒按每路宿主时间窗口检查，子时钟差限定在 ±5 秒。1 ms 睡眠按 19,200 ticks 下界和 10 s 样本时限检查。Host wait 使用 monotonic 纳秒，POSIX 使用 monotonic 条件变量，Win32 向上取整为毫秒，负值为无限期。记录定时差值时允许调度抖动。 |
| 线程和同步对象作用域 | guest 优先级、亲和性、TLS 和对象可见性限定于当前 Runtime 进程。线程退出后保留既有诊断对象，测试单独统计 guest 引用和退出时释放的 host handle。跨进程同步、共享内存与地址仲裁沿用 Legacy。接入前需在授权样本上验证映射与权限、跨进程可见性、等待者回收和退出竞争。 |
| 文件作用域 | Native 限只读 `sdmc` 常规文件。写入、目录、存档、RomFS 沿用既有路由。Linux 已测试符号链接和改名竞争，Windows 的 NTFS reparse/junction 和实际句柄总数仍需原生验收，文件路由继续实验性 opt-in。 |

## 2026-11-18 实测

Linux aarch64，LinuxKit 7.0.14、glibc 2.36、GCC 12.2.0，以非 root 用户运行。`bin/nha-phase2-linux-final/` 下的 `host-results.json` 记录 6 个 ASan/UBSan 宿主场景和 12 个调度配置通过。文件拒绝访问真实返回 `EACCES`，目录改名循环 300 次，成功读取的内容均来自测试根目录。IPC 故障注入中 host 尝试打开 3 次、获得并关闭 2 个文件，Legacy 打开 0 次，宿主 fd 数前后均为 4。线程注入 `EAGAIN` 后重试，2 次 host 创建尝试、1 次真实退出，线程 guest 引用归零。37 ns 的注入等待只进入 host 一次，返回 `0xEA01` 并清除登记的等待者及其引用。两种同步路由另外各重复运行 12 次，均通过。

Linux 的四路 guest 进程均退出 `0`，输入种子 `0x4E484132`。同一 ABI 1 模块的 SHA-256 为 `4801c9f054935d8b4db788ab987bdf9bca2fc02662718516df8ba9628b16be7f`。每路成功关闭 12 个 guest handle，新的 Runtime 另外各记录 1 次成功的 IFile 最终关闭和 1 次线程终止。完整观测见运行生成的 `fixture-manifest.json`。

| Linux 路由 | 完整 trace 调用 | Native 接口数 | 1 ms 睡眠测得 tick 差 | 重复启动 RC |
| --- | ---: | ---: | ---: | ---: |
| `713c861` 基线 | — | — | 22631 | `0` |
| 当前 Legacy | 71 | 0 | 35073 | `0` |
| 当前 Native | 71 | 18 | 25635 | `0xFA01` |
| 混合 | 71 | 15 | 26296 | `0xFA01` |

MinGW-w64 GCC 12-posix 交叉编译 Win64 headless、6 个宿主测试二进制和基线 ABI 1 DLL 成功。Wine 8.0 下的四路 Win64 guest 及 time、ipc_file、thread 和两条 sync 宿主场景均退出 `0`。`time` 用例验证传给 Win32 API 的 6 个等待期限，包括 1 ns 向上取整、有限期饱和及无限期。文件注入保留 Win32 错误码 `5`、`30`，逐个验证 2 个文件句柄和 1 个线程诊断句柄在关闭后无效。Wine 对活跃句柄的进程统计返回 `0`，单独的目录联接用例在创建/读取 fixture 阶段退出 `3`。原生 Windows 的进程句柄计数和 NTFS 路径验收待执行。
