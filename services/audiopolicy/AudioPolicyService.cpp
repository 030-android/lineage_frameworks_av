/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "AudioPolicyService"
//#define LOG_NDEBUG 0

#include "Configuration.h"
#undef __STRICT_ANSI__
#define __STDINT_LIMITS
#define __STDC_LIMIT_MACROS
#include <stdint.h>

#include <sys/time.h>
#include <binder/IServiceManager.h>
#include <utils/Log.h>
#include <cutils/properties.h>
#include <binder/IPCThreadState.h>
#include <utils/String16.h>
#include <utils/threads.h>
#include "AudioPolicyService.h"
#include "ServiceUtilities.h"
#include <hardware_legacy/power.h>
#include <media/AudioEffect.h>
#include <media/EffectsFactoryApi.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <hardware/audio_policy.h>
#include <audio_effects/audio_effects_conf.h>
#include <media/AudioParameter.h>

namespace android {

static const char kDeadlockedString[] = "AudioPolicyService may be deadlocked\n";
static const char kCmdDeadlockedString[] = "AudioPolicyService command thread may be deadlocked\n";

static const int kDumpLockRetries = 50;
static const int kDumpLockSleepUs = 20000;

static const nsecs_t kAudioCommandTimeoutNs = seconds(3); // 3 seconds

namespace {
    extern struct audio_policy_service_ops aps_ops;
};

// ----------------------------------------------------------------------------

AudioPolicyService::AudioPolicyService()
    : BnAudioPolicyService(), mpAudioPolicyDev(NULL), mpAudioPolicy(NULL),
      mAudioPolicyManager(NULL), mAudioPolicyClient(NULL)
{
    char value[PROPERTY_VALUE_MAX];
    const struct hw_module_t *module;
    int forced_val;
    int rc;

    Mutex::Autolock _l(mLock);

    // start tone playback thread
    mTonePlaybackThread = new AudioCommandThread(String8("ApmTone"), this);
    // start audio commands thread
    mAudioCommandThread = new AudioCommandThread(String8("ApmAudio"), this);
    // start output activity command thread
    mOutputCommandThread = new AudioCommandThread(String8("ApmOutput"), this);

#ifdef USE_LEGACY_AUDIO_POLICY
    ALOGI("AudioPolicyService CSTOR in legacy mode");

    /* instantiate the audio policy manager */
    rc = hw_get_module(AUDIO_POLICY_HARDWARE_MODULE_ID, &module);
    if (rc) {
        return;
    }
    rc = audio_policy_dev_open(module, &mpAudioPolicyDev);
    ALOGE_IF(rc, "couldn't open audio policy device (%s)", strerror(-rc));
    if (rc) {
        return;
    }

    rc = mpAudioPolicyDev->create_audio_policy(mpAudioPolicyDev, &aps_ops, this,
                                               &mpAudioPolicy);
    ALOGE_IF(rc, "couldn't create audio policy (%s)", strerror(-rc));
    if (rc) {
        return;
    }

    rc = mpAudioPolicy->init_check(mpAudioPolicy);
    ALOGE_IF(rc, "couldn't init_check the audio policy (%s)", strerror(-rc));
    if (rc) {
        return;
    }
    ALOGI("Loaded audio policy from %s (%s)", module->name, module->id);
#else
    ALOGI("AudioPolicyService CSTOR in new mode");

    mAudioPolicyClient = new AudioPolicyClient(this);
    mAudioPolicyManager = new AudioPolicyManager(mAudioPolicyClient);
#endif

    // load audio pre processing modules
    if (access(AUDIO_EFFECT_VENDOR_CONFIG_FILE, R_OK) == 0) {
        loadPreProcessorConfig(AUDIO_EFFECT_VENDOR_CONFIG_FILE);
    } else if (access(AUDIO_EFFECT_DEFAULT_CONFIG_FILE, R_OK) == 0) {
        loadPreProcessorConfig(AUDIO_EFFECT_DEFAULT_CONFIG_FILE);
    }
}

AudioPolicyService::~AudioPolicyService()
{
    mTonePlaybackThread->exit();
    mAudioCommandThread->exit();
    mOutputCommandThread->exit();

    // release audio pre processing resources
    for (size_t i = 0; i < mInputSources.size(); i++) {
        delete mInputSources.valueAt(i);
    }
    mInputSources.clear();

    for (size_t i = 0; i < mInputs.size(); i++) {
        mInputs.valueAt(i)->mEffects.clear();
        delete mInputs.valueAt(i);
    }
    mInputs.clear();

#ifdef USE_LEGACY_AUDIO_POLICY
    if (mpAudioPolicy != NULL && mpAudioPolicyDev != NULL) {
        mpAudioPolicyDev->destroy_audio_policy(mpAudioPolicyDev, mpAudioPolicy);
    }
    if (mpAudioPolicyDev != NULL) {
        audio_policy_dev_close(mpAudioPolicyDev);
    }
#else
    delete mAudioPolicyManager;
    delete mAudioPolicyClient;
#endif

    mNotificationClients.clear();
}

// A notification client is always registered by AudioSystem when the client process
// connects to AudioPolicyService.
void AudioPolicyService::registerClient(const sp<IAudioPolicyServiceClient>& client)
{

    Mutex::Autolock _l(mLock);

    uid_t uid = IPCThreadState::self()->getCallingUid();
    if (mNotificationClients.indexOfKey(uid) < 0) {
        sp<NotificationClient> notificationClient = new NotificationClient(this,
                                                                           client,
                                                                           uid);
        ALOGV("registerClient() client %p, uid %d", client.get(), uid);

        mNotificationClients.add(uid, notificationClient);

        sp<IBinder> binder = client->asBinder();
        binder->linkToDeath(notificationClient);
    }
}

// removeNotificationClient() is called when the client process dies.
void AudioPolicyService::removeNotificationClient(uid_t uid)
{
    Mutex::Autolock _l(mLock);

    mNotificationClients.removeItem(uid);

#ifndef USE_LEGACY_AUDIO_POLICY
        if (mAudioPolicyManager) {
            mAudioPolicyManager->clearAudioPatches(uid);
        }
#endif
}

void AudioPolicyService::onAudioPortListUpdate()
{
    mOutputCommandThread->updateAudioPortListCommand();
}

void AudioPolicyService::doOnAudioPortListUpdate()
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onAudioPortListUpdate();
    }
}

