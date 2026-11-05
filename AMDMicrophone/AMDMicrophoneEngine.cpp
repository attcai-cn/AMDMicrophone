//
//  AMDMicrophoneEngine.cpp
//  AMDMicrophone
//
//  Created by Huy Duong on 30/06/2023.
//

#include "AMDMicrophoneEngine.hpp"

#include "AMDMicrophoneCommon.hpp"
#include "AMDMicrophoneDevice.hpp"

#include <IOKit/audio/IOAudioDefines.h>
#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>

#include <stdint.h>

#define super IOAudioEngine

OSDefineMetaClassAndStructors(AMDMicrophoneEngine, IOAudioEngine);

static inline float AMDMicClampSample(float value)
{
    if (value > 1.0f)
        return 1.0f;
    if (value < -1.0f)
        return -1.0f;
    return value;
}

bool AMDMicrophoneEngine::createControls()
{
    bool result = false;
    IOAudioControl* control;

    control = IOAudioLevelControl::createVolumeControl(
        (SInt32)volume,
        0,
        MAX_VOLUME,
        (-22 << 16) + (32768),
        0,
        kIOAudioControlChannelIDAll,
        kIOAudioControlChannelNameAll,
        0,
        kIOAudioControlUsageInput
    );
    if (!control)
        goto Done;

    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)gainChangeHandler, this);
    addDefaultAudioControl(control);
    control->release();

    result = true;

Done:
    return result;
}

IOAudioStream* AMDMicrophoneEngine::createNewAudioStream(
    IOAudioStreamDirection direction, void* sampleBuffer, UInt32 sampleBufferSize
)
{
    IOAudioStream* audioStream;

    audioStream = new IOAudioStream;
    if (audioStream) {
        if (!audioStream->initWithAudioEngine(this, direction, 1))
            audioStream->release();
        else {
            IOAudioSampleRate rate;
            IOAudioStreamFormat format = {
                NUM_CHANNELS,
                kIOAudioStreamSampleFormatLinearPCM,
                kIOAudioStreamNumericRepresentationSignedInt,
                SAMPLE_DEPTH,
                SAMPLE_WIDTH,
                kIOAudioStreamAlignmentLowByte,
                kIOAudioStreamByteOrderLittleEndian,
                true,
                0
            };

            audioStream->setSampleBuffer(sampleBuffer, sampleBufferSize);
            rate.fraction = 0;
            rate.whole = SAMPLE_RATE;
            audioStream->addAvailableFormat(&format, &rate, &rate);
            audioStream->setFormat(&format);
        }
    }

    return audioStream;
}

IOReturn AMDMicrophoneEngine::gainChangeHandler(
    IOService* target, IOAudioControl* gainControl, SInt32 oldValue, SInt32 newValue
)
{
    AMDMicrophoneEngine* that = (AMDMicrophoneEngine*)target;

    if (!that)
        return kIOReturnBadArgument;

    SInt32 value = newValue;
    if (value < 0)
        value = 0;
    if (value > MAX_VOLUME)
        value = MAX_VOLUME;
    that->volume = (UInt32)value;

    return kIOReturnSuccess;
}

bool AMDMicrophoneEngine::init(AMDMicrophoneDevice* device)
{
    if (!super::init(NULL))
        return false;

    audioDevice = device;

    return true;
}

void AMDMicrophoneEngine::free()
{
    super::free();
}

bool AMDMicrophoneEngine::initHardware(IOService* provider)
{
    bool result = false;
    IOAudioSampleRate initialSampleRate;
    IOAudioStream* audioStream;

    if (!super::initHardware(provider))
        goto Done;

    setDescription("AMD Digital Microphone");

    initialSampleRate.whole = SAMPLE_RATE;
    initialSampleRate.fraction = 0;
    setSampleRate(&initialSampleRate);
    setNumSampleFramesPerBuffer(NUM_FRAMES);
    /* Keep CoreAudio a full period behind the DMA write head on input. */
    setInputSampleOffset(2 * PERIOD_FRAMES);

    if (!createControls())
        goto Done;

    audioStream = createNewAudioStream(
        kIOAudioStreamDirectionInput, audioDevice->dmaDescriptor->getBytesNoCopy(), BUFFER_SIZE
    );
    if (!audioStream)
        goto Done;
    addAudioStream(audioStream);
    audioStream->release();

    result = true;

Done:
    return result;
}

void AMDMicrophoneEngine::stop(IOService* provider)
{
    super::stop(provider);
}

