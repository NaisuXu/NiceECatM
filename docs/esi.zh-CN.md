# ESI 描述、信号名称与对象字典

[English](esi.md) | **简体中文** | [使用指南](../README.zh-CN.md)

NiceECatM 将本地 EtherCAT Slave Information XML 与在线发现的 PDO 布局结合。ESI 补充名称和标量类型，过程值来自实际映射的 SOEM 映像。

匹配的 ESI 包含 `Profile/Dictionary/Objects` 时，CoE · SDO 页面还会显示其对象字典。字典独立于 PDO 布局：没有 PDO 映射的配置、诊断对象也能显示。支持标量、记录子项和常见一维数组，可取得索引、子索引、名称、类型、位长及声明的访问权限。无法展开的数组明确标记为部分字典，不会仅凭 PDO 条目推测 SDO 地址。

ESI 是离线描述，不代表所连接固件一定实现相应对象或 CoE 访问，XML 中的默认值也不会作为当前读数显示。在线 SDO Information 可提供实际对象描述；对象当前值仍需明确执行 SDO 读取。离线信息用于补充在线描述，下次启动时匹配文件不存在便不再显示其离线字典。

## 使用

1. 将厂商 XML 文件放到可执行文件旁的 `esi/`。
2. 重启应用，使 ESI 目录按当前文件重新加载。
3. 扫描目标网卡，在拓扑中选择一个从站。
4. 查看 BufferPDO；信号详情显示匹配的 ESI 文件名。
5. 对于 CoE 从站，可在 CoE · SDO 页查看字典，点击条目自动填写右侧参数编辑器。在线发现需要从站支持 SDO Information，并处于 PRE-OP 且周期交换已停止；周期运行中使用此前加载的描述。

身份匹配依据 Vendor ID、Product Code 和 Revision。发布包的 `esi/` 中已包含 ASIX AX58100 和 Microchip LAN9252 测试设备的厂商 XML 文件。

附带的 AX58100、LAN9252 XML 只有 PDO 定义，没有 SDO 对象字典。要填充字典列表，需要包含字典的 ESI，或者支持在线 SDO Information 的从站。支持普通 SDO 读写并不保证可以枚举对象字典。

移除某个文件后，下次启动不再加载它的 ESI 描述。但从站的 SII 字符串或 CoE SDO Info 仍可能独立提供信号名，因此仅凭信号有名称，不能判断某个 ESI 文件仍在加载。

## 布局匹配

CoE 设备通过当前 PDO 分配和映射条目发现布局；设备声明且实现 SDO Info 时可取得名称和类型。其他已支持的布局读取 SII EEPROM PDO 条目。支持 CoE 或某个设备型号都不代表一定支持 SDO Info。

ESI 条目需要匹配方向、PDO/对象标识、子索引、位宽及兼容的 SyncManager 元数据，位偏移由在线顺序确定。仅总字节长度一致不足以建立绑定。版本错误、描述冲突或布局不匹配会保留诊断结果，不强行推测布局。

支持 BOOL、1–64 位有符号/无符号整数、REAL32、REAL64；可规范化 INT、UDINT、LREAL 等 IEC 别名，以及 BITARR8/16/32。填充位只读，未知或不支持的字符串、数组和结构区域保留为原始字节/位。数值按 EtherCAT 小端位序解释。

## EEPROM 镜像

ESI XML 是描述文件，不是 EEPROM 镜像。**EEPROM** 页面仅在“**编程**”模式下可用，独立地从选中的 ESI Device 生成二进制 Slave Information Interface（SII）镜像，也可导入现有 SII BIN。加载、预览和导出镜像不需要连接设备；读取或烧录实际 EEPROM 需要完成编程扫描并确认 INIT。EEPROM 通过 ESC 的 EEPROM 接口访问，不经过 CoE/SDO 或 FoE。

烧录可以有意更改 Vendor ID 或 Product Code；差异在现有写入确认中显示，不阻止烧录，也不需要额外解锁。目标设备与自身的扫描快照核对，镜像验证和声明容量检查仍然有效。标识一致本身不能证明硬件或固件兼容。这是编程规则，与正常工作台的 ESI 身份匹配不同：[Microchip AN1910 §4.2](https://ww1.microchip.com/downloads/en/AppNotes/AN1910-Application-Note-DS00001920B.pdf) 描述了修改 EEPROM 身份再烧录，[Beckhoff ET9300 §14.2](https://download.beckhoff.com/download/Document/io/ethercat-development-products/an_et9300_v1i10.pdf) 描述了从 ESI 烧录选中的从站。

原生生成器覆盖附带 AX58100／LAN9252 文件使用的固定布局特性：ESC ConfigData、身份、Bootstrap／普通邮箱配置，以及 Strings、General、FMMU、SyncManager、固定 PDO 和已支持的 DC 时序字段。BITARR 保留正确的 EtherCAT 类型编号。生成时检查范围、分类区和字符串引用、配置区 CRC，镜像未使用部分用 `0xFF` 填充。镜像字符串优先选英文，其次无语言标记或第一个可用名称；切换界面语言不改变字节内容。

模块化布局、自定义 PDO 数据类型、Eeprom/Data 或自定义 ESI 分类区、不支持的 DC 时序选项，以及未知的镜像相关字段都会明确拒绝。能显示某份 ESI，不代表可以从它生成 EEPROM。超出该范围时，可导入厂商提供或 SSC／TwinCAT 导出的 BIN。BIN 导入保留范围有效的未知分类区；在合法 SII 结束标记后截止的短镜像，会明确补齐至声明容量。结构有效或回读成功，本身不代表与特定设备固件兼容，也不代表新配置已经生效。

## 语言与范围

文件有本地化名称时按界面语言选择，只有英文时在中文界面仍使用英文，不要求用户补充别名，也不自动翻译。设备原始字符串和标识保持不变。

工作台的 ESI 目录用于显示元数据，不执行 ESI 初始化、启动命令、PDO 重映射或模块插槽配置；EEPROM 镜像准备是单独的明确操作。寄存器或对象写入使映射失效后，需要重新扫描，才能恢复类型化 PDO 访问和周期交换。

PDO 发现在扫描阶段执行；在线对象字典在 PRE-OP 且周期交换停止时单独读取，流程有界，两者都不进入周期循环。CLI 扫描 JSON 包含 ESI 目录和数量、逐站匹配结果及 `pdoEntries`。XML 解析已嵌入，无需额外 XML 运行时。