void AudioPolicyService::onAudioPatchListUpdate()
{
    mOutputCommandThread->updateAudioPatchListCommand();
}

status_t AudioPolicyService::clientCreateAudioPatch(const struct audio_patch *patch,
                                                audio_patch_handle_t *handle,
                                                int delayMs)
{
    return mAudioCommandThread->createAudioPatchCommand(patch, handle, delayMs);
}

status_t AudioPolicyService::clientReleaseAudioPatch(audio_patch_handle_t handle,
                                                 int delayMs)
{
    return mAudioCommandThread->releaseAudioPatchCommand(handle, delayMs);
}

void AudioPolicyService::doOnAudioPatchListUpdate()
{
    Mutex::Autolock _l(mLock);
    for (size_t i = 0; i < mNotificationClients.size(); i++) {
        mNotificationClients.valueAt(i)->onAudioPatchListUpdate();
    }
}

status_t AudioPolicyService::clientSetAudioPortConfig(const struct audio_port_config *config,
                                                      int delayMs)
{
    return mAudioCommandThread->setAudioPortConfigCommand(config, delayMs);
}

AudioPolicyService::NotificationClient::NotificationClient(const sp<AudioPolicyService>& service,
                                                     const sp<IAudioPolicyServiceClient>& client,
                                                     uid_t uid)
    : mService(service), mUid(uid), mAudioPolicyServiceClient(client)
{
}

AudioPolicyService::NotificationClient::~NotificationClient()
{
}

void AudioPolicyService::NotificationClient::binderDied(const wp<IBinder>& who __unused)
{
    sp<NotificationClient> keep(this);
    sp<AudioPolicyService> service = mService.promote();
    if (service != 0) {
        service->removeNotificationClient(mUid);
    }
}

void AudioPolicyService::NotificationClient::onAudioPortListUpdate()
{
    if (mAudioPolicyServiceClient != 0) {
        mAudioPolicyServiceClient->onAudioPortListUpdate();
    }
}

void AudioPolicyService::NotificationClient::onAudioPatchListUpdate()
{
    if (mAudioPolicyServiceClient != 0) {
        mAudioPolicyServiceClient->onAudioPatchListUpdate();
    }
}

void AudioPolicyService::binderDied(const wp<IBinder>& who) {
    ALOGW("binderDied() %p, calling pid %d", who.unsafe_get(),
            IPCThreadState::self()->getCallingPid());
}

static bool tryLock(Mutex& mutex)
{
    bool locked = false;
    for (int i = 0; i < kDumpLockRetries; ++i) {
        if (mutex.tryLock() == NO_ERROR) {
            locked = true;
            break;
        }
        usleep(kDumpLockSleepUs);
    }
    return locked;
}

status_t AudioPolicyService::dumpInternals(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

#ifdef USE_LEGACY_AUDIO_POLICY
    snprintf(buffer, SIZE, "PolicyManager Interface: %p\n", mpAudioPolicy);
#else
    snprintf(buffer, SIZE, "AudioPolicyManager: %p\n", mAudioPolicyManager);
#endif
    result.append(buffer);
    snprintf(buffer, SIZE, "Command Thread: %p\n", mAudioCommandThread.get());
    result.append(buffer);
    snprintf(buffer, SIZE, "Tones Thread: %p\n", mTonePlaybackThread.get());
    result.append(buffer);

    write(fd, result.string(), result.size());
    return NO_ERROR;
}

status_t AudioPolicyService::dump(int fd, const Vector<String16>& args __unused)
{
    if (!dumpAllowed()) {
        dumpPermissionDenial(fd);
    } else {
        bool locked = tryLock(mLock);
        if (!locked) {
            String8 result(kDeadlockedString);
            write(fd, result.string(), result.size());
        }

        dumpInternals(fd);
        if (mAudioCommandThread != 0) {
            mAudioCommandThread->dump(fd);
        }
        if (mTonePlaybackThread != 0) {
            mTonePlaybackThread->dump(fd);
        }

#ifdef USE_LEGACY_AUDIO_POLICY
        if (mpAudioPolicy) {
            mpAudioPolicy->dump(mpAudioPolicy, fd);
        }
#else
        if (mAudioPolicyManager) {
            mAudioPolicyManager->dump(fd);
        }
#endif

        if (locked) mLock.unlock();
    }
    return NO_ERROR;
}

