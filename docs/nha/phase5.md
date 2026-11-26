# Phase 5 发布前审计

审计日期为 2026-11-26。范围为第四阶段留存的 Linux aarch64 与 Windows x86-64 内部测试制品。**本批制品无可放行项。** 四个 Runtime 均报告 `v190 test`。Linux GUI 包缺字体和第三方许可材料，Windows 包缺静态依赖的完整许可和对应源码材料。原生 Windows、部分 GPU 回归以及启动画面和历史导入的人工复核仍待完成。 [Phase 4 验证](phase4.md) · [制品 SBOM（CycloneDX 1.6）](phase5.sbom.cdx.json)

## 制品、版本与对应源码

| 制品 | 实测文件数及压缩体积 | 核对结果 |
| --- | ---: | --- |
| `linux-nha-test.tar.gz` | 19，1,569,995 字节 | 两个 Runtime、GUI、启动脚本、三个 SDL 动态库及 12 项仓库文件。内含顶层 License、NOTICE、README、变更日志和 Python 脚本。缺 Runtime/GUI 构建源码、字体和第三方通知。 |
| `windows-nha-test.zip` | 329，4,883,744 字节 | 两个 Runtime、剥离调试符号的 GUI 和 326 项仓库文件。仓库文件逐项比对与 `dcd2c9f` 一致，含构建脚本、第三方源码通知和字体。缺实际链接的 SDL 依赖包通知及对应源码。 |
| 同批历史测试包 | SBOM 记录 6 份归档 | 早期未剥离的两个 Windows ZIP 各为 20,677,589 字节，超过 20 MB。其他历史包也排除在发布候选外，部分使用过渡版 Runtime 或旧源码。 |
| Win64 win32 + SNXVK | 无实际制品 | 第四阶段以 `SWR_NO_SNXVK=1` 构建。发布审查需要含实际静态库的制品、链接映射和对应源码。 |
| Linux AppImage | 无实际制品 | 审计对象是 Linux 测试 tar.gz。AppImage 需提供其实际 SquashFS、启动脚本和内含系统库清单。 |

