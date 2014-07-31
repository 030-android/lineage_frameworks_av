/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "GenericSource.h"

#include "AnotherPacketSource.h"

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/FileSource.h>
#include <media/stagefright/MediaBuffer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaExtractor.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include "../../libstagefright/include/WVMExtractor.h"

namespace android {

NuPlayer::GenericSource::GenericSource(
        const sp<AMessage> &notify,
        const sp<IMediaHTTPService> &httpService,
        const char *url,
        const KeyedVector<String8, String8> *headers,
        bool isWidevine,
        bool uidValid,
        uid_t uid)
    : Source(notify),
      mDurationUs(0ll),
      mAudioIsVorbis(false),
      mIsWidevine(isWidevine),
      mUIDValid(uidValid),
      mUID(uid) {
    DataSource::RegisterDefaultSniffers();

    sp<DataSource> dataSource =
        DataSource::CreateFromURI(httpService, url, headers);
    CHECK(dataSource != NULL);

    initFromDataSource(dataSource);
}

NuPlayer::GenericSource::GenericSource(
        const sp<AMessage> &notify,
        int fd, int64_t offset, int64_t length)
    : Source(notify),
      mDurationUs(0ll),
      mAudioIsVorbis(false),
      mIsWidevine(false) {
    DataSource::RegisterDefaultSniffers();

    sp<DataSource> dataSource = new FileSource(dup(fd), offset, length);

    initFromDataSource(dataSource);
}

void NuPlayer::GenericSource::initFromDataSource(
        const sp<DataSource> &dataSource) {
    sp<MediaExtractor> extractor;

    if (mIsWidevine) {
        String8 mimeType;
        float confidence;
        sp<AMessage> dummy;
        bool success;

        success = SniffWVM(dataSource, &mimeType, &confidence, &dummy);
        if (!success
                || strcasecmp(
                    mimeType.string(), MEDIA_MIMETYPE_CONTAINER_WVM)) {
            ALOGE("unsupported widevine mime: %s", mimeType.string());
            return;
        }

        sp<WVMExtractor> wvmExtractor = new WVMExtractor(dataSource);
        wvmExtractor->setAdaptiveStreamingMode(true);
        if (mUIDValid) {
            wvmExtractor->setUID(mUID);
        }
        extractor = wvmExtractor;
    } else {
        extractor = MediaExtractor::Create(dataSource);
    }

    CHECK(extractor != NULL);

    sp<MetaData> fileMeta = extractor->getMetaData();
    if (fileMeta != NULL) {
        int64_t duration;
        if (fileMeta->findInt64(kKeyDuration, &duration)) {
            mDurationUs = duration;
        }
    }

    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        sp<MediaSource> track = extractor->getTrack(i);

        if (!strncasecmp(mime, "audio/", 6)) {
            if (mAudioTrack.mSource == NULL) {
                mAudioTrack.mIndex = i;
                mAudioTrack.mSource = track;

                if (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_VORBIS)) {
                    mAudioIsVorbis = true;
                } else {
                    mAudioIsVorbis = false;
                }
            }
        } else if (!strncasecmp(mime, "video/", 6)) {
            if (mVideoTrack.mSource == NULL) {
                mVideoTrack.mIndex = i;
                mVideoTrack.mSource = track;
            }
        }

        if (track != NULL) {
            mSources.push(track);
            int64_t durationUs;
            if (meta->findInt64(kKeyDuration, &durationUs)) {
                if (durationUs > mDurationUs) {
                    mDurationUs = durationUs;
                }
            }
        }
    }
}

status_t NuPlayer::GenericSource::setBuffers(bool audio, Vector<MediaBuffer *> &buffers) {
    if (mIsWidevine && !audio) {
        return mVideoTrack.mSource->setBuffers(buffers);
    }
    return INVALID_OPERATION;
}

NuPlayer::GenericSource::~GenericSource() {
}

void NuPlayer::GenericSource::prepareAsync() {
    if (mVideoTrack.mSource != NULL) {
        sp<MetaData> meta = mVideoTrack.mSource->getFormat();

        int32_t width, height;
        CHECK(meta->findInt32(kKeyWidth, &width));
        CHECK(meta->findInt32(kKeyHeight, &height));

        notifyVideoSizeChanged(width, height);
    }

    notifyFlagsChanged(
            (mIsWidevine ? FLAG_SECURE : 0)
            | FLAG_CAN_PAUSE
            | FLAG_CAN_SEEK_BACKWARD
            | FLAG_CAN_SEEK_FORWARD
            | FLAG_CAN_SEEK);

    notifyPrepared();
}

void NuPlayer::GenericSource::start() {
    ALOGI("start");

    if (mAudioTrack.mSource != NULL) {
        CHECK_EQ(mAudioTrack.mSource->start(), (status_t)OK);

        mAudioTrack.mPackets =
            new AnotherPacketSource(mAudioTrack.mSource->getFormat());

        readBuffer(true /* audio */);
    }

    if (mVideoTrack.mSource != NULL) {
        CHECK_EQ(mVideoTrack.mSource->start(), (status_t)OK);

        mVideoTrack.mPackets =
            new AnotherPacketSource(mVideoTrack.mSource->getFormat());

        readBuffer(false /* audio */);
    }
}

status_t NuPlayer::GenericSource::feedMoreTSData() {
    return OK;
}

sp<MetaData> NuPlayer::GenericSource::getFormatMeta(bool audio) {
    sp<MediaSource> source = audio ? mAudioTrack.mSource : mVideoTrack.mSource;

    if (source == NULL) {
        return NULL;
    }

    return source->getFormat();
}