status_t AudioPolicyService::dumpPermissionDenial(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    snprintf(buffer, SIZE, "Permission Denial: "
            "can't dump AudioPolicyService from pid=%d, uid=%d\n",
            IPCThreadState::self()->getCallingPid(),
            IPCThreadState::self()->getCallingUid());
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

void AudioPolicyService::setPreProcessorEnabled(const InputDesc *inputDesc, bool enabled)
{
    const Vector<sp<AudioEffect> > &fxVector = inputDesc->mEffects;
    for (size_t i = 0; i < fxVector.size(); i++) {
        fxVector.itemAt(i)->setEnabled(enabled);
    }
}

status_t AudioPolicyService::onTransact(
        uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)
{
    return BnAudioPolicyService::onTransact(code, data, reply, flags);
}


// -----------  AudioPolicyService::AudioCommandThread implementation ----------

AudioPolicyService::AudioCommandThread::AudioCommandThread(String8 name,
                                                           const wp<AudioPolicyService>& service)
    : Thread(false), mName(name), mService(service)
{
    mpToneGenerator = NULL;
}


AudioPolicyService::AudioCommandThread::~AudioCommandThread()
{
    if (!mAudioCommands.isEmpty()) {
        release_wake_lock(mName.string());
    }
    mAudioCommands.clear();
    delete mpToneGenerator;
}

void AudioPolicyService::AudioCommandThread::onFirstRef()
{
    run(mName.string(), ANDROID_PRIORITY_AUDIO);
}

bool AudioPolicyService::AudioCommandThread::threadLoop()
{
    nsecs_t waitTime = INT64_MAX;

    mLock.lock();
    while (!exitPending())
    {
        sp<AudioPolicyService> svc;
        while (!mAudioCommands.isEmpty() && !exitPending()) {
            nsecs_t curTime = systemTime();
            // commands are sorted by increasing time stamp: execute them from index 0 and up
            if (mAudioCommands[0]->mTime <= curTime) {
                sp<AudioCommand> command = mAudioCommands[0];
                mAudioCommands.removeAt(0);
                mLastCommand = command;

                switch (command->mCommand) {
                case START_TONE: {
                    mLock.unlock();
                    ToneData *data = (ToneData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing start tone %d on stream %d",
                            data->mType, data->mStream);
                    delete mpToneGenerator;
                    mpToneGenerator = new ToneGenerator(data->mStream, 1.0);
                    mpToneGenerator->startTone(data->mType);
                    mLock.lock();
                    }break;
                case STOP_TONE: {
                    mLock.unlock();
                    ALOGV("AudioCommandThread() processing stop tone");
                    if (mpToneGenerator != NULL) {
                        mpToneGenerator->stopTone();
                        delete mpToneGenerator;
                        mpToneGenerator = NULL;
                    }
                    mLock.lock();
                    }break;
                case SET_VOLUME: {
                    VolumeData *data = (VolumeData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing set volume stream %d, \
                            volume %f, output %d", data->mStream, data->mVolume, data->mIO);
                    command->mStatus = AudioSystem::setStreamVolume(data->mStream,
                                                                    data->mVolume,
                                                                    data->mIO);
                    }break;
                case SET_PARAMETERS: {
                    ParametersData *data = (ParametersData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing set parameters string %s, io %d",
                            data->mKeyValuePairs.string(), data->mIO);
                    command->mStatus = AudioSystem::setParameters(data->mIO, data->mKeyValuePairs);
                    }break;
                case SET_VOICE_VOLUME: {
                    VoiceVolumeData *data = (VoiceVolumeData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing set voice volume volume %f",
                            data->mVolume);
                    command->mStatus = AudioSystem::setVoiceVolume(data->mVolume);
                    }break;
                case STOP_OUTPUT: {
                    StopOutputData *data = (StopOutputData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing stop output %d",
                            data->mIO);
                    svc = mService.promote();
                    if (svc == 0) {
                        break;
                    }
                    mLock.unlock();
                    svc->doStopOutput(data->mIO, data->mStream, data->mSession);
                    mLock.lock();
                    }break;
                case RELEASE_OUTPUT: {
                    ReleaseOutputData *data = (ReleaseOutputData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing release output %d",
                            data->mIO);
                    svc = mService.promote();
                    if (svc == 0) {
                        break;
                    }
                    mLock.unlock();
                    svc->doReleaseOutput(data->mIO);
                    mLock.lock();
                    }break;
                case CREATE_AUDIO_PATCH: {
                    CreateAudioPatchData *data = (CreateAudioPatchData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing create audio patch");
                    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
                    if (af == 0) {
                        command->mStatus = PERMISSION_DENIED;
                    } else {
                        command->mStatus = af->createAudioPatch(&data->mPatch, &data->mHandle);
                    }
                    } break;
                case RELEASE_AUDIO_PATCH: {
                    ReleaseAudioPatchData *data = (ReleaseAudioPatchData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing release audio patch");
                    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
                    if (af == 0) {
                        command->mStatus = PERMISSION_DENIED;
                    } else {
                        command->mStatus = af->releaseAudioPatch(data->mHandle);
                    }
                    } break;
                case UPDATE_AUDIOPORT_LIST: {
                    ALOGV("AudioCommandThread() processing update audio port list");
                    svc = mService.promote();
                    if (svc == 0) {
                        break;
                    }
                    mLock.unlock();
                    svc->doOnAudioPortListUpdate();
                    mLock.lock();
                    }break;
                case UPDATE_AUDIOPATCH_LIST: {
                    ALOGV("AudioCommandThread() processing update audio patch list");
                    svc = mService.promote();
                    if (svc == 0) {
                        break;
                    }
                    mLock.unlock();
                    svc->doOnAudioPatchListUpdate();
                    mLock.lock();
                    }break;
                case SET_AUDIOPORT_CONFIG: {
                    SetAudioPortConfigData *data = (SetAudioPortConfigData *)command->mParam.get();
                    ALOGV("AudioCommandThread() processing set port config");
                    sp<IAudioFlinger> af = AudioSystem::get_audio_flinger();
                    if (af == 0) {
                        command->mStatus = PERMISSION_DENIED;
                    } else {
                        command->mStatus = af->setAudioPortConfig(&data->mConfig);
                    }
                    } break;
                default:
                    ALOGW("AudioCommandThread() unknown command %d", command->mCommand);
                }
                {
                    Mutex::Autolock _l(command->mLock);
                    if (command->mWaitStatus) {
                        command->mWaitStatus = false;
                        command->mCond.signal();
                    }
                }
                waitTime = INT64_MAX;
            } else {
                waitTime = mAudioCommands[0]->mTime - curTime;
                break;
            }
        }
        // release delayed commands wake lock
        if (mAudioCommands.isEmpty()) {
            release_wake_lock(mName.string());
        }
        // release mLock before releasing strong reference on the service as
        // AudioPolicyService destructor calls AudioCommandThread::exit() which acquires mLock.
        mLock.unlock();
        svc.clear();
        mLock.lock();
        if (!exitPending()) {
            ALOGV("AudioCommandThread() going to sleep");
            mWaitWorkCV.waitRelative(mLock, waitTime);
            ALOGV("AudioCommandThread() waking up");
        }
    }
    mLock.unlock();
    return false;
}

status_t AudioPolicyService::AudioCommandThread::dump(int fd)
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;

    snprintf(buffer, SIZE, "AudioCommandThread %p Dump\n", this);
    result.append(buffer);
    write(fd, result.string(), result.size());

    bool locked = tryLock(mLock);
    if (!locked) {
        String8 result2(kCmdDeadlockedString);
        write(fd, result2.string(), result2.size());
    }

    snprintf(buffer, SIZE, "- Commands:\n");
    result = String8(buffer);
    result.append("   Command Time        Wait pParam\n");
    for (size_t i = 0; i < mAudioCommands.size(); i++) {
        mAudioCommands[i]->dump(buffer, SIZE);
        result.append(buffer);
    }
    result.append("  Last Command\n");
    if (mLastCommand != 0) {
        mLastCommand->dump(buffer, SIZE);
        result.append(buffer);
    } else {
        result.append("     none\n");
    }

    write(fd, result.string(), result.size());

    if (locked) mLock.unlock();

    return NO_ERROR;
}

void AudioPolicyService::AudioCommandThread::startToneCommand(ToneGenerator::tone_type type,
        audio_stream_type_t stream)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = START_TONE;
    sp<ToneData> data = new ToneData();
    data->mType = type;
    data->mStream = stream;
    command->mParam = data;
    ALOGV("AudioCommandThread() adding tone start type %d, stream %d", type, stream);
    sendCommand(command);
}

void AudioPolicyService::AudioCommandThread::stopToneCommand()
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = STOP_TONE;
    ALOGV("AudioCommandThread() adding tone stop");
    sendCommand(command);
}

status_t AudioPolicyService::AudioCommandThread::volumeCommand(audio_stream_type_t stream,
                                                               float volume,
                                                               audio_io_handle_t output,
                                                               int delayMs)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = SET_VOLUME;
    sp<VolumeData> data = new VolumeData();
    data->mStream = stream;
    data->mVolume = volume;
    data->mIO = output;
    command->mParam = data;
    command->mWaitStatus = true;
    ALOGV("AudioCommandThread() adding set volume stream %d, volume %f, output %d",
            stream, volume, output);
    return sendCommand(command, delayMs);
}

