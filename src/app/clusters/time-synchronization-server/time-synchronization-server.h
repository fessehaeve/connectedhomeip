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

/**
 * @file API declarations for time sync cluster.
 */

#pragma once

#include "TimeSyncDataProvider.h"
#include "time-synchronization-delegate.h"

#include <app/AppConfig.h>
#include <app/server/Server.h>
#include <app/util/af-types.h>
#include <credentials/FabricTable.h>
#include <lib/core/TLV.h>

#include <app-common/zap-generated/cluster-objects.h>
#include <lib/support/Span.h>

// NOTE: this is part of AppConfig, so this has to be checked for AFTER the inclusion
//       of that header
#if TIME_SYNC_ENABLE_TSC_FEATURE
#include <app/ReadClient.h>
#endif

namespace chip {
namespace app {
namespace Clusters {
namespace TimeSynchronization {

/**
 * @brief Describes the state of time zone and DSTOffset in use.
 */
enum class TimeState : uint8_t
{
    kInvalid = 0, // No valid offset available
    kActive  = 1, // An offset is currently being used
    kStopped = 2, // Permanent item in use
};

void SetDefaultDelegate(Delegate * delegate);
Delegate * GetDefaultDelegate();

class TimeSynchronizationServer : public FabricTable::Delegate
#if TIME_SYNC_ENABLE_TSC_FEATURE
    ,
                                  public ReadClient::Callback
#endif
{
public:
    TimeSynchronizationServer();
    void Init(PersistentStorageDelegate & persistentStorage);
    void Shutdown();

    static TimeSynchronizationServer & Instance(void);
    TimeSyncDataProvider & GetDataProvider(void) { return mTimeSyncDataProvider; }

    CHIP_ERROR SetTrustedTimeSource(const DataModel::Nullable<Structs::TrustedTimeSourceStruct::Type> & tts);
    CHIP_ERROR SetDefaultNTP(const DataModel::Nullable<chip::CharSpan> & dntp);
    void InitTimeZone(void);
    CHIP_ERROR SetTimeZone(const DataModel::DecodableList<Structs::TimeZoneStruct::Type> & tzL);
    CHIP_ERROR LoadTimeZone(void);
    CHIP_ERROR ClearTimeZone(void);
    void InitDSTOffset(void);
    CHIP_ERROR SetDSTOffset(const DataModel::DecodableList<Structs::DSTOffsetStruct::Type> & dstL);
    CHIP_ERROR LoadDSTOffset(void);
    CHIP_ERROR ClearDSTOffset(void);
    DataModel::Nullable<Structs::TrustedTimeSourceStruct::Type> & GetTrustedTimeSource(void);
    Span<TimeSyncDataProvider::TimeZoneStore> & GetTimeZone(void);
    DataModel::List<Structs::DSTOffsetStruct::Type> & GetDSTOffset(void);
    CHIP_ERROR GetDefaultNtp(MutableCharSpan & dntp);

    CHIP_ERROR SetUTCTime(chip::EndpointId ep, uint64_t utcTime, GranularityEnum granularity, TimeSourceEnum source);
    CHIP_ERROR GetLocalTime(chip::EndpointId ep, DataModel::Nullable<uint64_t> & localTime);
    GranularityEnum & GetGranularity() { return mGranularity; }
    TimeSourceEnum & GetTimeSource() { return mTimeSource; }

    void ScheduleDelayedAction(System::Clock::Seconds32 delay, System::TimerCompleteCallback action, void * aAppState);

    /**
     * @brief Updates time zone to the correct item from the time zone list.
     * If the TimeState has changed from the previous state, the function will emit a TimeZoneStatus event.
     *
     * Returns kInvalid if an error occurrs.
     * Otherwise, it returns kActive since there is always a default time zone available.
     * kStopped is not used in this function.
     */
    TimeState UpdateTimeZoneState();
    /**
     * @brief Updates DST offset to the correct item from the DSTOffset list.
     * If the DST offset has changed from the previous state or value, the function will emit a DSTStatus event.
     *
     * Returns kInvalid if an error occurrs or the DSTOffset list is empty.
     * Returns kActive if a DST offset is being used.
     * Returns kStopped if no DST offset is applicable at the current time.
     */
    TimeState UpdateDSTOffsetState();

    // Fabric Table delegate functions
    void OnFabricRemoved(const FabricTable & fabricTable, FabricIndex fabricIndex) override;

#if TIME_SYNC_ENABLE_TSC_FEATURE
    // CASE connection functions
    void OnDeviceConnectedFn(Messaging::ExchangeManager & exchangeMgr, const SessionHandle & sessionHandle);
    void OnDeviceConnectionFailureFn();

    // ReadClient::Callback functions
    void OnAttributeData(const ConcreteDataAttributePath & aPath, TLV::TLVReader * apData, const StatusIB & aStatus) override;
    void OnDone(ReadClient * apReadClient) override;

    CHIP_ERROR AttemptToGetTimeFromTrustedNode();
#endif

    // Platform event handler functions
    void OnPlatformEventFn(const DeviceLayer::ChipDeviceEvent & event);

    void OnTimeSyncCompletionFn(TimeSourceEnum timeSource, GranularityEnum granularity);
    void OnFallbackNTPCompletionFn(bool timeSyncSuccessful);

private:
    static constexpr size_t kMaxDefaultNTPSize = 128;
    DataModel::Nullable<Structs::TrustedTimeSourceStruct::Type> mTrustedTimeSource;
    TimeSyncDataProvider::TimeZoneObj mTimeZoneObj{ Span<TimeSyncDataProvider::TimeZoneStore>(mTz), 0 };
    TimeSyncDataProvider::DSTOffsetObj mDstOffsetObj{ DataModel::List<Structs::DSTOffsetStruct::Type>(mDst), 0 };
    GranularityEnum mGranularity = GranularityEnum::kNoTimeGranularity;
    TimeSourceEnum mTimeSource   = TimeSourceEnum::kNone;

    TimeSyncDataProvider::TimeZoneStore mTz[CHIP_CONFIG_TIME_ZONE_LIST_MAX_SIZE];
    Structs::DSTOffsetStruct::Type mDst[CHIP_CONFIG_DST_OFFSET_LIST_MAX_SIZE];

    TimeSyncDataProvider mTimeSyncDataProvider;
    static TimeSynchronizationServer sTimeSyncInstance;
    TimeState mTimeZoneState  = TimeState::kInvalid;
    TimeState mDSTOffsetState = TimeState::kInvalid;
#if TIME_SYNC_ENABLE_TSC_FEATURE
    chip::Callback::Callback<chip::OnDeviceConnected> mOnDeviceConnectedCallback;
    chip::Callback::Callback<chip::OnDeviceConnectionFailure> mOnDeviceConnectionFailureCallback;
    struct TimeReadInfo
    {
        TimeReadInfo(InteractionModelEngine * apImEngine, Messaging::ExchangeManager * apExchangeMgr,
                     ReadClient::Callback & apCallback, ReadClient::InteractionType aInteractionType) :
            readClient(apImEngine, apExchangeMgr, apCallback, aInteractionType)
        {
            utcTime.SetNull();
        }
        Attributes::UTCTime::TypeInfo::DecodableType utcTime;
        Attributes::Granularity::TypeInfo::DecodableType granularity = GranularityEnum::kNoTimeGranularity;
        ReadClient readClient;
    };
    Platform::UniquePtr<TimeReadInfo> mTimeReadInfo;
#endif
    chip::Callback::Callback<OnTimeSyncCompletion> mOnTimeSyncCompletion;
    chip::Callback::Callback<OnFallbackNTPCompletion> mOnFallbackNTPCompletion;

    // Called when the platform is set up - attempts to get time using the recommended source list in the spec.
    void AttemptToGetTime();
    // Attempts to get fallback NTP from the delegate (last available source)
    // If successful, the function will set mGranulatiry and the time source
    // If unsuccessful, it will emit a TimeFailure event.
    void AttemptToGetFallbackNTPTimeFromDelegate();
};

} // namespace TimeSynchronization
} // namespace Clusters
} // namespace app
} // namespace chip
