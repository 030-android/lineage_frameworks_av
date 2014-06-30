/*
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#ifndef ANDROID_AUDIO_MIXER_H
#define ANDROID_AUDIO_MIXER_H

#include <stdint.h>
#include <sys/types.h>

#include <utils/threads.h>

#include <media/AudioBufferProvider.h>
#include "AudioResampler.h"

#include <audio_effects/effect_downmix.h>
#include <system/audio.h>
#include <media/nbaio/NBLog.h>

// FIXME This is actually unity gain, which might not be max in future, expressed in U.12
#define MAX_GAIN_INT AudioMixer::UNITY_GAIN_INT

namespace android {

// ----------------------------------------------------------------------------

class AudioMixer
{
public:
                            AudioMixer(size_t frameCount, uint32_t sampleRate,
                                       uint32_t maxNumTracks = MAX_NUM_TRACKS);

    /*virtual*/             ~AudioMixer();  // non-virtual saves a v-table, restore if sub-classed


    // This mixer has a hard-coded upper limit of 32 active track inputs.
    // Adding support for > 32 tracks would require more than simply changing this value.
    static const uint32_t MAX_NUM_TRACKS = 32;
    // maximum number of channels supported by the mixer

    // This mixer has a hard-coded upper limit of 2 channels for output.
    // There is support for > 2 channel tracks down-mixed to 2 channel output via a down-mix effect.
    // Adding support for > 2 channel output would require more than simply changing this value.
    static const uint32_t MAX_NUM_CHANNELS = 2;
    // maximum number of channels supported for the content
    static const uint32_t MAX_NUM_CHANNELS_TO_DOWNMIX = 8;

    static const uint16_t UNITY_GAIN_INT = 0x1000;
    static const float    UNITY_GAIN_FLOAT = 1.0f;

    enum { // names

        // track names (MAX_NUM_TRACKS units)
        TRACK0          = 0x1000,

        // 0x2000 is unused

        // setParameter targets
        TRACK           = 0x3000,
        RESAMPLE        = 0x3001,
        RAMP_VOLUME     = 0x3002, // ramp to new volume
        VOLUME          = 0x3003, // don't ramp

        // set Parameter names
        // for target TRACK
        CHANNEL_MASK    = 0x4000,
        FORMAT          = 0x4001,
        MAIN_BUFFER     = 0x4002,
        AUX_BUFFER      = 0x4003,
        DOWNMIX_TYPE    = 0X4004,
        MIXER_FORMAT    = 0x4005, // AUDIO_FORMAT_PCM_(FLOAT|16_BIT)
        // for target RESAMPLE
        SAMPLE_RATE     = 0x4100, // Configure sample rate conversion on this track name;
                                  // parameter 'value' is the new sample rate in Hz.
                                  // Only creates a sample rate converter the first time that
                                  // the track sample rate is different from the mix sample rate.
                                  // If the new sample rate is the same as the mix sample rate,
                                  // and a sample rate converter already exists,
                                  // then the sample rate converter remains present but is a no-op.
        RESET           = 0x4101, // Reset sample rate converter without changing sample rate.
                                  // This clears out the resampler's input buffer.
        REMOVE          = 0x4102, // Remove the sample rate converter on this track name;
                                  // the track is restored to the mix sample rate.
        // for target RAMP_VOLUME and VOLUME (8 channels max)
        // FIXME use float for these 3 to improve the dynamic range
        VOLUME0         = 0x4200,
        VOLUME1         = 0x4201,
        AUXLEVEL        = 0x4210,
    };


    // For all APIs with "name": TRACK0 <= name < TRACK0 + MAX_NUM_TRACKS

    // Allocate a track name.  Returns new track name if successful, -1 on failure.
    // The failure could be because of an invalid channelMask or format, or that
    // the track capacity of the mixer is exceeded.
    int         getTrackName(audio_channel_mask_t channelMask,
                             audio_format_t format, int sessionId);

    // Free an allocated track by name
    void        deleteTrackName(int name);

    // Enable or disable an allocated track by name
    void        enable(int name);
    void        disable(int name);

    void        setParameter(int name, int target, int param, void *value);

    void        setBufferProvider(int name, AudioBufferProvider* bufferProvider);
    void        process(int64_t pts);

    uint32_t    trackNames() const { return mTrackNames; }

    size_t      getUnreleasedFrames(int name) const;

    static inline bool isValidPcmTrackFormat(audio_format_t format) {
        return format == AUDIO_FORMAT_PCM_16_BIT ||
                format == AUDIO_FORMAT_PCM_24_BIT_PACKED ||
                format == AUDIO_FORMAT_PCM_32_BIT ||
                format == AUDIO_FORMAT_PCM_FLOAT;
    }