status_t AudioPolicyService::AudioCommandThread::parametersCommand(audio_io_handle_t ioHandle,
                                                                   const char *keyValuePairs,
                                                                   int delayMs)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = SET_PARAMETERS;
    sp<ParametersData> data = new ParametersData();
    data->mIO = ioHandle;
    data->mKeyValuePairs = String8(keyValuePairs);
    command->mParam = data;
    command->mWaitStatus = true;
    ALOGV("AudioCommandThread() adding set parameter string %s, io %d ,delay %d",
            keyValuePairs, ioHandle, delayMs);
    return sendCommand(command, delayMs);
}

status_t AudioPolicyService::AudioCommandThread::voiceVolumeCommand(float volume, int delayMs)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = SET_VOICE_VOLUME;
    sp<VoiceVolumeData> data = new VoiceVolumeData();
    data->mVolume = volume;
    command->mParam = data;
    command->mWaitStatus = true;
    ALOGV("AudioCommandThread() adding set voice volume volume %f", volume);
    return sendCommand(command, delayMs);
}

void AudioPolicyService::AudioCommandThread::stopOutputCommand(audio_io_handle_t output,
                                                               audio_stream_type_t stream,
                                                               int session)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = STOP_OUTPUT;
    sp<StopOutputData> data = new StopOutputData();
    data->mIO = output;
    data->mStream = stream;
    data->mSession = session;
    command->mParam = data;
    ALOGV("AudioCommandThread() adding stop output %d", output);
    sendCommand(command);
}

void AudioPolicyService::AudioCommandThread::releaseOutputCommand(audio_io_handle_t output)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = RELEASE_OUTPUT;
    sp<ReleaseOutputData> data = new ReleaseOutputData();
    data->mIO = output;
    command->mParam = data;
    ALOGV("AudioCommandThread() adding release output %d", output);
    sendCommand(command);
}

status_t AudioPolicyService::AudioCommandThread::createAudioPatchCommand(
                                                const struct audio_patch *patch,
                                                audio_patch_handle_t *handle,
                                                int delayMs)
{
    status_t status = NO_ERROR;

    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = CREATE_AUDIO_PATCH;
    CreateAudioPatchData *data = new CreateAudioPatchData();
    data->mPatch = *patch;
    data->mHandle = *handle;
    command->mParam = data;
    command->mWaitStatus = true;
    ALOGV("AudioCommandThread() adding create patch delay %d", delayMs);
    status = sendCommand(command, delayMs);
    if (status == NO_ERROR) {
        *handle = data->mHandle;
    }
    return status;
}

