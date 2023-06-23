/*
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

#include "time-synchronization-server.h"
#include "DefaultTimeSyncDelegate.h"
#include "time-synchronization-delegate.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/cluster-objects.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/AttributeAccessInterface.h>
#include <app/CommandHandler.h>
#include <app/EventLogging.h>
#include <app/server/Server.h>
#include <app/util/attribute-storage.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/SortUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include <app-common/zap-generated/cluster-enums.h>

#include <system/SystemClock.h>

using namespace chip;
using namespace chip::app;
using namespace chip::DeviceLayer;
using namespace chip::app::Clusters;
using namespace chip::app::Clusters::TimeSynchronization;
using namespace chip::app::Clusters::TimeSynchronization::Attributes;
using chip::TimeSyncDataProvider;
using chip::Protocols::InteractionModel::Status;

// -----------------------------------------------------------------------------
// Delegate Implementation

namespace {

Delegate * gDelegate = nullptr;

Delegate * GetDelegate()
{
    if (gDelegate == nullptr)
    {
        static DefaultTimeSyncDelegate dg;
        gDelegate = &dg;
    }
    return gDelegate;
}
} // namespace

namespace chip {
namespace app {
namespace Clusters {
namespace TimeSynchronization {

void SetDefaultDelegate(Delegate * delegate)
{
    gDelegate = delegate;
}

Delegate * GetDefaultDelegate()
{
    return GetDelegate();
}

} // namespace TimeSynchronization
} // namespace Clusters
} // namespace app
} // namespace chip

constexpr uint64_t kChipEpochUsSinceUnixEpoch =
    static_cast<uint64_t>(kChipEpochSecondsSinceUnixEpoch) * chip::kMicrosecondsPerSecond;

static bool ChipEpochToUnixEpochMicro(uint64_t chipEpochTime, uint64_t & unixEpochTime)
{
    // in case chipEpochTime is too big and result overflows return false
    if (chipEpochTime + kChipEpochUsSinceUnixEpoch < kChipEpochUsSinceUnixEpoch)
    {
        return false;
    }
    unixEpochTime = chipEpochTime + kChipEpochUsSinceUnixEpoch;
    return true;
}

static bool UnixEpochToChipEpochMicro(uint64_t unixEpochTime, uint64_t & chipEpochTime)
{
    VerifyOrReturnValue(unixEpochTime >= kChipEpochUsSinceUnixEpoch, false);
    chipEpochTime = unixEpochTime - kChipEpochUsSinceUnixEpoch;

    return true;
}

static CHIP_ERROR UpdateUTCTime(uint64_t UTCTimeInChipEpochUs)
{
    System::Clock::Seconds32 lastKnownGoodChipEpoch;
    uint64_t UTCTimeInUnixEpochUs;

    ReturnErrorOnFailure(Server::GetInstance().GetFabricTable().GetLastKnownGoodChipEpochTime(lastKnownGoodChipEpoch));
    VerifyOrReturnError(ChipEpochToUnixEpochMicro(UTCTimeInChipEpochUs, UTCTimeInUnixEpochUs), CHIP_ERROR_INVALID_TIME);

    ReturnErrorOnFailure(Server::GetInstance().GetFabricTable().SetLastKnownGoodChipEpochTime(
        System::Clock::Seconds32(static_cast<uint32_t>(UTCTimeInChipEpochUs / chip::kMicrosecondsPerSecond))));
    ReturnErrorOnFailure(System::SystemClock().SetClock_RealTime(System::Clock::Microseconds64(UTCTimeInUnixEpochUs)));

    return CHIP_NO_ERROR;
}

static bool sendDSTTableEmptyEvent(EndpointId ep)
{
    Events::DSTTableEmpty::Type event;
    EventNumber eventNumber;

    CHIP_ERROR error = LogEvent(event, ep, eventNumber);

    if (CHIP_NO_ERROR != error)
    {
        ChipLogError(Zcl, "Unable to send DSTTableEmpty event [ep=%d]", ep);
        return false;
    }
    ChipLogProgress(Zcl, "Emit DSTTableEmpty event [ep=%d]", ep);

    // TODO: re-schedule event for after min 1hr https://github.com/project-chip/connectedhomeip/issues/27200
    // delegate->scheduleDSTTableEmptyEvent()
    return true;
}

static bool sendDSTStatusEvent(EndpointId ep, bool dstOffsetActive)
{
    Events::DSTStatus::Type event;
    event.DSTOffsetActive = dstOffsetActive;
    EventNumber eventNumber;

    CHIP_ERROR error = LogEvent(event, ep, eventNumber);

    if (CHIP_NO_ERROR != error)
    {
        ChipLogError(Zcl, "Unable to send sendDSTStatus event [ep=%d]", ep);
        return false;
    }

    ChipLogProgress(Zcl, "Emit sendDSTStatus event [ep=%d]", ep);
    return true;
}

static bool sendTimeZoneStatusEvent(EndpointId ep)
{
    auto tzList = TimeSynchronizationServer::Instance().GetTimeZone();
    VerifyOrReturnValue(tzList.size() != 0, false);
    auto & tz = tzList[0].timeZone;
    Events::TimeZoneStatus::Type event;

    event.offset = tz.offset;
    if (tz.name.HasValue())
    {
        event.name.SetValue(tz.name.Value());
    }
    EventNumber eventNumber;

    CHIP_ERROR error = LogEvent(event, ep, eventNumber);

    if (CHIP_NO_ERROR != error)
    {
        ChipLogError(Zcl, "Unable to send sendTimeZoneStatus event [ep=%d]", ep);
        return false;
    }

    ChipLogProgress(Zcl, "Emit sendTimeZoneStatus event [ep=%d]", ep);
    return true;
}

static bool sendTimeFailureEvent(EndpointId ep)
{
    Events::TimeFailure::Type event;
    EventNumber eventNumber;

    CHIP_ERROR error = LogEvent(event, ep, eventNumber);

    if (CHIP_NO_ERROR != error)
    {
        ChipLogError(Zcl, "Unable to send sendTimeFailure event [ep=%d]", ep);
        return false;
    }

    // TODO: re-schedule event for after min 1hr if no time is still available
    // https://github.com/project-chip/connectedhomeip/issues/27200
    ChipLogProgress(Zcl, "Emit sendTimeFailure event [ep=%d]", ep);
    return true;
}

static bool sendMissingTrustedTimeSourceEvent(EndpointId ep)
{
    Events::MissingTrustedTimeSource::Type event;
    EventNumber eventNumber;

    CHIP_ERROR error = LogEvent(event, ep, eventNumber);

    if (CHIP_NO_ERROR != error)
    {
        ChipLogError(Zcl, "Unable to send sendMissingTrustedTimeSource event [ep=%d]", ep);
        return false;
    }

    // TODO: re-schedule event for after min 1hr if TTS is null or cannot be reached
    // https://github.com/project-chip/connectedhomeip/issues/27200
    ChipLogProgress(Zcl, "Emit sendMissingTrustedTimeSource event [ep=%d]", ep);
    return true;
}

TimeSynchronizationServer TimeSynchronizationServer::sTimeSyncInstance;

TimeSynchronizationServer & TimeSynchronizationServer::Instance()
{
    return sTimeSyncInstance;
}

void TimeSynchronizationServer::Init()
{
    mTimeSyncDataProvider.Init(Server::GetInstance().GetPersistentStorage());

    Structs::TrustedTimeSourceStruct::Type tts;
    (CHIP_NO_ERROR == mTimeSyncDataProvider.LoadTrustedTimeSource(tts)) ? (void) mTrustedTimeSource.SetNonNull(tts)
                                                                        : mTrustedTimeSource.SetNull();
    LoadTimeZone();
    mTimeZoneObj.size = (mTimeZoneObj.size == 0) ? 1 : mTimeZoneObj.size; // initialize default value to {0,0}
    mTimeSyncDataProvider.LoadDSTOffset(mDstOffsetObj);
    if (!mTrustedTimeSource.IsNull())
    {
        // TODO: trusted time source is available, schedule a time read https://github.com/project-chip/connectedhomeip/issues/27201
    }
    System::Clock::Microseconds64 utcTime;
    if (System::SystemClock().GetClock_RealTime(utcTime) == CHIP_NO_ERROR)
    {
        mGranularity = GranularityEnum::kMinutesGranularity;
    }
    else
    {
        mGranularity = GranularityEnum::kNoTimeGranularity;
    }
}

CHIP_ERROR TimeSynchronizationServer::SetTrustedTimeSource(const DataModel::Nullable<Structs::TrustedTimeSourceStruct::Type> & tts)
{
    CHIP_ERROR err     = CHIP_NO_ERROR;
    mTrustedTimeSource = tts;
    if (!mTrustedTimeSource.IsNull())
    {
        err = mTimeSyncDataProvider.StoreTrustedTimeSource(mTrustedTimeSource.Value());
    }
    else
    {
        err = mTimeSyncDataProvider.ClearTrustedTimeSource();
    }
    return err;
}

CHIP_ERROR TimeSynchronizationServer::SetDefaultNTP(const DataModel::Nullable<CharSpan> & dntp)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    if (!dntp.IsNull())
    {
        err = mTimeSyncDataProvider.StoreDefaultNtp(dntp.Value());
    }
    else
    {
        err = mTimeSyncDataProvider.ClearDefaultNtp();
    }
    return err;
}

CHIP_ERROR TimeSynchronizationServer::SetTimeZone(const DataModel::DecodableList<Structs::TimeZoneStruct::Type> & tzL)
{
    size_t items;
    VerifyOrReturnError(CHIP_NO_ERROR == tzL.ComputeSize(&items), CHIP_IM_GLOBAL_STATUS(InvalidCommand));

    if (items > CHIP_CONFIG_TIME_ZONE_LIST_MAX_SIZE)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }

    char name[64];
    Structs::TimeZoneStruct::Type lastTz;
    TimeState lastTzState = GetUpdatedTimeZoneState();

    if (lastTzState != TimeState::kInvalid)
    {
        auto & tzStore = mTimeZoneObj.timeZoneList[0];
        lastTz.offset  = tzStore.timeZone.offset;
        if (tzStore.timeZone.name.HasValue())
        {
            lastTz.name.SetValue(CharSpan(name, sizeof(name)));
            memcpy(name, tzStore.name, sizeof(tzStore.name));
        }
    }

    auto newTzL = tzL.begin();
    uint8_t i   = 0;

    while (newTzL.Next())
    {
        auto & tzStore = mTimeZoneObj.timeZoneList[i];
        auto & newTz   = newTzL.GetValue();
        if (newTz.offset < -43200 || newTz.offset > 50400)
        {
            ReturnErrorOnFailure(LoadTimeZone());
            return CHIP_ERROR_IM_MALFORMED_COMMAND_DATA_IB;
        }
        // first element shall have validAt entry of 0
        if (i == 0 && newTz.validAt != 0)
        {
            return CHIP_ERROR_IM_MALFORMED_COMMAND_DATA_IB;
        }
        // if second element, it shall have validAt entry of non-0
        if (i != 0 && newTz.validAt == 0)
        {
            ReturnErrorOnFailure(LoadTimeZone());
            return CHIP_ERROR_IM_MALFORMED_COMMAND_DATA_IB;
        }
        tzStore.timeZone.offset  = newTz.offset;
        tzStore.timeZone.validAt = newTz.validAt;
        if (newTz.name.HasValue())
        {
            if (newTz.name.Value().size() > sizeof(tzStore.name))
            {
                ReturnErrorOnFailure(LoadTimeZone());
                return CHIP_ERROR_IM_MALFORMED_COMMAND_DATA_IB;
            }
            size_t len = newTz.name.Value().size();
            memset(tzStore.name, 0, sizeof(tzStore.name));
            chip::MutableCharSpan tempSpan(tzStore.name, len);
            if (CHIP_NO_ERROR != CopyCharSpanToMutableCharSpan(newTz.name.Value(), tempSpan))
            {
                ReturnErrorOnFailure(LoadTimeZone());
                return CHIP_IM_GLOBAL_STATUS(InvalidCommand);
            }
            tzStore.timeZone.name.SetValue(CharSpan(tzStore.name, len));
        }
        else
        {
            tzStore.timeZone.name.ClearValue();
        }
        i++;
    }
    if (CHIP_NO_ERROR != newTzL.GetStatus())
    {
        ReturnErrorOnFailure(LoadTimeZone());
        return CHIP_IM_GLOBAL_STATUS(InvalidCommand);
    }
    if (i == 0)
    {
        return ClearTimeZone();
    }

    mTimeZoneObj.size = i;

    if (lastTzState != TimeState::kInvalid && TimeState::kInvalid != GetUpdatedTimeZoneState())
    {
        bool emit = false;
        auto & tz = mTimeZoneObj.timeZoneList[0].timeZone;
        if (tz.offset != lastTz.offset)
        {
            emit = true;
        }
        if ((tz.name.HasValue() && lastTz.name.HasValue()) && !(tz.name.Value().data_equal(lastTz.name.Value())))
        {
            emit = true;
        }
        if (emit)
            mEventFlag = TimeSyncEventFlag::kTimeZoneStatus;
    }
    return mTimeSyncDataProvider.StoreTimeZone(GetTimeZone());
}

CHIP_ERROR TimeSynchronizationServer::LoadTimeZone()
{
    mTimeZoneObj.timeZoneList = Span<TimeZoneStore>(mTz);
    for (auto & tzStore : mTimeZoneObj.timeZoneList)
    {
        memset(tzStore.name, 0, sizeof(tzStore.name));
        tzStore.timeZone = { .offset = 0, .validAt = 0, .name = MakeOptional(CharSpan(tzStore.name, sizeof(tzStore.name))) };
    }
    return mTimeSyncDataProvider.LoadTimeZone(mTimeZoneObj);
}

CHIP_ERROR TimeSynchronizationServer::SetDSTOffset(const DataModel::DecodableList<Structs::DSTOffsetStruct::Type> & dstL)
{
    size_t items;
    VerifyOrReturnError(CHIP_NO_ERROR == dstL.ComputeSize(&items), CHIP_IM_GLOBAL_STATUS(InvalidCommand));

    if (items > CHIP_CONFIG_DST_OFFSET_LIST_MAX_SIZE)
    {
        return CHIP_ERROR_BUFFER_TOO_SMALL;
    }

    auto newDstL   = dstL.begin();
    auto & dstList = mDstOffsetObj.dstOffsetList;
    size_t i       = 0;

    while (newDstL.Next())
    {
        dstList[i] = newDstL.GetValue();
        i++;
    }

    if (CHIP_NO_ERROR != newDstL.GetStatus())
    {
        mTimeSyncDataProvider.LoadDSTOffset(mDstOffsetObj);
        return CHIP_IM_GLOBAL_STATUS(InvalidCommand);
    }
    if (i == 0)
    {
        return ClearDSTOffset();
    }

    mDstOffsetObj.size = i;

    // only 1 validuntil null value and shall be last in the list
    uint64_t lastValidUntil = 0;
    for (i = 0; i < mDstOffsetObj.size; i++)
    {
        const auto & dstItem = GetDSTOffset()[i];
        // list should be sorted by validStarting
        // validUntil shall be larger than validStarting
        if (!dstItem.validUntil.IsNull() && dstItem.validStarting >= dstItem.validUntil.Value())
        {
            mTimeSyncDataProvider.LoadDSTOffset(mDstOffsetObj);
            return CHIP_ERROR_INVALID_TIME;
        }
        // validStarting shall not be smaller than validUntil of previous entry
        if (dstItem.validStarting < lastValidUntil)
        {
            mTimeSyncDataProvider.LoadDSTOffset(mDstOffsetObj);
            return CHIP_ERROR_INVALID_TIME;
        }
        lastValidUntil = !dstItem.validUntil.IsNull() ? dstItem.validUntil.Value() : lastValidUntil;
        // only 1 validUntil null value and shall be last in the list
        if (dstItem.validUntil.IsNull() && (i != mDstOffsetObj.size - 1))
        {
            mTimeSyncDataProvider.LoadDSTOffset(mDstOffsetObj);
            return CHIP_ERROR_INVALID_TIME;
        }
    }

    return mTimeSyncDataProvider.StoreDSTOffset(GetDSTOffset());
}

CHIP_ERROR TimeSynchronizationServer::ClearTimeZone()
{
    mTimeZoneObj.size         = 1; // one default time zone item is needed
    mTimeZoneObj.timeZoneList = Span<TimeZoneStore>(mTz);
    for (auto & tzObj : mTimeZoneObj.timeZoneList)
    {
        memset(tzObj.name, 0, sizeof(tzObj.name));
        tzObj.timeZone = { .offset = 0, .validAt = 0, .name = MakeOptional(CharSpan(tzObj.name, sizeof(tzObj.name))) };
    }
    return mTimeSyncDataProvider.StoreTimeZone(GetTimeZone());
}

CHIP_ERROR TimeSynchronizationServer::ClearDSTOffset()
{
    mDstOffsetObj.size = 0;
    return mTimeSyncDataProvider.ClearDSTOffset();
}

DataModel::Nullable<Structs::TrustedTimeSourceStruct::Type> & TimeSynchronizationServer::GetTrustedTimeSource()
{
    return mTrustedTimeSource;
}

CHIP_ERROR TimeSynchronizationServer::GetDefaultNtp(MutableCharSpan & dntp)
{
    return mTimeSyncDataProvider.LoadDefaultNtp(dntp);
}

Span<TimeZoneStore> TimeSynchronizationServer::GetTimeZone()
{
    return mTimeZoneObj.timeZoneList.SubSpan(0, mTimeZoneObj.size);
}

DataModel::List<Structs::DSTOffsetStruct::Type> TimeSynchronizationServer::GetDSTOffset()
{
    return mDstOffsetObj.dstOffsetList.SubSpan(0, mDstOffsetObj.size);
}

void TimeSynchronizationServer::ScheduleDelayedAction(System::Clock::Seconds32 delay, System::TimerCompleteCallback action,
                                                      void * aAppState)
{
    if (CHIP_NO_ERROR != SystemLayer().StartTimer(std::chrono::duration_cast<System::Clock::Timeout>(delay), action, aAppState))
    {
        ChipLogError(Zcl, "Time Synchronization failed to schedule timer.");
    }
}

CHIP_ERROR TimeSynchronizationServer::SetUTCTime(EndpointId ep, uint64_t utcTime, GranularityEnum granularity,
                                                 TimeSourceEnum source)
{
    ReturnErrorOnFailure(UpdateUTCTime(utcTime));
    mGranularity = granularity;
    if (EMBER_ZCL_STATUS_SUCCESS != TimeSource::Set(ep, source))
    {
        ChipLogError(Zcl, "Writing TimeSource failed.");
        return CHIP_IM_GLOBAL_STATUS(Failure);
    }
    return CHIP_NO_ERROR;
}

CHIP_ERROR TimeSynchronizationServer::GetLocalTime(EndpointId ep, DataModel::Nullable<uint64_t> & localTime)
{
    int64_t timeZoneOffset = 0, dstOffset = 0;
    System::Clock::Microseconds64 utcTime;
    uint64_t chipEpochTime;
    VerifyOrReturnError(TimeState::kInvalid != GetUpdatedDSTOffsetState(), CHIP_ERROR_INVALID_TIME);
    ReturnErrorOnFailure(System::SystemClock().GetClock_RealTime(utcTime));
    VerifyOrReturnError(UnixEpochToChipEpochMicro(utcTime.count(), chipEpochTime), CHIP_ERROR_INVALID_TIME);
    if (TimeState::kChanged == GetUpdatedTimeZoneState())
    {
        sendTimeZoneStatusEvent(ep);
    }
    VerifyOrReturnError(GetTimeZone().size() != 0, CHIP_ERROR_INVALID_TIME);
    const auto & tzStore = GetTimeZone()[0];
    timeZoneOffset       = static_cast<int64_t>(tzStore.timeZone.offset);
    VerifyOrReturnError(GetDSTOffset().size() != 0, CHIP_ERROR_INVALID_TIME);
    const auto & dst = GetDSTOffset()[0];
    dstOffset        = static_cast<int64_t>(dst.offset);

    uint64_t usRemainder = chipEpochTime % chip::kMicrosecondsPerSecond;   // microseconds part of chipEpochTime
    chipEpochTime        = (chipEpochTime / chip::kMicrosecondsPerSecond); // make it safe to cast to int64 by converting to seconds

    uint64_t localTimeSec = static_cast<uint64_t>(static_cast<int64_t>(chipEpochTime) + timeZoneOffset + dstOffset);
    localTime.SetNonNull((localTimeSec * chip::kMicrosecondsPerSecond) + usRemainder);
    return CHIP_NO_ERROR;
}

TimeState TimeSynchronizationServer::GetUpdatedTimeZoneState()
{
    System::Clock::Microseconds64 utcTime;
    auto tzList          = GetTimeZone();
    size_t activeTzIndex = 0;
    uint64_t chipEpochTime;

    VerifyOrReturnValue(System::SystemClock().GetClock_RealTime(utcTime) == CHIP_NO_ERROR, TimeState::kInvalid);
    VerifyOrReturnValue(tzList.size() != 0, TimeState::kInvalid);
    VerifyOrReturnValue(UnixEpochToChipEpochMicro(utcTime.count(), chipEpochTime), TimeState::kInvalid);

    for (size_t i = 0; i < tzList.size(); i++)
    {
        auto & tz = tzList[i].timeZone;
        if (tz.validAt != 0 && tz.validAt <= chipEpochTime)
        {
            tz.validAt    = 0;
            activeTzIndex = i;
        }
    }
    if (activeTzIndex != 0)
    {
        mTimeZoneObj.size    = tzList.size() - activeTzIndex;
        auto newTimeZoneList = tzList.SubSpan(activeTzIndex);
        VerifyOrReturnValue(mTimeSyncDataProvider.StoreTimeZone(newTimeZoneList) == CHIP_NO_ERROR, TimeState::kInvalid);
        VerifyOrReturnValue(LoadTimeZone() == CHIP_NO_ERROR, TimeState::kInvalid);
        return TimeState::kChanged;
    }
    return TimeState::kActive;
}

TimeState TimeSynchronizationServer::GetUpdatedDSTOffsetState()
{
    System::Clock::Microseconds64 utcTime;
    size_t activeDstIndex = 0;
    auto dstList          = GetDSTOffset();
    uint64_t chipEpochTime;
    bool dstStopped = true;

    VerifyOrReturnValue(System::SystemClock().GetClock_RealTime(utcTime) == CHIP_NO_ERROR, TimeState::kInvalid);
    VerifyOrReturnValue(dstList.size() != 0, TimeState::kInvalid);
    VerifyOrReturnValue(UnixEpochToChipEpochMicro(utcTime.count(), chipEpochTime), TimeState::kInvalid);

    for (size_t i = 0; i < dstList.size(); i++)
    {
        if (dstList[i].validStarting <= chipEpochTime)
        {
            activeDstIndex = i;
            dstStopped     = false;
        }
    }
    VerifyOrReturnValue(dstStopped, TimeState::kStopped);
    // if offset is zero and validUntil is null then no DST is used
    if (dstList[activeDstIndex].offset == 0 && dstList[activeDstIndex].validUntil.IsNull())
    {
        return TimeState::kStopped;
    }
    if (!dstList[activeDstIndex].validUntil.IsNull() && dstList[activeDstIndex].validUntil.Value() <= chipEpochTime)
    {
        mDstOffsetObj.size = 0;
        VerifyOrReturnValue(mTimeSyncDataProvider.ClearDSTOffset() == CHIP_NO_ERROR, TimeState::kInvalid);
        // list is empty, generate DSTTableEmpty event
        sendDSTTableEmptyEvent(GetDelegate()->GetEndpoint());
        return TimeState::kInvalid;
    }
    if (activeDstIndex > 0)
    {
        mDstOffsetObj.size    = dstList.size() - activeDstIndex;
        auto newDstOffsetList = dstList.SubSpan(activeDstIndex);
        VerifyOrReturnValue(mTimeSyncDataProvider.StoreDSTOffset(newDstOffsetList) == CHIP_NO_ERROR, TimeState::kInvalid);
        VerifyOrReturnValue(mTimeSyncDataProvider.LoadDSTOffset(mDstOffsetObj) == CHIP_NO_ERROR, TimeState::kInvalid);
        return TimeState::kChanged;
    }
    return TimeState::kActive;
}

TimeSyncEventFlag TimeSynchronizationServer::GetEventFlag()
{
    return mEventFlag;
}

void TimeSynchronizationServer::ClearEventFlag(TimeSyncEventFlag flag)
{
    uint8_t eventFlag = to_underlying(mEventFlag) ^ to_underlying(flag);
    mEventFlag        = static_cast<TimeSyncEventFlag>(eventFlag);
}

namespace {

class TimeSynchronizationAttrAccess : public AttributeAccessInterface
{
public:
    // register for the TimeSync cluster on all endpoints
    TimeSynchronizationAttrAccess() : AttributeAccessInterface(Optional<EndpointId>::Missing(), Id) {}

    CHIP_ERROR Read(const ConcreteReadAttributePath & aPath, AttributeValueEncoder & aEncoder) override;

private:
    CHIP_ERROR ReadTrustedTimeSource(EndpointId endpoint, AttributeValueEncoder & aEncoder);
    CHIP_ERROR ReadDefaultNtp(EndpointId endpoint, AttributeValueEncoder & aEncoder);
    CHIP_ERROR ReadTimeZone(EndpointId endpoint, AttributeValueEncoder & aEncoder);
    CHIP_ERROR ReadDSTOffset(EndpointId endpoint, AttributeValueEncoder & aEncoder);
    CHIP_ERROR ReadLocalTime(EndpointId endpoint, AttributeValueEncoder & aEncoder);
};

TimeSynchronizationAttrAccess gAttrAccess;

CHIP_ERROR TimeSynchronizationAttrAccess::ReadTrustedTimeSource(EndpointId endpoint, AttributeValueEncoder & aEncoder)
{
    auto tts = TimeSynchronizationServer::Instance().GetTrustedTimeSource();
    return aEncoder.Encode(tts);
}

CHIP_ERROR TimeSynchronizationAttrAccess::ReadDefaultNtp(EndpointId endpoint, AttributeValueEncoder & aEncoder)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    char buffer[DefaultNTP::TypeInfo::MaxLength()];
    MutableCharSpan dntp(buffer);
    err = TimeSynchronizationServer::Instance().GetDefaultNtp(dntp);
    if (err == CHIP_NO_ERROR)
    {
        err = aEncoder.Encode(CharSpan(buffer, dntp.size()));
    }
    else if (err == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
    {
        err = aEncoder.EncodeNull();
    }
    return err;
}

CHIP_ERROR TimeSynchronizationAttrAccess::ReadTimeZone(EndpointId endpoint, AttributeValueEncoder & aEncoder)
{
    CHIP_ERROR err = aEncoder.EncodeList([](const auto & encoder) -> CHIP_ERROR {
        auto tzList = TimeSynchronizationServer::Instance().GetTimeZone();
        for (const auto & tzStore : tzList)
        {
            ReturnErrorOnFailure(encoder.Encode(tzStore.timeZone));
        }

        return CHIP_NO_ERROR;
    });

    return err;
}

CHIP_ERROR TimeSynchronizationAttrAccess::ReadDSTOffset(EndpointId endpoint, AttributeValueEncoder & aEncoder)
{
    CHIP_ERROR err = aEncoder.EncodeList([](const auto & encoder) -> CHIP_ERROR {
        DSTOffset::TypeInfo::Type dstList = TimeSynchronizationServer::Instance().GetDSTOffset();
        for (const auto & dstOffset : dstList)
        {
            ReturnErrorOnFailure(encoder.Encode(dstOffset));
        }

        return CHIP_NO_ERROR;
    });

    return err;
}

CHIP_ERROR TimeSynchronizationAttrAccess::ReadLocalTime(EndpointId endpoint, AttributeValueEncoder & aEncoder)
{
    DataModel::Nullable<uint64_t> localTime;
    CHIP_ERROR err = TimeSynchronizationServer::Instance().GetLocalTime(endpoint, localTime);
    if (err == CHIP_NO_ERROR)
    {
        if (!localTime.IsNull())
        {
            err = aEncoder.Encode(localTime);
        }
        else
        {
            err = aEncoder.EncodeNull();
        }
    }
    return err;
}

CHIP_ERROR TimeSynchronizationAttrAccess::Read(const ConcreteReadAttributePath & aPath, AttributeValueEncoder & aEncoder)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    if (aPath.mClusterId != Id)
    {
        return CHIP_ERROR_INVALID_PATH_LIST;
    }

    switch (aPath.mAttributeId)
    {
    case UTCTime::Id: {
        System::Clock::Microseconds64 utcTimeUnix;
        uint64_t chipEpochTime;

        VerifyOrReturnError(System::SystemClock().GetClock_RealTime(utcTimeUnix) == CHIP_NO_ERROR, aEncoder.EncodeNull());
        VerifyOrReturnError(UnixEpochToChipEpochMicro(utcTimeUnix.count(), chipEpochTime), aEncoder.EncodeNull());
        return aEncoder.Encode(chipEpochTime);
    }
    case Granularity::Id: {
        return aEncoder.Encode(TimeSynchronizationServer::Instance().GetGranularity());
    }
    case TrustedTimeSource::Id: {
        return ReadTrustedTimeSource(aPath.mEndpointId, aEncoder);
    }
    case DefaultNTP::Id: {
        return ReadDefaultNtp(aPath.mEndpointId, aEncoder);
    }
    case TimeZone::Id: {
        return ReadTimeZone(aPath.mEndpointId, aEncoder);
    }
    case DSTOffset::Id: {
        return ReadDSTOffset(aPath.mEndpointId, aEncoder);
    }
    case TimeZoneListMaxSize::Id: {
        uint8_t max = CHIP_CONFIG_TIME_ZONE_LIST_MAX_SIZE;
        return aEncoder.Encode(max);
    }
    case DSTOffsetListMaxSize::Id: {
        uint8_t max = CHIP_CONFIG_DST_OFFSET_LIST_MAX_SIZE;
        return aEncoder.Encode(max);
    }
    case LocalTime::Id: {
        return ReadLocalTime(aPath.mEndpointId, aEncoder);
    }
    default: {
        break;
    }
    }

    return err;
}
} // anonymous namespace

bool emberAfTimeSynchronizationClusterSetUTCTimeCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::TimeSynchronization::Commands::SetUTCTime::DecodableType & commandData)
{
    const auto & utcTime     = commandData.UTCTime;
    const auto & granularity = commandData.granularity;

    auto currentGranularity = TimeSynchronizationServer::Instance().GetGranularity();

    if (granularity != GranularityEnum::kNoTimeGranularity &&
        (currentGranularity == GranularityEnum::kNoTimeGranularity || granularity >= currentGranularity) &&
        CHIP_NO_ERROR ==
            TimeSynchronizationServer::Instance().SetUTCTime(commandPath.mEndpointId, utcTime, granularity, TimeSourceEnum::kAdmin))
    {
        commandObj->AddStatus(commandPath, Status::Success);
    }
    else
    {
        commandObj->AddClusterSpecificFailure(commandPath, to_underlying(StatusCode::kTimeNotAccepted));
    }
    return true;
}

bool emberAfTimeSynchronizationClusterSetTrustedTimeSourceCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::TimeSynchronization::Commands::SetTrustedTimeSource::DecodableType & commandData)
{
    const auto & timeSource = commandData.trustedTimeSource;
    DataModel::Nullable<Structs::TrustedTimeSourceStruct::Type> tts;

    if (!timeSource.IsNull())
    {

        Structs::TrustedTimeSourceStruct::Type ts = { commandObj->GetAccessingFabricIndex(), timeSource.Value().nodeID,
                                                      timeSource.Value().endpoint };
        tts.SetNonNull(ts);
        // TODO: schedule a utctime read from this time source and emit event only on failure to get time
        sendTimeFailureEvent(commandPath.mEndpointId);
    }
    else
    {
        tts.SetNull();
        sendMissingTrustedTimeSourceEvent(commandPath.mEndpointId);
    }

    TimeSynchronizationServer::Instance().SetTrustedTimeSource(tts);
    commandObj->AddStatus(commandPath, Status::Success);
    return true;
}

bool emberAfTimeSynchronizationClusterSetTimeZoneCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::TimeSynchronization::Commands::SetTimeZone::DecodableType & commandData)
{
    const auto & timeZone = commandData.timeZone;

    CHIP_ERROR err = TimeSynchronizationServer::Instance().SetTimeZone(timeZone);
    if (err != CHIP_NO_ERROR)
    {
        if (err == CHIP_ERROR_BUFFER_TOO_SMALL)
        {
            commandObj->AddStatus(commandPath, Status::ResourceExhausted);
        }
        else if (err == CHIP_IM_GLOBAL_STATUS(InvalidCommand))
        {
            commandObj->AddStatus(commandPath, Status::InvalidCommand);
        }
        else
        {
            commandObj->AddStatus(commandPath, Status::ConstraintError);
        }
        return true;
    }

    if (to_underlying(TimeSynchronizationServer::Instance().GetEventFlag()) & to_underlying(TimeSyncEventFlag::kTimeZoneStatus))
    {
        TimeSynchronizationServer::Instance().ClearEventFlag(TimeSyncEventFlag::kTimeZoneStatus);
        sendTimeZoneStatusEvent(commandPath.mEndpointId);
    }
    GetDelegate()->HandleTimeZoneChanged(TimeSynchronizationServer::Instance().GetTimeZone());

    TimeZoneDatabaseEnum tzDb;
    TimeZoneDatabase::Get(commandPath.mEndpointId, &tzDb);
    Commands::SetTimeZoneResponse::Type response;
    TimeSynchronizationServer::Instance().GetUpdatedTimeZoneState();
    const auto & tzList = TimeSynchronizationServer::Instance().GetTimeZone();
    if (GetDelegate()->HasFeature(commandPath.mEndpointId, Feature::kTimeZone) && tzDb != TimeZoneDatabaseEnum::kNone &&
        tzList.size() != 0)
    {
        auto & tz = tzList[0].timeZone;
        if (tz.name.HasValue() && GetDelegate()->HandleUpdateDSTOffset(tz.name.Value()))
        {
            response.DSTOffsetRequired = false;
            sendDSTStatusEvent(commandPath.mEndpointId, true);
        }
        else
        {
            TimeState dstState = TimeSynchronizationServer::Instance().GetUpdatedDSTOffsetState();
            TimeSynchronizationServer::Instance().ClearDSTOffset();
            sendDSTTableEmptyEvent(commandPath.mEndpointId);
            if (dstState == TimeState::kActive || dstState == TimeState::kChanged)
            {
                sendDSTStatusEvent(commandPath.mEndpointId, false);
            }
            response.DSTOffsetRequired = true;
        }
    }
    else
    {
        response.DSTOffsetRequired = true;
    }
    commandObj->AddResponse(commandPath, response);
    return true;
}

bool emberAfTimeSynchronizationClusterSetDSTOffsetCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::TimeSynchronization::Commands::SetDSTOffset::DecodableType & commandData)
{
    const auto & dstOffset = commandData.DSTOffset;

    TimeState dstState = TimeSynchronizationServer::Instance().GetUpdatedDSTOffsetState();

    CHIP_ERROR err = TimeSynchronizationServer::Instance().SetDSTOffset(dstOffset);
    if (err != CHIP_NO_ERROR)
    {
        if (err == CHIP_ERROR_BUFFER_TOO_SMALL)
        {
            commandObj->AddStatus(commandPath, Status::ResourceExhausted);
        }
        else if (err == CHIP_IM_GLOBAL_STATUS(InvalidCommand))
        {
            commandObj->AddStatus(commandPath, Status::InvalidCommand);
        }
        else
        {
            commandObj->AddStatus(commandPath, Status::ConstraintError);
        }
        return true;
    }
    // if DST state changes, generate DSTStatus event
    if (dstState != TimeSynchronizationServer::Instance().GetUpdatedDSTOffsetState())
    {
        sendDSTStatusEvent(commandPath.mEndpointId,
                           TimeState::kActive == TimeSynchronizationServer::Instance().GetUpdatedDSTOffsetState());
    }

    commandObj->AddStatus(commandPath, Status::Success);
    return true;
}

bool emberAfTimeSynchronizationClusterSetDefaultNTPCallback(
    chip::app::CommandHandler * commandObj, const chip::app::ConcreteCommandPath & commandPath,
    const chip::app::Clusters::TimeSynchronization::Commands::SetDefaultNTP::DecodableType & commandData)
{
    Status status = Status::Success;
    auto dNtpChar = commandData.defaultNTP;

    if (!dNtpChar.IsNull())
    {
        size_t len = dNtpChar.Value().size();
        if (len > DefaultNTP::TypeInfo::MaxLength())
        {
            commandObj->AddStatus(commandPath, Status::ConstraintError);
            return true;
        }
        if (!GetDelegate()->IsNTPAddressValid(dNtpChar.Value()))
        {
            commandObj->AddStatus(commandPath, Status::InvalidCommand);
            return true;
        }
        if (GetDelegate()->IsNTPAddressDomain(dNtpChar.Value()))
        {
            bool dnsResolve;
            if (EMBER_ZCL_STATUS_SUCCESS != SupportsDNSResolve::Get(commandPath.mEndpointId, &dnsResolve))
            {
                commandObj->AddStatus(commandPath, Status::Failure);
                return true;
            }
            if (!dnsResolve)
            {
                commandObj->AddStatus(commandPath, Status::InvalidCommand);
                return true;
            }
        }
    }

    status = (CHIP_NO_ERROR == TimeSynchronizationServer::Instance().SetDefaultNTP(dNtpChar)) ? Status::Success : Status::Failure;

    commandObj->AddStatus(commandPath, status);
    return true;
}

void MatterTimeSynchronizationPluginServerInitCallback()
{
    static bool attrAccessRegistered = false;
    TimeSynchronizationServer::Instance().Init();
    if (!attrAccessRegistered)
    {
        attrAccessRegistered = true;
        registerAttributeAccessOverride(&gAttrAccess);
    }
}