private:

    enum {
        // FIXME this representation permits up to 8 channels
        NEEDS_CHANNEL_COUNT__MASK   = 0x00000007,
    };

    enum {
        NEEDS_CHANNEL_1             = 0x00000000,   // mono
        NEEDS_CHANNEL_2             = 0x00000001,   // stereo

        // sample format is not explicitly specified, and is assumed to be AUDIO_FORMAT_PCM_16_BIT

        NEEDS_MUTE                  = 0x00000100,
        NEEDS_RESAMPLE              = 0x00001000,
        NEEDS_AUX                   = 0x00010000,
    };

    struct state_t;
    struct track_t;
    class DownmixerBufferProvider;
    class ReformatBufferProvider;

    typedef void (*hook_t)(track_t* t, int32_t* output, size_t numOutFrames, int32_t* temp,
                           int32_t* aux);
    static const int BLOCKSIZE = 16; // 4 cache lines

    struct track_t {
        uint32_t    needs;

        union {
        int16_t     volume[MAX_NUM_CHANNELS]; // [0]3.12 fixed point
        int32_t     volumeRL;
        };

        int32_t     prevVolume[MAX_NUM_CHANNELS];

        // 16-byte boundary

        int32_t     volumeInc[MAX_NUM_CHANNELS];
        int32_t     auxInc;
        int32_t     prevAuxLevel;

        // 16-byte boundary

        int16_t     auxLevel;       // 0 <= auxLevel <= MAX_GAIN_INT, but signed for mul performance
        uint16_t    frameCount;

        uint8_t     channelCount;   // 1 or 2, redundant with (needs & NEEDS_CHANNEL_COUNT__MASK)
        uint8_t     unused_padding; // formerly format, was always 16
        uint16_t    enabled;        // actually bool
        audio_channel_mask_t channelMask;

        // actual buffer provider used by the track hooks, see DownmixerBufferProvider below
        //  for how the Track buffer provider is wrapped by another one when dowmixing is required
        AudioBufferProvider*                bufferProvider;

        // 16-byte boundary

        mutable AudioBufferProvider::Buffer buffer; // 8 bytes

        hook_t      hook;
        const void* in;             // current location in buffer

        // 16-byte boundary

        AudioResampler*     resampler;
        uint32_t            sampleRate;
        int32_t*           mainBuffer;
        int32_t*           auxBuffer;

        // 16-byte boundary
        AudioBufferProvider*     mInputBufferProvider;    // 4 bytes
        ReformatBufferProvider*  mReformatBufferProvider; // 4 bytes
        DownmixerBufferProvider* downmixerBufferProvider; // 4 bytes

        int32_t     sessionId;

        // 16-byte boundary
        audio_format_t mMixerFormat;     // output mix format: AUDIO_FORMAT_PCM_(FLOAT|16_BIT)
        audio_format_t mFormat;          // input track format
        audio_format_t mMixerInFormat;   // mix internal format AUDIO_FORMAT_PCM_(FLOAT|16_BIT)
                                         // each track must be converted to this format.

        int32_t        mUnused[1];       // alignment padding

        // 16-byte boundary

        bool        needsRamp() { return (volumeInc[0] | volumeInc[1] | auxInc) != 0; }
        bool        setResampler(uint32_t sampleRate, uint32_t devSampleRate);
        bool        doesResample() const { return resampler != NULL; }
        void        resetResampler() { if (resampler != NULL) resampler->reset(); }
        void        adjustVolumeRamp(bool aux);
        size_t      getUnreleasedFrames() const { return resampler != NULL ?
                                                    resampler->getUnreleasedFrames() : 0; };
    };

    typedef void (*process_hook_t)(state_t* state, int64_t pts);

    // pad to 32-bytes to fill cache line
    struct state_t {
        uint32_t        enabledTracks;
        uint32_t        needsChanged;
        size_t          frameCount;
        process_hook_t  hook;   // one of process__*, never NULL
        int32_t         *outputTemp;
        int32_t         *resampleTemp;
        NBLog::Writer*  mLog;
        int32_t         reserved[1];
        // FIXME allocate dynamically to save some memory when maxNumTracks < MAX_NUM_TRACKS
        track_t         tracks[MAX_NUM_TRACKS] __attribute__((aligned(32)));
    };

    // AudioBufferProvider that wraps a track AudioBufferProvider by a call to a downmix effect
    class DownmixerBufferProvider : public AudioBufferProvider {
    public:
        virtual status_t getNextBuffer(Buffer* buffer, int64_t pts);
        virtual void releaseBuffer(Buffer* buffer);
        DownmixerBufferProvider();
        virtual ~DownmixerBufferProvider();

        AudioBufferProvider* mTrackBufferProvider;
        effect_handle_t    mDownmixHandle;
        effect_config_t    mDownmixConfig;
    };

    // AudioBufferProvider wrapper that reformats track to acceptable mixer input type
    class ReformatBufferProvider : public AudioBufferProvider {
    public:
        ReformatBufferProvider(int32_t channels,
                audio_format_t inputFormat, audio_format_t outputFormat);
        virtual ~ReformatBufferProvider();

        // overrides AudioBufferProvider methods
        virtual status_t getNextBuffer(Buffer* buffer, int64_t pts);
        virtual void releaseBuffer(Buffer* buffer);

        void reset();
        inline bool requiresInternalBuffers() {
            return true; //mInputFrameSize < mOutputFrameSize;
        }

        AudioBufferProvider* mTrackBufferProvider;
        int32_t              mChannels;
        audio_format_t       mInputFormat;
        audio_format_t       mOutputFormat;
        size_t               mInputFrameSize;
        size_t               mOutputFrameSize;
        // (only) required for reformatting to a larger size.
        AudioBufferProvider::Buffer mBuffer;
        void*                mOutputData;
        size_t               mOutputCount;
        size_t               mConsumed;
    };

    // bitmask of allocated track names, where bit 0 corresponds to TRACK0 etc.
    uint32_t        mTrackNames;

    // bitmask of configured track names; ~0 if maxNumTracks == MAX_NUM_TRACKS,
    // but will have fewer bits set if maxNumTracks < MAX_NUM_TRACKS
    const uint32_t  mConfiguredNames;

    const uint32_t  mSampleRate;

    NBLog::Writer   mDummyLog;
