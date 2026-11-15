# EtherCAT 性能

[English](performance.md) | **简体中文**

本文供源码目录中的开发与实机测试参考，不随发布包安装。NiceECatM 不会自动修改网卡设置。

本机最新结论：关闭中断节流显著降低了典型收包延迟和工作线程 CPU，但当前 Controller 在 30 秒测试中，1 ms 目标的平均间隔仍约 1.08～1.10 ms，500 us 目标仍约 0.86～0.87 ms，且仍有 WKC 异常。已修复无 IP 绑定时的网卡枚举问题，并完成实机验证。全部网卡设置与绑定已恢复原值，产品默认调优策略未改。

## 已测结果

最初在 Windows / Intel I219-LM / Npcap 环境、Interrupt Moderation 和 EEE 均开启时的只读 AL 状态 BRD 探针结果：每次只发收一次、不重发，接收上限 5 ms，各组约 400～2,000 次采样。

| 交换之间请求的间隔 | 总 RTT，p50 | 发送调用，p50 | 接收等待，p50 |
| --- | ---: | ---: | ---: |
| 无 | 489 us | 19 us | 468 us |
| 1 ms | 539 us | 41 us | 496 us |
| 10 ms | 683 us | 113 us | 547 us |

这些软件观测时间表明延迟主要出现在接收交付阶段；各列中位数不必相加。原来采用目标周期一半作为接收预算，在本机的 500 us、1 ms 目标周期下过短。

Windows 接收窗口现按目标周期允许 2～10 ms，帧一到便立即返回。仍需检查实际周期、偏差和错过周期：超时加长不代表周期达标。SOEM 按每帧计算超时，多分段交换可能超过一个接收窗口。

通过 Npcap 只读查询 `OID_GEN_INTERRUPT_MODERATION` 成功，但此 I219-LM 驱动返回 `NdisInterruptModerationNotSupported`（state 1），因此未发送 OID SET 请求。下文管理员测试改为修改驱动高级设置并重启网卡；高级选项存在不代表支持运行时 OID 切换。[微软 OID 约定](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-gen-interrupt-moderation)。

网卡调参前的软件短测比较了工作线程 Normal/AboveNormal/Highest 优先级、额外 `timeBeginPeriod(1)` 和 High QoS，目标周期为 10/5/2/1/0.5 ms，每组 5 秒。未观察到稳定收益，机器波动明显，因此产品保持默认调度设置。这些短测不构成严格基准。

## 本机 OP 测试

以下为网卡调参前的参考测试，环境为 Windows 11 LTSC build 26100、Npcap 1.90、Intel I219-LM 和 AX58100/LAN9252 两个从站，覆盖普通和 overlapped 映射。下表每项独立运行 20 秒，期望 WKC 为 6，每次测试前提交 `00/55/AA/FF/00` 输出花样。两个从站始终保持 OP、AL code 为 0，没有自动停止；结束前输出映像清零，再回到 PRE-OP 并断开。未对物理输出电压进行外部测量。

下文 WKC 异常数表示工作计数器不符合预期的周期数，不是测得的网络丢包率。

| 目标周期 | 普通：WKC 异常 / 循环数 | Overlapped：WKC 异常 / 循环数 | 平均间隔，普通 / overlapped |
| --- | ---: | ---: | ---: |
| 10 ms | 3 / 1992 | 6 / 1992 | 10.04 / 10.04 ms |
| 5 ms | 31 / 3956 | 33 / 3950 | 5.06 / 5.06 ms |
| 2 ms | 103 / 9810 | 141 / 9672 | 2.04 / 2.07 ms |
| 1 ms | 184 / 13605 | 165 / 14353 | 1.47 / 1.39 ms |
| 500 us | 206 / 16472 | 173 / 16729 | 1.22 / 1.20 ms |

这是主机负载变化下的短时观察，不能用于严格比较映射模式，也不构成周期保证。网卡 RX/TX 错误和丢弃计数保持为零。500 us 测试中 I/O 工作线程约占一个 CPU 核心的 43%～45%，主要用于轮询收包；提高线程优先级不能消除收包交付延迟。同一总线双口冗余尚未测试。

