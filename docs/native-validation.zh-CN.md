# 验证与发布检查

[English](native-validation.md) | **简体中文** | [使用指南](native.zh-CN.md)

本文给出当前桌面应用可复现的检查方法。软件回归和模拟 GUI 检查不能证明某套 EtherCAT 系统的硬件功能或周期性能。

## Windows 构建与测试

```powershell
./scripts/build-windows.ps1 -Clean -Jobs 4
./scripts/smoke-windows.ps1
```

清理选项重置 CMake 配置，并从 `third_party/` 重新编译目标。删除 `build/` 后依赖源码仍然保留，安装好目标平台的构建环境后即可离线完成干净配置和编译。预设输出隔离在 `build/windows-mingw-release/`。测试已有构建可直接运行 `ctest --preset windows-mingw-release`。

| CTest 套件 | 覆盖内容 |
| --- | --- |
| `core` | 类型转换、字段边界和模拟后端 |
| `controller` | 队列、代次、所有权、状态/周期行为和快照 |
| `script_bridge` | 脚本/控制器请求与映射信号操作 |
| `esi_catalog` | XML 解析、身份/布局匹配和描述回退 |
| `pdo_layout` | 过程映像范围、失败映射拒绝与合法零字节映射 |
| `script_runtime` | Lua 限制、主机操作、定时/周期调度与取消 |
| `cyclic_state_poll` | 确定性 AL 读取故障注入：瞬时 NONE、真实状态/错误回复、逐站恢复、暂存期限、PDO 失效与重连隔离 |
| `soem_segmented_sdo` | 57 个确定性 SDO 场景：上传/下载、Complete Access 应答兼容、无效请求、分段循环超时、畸形响应及 Mailbox 所有权 |

界面检查脚本使用 SDL dummy 视频驱动和软件渲染，检查七个页面的中英文、三种窗口大小下的 PDO、实际填充的诊断历史布局、模拟扫描和明确的抓包驱动缺失行为，不扫描真实总线。

检查程序复制到 `build/windows-mingw-release/smoke/app/`，截图位于 `smoke/`，测试配置和脚本与已安装应用隔离。

## 发布包检查

构建和界面检查通过后，在源码目录执行 `./scripts/package-windows.ps1`。将生成的 `dist/NiceECatM-<yyyymmddhhmm>-windows-x64.zip` 与旁边的 `.zip.sha256` 比对，再检查和运行解压目录。干净安装暂存流程排除本地开发配置和用户脚本。

检查可执行文件名称、图标、12 位版本及不依赖附加 DLL 的启动。ESI、示例脚本和第三方许可证应与源码资源一致。应用在自身旁边创建 `preferences.ini` 和用户脚本；这些属于用户数据，不是构建输入。

检查文档本地链接，源码包不应包含构建缓存或临时探测程序，并验证 Windows 包从其他工作目录启动。真实网卡访问需要 Npcap/WinPcap；缺少驱动不能妨碍 GUI 和模拟设备运行。

## 硬件验收

验收结果取决于目标机器、网卡、驱动、从站和接线。应记录这些条件，以及目标周期、实际周期、AL 状态和 WKC。

| 项目 | 目标硬件所需证据 |
| --- | --- |
| 扫描与 ESI | 身份正确、结束于 PRE-OP、方向/位偏移和描述匹配正确 |
| 状态控制 | INIT/PRE-OP/SAFE-OP/OP 实际状态、失败转换与错误信息 |
| PDO | 输入实值、OP 输出生效、SAFE-OP 安全行为 |
| ESC / SDO | 合法/拒绝地址、超时与传输错误报告 |
| 映射 | 普通和 overlapped 的实际映像范围与 WKC |
| 冗余 | 两个接口均初始化，实测单路断开/恢复及双路故障 |
| 周期性能 | 目标周期下持续运行、调度偏差和 WKC 异常 |
| 脚本生命周期 | 重复运行、取消、总线丢失与有界回调故障 |

**2026-11-14 实机短测：** Windows 11 LTSC build 26100；Npcap 1.90；Intel I219-LM，驱动 20.0.3.28，100 Mb/s；AX58100 和 LAN9252 两个从站，期望 WKC 为 6。仅在 SAFE-OP 交换，结束时回到 PRE-OP 后断开，未进入 OP，也未写入输出。

两个从站原先均为 AL Control/Status `0x12`、AL code `0`。扫描收尾现请求普通 `0x02` 并确认完整状态，重复扫描均返回 `0x02`。旧版重扫后等待约 15.6 ms；高分辨率可等待定时器修复了该调度问题。Windows 每帧接收预算现为 `clamp(cycleUs, 2000, 10000)` us。

下表新版每项运行 15 秒，单元格为通讯异常次数 / 循环数。

| 目标周期 | 普通映射 | Overlapped 映射 |
| --- | --- | --- |
| 10 ms | 0 / 1493 | 0 / 1496 |
| 1 ms | 70 / 10453 | 71 / 9900 |
| 500 us | 67 / 12293 | 70 / 14434 |

旧版 3 秒基线在 10 ms、1 ms、500 us 下分别为 4 / 189、141 / 189、180 / 189。测试时间短且窗口不同，不能视为严格性能基准。1 ms/500 us 仍有大量跳过周期，当前 PC 下不承诺达到这两个目标周期。

网卡 RX/TX errors/discards 均为零；ESC CRC/lost-link 计数未增长，但一次 LAN9252 forwarded-error 为 1，因此不能完全排除物理链路问题。中断节流和 EEE 为开启状态，未验证其因果影响。同一总线双口冗余尚未测试。

后续 OP 测试覆盖输出花样提交、普通/overlapped 映射及新增 5 ms 目标周期。结果与剩余时序限制见[通讯性能](performance.zh-CN.md#本机-op-测试)；上面的早期表格仅描述 SAFE-OP 测试。

**2026-11-15 周期 AL 状态回归：** Intel 测试总线上的 AX58100 和 LAN9252 使用普通映射、10 ms 目标周期，运行按索引写入的 Lua `cycle.every(100)` 脚本 60.0013 秒。完成 5,974 个周期、59 次回调及 118 次信号写入；118 次主站输出映像比较全部一致，没有脚本错误、主机写入错误或采样到的未知 AL 状态。记录的状态样本中两站均为 OP，期望 WKC 为 6，最大实测周期偏差为 10,279 us，仍有 27 个 PDO WKC 不完整的周期。本次短时实机运行未遇到 AL 读取失败；偶发失败、恢复和暂存到期路径由 60 个确定性故障注入检查覆盖。映像比较没有独立测量物理输出引脚。结束时清零两站输出映像，确认 PRE-OP 后断开。测试未修改网卡设置，探针仅保留在 `build/communication-diagnostics/`。

发布前目录和构建整理本身不构成硬件验收。Ubuntu x86/ARM 二进制与平台行为仍需在对应环境单独构建测试。FoE、EoE、SoE、VoE、BOOT 和 DC 触发配置不属于当前已实现服务。
