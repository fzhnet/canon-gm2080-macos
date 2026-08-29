# 佳能 GM2080 的 macOS 驱动

[English](README.md) · [简体中文](README.zh-CN.md)

佳能**没有为 GM2000/GM2080 系列发布过 macOS 驱动**，而这台打印机**不支持任何免驱协议**
—— 没有 AirPrint、没有 IPP、不认 PDF、不认 PCL。本仓库用两种办法让它在 macOS 上能打印。

开发环境：GM2080（固件 1.050），macOS 26.5，Apple Silicon。

| | [`native/`](native/) | [`bridge/`](bridge/) |
|---|---|---|
| 是什么 | 真正的 CUPS 驱动：`.pkg`、PPD、滤镜 | 容器里跑佳能的 Linux 驱动，再以 IPP 暴露出来 |
| 依赖 | 无 | Docker Desktop |
| 架构 | arm64 + x86_64 原生 | amd64，走 Rosetta |
| 含佳能代码 | 无 | 佳能官方闭源驱动 |
| 状态 | **可用 —— 已在 GM2080 上打印成功** | 可用，但未在纸上验证 |

原生驱动已经端到端跑通：从安装包装上、添加队列、打印成功。
其余验证项详见[验证状态](#验证状态)。

## 该用哪个

**先用 `native/`。** 它就是一个普通驱动：装个包、添加打印机，完事，后台不留任何常驻进程。

**`native/` 出问题再退回 `bridge/`。** 它把页面交给佳能自己的驱动处理，
输出天然正确 —— 代价是打印时必须有一个容器在运行。

## native —— 安装

从 [Releases](../../releases/latest) 下载安装包，然后：

```bash
sudo installer -pkg CanonGM2080Native-1.0.pkg -target /
./native/add-printer.sh <打印机IP>
```

也可以自己编译，需要 Xcode 命令行工具（`xcode-select --install`）：

```bash
cd native && ./build.sh
sudo installer -pkg dist/CanonGM2080Native-1.0.pkg -target /
./add-printer.sh <打印机IP>
```

安装包未经签名，双击会被 Gatekeeper 拦下，上面的 `installer` 命令才是正确路径。

> **在"系统设置"里手动添加打印机时，"协议"必须选「HP Jetdirect — 插口」，不能选 IPP。**
> IPP 恰恰是这台打印机唯一不支持的协议，而 macOS 默认就选中它 ——
> 选错会弹出"目前无法与打印机通信"。

安装的内容是两个滤镜，外加每个机型系列一份 PPD：

```
/Library/Printers/canon-gm2080/rastertocanonijgm     页面渲染
/Library/Printers/canon-gm2080/cmdtocanonijgm        维护功能
/Library/Printers/PPDs/Contents/Resources/canongm{2000,2080,4000,4080}-native.ppd
```

维护与墨量：

```bash
./native/maintenance.sh nozzle          # 喷嘴检查、清洗、深度清洗、对齐
./native/ink-level.sh <打印机IP>         # 通过 SNMP 读取耗材余量
```

## bridge —— 安装

需要 Docker Desktop 正在运行，并设为开机自启。

```bash
cp bridge/docker/.env.example bridge/docker/.env
# 编辑 bridge/docker/.env，填入 PRINTER_IP
./bridge/scripts/install.sh
```

卸载用 `./bridge/scripts/uninstall.sh`（加 `--purge` 连镜像一起删）。

## 这台打印机到底怎么工作

GM2080 通过 SNMP 报告的设备 ID：

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP,CHMPu;MDL:GM2080 series;
```

`CMD:` 是它能解析的数据格式的**完整清单**。没有 PDF、没有 PostScript、没有 PCL、
没有 PWG Raster、没有 URF。631 端口是关闭的，打印机自己的 Web UI 也报告
`g_ipp_over_usb = 0` —— 网络和 USB 两侧都没有 IPP。

这解释了两件事：为什么单靠写 PPD 不可能解决问题，以及为什么 AirPrint 根本无从谈起。

而它**真正接受**的东西其实很简单 —— 纯文本 XML 命令包着标准 PWG Raster 载荷，
直接拼接，没有任何二进制封帧：

```
StartJob                                   ┐
SetJobConfiguration                        │ IVEC XML
SetConfiguration     (纸张、颜色、双面)      ┘
  VendorCmd  nextpage=ON   ┐
  SendData   datasize=N    │ 每页一组
  <PWG Raster 页面数据>     ┘
  ...
  VendorCmd  nextpage=OFF     ← 仅最后一页
  SendData / <PWG Raster 页面数据>
EndJob
```

没有长度前缀、没有校验和、没有转义。macOS 本来就能产出 PWG Raster，
所以原生驱动**不需要任何佳能代码** —— 它只负责写外层信封，栅格数据原样透传。

每个字段是怎么确定的，详见
[`docs/2026-08-28-protocol.md`](docs/2026-08-28-protocol.md)。

## 验证状态

**原生驱动已验证的部分：**

- **打印机确实接受这个数据流并能打印。** 已在 GM2080（固件 1.050）、
  macOS 26.5、Apple Silicon 上通过真实 CUPS 队列确认 ——
  这同时也验证了滤镜在 `cupsd` 沙箱下的运行。
- 输出与佳能自家驱动在**字节结构上一致**：相同的命令块、相同的顺序、
  相同的命名空间前缀和逐块命名空间声明，单页和多页作业都是如此
  （三页作业的 `nextpage` 为 ON/ON/OFF，每页一个独立 PWG 流，
  每个声明的 `datasize` 都精确等于其实际载荷）。
- 栅格几何与佳能完全一致 —— A4 在 600 dpi 下为 4800×6826，每行 14400 字节，
  8 位色深，24 位每像素，sRGB。
- 纸张与介质类型对照表是从佳能驱动**读回来的**，不是猜的。
- 编译为通用二进制且零警告；PPD 通过 `cupstestppd` 校验。
- 作业标题和用户名会做 XML 转义，所以名为 `P&L <draft>.pdf` 的文件
  依然能产出格式良好的命令块。

**仍然开放的部分：** 只有 GM2080 在真机上跑过。GM2000、GM4000、GM4080
的 PPD 由同一份模板生成，行为应当一致，但没有人在设备上确认过 ——
见 [`native/docs/models.md`](native/docs/models.md)。
`bridge/` 那条路径也还没有实际打印过。

如果你测试了，**无论成功与否都请开一个 issue 告诉我结果。**

## 排障

**作业排队但打不出来。** 先确认打印机可达：

```bash
nc -z -G 5 <打印机IP> 9100 && echo reachable
```

**添加打印机时提示"目前无法与打印机通信"。** 协议选成 IPP 了，改成
「HP Jetdirect — 插口」，"队列"一栏留空。

**原生驱动：看滤镜在做什么。**

```bash
cupsctl --debug-logging
lp -d Canon_GM2080 somefile.pdf
tail -f /var/log/cups/error_log
```

**bridge：检查容器。**

```bash
docker ps --filter name=canon-gm2080-bridge
docker compose -f bridge/docker/docker-compose.yml logs -f
```

**bridge：agent 日志里出现 `Operation not permitted`。**
launchd 无法执行 `~/Documents` 下的任何脚本 —— 它启动时不带该目录的 TCC 权限，
会以 exit 126 失败。`install.sh` 正是因此把启动器放在
`~/Library/Application Support/`。如果你移动了项目位置，重新跑一次安装脚本。

## 本项目分发的内容

`native/` **不含任何佳能代码**。它是对命令信封的独立实现，
依据观察到的行为写成，与本仓库其余部分一样采用 MIT 许可（见 [LICENSE](LICENSE)）。

`bridge/` 同样不含佳能代码：它构建的镜像会在**构建时**从
[Ordissimo PPA](https://launchpad.net/~thierry-f/+archive/ubuntu/fork-michael-gruz)
安装 `cnijfilter2`（闭源，由佳能按其自有许可分发）。
本仓库没有内置任何专有文件，但构建该镜像意味着你接受佳能对该软件包的许可条款。

Canon、PIXMA、IVEC 是佳能公司的商标。本项目不是佳能产品，与佳能无关，也未获其背书。

## 两条已被排除的死路

[`docs/2026-08-23-investigation.md`](docs/2026-08-23-investigation.md)
记录了两条看起来可行、实则走不通的路线，以及否定它们的具体证据 ——
免得后来人再花一天重新发现一遍：

- **AirPrint / IPP Everywhere。** 这台打印机根本不说 IPP。
- **给佳能的 macOS IJ 驱动框架套一个 PPD。** 那套框架在很多 Mac 上都装着，
  而且确实实现了 IVEC，但它需要一份佳能从未为 GM 系列构建过的**逐机型二进制表**
  —— 而且没有任何其他机型的表能匹配这台单黑双仓机器。
