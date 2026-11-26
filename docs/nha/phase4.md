# Phase 4 Runtime 集成与回归

本轮于 2026-11-18 开始，2026-11-26 复跑。基线 Runtime 为 `713c861`，当前构建为 `a3dc903` 加本阶段 Runtime 补丁。构建和 Linux 运行使用 Apple Silicon 上的 Docker Linux aarch64、Debian 12 和 GCC 12.2.0。X11 使用 Xvfb 和 Mesa 22.3.6 llvmpipe。Windows 使用 MinGW-w64 GCC 12-posix 交叉编译，构建时设置 `SWR_NO_SNXVK=1`。

## Level 0 和 Level 1

| 检查 | 结果 |
| --- | --- |
| Linux x11 与 headless Runtime | 两次命令各退出 0 |
| Windows win32 与 headless Runtime | 两次交叉构建各退出 0 |
| 两个 GUI | Linux 和 Windows 构建各退出 0 |
| 包内容 | Linux 内部测试包约 1.57 MB。Windows GUI 的未剥离测试 ZIP 约 20.68 MB，剥离 GUI 调试符号后约 4.88 MB。解包后四个 Runtime 与 `bin/` 中对应文件的 MD5 一致，排除项检查通过。 |
| 发布版本 | 运行日志报 `v190 test`。正式 `v190` 包验收待发布阶段。 |
| 音频与录制时钟 | `sh tests/run_audio.sh` 退出 0，16 条 PASS，ASan/UBSan 无报告。SDL dummy device 提交通过。 |
| Phase 2 契约 | 普通用户运行 Linux ASan/UBSan。6 个宿主用例、12 个分派配置及四路 ABI 1 guest 对照通过。 |

