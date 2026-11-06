//
//  AMDMicrophoneDevice.cpp
//  AMDMicrophone
//
//  Created by Huy Duong on 25/06/2023.
//

#include "AMDMicrophoneDevice.hpp"

#include "AMDMicrophoneCommon.hpp"
#include "AMDMicrophoneEngine.hpp"

#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>

#define super IOAudioDevice

OSDefineMetaClassAndStructors(AMDMicrophoneDevice, IOAudioDevice);

UInt32 AMDMicrophoneDevice::readl(UInt32 reg)
{
    return *(const volatile UInt32*)(baseAddr + reg - ACP_PHY_BASE_ADDRESS);
}

void AMDMicrophoneDevice::writel(UInt32 val, UInt32 reg)
{
    *(volatile UInt32*)(baseAddr + reg - ACP_PHY_BASE_ADDRESS) = val;
}

bool AMDMicrophoneDevice::configDMA()
{
    const IOByteCount totalLength = dmaDescriptor->getLength();
    const UInt32 expectedPages = (UInt32)((totalLength + ACP_DMA_PAGE_SIZE - 1) / ACP_DMA_PAGE_SIZE);
    const UInt32 maxPTEOffset = expectedPages * 8;
    UInt32 pte = 0;
    IOByteCount offset = 0;

    writel(ACP_SRAM_PTE_OFFSET | BIT(31), ACP_AXI2AXI_ATU_BASE_ADDR_GRP_1);
    writel(ACP_PAGE_SIZE_4K_ENABLE, ACP_AXI2AXI_ATU_PAGE_SIZE_GRP_1);

    while (offset < totalLength && pte < maxPTEOffset) {
        IOByteCount segmentLength = 0;
        addr64_t address = dmaDescriptor->getPhysicalSegment(offset, &segmentLength);

        if (!address || !segmentLength)
            break;

        /*
         * A physically contiguous segment can span several 4K pages. The old
         * code always advanced "offset" by exactly one page, so any segment
         * larger than 4096 bytes left the trailing pages unmapped in the ACP
         * ATU - the DMA then wrote into unmapped memory and whole periods went
         * missing (audible drop-outs). Walk every page of every segment.
         */
        IOByteCount done = 0;
        while (done < segmentLength && pte < maxPTEOffset) {
            addr64_t pageAddress = address + done;
            UInt32 low = (UInt32)pageAddress;
            UInt32 high = (UInt32)(pageAddress >> 32) | (UInt32)BIT(31);

            writel(low, ACP_SCRATCH_REG_0 + pte);
            writel(high, ACP_SCRATCH_REG_0 + pte + 4);

            pte += 8;
            done += ACP_DMA_PAGE_SIZE;
        }

        offset += segmentLength;
    }

    UInt32 mappedPages = pte / 8;
    if (mappedPages < expectedPages) {
        LOG("ACP DMA page table incomplete: %u/%u pages mapped\n", mappedPages, expectedPages);
        return false;
    }

    if (config.debug)
        LOG("ACP DMA page table: %u pages mapped, hw gain %u, sw gain %u.%u, "
            "mix %u, dc-block %u, gate %u (floor %u%%, threshold 0.%03u)\n",
            mappedPages, config.hardwareGain, config.softwareGainX10 / 10,
            config.softwareGainX10 % 10, config.mixMode, (UInt32)config.dcBlock,
            (UInt32)config.noiseGate, config.gateFloorPercent, config.gateThresholdX1000);

    return true;
}

void AMDMicrophoneDevice::clearDMABuffer()
{
    UInt8* buffer = (UInt8*)dmaDescriptor->getBytesNoCopy();

    if (!buffer)
        return;

    for (UInt32 index = 0; index < BUFFER_SIZE; index++)
        buffer[index] = 0;
}

void AMDMicrophoneDevice::disableInterrupt()
{
    UInt32 val;

    writel(ACP_EXT_INTR_STAT_CLEAR_MASK, ACP_EXTERNAL_INTR_STAT);
    val = readl(ACP_EXTERNAL_INTR_CNTL);
    val &= ~(UInt32)ACP_PDM_DMA_INTR_MASK;
    writel(val, ACP_EXTERNAL_INTR_CNTL);
    writel(0x0, ACP_EXTERNAL_INTR_ENB);
}

