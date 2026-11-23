# 构建与使用指南

[English](native.md) | **简体中文** | [README](../README.zh-CN.md)

## 构建环境

Windows x64 需要 CMake 3.28+、支持 C++17 的 MinGW-w64 编译器、Make 和 windres，并将它们加入 PATH：

```powershell
./scripts/build-windows.ps1 -Jobs 4
```

脚本依次配置、构建、运行 CTest 并安装到 `dist/windows-x64/`。加入 `-Clean` 可重置配置，并从随附源码重新编译目标。应用为 `NiceECatM.exe`，应用依赖与字体静态链接或嵌入，运行时仍使用 Windows 系统库。

Ubuntu 需要 C++17 编译器、Make、CMake 3.28+，以及机器上所选 SDL 显示后端的开发库。以下脚本按本机 CPU 架构构建：

```sh
./scripts/build-linux.sh
```

结果安装到 `dist/linux-$(uname -m)/NiceECatM`。加入 `--clean` 可重置配置并重新编译，使用 `NICEECATM_JOBS` 设置并行数。真实 EtherCAT 访问需要原始以太网套接字权限。Linux 构建与硬件运行仍需在对应 x86 或 ARM 目标机验证。

后台版本检测在 Windows 使用系统 WinHTTP，在 Linux 使用可选的 libcurl。Ubuntu 配置构建前安装 `libcurl4-openssl-dev` 即可启用；未安装时仍可点击版本号打开 Releases，但不进行网络检测。普通桌面启动时仅检查 GitHub 一次，CLI 与界面冒烟测试不发起更新请求。检测独立于 EtherCAT 传输，不阻塞界面、不弹窗、不写日志。正式发布的 tag 使用构建版本 `yyyymmddhhmm`（也接受 `v` 前缀），不检测草稿与预发布版本。

若 Linux 程序链接共享 libcurl，目标系统还须安装匹配的运行库（通常为 Ubuntu 的 `libcurl4` 包）；构建时可选不代表已链接的二进制文件在运行时可以缺少该库。

SOEM、SDL、Dear ImGui、Lua、TinyXML2 和代码编辑器均以固定版本随源码放在 `third_party/`。安装好目标平台的构建环境后，配置和编译直接使用本地源码，无需下载依赖。依赖配置统一在 `cmake/dependencies.cmake`，版本、源归档哈希和适配记录见 [THIRD_PARTY.md](../THIRD_PARTY.md)。

## 构建目录

`windows-mingw-release` 是描述平台、编译器和构建类型的 CMake 预设。将 `build/windows-mingw-release/` 与 `build/linux-release/` 分开，可以避免不同工具链的缓存和目标文件混用。

CMake 构建目录是生成内容，`build/<预设>/_deps/` 保存已编译的依赖产物。删除这些目录后，依赖源码仍保留在 `third_party/`，重新构建无需联网。`dist/` 是安装目录，其中可能有用户放在可执行文件旁的文件。清空 `build/` 前应保留手动存放的备份，替换安装前应保留用户文件。

