/*
 * Copyright (C) 2014 The Android Open Source Project
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

/*
 * This file defines an NDK API.
 * Do not remove methods.
 * Do not change method signatures.
 * Do not change the value of constants.
 * Do not change the size of any of the classes defined in here.
 * Do not reference types that are not part of the NDK.
 * Do not #include files that aren't part of the NDK.
 */

#ifndef _NDK_MEDIA_CODEC_H
#define _NDK_MEDIA_CODEC_H

#include <android/native_window.h>

#include "NdkMediaFormat.h"

#ifdef __cplusplus
extern "C" {
#endif


struct AMediaCodec;
typedef struct AMediaCodec AMediaCodec;

struct AMediaCodecBufferInfo {
    int32_t offset;
    int32_t size;
    int64_t presentationTimeUs;
    uint32_t flags;
};
typedef struct AMediaCodecBufferInfo AMediaCodecBufferInfo;

enum {
    AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM = 4,
    AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED = -3,
    AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED = -2,
    AMEDIACODEC_INFO_TRY_AGAIN_LATER = -1
};


/**
 * Create decoder by name. Use this if you know the exact codec you want to use.
 */
AMediaCodec* AMediaCodec_createByCodecName(const char *name);

/**
 * Create codec by mime type. Most applications will use this, specifying a
 * mime type obtained from media extractor.
 */
AMediaCodec* AMediaCodec_createByCodecType(const char *mime_type);

/**
 * Create encoder by name.
 */
AMediaCodec* AMediaCodec_createEncoderByName(const char *name);

/**
 * delete the codec and free its resources
 */
int AMediaCodec_delete(AMediaCodec*);

/**
 * Configure the codec. For decoding you would typically get the format from an extractor.
 */
int AMediaCodec_configure(AMediaCodec*, const AMediaFormat* format, ANativeWindow* surface); // TODO: other args

/**
 * Start the codec. A codec must be configured before it can be started, and must be started
 * before buffers can be sent to it.
 */
int AMediaCodec_start(AMediaCodec*);

/**
 * Stop the codec.
 */
int AMediaCodec_stop(AMediaCodec*);

/*
 * Flush the codec's input and output. All indices previously returned from calls to
 * AMediaCodec_dequeueInputBuffer and AMediaCodec_dequeueOutputBuffer become invalid.
 */
int AMediaCodec_flush(AMediaCodec*);

/**
 * Get an input buffer. The specified buffer index must have been previously obtained from
 * dequeueInputBuffer, and not yet queued.
 */
uint8_t* AMediaCodec_getInputBuffer(AMediaCodec*, size_t idx, size_t *out_size);

/**
 * Get an output buffer. The specified buffer index must have been previously obtained from
 * dequeueOutputBuffer, and not yet queued.
 */
uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec*, size_t idx, size_t *out_size);

/**
 * Get the index of the next available input buffer. An app will typically use this with
 * getInputBuffer() to get a pointer to the buffer, then copy the data to be encoded or decoded
 * into the buffer before passing it to the codec.
 */
ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec*, int64_t timeoutUs);

/**
 * Send the specified buffer to the codec for processing.
 */
int AMediaCodec_queueInputBuffer(AMediaCodec*,
        size_t idx, off_t offset, size_t size, uint64_t time, uint32_t flags);

/**
 * Get the index of the next available buffer of processed data.
 */
ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec*, AMediaCodecBufferInfo *info, int64_t timeoutUs);
AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec*);

/**
 * Release and optionally render the specified buffer.
 */
int AMediaCodec_releaseOutputBuffer(AMediaCodec*, size_t idx, bool render);



#ifdef __cplusplus
} // extern "C"
#endif

#endif //_NDK_MEDIA_CODEC_H