## 管理员 A/B 测试，2026-11-14

使用相同硬件及控制器/后端库、平衡电源计划和默认工作线程调度，不运行 GUI 渲染或脚本负载。本机只提供平衡计划，没有创建或切换全局电源计划。先备份原值，各项设置修改并重启网卡后读回确认，分别测试并恢复；最后两项均开启，再测一次基线。采用普通映射，每档 OP 目标周期运行 5 秒，采样前进行输出花样检查。观察到的两个从站状态均为 OP、AL code 为 0，没有自动停止。

1 ms 间隔的 RTT 探针每项发起 2,000 次请求。以下分位数只计算成功帧，超时单独列出。

| 配置：中断节流 / EEE | RTT p50 | RTT p99 | 超时 / 请求数 |
| --- | ---: | ---: | ---: |
| 原始：开 / 开 | 545 us | 1361 us | 2 / 2000 |
| 关 / 开 | 116 us | 1441 us | 2 / 2000 |
| 开 / 关 | 548 us | 1546 us | 2 / 2000 |
| 关 / 关 | 122 us | 1752 us | 4 / 2000 |
| 恢复：开 / 开 | 540 us | 1695 us | 2 / 2000 |

对应的 1 ms OP 目标周期结果：

| 配置 | 估算平均间隔 | 工作线程 CPU，占一个核心 | WKC 异常 / 循环数 |
| --- | ---: | ---: | ---: |
| 原始 | 1.42 ms | 37.8% | 10 / 3514（0.28%） |
| 仅关闭中断节流 | 1.08 ms | 10.6% | 28 / 4636（0.60%） |
| 仅关闭 EEE | 1.43 ms | 35.9% | 29 / 3502（0.83%） |
| 两项均关闭 | 1.10 ms | 9.7% | 23 / 4568（0.50%） |
| 恢复原值 | 1.44 ms | 41.5% | 29 / 3483（0.83%） |

关闭中断节流显著降低了典型收包交付延迟和轮询 CPU 开销，恢复后也重现了原来的延迟范围。单独关闭 EEE 在此链路上未显示明显收益，同时关闭两项也没有证明额外收益。500 us OP 目标周期下，仅关闭中断节流使估算平均间隔从 1.23 降至 0.87 ms，CPU 从 40.6% 降至 15.6%，但仍有大量目标周期未达标。

WKC 异常、RTT p99 和超时次数未呈现一致改善。这些短时顺序测试确认了一项对本机延迟/CPU 有用的设置，不代表确定性时序或稳定性问题全部解决。平均间隔由时间/计数器差值估算，停止命令的采样边界会带来少量偏差。探针的最大偏差字段还包含正式采样前的输出检查，不是精确的 5 秒窗口统计，因此上表没有使用该字段。

重启网卡会主动中断链路，可能使 ESC lost-link 计数增长，不能将跨配置的累计变化当作连续运行中自发掉线；应检查每次稳定运行内的计数差值。Forwarded RX 计数需要另行解释，见下文。这组 A/B 测试后已恢复原值，备份、设置读回和日志保存在 `build/communication-diagnostics/admin-20261114-140501/`。

## 当前 Controller：30 秒测试

随后使用当前 Controller，在关闭中断节流、保持 EEE 开启、Normal 工作线程优先级、不使用忙等原型的条件下复测。下表每行独立运行 30 秒，不含 GUI 渲染和脚本负载。观察到的两个从站均为 OP（state 8）、AL code 0，没有自动停止。

