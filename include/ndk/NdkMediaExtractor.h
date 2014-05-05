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

#ifndef _NDK_MEDIA_EXTRACTOR_H
#define _NDK_MEDIA_EXTRACTOR_H

#include <sys/types.h>

#include "NdkMediaFormat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct AMediaExtractor;
typedef struct AMediaExtractor AMediaExtractor;


/**
 * Create new media extractor
 */
AMediaExtractor* AMediaExtractor_new();

/**
 * Delete a previously created media extractor
 */
int AMediaExtractor_delete(AMediaExtractor*);

/**
 *  Set the file descriptor from which the extractor will read.
 */
int AMediaExtractor_setDataSourceFd(AMediaExtractor*, int fd, off64_t offset, off64_t length);

/**
 * Set the URI from which the extractor will read.
 */
int AMediaExtractor_setDataSource(AMediaExtractor*, const char *location); // TODO support headers

/**
 * Return the number of tracks in the previously specified media file
 */
int AMediaExtractor_getTrackCount(AMediaExtractor*);

/**
 * Return the format of the specified track. The caller must free the returned format
 */
AMediaFormat* AMediaExtractor_getTrackFormat(AMediaExtractor*, size_t idx);

/**
 * Select the specified track. Subsequent calls to readSampleData, getSampleTrackIndex and
 * getSampleTime only retrieve information for the subset of tracks selected.
 * Selecting the same track multiple times has no effect, the track is
 * only selected once.
 */
int AMediaExtractor_selectTrack(AMediaExtractor*, size_t idx);

/**
 * Unselect the specified track. Subsequent calls to readSampleData, getSampleTrackIndex and
 * getSampleTime only retrieve information for the subset of tracks selected..
 */
int AMediaExtractor_unselectTrack(AMediaExtractor*, size_t idx);

/**
 * Read the current sample.
 */
int AMediaExtractor_readSampleData(AMediaExtractor*, uint8_t *buffer, size_t capacity);

/**
 * Read the current sample's flags.
 */
int AMediaExtractor_getSampleFlags(AMediaExtractor*); // see definitions below

/**
 * Returns the track index the current sample originates from (or -1
 * if no more samples are available)
 */
int AMediaExtractor_getSampleTrackIndex(AMediaExtractor*);

/**
 * Returns the current sample's presentation time in microseconds.
 * or -1 if no more samples are available.
 */
int64_t AMediaExtractor_getSampletime(AMediaExtractor*);

/**
 * Advance to the next sample. Returns false if no more sample data
 * is available (end of stream).
 */
bool AMediaExtractor_advance(AMediaExtractor*);

enum {
    AMEDIAEXTRACTOR_SAMPLE_FLAG_SYNC = 1,
    AMEDIAEXTRACTOR_SAMPLE_FLAG_ENCRYPTED = 2,
};


#ifdef __cplusplus
} // extern "C"
#endif

#endif // _NDK_MEDIA_EXTRACTOR_H
