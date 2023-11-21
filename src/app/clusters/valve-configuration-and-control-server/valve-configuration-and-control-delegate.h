/*
 *
 *    Copyright (c) 2023 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <app-common/zap-generated/cluster-enums.h>
#include <lib/core/CHIPError.h>

namespace chip {
namespace app {
namespace Clusters {
namespace ValveConfigurationAndControl {

/** @brief
 *    Defines methods for implementing application-specific logic for the Valve Configuration and Control Cluster.
 */
class Delegate
{
public:
    Delegate(){};

    virtual CHIP_ERROR HandleOpenValve() = 0;

    virtual ~Delegate() = default;
};

} // namespace ValveConfigurationAndControl
} // namespace Clusters
} // namespace app
} // namespace chip
