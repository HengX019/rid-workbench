# 第三方代码和数据

本文件说明随仓库分发的主要第三方材料。各文件中的版权声明、许可证及 NOTICE 同时适用。

- `RID_Workbench/opendroneid.c`、`opendroneid.h`、`opendroneidcn.c`、`opendroneidcn.h` 基于 Open Drone ID C Library，文件保留 Apache-2.0 声明和版权标记；许可证文本见 `RID_Workbench/LICENSE.opendroneid`。本项目版本包含对上游文件的修改，具体差异和来源仍待进一步核对。
- `RID_Dependencies/NimBLE-Arduino` 基于 NimBLE-Arduino 2.5.1，许可和版权通知保留在该目录及其子目录中。上游提交和本地启用扩展广播的配置修改见 `RID-LAB-PROVENANCE.txt`；其包含的 TinyCrypt 等组件遵循各自随附的条款。
- `RID_Workbench/map_picker.h` 的简化海岸线基于 Natural Earth；数据来源和使用限制见 `RID_Workbench/OFFLINE_MAP.md`。
- `RID_Workbench/MODEL_CODES.json` 和 `model_codes.h` 的型号对应依据列于 `RID_Workbench/MODEL_LOOKUP.md` 和数据条目中的来源链接。原始第三方响应和采集脚本未随此整理版分发；请访问各来源核验数据和再分发条件。

以上第三方许可不自动赋予本项目其余原创代码的使用许可。项目所有者尚需确定其原创部分的许可证并确认拥有发布权。