void AMDMicrophoneDevice::enableClock()
{
    UInt32 val;

    writel(ACP_PDM_CLK_FREQ_MASK, ACP_WOV_CLK_CTRL);
    val = readl(ACP_WOV_MISC_CTRL);
    val &= ~(UInt32)ACP_WOV_GAIN_CONTROL;
    val |= (config.hardwareGain << ACP_WOV_GAIN_CONTROL_SHIFT) & ACP_WOV_GAIN_CONTROL;
    writel(val, ACP_WOV_MISC_CTRL);
}

void AMDMicrophoneDevice::enableInterrupt()
{
    UInt32 val;

    val = readl(ACP_EXTERNAL_INTR_CNTL);
    val |= (UInt32)ACP_PDM_DMA_INTR_MASK;
    writel(val, ACP_EXTERNAL_INTR_CNTL);
    writel(0x1, ACP_EXTERNAL_INTR_ENB);
}

UInt64 AMDMicrophoneDevice::getByteCount()
{
    UInt32 high, highAgain, low;

    /*
     * The 64-bit linearposition counter is exposed as two 32-bit MMIO
     * registers. Reading high/low without checking for a low-word wrap can
     * produce a bogus byte count once, which shows up as a sudden clock jump.
     */
    high = readl(ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH);
    low = readl(ACP_WOV_RX_LINEARPOSITIONCNTR_LOW);
    highAgain = readl(ACP_WOV_RX_LINEARPOSITIONCNTR_HIGH);
    if (high != highAgain)
        low = readl(ACP_WOV_RX_LINEARPOSITIONCNTR_LOW);

    return ((UInt64)highAgain << 32) | low;
}

UInt64 AMDMicrophoneDevice::getRelativeByteCount()
{
    UInt64 byteCount = getByteCount();

    if (byteCount < dmaStartByteCount)
        return 0;

    return byteCount - dmaStartByteCount;
}

void AMDMicrophoneDevice::initRingBuffer(UInt32 physAddr, UInt32 bufferSize, UInt32 watermarkSize)
{
    writel(physAddr, ACP_WOV_RX_RINGBUFADDR);
    writel(bufferSize, ACP_WOV_RX_RINGBUFSIZE);
    writel(watermarkSize, ACP_WOV_RX_INTR_WATERMARK_SIZE);
    writel(0x1, ACP_AXI2AXI_ATU_CTRL);
}

IOReturn AMDMicrophoneDevice::powerOff()
{
    UInt32 val;
    UInt32 timeout;

    writel(ACP_PGFSM_CNTL_POWER_OFF_MASK, ACP_PGFSM_CONTROL);

    timeout = 0;
    while (++timeout < 500) {
        val = readl(ACP_PGFSM_STATUS);
        if ((val & ACP_PGFSM_STATUS_MASK) == ACP_POWERED_OFF)
            return kIOReturnSuccess;
        IODelay(1);
    }

    return kIOReturnTimeout;
}

IOReturn AMDMicrophoneDevice::powerOn()
{
    UInt32 val;
    UInt32 timeout;

    val = readl(ACP_PGFSM_STATUS);
    if (val == 0)
        return kIOReturnSuccess;

    if ((val & ACP_PGFSM_STATUS_MASK) != ACP_POWER_ON_IN_PROGRESS)
        writel(ACP_PGFSM_CNTL_POWER_ON_MASK, ACP_PGFSM_CONTROL);

    timeout = 0;
    while (++timeout < 500) {
        val = readl(ACP_PGFSM_STATUS);
        if (!val)
            return kIOReturnSuccess;
        IODelay(1);
    }

    return kIOReturnTimeout;
}

IOReturn AMDMicrophoneDevice::reset()
{
    UInt32 val;
    UInt32 timeout;

    writel(0x1, ACP_SOFT_RESET);
    timeout = 0;
    while (++timeout < 500) {
        val = readl(ACP_SOFT_RESET);
        if (val & ACP_SOFT_RESET_SOFTRESET_AUDDONE_MASK)
            break;
        cpu_relax();
    }
    writel(0x0, ACP_SOFT_RESET);
    timeout = 0;
    while (++timeout < 500) {
        val = readl(ACP_SOFT_RESET);
        if (!val)
            return kIOReturnSuccess;
        cpu_relax();
    }
    return kIOReturnTimeout;
}

