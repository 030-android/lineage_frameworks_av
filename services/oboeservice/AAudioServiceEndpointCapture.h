/*
 * Copyright (C) 2017 The Android Open Source Project
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

#ifndef AAUDIO_SERVICE_ENDPOINT_CAPTURE_H
#define AAUDIO_SERVICE_ENDPOINT_CAPTURE_H

#include <memory>

#include "client/AudioStreamInternal.h"
#include "client/AudioStreamInternalCapture.h"

#include "AAudioServiceEndpointShared.h"
#include "AAudioServiceStreamShared.h"

namespace aaudio {

class AAudioServiceEndpointCapture : public AAudioServiceEndpointShared {
public:
    explicit AAudioServiceEndpointCapture(android::AAudioService &audioService);
    ~AAudioServiceEndpointCapture() override = default;

    aaudio_result_t open(const aaudio::AAudioStreamRequest &request) override;

    void *callbackLoop() override;

private:
    std::unique_ptr<uint8_t[]>  mDistributionBuffer;
};

} /* namespace aaudio */

#endif //AAUDIO_SERVICE_ENDPOINT_CAPTURE_H