status_t AudioPolicyService::AudioCommandThread::releaseAudioPatchCommand(audio_patch_handle_t handle,
                                                 int delayMs)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = RELEASE_AUDIO_PATCH;
    ReleaseAudioPatchData *data = new ReleaseAudioPatchData();
    data->mHandle = handle;
    command->mParam = data;
    command->mWaitStatus = true;
    ALOGV("AudioCommandThread() adding release patch delay %d", delayMs);
    return sendCommand(command, delayMs);
}

void AudioPolicyService::AudioCommandThread::updateAudioPortListCommand()
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = UPDATE_AUDIOPORT_LIST;
    ALOGV("AudioCommandThread() adding update audio port list");
    sendCommand(command);
}

void AudioPolicyService::AudioCommandThread::updateAudioPatchListCommand()
{
    sp<AudioCommand>command = new AudioCommand();
    command->mCommand = UPDATE_AUDIOPATCH_LIST;
    ALOGV("AudioCommandThread() adding update audio patch list");
    sendCommand(command);
}

status_t AudioPolicyService::AudioCommandThread::setAudioPortConfigCommand(
                                            const struct audio_port_config *config, int delayMs)
{
    sp<AudioCommand> command = new AudioCommand();
    command->mCommand = SET_AUDIOPORT_CONFIG;
    SetAudioPortConfigData *data = new SetAudioPortConfigData();
    data->mConfig = *config;
    command->mParam = data;
    command->mWaitStatus = true;
    ALOGV("AudioCommandThread() adding set port config delay %d", delayMs);
    return sendCommand(command, delayMs);
}

status_t AudioPolicyService::AudioCommandThread::sendCommand(sp<AudioCommand>& command, int delayMs)
{
    {
        Mutex::Autolock _l(mLock);
        insertCommand_l(command, delayMs);
        mWaitWorkCV.signal();
    }
    Mutex::Autolock _l(command->mLock);
    while (command->mWaitStatus) {
        nsecs_t timeOutNs = kAudioCommandTimeoutNs + milliseconds(delayMs);
        if (command->mCond.waitRelative(command->mLock, timeOutNs) != NO_ERROR) {
            command->mStatus = TIMED_OUT;
            command->mWaitStatus = false;
        }
    }
    return command->mStatus;
}

// insertCommand_l() must be called with mLock held
void AudioPolicyService::AudioCommandThread::insertCommand_l(sp<AudioCommand>& command, int delayMs)
{
    ssize_t i;  // not size_t because i will count down to -1
    Vector < sp<AudioCommand> > removedCommands;
    command->mTime = systemTime() + milliseconds(delayMs);

    // acquire wake lock to make sure delayed commands are processed
    if (mAudioCommands.isEmpty()) {
        acquire_wake_lock(PARTIAL_WAKE_LOCK, mName.string());
    }

    // check same pending commands with later time stamps and eliminate them
    for (i = mAudioCommands.size()-1; i >= 0; i--) {
        sp<AudioCommand> command2 = mAudioCommands[i];
        // commands are sorted by increasing time stamp: no need to scan the rest of mAudioCommands
        if (command2->mTime <= command->mTime) break;
        if (command2->mCommand != command->mCommand) continue;

        switch (command->mCommand) {
        case SET_PARAMETERS: {
            ParametersData *data = (ParametersData *)command->mParam.get();
            ParametersData *data2 = (ParametersData *)command2->mParam.get();
            if (data->mIO != data2->mIO) break;
            ALOGV("Comparing parameter command %s to new command %s",
                    data2->mKeyValuePairs.string(), data->mKeyValuePairs.string());
            AudioParameter param = AudioParameter(data->mKeyValuePairs);
            AudioParameter param2 = AudioParameter(data2->mKeyValuePairs);
            for (size_t j = 0; j < param.size(); j++) {
                String8 key;
                String8 value;
                param.getAt(j, key, value);
                for (size_t k = 0; k < param2.size(); k++) {
                    String8 key2;
                    String8 value2;
                    param2.getAt(k, key2, value2);
                    if (key2 == key) {
                        param2.remove(key2);
                        ALOGV("Filtering out parameter %s", key2.string());
                        break;
                    }
                }
            }
            // if all keys have been filtered out, remove the command.
            // otherwise, update the key value pairs
            if (param2.size() == 0) {
                removedCommands.add(command2);
            } else {
                data2->mKeyValuePairs = param2.toString();
            }
            command->mTime = command2->mTime;
            // force delayMs to non 0 so that code below does not request to wait for
            // command status as the command is now delayed
            delayMs = 1;
        } break;

        case SET_VOLUME: {
            VolumeData *data = (VolumeData *)command->mParam.get();
            VolumeData *data2 = (VolumeData *)command2->mParam.get();
            if (data->mIO != data2->mIO) break;
            if (data->mStream != data2->mStream) break;
            ALOGV("Filtering out volume command on output %d for stream %d",
                    data->mIO, data->mStream);
            removedCommands.add(command2);
            command->mTime = command2->mTime;
            // force delayMs to non 0 so that code below does not request to wait for
            // command status as the command is now delayed
            delayMs = 1;
        } break;
        case START_TONE:
        case STOP_TONE:
        default:
            break;
        }
    }

    // remove filtered commands
    for (size_t j = 0; j < removedCommands.size(); j++) {
        // removed commands always have time stamps greater than current command
        for (size_t k = i + 1; k < mAudioCommands.size(); k++) {
            if (mAudioCommands[k].get() == removedCommands[j].get()) {
                ALOGV("suppressing command: %d", mAudioCommands[k]->mCommand);
                mAudioCommands.removeAt(k);
                break;
            }
        }
    }
    removedCommands.clear();

    // Disable wait for status if delay is not 0
    if (delayMs != 0) {
        command->mWaitStatus = false;
    }

    // insert command at the right place according to its time stamp
    ALOGV("inserting command: %d at index %d, num commands %d",
            command->mCommand, (int)i+1, mAudioCommands.size());
    mAudioCommands.insertAt(command, i + 1);
}