IOReturn AMDMicrophoneDevice::startDMA()
{
    UInt32 val;
    UInt32 timeout;

    /*
     * The ACP linearposition counter is free running. Linux's acp3x-pdm-dma
     * snapshots it in TRIGGER_START, *before* ACP_WOV_PDM_DMA_ENABLE is
     * asserted, and treats that value as byte 0 of the ring buffer. Reading it
     * after the DMA has already started (the old behaviour) leaves a permanent
     * phase error between getCurrentSampleFrame() and the location the hardware
     * is actually writing, which is one source of the drop-outs.
     */
    dmaStartByteCount = getByteCount();
    lastLoopIndex = 0;
    missedLoopCount = 0;
    interruptCount = 0;
    dmaRunning = false;

    writel(0x1, ACP_WOV_PDM_FIFO_FLUSH);
    enableClock();
    writel(0x1, ACP_WOV_PDM_ENABLE);
    writel(0x1, ACP_WOV_PDM_DMA_ENABLE);

    timeout = 0;
    while (++timeout < ACP_COUNTER) {
        val = readl(ACP_WOV_PDM_DMA_ENABLE);
        if ((val & 0x2) == ACP_PDM_DMA_EN_STATUS)
            break;
        IODelay(5);
    }

    if (timeout == ACP_COUNTER)
        return kIOReturnTimeout;

    dmaRunning = true;
    return kIOReturnSuccess;
}

IOReturn AMDMicrophoneDevice::stopDMA()
{
    UInt32 val;
    UInt32 timeout;

    dmaRunning = false;

    val = readl(ACP_WOV_PDM_DMA_ENABLE);
    if (val & 0x1) {
        writel(0x0, ACP_WOV_PDM_DMA_ENABLE);
        timeout = 0;
        while (++timeout < ACP_COUNTER) {
            val = readl(ACP_WOV_PDM_DMA_ENABLE);
            if (val == 0x0)
                break;
            IODelay(5);
        }
        if (timeout == ACP_COUNTER)
            return kIOReturnTimeout;
    }

    val = readl(ACP_WOV_PDM_ENABLE);
    if (val == 0x1)
        writel(0x0, ACP_WOV_PDM_ENABLE);

    writel(0x1, ACP_WOV_PDM_FIFO_FLUSH);

    if (config.debug)
        LOG("DMA stopped: %llu IRQs, %llu merged wraps\n", interruptCount, missedLoopCount);

    return kIOReturnSuccess;
}

bool AMDMicrophoneDevice::createAudioEngine()
{
    bool result = false;

    audioEngine = new AMDMicrophoneEngine;
    if (!audioEngine)
        goto Done;

    if (!audioEngine->init(this))
        goto Done;

    if (activateAudioEngine(audioEngine) != kIOReturnSuccess)
        goto Done;

    result = true;

Done:
    if (audioEngine != NULL) {
        audioEngine->release();
    }

    return result;
}

int AMDMicrophoneDevice::findMSIInterruptTypeIndex()
{
    IOReturn ret;
    int index, source = 0;
    for (index = 0;; index++) {
        int interruptType;
        ret = pciDevice->getInterruptType(index, &interruptType);
        if (ret != kIOReturnSuccess)
            break;
        if (interruptType & kIOInterruptTypePCIMessaged) {
            source = index;
            break;
        }
    }
    return source;
}

void AMDMicrophoneDevice::interruptHandler()
{
    UInt32 val;
    UInt64 byteCount;
    UInt64 loopIndex;

    val = readl(ACP_EXTERNAL_INTR_STAT);
    if (!(val & BIT(ACP_PDM_DMA_STAT)))
        return;

    /* Acknowledge (W1C) before doing any work so a following period is not lost. */
    writel(BIT(ACP_PDM_DMA_STAT), ACP_EXTERNAL_INTR_STAT);

    if (!dmaRunning || !audioEngine)
        return;

    byteCount = getRelativeByteCount();
    loopIndex = byteCount / BUFFER_SIZE;
    interruptCount++;

    if (loopIndex < lastLoopIndex) {
        /* Counter went backwards (DMA restart/resume) - resynchronise quietly. */
        lastLoopIndex = loopIndex;
        return;
    }

    if (loopIndex == lastLoopIndex)
        return;

    /*
     * IOAudioEngine expects exactly one takeTimeStamp() per ring buffer wrap:
     * it increments fCurrentLoopCount and records fLastLoopTime, which is the
     * basis of the whole CoreAudio timeline.
     *
     * The original code tested "byteCount % BUFFER_SIZE == 0". The byte counter
     * has already advanced past the watermark by the time the ISR reads it, so
     * that exact equality almost never became true - the timestamp froze and
     * CoreAudio repeatedly underran the input stream (the "stuttering").
     * Compare wrap *counts* instead, and catch up if an IRQ was coalesced.
     */
    UInt64 skipped = loopIndex - lastLoopIndex;
    if (skipped > 1) {
        missedLoopCount += skipped - 1;
        if (config.debug)
            LOG("coalesced IRQ: %llu wraps\n", skipped);
    }

    UInt64 stamps = skipped;
    if (stamps > ACP_MAX_LOOP_CATCHUP)
        stamps = ACP_MAX_LOOP_CATCHUP;

    for (UInt64 index = 0; index < stamps; index++)
        audioEngine->takeTimeStamp();

    lastLoopIndex = loopIndex;
}

