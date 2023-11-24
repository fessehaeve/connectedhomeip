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
 */

#include "ValveControlDelegate.h"
#include <lib/support/logging/CHIPLogging.h>

using namespace chip;
using namespace chip::app;
using namespace chip::app::Clusters::ValveConfigurationAndControl;

CHIP_ERROR ValveControlDelegate::HandleOpenValve(DataModel::Nullable<chip::Percent> level)
{
    chip::Percent lvl = level.IsNull() ? 100 : level.Value();
    ChipLogProgress(NotSpecified, "Valve opened to level: %d", lvl);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ValveControlDelegate::HandleCloseValve()
{
    ChipLogProgress(NotSpecified, "Valve closed");
    return CHIP_NO_ERROR;
}