void AudioPolicyService::AudioCommandThread::exit()
{
    ALOGV("AudioCommandThread::exit");
    {
        AutoMutex _l(mLock);
        requestExit();
        mWaitWorkCV.signal();
    }
    requestExitAndWait();
}

void AudioPolicyService::AudioCommandThread::AudioCommand::dump(char* buffer, size_t size)
{
    snprintf(buffer, size, "   %02d      %06d.%03d  %01u    %p\n",
            mCommand,
            (int)ns2s(mTime),
            (int)ns2ms(mTime)%1000,
            mWaitStatus,
            mParam.get());
}

/******* helpers for the service_ops callbacks defined below *********/
void AudioPolicyService::setParameters(audio_io_handle_t ioHandle,
                                       const char *keyValuePairs,
                                       int delayMs)
{
    mAudioCommandThread->parametersCommand(ioHandle, keyValuePairs,
                                           delayMs);
}

int AudioPolicyService::setStreamVolume(audio_stream_type_t stream,
                                        float volume,
                                        audio_io_handle_t output,
                                        int delayMs)
{
    return (int)mAudioCommandThread->volumeCommand(stream, volume,
                                                   output, delayMs);
}

int AudioPolicyService::startTone(audio_policy_tone_t tone,
                                  audio_stream_type_t stream)
{
    if (tone != AUDIO_POLICY_TONE_IN_CALL_NOTIFICATION) {
        ALOGE("startTone: illegal tone requested (%d)", tone);
    }
    if (stream != AUDIO_STREAM_VOICE_CALL) {
        ALOGE("startTone: illegal stream (%d) requested for tone %d", stream,
            tone);
    }
    mTonePlaybackThread->startToneCommand(ToneGenerator::TONE_SUP_CALL_WAITING,
                                          AUDIO_STREAM_VOICE_CALL);
    return 0;
}

int AudioPolicyService::stopTone()
{
    mTonePlaybackThread->stopToneCommand();
    return 0;
}

int AudioPolicyService::setVoiceVolume(float volume, int delayMs)
{
    return (int)mAudioCommandThread->voiceVolumeCommand(volume, delayMs);
}

// ----------------------------------------------------------------------------
// Audio pre-processing configuration
// ----------------------------------------------------------------------------

/*static*/ const char * const AudioPolicyService::kInputSourceNames[AUDIO_SOURCE_CNT -1] = {
    MIC_SRC_TAG,
    VOICE_UL_SRC_TAG,
    VOICE_DL_SRC_TAG,
    VOICE_CALL_SRC_TAG,
    CAMCORDER_SRC_TAG,
    VOICE_REC_SRC_TAG,
    VOICE_COMM_SRC_TAG
};

// returns the audio_source_t enum corresponding to the input source name or
// AUDIO_SOURCE_CNT is no match found
audio_source_t AudioPolicyService::inputSourceNameToEnum(const char *name)
{
    int i;
    for (i = AUDIO_SOURCE_MIC; i < AUDIO_SOURCE_CNT; i++) {
        if (strcmp(name, kInputSourceNames[i - AUDIO_SOURCE_MIC]) == 0) {
            ALOGV("inputSourceNameToEnum found source %s %d", name, i);
            break;
        }
    }
    return (audio_source_t)i;
}

size_t AudioPolicyService::growParamSize(char *param,
                                         size_t size,
                                         size_t *curSize,
                                         size_t *totSize)
{
    // *curSize is at least sizeof(effect_param_t) + 2 * sizeof(int)
    size_t pos = ((*curSize - 1 ) / size + 1) * size;

    if (pos + size > *totSize) {
        while (pos + size > *totSize) {
            *totSize += ((*totSize + 7) / 8) * 4;
        }
        param = (char *)realloc(param, *totSize);
    }
    *curSize = pos + size;
    return pos;
}

