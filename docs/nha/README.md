# Native Horizon Adapter 接口依赖与 Runtime 最小化报告

四目标构建、ABI 1 路由矩阵和设备回归见 [Phase 4 集成验证](phase4.md)。

最终测试包的制品清单及发布阻断见 [Phase 5 发布前审计](phase5.md)。

[接口清单](interfaces.json) 固定于 Runtime 源码提交 `d7d26a9`。清单包含 58 个显式 SVC、78 个 IPC 注册名、1,233 个 IPC 命令、65 个 nvdrv IOCTL 分支，以及 IPC 控制、资源析构、Binder 和直接调用等入口，共 1,384 条。接口行为与错误码按条目中的源码或契约定位。仅能定位处理分支的条目带有 `insufficient_evidence`，具体字段和跨平台语义仍需验证。

IPC 名称采用 trace 的完整 `ipc.<family>.<interface>.<command>`。例如 `nfp:user` 与 `nfc:user` 都创建 `IUser`，两者保留独立 ID。由 Runtime 主动归一化的 `time:u`、`time:a`、`time:s` 都进入 `ipc.time.*`，原服务名仍可在注册表核对。动态会话、generic 兼容占位以及同一 ID 下的多个对象后端见 `dynamic_dependencies`。标记为 `legacy_stub` 的静态分支有明确的占位或无动作证据，客体协议的兼容性仍由各自的测试状态表示。

## 重跑

在普通用户的 Linux 环境执行。完整构建和 Windows 验证参数见 [NHA 测试说明](../../tests/nha/README.md)。

```sh
python3 tests/nha/test_report.py
python3 switchrecomp.py --build-runtime --target linux --backend headless --cc gcc
sh tests/nha/run.sh --sanitize --runtime bin/runtime_linux_headless \
  --runtime-commit "$(git rev-parse HEAD)" --output bin/nha-phase3-sample
python3 tests/nha/report.py --manifest bin/nha-phase3-sample/fixture-manifest.json \
  --output bin/nha-phase3-sample/report.json
```

`--runtime-commit` 声明实际构建传入 Runtime 的 Git 修订。报告默认读取样本清单旁的 `guest-<run>.log`，可用 `--trace native=PATH` 指定同一路的单独 trace，也可用 `--interfaces PATH` 指定接口清单。支持 stderr 的 JSON Lines 和带 Runtime 时间前缀的日志。接口清单、模块与 Runtime 哈希、样本平台、各路环境变量及 trace 哈希均写入报告。相同输入及日志文件名会生成逐字节相同的报告。新一轮运行的时间和原始日志哈希可以变化。

## 报告字段

| 字段 | 含义 |
| --- | --- |
| `runs.*.calls` | 配对成功并具有完整 ID 的调用，按完整 ID 和路由计数。保留错误、占位和无返回终止。IPC 传输 RC、服务 RC 和宿主错误单独统计。值返回保留类型，变动的 tick、句柄值及 guest 地址留在原 trace。 |
| `observed_relations` | 完整 trace 中，同一非零 guest 线程上，由 begin/end 恢复的直接嵌套。IPC 服务、析构可以是 SVC 的子调用。 |
| `static_relations` | 源码定位的调用、对象创建、资源生命周期及可能的分派目标。通过清单中的 `sources` 回查文件和符号。 |
| `native_modules`、`legacy_dependencies` | 该路样本的实测 Native 模块、共享依赖和仍走 Legacy 的接口。未知或未分类的 Native 命中单列。 |
| `unknown_operations`、`observed_placeholders`、`unimplemented_operations` | 未入清单的 ID、运行时或静态识别的实际占位调用、已知未实现调用。同一次调用可同时属于多项。 |
| `coverage.uncalled` | 有完整 trace，且在该路样本未观察到这一可记录接口。 |
| `coverage.uncovered` | 该路缺少 trace，或 trace v1 无法逐条观察，例如直接 `RtAPI.ticks`、IPC control、Binder 事务和 nvdrv IOCTL 子命令。 |
| `coverage.insufficient_evidence`、`static_evidence_gaps` | 前者表示 trace 损坏/缺记录时，未观察到的可记录接口。后者表示清单已定位实现，但命令级语义证据不足。 |
| `pruning_candidates`、`license_review` | 候选文件与符号、源码依据、各路样本是否有证据、适用构建条件及待审核的许可范围。 |

`trace_health` 记录配对结果、缺号区间、缺失 begin/end、重复、身份不符、非法记录和 Runtime 汇总中的丢失与截断。清单提供期望计数、Native 接口集合或 trace 记录哈希时一并核对。降级报告仍保留可配对调用的计数，省略观察嵌套及未调用判断。裁剪证据还需独立检查直接调用、跨模块调用、动态分派、构建选项和目标运行。候选包括可选 Vulkan 提交、NVDEC/VIC 的 host1x 路径和 Opus 解码服务。

## 2026-11-18 实测

自有 ABI 1 样本由 `713c861` 生成，种子 `0x4E484132`。模块 SHA-256 为 `4801c9f054935d8b4db788ab987bdf9bca2fc02662718516df8ba9628b16be7f`。当前 Runtime 为 `d7d26a9`，在 Linux aarch64 容器内使用 GCC 12.2.0 构建 headless 目标。四路运行均退出 0。6 个 ASan/UBSan 宿主场景和 12 个调度配置通过。

| 路由 | 完整调用数 | Native / Legacy 调用数 | Native / Legacy 接口数 | 观察到的嵌套关系种类 |
| --- | ---: | ---: | ---: | ---: |
| 原始基线 | 未采集 trace | — | — | — |
| 默认 Legacy | 71 | 0 / 71 | 0 / 34 | 14 |
| 完整 Native | 71 | 24 / 47 | 18 / 16 | 14 |
| 混合 | 71 | 21 / 50 | 15 / 19 | 14 |

三组 trace 汇总均无丢失或截断，34 个实际调用的 ID 全部命中清单。完整 Native 仍依赖 Legacy 的 IPC 传输、SM、时间子服务、SD 卡挂载及句柄回收。样本的 CNTVCT 由 guest 断言覆盖，调用次数在 trace v1 中不可见。样本未执行 HID 输入服务、音频服务或 GPU 提交，原生 Windows 证据沿用计划中的待验证状态。
