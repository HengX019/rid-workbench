# SN 型号识别 · 2.19

收到明确商品型号自述时保留该自述；否则按完整 20 字符 SN 的生产者及型号前缀匹配。ASTM 只对工厂序列号类型身份匹配，GB 使用产品身份字段。无匹配保留原始代码，绝不通过 MAC、配对名、登记号猜型号。识别不证明身份真实。

数据来自制造商公开声明与大疆官网公开文档选择器的 `temp_sn` 前缀，仅用于商品身份对应，不引入其他地区运行要求。官网的 `temp_sn` 是其提供的文档选择前缀，不是从个人 SN 推断。原始第三方响应未随本整理版分发，可通过型号数据中的来源链接核验。

收录 63 个前缀，52 个可唯一识别机型，3 个前缀存在多机型声明，显示候选机型并等待确认。不能宣称所有历史机型、区域版本和后续生产批次均已覆盖。

| 完整前缀 | 英文机型 / 候选 | 证据记录 |
|---|---|---|
| 1581F0M6 | mavic2zoom | RID000000758 |
| 1581F0UY | phantom4rtk | RID000000752 |
| 1581F11V | phantom4prov20 | RID000000759 |
| 1581F163 | mavic2pro | RID000000757 |
| 1581F1WN | mavicair2 | RID000000754 |
| 1581F1ZN | matrice300rtk | RID000000021 |
| 1581F37Q | fpv | RID000000756 |
| 1581F3N3 | mavicair2 | RID000000755 |
| 1581F3YT | air2s | RID000000007 |
| 1581F446 | agrast10 / agrast30（无法唯一确认） | RID000000038;RID000001163 |
| 1581F45T | mavic3 | RID000000009 |
| 1581F4BN | matrice30 | RID000000011 |
| 1581F4GC | mavic2enterpriseadvanced | RID000000753 |
| 1581F4QW | avata | RID000000008 |
| 1581F4QZ | mavic3cine | RID000000010 |
| 1581F4XF | mini3pro | RID000000001 |
| 1581F4Z4 | inspire3 | RID000000192 |
| 1581F574 | agrast20p / agrast40（无法唯一确认） | RID000000039;RID000001181 |
| 1581F5BK | matrice30t | RID000000012 |
| 1581F5BL | matrice30 | RID000000273 |
| 1581F5BM | matrice30t | RID000000274 |
| 1581F5FH | mavic3e | RID000000019 |
| 1581F5FJ | mavic3t | RID000000020 |
| 1581F5FK | mavic3m | RID000000057 |
| 1581F5YH | mini3 | RID000000062 |
| 1581F62H | matrice30t | RID000001136 |
| 1581F67P | mavic3classic | RID000000041 |
| 1581F67Q | mavic3pro | RID000000204 |
| 1581F6BU | agrast25 / agrast50（无法唯一确认） | RID000001594 |
| 1581F6CD | mini2se | DJI-DOC-2211;DJI-DOC-2096 |
| 1581F6GK | matrice350rtk | RID000000256 |
| 1581F6L5 | mini3pro | RID000000001 |
| 1581F6MK | mavic3procine | RID000000205 |
| 1581F6N8 | air3 | RID000000714 |
| 1581F6Q8 | matrice3td | RID000001167 |
| 1581F6QA | matrice3d | RID000001166 |
| 1581F6W8 | avata2 | RID000001376 |
| 1581F6Z9 | mini4pro | RID000000713 |
| 1581F7C6 | flycart30 | RID000000760 |
| 1581F7FV | matrice4e | RID000002198 |
| 1581F7K3 | matrice4t | RID000002199 |
| 1581F7V2 | flip | DJI-DOC-2146 |
| 1581F836 | agrast25p | RID000002587 |
| 1581F87L | neo | DJI-DOC-2111 |
| 1581F895 | air3s | RID000002046 |
| 1581F8DB | matrice400 | RID000002486 |
| 1581F8HG | matrice4td | RID000002247 |
| 1581F8HH | matrice4d | RID000002246 |
| 1581F8LQ | mavic4pro | RID000002375 |
| 1581F8PJ | mini4k | DJI-DOC-2076 |
| 1581F8ZL | agrast100 | RID000002576 |
| 1581F8ZX | agrast70p | RID000002579 |
| 1581F986 | mavic4pro | RID000002376 |
| 1581F9DE | mini5pro | RID000002581;DJI-DOC-2381 |
| 1581FA6Q | neo2 | DJI-DOC-2416 |
| 1581FA8J | avata360 | RID000002651 |
| 1581FAN4 | flycart100 | RID000002620 |
| 1581FB34 | litox1 | DJI-DOC-2561;DJI-DOC-2556 |
| 1581FBEX | mavic3ta | RID000002584 |
| 1581FBLK | avata360 | RID000002650 |
| 1581FBV5 | lito1 | DJI-DOC-2566 |
| 1581FCB6 | mini5pro | DJI-DOC-2381;DJI-DOC-2376 |
| 2137FDE1 | antigravitya1 | RID000002621 |

## 资料更新

- 型号库：MODEL_CODES.json，生成头文件：model_codes.h。
- 大疆官网：https://www.dji.com/euro-compliance 。四个新机型与 flip、mini2se、mini4k、新 mini5pro 前缀取自官网结构化公开数据；查阅其中 document_url 可获取厂家文件。
- A1：Antigravity 公开声明 RID000002621，2137FDE1。该前缀识别已加入，不代表接收链路已实机修复。
- 不将公开目录中不存在的机型等同于大陆不广播 RID。
- UOM 上次公开查询需要图形验证码，仍未实现自动查询；现场 SN 不会自动上传第三方。