IOReturn AMDMicrophoneEngine::convertInputSamples(
    const void* sampleBuf, void* destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames,
    const IOAudioStreamFormat* streamFormat, IOAudioStream* audioStream
)
{
    if (!sampleBuf || !destBuf || !streamFormat || !audioDevice)
        return kIOReturnBadArgument;

    const UInt32 channels = streamFormat->fNumChannels;
    if (channels == 0)
        return kIOReturnBadArgument;

    const AMDMicrophoneConfig& config = audioDevice->config;
    const float softwareGain = (float)config.softwareGainX10 * 0.1f;
    const float controlGain = (float)volume / (float)MAX_VOLUME;

    /* The ACP always writes two S32_LE words per frame, independent of the
     * channel count CoreAudio negotiated, so stride on the hardware layout. */
    const SInt32* inputBuf32 = (const SInt32*)sampleBuf + (firstSampleFrame * NUM_CHANNELS);
    float* floatDestBuf = (float*)destBuf;

    for (UInt32 frame = 0; frame < numSampleFrames; frame++) {
        float left = (float)inputBuf32[0] / (float)INT32_MAX;
        float right = (float)inputBuf32[1] / (float)INT32_MAX;
        float mono;

        switch (config.mixMode) {
        case kAMDMicMixLeft:
            mono = left;
            break;
        case kAMDMicMixRight:
            mono = right;
            break;
        case kAMDMicMixSum:
            mono = left + right;
            break;
        default:
            mono = (left + right) * 0.5f;
            break;
        }
        inputBuf32 += NUM_CHANNELS;

        float finalGain = controlGain * softwareGain;

        /*
         * Optional, gentle downward expander. Disabled by default: the old
         * always-on gate (floor 0.18, slow release) chopped speech tails and
         * made the signal sound both quiet and choppy.
         */
        if (config.noiseGate) {
            float level = mono * controlGain;
            if (level < 0.0f)
                level = -level;

            expanderEnvelope = expanderEnvelope * INPUT_EXPANDER_ENVELOPE_COEFF
                + level * (1.0f - INPUT_EXPANDER_ENVELOPE_COEFF);

            float targetGain;
            if (expanderEnvelope >= INPUT_EXPANDER_THRESHOLD) {
                targetGain = 1.0f;
            } else {
                targetGain = INPUT_EXPANDER_FLOOR
                    + (1.0f - INPUT_EXPANDER_FLOOR) * expanderEnvelope / INPUT_EXPANDER_THRESHOLD;
            }

            float coefficient = (targetGain > expanderGain)
                ? INPUT_EXPANDER_ATTACK
                : INPUT_EXPANDER_RELEASE;
            expanderGain += (targetGain - expanderGain) * coefficient;
            finalGain *= expanderGain;
        }

        /* 10 ms click suppressor; the DMA buffer is already zeroed on start. */
        if (startupFadeFrame < STARTUP_FADE_FRAMES) {
            finalGain *= (float)startupFadeFrame / (float)STARTUP_FADE_FRAMES;
            startupFadeFrame++;
        }

        if (config.mixMode == kAMDMicMixStereo && channels >= 2) {
            float outLeft = AMDMicClampSample(left * finalGain);
            float outRight = AMDMicClampSample(right * finalGain);
            *floatDestBuf++ = outLeft;
            *floatDestBuf++ = outRight;
            float centre = (outLeft + outRight) * 0.5f;
            for (UInt32 channel = 2; channel < channels; channel++)
                *floatDestBuf++ = centre;
        } else {
            float sample = AMDMicClampSample(mono * finalGain);
            for (UInt32 channel = 0; channel < channels; channel++)
                *floatDestBuf++ = sample;
        }
    }

    return kIOReturnSuccess;
}

UInt32 AMDMicrophoneEngine::getCurrentSampleFrame()
{
    if (!audioDevice)
        return 0;

    UInt32 frame = (UInt32)((audioDevice->getRelativeByteCount() % BUFFER_SIZE) / FRAME_SIZE);
    if (frame >= NUM_FRAMES)
        frame = NUM_FRAMES - 1;

    return frame;
}

IOReturn AMDMicrophoneEngine::performAudioEngineStart()
{
    takeTimeStamp(false);
    startupFadeFrame = 0;
    expanderEnvelope = 0.0f;
    expanderGain = 1.0f;

    audioDevice->clearDMABuffer();
    audioDevice->initRingBuffer(ACP_MEM_WINDOW_START, BUFFER_SIZE, PERIOD_SIZE);
    audioDevice->writel(0x0, ACP_WOV_PDM_NO_OF_CHANNELS);
    audioDevice->writel(ACP_PDM_DECIMATION_FACTOR, ACP_WOV_PDM_DECIMATION_FACTOR);

    IOReturn ret = audioDevice->startDMA();
    if (ret != kIOReturnSuccess)
        return ret;

    IODelay(100);
    audioDevice->enableInterrupt();

    return kIOReturnSuccess;
}

IOReturn AMDMicrophoneEngine::performAudioEngineStop()
{
    audioDevice->disableInterrupt();
    audioDevice->stopDMA();

    return kIOReturnSuccess;
}

IOReturn AMDMicrophoneEngine::performFormatChange(
    IOAudioStream* audioStream, const IOAudioStreamFormat* newFormat, const IOAudioSampleRate* newSampleRate
)
{
    IOReturn result = kIOReturnSuccess;

    if (newSampleRate) {
        switch (newSampleRate->whole) {
        case SAMPLE_RATE:
            result = kIOReturnSuccess;
            break;
        default:
            result = kIOReturnUnsupported;
            break;
        }
    }

    return result;
}