size_t AudioPolicyService::readParamValue(cnode *node,
                                          char *param,
                                          size_t *curSize,
                                          size_t *totSize)
{
    if (strncmp(node->name, SHORT_TAG, sizeof(SHORT_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(short), curSize, totSize);
        *(short *)((char *)param + pos) = (short)atoi(node->value);
        ALOGV("readParamValue() reading short %d", *(short *)((char *)param + pos));
        return sizeof(short);
    } else if (strncmp(node->name, INT_TAG, sizeof(INT_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(int), curSize, totSize);
        *(int *)((char *)param + pos) = atoi(node->value);
        ALOGV("readParamValue() reading int %d", *(int *)((char *)param + pos));
        return sizeof(int);
    } else if (strncmp(node->name, FLOAT_TAG, sizeof(FLOAT_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(float), curSize, totSize);
        *(float *)((char *)param + pos) = (float)atof(node->value);
        ALOGV("readParamValue() reading float %f",*(float *)((char *)param + pos));
        return sizeof(float);
    } else if (strncmp(node->name, BOOL_TAG, sizeof(BOOL_TAG) + 1) == 0) {
        size_t pos = growParamSize(param, sizeof(bool), curSize, totSize);
        if (strncmp(node->value, "false", strlen("false") + 1) == 0) {
            *(bool *)((char *)param + pos) = false;
        } else {
            *(bool *)((char *)param + pos) = true;
        }
        ALOGV("readParamValue() reading bool %s",*(bool *)((char *)param + pos) ? "true" : "false");
        return sizeof(bool);
    } else if (strncmp(node->name, STRING_TAG, sizeof(STRING_TAG) + 1) == 0) {
        size_t len = strnlen(node->value, EFFECT_STRING_LEN_MAX);
        if (*curSize + len + 1 > *totSize) {
            *totSize = *curSize + len + 1;
            param = (char *)realloc(param, *totSize);
        }
        strncpy(param + *curSize, node->value, len);
        *curSize += len;
        param[*curSize] = '\0';
        ALOGV("readParamValue() reading string %s", param + *curSize - len);
        return len;
    }
    ALOGW("readParamValue() unknown param type %s", node->name);
    return 0;
}

effect_param_t *AudioPolicyService::loadEffectParameter(cnode *root)
{
    cnode *param;
    cnode *value;
    size_t curSize = sizeof(effect_param_t);
    size_t totSize = sizeof(effect_param_t) + 2 * sizeof(int);
    effect_param_t *fx_param = (effect_param_t *)malloc(totSize);

    param = config_find(root, PARAM_TAG);
    value = config_find(root, VALUE_TAG);
    if (param == NULL && value == NULL) {
        // try to parse simple parameter form {int int}
        param = root->first_child;
        if (param != NULL) {
            // Note: that a pair of random strings is read as 0 0
            int *ptr = (int *)fx_param->data;
            int *ptr2 = (int *)((char *)param + sizeof(effect_param_t));
            ALOGW("loadEffectParameter() ptr %p ptr2 %p", ptr, ptr2);
            *ptr++ = atoi(param->name);
            *ptr = atoi(param->value);
            fx_param->psize = sizeof(int);
            fx_param->vsize = sizeof(int);
            return fx_param;
        }
    }
    if (param == NULL || value == NULL) {
        ALOGW("loadEffectParameter() invalid parameter description %s", root->name);
        goto error;
    }

    fx_param->psize = 0;
    param = param->first_child;
    while (param) {
        ALOGV("loadEffectParameter() reading param of type %s", param->name);
        size_t size = readParamValue(param, (char *)fx_param, &curSize, &totSize);
        if (size == 0) {
            goto error;
        }
        fx_param->psize += size;
        param = param->next;
    }

    // align start of value field on 32 bit boundary
    curSize = ((curSize - 1 ) / sizeof(int) + 1) * sizeof(int);

    fx_param->vsize = 0;
    value = value->first_child;
    while (value) {
        ALOGV("loadEffectParameter() reading value of type %s", value->name);
        size_t size = readParamValue(value, (char *)fx_param, &curSize, &totSize);
        if (size == 0) {
            goto error;
        }
        fx_param->vsize += size;
        value = value->next;
    }

    return fx_param;

error:
    free(fx_param);
    return NULL;
}

void AudioPolicyService::loadEffectParameters(cnode *root, Vector <effect_param_t *>& params)
{
    cnode *node = root->first_child;
    while (node) {
        ALOGV("loadEffectParameters() loading param %s", node->name);
        effect_param_t *param = loadEffectParameter(node);
        if (param == NULL) {
            node = node->next;
            continue;
        }
        params.add(param);
        node = node->next;
    }
}

AudioPolicyService::InputSourceDesc *AudioPolicyService::loadInputSource(
                                                            cnode *root,
                                                            const Vector <EffectDesc *>& effects)
{
    cnode *node = root->first_child;
    if (node == NULL) {
        ALOGW("loadInputSource() empty element %s", root->name);
        return NULL;
    }
    InputSourceDesc *source = new InputSourceDesc();
    while (node) {
        size_t i;
        for (i = 0; i < effects.size(); i++) {
            if (strncmp(effects[i]->mName, node->name, EFFECT_STRING_LEN_MAX) == 0) {
                ALOGV("loadInputSource() found effect %s in list", node->name);
                break;
            }
        }
        if (i == effects.size()) {
            ALOGV("loadInputSource() effect %s not in list", node->name);
            node = node->next;
            continue;
        }
        EffectDesc *effect = new EffectDesc(*effects[i]);   // deep copy
        loadEffectParameters(node, effect->mParams);
        ALOGV("loadInputSource() adding effect %s uuid %08x", effect->mName, effect->mUuid.timeLow);
        source->mEffects.add(effect);
        node = node->next;
    }
    if (source->mEffects.size() == 0) {
        ALOGW("loadInputSource() no valid effects found in source %s", root->name);
        delete source;
        return NULL;
    }
    return source;
}

status_t AudioPolicyService::loadInputSources(cnode *root, const Vector <EffectDesc *>& effects)
{
    cnode *node = config_find(root, PREPROCESSING_TAG);
    if (node == NULL) {
        return -ENOENT;
    }
    node = node->first_child;
    while (node) {
        audio_source_t source = inputSourceNameToEnum(node->name);
        if (source == AUDIO_SOURCE_CNT) {
            ALOGW("loadInputSources() invalid input source %s", node->name);
            node = node->next;
            continue;
        }
        ALOGV("loadInputSources() loading input source %s", node->name);
        InputSourceDesc *desc = loadInputSource(node, effects);
        if (desc == NULL) {
            node = node->next;
            continue;
        }
        mInputSources.add(source, desc);
        node = node->next;
    }
    return NO_ERROR;
}

AudioPolicyService::EffectDesc *AudioPolicyService::loadEffect(cnode *root)
{
    cnode *node = config_find(root, UUID_TAG);
    if (node == NULL) {
        return NULL;
    }
    effect_uuid_t uuid;
    if (AudioEffect::stringToGuid(node->value, &uuid) != NO_ERROR) {
        ALOGW("loadEffect() invalid uuid %s", node->value);
        return NULL;
    }
    return new EffectDesc(root->name, uuid);
}

status_t AudioPolicyService::loadEffects(cnode *root, Vector <EffectDesc *>& effects)
{
    cnode *node = config_find(root, EFFECTS_TAG);
    if (node == NULL) {
        return -ENOENT;
    }
    node = node->first_child;
    while (node) {
        ALOGV("loadEffects() loading effect %s", node->name);
        EffectDesc *effect = loadEffect(node);
        if (effect == NULL) {
            node = node->next;
            continue;
        }
        effects.add(effect);
        node = node->next;
    }
    return NO_ERROR;
}

status_t AudioPolicyService::loadPreProcessorConfig(const char *path)
{
    cnode *root;
    char *data;

    data = (char *)load_file(path, NULL);
    if (data == NULL) {
        return -ENODEV;
    }
    root = config_node("", "");
    config_load(root, data);

    Vector <EffectDesc *> effects;
    loadEffects(root, effects);
    loadInputSources(root, effects);

    // delete effects to fix memory leak.
    // as effects is local var and valgrind would treat this as memory leak
    // and although it only did in mediaserver init, but free it in case mediaserver reboot
    size_t i;
    for (i = 0; i < effects.size(); i++) {
      delete effects[i];
    }

    config_free(root);
    free(root);
    free(data);

    return NO_ERROR;
}

extern "C" {
audio_module_handle_t aps_load_hw_module(void *service __unused,
                                             const char *name);
audio_io_handle_t aps_open_output(void *service __unused,
                                         audio_devices_t *pDevices,
                                         uint32_t *pSamplingRate,
                                         audio_format_t *pFormat,
                                         audio_channel_mask_t *pChannelMask,
                                         uint32_t *pLatencyMs,
                                         audio_output_flags_t flags);

audio_io_handle_t aps_open_output_on_module(void *service __unused,
                                                   audio_module_handle_t module,
                                                   audio_devices_t *pDevices,
                                                   uint32_t *pSamplingRate,
                                                   audio_format_t *pFormat,
                                                   audio_channel_mask_t *pChannelMask,
                                                   uint32_t *pLatencyMs,
                                                   audio_output_flags_t flags,
                                                   const audio_offload_info_t *offloadInfo);
audio_io_handle_t aps_open_dup_output(void *service __unused,
                                                 audio_io_handle_t output1,
                                                 audio_io_handle_t output2);
int aps_close_output(void *service __unused, audio_io_handle_t output);
int aps_suspend_output(void *service __unused, audio_io_handle_t output);
int aps_restore_output(void *service __unused, audio_io_handle_t output);
audio_io_handle_t aps_open_input(void *service __unused,
                                        audio_devices_t *pDevices,
                                        uint32_t *pSamplingRate,
                                        audio_format_t *pFormat,
                                        audio_channel_mask_t *pChannelMask,
                                        audio_in_acoustics_t acoustics __unused);
audio_io_handle_t aps_open_input_on_module(void *service __unused,
                                                  audio_module_handle_t module,
                                                  audio_devices_t *pDevices,
                                                  uint32_t *pSamplingRate,
                                                  audio_format_t *pFormat,
                                                  audio_channel_mask_t *pChannelMask);
int aps_close_input(void *service __unused, audio_io_handle_t input);
int aps_invalidate_stream(void *service __unused, audio_stream_type_t stream);
int aps_move_effects(void *service __unused, int session,
                                audio_io_handle_t src_output,
                                audio_io_handle_t dst_output);
char * aps_get_parameters(void *service __unused, audio_io_handle_t io_handle,
                                     const char *keys);
void aps_set_parameters(void *service, audio_io_handle_t io_handle,
                                   const char *kv_pairs, int delay_ms);
int aps_set_stream_volume(void *service, audio_stream_type_t stream,
                                     float volume, audio_io_handle_t output,
                                     int delay_ms);
int aps_start_tone(void *service, audio_policy_tone_t tone,
                              audio_stream_type_t stream);
int aps_stop_tone(void *service);
int aps_set_voice_volume(void *service, float volume, int delay_ms);
};

namespace {
    struct audio_policy_service_ops aps_ops = {
        .open_output           = aps_open_output,
        .open_duplicate_output = aps_open_dup_output,
        .close_output          = aps_close_output,
        .suspend_output        = aps_suspend_output,
        .restore_output        = aps_restore_output,
        .open_input            = aps_open_input,
        .close_input           = aps_close_input,
        .set_stream_volume     = aps_set_stream_volume,
        .invalidate_stream     = aps_invalidate_stream,
        .set_parameters        = aps_set_parameters,
        .get_parameters        = aps_get_parameters,
        .start_tone            = aps_start_tone,
        .stop_tone             = aps_stop_tone,
        .set_voice_volume      = aps_set_voice_volume,
        .move_effects          = aps_move_effects,
        .load_hw_module        = aps_load_hw_module,
        .open_output_on_module = aps_open_output_on_module,
        .open_input_on_module  = aps_open_input_on_module,
    };
}; // namespace <unnamed>

}; // namespace android