手动操作的等价命令：

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release --parallel 4
ctest --preset windows-mingw-release
cmake --install build/windows-mingw-release --prefix dist/windows-x64 --strip
```

配置时使用 `-DNICEECATM_BUILD_GUI=OFF` 可构建不含 SDL/ImGui 的 CLI；`NICEECATM_BUILD_TESTS` 控制测试目标。

## 生成 Windows 发布包

在源码目录完成 Release 构建和测试后执行：

```powershell
./scripts/package-windows.ps1
```

脚本复用已有构建，安装到干净的私有暂存目录，生成 `dist/NiceECatM-<yyyymmddhhmm>-windows-x64.zip` 和 `.zip.sha256`。压缩包包含命名的应用文件夹，内有程序、精简的中英文 README、项目许可证、第三方说明、随附 ESI 和示例脚本，不会打入开发安装目录的 `preferences.ini` 或用户脚本，也不清理 `dist/windows-x64/`。

发布版 README 和第三方说明来自 `resources/release/`，与源码仓库首页分开维护；其中的相对链接按解压后的目录布局编写。发布包没有 `docs/` 目录，详细的构建、架构、验证、ESI 和 Lua 指南保留在源码仓库中。

将压缩包解压到可写目录，启动其中的 `NiceECatM.exe`。发布打包不安装 Npcap，也不修改系统驱动。

## 便携目录

| 可执行文件旁的文件或目录 | 用途 |
| --- | --- |
| `NiceECatM.exe` / `NiceECatM` | 应用程序 |
| `LICENSE` | 项目版权及许可证：Naisu Xu；GPL-3.0-only |
| `licenses/` | 第三方许可证 |
| `README.md`、`README.zh-CN.md` | 精简的中英文使用说明，含脚本章节 |
| `THIRD_PARTY.md` | 第三方说明 |
| `esi/` | 厂商 ESI XML |
| `scripts/` | Lua 脚本和随附示例 |
| `preferences.ini` | 界面语言，由应用创建或更新 |

应用依据可执行文件目录解析这些路径，不受当前工作目录影响。修改配置或脚本时应使用可写目录。ESI 和 Lua 文件为可编辑数据，不需要额外安装语言运行时。

软件版本使用本地构建时间 `yyyymmddhhmm`，界面、CLI 帮助与 Windows 版本资源共用该值。Windows 图标由 windres 编译，不需要 rcedit。

## 连接总线

Windows 真实设备访问需要已安装的 Npcap 或兼容 WinPcap，不随包分发驱动安装器或抓包 DLL。没有驱动时仍可使用 GUI 与模拟设备，真实网卡枚举会明确报错。

选择有线物理网卡并扫描。单口模式使用一个接口；同一总线双口冗余使用两个不同接口，分别连接从站链两端。普通/overlapped 映射与网口数量相互独立。更改模拟／硬件／编程模式、网卡、连接模式或映射方式前，须先断开连接，再修改并重新扫描；切换运行模式还需先停止脚本。

扫描是主动执行的 SOEM 配置和映射流程，响应从站结束于 PRE-OP，并非被动抓包。扫描完成后，在拓扑中选中一个从站以查看其信号、身份、寄存器和 SDO。

## 状态与周期 I/O

| 操作 | 结果 |
| --- | --- |
| BOOT | 停止周期 I/O 和脚本后，检查完整目标范围，经 INIT 配置 Bootstrap Mailbox 并进入 BOOT；PDO 映射失效 |
| INIT | 请求 INIT，并停止周期交换 |
| PRE-OP | 请求 PRE-OP，并停止周期交换 |
| SAFE-OP | 请求 SAFE-OP 并启动周期交换；从站输出仍保持安全状态 |
| OP | 请求 OP 并启动周期交换；从站可应用有效输出 PDO |
| 断开连接 | 停止交换并关闭主站连接 |

状态请求可能失败，应以实际 AL 与日志结果为准。顶部按钮顺序为 **BOOT / INIT / PRE-OP / SAFE-OP / OP**，目标选择可对全部从站或当前拓扑选中从站发起请求。BOOT 在改变状态前检查完整目标范围，随后逐站确认；退出时经 INIT 恢复普通 Mailbox，需要重新扫描后才能启动周期 I/O。实际从站状态包含 AL 错误标记。

停止交换且全部从站处于 INIT/PRE-OP 时可调整目标周期。支持 500 us、1、2、5、10、20、50、100 ms，默认 10 ms。统计周期为主机测量结果，受到操作系统和驱动调度影响。

Windows 周期调度使用高分辨率可等待定时器（waitable timer）。Npcap 每帧接收预算为 `min(max(目标周期, 2 ms), 10 ms)`。该预算不保证达到目标周期；实际周期偏差和跳过周期仍会统计。

后台 AL 状态监测会区分未读到有效状态的 `NONE` 与已经确认的状态变化。PDO WKC 完整有效时，AL 读取失败可暂存该从站最后确认的状态，从首次失败起最长 2 秒。PDO WKC 无效或暂存到期后，该状态失效，必须取得新的 AL 回复才能写入输出。真实 INIT、SAFE-OP 和错误状态回复会立即采纳，显式状态切换始终需要重新确认。日志分别记录首次 AL 读取失败、暂存失效和读取恢复，与脚本回调错误、PDO WKC 异常区分。PDO 连续失败 2 秒仍会停止交换。

## 页面

- **设备 / DC：** 设备身份、映射布局与声明能力；DC 触发配置和读回尚未接入。
- **ESC 寄存器：** 按 ESC 物理偏移访问字节。写入受已实现的寄存器规则与当前状态限制。
- **BufferPDO：** 显示所选从站的信号、直接输出编辑和约 20 秒趋势。I/O 工作线程按每个有效通讯周期聚合，并以 100 ms 为一个区间保存最小值、最大值和最新值，因此短脉冲仍可见。输入只读，停止或数据缓存时保留已有趋势。
- **CoE / SDO：** 可搜索的对象字典，子索引始终展开，分组标题采用浅色底色、子项缩进，不提供折叠控件；显示描述、类型、位长、权限、来源和手动读取值。匹配 ESI 的描述不需要 Mailbox 请求。在确认 PRE-OP、从站声明 SDO Information 且脚本未运行时，首次进入此页自动获取一次在线描述；“刷新字典”可手动重试。在线元数据优先，ESI 补充缺失描述。发现流程有请求和时间上限，可能返回部分字典，不批量读取当前值，也不切换 AL 状态。仍可手动填写 Index/SubIndex，未知编码使用原始字节。
- **FoE：** 在确认的 PRE-OP 或 BOOT 中上传／下载所选从站的文件，显示进度，支持取消及有界超时／重试。
- **脚本：** Lua 编辑器、文件列表、API 片段与自动化，详见[脚本指南](scripts.zh-CN.md)。
- **EEPROM：** 仅编程模式可用，支持镜像准备／导出、读取保存，以及烧录所选目标并完整回读比较。
- **诊断：** WKC、主机周期、总线上下文和 200 个采样点的周期偏差历史，标签位于 EEPROM 后。
- **日志：** 应用与脚本共用消息，选中单条后按 Ctrl+C 复制该条，提供跟随和清空。

FoE 位于 CoE / SDO 后，诊断位于 EEPROM 后。进入编程模式时选中 EEPROM，其余页面全部不可选；硬件／模拟模式下 EEPROM 不可选。

写入不设置全局解锁开关，但始终检查状态、方向、大小、映射和对象权限。在 PRE-OP/SAFE-OP 修改输出映像只是准备主站数据，不代表物理输出已生效。同步寄存器和 SDO 访问要求停止周期交换。

## FoE 文件传输

在硬件／模拟模式停止脚本，将全部从站切换到 PRE-OP，再选择目标、上传／下载方向、厂商要求的从站文件名与 32 位密码，以及本地文件或保存位置。厂商要求 BOOT 时使用顶部共用按钮。文件上限为 128 MiB，上传默认接收上限为 16 MiB。响应／无进展／总超时默认为 3／30／300 秒，重试默认 3 次。页面提供进度与取消，详情进入全局日志，密码不保存、不记录。

从站返回错误时，页面和全局日志显示协议错误码、按界面语言翻译的标准原因及可用的从站原始文本；通用或厂商自定义错误无法可靠区分文件名不受支持还是密码错误。

**FoE 完成仅表示文件传输成功，固件是否生效由从站决定。**取消不撤销已接收的从站数据；上传失败或取消时保留原有本地目标文件。EEPROM 可用不代表支持 FoE。操作步骤见[发布包说明](../resources/release/README.zh-CN.md#foe-文件传输)，协议边界见[架构文档](design.zh-CN.md#foe-传输与-boot-会话)。尚未提供 Lua／CLI 文件传输 API。

## EEPROM 编程

停止脚本、断开连接，再选择**编程**启用 EEPROM 页面。打开支持的 ESI 设备描述或 SII BIN；未连接时也可准备／导出镜像。选择一张网卡并专用扫描至 INIT，不进行 PDO 映射，再在左侧拓扑选择物理目标。**读取并保存**读取其 EEPROM；**烧录 EEPROM**确认目标、镜像和容量后，写入包含别名、序列号及填充的完整镜像，并完整回读比较。Vendor ID／Product Code 变化会提示但允许烧录，标识相同不代表硬件配置兼容；SII 容量是声明值，不是物理实测值。镜像支持范围及校验见 [ESI 文档](esi.zh-CN.md#eeprom-镜像)。

**导出 ESI**可从已加载的镜像或校验有效的 EEPROM 读取结果保存基础 ESI XML，无需连接设备。导出 ESI 仅包含 EEPROM/SII 中已有的信息，无法还原完整对象字典和初始化命令。

开始写入后取消可能留下部分镜像，不会回滚，仍保持编程模式。请按设备要求重载或重新上电，再断开、选择硬件、重新扫描后使用普通 I/O，不会自动恢复 OP。

## CLI

```powershell
./dist/windows-x64/NiceECatM.exe --help
./dist/windows-x64/NiceECatM.exe --adapters
./dist/windows-x64/NiceECatM.exe --simulate --scan
./dist/windows-x64/NiceECatM.exe --scan --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --read-register 0x130 --length 6 --slave 1 --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --sdo-read 0x1008 --sub 0 --length 128 --slave 1 --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --programming-scan --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --eeprom-read backup.bin --slave 1 --interface "<adapter name>"
./dist/windows-x64/NiceECatM.exe --sii-export device.xml --esi-device 0 --output device.bin
./dist/windows-x64/NiceECatM.exe --esi-export backup.bin --output device.xml
```

CLI 操作输出 JSON，硬件操作必须明确指定网卡。普通操作先扫描配置，完成后关闭后端，不是持续周期运行器；`--programming-scan` 和 `--eeprom-read` 使用不含 PDO 映射的专用 INIT 清单路径，`--sii-export`（ESI 转 BIN）和 `--esi-export`（BIN 转基础 ESI）均为离线转换。可使用 `--secondary NAME`、`--overlap`、`--cycle-us` 等选项。GUI 可用 `--language en` 或 `--language zh-CN` 指定语言；`--esi-image file.xml`（或 `file.bin`）在离线编程页面打开镜像，不扫描设备，也不打开抓包连接。

复现检查见[验证文档](native-validation.zh-CN.md)，实现边界见[架构文档](design.zh-CN.md)。