public:
    void            setLog(NBLog::Writer* log);
private:
    state_t         mState __attribute__((aligned(32)));

    // effect descriptor for the downmixer used by the mixer
    static effect_descriptor_t sDwnmFxDesc;
    // indicates whether a downmix effect has been found and is usable by this mixer
    static bool                sIsMultichannelCapable;

    // Call after changing either the enabled status of a track, or parameters of an enabled track.
    // OK to call more often than that, but unnecessary.
    void invalidateState(uint32_t mask);

    static status_t initTrackDownmix(track_t* pTrack, int trackNum, audio_channel_mask_t mask);
    static status_t prepareTrackForDownmix(track_t* pTrack, int trackNum);
    static void unprepareTrackForDownmix(track_t* pTrack, int trackName);
    static status_t prepareTrackForReformat(track_t* pTrack, int trackNum);
    static void unprepareTrackForReformat(track_t* pTrack, int trackName);
    static void reconfigureBufferProviders(track_t* pTrack);

    static void track__genericResample(track_t* t, int32_t* out, size_t numFrames, int32_t* temp,
            int32_t* aux);
    static void track__nop(track_t* t, int32_t* out, size_t numFrames, int32_t* temp, int32_t* aux);
    static void track__16BitsStereo(track_t* t, int32_t* out, size_t numFrames, int32_t* temp,
            int32_t* aux);
    static void track__16BitsMono(track_t* t, int32_t* out, size_t numFrames, int32_t* temp,
            int32_t* aux);
    static void volumeRampStereo(track_t* t, int32_t* out, size_t frameCount, int32_t* temp,
            int32_t* aux);
    static void volumeStereo(track_t* t, int32_t* out, size_t frameCount, int32_t* temp,
            int32_t* aux);

    static void process__validate(state_t* state, int64_t pts);
    static void process__nop(state_t* state, int64_t pts);
    static void process__genericNoResampling(state_t* state, int64_t pts);
    static void process__genericResampling(state_t* state, int64_t pts);
    static void process__OneTrack16BitsStereoNoResampling(state_t* state,
                                                          int64_t pts);
#if 0
    static void process__TwoTracks16BitsStereoNoResampling(state_t* state,
                                                           int64_t pts);
#endif

    static int64_t calculateOutputPTS(const track_t& t, int64_t basePTS,
                                      int outputFrameIndex);

    static uint64_t         sLocalTimeFreq;
    static pthread_once_t   sOnceControl;
    static void             sInitRoutine();

    // multi-format process hooks
    template <int MIXTYPE, int NCHAN, typename TO, typename TI, typename TA>
    static void process_NoResampleOneTrack(state_t* state, int64_t pts);

    // multi-format track hooks
    template <int MIXTYPE, int NCHAN, typename TO, typename TI, typename TA>
    static void track__Resample(track_t* t, TO* out, size_t frameCount,
            TO* temp __unused, TA* aux);
    template <int MIXTYPE, int NCHAN, typename TO, typename TI, typename TA>
    static void track__NoResample(track_t* t, TO* out, size_t frameCount,
            TO* temp __unused, TA* aux);

    static void convertMixerFormat(void *out, audio_format_t mixerOutFormat,
            void *in, audio_format_t mixerInFormat, size_t sampleCount);

    // hook types
    enum {
        PROCESSTYPE_NORESAMPLEONETRACK,
    };
    enum {
        TRACKTYPE_NOP,
        TRACKTYPE_RESAMPLE,
        TRACKTYPE_NORESAMPLE,
        TRACKTYPE_NORESAMPLEMONO,
    };

    // functions for determining the proper process and track hooks.
    static process_hook_t getProcessHook(int processType, int channels,
            audio_format_t mixerInFormat, audio_format_t mixerOutFormat);
    static hook_t getTrackHook(int trackType, int channels,
            audio_format_t mixerInFormat, audio_format_t mixerOutFormat);
};

// ----------------------------------------------------------------------------
}; // namespace android

#endif // ANDROID_AUDIO_MIXER_H
