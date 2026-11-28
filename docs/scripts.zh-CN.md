# Lua 脚本与自动化

[English](scripts.md) | **简体中文** | [使用指南](../README.zh-CN.md)

脚本页在独立线程运行 Lua 5.4，通过与界面相同的控制器扫描、控制状态、检查信号和执行自动化。Lua 不直接调用 SOEM，也不提供硬实时 PLC 循环。

## 编辑与运行

脚本为可执行文件旁 `scripts/` 中的 UTF-8 `.lua` 文件。“新建”立即增加一个不重名的草稿，双击列表名称可重命名。名称前的 `*` 表示未保存，切换到其他文件后标记也会保留。

运行前必须保存；存在未保存编辑时会报错，不会执行旧内容。“打开 / 重新加载”从磁盘读取当前文件，丢弃该文件缓存的编辑。未保存内容只保留在内存，退出应用后会丢失。

编辑器提供行号、语法颜色、等宽字体和格式化。API 片段只插入代码，不会执行。脚本消息和错误显示在全局日志中。同一时间运行一个脚本；运行中可查看其他文件。

用户删除的示例不会在启动时重新生成。`scripts/` 为空时，编辑器显示未保存的 `untitled.lua` 草稿，保存后才创建文件。

随包在 `scripts/` 中提供五个 `Example_*.lua`：真实硬件完整生命周期、信号检查、周期自动化、输出写入测试和延迟状态序列。真实硬件生命周期枚举物理网卡，需要抓包驱动。无硬件测试应明确使用模拟：

```lua
local bus = ecat.open({ simulation = true, cycle_us = 10000 })
local devices = bus:scan()
log.info("devices: " .. #devices)
bus:disconnect()
```

## 连接与操作

`ecat.current()` 取得面向当前工作台连接的 Lua 句柄。`ecat.open(options)` 复制当前连接设置并覆盖指定选项，只创建句柄，不扫描。选项为 `interface`（或 `primary`）、`secondary`、`redundant`、`overlap`、`simulation` 和 `cycle_us`。

运行脚本前先选择“模拟”或“硬件”。切换模式要求脚本停止且总线断开；编程模式不能运行脚本。完整硬件生命周期示例应先选择“硬件”，上方无硬件示例应先选择“模拟”。网卡枚举仅返回物理接口资料，不改变所选模式，也不替换或断开总线。

总线操作等待主机结果后返回，失败时产生 Lua 错误，不需要 `await` 包装。

| API | 行为 |
| --- | --- |
| `ecat.adapters()` | 枚举有线物理接口，返回 `name`、`description`；不切换模式、打开总线或扫描从站 |
| `ecat.current()` | 按当前工作台连接设置取得句柄 |
| `ecat.open(options)` | 按显式覆盖后的连接设置取得句柄 |
| `bus:scan()` | 配置和映射从站，结束于 PRE-OP，返回含 `position`、`name`、`state`、`dc` 的数组 |
| `bus:disconnect()` | 关闭当前总线连接 |
| `bus:state(target, state)` | 对 `"all"` 或数字从站位置请求 INIT、PRE-OP、SAFE-OP、OP；SAFE-OP/OP 启动周期 |
| `bus:start()` | 在 SAFE-OP/OP 启动周期交换 |
| `bus:stop()` | 停止周期交换，并将 OP 从站降至 SAFE-OP |
| `bus:cycle(cycle_us)` | 停止交换且全部从站为 INIT/PRE-OP 时修改目标周期 |
| `bus:status()` | 读取已发布的状态快照，不发起总线事务 |
| `bus:signals(slave)` | 列出映射中有名称且非填充的信号描述 |
| `bus:signal(slave, name)` | 取得 PDO 信号句柄，解析操作时拒绝有歧义的名称 |
| `bus:signal(slave, index, subindex)` | 按对象标识取得 PDO 信号句柄 |
| `signal:read()` | 返回解码后的 PDO 字符串值 |
| `signal:write(value)` | 写入类型化输出；读取的值可原样传入 |
| `bus:register(slave, offset)` | 取得 ESC 句柄；`:read(length)` 返回 HEX 字节，`:write(hex_bytes)` 写入字节 |
| `bus:sdo(slave, index, subindex)` | 取得 CoE SDO 句柄，使用相同字节读写方法 |
| `log.info(text)`、`log.warn(text)`、`log.error(text)` | 写入全局日志，`print(text)` 为 info 别名 |

从站位置从 1 开始；对全部从站切换状态时明确使用 `"all"`。周期单位为微秒，可选 500、1000、2000、5000、10000、20000、50000、100000。