两个最终测试包均低于 20 MB。四个解包后的 Runtime 与 `bin/`、Phase 4 的 `final/SHA256SUMS` 和验收矩阵的 Runtime 哈希一致。四份 guest 启动日志和四个二进制里的版本字符串均为 `v190 test`。GUI 窗口标题中的 `v190` 不满足 Runtime 日志的版本门槛。 [Level 0 规则](../../REGRESSION.md#level-0-build) · [Runtime 版本宏和日志](../../runtime/main.c)

用 Phase 4 保存的构建容器文件系统核对依赖并重新编译。以 `git archive dcd2c9f` 取源码，Linux x11/headless 使用 Debian 12 aarch64 GCC 12.2.0，Win64 win32/headless 使用 MinGW-w64 GCC 12-posix。使用仓库构建入口和 `SWR_NO_SNXVK=1`，记录 GNU ld 的 `-Map` 链接映射，并按原制品设置构建日期及 PE 时间戳。两个 GUI 使用各自的构建脚本，Windows GUI 在重建后以 `x86_64-w64-mingw32-strip --strip-debug` 剥离。**重建的六个二进制与最终包逐字节一致。** SBOM 记录每个文件的 SHA-256、ELF 依赖或 PE 导入表、实际抽取成员的静态归档和链接映射哈希。

Phase 4 的 Runtime 输入为 `a3dc903` 加 `runtime-final.patch`。补丁 SHA-256 为 `5e96dc3d19ce3dd9ad60b920d81af8d567f473438fd3a2f48cd50a68d086e5a9`，其稳定 patch ID 与 `git diff a3dc903 dcd2c9f -- runtime` 一致。已生成主项目源码快照，包含对应的构建脚本。配套 SDL 源包、编译库中实际使用的第三方源码、适用的发布组装脚本及可取得这些材料的发布渠道记录仍待归档。 [Runtime 构建入口](../../switchrecomp.py) · [Linux GUI 构建脚本](../../tool/build_tool_linux.sh) · [Windows GUI 构建脚本](../../tool/build_tool.sh)

### 修改文件与实现来源

以 `713c861` 为 NHA 审计基线。至 `dcd2c9f` 共新增 25 个文件、修改 19 个文件。以下分组覆盖该差异，完整路径可用 `git diff --name-status 713c861 dcd2c9f` 复查。

| 文件 | 实现和证据 |
| --- | --- |
| `runtime/native_hos.{c,h}`、`kernel.c`、`ipc.c`、`main.c`、`rt.h`、`mem.c` | 项目内 NHA 的调度、trace、SVC/IPC 接入及状态管理，复用基线 ABI 1、句柄、线程与 guest 内存模型。 |
| `runtime/{plat.c,svc_core.c,svc_fs.c}` | 复用项目平台层和服务对象，Native 路径调用 Win32/POSIX 原语。兼容差异通过自有测试与基线对照，接口来源见契约和清单。 |
| `runtime/{audio.c,audio.h,backend_headless.c,backend_x11.c,backend_win32.c}`、`switchrecomp.py` | 现有设备后端、退出路径与构建入口的集成。 |
| `tests/nha/*`、`tests/regress/{clear_present.c,cmpreg.py,regrun.sh,test_regress.py}`、`tests/test_audio.c` | 自有无 SDK 的汇编夹具、C 故障注入、测试驱动与报告。GPU 样本 C 源码自有，生成 NRO 时使用 libnx 和 deko3d。 |
| `docs/nha/{README.md,interfaces.json,phase4.md}`、`docs/superpowers/plans/2026-11-18-native-horizon-adapter*.md`、`REGRESSION.md`、`CHANGELOG_v190.txt` | 接口盘点、审计、回归证据和英文修改记录。 |

基线至该修订的 License、NOTICE、OFL 和 PATENTS 文件无变更。两个新增测试启动脚本 `tests/nha/run.sh`、`tests/nha/run.ps1` 缺少 Level 0 所要求的 MZ Labs 文件头。变更记录保留在英文 changelog，发布时需核对适用文件的修改声明和日期。 [接口契约](../../tests/nha/CONTRACTS.md) · [接口来源清单](interfaces.json) · [英文变更记录](../../CHANGELOG_v190.txt)

## 实际链接与许可材料

顶层 `LICENSE.txt`、`NOTICE.txt` 与仓库逐字节相同。两包的 `README.txt` 含可见的 “SwitchRecomp by MZ Labs” 署名。NHA 改动沿用项目 AGPL-3.0，基线中的 Mythrax-ZS 修复仍按顶层 NOTICE 保留 Apache-2.0 声明。相关修复的逐项原补丁与 Apache 通知保留范围仍待溯源。 [LICENSE](../../LICENSE.txt) · [NOTICE](../../NOTICE.txt) · [README](../../README.txt)

| 范围 | 实际依赖与检查结果 |
| --- | --- |
| OpenGL Runtime | 两个 GL 目标均编入 BCn 解码代码，保留 texture2ddecoder、FP16、endianness 的 MIT 授权。Windows 包有相关源码文本，Linux 包缺这些通知。Linux x11 依赖宿主 libX11、libGL、glibc，Phase 4 的 Mesa 驱动属于宿主环境。 |
| Headless Runtime | Linux ELF 只直接依赖宿主 libm、libc，音频经 `dlopen` 探测 SDL2。Windows headless 的 PE 导入含 `OPENGL32.dll`，实际使用 `gpu_nogl.c`，构建未编入桌面 GL 渲染路径。Windows 两个 Runtime 均静态抽取了 MinGW CRT、winpthreads 和 GCC 运行库成员，且内含 Hind 字体子集。 |
| Linux GUI | 三份包内 SDL .so 与构建环境的 Debian arm64 文件逐字节一致。版本分别为 SDL2 `2.26.5+dfsg-1`、SDL2_image `2.6.3+dfsg-1`、SDL2_ttf `2.20.1+dfsg-2`。对应 Debian `copyright` 的 SHA-256 在 SBOM，包未带这些文件。ELF 依赖还指向宿主 FreeType、HarfBuzz、PNG、JPEG、TIFF、WebP 及 SDL2 的平台库，其实际 SONAME 在 SBOM。 |
| Windows GUI | 静态链接 SDL2 `2.30.9`、SDL2_image `2.8.2`、SDL2_ttf `2.22.0`，开发包归档及实际 `.a` 哈希在 SBOM。链接映射证实 SDL2_ttf 包含 FreeType、HarfBuzz 成员，PE 版本函数验证为 `2.13.2` 和 `8.1.1`。SDL_image 的 AVIF/TIFF/WebP 兼容路径含可选动态库名，归档无这些 DLL。内含 MinGW CRT 与 GCC 运行库成员、外部 Hind 和 Barlow 字体文件，缺 SDL 全套许可、实链成员相关的第三方通知与对应源码材料。 [SDL2](https://github.com/libsdl-org/SDL/releases/tag/release-2.30.9) · [SDL2_image](https://github.com/libsdl-org/SDL_image/releases/tag/release-2.8.2) · [SDL2_ttf](https://github.com/libsdl-org/SDL_ttf/releases/tag/release-2.22.0) |
| 两个 GUI | 编入 dr_wav `0.14.6`、dr_mp3 `0.7.4`、stb_vorbis `1.22`。许可在仓库原文件，Windows 包内有这些文件，Linux 包缺相应文本。 [解码器源码和许可](../../tool/audio_decode/) |
| 可选静态库 | 四个 Runtime 的实际链接参数和映射均无 OpenH264、Opus、libvpx、SPIRV-Cross 或 SNXVK。Windows 源码包内有对应的入库头文件或源码、以及跟踪的许可证和 libvpx PATENTS。启用任一库时需按实际归档哈希、版本、链接成员、源码和授权重新生成 SBOM。 [构建条件](../../switchrecomp.py) |

### `LEGAL_REVIEW_REQUIRED` 混合许可与二进制制品

`runtime/snxvk/README.txt` 指向 ScarletNX `b5f4f14f2bfc` 与本地移植，并说明 Eden/yuzu 来源。源码目录保留 GPL、历史 Apache-2.0 文本及 NOTICE，源文件中还有 GPL-2.0-or-later 和 GPL-3.0-or-later 标识。Vulkan-Headers 1.4.365、volk 1.4.363、VMA 3.4.0 和 RenderDoc header 来自构建脚本及源码声明。本轮无 `libsnxvk.a`，无法将这些声明与一个已链接的制品及其具体依赖 revision 配对。Windows ZIP 实际携带 SNXVK 源码，历史 NOTICE 的 Apache 声明仍须逐文件核定范围。 [SNXVK 来源](../../runtime/snxvk/README.txt) · [构建脚本](../../runtime/snxvk/build_lib.py) · [历史 NOTICE](../../runtime/snxvk/NOTICE)

AGPLv3 和 GPLv3 的第 13 节允许组合并保留各自部分的适用条款。组合发布仍需核对覆盖范围、完整对应源码、生成 SPIR-V 所需源码、依赖和构建脚本，并按实际分发方式确认适用的源码提供和署名义务。项目顶层 NOTICE 及其附加条款应与 SNXVK 的来源记录一并复核。发布阻断覆盖含 SNXVK 的二进制，以及将其源码带入候选包时尚未确认的来源文件。 [GNU AGPLv3](https://www.gnu.org/licenses/agpl-3.0.html) · [GNU GPLv3](https://www.gnu.org/licenses/gpl-3.0.html) · [FSF 对对应源码的说明](https://www.gnu.org/licenses/gpl-faq.html#AGPLv3CorrespondingSource)

### `LEGAL_REVIEW_REQUIRED` 启动画面与字体

- GUI 由 `tool/main.c` 调用启动画面，两个桌面 GL Runtime 由 `gpu_gl_start` 调用启动画面。两个 `splash_img.h` 与基线一致。最终包的 Linux x11 Runtime 在 Xvfb 下运行自有 GPU 样本，实拍黑底标志与 MZ Labs 署名清晰可读。截图保存在 `bin/nha-phase5/splash/startup-x11.png`，SHA-256 为 `f8112b8957bd79682aa680d92d8d7cdc26f7e2057361e687928c7a1d00a6f93d`。
- 解包 Linux 测试包后运行随包启动脚本，GUI 退出码为 1，报 `tool/fonts missing?`。所有 4 个必需字体文件及两份 OFL 文本都缺失，启动画面位于字体初始化成功之后。Windows 包内四个 TTF 与其 OFL 文件均与仓库一致，GUI 尚缺原生 Windows 启动实测。
- Windows 两个 Runtime 中检出与 `runtime/swkbd_font.h` 一致的 19,692 字节 Hind 2.001 子集。它保留 Indian Type Foundry 版权字段和 OFL 链接。子集生成来源、相应许可通知和名称使用仍需复核。
- Headless 无可见启动窗口，需对其分发方式核定 NOTICE 的画面条款。Win32 在 Vulkan 成功启动后会跳过 GL 初始化。待有实际 Vulkan 制品时验证该路径能显示要求的画面。 [顶层 NOTICE](../../NOTICE.txt) · [GUI 启动](../../tool/main.c#L1892) · [OpenGL 启动](../../runtime/gpu_gl.c#L3435) · [Win32 后端](../../runtime/backend_win32.c#L318)

### `LEGAL_REVIEW_REQUIRED` 外部资源、样本与 trace

- 检查了 326 项仓库跟踪文件、源码快照及两份最终归档的文件类型、路径、内容归属与常见密钥标记。归档没有 GCC/Python 目录、缓存、原始游戏/固件、密钥文件、编译后的 guest 样本、外部 SDK/DLL、捕获或 trace。源码命中为 README 的文件名说明、已标记的第三方接口声明、运行时加载逻辑以及源码内嵌字体与 shader 资源。未见常见私钥块或作为发布输入的 DRM 绕过工具。文件扫描不构成来源或授权证明。 [仓库范围](../../README.md) · [安装输入](../../tool/get_deps.py)
- Linux 包内可执行内容限定为三个本项目 ELF、三个已匹配 Debian 的 SDL .so 和项目启动脚本。Windows 包内额外编译文件只有三个 PE。其中 `lsfg.c` 和 `dlss.c` 作为源码出现，Lossless.dll、NGX/DLSS DLL、对应 SDK 及用户 shader 缓存均未随包分发。发布新功能组合前需复核运行时读取资源的条款、历史移植来源和对应 shader 的源码/生成链，尤其是 `runtime/snxvk/src/nx/gpu/` 的内嵌 SPIR-V。 [LSFG 加载路径](../../runtime/lsfg.c) · [DLSS 加载路径](../../runtime/snxvk/src/nx/gpu/vk/dlss.c)
- ABI 1 用例来自自有 `guest.S` 与 `make_guest.py`，初始文件是生成的 NRO 和测试文本。Phase 4 两份模块及原始 trace 仅作测试证据。重新生成四份 trace 报告，每个目标 17 个记录路由，各含 65 次已配对调用。Wine Win32 的 `0x1a`、`0x1b` 两条运行因进程异常退出仍标为 `sample_exit_unverified`。NHA trace 块记录 ID、线程、PC、结果、错误和耗时，普通 Runtime 日志还可能包含路径。审计交付仅引用汇总与哈希，原始日志保存在本地测试目录。 [样本源码](../../tests/nha/guest.S) · [trace 实现](../../runtime/native_hos.c) · [Phase 4 矩阵](phase4.md#abi-1-与路由对照)
- GPU 样本 `clear_present.c` 为自有源码。测试 NRO 由 devkitA64 r29.2、libnx 4.12.0 和 deko3d 0.5.0 构建，NRO SHA-256 为 `4135249f7c63fbd8291e9f75a9b8d0465428f2f1c7bcc187fb53c68b9a6b1b6b`。原始 NRO、输出模块、截图及本地下载的历史示例均排除在发布清单外。若再分发这些材料，需分别核实随样本静态链接的源码、许可和授权。 [样本与构建记录](phase4.md#level-2-gpu-样本) · [libnx 4.12.0](https://github.com/switchbrew/libnx/tree/v4.12.0) · [deko3d 许可](https://github.com/devkitPro/deko3d/blob/master/LICENSE)

## 放行记录

| 对象 | 状态 | 关闭条件 |
| --- | --- | --- |
| 所有 Runtime | 阻断 | 以实际发布配置生成日志为 `v190` 的 Runtime，重跑适用回归并更新哈希和对应源码。补齐 Level 0 的新增脚本文件头及适用的修改声明。 |
| Linux x11/headless 与 GUI 包 | 阻断 | 补齐 GUI 字体/OFL、实际分发的 SDL 等第三方通知、图形/音频源码许可、对应源码和包组装记录。用最终候选包复跑 x11 启动画面检查，验证 GUI 启动及适用的 headless 署名。 |
| Windows OpenGL/headless 与 GUI 包 | 阻断 | 核实静态 SDL 及 FreeType/HarfBuzz、MinGW/GCC 运行库的实际来源、对应源码和通知。原生 Windows 重跑 GUI/启动画面、两 Runtime、音频、VSync、句柄和 NTFS 文件契约。核对 ZIP 内历史导入源码的许可范围。 |
| Windows SNXVK | 阻断且无候选制品 | 提交真实链接清单和锁定来源，复核 AGPL/GPL/Apache、生成器及 shader，验证 Native Vulkan 启动画面、驱动和回归。 |
| 尚缺的回归场景 | 待取得证据 | 历史 17 个 GPU 样本、Level 3 的授权场景、Level 4 的 Intel Windows 设备均待执行适用用例。现有 Phase 4 证据覆盖 1 个自有 GPU 样本和 Linux 运行。Wine Win32 有 2 个进程异常及 VSync 超时。 |

## 复查入口

在仓库根目录执行。原始审计工作目录为本地忽略的 `bin/nha-phase5/`，保留重建的链接命令、映射、SHA-256、源码快照和重算的 trace 汇总。其他工作区可按下列命令复查制品，先取得 SBOM 标识的同一归档与构建输入。

```sh
git diff --name-status 713c861 dcd2c9f
mkdir -p bin/nha-phase5
git archive --format=tar --prefix=SwitchRecomp/ dcd2c9f | gzip -n > bin/nha-phase5/switchrecomp-dcd2c9f-source.tar.gz
sha256sum bin/nha-phase4/packages-final/*-nha-test.*
(cd bin/nha-phase4/final && sha256sum -c SHA256SUMS)
tar -tzf bin/nha-phase4/packages-final/linux-nha-test.tar.gz
unzip -Z1 bin/nha-phase4/packages-final/windows-nha-test.zip
```

对解包制品用 `readelf -d` 检查 ELF NEEDED，用 `x86_64-w64-mingw32-objdump -p` 检查 PE 导入，使用编译器包装脚本给链接命令追加 `-Wl,-Map,FILE`，按 map 的归档成员提取记录复核静态依赖。检查 Linux GUI 的实际包时，在解包目录用 `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./SwitchRecomp` 复现字体错误。SBOM 采用 CycloneDX 1.6，完整度标为 `incomplete`，待审的依赖来源与外部输入可由上述阻断记录定位。 [CycloneDX 1.6 JSON 规范](https://cyclonedx.org/docs/1.6/json/)

本轮在 Linux 构建环境复跑报告测试 10 项、GPU 回归驱动测试 7 项及音频/录制时钟测试 16 项，全部通过。音频/录制时钟运行无 ASan/UBSan 报告。

## v190 候选制品复核

本轮在当前分支补齐可在本地重建和验证的项目。通过 `--build-runtime --release` 生成正式版本标识，保留普通开发构建的 `v190 test`。补齐两个测试入口的 MZ Labs 文件头，新增可重复组装的 OpenGL/headless 打包命令。打包读取 GNU ld 链接映射，遇到 SNXVK 或其他可选静态归档会拒绝这一配置。 [构建入口](../../switchrecomp.py) · [打包入口](../../tool/package_release.py) · [测试](../../tests/test_release.py) · [候选制品 SBOM](phase5-release.sbom.cdx.json)

| 候选制品 | 文件数及压缩体积 | 本轮实际收录 |
| --- | ---: | --- |
| `SwitchRecomp-v190-linux-aarch64.tar.gz` | 32，1,022,826 字节 | 两个 v190 Runtime、Linux GUI、四个 TTF 及配套 OFL、GCC/glibc 通知和对应源码索引。GUI 使用宿主 SDL2、SDL2_image、SDL2_ttf。 |
| `SwitchRecomp-v190-win64.zip` | 37，3,235,926 字节 | 两个 v190 Runtime、剥离调试信息的 Windows GUI、字体与 OFL、GCC/MinGW-w64、SDL2、SDL2_image、SDL2_ttf、FreeType、HarfBuzz 通知和对应源码索引。 |
| `SwitchRecomp-v190-project-source.tar.gz` | 独立源码归档 | 两包的对应项目源码与构建、打包脚本。候选使用 SNXVK/LSFG stub，归档范围随实际链接模块确定。 |

本地证据位于 `bin/nha-phase5/remaining/`。两套解包文件清单和 SHA-256 可由候选 SBOM 复核。四个 Runtime 从包内取出后启动，均报告 `v190`。编译器仍使用 Phase 4 的 Debian 12 GCC 12.2.0/MinGW-w64 GCC 12-posix，`SWR_NO_SNXVK=1`。Windows GUI 的三个 SDL 开发归档和实链静态库哈希与上一轮相同。从最终源码归档重新构建后，六个二进制与包内文件逐字节一致。两个候选包分别重复组装，归档也逐字节一致。新的链接映射记录与候选二进制对应。 [候选制品 SBOM](phase5-release.sbom.cdx.json)

两个包的 `SOURCE.txt` 记录项目快照及精确版本的第三方源包和 SHA-256。已取得 GCC 12.2.0、MinGW-w64 与 glibc 对应 Debian 源文件，按 `.dsc` 记录核对成员哈希。Windows GUI 的对应 SDL 源码来自上游发布归档，SDL2_ttf 源包同时提供 FreeType 2.13.2 和 HarfBuzz 8.1.1。通过链接映射核对 SDL_image 的 stb_image、QOI、NanoSVG 成员与 FreeType 的 gzip 成员，把随代码的许可纳入候选通知。完整素材仍需在正式分发渠道与相同二进制一起提供。 [SDL2 源码](https://www.libsdl.org/release/) · [SDL_image 源码](https://www.libsdl.org/projects/SDL_image/release/) · [SDL_ttf 源码](https://www.libsdl.org/projects/SDL_ttf/release/) · [GCC 例外](https://www.gnu.org/licenses/gcc-exception-3.1.html)

| 验证 | 本轮结果 |
| --- | --- |
| Linux GUI | 从候选包解压后用 Xvfb 运行。实拍 MZ Labs 启动画面，进入主菜单，窗口关闭返回 0。 |
| Linux ABI 1 | Headless 19/19 路退出 0 并通过断言。X11 首轮候选 Runtime 的 18 路全部通过，旧基线在客体断言通过后退出 1。限制 Mesa 工作线程后，X11 复跑 19/19 通过。 |
| Linux GPU | X11 基线、Legacy、Native 分别 52、64、50 帧，三路退出 0，两次截图对照各 1 同 0 异。Headless 输出 12 帧和哈希 `4b524a17b95b6f83`。 |
| Linux 接口契约 | 普通用户在 ASan/UBSan 下 6 个宿主用例、12 个分派配置和 4 路宿主/客体对照通过。 |
| Windows 兼容层 | Wine 首轮将样本放在绑定目录。本地 `C:` 盘复跑后 headless 19/19 通过，Win32 14/19 通过。后者五次异常发生在客体断言之后，日志含 x86 转译层的 `rt_sigreturn` 断言。 |
| 源码和包复验 | 5 个发布工具用例、10 个报告测试、7 个 GPU 回归驱动测试、16 个音频/录制时钟用例通过。候选包及项目源码快照只含选定文件。 |

组包例子中的目录均位于 `bin/nha-phase5/remaining/`，其中 `build/` 保存记录制品链接时的 GNU ld map，`notices/` 保存按平台归集的文本。

```sh
out=bin/nha-phase5/remaining
python3 tool/package_release.py --target linux --binaries "$out/binaries" \
  --link-maps "$out/build" --notices "$out/notices/linux" \
  --source-index "$out/SOURCE-linux.txt" --output "$out/SwitchRecomp-v190-linux-aarch64.tar.gz"
python3 tool/package_release.py --target win64 --binaries "$out/binaries" \
  --link-maps "$out/build" --notices "$out/notices/win64" \
  --source-index "$out/SOURCE-win64.txt" --output "$out/SwitchRecomp-v190-win64.zip"
```

**放行状态仍为阻断。** 正式版本、Linux GUI 字体和候选包内许可材料已补齐。原生 Windows GUI/Runtime、VSync、音频、NTFS 及 Intel Windows 回归仍需实机或虚拟机证据。历史 GPU 样本及商业场景需取得可测试授权。适用的历史导入、嵌入字体来源和 headless 署名条款仍需人工复核。Vulkan 未进入两个候选包，其实际链接制品和对应来源继续按 Phase 0 风险登记处理。发布时还需核对源码同等可取得性及实际分发清单。
