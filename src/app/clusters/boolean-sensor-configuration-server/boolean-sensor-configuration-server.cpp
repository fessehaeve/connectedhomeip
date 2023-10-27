/**
 *
 *    Copyright (c) 2023 Project CHIP Authors
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
 *
 */

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/EventLogging.h>
#include <app/util/af-enums.h>
#include <app/util/basic-types.h>
#include <lib/core/CHIPError.h>
#include <lib/support/logging/CHIPLogging.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters;

/**
 * @brief Boolean Sensor Configuration Cluster SupressRequest Command callback (from client)
 */
bool emberAfBooleanSensorConfigurationClusterSupressRequestCallback(
    CommandHandler * commandObj, const ConcreteCommandPath & commandPath,
    const BooleanSensorConfiguration::Commands::SupressRequest::DecodableType & commandData)
{
    return true;
}

void MatterBooleanSensorConfigurationPluginServerInitCallback() {}