`bus:status()` 返回 `mode`（`simulation`、`hardware` 或 `programming`）、`connected`、`mapped`、`running`、`busy`、`generation`、`cycles`、`wkc`、`expected_wkc`、`slaves`（数量）、`state`、`status`。信号描述包含 `name`、`input`、`type`、`bit_offset`、`bit_length`、`index`、`subindex`。

信号名称与对象地址因设备而异，不应假定 Input 之类名称唯一；可以先查看描述：

```lua
local bus = ecat.current()
assert(bus:status().connected, "scan the bus first")
for _, signal in ipairs(bus:signals(1)) do
    if signal.input then
        local value = bus:signal(1, signal.index, signal.subindex):read()
        log.info(signal.name .. " = " .. value)
    end
end
```

连接代次用于拒绝旧上下文中的排队命令，Lua 句柄本身不是永久绑定代次的地址。扫描或映射变化后应重新发现并绑定信号。同步 ESC/SDO 调用要求停止周期交换。写入始终检查状态、方向、映射和范围，不设置额外写入许可开关。

## 定时器与周期回调

| API | 行为 |
| --- | --- |
| `timer.after(duration, fn)` | 注册单次回调，返回数字任务 ID |
| `timer.every(duration, fn)` | 注册重复回调，返回任务 ID |
| `timer.cancel(id)` | 移除 after/every 任务 |
| `cycle.every(cycles, fn)` | 在收到的快照中观察到每经过 N 个周期后调用，返回任务 ID |
| `cycle.cancel(id)` | 移除周期任务 |

数字定时参数单位为毫秒；`"1s"`、`"1000us"` 等字符串在解析后不少于 1 ms 时也可使用。小于 1 ms 会被拒绝。主机定时器适用于轮询和超时，不保证恰好在 EtherCAT 周期边沿执行。

周期参数是正整数周期数：1 跟随收到的周期快照，2 至少等待两个周期，依此类推。回调参数 `frame` 包含 `cycles`、`missed`、`wkc`、`expected_wkc`、`connected`、`running`、`status`，以及含输入/输出 HEX 快照的 `slaves` 数组。主站在后台向脚本传递周期快照和命令结果，拖动窗口时也能继续执行。脚本线程落后时会从最新收到的周期继续，跳过中间的回调。

回调串行执行，默认最多同时存在 64 个定时/周期任务。取消成功返回 `true`；不存在、已结束、已取消或类型不匹配的 ID 返回 `false`。ID 必须是数字类型的正整数，无效参数产生 Lua 错误。

回调可取消自身或其他任务，取消会阻止后续调用，当前调用仍执行到返回。自取消时先声明局部 ID，再赋值：

```lua
local count = 0
local timerId
timerId = timer.every(100, function()
    count = count + 1
    log.info("tick " .. count)
    if count >= 10 then timer.cancel(timerId) end
end)

local cycleId = cycle.every(2, function(frame)
    log.info("cycle " .. frame.cycles)
end)
timer.after(2000, function()
    cycle.cancel(cycleId)
end)
```

顶层代码返回且没有已注册任务时，脚本自然结束。重复任务会保持运行，包括等待已停止/断开总线的周期任务。脚本“停止”按钮取消全部任务与等待；`bus:stop()` 只停止总线 I/O。

## 长期运行与错误

建议先做简短初始化，再注册每次都能完成的回调。Lua 支持 `while`、`repeat`，但无限忙循环阻止其他回调并触发执行保护。回调捕获的局部变量在各次执行间保留。

```lua
local bus = ecat.current()
local polls = 0
timer.every(1000, function()
    local status = bus:status()
    polls = polls + 1
    log.info(string.format("poll %d: %s WKC=%d/%d",
        polls, status.state, status.wkc, status.expected_wkc))
end)
```

每次运行在脚本线程创建新的 Lua 状态。内部状态为 Empty、Ready、Running、Waiting、Stopped、Error，界面显示运行/停止指示。顶层语法或运行错误结束本次运行；回调错误会记录日志，重复回调仍保留，直到取消或停止。预期失败可通过 `pcall` 捕获：

```lua
local ok, result = pcall(function()
    return ecat.current():register(1, 0x0130):read(2)
end)
if ok then log.info(result) else log.error(result) end
```

除非脚本明确断开，停止脚本不会关闭总线。停止阻止后续调度，但不能撤销控制器已接受的写入。

编辑器使用 64 KiB 源码缓冲区。运行时限制内存、源码、日志和注册任务，主机请求默认超时为 30 秒，顶层代码与每次回调分别使用执行预算。文件、OS、package、debug、网络库不开放。正常长期回调不会耗尽整个脚本生命周期累计的指令预算。
