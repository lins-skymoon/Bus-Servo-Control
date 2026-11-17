# MM32F103CBT6 总线舵机控制板

更改日期：2026.11.17

本工程是 `bus.hex` 的 C 语言功能重建。工程包含启动向量、链接配置、外设驱动和应用逻辑，可在 Keil µVision 5 中逐文件编辑和重新编译。构建只使用本目录的源文件与 Arm 工具链。原固件的反汇编地址和板级测量对照见 [bus.hex 分析](../bus_analysis/README.md)。

## 打开与编译

1. 在 Keil µVision 5 打开 [bus.uvprojx](MDK-ARM/bus.uvprojx)。
2. 使用 `bus MM32F103CBT6` 目标。当前工程配置为 Arm Compiler 6.19 和 `MindMotion.MM32F103x8xB_DFP` 1.4.8，器件在 Pack 中显示为 `MM32F103CBT`。
3. 执行 **Project → Rebuild all target files**。烧录文件位于 [Objects/bus.hex](MDK-ARM/Objects/bus.hex)，内部 Flash 基地址是 `0x08000000`。外置 W25Q128 的已有动作组和配置保存在独立芯片上。


| 文件                                                         | 内容                                                   |
| -------------------------------------------------------------- | -------------------------------------------------------- |
| [Core/Src/main.c](Core/Src/main.c)                           | 启动顺序和主循环                                       |
| [Core/Src/startup_mm32f103.c](Core/Src/startup_mm32f103.c)   | 纯 C 启动入口和中断向量                                |
| [Core/Src/board.c](Core/Src/board.c)                         | 8 MHz HSE、48 MHz SYSCLK、GPIO、定时器蜂鸣器、电池 ADC |
| [Drivers/Src/links.c](Drivers/Src/links.c)                   | 三路 UART、74HC126 收发控制、SPI2 手柄接收             |
| [Drivers/Src/flash.c](Drivers/Src/flash.c)                   | SPI1、W25Q128 读写、页编程、扇区擦除                   |
| [Drivers/Src/usb_hid.c](Drivers/Src/usb_hid.c)               | 片上 USB HID Device、控制请求、IN/OUT                  |
| [Application/Src/controller.c](Application/Src/controller.c) | 控制命令、舵机参数、游戏手柄、按键示教                 |
| [Application/Src/actions.c](Application/Src/actions.c)       | 动作组上传、播放、删除、重复和调速                     |
| [MDK-ARM/bus.sct](MDK-ARM/bus.sct)                           | 内部 Flash 128 KiB、SRAM 20 KiB 布局                   |

## 引脚


| 引脚                      | 功能                                                     |
| --------------------------- | ---------------------------------------------------------- |
| PA0 / PA1                 | 74HC126 的 RX_CON / TX_CON，高电平使能                   |
| PA2 / PA3                 | 舵机 UART2 TX / RX，115200 8N1                           |
| PA4 / PA5 / PA6 / PA7     | W25Q128 的 CS / SCK / MISO / MOSI                        |
| PA8                       | 蜂鸣器，由 TIM2 调度                                     |
| PA9 / PA10                | UART1 TX / RX，9600 8N1                                  |
| PA11 / PA12               | MM32 USB D− / D+，HID Device，VID:PID `0483:5750`       |
| PB0 / PB1                 | 电池 ADC / LED2                                          |
| PB4 / PB5                 | Program / Run 按键，低有效                               |
| PB10 / PB11               | UART3 TX / RX，9600 8N1，蓝牙模组走线待实测              |
| PB12 / PB13 / PB14 / PB15 | SPI2 从机 NSS / SCK / MISO / MOSI，CH559T 对端走线待实测 |

串口和 USB 应用帧以 `55 55` 开始，普通帧的总长度等于长度字段加 2。舵机总线帧使用 ID、长度、命令、参数和反码校验。动作组使用外置 Flash 的 `LOBOT` 标记、`0x1200` 有效标记以及每组 `+0x2000` 轨道长度、`+0x2100` 播放序列和 `+0x3000` 的 128 字节动作记录。按键示教写入第 100 组。

### 串口 LED2 和蜂鸣提示

