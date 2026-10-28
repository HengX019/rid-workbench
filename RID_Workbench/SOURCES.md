# 依据 · 2026-10-26

- 民航局发布的 GB 46750—2025 正文：
  https://www.caac.gov.cn/XXGK/XXGK/BZGF/BZGF_GJBZ/202601/P020260120357575326873.pdf
  核对 5.1.2（运行区间）、5.2.1/表 1（包结构）、5.2.3/表 2（必选和可选标识）、5.2.4/表 3（登记号 NULL、旧序列号填充、未知值）、6.1（BLE/Wi-Fi 链路）、8.2.1.2（制造商触发条件）。
- 国家标准 GB/T 41300—2022：
  https://openstd.samr.gov.cn/bzgk/std/newGbInfo?hcno=F0352B4E119D73AC2DA380878B7A2714
  国标引用的产品识别码结构为生产者代码、产品型号代码和序列号。产品型号代码不等同于商品名；本版不内置未经核验的型号代码映射。
- ESP32-S3 厂商数据手册：
  https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf
  2.4 GHz Wi-Fi 与 Bluetooth 5 LE；本硬件无 5.8 GHz Wi-Fi 接收能力。
- 影翎大陆官方产品说明：
  https://www.antigravity.tech/cn/drone/antigravity-a1
- 影翎官方 RID 链路与地区说明：
  https://www.antigravity.tech/li/knowledge/post?id=a714131b1fa0464397f721c0971bd262
- OpenDroneID 原始 C 实现（兼容数据解码，不作为大陆运行条件）：
  https://github.com/opendroneid/opendroneid-core-c/tree/master/libopendroneidcn
  https://github.com/opendroneid/opendroneid-core-c/tree/master/libopendroneid

本次代码变更涉及厂商 AD 和 IE 的有界兼容接收。国标未在所核对章节统一规定每种厂商封装的全部外层标识；增加这些路径不意味着证实某个指定机型采用该封装。

## 2.18 型号对应关系

见 MODEL_LOOKUP.md 的逐项声明引用、MODEL_CODES.json 的 source_url 及数据条目所列的生产者公开声明链接。它们提供身份范围与商品型号的对应数据，不作为中国大陆运行规定。UOM 公开查询观察单独记录，未声称 UOM 自动查型已实现。
