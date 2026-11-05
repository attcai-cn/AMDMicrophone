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

需要完整 Xcode 16 + macOS 15 SDK（仅装 CommandLineTools 不能构建 kext）：

```bash
git submodule update --init --recursive
xcodebuild -project AMDMicrophone.xcodeproj -scheme AMDMicrophone -configuration Release SYMROOT=$PWD/build
```

产物为 `build/Release/AMDMicrophone.kext`。

### 在 Sequoia 上安装

沿用上文的 `csr-active-config = 01000000` 与 `/Library/Extensions` 流程，缓存重建改用 `kmutil`：

```bash
sudo cp -r build/Release/AMDMicrophone.kext /Library/Extensions/
sudo kmutil install --update-all
# 在「系统设置 > 隐私与安全性」中允许，然后重启
```

验证：

```bash
kextstat | grep -i amdmic
log show --last 5m --info --predicate 'eventMessage CONTAINS "AMD Digital Microphone"'
```

### 免重编译调参

把下列属性注入 ACP PCI 设备（`0x15e21022`）的 DeviceProperties，先用 `gfxutil` 或 IORegistryExplorer 查到对应的 PciRoot 路径，类型均为 Number：

| 键 | 默认 | 范围 | 用途 |
| --- | --- | --- | --- |
| `amdmic-hw-gain` | 3 | 0–3 | ACP PDM 硬件增益档位 |
| `amdmic-sw-gain` | 45 | 10–200 | 软件增益 ×0.1（45 = 4.5×）。仍偏小可试 60–80；破音则调低 |
| `amdmic-mix` | 0 | 0–4 | 0 双麦平均 / 1 仅左 / 2 仅右 / 3 相加 / 4 真立体声。若两路反相导致抵消，试 1 或 2 |
| `amdmic-noise-gate` | 0 | 0 或 1 | 噪声门开关，默认关 |
| `amdmic-debug` | 0 | 0 或 1 | 内核日志打印 ATU 页表数、被合并的回绕数、停止时的中断统计 |

调参前先确认「系统设置 > 声音 > 输入」的音量滑块不在低位。

### 已知限制

- 仍只支持 Renoir（revision `0x1`）的 ACP3x，ACP6x 及其它变体未适配。
- 基于已废弃的 `IOAudioFamily`，仅 x86_64 可用；Apple Silicon 需要迁移到 AudioDriverKit / AudioServerPlugIn，属于后续工作。
- 装机前请备份当前可正常使用的旧版 kext，以便快速回滚。
