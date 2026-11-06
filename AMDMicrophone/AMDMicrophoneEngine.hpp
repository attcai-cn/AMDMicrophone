//
//  AMDMicrophoneEngine.hpp
//  AMDMicrophone
//
//  Created by Huy Duong on 30/06/2023.
//

#ifndef AMDMicrophoneEngine_hpp
#define AMDMicrophoneEngine_hpp

#include <IOKit/audio/IOAudioEngine.h>

#define NUM_PERIODS 4
#define PERIOD_SIZE 8192
#define BUFFER_SIZE (PERIOD_SIZE * NUM_PERIODS)

#define SAMPLE_RATE  48000
#define NUM_CHANNELS 2
#define SAMPLE_DEPTH 32
#define SAMPLE_WIDTH 32
#define FRAME_SIZE   (NUM_CHANNELS * SAMPLE_WIDTH / 8)
#define PERIOD_FRAMES (PERIOD_SIZE / FRAME_SIZE)
#define NUM_FRAMES   (BUFFER_SIZE / FRAME_SIZE)
#define MAX_VOLUME   100

/*
 * The old DSP chain was the main reason the microphone sounded quiet:
 *  - a 500 ms fade-in restarted on every engine start,
 *  - an aggressive downward expander (floor 0.18, 20 ms release) that kept
 *    speech tails and consonants ~15 dB down,
 *  - a fixed 3.0x software gain with no way to tune it.
 * Gain now comes from the device properties (see AMDMicrophoneCommon.hpp);
 * the expander is off by default and its floor/threshold are tuneable too.
 *
 * The higher gain makes the PDM noise floor audible, so a first-order
 * high-pass (~38 Hz at 48 kHz) strips DC offset and rumble before any gain.
 * Without it the DC offset also holds the gate's envelope detector open,
 * which is why a gate alone never silenced the hiss.
 */
#define STARTUP_FADE_FRAMES (SAMPLE_RATE / 100)
#define INPUT_DC_BLOCK_COEFF 0.995f
#define INPUT_EXPANDER_ENVELOPE_COEFF 0.999f
#define INPUT_EXPANDER_ATTACK 0.010f
#define INPUT_EXPANDER_RELEASE 0.0005f

class AMDMicrophoneDevice;

class AMDMicrophoneEngine : public IOAudioEngine {
    OSDeclareDefaultStructors(AMDMicrophoneEngine);

    AMDMicrophoneDevice* audioDevice = nullptr;
    UInt32 volume = MAX_VOLUME;
    UInt32 startupFadeFrame = STARTUP_FADE_FRAMES;
    float expanderEnvelope = 0.0f;
    float expanderGain = 1.0f;
    /* Per-channel high-pass state, indexed by hardware channel (left/right). */
    float dcPrevX[NUM_CHANNELS] = {};
    float dcPrevY[NUM_CHANNELS] = {};

    bool createControls();
    IOAudioStream* createNewAudioStream(
        IOAudioStreamDirection direction, void* sampleBuffer, UInt32 sampleBufferSize
    );
    static IOReturn gainChangeHandler(
        IOService* target, IOAudioControl* gainControl, SInt32 oldValue, SInt32 newValue
    );

public:
    bool init(AMDMicrophoneDevice* device);
    void free() override;

    bool initHardware(IOService* provider) override;
    void stop(IOService* provider) override;

    IOReturn convertInputSamples(
        const void* sampleBuf, void* destBuf, UInt32 firstSampleFrame, UInt32 numSampleFrames,
        const IOAudioStreamFormat* streamFormat, IOAudioStream* audioStream
    ) override;
    UInt32 getCurrentSampleFrame() override;
    IOReturn performAudioEngineStart() override;
    IOReturn performAudioEngineStop() override;
    IOReturn performFormatChange(
        IOAudioStream* audioStream, const IOAudioStreamFormat* newFormat, const IOAudioSampleRate* newSampleRate
    ) override;
};

#endif /* AMDMicrophoneEngine_hpp */