本地原始输出保存在 `bin/nha-phase4/final/`、`level0/`、`packages-nha.log` 和 `phase2-linux-final/`。包检查使用本地组装的测试包，Linux 包使用容器内的 SDL 2、SDL_image 和 SDL_ttf，Windows 包剥离命令为 `x86_64-w64-mingw32-strip --strip-debug SwitchRecomp.exe`。 [Level 0 构建命令](../../REGRESSION.md#level-0-build)

## ABI 1 与路由对照

直接复用 Phase 1 保存的 SDK-free 原创模块和初始数据。Linux 模块 SHA-256 为 `efc3122789ee791cee6fd99df7dbe441a414d49b8d7769e614f999f905172f08`，Windows 为 `f229bdd67a22db6059393337a5b84994d2537c87e80b1bafe886e0e34012ae86`。每个矩阵包含原始基线、缺省配置、仅开启 trace 的缺省路由、完整 Native、14 个单接口覆盖和 1 个只读文件生命周期覆盖。打开、读、查大小和关闭共用一个切换。

| 目标 | 进程与 ABI 1 断言 | Trace |
| --- | --- | --- |
| Linux x11 | 设置 `LP_NUM_THREADS=2` 复跑 19/19 退出 0，全部通过 guest 断言 | 17 路，各 65 次完整调用 |
| Linux headless | 最终 19/19 退出 0，全部通过 guest 断言 | 17 路，各 65 次完整调用 |
| Windows headless，Wine 8.0 | 19/19 退出 0，全部通过 guest 断言 | 17 路，各 65 次完整调用 |
| Windows win32，Wine 8.0 | 严格复跑 17/19 退出 0，缺省和完整 Native 两路通过 | 17 路各有 65 次完整调用。`0x1a`、`0x1b` 两条单项运行在 guest 断言完成后分别收到宿主信号 9 和 5。后者同时记录了 x86 转译层的 `rt_sigreturn` 断言。 |

四目标的缺省路由均为 Legacy，完整 Native 覆盖 18 个接口和 23 次 Native 调用，逐项设置的 Native 命中符合预期。Trace 按接口与 guest 调用点比较调用数、结果类型、传输 RC、服务 RC、等待结果、终止状态及宿主错误。时钟的绝对 tick 与新建句柄数值会变化，相应的时间顺序、事件等待、句柄使用和关闭由 guest 断言验证。Phase 2 样本另覆盖重复启动的已知 RC 差异，Native 返回 `0xFA01`，Legacy 返回 `0`。所列矩阵的 trace 汇总无丢失、截断或未配对。

初次 X11 矩阵中 15 个当前版进程在 guest 断言通过后退出 1。GDB 在 Mesa 的 `swrast_dri.so` 查到并行 LLVM 编译崩溃，guest 线程同时进入 libc `exit()` 执行驱动清理。修复后 Linux guest 退出显式关闭音频，写完 trace 后调用 `_exit()`。原始基线在默认 Mesa 线程配置下仍偶发退出 1 或 SIGABRT。限制 Mesa 工作线程后，复跑的基线及当前版均退出 0。ABI 矩阵要求由 guest 退出，并校验窗口后端的 OpenGL 启动记录。窗口后端的 `--seconds` 截图退出现在也会写 trace 汇总，进行中的调用会计入汇总。

结果文件位于 `bin/nha-phase4/` 下的 `repeat-linux-x11/`、`accepted-linux-headless/` 和 `accepted-wine-*/`。每路保留实测退出码、环境、运行时间、模块和 Runtime 哈希、trace 哈希及校验结果。X11 故障证据保存在 `linux-x11/`、`linux-x11-segv.log` 和 `accepted-linux-x11/`。Wine Win32 的较早运行也在不同路由录得宿主信号 5、9 或 11，均保留了 trace 和退出状态。进程耗时包含设备初始化、宿主调度和文件系统开销，当前样本仅核对时钟契约。

### 重跑 ABI 1 矩阵

将 `SAVED` 指向对应平台保存的 Phase 1 目录，它应包含 `original/` 和 `fixture-manifest.json`。Linux X11 示例使用同一个基线模块和两份 x11 Runtime。headless 可省略 `xvfb-run`。Windows 在原生 PowerShell 上执行同一 Python 脚本，使用 `--backend win32` 或 `--backend win64-headless`，指定对应的 `.exe` 和保存的 Windows 样本。Windows 原生定向文件契约需另在 NTFS 上通过 Phase 2 runner 执行。 [Phase 2 命令](../../tests/nha/README.md)

```sh
SWR_NO_SNXVK=1 python3 switchrecomp.py --build-runtime --target linux --backend x11 --cc gcc
SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 LP_NUM_THREADS=2 xvfb-run -a \
  python3 tests/nha/compare_runtime.py --saved "$SAVED" \
  --runtime bin/runtime_linux_x11 --baseline-runtime "$BASE_X11" \
  --backend x11 --compiler "$(gcc --version | head -1)" \
  --runtime-commit "$(git rev-parse HEAD)" --output bin/nha-phase4-x11
python3 tests/nha/report.py --manifest bin/nha-phase4-x11/fixture-manifest.json \
  --output bin/nha-phase4-x11/report.json
```

基线 Runtime 从 `git archive 713c861` 解包的源码按相同目标构建。如果构建含未提交的 Runtime 补丁，将补丁文件用 `--runtime-patch` 传入以记录 SHA-256。Wine 补充验证可用 `--launcher /usr/lib/wine/wine64`。

## Level 2 GPU 样本

本轮重建并实测 1 个样本 `clear_present`。它由仓库原创 C 源码生成，在 libnx 的 VI 事件上等待 VSync，通过 deko3d 提交两块颜色并展示。未使用商业素材或第三方示例代码。构建使用 devkitA64 r29.2、libnx 4.12.0 和 deko3d 0.5.0，来源为 devkitPro 容器 `devkitpro/devkita64:20260219`。 [源码](../../tests/regress/clear_present.c) · [容器](https://hub.docker.com/r/devkitpro/devkita64) · [deko3d 许可证](https://github.com/devkitPro/deko3d/blob/master/LICENSE) · [libnx 源码和许可证](https://github.com/switchbrew/libnx/tree/v4.12.0)

| 运行 | 退出状态 | 画面或同步证据 |
| --- | --- | --- |
| X11 基线 / 缺省 / 完整 Native | 0 / 0 / 0 | 严格复跑为 70 / 70 / 71 帧。三张 640×360 截图 SHA-256 相同。两次 `cmpreg.py` 均为 `1 same, 0 different`。 |
| Headless 基线 / 缺省 / 完整 Native | 0 / 0 / 0 | 13 / 12 / 12 帧，均为 640×360。达到指定的 12 帧后，下一次前端轮询退出。三个帧哈希均为 `4b524a17b95b6f83`。 |
| Win32，Wine 8.0，基线 / 缺省 / 完整 Native | 原始退出码 0 / 0 / 0，三路判失败 | 21 / 21 / 17 帧并留下相同截图。三路均先完成初始展示，随后等待 VSync 超时并记录 guest fatal `0xEA01`。限时截图仍结束了进程。 |

X11 截图的角落 RGB 为 `32,96,191`，中心为 `223,159,64`，确认测试比较的是已渲染的色块。三路均输出 VSync 与 GPU 展示成功标记。X11 完整 Native 的限时退出汇总有 900 次开始、898 次完成、2 次进行中、0 丢失和 0 截断。较早的 X11 运行记录了 53 / 67 / 64 帧，固定的三秒窗口受软件渲染与并行任务调度影响。Headless 记录帧计数和软件前端证据。原始截图、日志和退出码在 `bin/nha-phase4/gpu/`。

构建 NRO 时设定 devkitPro 环境，然后用基线 SwitchRecomp 生成同一个 Linux 模块。

```sh
mkdir -p bin/nha-gpu
"$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc" -O2 -Wall -Wextra -fPIE \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -D__SWITCH__ \
  -ffunction-sections -fdata-sections -specs="$DEVKITPRO/libnx/switch.specs" \
  -I"$DEVKITPRO/libnx/include" tests/regress/clear_present.c -o bin/nha-gpu/clear_present.elf \
  -L"$DEVKITPRO/libnx/lib" -Wl,--gc-sections -ldeko3d -lnx -lstdc++
elf2nro bin/nha-gpu/clear_present.elf bin/nha-gpu/clear_present.nro
python3 "$BASE_SRC/switchrecomp.py" bin/nha-gpu/clear_present.nro \
  --target linux --backend x11 --cc gcc --jobs 2 -o clear_present \
  --outdir bin/nha-gpu/programs/clear_present
bash tests/regress/regrun.sh "$BASE_X11" base bin/nha-gpu/programs &&
bash tests/regress/regrun.sh bin/runtime_linux_x11 new bin/nha-gpu/programs &&
python3 tests/regress/cmpreg.py new base bin/nha-gpu/programs
```

完整 Native GPU 运行可给第二条 `regrun.sh` 加上 `SWR_NATIVE_HOS=1`、四个类别开关与 `SWR_NATIVE_TRACE=1`，使用独立的 `native` 标签。每路应报告 1 个样本和 0 次捕获失败。

## 待覆盖

- 原生 Windows 两目标及本地 NTFS 文件契约缺少 Windows 实机或虚拟机。Wine 两目标均记录 `waveOutOpen failed (2)`，客体通过了 ABI 1 路由断言，宿主音频输出待验证。Win32 的 VSync 超时也待在 Windows 上复核。
- 历史 v190 的 [17 个 GPU 样本](../../REGRESSION.md#level-2-homebrew-gpu-test-programs) 缺少可复用的授权输入，现有截图证据覆盖自有 clear/present 用例。
- Level 3 商业游戏场景缺少授权输入，Level 4 Intel Windows 缺少对应设备。Windows 可选 Vulkan 缺少实际链接库、驱动运行和对应源码的复核证据。保留了 `SWR_NO_SNXVK=1` 的 Windows 桌面基线，Vulkan 组合发布继续按 [Phase 0 许可矩阵](../superpowers/plans/2026-11-18-native-horizon-adapter-audit.md#许可依赖矩阵) 核对。
- Native 接入范围限于 [开关清单](../../tests/nha/README.md)。文件写入、目录、存档、RomFS、其他时间子服务、线程优先级与 affinity、地址/条件变量仲裁、VM 和设备 IPC 继续走既有路径。 [静态接口及未覆盖项](README.md)