| 映射 | 目标周期 | 估算平均间隔 | 工作线程 CPU，占一个核心 | WKC 异常 / 循环数 | 跳过的目标周期数 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 普通 | 5 ms | 5.043 ms | 6.09% | 22 / 5949 | 51 |
| 普通 | 1 ms | 1.082 ms | 14.06% | 137 / 27725 | 2296 |
| 普通 | 500 us | 0.860 ms | 15.41% | 160 / 34899 | 25140 |
| Overlapped | 5 ms | 5.034 ms | 6.98% | 31 / 5960 | 40 |
| Overlapped | 1 ms | 1.095 ms | 13.23% | 155 / 27393 | 2630 |
| Overlapped | 500 us | 0.869 ms | 15.21% | 205 / 34539 | 25493 |

5 ms 时平均间隔接近目标，但仍偶有交换失败。两种映射都不能持续达到 500 us，overlapped 也未显示稳定的时序或通讯质量改善。上文关于平均间隔和最大偏差采样范围的限制同样适用于本表。

ESC 读数覆盖每轮运行的前后边界，也包含输出检查和状态切换。三轮普通映射中，从站 2 的 `0x0308` 计数分别为 1→66、0→5、0→3，lost-link 计数均未变化；三轮 overlapped 中，转发计数均为 0→0，lost-link 也未变化，但仍有 WKC 异常。这些计数不能确定具体哪些 EtherCAT 交换失败，也不能认定映射方式是异常原因。全部测试后，中断节流 / EEE 已恢复为开 / 开。日志为上述诊断目录中的 `im-long-mapping-0.log`、`im-long-mapping-1.log`。

## ESC 错误计数的解释

