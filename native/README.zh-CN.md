# 佳能 GM2080 —— macOS 原生驱动

[English](README.md) · [简体中文](README.zh-CN.md)

一个真正的 CUPS 驱动：两个小滤镜加一份 PPD，由 `.pkg` 安装。
不需要 Docker、不需要 Rosetta、不含任何佳能二进制。通用二进制（arm64 + x86_64）。

佳能没有为 GM2000/GM2080 系列发布 macOS 驱动，而这台打印机不支持任何免驱协议，
所以本驱动直接实现了佳能自己的传输格式。参见[工作原理](#工作原理)。

## 支持的机型

同一款硬件在不同地区用不同型号销售。一个驱动覆盖全部四个系列 —— 它们的能力完全相同。

| 系列 | 零售型号 | 地区 | 扫描 |
|---|---|---|---|
| GM2000 | GM2070 | 印度、南亚及东南亚 | 无 |
| **GM2080** | **GM2080** | **中国** | 无 |
| GM4000 | GM4070 | 印度、南亚及东南亚 | 有 |
| GM4080 | GM4080 | 中国 | 有 |

只有 GM2080 在真实硬件上验证过。日本市场的 GM2030 / GM4030 可能也能用，
但佳能的 Linux PPD 里没有它们，因此没有证据。
详细依据以及如何反馈你的机型：[docs/models.md](docs/models.md)。

## 安装

从 [Releases](../../releases/latest) 下载 `.pkg`，然后：

```bash
sudo installer -pkg CanonGM2080Native-1.0.1.pkg -target /
./add-printer.sh 192.168.1.50                        # 你的打印机地址
./add-printer.sh 192.168.1.50 Office_Mono            # ...指定队列名
./add-printer.sh 192.168.1.50 Office_GM4070 gm4000   # ...再指定机型系列
```

安装包未签名，双击会被 Gatekeeper 拦下 —— 上面的 `installer` 命令才是正确路径。
需要签名并公证的包，见[构建](#构建)。

> **手动在"系统设置"里添加时，"协议"要选「HP Jetdirect — 插口」。**
> macOS 默认选中的 IPP 恰好是这台打印机唯一不支持的协议，选它会报
> "目前无法与打印机通信"。"队列"一栏留空。
>
> 顺带一提，"HP Jetdirect" 只是个历史遗留的名字，它不是 HP 的私有协议 ——
> 端口 9100 的标准名称是 PDL Data Streaming，本质上就是一条裸 TCP 连接，
> 各家打印机通用。

安装内容为两个滤镜，外加每个系列一份 PPD：

```
/Library/Printers/canon-gm2080/rastertocanonijgm     页面渲染
/Library/Printers/canon-gm2080/cmdtocanonijgm        维护功能
/Library/Printers/PPDs/Contents/Resources/canongm{2000,2080,4000,4080}-native.ppd
```

## 维护功能

```bash
./maintenance.sh nozzle       # 打印喷嘴检查图案
./maintenance.sh clean        # 清洗打印头
./maintenance.sh deepclean    # 深度清洗（耗墨明显更多，会先确认）
./maintenance.sh systemclean  # 系统清洗（大量耗墨；未经验证）
./maintenance.sh align        # 自动打印头对齐
```

打印机 Web UI 还有另外三项 —— 滚轴清洁、底板清洁、打印对齐数值。
它们没有做进来，是因为其传输格式并非 IVEC XML，
在任何佳能二进制或抓包中都无法确定编码方式；这三项请用 Web UI 操作。
`systemclean` 虽然实现了，但它的 `type` 取值是**推断**而非抓包得来的，
因此它要求你输入一整句确认。每一项的依据都记在
[docs/protocol.md](docs/protocol.md)。

队列名不是 `Canon_GM2080` 的话，作为第二个参数传进去。

这些本质上是 CUPS 命令作业，不用脚本也能发：

```bash
printf '#CUPS-COMMAND\nClean all\n' > /tmp/c
lp -d Canon_GM2080 -o document-format=application/vnd.cups-command /tmp/c
```

**建议顺序**：先 `nozzle`。图案有断线就 `clean`，然后再 `nozzle` 复查。
只有普通清洗解决不了才动 `deepclean` —— 它消耗的是你手动补充的墨水。

## 墨水余量

```bash
./ink-level.sh 192.168.1.50
```

```
  Canon Black Ink Tank     [###                     ]  14%  LOW - refill soon
  Fixed Ink Absorber 1     [###################     ]  81%
  Fixed Ink Absorber 2     [#################       ]  71%
```

打印机支持标准 Printer MIB，所以这是直接从设备读的。
两个 absorber 是废墨吸收垫，属于随清洗次数增长的**耗材部件**，不可补充；
只有 Ink Tank 是你能加墨的那个。

### 为什么"打印机与扫描仪"里显示"信息不详"

CUPS 只在**作业运行期间**刷新耗材数据：socket 后端一边打印一边查 SNMP，
再把结果回报给 CUPS。队列一次都没打印过，就没有缓存，那个面板自然是空的。
随便打一张就会出现。

如果你还在用容器桥接方案，即使打印过面板也可能是空的 ——
因为此时 macOS 是在跟容器里的 CUPS 通信，而不是跟打印机。
原生驱动直连打印机，没有这个问题。

## 构建

```bash
./build.sh
```

签名并公证（需要付费的 Apple Developer 账号）：

```bash
xcrun notarytool store-credentials canon-gm2080 \
    --apple-id you@example.com --team-id TEAMID     # 交互式，只需做一次

SIGN_APP="Developer ID Application: Your Name (TEAMID)" \
SIGN_PKG="Developer ID Installer: Your Name (TEAMID)" \
NOTARY_PROFILE=canon-gm2080 \
./build.sh
```

`build.sh` 会在编译前先校验签名身份和公证凭据，为两个滤镜加上
hardened runtime 与安全时间戳，提交公证，失败时自动拉取苹果的日志，
成功后装订票据，最后用 `spctl` 验证结果。

**任何凭据都不会传给 `build.sh`。** `store-credentials` 会提示你输入
App 专用密码并存进钥匙串；构建过程只引用钥匙串配置的名字。

## 工作原理

打印机的 IEEE-1284 设备 ID：

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP,CHMPu;MDL:GM2080 series;
CID:CA_IVEC1TYPE2_IJP;
```

`CMD:` 是它能解析的格式的完整清单 —— 没有 PDF、没有 PostScript、没有 PCL、
没有 PWG Raster、没有 URF。631 端口关闭，打印机自己的 Web UI 报告
`g_ipp_over_usb = 0`，所以任何一侧都没有 IPP。

而它真正想要的东西其实很简单：**用纯文本 IVEC XML 命令包裹的 PWG Raster**，
发到 9100 端口。

```
<?xml …><cmd …><ivec:contents><ivec:operation>StartJob</ivec:operation>…
SetJobConfiguration
SetConfiguration           纸张、介质类型、颜色模式、双面
  VendorCmd  nextpage=ON   ─┐
  SendData   datasize=N     ├─ 每页一组，直到最后一页才 OFF
  <PWG raster 数据流>       ─┘
EndJob
```

没有二进制封帧、没有长度前缀、没有校验和、没有转义 ——
这正是它不需要任何佳能代码的原因。维护功能用同样的信封，
把 `servicetype` 换成 `maintenance`，把栅格数据换成 `Cleaning` 或 `TestPrint` 操作。

格式是通过**差分分析**确定的：在容器里跑佳能自己的 Linux 驱动，
喂给它已知输入，抓下它产出的字节，再复现出来。
上层目录的 `docs/` 记录了完整证据。

### 已验证

- **可以打印。** 已在 GM2080（固件 1.050）、macOS 26.5、Apple Silicon 上确认：
  从安装包装上、用 `add-printer.sh` 添加队列、通过真实 CUPS 队列打印成功 ——
  这同时也验证了滤镜在 `cupsd` 沙箱下运行、以及它使用 CUPS 导出的 `TMPDIR`。
- 栅格几何与佳能完全一致：600 dpi 下 4800 × 6826，每行 14400 字节，
  8 位色深，24 位每像素，sRGB —— 是**可打印区域**，不是整张纸。
- 多页结构一致：每页一组 `VendorCmd`+`SendData`，`nextpage` 为 ON/ON/OFF，
  每页一个完整的 PWG 流，每个声明的 `datasize` 都等于其实际载荷。
- 维护块的操作名、`servicetype`、参数取值和字段顺序都与佳能一致。
- 命名空间前缀和逐块的命名空间声明，在抓取到的全部 12 个块上都与佳能一致。
  剩余的每一个字节差异都有交代，且都是刻意为之 —— 见
  [docs/protocol.md](docs/protocol.md)。
- 作业标题和用户名会做 XML 转义，所以名为 `P&L <draft>.pdf` 的文件
  依然能产出格式良好的命令块。
- 双面设置从 PPD 解析：队列**默认**双面能被正确继承，显式指定单面仍然优先，
  短边装订也能真正传到打印机。macOS 的 RIP 不会填充栅格页头里的
  Duplex/Tumble 字段（Linux 会），所以由滤镜自己写入 ——
  见 [docs/protocol.md](docs/protocol.md)。

### 仍然开放

- 只有 GM2080 在真机上跑过。另外三个系列的 PPD 由同一份模板生成，
  行为应当一致，但未经设备确认 —— 见 [docs/models.md](docs/models.md)。
- 五个维护命令里，只有 `systemclean` 的 `type` 取值是推断的，
  其余四个都是从佳能驱动抓包读回来的。理由见
  [docs/protocol.md](docs/protocol.md)。

## 许可

MIT。不含任何佳能代码。Canon、PIXMA、IVEC 是佳能公司的商标。
本项目不是佳能产品，也未获佳能背书。