status_t NuPlayer::GenericSource::dequeueAccessUnit(
        bool audio, sp<ABuffer> *accessUnit) {
    Track *track = audio ? &mAudioTrack : &mVideoTrack;

    if (track->mSource == NULL) {
        return -EWOULDBLOCK;
    }

    if (mIsWidevine && !audio) {
        // try to read a buffer as we may not have been able to the last time
        readBuffer(audio, -1ll);
    }

    status_t finalResult;
    if (!track->mPackets->hasBufferAvailable(&finalResult)) {
        return (finalResult == OK ? -EWOULDBLOCK : finalResult);
    }

    status_t result = track->mPackets->dequeueAccessUnit(accessUnit);

    readBuffer(audio, -1ll);

    return result;
}

status_t NuPlayer::GenericSource::getDuration(int64_t *durationUs) {
    *durationUs = mDurationUs;
    return OK;
}

size_t NuPlayer::GenericSource::getTrackCount() const {
    return mSources.size();
}

sp<AMessage> NuPlayer::GenericSource::getTrackInfo(size_t trackIndex) const {
    size_t trackCount = mSources.size();
    if (trackIndex >= trackCount) {
        return NULL;
    }

    sp<AMessage> format = new AMessage();
    sp<MetaData> meta = mSources.itemAt(trackIndex)->getFormat();

    const char *mime;
    CHECK(meta->findCString(kKeyMIMEType, &mime));

    int32_t trackType;
    if (!strncasecmp(mime, "video/", 6)) {
        trackType = MEDIA_TRACK_TYPE_VIDEO;
    } else if (!strncasecmp(mime, "audio/", 6)) {
        trackType = MEDIA_TRACK_TYPE_AUDIO;
    } else if (!strcasecmp(mime, MEDIA_MIMETYPE_TEXT_3GPP)) {
        trackType = MEDIA_TRACK_TYPE_TIMEDTEXT;
    } else {
        trackType = MEDIA_TRACK_TYPE_UNKNOWN;
    }
    format->setInt32("type", trackType);

    const char *lang;
    if (!meta->findCString(kKeyMediaLanguage, &lang)) {
        lang = "und";
    }
    format->setString("language", lang);

    if (trackType == MEDIA_TRACK_TYPE_SUBTITLE) {
        format->setString("mime", mime);

        int32_t isAutoselect = 1, isDefault = 0, isForced = 0;
        meta->findInt32(kKeyTrackIsAutoselect, &isAutoselect);
        meta->findInt32(kKeyTrackIsDefault, &isDefault);
        meta->findInt32(kKeyTrackIsForced, &isForced);

        format->setInt32("auto", !!isAutoselect);
        format->setInt32("default", !!isDefault);
        format->setInt32("forced", !!isForced);
    }

    return format;
}

status_t NuPlayer::GenericSource::seekTo(int64_t seekTimeUs) {
    if (mVideoTrack.mSource != NULL) {
        int64_t actualTimeUs;
        readBuffer(false /* audio */, seekTimeUs, &actualTimeUs);

        seekTimeUs = actualTimeUs;
    }

    if (mAudioTrack.mSource != NULL) {
        readBuffer(true /* audio */, seekTimeUs);
    }

    return OK;
}

void NuPlayer::GenericSource::readBuffer(
        bool audio, int64_t seekTimeUs, int64_t *actualTimeUs) {
    Track *track = audio ? &mAudioTrack : &mVideoTrack;
    CHECK(track->mSource != NULL);

    if (actualTimeUs) {
        *actualTimeUs = seekTimeUs;
    }

    MediaSource::ReadOptions options;

    bool seeking = false;

    if (seekTimeUs >= 0) {
        options.setSeekTo(seekTimeUs);
        seeking = true;
    }

    if (mIsWidevine && !audio) {
        options.setNonBlocking();
    }

    for (;;) {
        MediaBuffer *mbuf;
        status_t err = track->mSource->read(&mbuf, &options);

        options.clearSeekTo();

        if (err == OK) {
            size_t outLength = mbuf->range_length();

            if (audio && mAudioIsVorbis) {
                outLength += sizeof(int32_t);
            }

            sp<ABuffer> buffer;
            if (mIsWidevine && !audio) {
                // data is already provided in the buffer
                buffer = new ABuffer(NULL, mbuf->range_length());
                buffer->meta()->setPointer("mediaBuffer", mbuf);
                mbuf->add_ref();
            } else {
                buffer = new ABuffer(outLength);
                memcpy(buffer->data(),
                       (const uint8_t *)mbuf->data() + mbuf->range_offset(),
                       mbuf->range_length());
            }

            if (audio && mAudioIsVorbis) {
                int32_t numPageSamples;
                if (!mbuf->meta_data()->findInt32(
                            kKeyValidSamples, &numPageSamples)) {
                    numPageSamples = -1;
                }

                memcpy(buffer->data() + mbuf->range_length(),
                       &numPageSamples,
                       sizeof(numPageSamples));
            }

            int64_t timeUs;
            CHECK(mbuf->meta_data()->findInt64(kKeyTime, &timeUs));

            buffer->meta()->setInt64("timeUs", timeUs);

            if (actualTimeUs) {
                *actualTimeUs = timeUs;
            }

            mbuf->release();
            mbuf = NULL;

            if (seeking) {
                track->mPackets->queueDiscontinuity(
                        ATSParser::DISCONTINUITY_SEEK,
                        NULL,
                        true /* discard */);
            }

            track->mPackets->queueAccessUnit(buffer);
            break;
        } else if (err == WOULD_BLOCK) {
            break;
        } else if (err == INFO_FORMAT_CHANGED) {
#if 0
            track->mPackets->queueDiscontinuity(
                    ATSParser::DISCONTINUITY_FORMATCHANGE,
                    NULL,
                    false /* discard */);
#endif
        } else {
            track->mPackets->signalEOS(err);
            break;
        }
    }
}

}  // namespace android