void AMDMicrophoneDevice::interruptOccurred(OSObject* owner, IOInterruptEventSource* src, int intCount)
{
    AMDMicrophoneDevice* that = (AMDMicrophoneDevice*)owner;

    if (that)
        that->interruptHandler();
}

IOService* AMDMicrophoneDevice::probe(IOService* provider, SInt32* score)
{
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice)
        return NULL;

    UInt8 revisionId = pciDevice->configRead8(kIOPCIConfigRevisionID);

    if (revisionId == 0x1)
        LOG("AMD Digital Microphone for Renoir\n");
    else {
        LOG("Only Renoir is supported at the moment\n");
        return NULL;
    }

    return this;
}

bool AMDMicrophoneDevice::initHardware(IOService* provider)
{
    bool result = false;
    IOWorkLoop* workLoop;

    if (!super::initHardware(provider)) {
        goto Done;
    }

    /* DeviceProperties / IORegistry tuning knobs (see AMDMicrophoneCommon.hpp). */
    config = AMDMicReadConfig(provider);

    setDeviceName("AMD Digital Microphone");
    setDeviceShortName("AMDMicrophone");
    setManufacturerName("AMD");
    setDeviceTransportType(kIOAudioDeviceTransportTypePCI);

    pciDevice->setBusMasterEnable(true);
    pciDevice->setIOEnable(true);
    pciDevice->setMemoryEnable(true);

    baseAddrMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!baseAddrMap)
        goto Done;

    baseAddr = baseAddrMap->getVirtualAddress();
    if (!baseAddr)
        goto Done;

    /*
     * Force page alignment: the ACP ATU is a 4K page table, so every PTE must
     * point at a page-aligned physical address. This also makes the segment
     * walk in configDMA() unambiguous.
     */
    dmaDescriptor = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionIn, BUFFER_SIZE, ACP_DMA_PAGE_SIZE
    );
    if (!dmaDescriptor)
        goto Done;
    if (dmaDescriptor->prepare(kIODirectionIn) != kIOReturnSuccess)
        goto Done;
    dmaPrepared = true;
    clearDMABuffer();

    workLoop = getWorkLoop();
    if (!workLoop)
        goto Done;

    irqEventSource = IOInterruptEventSource::interruptEventSource(
        this,
        (IOInterruptEventAction)&AMDMicrophoneDevice::interruptOccurred,
        provider,
        findMSIInterruptTypeIndex()
    );
    if (!irqEventSource)
        goto Done;
    if (workLoop->addEventSource(irqEventSource) != kIOReturnSuccess)
        goto Done;

    irqEventSource->enable();

    if (!createAudioEngine())
        goto Done;

    if (powerOn() != kIOReturnSuccess)
        goto Done;

    writel(0x1, ACP_CONTROL);
    if (reset() != kIOReturnSuccess)
        goto Done;

    writel(0x3, ACP_CLKMUX_SEL);

    if (!configDMA())
        goto Done;

    result = true;

Done:
    if (!result)
        free();

    return result;
}

void AMDMicrophoneDevice::stop(IOService* provider)
{
    dmaRunning = false;

    if (irqEventSource)
        irqEventSource->disable();

    if (baseAddr) {
        reset();
        writel(0x0, ACP_CLKMUX_SEL);
        writel(0x0, ACP_CONTROL);
    }

    super::stop(provider);
}

void AMDMicrophoneDevice::free()
{
    dmaRunning = false;

    if (baseAddrMap) {
        baseAddrMap->release();
        baseAddrMap = NULL;
    }
    baseAddr = 0;

    if (dmaDescriptor) {
        if (dmaPrepared) {
            dmaDescriptor->complete(kIODirectionIn);
            dmaPrepared = false;
        }
        dmaDescriptor->release();
        dmaDescriptor = NULL;
    }

    super::free();
}
