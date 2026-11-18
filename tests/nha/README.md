# Native Horizon Adapter 验证

Runtime 在启动时固定路由，缺省为 Legacy。设置 `SWR_NATIVE_HOS=1` 后，可以独立启用下列类别。各开关只接受 `0` 或 `1`。

| 开关 | 接入范围 |
| --- | --- |
| `SWR_NATIVE_TIME` | `svc.0x1e`、`svc.0x0b`、`ipc.time.time.300`。沿用模块的 19.2 MHz 虚拟时钟回调，`time:u`、`time:a`、`time:s` 共用时钟服务标识。 |
| `SWR_NATIVE_FS` | 模拟 SD 卡 `sdmc` 上以只读模式打开的普通文件。覆盖 `ipc.fsp-srv.IFileSystem.8`、`ipc.fsp-srv.IFile.0`、`ipc.fsp-srv.IFile.4` 和 `ipc.fsp-srv.IFile.close`。 |
| `SWR_NATIVE_THREAD` | `svc.0x08/09/0a`。失败的启动保留已创建线程及其 handle，允许重试。 |
| `SWR_NATIVE_SYNC` | `svc.0x11/12/17/18/19/45` 手动复位 Event 和等待，以及 `svc.0x1a/1b` 互斥字仲裁。 |

`SWR_NATIVE_APIS=svc.0x1e=native,ipc.time.time.300=legacy` 精确覆盖类别设置。四条文件命令构成同一个资源生命周期，必须选择一致的路由。如果配置含未知或重复的 ID，或将文件生命周期拆给两个后端，Runtime 会在加载模块前退出并记录配置错误。

文件系统的可写打开、目录、存档与 RomFS 沿用原处理。已经打开的 Native 文件始终由同一资源所有者读取和关闭，写入或改大小返回只读模式错误。其他 Event 模式需要后续语义验证。成功创建的 guest 线程沿用现有诊断对象寿命。

Linux 文件路径使用 `openat2` 在目录句柄下解析，拒绝符号链接和跨挂载跳转。确认 inode 是普通文件后，通过 `/proc/self/fd` 打开该 inode。缺少相应内核能力或 procfs 时返回 guest 错误。Windows 从 `CreateFileW` 打开的本地卷获取目录句柄，用 `NtCreateFile` 做拒绝 reparse 的相对打开，再验证返回句柄位于根目录下。同步文件句柄的定位和读取在同一把锁内执行。[Linux 路径解析标志](https://github.com/torvalds/linux/blob/master/include/uapi/linux/openat2.h) · [Windows 对象属性](https://learn.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-_object_attributes) · [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)

`SWR_NATIVE_TRACE=1` 可独立开启。每条已分派的调用有对应的 `begin` 和 `end` 记录。SVC 区分结果码、直接返回值、无结果和退出，IPC 同时记录传输与服务结果码。正常关闭时的 `trace_summary` 统计未结束调用、写入失败及截断标识。标准错误流中以 `{"schema_version":` 开头的行是 JSON 记录，Runtime 的 `.log` 会加时间前缀。

## 重跑验证

需要 Python 3.9 或更高版本及目标 GCC。Linux 建议以普通用户运行，使文件权限用例覆盖 `EACCES`。Windows 使用 MinGW-w64 GCC 和本地 NTFS，关闭可选 Vulkan 链接。ABI 1 集成样本使用仓库原创的 `guest.S`，可用 GNU AArch64 汇编器或 LLVM Clang 生成。

Linux

```sh
python3 switchrecomp.py --build-runtime --target linux --backend headless --cc gcc
sh tests/nha/run.sh --sanitize --output bin/nha-phase2-linux --runtime bin/runtime_linux_headless
```

原生 Windows PowerShell

```powershell
$env:SWR_NO_SNXVK = '1'
python switchrecomp.py --build-runtime --target win64 --backend headless --cc gcc
.\tests\nha\run.ps1 --output bin/nha-phase2-win64 --runtime bin/runtime_win64_headless.exe
```

两条测试命令会构建宿主契约测试，生成 NRO，并通过固定的 `713c861` 入口生成 ABI 1 模块。输出目录需要尚未创建。`fixture-manifest.json` 保存源文件、NRO、模块和初始文件的 SHA-256、编译命令、固定种子及每路的 24 个观测字段。验证脚本在基线、默认 Legacy、完整 Native 和混合配置中使用同一个模块，检查 guest 结果、输入树及 trace 配对。`host-results.json` 保存各宿主用例的退出码、故障 trace 和资源计数。原始日志保存在同一目录。

只运行宿主契约测试时可省略 `--runtime`。仅交叉编译时可加 `--compile-only` 并指定 `--target win64 --cc x86_64-w64-mingw32-gcc`。Linux 的 `test_fs` 在 root 下会将实际权限拒绝记录为跳过，ASan/UBSan 运行仍会验证注入的拒绝访问和文件资源寿命。原生 Windows 应从本地 NTFS 运行，临时目录也应位于本地 NTFS，以便测量实际目录联接、分享冲突及进程句柄计数。

每个已接入接口的期望、源码和测试索引见 [Phase 2 接口契约](CONTRACTS.md)。