UART1 或 UART3 的完整帧进入命令处理时，LED2 的 PB1 从空闲低电平翻转约 150 ms。示教模式下命令处理暂停。帧头错误、普通长度字段越界、或未收齐整帧均会触发两声蜂鸣。超时从第一个 `55` 起计，UART1 为 180 ms，UART3 为 100 ms。上电前 500 ms 抑制串口错误蜂鸣。工程保留 UART3 的 `0x19` 长帧接收，使用 180 ms 超时。

可在串口工具的 HEX 模式发送 `55 55 02 0F` 验证 LED2 和电压回包。单独发送 `55 55 01` 验证长度错误蜂鸣，发送 `55 55 03` 后停发可验证超时蜂鸣。

### 低压报警与启动保护

上电先采两组电压，第二组低于 4400 mV 时进入锁存报警。运行中每 500 ms 采样，连续 11 次低于 5600 mV 触发预警。下一次采样达到 5600 mV 时解除预警。连续两次低于 4400 mV 会锁存报警，LED2 闪烁，前台命令和动作调度停在报警状态，恢复供电后需复位。

Flash 器件 ID 异常也进入启动报警。若能读出 Flash 但缺少 `LOBOT` 标记，工程初始化 `0x1200` 有效表和 `0x1300` 配置区，写入成功后再写标记。启动 ADC 无法取得有效采样时按启动故障处理。

### Bus Servo Control 上位机

已对照本机安装的 Bus Servo Control 3.7.0.0 及 GenericHid 2.2.1.1。工具的连接提示来自 Windows 对 HID 设备的枚举。设备收到 64 字节 HID OUT 报告后，按逻辑长度字段拼接命令并丢弃末尾填充。HID IN 回包固定为 64 字节，Windows HID API 在前面提供一个报告 ID 字节。

上位机发送普通动作组时，轨道表、播放顺序和舵机帧只带实际使用的字段。固件将它们补零到 Flash 的 256、200 和 128 字节存储单位。单组擦除和全部擦除返回 `55 55 02 08`，清除相应有效标记。普通动作帧最多容纳 41 个舵机条目，上传也检查动作组区边界，防止覆盖相邻组的索引。加密动作文件的下载路径尚未验证。

## 验证与兼容性

- 在本机 Keil µVision 5 中使用 Arm Compiler 6.19 完整重编译，0 错误、0 警告。
- Cortex-M3 指令仿真验证了复位向量、时钟和串口配置。使用原 HEX 逐字节核对 USB 设备、配置和 HID 报告描述符。模拟主机完成了设置地址、设置配置和应用握手。
- 从 UART1、UART3 和 HID OUT 各输入同一条单舵机命令，捕获到相同的 UART2 舵机帧。模拟 W25Q128 验证了读取、播放、完成通知、HID 五步上传以及删除全部标记。模拟 SPI2 数字手柄报文触发了对应动作组。
- UART1 与 UART3 的 GPIO 仿真覆盖了 LED2 脉冲、帧头错误、越界长度、收帧超时、两段蜂鸣波形，以及报警后恢复接收。官方串口示例帧也通过了回归验证。
- 以模拟 ADC 独立验证了上电检查、500 ms 采样间隔、5600 mV 预警、4400 mV 临界锁存，以及单次临界低压后恢复可清零连续计数。尚未在实板校准输入分压和触发电压。
- USB 产品字符串和固定序列号保留原固件的字节和长度。按安装版上位机的报文仿真验证了滑块单舵机、40 和 83 舵机批量位置、多报告动作帧、下载后播放、位置回包和两种擦除。上位机实板交互和 UART3 的 `0x19` 实际发送端仍待确认。
- 按键使用毫秒计时轮询，蜂鸣器由定时器驱动。按键边缘时序、长按重复、所有手柄组合键和舵机回包仍待实测。现有验证使用模拟外设，尚未烧录实板。
- CH559T 的 USB Host 与手柄 HID 解析运行在 CH559T 自身。这里重建的是 MM32 对其 SPI2 报文的接收，以及 MM32 的片上 USB HID Device。CH559T 固件及板上已有动作数据需分别从对应芯片取得。