SOEM 将首站 DL Control `0x0100[0]` 设为 1、后续从站设为 0，本次只读检查也确认了这两个值。该位为 1 时，ESC 会销毁非 EtherCAT 帧并增加 EPU 错误计数；下游 ESC 可能将带标记的帧计为 forwarded RX error。因此仅凭转发计数不能认定网线损坏或 EtherCAT 帧失败。[Beckhoff ESC Technology，第 14.1～14.2 节](https://download.beckhoff.com/download/document/io/ethercat-development-products/ethercat_esc_datasheet_sec1_technology_v2.5.pdf)。

独立的 15 秒抓包发现 16 帧 IPv4、12 帧 IPv6，抓包丢弃为 0；此抓包未混入正式性能测量。首站 EPU 计数 `0x030C` 已在 `0xFF` 饱和，无法观察后续增量并做同步关联。下游转发计数增长符合普通非 EtherCAT 流量被销毁的机制，但尚未逐帧证明。采样中的 `0x0300～0x0307` 和网卡错误计数始终为零，目前没有物理损坏的直接证据。残留 WKC 异常的来源仍待确定。

## 网卡绑定测试与枚举修复

固定关闭中断节流，仅关闭两个 TwinCAT 绑定（`beckhoff_tcether`、`tcrtefilter`），分别进行 5 ms、1 ms、500 us 目标下各 10 秒的 OP 测试，结果有升有降。1 ms 间隔 RTT 的 p50 从 123 变为 163 us，p99 从 1344 变为 1278 us。这些短测不能证明稳定收益，也不能认定存在 TwinCAT 驱动冲突。

首次关闭 IPv4/IPv6 绑定时发现一个软件缺陷：原始 RTT 探针在 1 ms 间隔下仍成功完成全部 2,000 次请求，但 Controller 无法再枚举 Intel 网卡。原 Windows 过滤逻辑依赖 IPv4 网卡枚举，现已改为通过 Npcap GUID → LUID / `GetIfEntry2` 查询接口，不再依赖 IP 绑定。首次测试没有有效的 OP 对比数据。修复后通过全部 7 项 CTest 检查及后续实机扫描。

修复后按“基线 → 关闭 IPv4/IPv6 绑定 → 恢复绑定”复测，固定中断节流关闭、EEE 开启、普通映射及默认工作线程调度。关闭 IP 时，Windows 还自动关闭了依赖它的 `ms_server`、`ms_netbt`、`ms_netbios`、`ms_msclient` 绑定；两个 TwinCAT 绑定和 Npcap 均保持开启，因此该阶段测试的是 IP 栈及其依赖绑定一起停用的效果。每档 OP 目标周期运行 10 秒，不含 GUI 或脚本负载。三个阶段均扫描到两个 PRE-OP、AL code 0 的从站，期望 WKC 为 6；OP 运行中观察到的两个从站均为 OP、AL code 0，没有自动停止。下表为 1 ms 间隔 RTT 及 1 ms OP 目标的结果，RTT 分位数只计算成功请求。

| 配置 | RTT p50 / p99 | 超时 / 请求数 | 估算 OP 平均间隔 | WKC 异常周期 / 循环数 |
| --- | ---: | ---: | ---: | ---: |
| 基线 | 123 / 1075 us | 0 / 2000 | 1.081 ms | 24 / 9260 |
| 关闭 IPv4/IPv6 | 120 / 1451 us | 5 / 2000 | 1.117 ms | 51 / 8965 |
| 恢复绑定 | 122 / 879 us | 2 / 2000 | 1.079 ms | 31 / 9268 |

按相同阶段顺序，5 ms 目标下的平均间隔为 5.026 / 5.033 / 5.028 ms，500 us 目标下为 0.876 / 0.906 / 0.858 ms，仍有 WKC 异常。因此关闭 IP 并未解决周期时序或交换稳定性。次站在 5 ms / 1 ms / 500 us 三档下的转发计数增量，基线为 34 / 2 / 1，关闭 IP 后为 0 / 0 / 0，恢复后为 3 / 0 / 0，各轮运行内 lost-link 计数未变化。这支持上文关于背景帧的解释，但不构成逐帧证明，也不能将转发计数等同于失败的 EtherCAT 周期数。

最终核对原有 47 项高级设置及 21 项绑定，均无差异；中断节流和 EEE 均已恢复开启，链路 Up、100 Mbps，网卡 RX/TX 错误及丢弃均为零，电源计划仍为平衡。本次进入产品的改动为不依赖 IP 的网卡枚举修复，没有启用实验性时序策略。完整的 `fixed-bindings-baseline-*`、`fixed-ip-bindings-off-*`、`fixed-bindings-restored-*` 日志及 `final-verification.json` 保存在上述诊断目录。

## 关闭中断节流后的软件试验

关闭中断节流后，再次比较 Normal/Highest 工作线程优先级、额外 `timeBeginPeriod(1)` 和 High QoS；五档目标周期各运行 5 秒。仍未观察到稳定的额外收益，500 us 目标下的估算平均间隔约为 0.85～0.89 ms。

另一个独立原型使用相同后端库和 Controller 的实验副本，将阻塞定时器提前最多 50/100/200 us 唤醒，再进行有界忙等，保持原总线截止时间不变。500 us 目标下，200 us 忙等预算使估算平均间隔从无忙等时的 0.922 ms 降至 0.588 ms，工作线程 CPU 从一个核心的 16.5% 增至 25.3%。最后再次关闭忙等，结果回到 0.896 ms / 15.3% CPU。200 us 样本在 5 秒、8,517 个循环中仍有 1,532 个跳过周期和 18 次 WKC 异常。

此原型只保留在 `build/`，没有成为发行版默认行为或公开产品选项。这些短时顺序测试显示了时序与 CPU 的取舍，不能保证 500 us；测试也未包含 GUI 渲染或脚本负载，残留 WKC 异常仍不允许认定稳定性问题已全部解决。

## 建议顺序

1. 使用专供 EtherCAT 的物理有线网卡，直接连接第一个从站。协商到 100 Mbps 属于正常情况；保留正常工作的速度/双工协商，不要强制速率。
2. 优先测试 **Interrupt Moderation（中断调节/中断节流）= Disabled**。合并中断通知会增加延迟；关闭后可能增加处理硬件中断的 CPU 开销，但本机 I/O 线程轮询等待交付的 CPU 开销反而下降。具体取舍需本机验证。[微软机制](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/interrupt-moderation)、[低延迟建议](https://learn.microsoft.com/en-us/windows-server/networking/technologies/network-subsystem/net-sub-performance-tuning-nics)。
3. 单独测试 **EEE = Disabled**。唤醒可能增加延迟，但必须两端支持并协商；仅驱动选项开启不能证明链路使用了 EEE。驱动若提供 DMA Coalescing，也可单独测试。[Intel EEE 说明](https://edc.intel.com/output/DownloadPdfDocument?id=7604)、[DMA Coalescing](https://edc.intel.com/content/www/us/en/design/products/ethernet/adapters-and-devices-user-guide/30.1/direct-memory-access-dma-coalescing/)。
4. 对剩余延迟尖峰，可对照交流供电和高性能电源计划。不要采用 Windows **Realtime（实时）** 进程优先级或 `THREAD_PRIORITY_TIME_CRITICAL`。[Windows 优先级](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities)。

TCP/UDP 卸载、TCP 窗口及按 TCP 端口/PSH 匹配的 Low Latency Interrupt 规则不优化 EtherCAT 原始帧。不要套用批量修改卸载功能/注册表的方案。[Intel 选项定义](https://www.intel.com/content/www/us/en/support/articles/000005783/processors.html)。

## Windows 辅助脚本

在项目根目录先检查选定网卡，把示例名称换成专用网卡的准确名称：

```powershell
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Inspect
```

`Inspect` 是默认只读模式，展示两项设置。`Apply` 默认仅修改 Interrupt Moderation；`-Setting EEE` 单独修改 EEE。`Apply`/`Restore` 需要管理员 PowerShell，`Restore` 必须提供 `-BackupPath`。执行前断开总线：两种操作都会重启选定网卡。保留各次备份。

先测试中断节流，再恢复基线，使用不同备份单独测试 EEE：

```powershell
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Apply -BackupPath './ethercat-im-backup.json'
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Restore -BackupPath './ethercat-im-backup.json'
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Apply -Setting EEE -BackupPath './ethercat-eee-backup.json'
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Restore -BackupPath './ethercat-eee-backup.json'
```

`-Setting InterruptModeration,EEE` 可同时修改两项；`Restore` 完全按备份恢复，忽略 `-Setting`。不支持的设置会跳过。阅读操作输出；两项一起修改不能区分具体哪项生效。脚本仅供源码辅助，不会自动执行。[微软网卡属性接口](https://learn.microsoft.com/en-us/powershell/module/netadapter/set-netadapteradvancedproperty)。

每次只改一项并保留原值。以相同次数、间隔、目标周期和负载复测，记录 RTT p50/p99/max、发送/接收耗时、WKC 异常、错过周期、偏差和 CPU。

## 平台边界

Windows/Npcap 适合扫描、信号测试和自动化，已配置立即交付、非阻塞且只捕获 EtherCAT。增大缓冲区主要解决溢出，不能据此认定 RTT 降低或周期具有确定性。[Npcap API](https://npcap.com/guide/npcap-api.html)。

若要求亚毫秒周期，可在 Linux 对照相同探针，再评估 PREEMPT_RT 及网卡/工作线程/IRQ 调优。PREEMPT_RT 改善调度延迟，不消除网卡延迟；Ubuntu x86/ARM 仍需硬件验证。忙轮询的 FIFO 工作线程不能饿死网卡 IRQ 线程。[PREEMPT_RT](https://www.kernel.org/doc/html/latest/core-api/real-time/theory.html)、[e1000e](https://docs.kernel.org/networking/device_drivers/ethernet/intel/e1000e.html)、[Ubuntu IRQ 亲和性](https://ubuntu.com/real-time/docs/how-to/tune-irq-affinity/)。

Windows 专用实时环境/直接网卡驱动会增加部署、硬件适配验证，有时还有商业授权成本。单纯替换普通 NDIS 捕获/过滤实现不能保证确定性。[Beckhoff 实时要求](https://infosys.beckhoff.com/content/1033/tc3_overview/9309844363.html)、[acontis 架构](https://www.acontis.com/en/ethercat-master-windows.html)。
