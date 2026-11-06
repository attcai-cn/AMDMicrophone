//
//  AMDMicrophoneCommon.hpp
//  AMDMicrophone
//
//  Created by Huy Duong on 01/07/2023.
//

#ifndef AMDMicrophoneCommon_h
#define AMDMicrophoneCommon_h

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSNumber.h>

#define LOG(fmt, ...) IOLog("%s: " fmt, this->getName(), ##__VA_ARGS__)

/*
 * Optional tuning knobs. These can be injected into the ACP PCI device with
 * OpenCore DeviceProperties (or set in the IORegistry before the driver
 * matches), so gain / channel routing can be adjusted without rebuilding
 * the kext. See README.md for the recommended values.
 */
#define kAMDMicHWGainKey          "amdmic-hw-gain"
#define kAMDMicSWGainKey          "amdmic-sw-gain"
#define kAMDMicMixKey             "amdmic-mix"
#define kAMDMicNoiseGateKey       "amdmic-noise-gate"
#define kAMDMicDCBlockKey         "amdmic-dc-block"
#define kAMDMicGateFloorKey       "amdmic-gate-floor"
#define kAMDMicGateThresholdKey   "amdmic-gate-threshold"
#define kAMDMicDebugKey           "amdmic-debug"

#define kAMDMicMixAverage 0
#define kAMDMicMixLeft    1
#define kAMDMicMixRight   2
#define kAMDMicMixSum     3
#define kAMDMicMixStereo  4

/* Linux's acp3x-pdm-dma defaults pdm_gain to 3 (the top hardware gain step). */
#define kAMDMicDefaultHWGain    3
/* Software gain is expressed in tenths: 45 -> 4.5x. */
#define kAMDMicDefaultSWGainX10 45
#define kAMDMicDefaultMix       kAMDMicMixAverage
#define kAMDMicDefaultNoiseGate 0

/*
 * Noise-floor tuning. A PDM stream carries a small DC offset and some
 * low-frequency rumble; amplifying it 4.5x makes the hiss very obvious and,
 * worse, keeps the gate's envelope detector permanently open. A ~38 Hz
 * high-pass removes that before any gain is applied.
 */
#define kAMDMicDefaultDCBlock              1
/* Percent of signal kept while the gate considers the input silent. */
#define kAMDMicDefaultGateFloorPercent     20
/* Threshold in thousandths of full scale: 6 -> 0.006 (matches the old gate). */
#define kAMDMicDefaultGateThresholdX1000  6

struct AMDMicrophoneConfig {
    UInt32 hardwareGain;
    UInt32 softwareGainX10;
    UInt32 mixMode;
    bool   noiseGate;
    bool   dcBlock;
    UInt32 gateFloorPercent;
    UInt32 gateThresholdX1000;
    bool   debug;
};

static inline UInt32 AMDMicGetProperty(
    IOService* provider, const char* key, UInt32 fallback, UInt32 minimum, UInt32 maximum
)
{
    if (!provider || !key)
        return fallback;

    OSNumber* number = OSDynamicCast(OSNumber, provider->getProperty(key));
    if (!number)
        return fallback;

    UInt32 value = number->unsigned32BitValue();
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    return value;
}

static inline AMDMicrophoneConfig AMDMicReadConfig(IOService* provider)
{
    AMDMicrophoneConfig config = {};

    config.hardwareGain = AMDMicGetProperty(
        provider, kAMDMicHWGainKey, kAMDMicDefaultHWGain, 0, 3
    );
    config.softwareGainX10 = AMDMicGetProperty(
        provider, kAMDMicSWGainKey, kAMDMicDefaultSWGainX10, 10, 200
    );
    config.mixMode = AMDMicGetProperty(
        provider, kAMDMicMixKey, kAMDMicDefaultMix, 0, kAMDMicMixStereo
    );
    config.noiseGate = AMDMicGetProperty(
        provider, kAMDMicNoiseGateKey, kAMDMicDefaultNoiseGate, 0, 1
    ) != 0;
    config.dcBlock = AMDMicGetProperty(
        provider, kAMDMicDCBlockKey, kAMDMicDefaultDCBlock, 0, 1
    ) != 0;
    config.gateFloorPercent = AMDMicGetProperty(
        provider, kAMDMicGateFloorKey, kAMDMicDefaultGateFloorPercent, 0, 100
    );
    config.gateThresholdX1000 = AMDMicGetProperty(
        provider, kAMDMicGateThresholdKey, kAMDMicDefaultGateThresholdX1000, 1, 500
    );
    config.debug = AMDMicGetProperty(
        provider, kAMDMicDebugKey, 0, 0, 1
    ) != 0;

    return config;
}

#endif /* AMDMicrophoneCommon_h */
