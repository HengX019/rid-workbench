# RID Workbench

基于 ESP32-S3 的无人机远程识别（RID）接收与实验工作台，面向中国大陆广播场景。手机或电脑连接设备热点后，可在浏览器查看收到的身份和位置，也可在发射界面使用模拟数据。

> 当前版本：2.21。新增选点地图双指缩放和鼠标滚轮缩放。此前 2.20 已在一块 ESP32-S3 N16R8 上编译、烧录并读取启动状态；2.21 的手势逻辑通过 Node 回归检查和固件编译，尚未进行手机触摸或电脑鼠标实机验证。它不是经过认证的接收设备。

## 功能

- 监听设备支持的 **2.4 GHz Wi-Fi 和 Bluetooth LE** RID 广播，展示收到的身份、位置和其他有效字段；不接收 5.8 GHz Wi-Fi。
- 根据广播中明确的型号自述或本地 SN 前缀库显示英文机型名，例如 `mavic4pro`、`mini5pro`、`neo2` 和 `antigravitya1`。前缀未收录或存在歧义时保留未知或候选信息，不能保证识别全部机型和生产批次。未实现 UOM 按 SN 自动查询。
- 支持单机和多机模拟广播，使用虚拟身份和轨迹；其他接收器能否显示型号取决于其解析方式。请仅在适合的受控环境中使用发射功能。
- 发射选点地图内置中国及周边的简化海岸线和城市，断网可用。可向浏览器导入自行取得的 XYZ 离线瓦片；内置概览没有街道和建筑。IP 粗定位与地点搜索需要联网。
- 电脑客户端可在预先启用后台监听后检测相应 ESP32 USB 插入。网页通过设备 Wi-Fi 访问；USB 用于供电和检测设备。macOS 提供应用源码与构建脚本；Windows 提供源码与本机编译脚本，尚未在 Windows 实机验证。

## 硬件与使用

- ESP32-S3 N16R8，16 MB Flash，8 MB OPI PSRAM。
- 设备启动后连接其 Wi-Fi 热点，浏览器访问 `http://192.168.4.1/`。新设备默认热点名称 `RID-Test-ESP32`；已有设备沿用保存的热点设置。默认热点无密码，设备控制通过局域网 HTTP 提供；在公开场所使用前，请在设备设置中设置热点密码，避免不受信任的人连接。
- 点击“开始监测”查看接收记录。飞机未发送位置、接收链路不匹配或信号未被收到时，页面无法凭空显示数据。
- 固定参数和模拟发射页面均支持单指或鼠标拖动、点击选点；手机双指捏合缩放，电脑鼠标滚轮缩放。缩放以双指中点或鼠标位置为中心，不会改变选定坐标。详细离线瓦片的格式和浏览器存储限制见 `RID_Workbench/OFFLINE_MAP.md`。
- 设备设置提供短时接收诊断和导出。分享诊断前请自行去除位置、序列号等不希望公开的信息。

影翎 A1 起桨后用户报告未识别；现有记录不能证明原因，仍需实际运行时的室外原始诊断。不要将型号库条目理解为该机型已完成实机兼容验证。

## 从源码构建固件

以下组合用于 2.21：Arduino CLI 1.5.2-rc.1、Espressif Arduino ESP32 core 3.3.12、NimBLE-Arduino 2.5.1。需要先安装 Arduino CLI 和相应 ESP32 core。将 NimBLE-Arduino 2.5.1 放在 `RID_Dependencies/NimBLE-Arduino`，或将命令中的 `--library` 改成该库的实际绝对路径。运行目录为仓库根目录：

```sh
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc' \
  --library RID_Dependencies/NimBLE-Arduino \
  --build-path build \
  RID_Workbench
```

烧录前先核实板型、端口与现有分区布局。初次安装时，在核实板型和全新设备的分区布局后，写入构建输出的引导程序、分区表和应用：

```sh
esptool --chip esp32s3 --port PORT --baud 460800 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 build/RID_Workbench.ino.bootloader.bin \
  0x8000 build/RID_Workbench.ino.partitions.bin \
  0xe000 build/boot_app0.bin \
  0x10000 build/RID_Workbench.ino.bin
```

升级现有相同分区布局的设备可以只写入应用区域，避免擦除配置：

```sh
esptool --chip esp32s3 --port PORT --baud 460800 write-flash \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x10000 build/RID_Workbench.ino.bin
```

将 `PORT` 替换为实际串口；不要对不确定分区布局的设备使用上述升级命令。首次安装会覆盖所列区域，因此已有设备请先备份并核实。构建成功不代表射频接收或网页功能已经验证。

## 电脑客户端

参见 `PC_Client/使用说明.md`。自动弹窗需要先安装客户端并启用登录后后台监听；单纯插入 USB 不会自动安装程序。当前检测 USB VID/PID `303A:1001`，同类设备可能误触发，使用其他 USB 接口的板子可能无法触发。

## 项目资料

- `RID_Workbench/MODEL_LOOKUP.md`、`RID_Workbench/MODEL_CODES.json`：型号库和依据。
- `RID_Workbench/OFFLINE_MAP.md`：离线地图、数据来源及瓦片格式。
- `RID_Workbench/ANTIGRAVITY_A1.md`：影翎 A1 的现有信息和未解决问题。
- `RID_Workbench/SOURCES.md`：标准、设备和上游项目资料。

## 许可和数据来源

项目自身的开源许可证仍待作者确定。第三方组件分别遵循其原有许可；第三方许可、来源及修改说明见 `THIRD_PARTY.md`。公开发布前，应核查各来源文件的再分发条件。

## 地图交互回归检查

需要 Node.js，运行 `node --test tests/map_gestures.test.cjs`。检查缩放锚点、双指松开不误选、鼠标拖动、缩放边界、禁用状态和整数瓦片地址；这是模拟事件检查，不替代 Safari 或其他浏览器的真机验证。
