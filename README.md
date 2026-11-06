# AMD Microphone
AMD Digital Microphone kext for modern AMD laptops with AMD's Audio Co-Processor (ACP).

### NOTE: I no longer do hackintosh development. PR is welcome.

<p align="center">
  <img src="https://github.com/qhuyduong/AMDMicrophone/assets/16358771/4e2b9655-e53a-4433-a1f4-9b926bcec14b" width="70%">
</p>

## What does this kext do?
In some modern AMD laptops, microphones are no longer connected to High Definition Audio bus.
They are now controlled by a dedicated Audio Co-Processor, hence obviously no driver in macOS.
This kext brings support for them. Refer [FAQ](#faq) for more information.

## What's working?
* Microphone for Renoir-base laptops with digital microphone (aka ACP-based mic), see [this](#check-if-your-laptop-is-equipped-with-an-acp-based-microphone) to identify if your laptop is supported.

## What's not working?
* This kext may not work for other CPU variants than Renoir, such as Raven/Raven2/Fire Flight/Van Gogh/Yellow Carp.

## Prerequisites 
### Check if your laptop is equipped with an ACP-based microphone
#### Windows
Make sure you have a working Microphone Array device.

<p align="center">
  <img src="https://github.com/qhuyduong/AMDMicrophone/assets/16358771/bc4ccf6b-35e6-49ac-b6ac-18c794600d3f" width="40%">
</p>

#### Linux
Make sure default source device is ACP

```console
$ pactl get-default-source | grep acp
alsa_input.pci-0000_03_00.6.HiFi__hw_acp__source
```

### Update csr-active-config
This kext relies on `com.apple.iokit.IOAudioFamily`, which somehow is loaded very late in the boot process, so we cannot add this kext directly to OpenCore.
At the moment the only way to install this kext is to place it under `/Library/Extensions/`, which requires kexts to be signed. The workaround is to set `csr-active-config` in OpenCore NVRAM settings to `01000000` (aka `CSR_ALLOW_UNTRUSTED_KEXTS`).


## Installation (follow in the right order)
1. Set `csr-active-config` to `01000000`.
2. Download the kext from Github Actions.
3. Copy it to `/Library/Extensions/`.

```console
sudo cp -r AMDMicrophone.kext /Library/Extensions/
```

4. When notified, allow it in `Security & Privacy` settings and reboot.

<p align="center">
  <img src="https://github.com/qhuyduong/AMDMicrophone/assets/16358771/58f053d7-7471-4cda-b9c1-fadd2de328aa" width="50%">
</p>

## FAQ
#### I heard that AMD Microphone can work with VoodooHDA. How is this kext different?
Some modern AMD laptops no longer use High Definition Audio (HDA). It has a dedicated Audio Co-Processor for controlling microphones.

#### Can I use this kext along with AppleALC?
Yes. This kext is a separate driver for microphones. You still need AppleALC for audio output.

#### After installing the kext, Microphone appears in the settings but no sound
Refer [this](#check-if-your-laptop-is-equipped-with-an-acp-based-microphone) to make sure your device is supported.
If your laptop uses High Definition Audio, microphone should work since [AppleALC v1.8.8](https://github.com/acidanthera/AppleALC/releases/tag/1.8.8).

## macOS Sequoia (15.x) 适配

针对 macOS Sequoia（实测 15.8.2 / Darwin 24.6.0 / `com.apple.iokit.IOAudioFamily 600.2` / x86_64）修正「麦克风卡顿」与「声音小」。

### 修了什么

| 现象 | 根因 | 处理 |
| --- | --- | --- |
| 周期性卡顿、断续 | 中断里用 `byteCount % BUFFER_SIZE == 0` 判断 ring buffer 回绕。中断触发时硬件计数器早已越过 watermark，这个精确相等几乎永不成立，`takeTimeStamp()` 长期不被调用，CoreAudio 时间线停滞并反复 underrun | 改为比较 `byteCount / BUFFER_SIZE` 的回绕次数，跨越即打时间戳；中断被合并时补齐 `loopCount` |
| 固定相位偏移 | `dmaStartByteCount` 在 DMA 已启动之后才采样，与 Linux `acp3x-pdm-dma` 在 TRIGGER_START 之前采样的做法不一致，导致 `getCurrentSampleFrame()` 与硬件实际写入位置长期错位 | 移到 `ACP_WOV_PDM_DMA_ENABLE` 置位之前采样 |
| 偶发位置跳变 | 64-bit linearposition counter 由两个 32-bit MMIO 寄存器组成，高低字读取跨越进位会读到错值 | 高-低-高重读校验 |
| 整段丢数据 | `configDMA()` 固定按 4096 推进 offset，物理连续段大于 4K 时后续页没写进 ACP ATU 页表 | 逐页遍历每个 physical segment，并强制 4K 对齐分配 DMA 缓冲 |
| 声音小 | 硬件 PDM gain 设成 2（Linux 默认 3）；0.5 秒淡入每次启动重来；常开且释放极慢的噪声门把语音压低约 15 dB；软件增益固定 3.0x 不可调 | 硬件 gain → 3；淡入 → 10 ms；噪声门默认关闭；软件增益默认 4.5x 且可外部调整 |
| 声音忽大忽小 | 噪声门包络跟随瞬时波形（约 2 ms 时间常数），逐周期调制增益 | 默认关闭；开启时改为约 20 ms 包络、下限 0.65 |

`OSBundleLibraries` 中的 `com.apple.iokit.IOAudioFamily 300.2` 在 Sequoia 的 600.2 上依然有效（系统 kext 的 `OSBundleCompatibleVersion` 为 `1.0`），因此**无需改动依赖版本**，调高反而会使旧系统无法加载。

### 构建

**只需 Command Line Tools，无需完整 Xcode.app。** `xcodebuild` 只是 `.xcodeproj` 的解释器；真正的编译、链接、打包分别由 `clang++`、`ld -kext`、`plutil` 和 `codesign` 完成，CLT 全部提供。内核头文件来自仓库内的 `MacKernelSDK`，本来就不绑定 Xcode 版本。

推荐直接跑仓库根目录的 `build.sh`（把 Xcode kext target 的编译参数、`ld -kext -lkmod -lkmodc++`、**自动生成的 `kmod_info` 桩**、`GENERATE_INFOPLIST_FILE` 的 plist 展开、ad-hoc 签名都显式复刻了一遍）：

```bash
git submodule update --init MacKernelSDK
./build.sh
```

若已装完整 Xcode，也可以继续用原工程：

```bash
xcodebuild -project AMDMicrophone.xcodeproj -scheme AMDMicrophone -configuration Release SYMROOT=$PWD/build
```

两种方式产物都是 `build/Release/AMDMicrophone.kext`（ad-hoc 签名）。静态检查 KPI 依赖：

```bash
kmutil libraries -p build/Release/AMDMicrophone.kext -z
```

注意 Sequoia 上 `kextutil -n` 已被移除（`-n is not a supported kmutil mode`）。ad-hoc 签名的 kext 需要放宽 SIP 才能加载，正式分发需 Developer ID 签名。

**关于 `_kmod_info`**：kext 编入 kernel collection 前必须带这个符号，否则 `kmutil load` 报 `KMErrorDomain Code=31 ... kexts must have a _kmod_info symbol`。Xcode 的 Kernel Extension target 会自动生成 kmod_info 桩，纯手工 `ld -kext` 不会——`-lkmod` 只提供 `_start` / `_stop` 并**引用** `_kmod_info`，不定义它。`AMDMicrophone/AMDMicrophoneKmodInfo.c` 用 `KMOD_EXPLICIT_DECL` 补上定义，由 `build.sh` 单独编译并链入；该文件只服务于 CLT 构建，**没有加入 `.xcodeproj`**（Xcode 会自行生成，重复定义会冲突）。可用 `nm -g build/Release/AMDMicrophone.kext/Contents/MacOS/AMDMicrophone | grep _kmod_info` 确认输出为 `D _kmod_info`。

`build.sh` **原地覆盖同名 bundle**，不会生成带版本号的新文件；它每次会先清空再重建该目录，因此 Finder 里 bundle 的修改时间会跟着变，末尾也会打印产物大小、时间和 sha256。可用 `shasum -a 256 build/Release/AMDMicrophone.kext/Contents/MacOS/AMDMicrophone` 与 `/Library/Extensions/AMDMicrophone.kext/Contents/MacOS/AMDMicrophone` 对比，确认系统里装的是否已经是刚构建的版本。

### 在 Sequoia 上安装

前提：`csrutil status` 显示 **Kext Signing: disabled**（例如 `csr-active-config = 01000000`，或任何包含 `CSR_ALLOW_UNTRUSTED_KEXTS` 的自定义配置）。本项目产物是 ad-hoc 签名，SIP 完整开启时会被静默拒绝。

```bash
sudo cp -R build/Release/AMDMicrophone.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/AMDMicrophone.kext
sudo chmod -R 755 /Library/Extensions/AMDMicrophone.kext
sudo kmutil rebuild        # 只重建 Auxiliary KC，见下方警告
sudo reboot
```

若 `/Library/Extensions` 里装的是早期构建产物（缺 `_kmod_info`），必须先重新 `./build.sh` 再执行上面的 `cp -R` 替换，否则 `kmutil load` 会直接报 Code=31。

**不要用 `kmutil create --update-all` 或 `kmutil install --update-all`。** 在黑苹果 / 未装 KDK 的机器上，全量重建会去解析整个 System KC 的依赖链，因缺少 KDK 提供的 `com.apple.kpi.private` 和 `kernel.kasan` / `kernel.development` / `kernel.debug` / `kernel.research` 变体内核而报 `KMErrorDomain Code=71`（典型报错是 `IOHIDSystem` 或 `AppleUSBHostPlatformProperties` 的依赖找不到，与本项目无关）。`--allow-missing-kdk` 只是跳过「KDK 是否存在」的检查，并不会补上这些文件，因此必然失败。`kmutil rebuild` 只动 `/Library/Extensions` 对应的 `AuxiliaryKernelExtensions.kc`，绕开整条系统依赖链。若确实需要全量重建，必须先装匹配系统 build（如 `24H232`）的 Kernel Debug Kit，但重建 System/Boot KC 在黑苹果上有无法开机的风险。

只想验证 kext 能否加载、不重建任何缓存时：

```bash
sudo kmutil load -p /Library/Extensions/AMDMicrophone.kext -v --no-authorization
kmutil print-diagnostics -p /Library/Extensions/AMDMicrophone.kext
```

**`cp` 之后不会有任何系统提示。** macOS 不监视 `/Library/Extensions`；签名校验、用户审批、把 kext 编入 `AuxiliaryKernelExtensions.kc` 都发生在 `kmutil rebuild`（或 `kmutil load`）和重启时。可比对时间确认缓存确实重建过：

```bash
ls -l /Library/KernelCollections/AuxiliaryKernelExtensions.kc
```

若该文件的修改时间早于拷贝时间，说明缓存没重建，kext 一定没加载。

「系统设置 > 隐私与安全性 > 允许」的弹窗**只在 SIP 完整开启、kext 有有效 Developer ID 签名但尚未获用户批准时**才出现。ad-hoc 签名 + 已禁用 Kext Signing 会跳过这一流程，因此没有弹窗、系统设置里也没有允许按钮，属于正常现象（上文 Installation 第 4 步的提示不适用于此配置）。反过来，一旦 SIP 恢复完整开启，ad-hoc 签名会被静默拒绝，只在 `syspolicyd` 日志里留记录。

**ad-hoc 签名还有第二道关卡**：`codesign --verify --strict` 会报 `valid on disk / satisfies its Designated Requirement`，但 `kmutil print-diagnostics` 仍可能报 `Authenticating extension failed: Bad code signature`——kernel manager 在编入/加载时要求带开发者身份的 CMS 签名，ad-hoc（无 `TeamIdentifier`）不一定被接受，即使 SIP 的 Kext Signing 已禁用。诊断里 `Dependencies: OK` 说明依赖链没问题，此时需要用真实代码签名证书重签（对照 `/Library/Extensions` 里其它第三方 kext 通常都带 `TeamIdentifier`）。

排查时注意 `kmutil` 的错误写向 stderr，`... | grep amd` 过滤不到它，要写成 `... 2>&1 | grep -i amd`。

重启后验证：

```bash
kmutil showloaded --list-only | grep -i amdmic
kextstat | grep -i amdmic
log show --last 5m --info --predicate 'eventMessage CONTAINS "AMD Digital Microphone"'
```

### 免重编译调参

把下列属性注入 ACP PCI 设备（`0x15e21022`）的 DeviceProperties，类型均为 Number。路径用 `gfxutil -f` 或 IORegistryExplorer 查；本机（Lenovo / ACP3x）实测为 `PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x5)`，对应 `pcidebug = 5:0:5`、`acpi-path = .../_SB/PCI0@0/GP17@80001/ACP@5`。

| 键 | 默认 | 范围 | 用途 |
| --- | --- | --- | --- |
| `amdmic-hw-gain` | 3 | 0–3 | ACP PDM 硬件增益档位。底噪大优先降到 2 或 1 |
| `amdmic-sw-gain` | 45 | 10–200 | 软件增益 ×0.1（45 = 4.5×）。底噪大试 25–30；仍偏小试 60–80；破音则调低 |
| `amdmic-mix` | 0 | 0–4 | 0 双麦平均 / 1 仅左 / 2 仅右 / 3 相加 / 4 真立体声。某一路麦噪声大或两路反相抵消时试 1 或 2 |
| `amdmic-dc-block` | 1 | 0 或 1 | 一阶高通（约 38 Hz）去 PDM 直流偏置与低频嗡声，也让噪声门包络不被 DC 一直顶开 |
| `amdmic-noise-gate` | 0 | 0 或 1 | 噪声门开关，默认关 |
| `amdmic-gate-floor` | 20 | 0–100 | 判定为静音时保留的百分比（20 ≈ −14 dB）。越小压得越狠，太小会切掉字尾 |
| `amdmic-gate-threshold` | 6 | 1–500 | 门限，单位千分之一满幅（6 = 0.006）。需高于底噪、低于语音；调高压得多，调高低保真 |
| `amdmic-debug` | 0 | 0 或 1 | 内核日志打印 ATU 页表数、当前全部增益参数、被合并的回绕数与中断统计 |

调参前先确认「系统设置 > 声音 > 输入」的音量滑块不在低位。设 `amdmic-debug = 1` 后可用 `log show --last 5m --predicate 'eventMessage CONTAINS "AMD Digital Microphone"'` 确认注入值确实被读到。

### 底噪偏大怎么调

修完卡顿后的默认链路是 **硬件 gain 3 + 软件 4.5× + 双麦平均 + 噪声门关**，这套组合是为治「声音小」配的，会连带把 PDM 本底噪声一起抬高。按顺序试：

1. **先降总增益**，最立竿见影：`amdmic-hw-gain = 2`、`amdmic-sw-gain = 30`。总增益从 13.5× 降到约 6×，底噪绝对电平降约 7 dB；因为淡入和压语音的噪声门都已去掉，听感通常仍比最初那版响。
2. **换通道**：若某一路麦本身噪声大或两路反相（表现为声音空、底噪突出），试 `amdmic-mix = 1`，不行再试 `2`。这是少数能真正改善信噪比的手段。
3. **再开噪声门**：`amdmic-noise-gate = 1`。它只压「不说话」的段落，对说话时的持续底噪帮助有限。若字尾被吃掉 → 降 `amdmic-gate-threshold`（2–4）或升 `amdmic-gate-floor`（40–70）；若底噪依旧 → 升 threshold（10–30）或降 floor（5–10）。
4. `amdmic-dc-block` 保持 1；只有在出现低频缺失或人声发薄时才设 0 对比。

一个可作为起点的 OpenCore `config.plist`（`DeviceProperties/Add`）：

```xml
<key>PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x5)</key>
<dict>
    <key>amdmic-hw-gain</key>         <integer>2</integer>
    <key>amdmic-sw-gain</key>         <integer>30</integer>
    <key>amdmic-mix</key>             <integer>0</integer>
    <key>amdmic-dc-block</key>        <integer>1</integer>
    <key>amdmic-noise-gate</key>      <integer>1</integer>
    <key>amdmic-gate-floor</key>      <integer>20</integer>
    <key>amdmic-gate-threshold</key>  <integer>6</integer>
</dict>
```

若配置工具要求 Data 而非 Integer，填 8 字节小端十六进制（例如 `2` → `0200000000000000`）。每次改完 DeviceProperties 需重启生效。

注意：降增益只改变绝对电平，**不改变信噪比**；真正改善 SNR 的是选对麦克风通道和去掉 DC / 低频，噪声门则只负责静音段。

### 已知限制

- 仍只支持 Renoir（revision `0x1`）的 ACP3x，ACP6x 及其它变体未适配。
- 基于已废弃的 `IOAudioFamily`，仅 x86_64 可用；Apple Silicon 需要迁移到 AudioDriverKit / AudioServerPlugIn，属于后续工作。
- 装机前请备份当前可正常使用的旧版 kext，以便快速回滚。
