/**
 * @file GGYGOCharacterMovementComponent.cpp
 * @brief 项目 CMC 实现
 */
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Character/Data/GGYGOLocomotionEvaluation.h"
#include "Character/Data/GGYGOLocomotionSteeringEvaluation.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Input/GGYGOPlayerInput.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOCurveRootMotionSource.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Misc/ScopeExit.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Class.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterMovementComponent)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMovement, Log, All);

bool FGGYGOQualifiedMovementIntent::operator==(const FGGYGOQualifiedMovementIntent& Other) const
{
	return Scope == Other.Scope && Provenance == Other.Provenance && BindingSerial == Other.BindingSerial
		&& SessionSerial == Other.SessionSerial && RequestSerial == Other.RequestSerial
		&& ExecutionRequestSerial == Other.ExecutionRequestSerial;
}

struct UGGYGOCharacterMovementComponent::FQualifiedMovementIntentObserver
{
	FGGYGOQualifiedMovementIntentObserverId Id;
	FGGYGOQualifiedMovementIntentDelegate Callback;
	EGGYGOQualifiedMovementIntentQueryResult LastResult = EGGYGOQualifiedMovementIntentQueryResult::Unavailable;
	FGGYGOQualifiedMovementIntent LastIntent;
	FString LastError;
	bool bClosed = false;
};

namespace GGYGOMovementOwnerSync
{
	// 仅签身份；当前拥有者、就绪和执行状态仍由各原 CMC 唯一持有。
	uint64 LastIdentitySerial = 0;
	uint64 IssueIdentity()
	{
		if (!IsInGameThread() || LastIdentitySerial == MAX_uint64)
		{
			UE_LOG(LogGGYGOMovement, Error, TEXT("[Movement.OwnerSync] Identity issuance rejected: game thread required and serial must not wrap."));
			return 0;
		}
		return ++LastIdentitySerial;
	}

	UNetConnection* GetConnection(APawn* Pawn, APlayerController* Controller)
	{
		if (!Pawn || !Controller) return nullptr;
		if (Pawn->HasAuthority()) return Controller->GetNetConnection();
		// 本地客户端的原生端点是 ServerConnection，PC 的 Player 是 LocalPlayer。
		UNetDriver* Driver = Pawn->GetNetDriver();
		return Driver ? ToRawPtr(Driver->ServerConnection) : nullptr;
	}

	bool IsConnectionLive(const UNetConnection* Connection)
	{
		return IsValid(Connection) && Connection->GetConnectionState() != USOCK_Closed
			&& Connection->GetConnectionState() != USOCK_Invalid;
	}

	bool SerializeMove(FArchive& Ar, bool& Has, uint64& Nonce, uint64& Generation)
	{
		uint8 Version = FCharacterNetworkMoveData_GGYGO::OwnerSyncWireVersion;
		bool Present = Ar.IsLoading() ? false : Has;
		uint64 ReadNonce = Ar.IsLoading() ? 0 : Nonce;
		uint64 ReadGeneration = Ar.IsLoading() ? 0 : Generation;
		Ar.SerializeBits(&Version, 8);
		if (Ar.IsError() || Version != FCharacterNetworkMoveData_GGYGO::OwnerSyncWireVersion)
		{
			Ar.SetError(); return false;
		}
		Ar.SerializeBits(&Present, 1);
		if (Present) { Ar << ReadNonce; Ar << ReadGeneration; }
		if (Ar.IsError() || (Present ? ReadNonce == 0 : (ReadNonce != 0 || ReadGeneration != 0)))
		{
			Ar.SetError(); return false;
		}
		if (Ar.IsLoading()) { Has = Present; Nonce = ReadNonce; Generation = ReadGeneration; }
		return true;
	}

	bool SerializeResponse(FArchive& Ar, FCharacterMoveResponseDataContainer_GGYGO& Response)
	{
		uint8 Version = FCharacterMoveResponseDataContainer_GGYGO::OwnerSyncWireVersion;
		bool Present = Ar.IsLoading() ? false : Response.bHasMovementOwnerSync;
		uint64 Nonce = Ar.IsLoading() ? 0 : Response.MovementOwnerSyncNonce;
		uint64 Generation = Ar.IsLoading() ? 0 : Response.ServerOwnerGeneration;
		uint64 Adjustment = Ar.IsLoading() ? 0 : Response.AdjustmentOwnerGeneration;
		bool Active = Ar.IsLoading() ? false : Response.bMovementOwnerActive;
		bool Initial = Ar.IsLoading() ? false : Response.bInitialSynchronizationEligible;
		Ar.SerializeBits(&Version, 8);
		if (Ar.IsError() || Version != FCharacterMoveResponseDataContainer_GGYGO::OwnerSyncWireVersion)
		{
			Ar.SetError(); return false;
		}
		Ar.SerializeBits(&Present, 1);
		if (Present)
		{
			Ar << Nonce; Ar << Generation; Ar << Adjustment;
			Ar.SerializeBits(&Active, 1); Ar.SerializeBits(&Initial, 1);
		}
		if (Ar.IsError() || (Present
			? (Nonce == 0 || Generation == 0 || Adjustment == 0 || (Initial && !Active))
			: (Nonce != 0 || Generation != 0 || Adjustment != 0 || Active || Initial)))
		{
			Ar.SetError(); return false;
		}
		if (Ar.IsLoading())
		{
			Response.bHasMovementOwnerSync = Present;
			Response.MovementOwnerSyncNonce = Nonce;
			Response.ServerOwnerGeneration = Generation;
			Response.AdjustmentOwnerGeneration = Adjustment;
			Response.bMovementOwnerActive = Active;
			Response.bInitialSynchronizationEligible = Initial;
		}
		return true;
	}
}

bool FGGYGOMovementOwnerSyncScopeId::IsSet() const
{
	return !Consumer.IsExplicitlyNull() && !OriginalPawn.IsExplicitlyNull()
		&& !OriginalPlayerController.IsExplicitlyNull() && ConsumerLifetimeSerial != 0
		&& OwnerContextSerial != 0 && ScopeSerial != 0 && ResponseNonce != 0;
}
bool FGGYGOMovementOwnerSyncScopeId::operator==(const FGGYGOMovementOwnerSyncScopeId& Other) const
{
	return Consumer.HasSameIndexAndSerialNumber(Other.Consumer)
		&& OriginalPawn.HasSameIndexAndSerialNumber(Other.OriginalPawn)
		&& OriginalPlayerController.HasSameIndexAndSerialNumber(Other.OriginalPlayerController)
		&& ConsumerLifetimeSerial == Other.ConsumerLifetimeSerial && OwnerContextSerial == Other.OwnerContextSerial
		&& ScopeSerial == Other.ScopeSerial && ResponseNonce == Other.ResponseNonce;
}
bool FGGYGOMovementOwnerSyncScopeId::operator!=(const FGGYGOMovementOwnerSyncScopeId& Other) const { return !(*this == Other); }
TWeakObjectPtr<UGGYGOCharacterMovementComponent> FGGYGOMovementOwnerSyncScopeId::GetConsumer() const { return Consumer; }
TWeakObjectPtr<APawn> FGGYGOMovementOwnerSyncScopeId::GetOriginalPawn() const { return OriginalPawn; }
TWeakObjectPtr<APlayerController> FGGYGOMovementOwnerSyncScopeId::GetOriginalPlayerController() const { return OriginalPlayerController; }
uint64 FGGYGOMovementOwnerSyncScopeId::GetConsumerLifetimeSerial() const { return ConsumerLifetimeSerial; }
uint64 FGGYGOMovementOwnerSyncScopeId::GetOwnerContextSerial() const { return OwnerContextSerial; }
uint64 FGGYGOMovementOwnerSyncScopeId::GetScopeSerial() const { return ScopeSerial; }
uint64 FGGYGOMovementOwnerSyncScopeId::GetResponseNonce() const { return ResponseNonce; }
bool FGGYGOMovementOwnerSyncObserverId::IsSet() const { return Scope.IsSet() && ObserverSerial != 0; }
const FGGYGOMovementOwnerSyncScopeId& FGGYGOMovementOwnerSyncObserverId::GetScope() const { return Scope; }
uint64 FGGYGOMovementOwnerSyncObserverId::GetObserverSerial() const { return ObserverSerial; }
bool FGGYGOMovementOwnerSyncNotice::IsSet() const
{
	return Scope.IsSet() && NoticeSerial != 0 && static_cast<uint8>(State) <= static_cast<uint8>(EGGYGOMovementOwnerSyncState::Invalidated);
}
const FGGYGOMovementOwnerSyncScopeId& FGGYGOMovementOwnerSyncNotice::GetScope() const { return Scope; }
EGGYGOMovementOwnerSyncState FGGYGOMovementOwnerSyncNotice::GetState() const { return State; }
uint64 FGGYGOMovementOwnerSyncNotice::GetNoticeSerial() const { return NoticeSerial; }
uint64 FGGYGOMovementOwnerSyncNotice::GetServerOwnerGeneration() const { return ServerOwnerGeneration; }
uint64 FGGYGOMovementOwnerSyncNotice::GetNativeResponseNonce() const { return NativeResponseNonce; }
bool FGGYGOMovementOwnerSyncNotice::IsInitialSynchronizationEligible() const { return bInitialSynchronizationEligible; }
FName FGGYGOMovementOwnerSyncNotice::GetReason() const { return Reason; }

namespace GGYGOMovementConstants
{
	/** 判定"正在移动"的水平速度阈值（cm/s）。用于过滤碰撞挤压等微小残余速度。 */
	constexpr float MovingSpeedThreshold = 10.0f;

	/**
	 * 单帧 DeltaTime 上限（秒）。
	 *
	 * 卡顿帧（加载、断点）会带来一个巨大的 DeltaTime，直接累加会让走跑计时器
	 * 一帧跳过阈值，玩家感觉"莫名其妙就跑起来了"。钳住比忽略好 ——
	 * 忽略会让长时间低帧率下永远升不了档。
	 */
	constexpr float MaxClampedDelta = 0.1f;

	/** 走跑计时器上限（秒）。防止长时间行走导致浮点累加失去精度。 */
	constexpr float MaxWalkHoldSeconds = 3600.0f;

	/** ForceWalk 是客户端输入请求；步态与动作段由服务端按自身 MovementSet/原动画源重算。 */
	constexpr uint8 ForceWalkFlag = FSavedMove_Character::FLAG_Custom_3;

	/**
	 * 曲线位移源的实例名。
	 *
	 * root motion source 按名字管理（挂载、查询、摘除），所以每个语义段必须有
	 * 各自的名字。同名会被引擎当成同一个源，`Matches` 也用它做网络匹配。
	 */
	const FName CurveBrakeSourceName(TEXT("GGYGO.CurveBrake"));
	const FName CurveTurnBackSourceName(TEXT("GGYGO.CurveTurnBack"));
	const FName ActionCurveSourceName(TEXT("GGYGO.ActionCurve"));

	/**
	 * 曲线位移源的优先级。
	 *
	 * 只有最高优先级的 Override 源生效，其余被忽略。刹停与转身在设计上互斥，
	 * 给转身更高的值是兜底：万一两者同时在场，转身赢。
	 */
	constexpr uint16 CurveBrakePriority = 10;
	constexpr uint16 CurveTurnBackPriority = 20;
	constexpr uint16 ActionCurvePriority = 100;
}

namespace
{
	bool IsRegisteredActionCurveSource(const FRootMotionSource& Source)
	{
		if (Source.GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()
			|| Source.InstanceName != GGYGOMovementConstants::ActionCurveSourceName
			|| Source.Priority != GGYGOMovementConstants::ActionCurvePriority) return false;
		const auto& Typed = static_cast<const FRootMotionSource_GGYGOActionCurve&>(Source);
		return Source.AccumulateMode == ERootMotionAccumulateMode::Override
			|| (Typed.SourceMode == EGGYGOActionCurveSourceMode::OriginalMontage
				&& Typed.bExplicitlyCancelled && Source.AccumulateMode == ERootMotionAccumulateMode::Additive);
	}

	bool IsOwnedLocomotionCurveSource(const FRootMotionSource& Source)
	{
		return Source.GetScriptStruct() == FRootMotionSource_GGYGOCurve::StaticStruct()
			&& Source.AccumulateMode == ERootMotionAccumulateMode::Override
			&& ((Source.InstanceName == GGYGOMovementConstants::CurveBrakeSourceName
					&& Source.Priority == GGYGOMovementConstants::CurveBrakePriority)
				|| (Source.InstanceName == GGYGOMovementConstants::CurveTurnBackSourceName
					&& Source.Priority == GGYGOMovementConstants::CurveTurnBackPriority));
	}

	bool IsLocomotionCurveSourceInGroup(const FRootMotionSourceGroup& Group, const FRootMotionSource* Source)
	{
		const auto Contains = [Source](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
		{
			return Sources.ContainsByPredicate([Source](const TSharedPtr<FRootMotionSource>& Item)
			{
				return Item.Get() == Source;
			});
		};
		return Contains(Group.RootMotionSources) || Contains(Group.PendingAddRootMotionSources);
	}

	bool IsFiniteLocomotionCurveVector(const FVector& Value)
	{
		return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
	}
}

/** Immutable original move input; CMC owns both capture and interpretation. */
struct FGGYGOCurveRootMotionMoveInput
{
	TSharedPtr<const FGGYGOCurveRootMotionOrigin> Origin;
	TSharedPtr<const UGGYGOCharacterMovementComponent::FMovementInputNativeSource> NativeSource;
	UGGYGOCharacterMovementComponent::FLocomotionUpdateCandidate StartCandidate;
	FVector Acceleration = FVector::ZeroVector;
	float MovementTickTime = 0.0f;
	bool bHasInput = false;
	bool bBlocked = false;
	bool bOnGround = false;
	bool bForceWalk = false;
	bool bHadMoveInput = false;
	EGGYGOGait PreviousGait = EGGYGOGait::None;
};

/** Bounded derived end state; request admission is deliberately not copied. */
struct FGGYGOLocomotionPreparedState
{
	UGGYGOCharacterMovementComponent::FLocomotionUpdateCandidate Candidate;
};

bool FGGYGOMovementInputSourceCheckpoint::operator==(const FGGYGOMovementInputSourceCheckpoint& Other) const
{
	return bPresent == Other.bPresent && bConsumerInvalidated == Other.bConsumerInvalidated
		&& BindingSerial == Other.BindingSerial && ConsumerFenceSerial == Other.ConsumerFenceSerial
		&& SessionSerial == Other.SessionSerial && SessionOpenedEventSerial == Other.SessionOpenedEventSerial
		&& EventSerial == Other.EventSerial && FactRequestSerial == Other.FactRequestSerial
		&& RequestSerial == Other.RequestSerial && RequestStartedEventSerial == Other.RequestStartedEventSerial
		&& StartReleaseRequestSerial == Other.StartReleaseRequestSerial
		&& StartReleaseEventSerial == Other.StartReleaseEventSerial
		&& StartNeutralEventSerial == Other.StartNeutralEventSerial
		&& RequestReleasedEventSerial == Other.RequestReleasedEventSerial
		&& NeutralEventSerial == Other.NeutralEventSerial
		&& SourceUnresolvedEventSerial == Other.SourceUnresolvedEventSerial
		&& LastFactKind == Other.LastFactKind && SessionMode == Other.SessionMode && StartProof == Other.StartProof;
}

bool FGGYGOMovementInputSourceCheckpoint::IsValid(FString* OutError) const
{
	if (OutError) OutError->Reset();
	const auto Reject = [OutError](const TCHAR* Reason)
	{
		if (OutError) *OutError = Reason;
		return false;
	};
	if (static_cast<uint8>(LastFactKind) > static_cast<uint8>(EGGYGOMovementInputFactKind::SourceUnresolved)
		|| static_cast<uint8>(SessionMode) > static_cast<uint8>(EGGYGOMovementInputSessionMode::Rearm)
		|| static_cast<uint8>(StartProof) > static_cast<uint8>(EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress))
	{
		return Reject(TEXT("source checkpoint enum is out of range"));
	}
	if (!bPresent)
	{
		return *this == FGGYGOMovementInputSourceCheckpoint{}
			? true : Reject(TEXT("absent source checkpoint carries non-empty values"));
	}
	if (BindingSerial == 0 || SessionSerial == 0 || ConsumerFenceSerial < BindingSerial
		|| (!bConsumerInvalidated && ConsumerFenceSerial != BindingSerial)
		|| (bConsumerInvalidated && ConsumerFenceSerial != (BindingSerial == MAX_uint64 ? BindingSerial : BindingSerial + 1)))
	{
		return Reject(TEXT("source binding/session or exact receiver retirement fence is invalid"));
	}
	if ((EventSerial == 0) != (LastFactKind == EGGYGOMovementInputFactKind::Invalid)
		|| (EventSerial == 0 && FactRequestSerial != 0))
	{
		return Reject(TEXT("latest original fact kind/event/request combination is invalid"));
	}
	const uint64 EventAnchors[] = {SessionOpenedEventSerial, RequestStartedEventSerial,
		StartReleaseEventSerial, StartNeutralEventSerial, RequestReleasedEventSerial,
		NeutralEventSerial, SourceUnresolvedEventSerial};
	for (uint64 Anchor : EventAnchors)
	{
		if (Anchor > EventSerial) return Reject(TEXT("source anchor is newer than the latest original fact"));
	}
	if (SessionOpenedEventSerial == 0)
	{
		if (SessionMode != EGGYGOMovementInputSessionMode::Invalid || RequestSerial != 0
			|| NeutralEventSerial != 0 || SourceUnresolvedEventSerial != 0
			|| (LastFactKind != EGGYGOMovementInputFactKind::Invalid
				&& LastFactKind != EGGYGOMovementInputFactKind::SessionInvalidated))
		{
			return Reject(TEXT("unopened binding carries opened-session facts"));
		}
	}
	else if (SessionMode == EGGYGOMovementInputSessionMode::Invalid
		|| (NeutralEventSerial != 0 && NeutralEventSerial <= SessionOpenedEventSerial)
		|| (SourceUnresolvedEventSerial != 0 && SourceUnresolvedEventSerial <= SessionOpenedEventSerial))
	{
		return Reject(TEXT("session mode or session-relative fact ordering is invalid"));
	}
	if (RequestSerial == 0)
	{
		if (RequestStartedEventSerial != 0 || StartProof != EGGYGOMovementInputStartProof::Invalid
			|| StartReleaseRequestSerial != 0 || StartReleaseEventSerial != 0
			|| StartNeutralEventSerial != 0 || RequestReleasedEventSerial != 0)
		{
			return Reject(TEXT("unstarted source carries request/start/release anchors"));
		}
	}
	else
	{
		if (SessionOpenedEventSerial == 0 || RequestStartedEventSerial <= SessionOpenedEventSerial
			|| StartProof == EGGYGOMovementInputStartProof::Invalid
			|| ((StartReleaseRequestSerial == 0) != (StartReleaseEventSerial == 0))
			|| (StartReleaseRequestSerial != 0 && (StartReleaseRequestSerial >= RequestSerial
				|| StartReleaseEventSerial <= SessionOpenedEventSerial))
			|| (StartNeutralEventSerial != 0 && (StartNeutralEventSerial <= SessionOpenedEventSerial
				|| StartNeutralEventSerial >= RequestStartedEventSerial))
			|| NeutralEventSerial < StartNeutralEventSerial
			|| (RequestReleasedEventSerial != 0 && RequestReleasedEventSerial <= RequestStartedEventSerial))
		{
			return Reject(TEXT("original request start/release/neutral anchors are inconsistent"));
		}
		if (StartProof == EGGYGOMovementInputStartProof::ColdPhysicalPress)
		{
			if (SessionMode != EGGYGOMovementInputSessionMode::Cold || StartReleaseRequestSerial != 0
				|| StartReleaseEventSerial != 0
				|| (SourceUnresolvedEventSerial != 0 && SourceUnresolvedEventSerial < RequestStartedEventSerial))
			{
				return Reject(TEXT("Cold start has no original Cold window or follows source revocation"));
			}
		}
		else if (StartNeutralEventSerial == 0
			|| (StartReleaseEventSerial != 0 && StartReleaseEventSerial >= StartNeutralEventSerial)
			|| (SourceUnresolvedEventSerial != 0 && SourceUnresolvedEventSerial < RequestStartedEventSerial
				&& SourceUnresolvedEventSerial >= StartNeutralEventSerial))
		{
			return Reject(TEXT("rearm start lacks its original ordered release/neutral basis"));
		}
	}
	switch (LastFactKind)
	{
	case EGGYGOMovementInputFactKind::Invalid:
		break;
	case EGGYGOMovementInputFactKind::SessionOpened:
		if (EventSerial != SessionOpenedEventSerial || FactRequestSerial != 0 || RequestSerial != 0)
			return Reject(TEXT("latest SessionOpened does not name its original opening"));
		break;
	case EGGYGOMovementInputFactKind::NeutralConfirmed:
		if (EventSerial != NeutralEventSerial || (RequestSerial != 0
			&& (RequestReleasedEventSerial == 0 || NeutralEventSerial <= RequestReleasedEventSerial)))
			return Reject(TEXT("latest neutral replaced an unreleased original request"));
		break;
	case EGGYGOMovementInputFactKind::RequestStarted:
		if (RequestSerial == 0 || EventSerial != RequestStartedEventSerial || FactRequestSerial != RequestSerial)
			return Reject(TEXT("latest start does not name its original request"));
		break;
	case EGGYGOMovementInputFactKind::RequestReleased:
		if (RequestSerial == 0 || EventSerial != RequestReleasedEventSerial || FactRequestSerial != RequestSerial)
			return Reject(TEXT("latest release does not name its original request"));
		break;
	case EGGYGOMovementInputFactKind::SessionInvalidated:
		if (!bConsumerInvalidated || (SessionOpenedEventSerial != 0 && EventSerial <= SessionOpenedEventSerial))
			return Reject(TEXT("original session invalidation has no receiver retirement"));
		break;
	case EGGYGOMovementInputFactKind::SourceUnresolved:
		if (EventSerial != SourceUnresolvedEventSerial || (FactRequestSerial != 0 && FactRequestSerial != RequestSerial))
			return Reject(TEXT("latest unresolved fact names a different original source"));
		break;
	default:
		return Reject(TEXT("unknown source fact"));
	}
	return true;
}

bool FGGYGOMovementInputSourceCheckpoint::Serialize(FArchive& Ar, FString* OutError)
{
	if (OutError) OutError->Reset();
	FGGYGOMovementInputSourceCheckpoint Wire = Ar.IsLoading() ? FGGYGOMovementInputSourceCheckpoint{} : *this;
	const auto Reject = [&Ar, &Wire, OutError](const TCHAR* Reason)
	{
		if (OutError) *OutError = FString::Printf(TEXT("Binding=%llu Session=%llu Event=%llu: %s"),
			static_cast<unsigned long long>(Wire.BindingSerial), static_cast<unsigned long long>(Wire.SessionSerial),
			static_cast<unsigned long long>(Wire.EventSerial), Reason);
		Ar.SetError();
		return false;
	};
	FString Error;
	if (Ar.IsSaving() && !Wire.IsValid(&Error)) return Reject(*Error);
	uint8 Version = WireVersion;
	uint8 Present = Wire.bPresent ? 1 : 0;
	Ar.SerializeBits(&Version, 8);
	Ar.SerializeBits(&Present, 1);
	if (Ar.IsError()) return Reject(TEXT("native archive failed reading/writing checkpoint header"));
	if (Version != WireVersion) return Reject(TEXT("unsupported source checkpoint wire version"));
	Wire.bPresent = Present != 0;
	if (Wire.bPresent)
	{
		uint64* const Serials[] = {&Wire.BindingSerial, &Wire.ConsumerFenceSerial, &Wire.SessionSerial,
			&Wire.SessionOpenedEventSerial, &Wire.EventSerial, &Wire.FactRequestSerial,
			&Wire.RequestSerial, &Wire.RequestStartedEventSerial, &Wire.StartReleaseRequestSerial,
			&Wire.StartReleaseEventSerial, &Wire.StartNeutralEventSerial, &Wire.RequestReleasedEventSerial,
			&Wire.NeutralEventSerial, &Wire.SourceUnresolvedEventSerial};
		static_assert(UE_ARRAY_COUNT(Serials) == SerialFieldCount);
		for (uint64* Serial : Serials) Ar.SerializeBits(Serial, 64);
		uint8 Invalidated = Wire.bConsumerInvalidated ? 1 : 0;
		uint8 Kind = static_cast<uint8>(Wire.LastFactKind);
		uint8 Mode = static_cast<uint8>(Wire.SessionMode);
		uint8 Proof = static_cast<uint8>(Wire.StartProof);
		Ar.SerializeBits(&Invalidated, 1);
		Ar.SerializeBits(&Kind, 3);
		Ar.SerializeBits(&Mode, 2);
		Ar.SerializeBits(&Proof, 2);
		Wire.bConsumerInvalidated = Invalidated != 0;
		Wire.LastFactKind = static_cast<EGGYGOMovementInputFactKind>(Kind);
		Wire.SessionMode = static_cast<EGGYGOMovementInputSessionMode>(Mode);
		Wire.StartProof = static_cast<EGGYGOMovementInputStartProof>(Proof);
	}
	if (Ar.IsError()) return Reject(TEXT("native archive failed reading/writing checkpoint body"));
	if (!Wire.IsValid(&Error)) return Reject(*Error);
	if (Ar.IsLoading()) *this = Wire;
	return true;
}

// ============================================================================
// FSavedMove_GGYGO
// ============================================================================

void FSavedMove_GGYGO::Clear()
{
	Super::Clear();
	SavedMovementOwnerSyncScope = {};
	SavedMovementOwnerGeneration = 0;
	SavedMovementInputSourceCheckpoint = {};
	SavedMovementInputRequest = {};
	SavedCurveRootMotionInput.Reset();
	SavedCurveRootMotionPrepared.Reset();
	SavedLocomotionSourceBinding.Reset();

	SavedGait = EGGYGOGait::None;
	NetworkGait = EGGYGOGait::None;
	bNetworkTurnBackCurveDriven = false;
	SavedWalkHoldTimer = 0.0f;
	bSavedForceWalkRequested = false;
	bSavedPreviousHasMoveInput = false;
	bSavedPreviousMovementBlocked = false;
	bSavedWantsRunOnNextMove = false;
	SavedLocomotionMotionType = EGGYGOLocomotionMotionType::None;
	SavedStopMotionType = EGGYGOStopMotionType::None;
	SavedLocomotionMotionTime = 0.0f;
	SavedWalkRunCyclePhase = 0.0f;
	SavedWalkRunBlendAlpha = 0.0f;
	SavedLocomotionMotionSequence = 0;
	NetworkLocomotionMotionType = EGGYGOLocomotionMotionType::None;
	NetworkStopMotionType = EGGYGOStopMotionType::None;
	NetworkTurnBackPhase = EGGYGOTurnBackPhase::None;
	NetworkLocomotionMotionSequence = 0;

	SavedTurnBackPhase = EGGYGOTurnBackPhase::None;
	SavedTurnBackElapsed = 0.0f;
	SavedTurnBackEntryYaw = 0.0f;
	bSavedTurnBackInputLatched = false;
}

void FSavedMove_GGYGO::SetMoveFor(ACharacter* C, float InDeltaTime, FVector const& NewAccel, FNetworkPredictionData_Client_Character& ClientData)
{
	Super::SetMoveFor(C, InDeltaTime, NewAccel, ClientData);
	SavedMovementOwnerSyncScope = {};
	SavedMovementOwnerGeneration = 0;
	SavedMovementInputSourceCheckpoint = {};
	SavedMovementInputRequest = {};
	SavedCurveRootMotionInput.Reset();
	SavedCurveRootMotionPrepared.Reset();
	SavedLocomotionSourceBinding.Reset();

	if (const UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr)
	{
		const auto& Context = MoveComp->MovementOwnerSyncContext;
		if (MoveComp->IsMovementOwnerSyncContextCurrent(Context))
		{
			// Original capture only. PostUpdate/PrepMoveFor/responses never retag this move.
			SavedMovementOwnerSyncScope = Context->Scope;
			SavedMovementOwnerGeneration = Context->Notice.GetServerOwnerGeneration();
		}
		SavedMovementInputSourceCheckpoint = MoveComp->MovementInputSourceCheckpoint;
		SavedLocomotionSourceBinding = MoveComp->LocomotionSourceBinding;
		SavedMovementInputRequest.Binding = MoveComp->MovementInputBinding;
		SavedMovementInputRequest.Request = MoveComp->MovementInputRequest;
		SavedMovementInputRequest.ExecutionRequestSerial = MoveComp->LocomotionRequestSerial;
		SavedMovementInputRequest.Admission = static_cast<uint8>(MoveComp->LocomotionRequestAdmission);
		SavedMovementInputRequest.bExecutionEligible = MoveComp->LocomotionRequestSerial != 0
			&& MoveComp->IsMovementInputBindingCurrent(MoveComp->MovementInputBinding)
			&& !MoveComp->IsMovementInputRequestBlocked();
		SavedGait = MoveComp->ResolvedGait;
		NetworkGait = SavedGait;
		bNetworkTurnBackCurveDriven = MoveComp->IsTurnBackCurveDriven();
		SavedWalkHoldTimer = MoveComp->WalkHoldTimer;
		bSavedForceWalkRequested = MoveComp->bForceWalkRequested;
		bSavedPreviousHasMoveInput = MoveComp->bPreviousHasMoveInput;
		bSavedPreviousMovementBlocked = MoveComp->bPreviousMovementBlocked;
		bSavedWantsRunOnNextMove = MoveComp->bWantsRunOnNextMove;
		SavedLocomotionMotionType = MoveComp->LocomotionMotionType;
		SavedStopMotionType = MoveComp->StopMotionType;
		SavedLocomotionMotionTime = MoveComp->LocomotionMotionTime;
		SavedWalkRunCyclePhase = MoveComp->WalkRunCyclePhase;
		SavedWalkRunBlendAlpha = MoveComp->WalkRunBlendAlpha;
		SavedLocomotionMotionSequence = MoveComp->LocomotionMotionSequence;
		NetworkLocomotionMotionType = SavedLocomotionMotionType;
		NetworkStopMotionType = SavedStopMotionType;
		NetworkTurnBackPhase = MoveComp->TurnBackPhase;
		NetworkLocomotionMotionSequence = SavedLocomotionMotionSequence;

		SavedTurnBackPhase = MoveComp->TurnBackPhase;
		SavedTurnBackElapsed = MoveComp->TurnBackElapsed;
		SavedTurnBackEntryYaw = MoveComp->TurnBackEntryYaw;
		bSavedTurnBackInputLatched = MoveComp->bTurnBackInputLatched;

		// 曲线位移源在场时禁止 move 合并。
		//
		// 合并会把角色拉回起点重演一次更长的 move，而曲线速度是逐帧变化的
		// （走路刹停从 459 一路衰减到 0），重演时只剩下最后一帧的曲线值，
		// 算出来的滑行距离与客户端实际走过的不同。
		//
		// 引擎自己的 `CanCombineWith` 只挡 anim montage 的 root motion，
		// 不挡 root motion source；而刹停期间两帧的 `Acceleration` 都是零，
		// 恰好落在引擎"允许合并"的分支里。所以必须自己挡。
		if (MoveComp->HasCurveRootMotionSource()
			|| (MoveComp->HasNativePassiveGroundVelocity()
				&& MoveComp->ProjectToGravityFloor(MoveComp->Velocity).SizeSquared() > KINDA_SMALL_NUMBER
				&& (MoveComp->ShouldRejectMovementInputGroundLocomotion()
					|| MoveComp->ShouldRejectUnownedCurveGroundLocomotion()
					|| MoveComp->ShouldRejectUnconfiguredGroundLocomotion())))
		{
			// Passive braking also keeps its native starting result; a combined rewind must
			// not substitute an earlier velocity under a later result's provenance.
			bForceNoCombine = true;
		}
	}
}

void FSavedMove_GGYGO::PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode)
{
	Super::PostUpdate(C, PostUpdateMode);
	// Native replay may update its own bookkeeping; it never replaces original request/input/result capture.
	if (PostUpdateMode == PostUpdate_Replay) return;
	if (const UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr)
	{
		if (SavedMovementInputRequest.Binding.ConsumerBindingSerial != 0
			&& (!SavedMovementInputRequest.bExecutionEligible
				|| SavedMovementInputRequest.ExecutionRequestSerial != MoveComp->LocomotionRequestSerial
				|| SavedMovementInputRequest.Binding != MoveComp->MovementInputBinding
				|| SavedMovementInputRequest.Request != MoveComp->MovementInputRequest)) return;
		SavedCurveRootMotionPrepared = MoveComp->LastLocomotionCurvePrepared;
		SavedCurveRootMotionInput = SavedCurveRootMotionPrepared.IsValid()
			? SavedCurveRootMotionPrepared->Input : MoveComp->PendingLocomotionCurveInput;
		bForceNoCombine |= SavedCurveRootMotionInput.IsValid();
		NetworkGait = MoveComp->ResolvedGait;
		bNetworkTurnBackCurveDriven = MoveComp->IsTurnBackCurveDriven();
		NetworkLocomotionMotionType = MoveComp->LocomotionMotionType;
		NetworkStopMotionType = MoveComp->StopMotionType;
		NetworkTurnBackPhase = MoveComp->TurnBackPhase;
		NetworkLocomotionMotionSequence = MoveComp->LocomotionMotionSequence;
		// 升档/转身入口帧的结果必须按本次 move 发送，不能推迟到下一帧。
		bForceNoCombine |= NetworkGait != SavedGait
			|| bNetworkTurnBackCurveDriven
			|| NetworkLocomotionMotionSequence != SavedLocomotionMotionSequence;
	}
}

void FSavedMove_GGYGO::PrepMoveFor(ACharacter* C)
{
	UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr;
	if (MoveComp)
	{
		FString Error;
		if (!MoveComp->BeginLocomotionCurveReplay(*this, Error))
		{
			UE_LOG(LogGGYGOMovement, Error, TEXT("Movement SavedMove rejected: CMC='%s', Reason='%s'."),
				*MoveComp->GetPathName(), *Error);
		}
		const auto HasSavedIndependentSource = [](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
		{
			return Sources.ContainsByPredicate([](const TSharedPtr<FRootMotionSource>& Source)
			{
				return Source.IsValid() && IsRegisteredActionCurveSource(*Source);
			});
		};
		if (MoveComp->IsMovingOnGround() && MoveComp->IsMovementInputRequestBlocked()
			&& !MoveComp->HasIndependentGroundRootMotion() && !RootMotionMontage.IsValid()
			&& !HasSavedIndependentSource(SavedRootMotion.RootMotionSources)
			&& !HasSavedIndependentSource(SavedRootMotion.PendingAddRootMotionSources))
		{
			// A rejected original interval must not install its group over a successor's resources.
			MoveComp->FinishLocomotionCurveReplayPreparation();
			return;
		}
		// 回放这一帧之前把状态还原到当时的样子。
		// 不还原计时器的话，回放多帧时计时器会从"现在"的值继续累加，
		// 于是回放中途可能升档，而首次执行时并没有 —— 预测就失配了。
		// 普通预测回放恢复本 move 的起始状态。服务器校正后的重放则必须从
		// response 带回的权威时间连续推进，不能再覆盖成旧的预测时钟。
		if (!MoveComp->bReplayLocomotionFromAuthority && !MoveComp->IsMovementInputRequestBlocked())
		{
			MoveComp->ResolvedGait = SavedGait;
			MoveComp->WalkHoldTimer = SavedWalkHoldTimer;
			MoveComp->bForceWalkRequested = bSavedForceWalkRequested;
			MoveComp->bPreviousHasMoveInput = bSavedPreviousHasMoveInput;
			MoveComp->bPreviousMovementBlocked = bSavedPreviousMovementBlocked;
			MoveComp->bWantsRunOnNextMove = bSavedWantsRunOnNextMove;
			MoveComp->LocomotionMotionType = SavedLocomotionMotionType;
			MoveComp->StopMotionType = SavedStopMotionType;
			MoveComp->LocomotionMotionTime = SavedLocomotionMotionTime;
			MoveComp->WalkRunCyclePhase = SavedWalkRunCyclePhase;
			MoveComp->WalkRunBlendAlpha = SavedWalkRunBlendAlpha;
			MoveComp->LocomotionMotionSequence = SavedLocomotionMotionSequence;
			MoveComp->TurnBackPhase = SavedTurnBackPhase;
			MoveComp->TurnBackElapsed = SavedTurnBackElapsed;
			MoveComp->TurnBackEntryYaw = SavedTurnBackEntryYaw;
			MoveComp->bTurnBackInputLatched = bSavedTurnBackInputLatched;
		}
	}
	// Native Super can already Prepare SavedRootMotion; its original input is installed above.
	Super::PrepMoveFor(C);
	if (MoveComp) MoveComp->FinishLocomotionCurveReplayPreparation();
}

bool FSavedMove_GGYGO::CanCombineWith(const FSavedMovePtr& NewMove, ACharacter* InCharacter, float MaxDelta) const
{
	const FSavedMove_GGYGO* NewGGYGOMove = static_cast<const FSavedMove_GGYGO*>(NewMove.Get());
	if (NewGGYGOMove && NewGGYGOMove->SavedLocomotionSourceBinding != SavedLocomotionSourceBinding) return false;
	if (NewGGYGOMove && (NewGGYGOMove->SavedMovementOwnerSyncScope != SavedMovementOwnerSyncScope
		|| NewGGYGOMove->SavedMovementOwnerGeneration != SavedMovementOwnerGeneration)) return false;
	if (NewGGYGOMove && NewGGYGOMove->SavedMovementInputSourceCheckpoint != SavedMovementInputSourceCheckpoint)
	{
		return false;
	}
	if (NewGGYGOMove && (NewGGYGOMove->SavedMovementInputRequest.Binding != SavedMovementInputRequest.Binding
		|| NewGGYGOMove->SavedMovementInputRequest.Request != SavedMovementInputRequest.Request
		|| NewGGYGOMove->SavedMovementInputRequest.ExecutionRequestSerial != SavedMovementInputRequest.ExecutionRequestSerial
		|| NewGGYGOMove->SavedMovementInputRequest.Admission != SavedMovementInputRequest.Admission
		|| NewGGYGOMove->SavedMovementInputRequest.bExecutionEligible != SavedMovementInputRequest.bExecutionEligible)) return false;

	// 步态不同不能合并：合并后服务器只会看到一个步态值，
	// 另一帧就会按错误的速度上限重演。
	if (NewGGYGOMove && NewGGYGOMove->SavedGait != SavedGait)
	{
		return false;
	}
	if (NewGGYGOMove && (NewGGYGOMove->bSavedForceWalkRequested != bSavedForceWalkRequested
		|| NewGGYGOMove->bSavedPreviousHasMoveInput != bSavedPreviousHasMoveInput
		|| NewGGYGOMove->bSavedPreviousMovementBlocked != bSavedPreviousMovementBlocked))
	{
		return false;
	}

	// 契约状态不同同样不能合并 —— 它会改变下一帧的步态解算结果。
	if (NewGGYGOMove && NewGGYGOMove->bSavedWantsRunOnNextMove != bSavedWantsRunOnNextMove)
	{
		return false;
	}

	// 转身期间不合并。这段的位移方向由曲线逐帧给出，合并会丢掉中间帧的方向变化，
	// 服务器重演出的轨迹与客户端不同。
	if (NewGGYGOMove && NewGGYGOMove->SavedTurnBackPhase != SavedTurnBackPhase)
	{
		return false;
	}

	if (SavedTurnBackPhase != EGGYGOTurnBackPhase::None)
	{
		return false;
	}

	if (NewGGYGOMove && (NewGGYGOMove->SavedLocomotionMotionType != SavedLocomotionMotionType
		|| NewGGYGOMove->SavedStopMotionType != SavedStopMotionType
		|| NewGGYGOMove->SavedLocomotionMotionSequence != SavedLocomotionMotionSequence))
	{
		return false;
	}

	return Super::CanCombineWith(NewMove, InCharacter, MaxDelta);
}

bool FSavedMove_GGYGO::IsImportantMove(const FSavedMovePtr& LastAckedMovePtr) const
{
	if (!LastAckedMovePtr.IsValid()) return true;
	const FSavedMove_GGYGO& LastAcked = static_cast<const FSavedMove_GGYGO&>(*LastAckedMovePtr);
	return SavedMovementInputSourceCheckpoint != LastAcked.SavedMovementInputSourceCheckpoint
		|| SavedMovementOwnerSyncScope != LastAcked.SavedMovementOwnerSyncScope
		|| SavedMovementOwnerGeneration != LastAcked.SavedMovementOwnerGeneration
		|| SavedMovementInputRequest.Binding != LastAcked.SavedMovementInputRequest.Binding
		|| SavedMovementInputRequest.Request != LastAcked.SavedMovementInputRequest.Request
		|| SavedMovementInputRequest.ExecutionRequestSerial != LastAcked.SavedMovementInputRequest.ExecutionRequestSerial
		|| SavedMovementInputRequest.Admission != LastAcked.SavedMovementInputRequest.Admission
		|| SavedMovementInputRequest.bExecutionEligible != LastAcked.SavedMovementInputRequest.bExecutionEligible
		|| Super::IsImportantMove(LastAckedMovePtr);
}

uint8 FSavedMove_GGYGO::GetCompressedFlags() const
{
	uint8 Result = Super::GetCompressedFlags();

	if (bSavedForceWalkRequested)
	{
		Result |= GGYGOMovementConstants::ForceWalkFlag;
	}

	return Result;
}

// ============================================================================
// FNetworkPredictionData_Client_GGYGO
// ============================================================================

FNetworkPredictionData_Client_GGYGO::FNetworkPredictionData_Client_GGYGO(const UCharacterMovementComponent& ClientMovement)
	: Super(ClientMovement)
{
}

FSavedMovePtr FNetworkPredictionData_Client_GGYGO::AllocateNewMove()
{
	return FSavedMovePtr(new FSavedMove_GGYGO());
}

// ============================================================================
// Packed movement data
// ============================================================================

void FCharacterNetworkMoveData_GGYGO::ClientFillNetworkMoveData(
	const FSavedMove_Character& ClientMove,
	ENetworkMoveType MoveType)
{
	FCharacterNetworkMoveData::ClientFillNetworkMoveData(ClientMove, MoveType);
	const FSavedMove_GGYGO& GGYGOMove = static_cast<const FSavedMove_GGYGO&>(ClientMove);
	// Issued capture remains original even when its weak endpoint later expires.
	bHasMovementOwnerSync = GGYGOMove.SavedMovementOwnerSyncScope.GetScopeSerial() != 0
		&& GGYGOMove.SavedMovementOwnerSyncScope.GetResponseNonce() != 0;
	MovementOwnerSyncNonce = bHasMovementOwnerSync ? GGYGOMove.SavedMovementOwnerSyncScope.GetResponseNonce() : 0;
	MovementOwnerGeneration = bHasMovementOwnerSync ? GGYGOMove.SavedMovementOwnerGeneration : 0;
	const FGGYGOMovementInputSourceCheckpoint& Captured = GGYGOMove.SavedMovementInputSourceCheckpoint;
	const bool bOriginalTerminal = Captured.bPresent && (Captured.bConsumerInvalidated
		|| Captured.RequestReleasedEventSerial != 0
		|| (Captured.SourceUnresolvedEventSerial != 0
			&& Captured.SourceUnresolvedEventSerial > Captured.RequestStartedEventSerial));
	// Absence is the existing no-source wire mode. Preserve the full local capture, but do not
	// export an unadmitted start as execution provenance. Real terminal facts may still retire it.
	MovementInputSourceCheckpoint = GGYGOMove.SavedMovementInputRequest.bExecutionEligible || bOriginalTerminal
		? Captured : FGGYGOMovementInputSourceCheckpoint{};
	LocomotionMotionType = GGYGOMove.NetworkLocomotionMotionType;
	StopMotionType = GGYGOMove.NetworkStopMotionType;
	TurnBackPhase = GGYGOMove.NetworkTurnBackPhase;
	LocomotionMotionSequence = GGYGOMove.NetworkLocomotionMotionSequence;
}

bool FCharacterNetworkMoveData_GGYGO::Serialize(
	UCharacterMovementComponent& CharacterMovement,
	FArchive& Ar,
	UPackageMap* PackageMap,
	ENetworkMoveType MoveType)
{
	const bool bParentSuccess = FCharacterNetworkMoveData::Serialize(CharacterMovement, Ar, PackageMap, MoveType);
	if (!bParentSuccess || Ar.IsError()) return false;
	if (!GGYGOMovementOwnerSync::SerializeMove(Ar, bHasMovementOwnerSync, MovementOwnerSyncNonce, MovementOwnerGeneration))
	{
		if (auto* CMC = Cast<UGGYGOCharacterMovementComponent>(&CharacterMovement))
			CMC->ReportMovementOwnerSyncOnce(FName(TEXT("MoveWireInvalid")), TEXT("Move owner-sync version/nonce encoding rejected."));
		return false;
	}
	FString SourceError;
	if (!MovementInputSourceCheckpoint.Serialize(Ar, &SourceError))
	{
		UE_LOG(LogGGYGOMovement, Error, TEXT("Movement source checkpoint serialization rejected: CMC='%s', Reason='%s'."),
			*CharacterMovement.GetPathName(), *SourceError);
		return false;
	}
	uint8 MotionValue = static_cast<uint8>(LocomotionMotionType);
	uint8 StopValue = static_cast<uint8>(StopMotionType);
	uint8 TurnBackValue = static_cast<uint8>(TurnBackPhase);
	Ar.SerializeBits(&MotionValue, 3);
	Ar.SerializeBits(&StopValue, 2);
	Ar.SerializeBits(&TurnBackValue, 2);
	Ar.SerializeBits(&LocomotionMotionSequence, 16);

	if (Ar.IsLoading())
	{
		LocomotionMotionType = MotionValue <= static_cast<uint8>(EGGYGOLocomotionMotionType::TurnBack)
			? static_cast<EGGYGOLocomotionMotionType>(MotionValue)
			: EGGYGOLocomotionMotionType::None;
		StopMotionType = StopValue <= static_cast<uint8>(EGGYGOStopMotionType::RunStop)
			? static_cast<EGGYGOStopMotionType>(StopValue)
			: EGGYGOStopMotionType::None;
		TurnBackPhase = TurnBackValue <= static_cast<uint8>(EGGYGOTurnBackPhase::RunOut)
			? static_cast<EGGYGOTurnBackPhase>(TurnBackValue)
			: EGGYGOTurnBackPhase::None;
	}

	return bParentSuccess && !Ar.IsError();
}

FCharacterNetworkMoveDataContainer_GGYGO::FCharacterNetworkMoveDataContainer_GGYGO()
{
	NewMoveData = &MoveData[0];
	PendingMoveData = &MoveData[1];
	OldMoveData = &MoveData[2];
}

void FCharacterMoveResponseDataContainer_GGYGO::ServerFillResponseData(
	const UCharacterMovementComponent& CharacterMovement,
	const FClientAdjustment& PendingAdjustment)
{
	FCharacterMoveResponseDataContainer::ServerFillResponseData(CharacterMovement, PendingAdjustment);
	bHasMovementOwnerSync = false;
	MovementOwnerSyncNonce = 0;
	ServerOwnerGeneration = 0;
	AdjustmentOwnerGeneration = 0;
	bMovementOwnerActive = false;
	bInitialSynchronizationEligible = false;
	if (const UGGYGOCharacterMovementComponent* MoveComp = Cast<UGGYGOCharacterMovementComponent>(&CharacterMovement))
	{
		const auto& Receipt = MoveComp->MovementOwnerSyncPendingReceipt;
		if (MoveComp->IsMovementOwnerSyncReceiptCurrent(Receipt) && Receipt.TimeStamp == PendingAdjustment.TimeStamp)
		{
			bHasMovementOwnerSync = true;
			MovementOwnerSyncNonce = Receipt.Nonce;
			ServerOwnerGeneration = Receipt.OwnerGeneration;
			// A previously known generation stays original even when native ownership changed.
			// Unknown first-sync moves use the server's original native receipt, not a rewritten SavedMove.
			AdjustmentOwnerGeneration = Receipt.ClientOwnerGeneration != 0
				? Receipt.ClientOwnerGeneration : Receipt.OwnerGeneration;
			bMovementOwnerActive = true;
			bInitialSynchronizationEligible = Receipt.bInitialOwnerGeneration;
		}
		LocomotionMotionType = MoveComp->LocomotionMotionType;
		StopMotionType = MoveComp->StopMotionType;
		TurnBackPhase = MoveComp->TurnBackPhase;
		Gait = MoveComp->ResolvedGait;
		LocomotionMotionTime = MoveComp->LocomotionMotionTime;
		WalkRunCyclePhase = MoveComp->WalkRunCyclePhase;
		WalkRunBlendAlpha = MoveComp->WalkRunBlendAlpha;
		WalkHoldTimer = MoveComp->WalkHoldTimer;
		TurnBackElapsed = MoveComp->TurnBackElapsed;
		TurnBackEntryYaw = MoveComp->TurnBackEntryYaw;
		LocomotionMotionSequence = MoveComp->LocomotionMotionSequence;
		bForceWalkRequested = MoveComp->bForceWalkRequested;
		bPreviousHasMoveInput = MoveComp->bPreviousHasMoveInput;
		bPreviousMovementBlocked = MoveComp->bPreviousMovementBlocked;
		bWantsRunOnNextMove = MoveComp->bWantsRunOnNextMove;
		bTurnBackInputLatched = MoveComp->bTurnBackInputLatched;
	}
}

bool FCharacterMoveResponseDataContainer_GGYGO::Serialize(
	UCharacterMovementComponent& CharacterMovement,
	FArchive& Ar,
	UPackageMap* PackageMap)
{
	const bool bParentSuccess = FCharacterMoveResponseDataContainer::Serialize(CharacterMovement, Ar, PackageMap);
	if (!bParentSuccess || Ar.IsError()) return false;
	// Good ACKs carry the same owner receipt; this must not be inside IsCorrection().
	if (!GGYGOMovementOwnerSync::SerializeResponse(Ar, *this))
	{
		if (auto* CMC = Cast<UGGYGOCharacterMovementComponent>(&CharacterMovement))
			CMC->ReportMovementOwnerSyncOnce(FName(TEXT("ResponseWireInvalid")), TEXT("Response owner-sync version/receipt encoding rejected."));
		return false;
	}
	if (IsCorrection())
	{
		uint8 MotionValue = static_cast<uint8>(LocomotionMotionType);
		uint8 StopValue = static_cast<uint8>(StopMotionType);
		uint8 TurnBackValue = static_cast<uint8>(TurnBackPhase);
		uint8 GaitValue = static_cast<uint8>(Gait);
		Ar.SerializeBits(&MotionValue, 3);
		Ar.SerializeBits(&StopValue, 2);
		Ar.SerializeBits(&TurnBackValue, 2);
		Ar.SerializeBits(&GaitValue, 2);
		Ar << LocomotionMotionTime;
		Ar << WalkRunCyclePhase;
		Ar << WalkRunBlendAlpha;
		Ar << WalkHoldTimer;
		Ar << TurnBackElapsed;
		Ar << TurnBackEntryYaw;
		Ar.SerializeBits(&LocomotionMotionSequence, 16);
		Ar.SerializeBits(&bForceWalkRequested, 1);
		Ar.SerializeBits(&bPreviousHasMoveInput, 1);
		Ar.SerializeBits(&bPreviousMovementBlocked, 1);
		Ar.SerializeBits(&bWantsRunOnNextMove, 1);
		Ar.SerializeBits(&bTurnBackInputLatched, 1);

		if (Ar.IsLoading())
		{
			LocomotionMotionType = MotionValue <= static_cast<uint8>(EGGYGOLocomotionMotionType::TurnBack)
				? static_cast<EGGYGOLocomotionMotionType>(MotionValue)
				: EGGYGOLocomotionMotionType::None;
			StopMotionType = StopValue <= static_cast<uint8>(EGGYGOStopMotionType::RunStop)
				? static_cast<EGGYGOStopMotionType>(StopValue)
				: EGGYGOStopMotionType::None;
			TurnBackPhase = TurnBackValue <= static_cast<uint8>(EGGYGOTurnBackPhase::RunOut)
				? static_cast<EGGYGOTurnBackPhase>(TurnBackValue)
				: EGGYGOTurnBackPhase::None;
			Gait = GaitValue <= static_cast<uint8>(EGGYGOGait::Run)
				? static_cast<EGGYGOGait>(GaitValue)
				: EGGYGOGait::None;
		}
	}

	return bParentSuccess && !Ar.IsError();
}

// ============================================================================
// UGGYGOCharacterMovementComponent
// ============================================================================

UGGYGOCharacterMovementComponent::UGGYGOCharacterMovementComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SetNetworkMoveDataContainer(NetworkMoveDataContainer);
	SetMoveResponseDataContainer(MoveResponseDataContainer);
	// 动作游戏默认让角色面向移动方向，锁定目标时由能力临时关掉。
	// 这里给的是兜底值，有 MovementSet 时会被覆盖。
	bOrientRotationToMovement = true;
	bUseControllerDesiredRotation = false;
	RotationRate = FRotator(0.0f, 720.0f, 0.0f);

	// CMC 默认不复制自己的属性（移动状态走专门的 RPC 通道）。
	// 这里需要属性复制，因为转身状态要发给模拟代理，而那条信息
	// 不属于移动预测数据。
	SetIsReplicatedByDefault(true);
}

void UGGYGOCharacterMovementComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// SkipOwner：拥有者自己是权威解算方，收到服务器的回传只会与本地预测打架。
	DOREPLIFETIME_CONDITION(UGGYGOCharacterMovementComponent, bReplicatedTurnBackCurveDriven, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UGGYGOCharacterMovementComponent, ResolvedGait, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UGGYGOCharacterMovementComponent, LocomotionMotionType, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UGGYGOCharacterMovementComponent, StopMotionType, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(UGGYGOCharacterMovementComponent, WalkRunBlendAlpha, COND_SkipOwner);
}

bool UGGYGOCharacterMovementComponent::EnsureMovementOwnerSyncLifetime()
{
	if (!IsInGameThread() || bMovementOwnerSyncClosed || !IsValid(this) || IsBeingDestroyed()
		|| !IsValid(GetOwner()) || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return false;
	if (MovementOwnerSyncLifetimeSerial != 0) return true;
	MovementOwnerSyncLifetimeSerial = GGYGOMovementOwnerSync::IssueIdentity();
	if (MovementOwnerSyncLifetimeSerial != 0) return true;
	bMovementOwnerSyncClosed = true;
	ReportMovementOwnerSyncOnce(FName(TEXT("ConsumerLifetimeExhausted")),
		TEXT("M2 consumer lifetime could not be issued; native owner synchronization is closed."));
	return false;
}

void UGGYGOCharacterMovementComponent::StopMovementImmediately()
{
	ResetLocomotionSteeringObservation();
	NativeMovementVelocityResult.Reset();
	NativeVelocityBeforeRootMotion.Reset();
	bNativeVelocityIntervalCanRetain = false;
	Super::StopMovementImmediately();
}

void UGGYGOCharacterMovementComponent::SetUpdatedComponent(USceneComponent* NewUpdatedComponent)
{
	USceneComponent* OriginalComponent = UpdatedComponent;
	Super::SetUpdatedComponent(NewUpdatedComponent);
	if (UpdatedComponent != OriginalComponent)
	{
		ResetLocomotionSteeringObservation();
		NativeMovementVelocityResult.Reset();
		NativeVelocityBeforeRootMotion.Reset();
		bNativeVelocityIntervalOpen = false;
		bNativeVelocityIntervalCanRetain = false;
		NativeVelocityIntervalCharacter.Reset();
		NativeVelocityIntervalComponent.Reset();
	}
}

void UGGYGOCharacterMovementComponent::BeginPlay()
{
	Super::BeginPlay();
	EnsureMovementOwnerSyncLifetime(); // A valid early binding already owns this same lifetime and scope.
	CaptureComponentDefaults();

	CacheAbilitySystemComponent();
	RefreshMovementOwnerSyncContext();
}

void UGGYGOCharacterMovementComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ResetLocomotionSteeringObservation();
	bMovementOwnerSyncClosed = true;
	NativeMovementVelocityResult.Reset();
	NativeVelocityBeforeRootMotion.Reset();
	NativeVelocityIntervalCharacter.Reset();
	NativeVelocityIntervalComponent.Reset();
	bNativeVelocityIntervalOpen = false;
	bNativeVelocityIntervalCanRetain = false;
	ActiveMovementOwnerSyncNativeMove = nullptr;
	// Terminal frames already own retired scopes; cancel their remaining callbacks.
	// Current Ready subscriptions remain for the EndPlay invalidation below.
	for (FMovementOwnerSyncDispatchFrame* Frame = ActiveMovementOwnerSyncDispatch; Frame; Frame = Frame->Previous)
	{
		Frame->ClosePendingRecords();
	}
	ActiveMovementOwnerSyncDispatch = nullptr;
	if (APawn* Pawn = MovementOwnerObservedPawn.Get())
	{
		Pawn->ReceiveControllerChangedDelegate.RemoveDynamic(this,
			&UGGYGOCharacterMovementComponent::HandleMovementOwnerControllerChanged);
	}
	MovementOwnerObservedPawn.Reset();
	RetireServerMovementOwner(FName(TEXT("ConsumerEndPlay")));
	RetireLocomotionCurveRootMotion();
	CompletedLocomotionCurveOrigin.Reset();
	LastLocomotionCurvePrepared.Reset();
	ConsumedLocomotionCurvePrepared.Reset();
	LocomotionCurveReplayEntryState.Reset();
	EndLocomotionCurveReplay();
	LocomotionSourceBinding.Reset();
	LastLocomotionSourceProducer.Reset();
	LastLocomotionSourceConfigurationGeneration = 0;
	ReleaseLocalAbilitySystemSubscription();
	NeutralizeMontageActionSource(ActiveMontageActionResource);
	NeutralizeMontageActionSource(CompletedMontageActionResource);
	ActiveMontageActionResource.Reset();
	CompletedMontageActionResource.Reset();
	bCompletedMontageActionNativeContributionConsumed = false;
	const auto RetiredActionFailureCallback = MoveTemp(ActionMotionFailureCallback);
	ActionMotionFailureResource.Reset();
	const auto RetiredActionCompletionCallback = MoveTemp(ActionMotionCompletionCallback);
	ActionMotionCompletionResource.Reset();
	bMontageActionCompletionObserverInstalled = false;
	MontageActionNaturalBlendOutResource.Reset();
	ActiveActionMotionHandle = INDEX_NONE;
	ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
	ActionSkippedMovementTickTime = 0.0f;

	if (bMovementInputBindingActive)
	{
		FString Error;
		InvalidateMovementInputSession(MovementInputBinding, FName(TEXT("ConsumerEndPlay")), Error);
	}
	// Existing execution resources are sealed before external invalidation callbacks.
	RetireMovementOwnerSyncScope(FName(TEXT("ConsumerEndPlay")));
	Super::EndPlay(EndPlayReason);
}

void UGGYGOCharacterMovementComponent::ReportMovementOwnerSyncOnce(FName Reason, const FString& Detail)
{
	if (MovementOwnerSyncReportedReasons.Contains(Reason)) return;
	MovementOwnerSyncReportedReasons.Add(Reason);
	UE_LOG(LogGGYGOMovement, Warning, TEXT("[Movement.OwnerSync] CMC='%s', Pawn='%s', Reason='%s': %s"),
		*GetPathName(), *GetPathNameSafe(CharacterOwner), *Reason.ToString(), *Detail);
}

bool UGGYGOCharacterMovementComponent::IsMovementOwnerSyncContextCurrent(
	const TSharedPtr<FMovementOwnerSyncContext>& Context) const
{
	if (bMovementOwnerSyncClosed || !Context.IsValid() || Context != MovementOwnerSyncContext
		|| Context->bRetired || !Context->Scope.IsSet()
		|| Context->Scope.Consumer.Get() != this
		|| Context->Scope.ConsumerLifetimeSerial != MovementOwnerSyncLifetimeSerial) return false;
	APawn* Pawn = Context->Scope.OriginalPawn.Get();
	APlayerController* PC = Context->Scope.OriginalPlayerController.Get();
	if (!IsValid(Pawn) || !IsValid(PC) || Pawn != CharacterOwner
		|| Pawn->GetController() != PC || Pawn->GetNetDriver() != Context->NetDriver.Get()
		|| (!Pawn->HasAuthority() && (!Pawn->IsLocallyControlled()
			|| Pawn->GetLocalRole() != ROLE_AutonomousProxy))) return false;
	if (Context->bOwnerPairCaptured && PC->GetPawn() != Pawn) return false;
	UNetConnection* Connection = GGYGOMovementOwnerSync::GetConnection(Pawn, PC);
	if (Context->bConnectionCaptured
		&& (Connection != Context->Connection.Get()
			|| !GGYGOMovementOwnerSync::IsConnectionLive(Connection))) return false;
	if (Context->bRequiresNativeResponse && !IsValid(Context->NetDriver.Get())) return false;
	if (!Context->NetDriver.IsExplicitlyNull() && !Context->NetDriver.IsValid()) return false;
	// Only the very first physical binding may claim an as-yet unbound initial scope.
	const bool bFirstBinding = Context->ObservedBindingSerial == 0
		&& MovementInputBindingSerial == 1 && bMovementInputBindingActive;
	if (Context->ObservedBindingSerial != MovementInputBindingSerial && !bFirstBinding) return false;
	if (Context->bObservedBindingActive != bMovementInputBindingActive && !bFirstBinding) return false;
	if (Context->Notice.State == EGGYGOMovementOwnerSyncState::Ready)
	{
		if (PC->GetPawn() != Pawn) return false;
		if (Context->bRequiresNativeResponse && PC->AcknowledgedPawn != Pawn) return false;
		if (Pawn->HasAuthority() && (!bServerMovementOwnerActive
			|| Context->Notice.ServerOwnerGeneration != ServerMovementOwnerGeneration)) return false;
	}
	return true;
}

bool UGGYGOCharacterMovementComponent::GetMovementOwnerSyncScope(
	FGGYGOMovementOwnerSyncScopeId& OutScope, FString& OutError) const
{
	OutError.Reset();
	if (!IsInGameThread() || !IsMovementOwnerSyncContextCurrent(MovementOwnerSyncContext))
	{
		OutError = TEXT("Movement.OwnerSync: no current native owner scope; original owner/channel must be ready for observation.");
		return false;
	}
	OutScope = MovementOwnerSyncContext->Scope;
	return true;
}

bool UGGYGOCharacterMovementComponent::SubscribeMovementOwnerSync(
	const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FGGYGOMovementOwnerSyncDelegate Observer,
	FGGYGOMovementOwnerSyncObserverId& OutObserver, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Movement.OwnerSync: subscribe requires the game thread.");
		return false;
	}
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (OutObserver.IsSet() || !Observer.IsBound()
		|| !IsMovementOwnerSyncContextCurrent(Context) || OriginalScope != Context->Scope
		|| MovementOwnerSyncLastObserverSerial == MAX_uint64)
	{
		OutError = TEXT("Movement.OwnerSync: subscribe rejected: stale scope, occupied output, unbound observer or exhausted serial.");
		return false;
	}
	const TSharedPtr<FMovementOwnerSyncObserverRecord> Record = MakeShared<FMovementOwnerSyncObserverRecord>();
	Record->Id.Scope = OriginalScope;
	Record->Id.ObserverSerial = ++MovementOwnerSyncLastObserverSerial;
	Record->Callback = MoveTemp(Observer);
	Context->Observers.Add(Record->Id.ObserverSerial, Record);
	OutObserver = Record->Id;
	// Copy before calling out: the receiver can unsubscribe or retire/destroy this scope.
	const FGGYGOMovementOwnerSyncNotice Notice = Context->Notice;
	const FGGYGOMovementOwnerSyncObserverId Id = Record->Id;
	const FGGYGOMovementOwnerSyncDelegate Callback = Record->Callback;
	Callback.Execute(Id, Notice);
	return true; // Historical installation; no output/self writes after replay.
}

UGGYGOCharacterMovementComponent::FMovementOwnerSyncDispatchFrame::FMovementOwnerSyncDispatchFrame(
	UGGYGOCharacterMovementComponent* InOwner,
	const TSharedPtr<FMovementOwnerSyncContext>& InContext, bool bInTerminal)
	: Owner(InOwner), Context(InContext), Previous(InOwner->ActiveMovementOwnerSyncDispatch),
	  OwnerLifetimeSerial(InOwner->MovementOwnerSyncLifetimeSerial), bTerminal(bInTerminal)
{
	Context->Observers.GenerateValueArray(Records);
	InOwner->ActiveMovementOwnerSyncDispatch = this;
}

void UGGYGOCharacterMovementComponent::FMovementOwnerSyncDispatchFrame::ClosePendingRecords()
{
	if (!bTerminal || bClosing) return;
	bClosing = true;
	for (const auto& Entry : Context->Observers) Entry.Value->bClosed = true;
	// Records retains every snapshot record; removing the map cannot release captures.
	Context->Observers.Reset();
}

UGGYGOCharacterMovementComponent::FMovementOwnerSyncDispatchFrame::~FMovementOwnerSyncDispatchFrame()
{
	ClosePendingRecords();
	// Seal and unlink before user capture destructors can reenter. No CMC member tail
	// follows Unbind/Reset; nested publications then see only live outer frames.
	if (UGGYGOCharacterMovementComponent* Self = Owner.Get())
	{
		if (Self->MovementOwnerSyncLifetimeSerial == OwnerLifetimeSerial
			&& Self->ActiveMovementOwnerSyncDispatch == this)
			Self->ActiveMovementOwnerSyncDispatch = Self->bMovementOwnerSyncClosed ? nullptr : Previous;
	}
	if (bTerminal)
	{
		for (const auto& Record : Records) Record->Callback.Unbind();
	}
	Records.Reset();
	Context.Reset();
}

bool UGGYGOCharacterMovementComponent::UnsubscribeMovementOwnerSync(
	const FGGYGOMovementOwnerSyncObserverId& OriginalObserver, FName Reason, FString& OutError)
{
	OutError.Reset();
	const FGGYGOMovementOwnerSyncScopeId& Scope = OriginalObserver.Scope;
	if (!IsInGameThread() || !OriginalObserver.IsSet() || Scope.Consumer.Get() != this
		|| Scope.ConsumerLifetimeSerial != MovementOwnerSyncLifetimeSerial
		|| OriginalObserver.ObserverSerial > MovementOwnerSyncLastObserverSerial)
	{
		OutError = FString::Printf(TEXT("Movement.OwnerSync: foreign/unissued observer (%s)."), *Reason.ToString());
		return false;
	}
	TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!Context.IsValid() || Context->Scope != Scope)
	{
		// Only in-flight original resources are searchable; no retired history cache.
		for (FMovementOwnerSyncDispatchFrame* Frame = ActiveMovementOwnerSyncDispatch; Frame; Frame = Frame->Previous)
		{
			if (Frame->Context.IsValid() && Frame->Context->Scope == Scope)
			{
				Context = Frame->Context;
				break;
			}
		}
	}
	// Already closed IDs remain idempotent without touching any successor.
	if (!Context.IsValid() || Context->Scope != Scope) return true;
	if (const TSharedPtr<FMovementOwnerSyncObserverRecord>* Found = Context->Observers.Find(OriginalObserver.ObserverSerial))
	{
		const TSharedPtr<FMovementOwnerSyncObserverRecord> Record = *Found;
		if (Record->Id.Scope != Scope) return false;
		Record->bClosed = true;
		Context->Observers.Remove(OriginalObserver.ObserverSerial);
		Record->Callback.Unbind(); // Capture destruction may reenter; no member/output writes follow.
	}
	return true;
}

void UGGYGOCharacterMovementComponent::PublishMovementOwnerSyncNotice(
	const TSharedPtr<FMovementOwnerSyncContext>& Context, EGGYGOMovementOwnerSyncState State,
	uint64 Generation, uint64 Nonce, bool bInitialEligible, FName Reason)
{
	if (!Context.IsValid()) return;
	const bool bInvalidated = State == EGGYGOMovementOwnerSyncState::Invalidated;
	FMovementOwnerSyncDispatchFrame Dispatch(this, Context, bInvalidated);
	if (!bInvalidated && (!IsMovementOwnerSyncContextCurrent(Context)
		|| MovementOwnerSyncLastNoticeSerial >= MAX_uint64 - 1))
	{
		if (MovementOwnerSyncLastNoticeSerial >= MAX_uint64 - 1)
		{
			bMovementOwnerSyncClosed = true;
			ReportMovementOwnerSyncOnce(FName(TEXT("NoticeSerialExhausted")), TEXT("Native owner synchronization closed; notice serial cannot wrap."));
			RetireMovementOwnerSyncScope(FName(TEXT("NoticeSerialExhausted")));
		}
		return;
	}
	if (MovementOwnerSyncLastNoticeSerial == MAX_uint64) return;
	FGGYGOMovementOwnerSyncNotice Notice;
	Notice.Scope = Context->Scope;
	Notice.State = State;
	Notice.NoticeSerial = ++MovementOwnerSyncLastNoticeSerial;
	Notice.ServerOwnerGeneration = Generation;
	Notice.NativeResponseNonce = Nonce;
	Notice.bInitialSynchronizationEligible = bInitialEligible && !bInvalidated;
	Notice.Reason = Reason;
	Context->Notice = Notice;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	for (const auto& Record : Dispatch.Records)
	{
		if (Record->bClosed || !Context->Observers.Contains(Record->Id.ObserverSerial)) continue;
		const FGGYGOMovementOwnerSyncObserverId Id = Record->Id;
		{
			FGGYGOMovementOwnerSyncDelegate Callback;
			if (bInvalidated)
			{
				Record->bClosed = true;
				Context->Observers.Remove(Id.ObserverSerial);
				Callback = MoveTemp(Record->Callback);
			}
			else
			{
				Callback = Record->Callback;
			}
			if (Callback.IsBound()) Callback.Execute(Id, Notice);
		} // Release captures before reacquiring the original weak owner.
		UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
		if (!Self || Self->MovementOwnerSyncLifetimeSerial != Notice.Scope.ConsumerLifetimeSerial) return;
		if (!bInvalidated && (!Self->IsMovementOwnerSyncContextCurrent(Context)
			|| Context->Notice.NoticeSerial != Notice.NoticeSerial)) return;
	}
}

void UGGYGOCharacterMovementComponent::RetireMovementOwnerSyncScope(FName Reason)
{
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!Context.IsValid() || Context->bRetired) return;
	Context->bRetired = true;
	ResetLocomotionSteeringObservation();
	MovementOwnerSyncContext.Reset(); // Seal first; callbacks cannot reopen the old scope.
	if (MovementInputNativeSource.IsValid() && MovementInputNativeSource->OwnerScope == Context->Scope)
	{
		// Retire borrowed native provenance before callbacks. Historical moves keep their original values.
		MovementInputNativeSource.Reset();
		if (MovementInputBindingSerial == 0)
		{
			MovementInputSourceCheckpoint = {};
			LastMovementInputRequestSerial = 0;
		}
	}
	if (MovementInputRequestOwnerScope == Context->Scope)
	{
		RevokeMovementInputRequest();
	}
	else if (Context->ObservedBindingSerial == MovementInputBindingSerial
		|| (Context->ObservedBindingSerial == 0 && MovementInputBindingSerial == 1
			&& bMovementInputBindingActive))
	{
		// Retiring this binding's original owner also closes an unused Cold window.
		// It does not manufacture Source Released or revoke a successor request.
		bMovementInputColdStartWindowOpen = false;
	}
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	RetireQualifiedMovementIntent(Context->Scope, Reason);
	if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get())
		Self->PublishMovementOwnerSyncNotice(Context, EGGYGOMovementOwnerSyncState::Invalidated,
			Context->Notice.ServerOwnerGeneration, Context->Scope.ResponseNonce, false, Reason);
}

void UGGYGOCharacterMovementComponent::RetireServerMovementOwner(FName Reason)
{
	if (!bServerMovementOwnerActive) return;
	bServerMovementOwnerActive = false;
	bServerMovementInitialOwnerGeneration = false;
	ServerMovementOwnerPawn.Reset();
	ServerMovementOwnerController.Reset();
	ServerMovementOwnerConnection.Reset();
	ServerMovementOwnerNetDriver.Reset();
	MovementOwnerSyncPendingReceipt = {};
	ServerMovementOwnerGeneration = GGYGOMovementOwnerSync::IssueIdentity();
	if (ServerMovementOwnerGeneration == 0)
	{
		bMovementOwnerSyncClosed = true;
		ReportMovementOwnerSyncOnce(Reason, TEXT("Server owner retirement exhausted its identity; synchronization closed."));
	}
}

void UGGYGOCharacterMovementComponent::OpenMovementOwnerSyncScope(
	APawn* Pawn, APlayerController* Controller, UNetConnection* Connection, UNetDriver* Driver,
	bool bInitialLocalScope, uint64 ExpectedServerGeneration)
{
	if (bMovementOwnerSyncClosed || MovementOwnerSyncContext.IsValid()
		|| !IsValid(Pawn) || !IsValid(Controller) || MovementOwnerSyncLifetimeSerial == 0) return;
	const TSharedPtr<FMovementOwnerSyncContext> Context = MakeShared<FMovementOwnerSyncContext>();
	Context->Scope.Consumer = this;
	Context->Scope.OriginalPawn = Pawn;
	Context->Scope.OriginalPlayerController = Controller;
	Context->Scope.ConsumerLifetimeSerial = MovementOwnerSyncLifetimeSerial;
	Context->Scope.OwnerContextSerial = GGYGOMovementOwnerSync::IssueIdentity();
	Context->Scope.ScopeSerial = GGYGOMovementOwnerSync::IssueIdentity();
	Context->Scope.ResponseNonce = GGYGOMovementOwnerSync::IssueIdentity();
	if (!Context->Scope.IsSet())
	{
		bMovementOwnerSyncClosed = true;
		ReportMovementOwnerSyncOnce(FName(TEXT("ScopeIdentityExhausted")), TEXT("Cannot issue original owner scope."));
		return;
	}
	Context->Connection = Connection;
	Context->NetDriver = Driver;
	Context->bConnectionCaptured = Connection != nullptr;
	Context->bOwnerPairCaptured = Controller->GetPawn() == Pawn;
	Context->ObservedBindingSerial = MovementInputBindingSerial;
	Context->bObservedBindingActive = bMovementInputBindingActive;
	Context->ExpectedServerGeneration = ExpectedServerGeneration;
	Context->bInitialLocalScope = bInitialLocalScope;
	Context->bRequiresNativeResponse = !Pawn->HasAuthority();
	bMovementOwnerSyncEverOpened = true;
	MovementOwnerSyncReportedReasons.Reset();
	MovementOwnerSyncContext = Context;
	PublishMovementOwnerSyncNotice(Context, EGGYGOMovementOwnerSyncState::Waiting,
		ExpectedServerGeneration, 0, bInitialLocalScope, FName(TEXT("WaitingForNativeOwnerSynchronization")));
}

void UGGYGOCharacterMovementComponent::HandleMovementOwnerControllerChanged(
	APawn* Pawn, AController* OldController, AController* NewController)
{
	if (bMovementOwnerSyncClosed || Pawn != MovementOwnerObservedPawn.Get()
		|| OldController == NewController) return;
	if (OldController)
	{
		bMovementOwnerSyncEverOpened = true;
		bServerMovementOwnerEverOpened = true;
	}
	RetireServerMovementOwner(FName(TEXT("NativeControllerChanged")));
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	RetireMovementOwnerSyncScope(FName(TEXT("NativeControllerChanged")));
	if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get()) Self->RefreshMovementOwnerSyncContext();
}

void UGGYGOCharacterMovementComponent::RefreshMovementOwnerSyncContext()
{
	if (bMovementOwnerSyncClosed || MovementOwnerSyncLifetimeSerial == 0) return;
	APawn* Pawn = CharacterOwner;
	if (Pawn != MovementOwnerObservedPawn.Get())
	{
		if (APawn* Old = MovementOwnerObservedPawn.Get())
			Old->ReceiveControllerChangedDelegate.RemoveDynamic(this, &UGGYGOCharacterMovementComponent::HandleMovementOwnerControllerChanged);
		MovementOwnerObservedPawn = Pawn;
		if (IsValid(Pawn))
			Pawn->ReceiveControllerChangedDelegate.AddUniqueDynamic(this, &UGGYGOCharacterMovementComponent::HandleMovementOwnerControllerChanged);
	}
	APlayerController* PC = IsValid(Pawn) ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
	UNetDriver* Driver = IsValid(Pawn) ? Pawn->GetNetDriver() : nullptr;
	UNetConnection* Connection = GGYGOMovementOwnerSync::GetConnection(Pawn, PC);
	const bool bCandidate = IsValid(Pawn) && IsValid(PC)
		&& (Pawn->HasAuthority() || (Pawn->IsLocallyControlled() && Pawn->GetLocalRole() == ROLE_AutonomousProxy))
		&& (Pawn->HasAuthority() || IsValid(Driver))
		&& (!Connection || GGYGOMovementOwnerSync::IsConnectionLive(Connection));
	const bool bServerTurnover = bServerMovementOwnerActive
		&& (!bCandidate || !Pawn->HasAuthority() || ServerMovementOwnerPawn.Get() != Pawn
			|| ServerMovementOwnerController.Get() != PC
			|| ServerMovementOwnerNetDriver.Get() != Driver
			|| (!ServerMovementOwnerNetDriver.IsExplicitlyNull() && !ServerMovementOwnerNetDriver.IsValid())
			|| (!ServerMovementOwnerConnection.IsExplicitlyNull()
				&& (ServerMovementOwnerConnection.Get() != Connection
					|| !GGYGOMovementOwnerSync::IsConnectionLive(Connection))));
	if (bServerTurnover) RetireServerMovementOwner(FName(TEXT("NativeOwnerEndpointChanged")));
	if (bCandidate && Pawn->HasAuthority() && !bServerMovementOwnerActive)
	{
		ServerMovementOwnerGeneration = GGYGOMovementOwnerSync::IssueIdentity();
		if (ServerMovementOwnerGeneration == 0)
		{
			bMovementOwnerSyncClosed = true;
			RetireMovementOwnerSyncScope(FName(TEXT("ServerIdentityExhausted")));
			return;
		}
		bServerMovementOwnerActive = true;
		bServerMovementInitialOwnerGeneration = !bServerMovementOwnerEverOpened;
		bServerMovementOwnerEverOpened = true;
		ServerMovementOwnerPawn = Pawn;
		ServerMovementOwnerController = PC;
		ServerMovementOwnerConnection = Connection;
		ServerMovementOwnerNetDriver = Driver;
	}
	if (bServerMovementOwnerActive && ServerMovementOwnerConnection.IsExplicitlyNull() && Connection)
		ServerMovementOwnerConnection = Connection;
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (Context.IsValid())
	{
		const bool bFirstBinding = Context->ObservedBindingSerial == 0
			&& MovementInputBindingSerial == 1 && bMovementInputBindingActive;
		const bool bChanged = !bCandidate || Context->Scope.OriginalPawn.Get() != Pawn
			|| Context->Scope.OriginalPlayerController.Get() != PC || Context->NetDriver.Get() != Driver
			|| (!Context->NetDriver.IsExplicitlyNull() && !Context->NetDriver.IsValid())
			|| (Context->bConnectionCaptured && (Context->Connection.Get() != Connection
				|| !GGYGOMovementOwnerSync::IsConnectionLive(Connection)))
			|| (Context->bOwnerPairCaptured && PC->GetPawn() != Pawn)
			|| (Context->Notice.State == EGGYGOMovementOwnerSyncState::Ready
				&& (Context->bRequiresNativeResponse || !PC->IsLocalController()) && PC->AcknowledgedPawn != Pawn)
			|| (Context->ObservedBindingSerial != MovementInputBindingSerial && !bFirstBinding)
			|| (Context->bObservedBindingActive != bMovementInputBindingActive && !bFirstBinding)
			|| bServerTurnover;
		if (bChanged)
		{
			const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
			RetireMovementOwnerSyncScope(FName(TEXT("NativeOwnerOrInputScopeChanged")));
			// Re-read after callouts; never issue a scope from captured old actors.
			if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get()) Self->RefreshMovementOwnerSyncContext();
			return;
		}
		if (bFirstBinding)
		{
			Context->ObservedBindingSerial = MovementInputBindingSerial;
			Context->bObservedBindingActive = bMovementInputBindingActive;
		}
		if (!Context->bConnectionCaptured && Connection)
		{
			Context->Connection = Connection;
			Context->bConnectionCaptured = true;
		}
		if (PC->GetPawn() == Pawn) Context->bOwnerPairCaptured = true;
	}
	if (!bCandidate)
	{
		if (Connection && !GGYGOMovementOwnerSync::IsConnectionLive(Connection))
			ReportMovementOwnerSyncOnce(FName(TEXT("NativeConnectionClosed")), TEXT("Original native connection is closed or invalid."));
		return;
	}
	if (!MovementOwnerSyncContext.IsValid())
	{
		OpenMovementOwnerSyncScope(Pawn, PC, Connection, Driver,
			!bMovementOwnerSyncEverOpened && MovementInputBindingSerial <= 1,
			Pawn->HasAuthority() ? ServerMovementOwnerGeneration : 0);
	}
	const TSharedPtr<FMovementOwnerSyncContext> Current = MovementOwnerSyncContext;
	if (Current.IsValid() && !Current->bRequiresNativeResponse
		&& Current->Notice.State == EGGYGOMovementOwnerSyncState::Waiting
		&& IsMovementOwnerSyncContextCurrent(Current) && PC->GetPawn() == Pawn
		&& (PC->IsLocalController() || (GGYGOMovementOwnerSync::IsConnectionLive(Connection) && PC->AcknowledgedPawn == Pawn)))
	{
		PublishMovementOwnerSyncNotice(Current, EGGYGOMovementOwnerSyncState::Ready,
			ServerMovementOwnerGeneration, Current->Scope.ResponseNonce,
			Current->bInitialLocalScope && bServerMovementInitialOwnerGeneration, FName(TEXT("NativeAuthorityOwnerReady")));
	}
}

bool UGGYGOCharacterMovementComponent::IsMovementOwnerSyncReceiptCurrent(
	const FMovementOwnerSyncNativeReceipt& Receipt) const
{
	APawn* Pawn = Receipt.Pawn.Get();
	APlayerController* PC = Receipt.Controller.Get();
	UNetConnection* Connection = Receipt.Connection.Get();
	return !bMovementOwnerSyncClosed && bServerMovementOwnerActive && Receipt.Nonce != 0
		&& Receipt.OwnerGeneration == ServerMovementOwnerGeneration
		&& IsValid(Pawn) && Pawn == CharacterOwner && Pawn->HasAuthority()
		&& IsValid(PC) && PC == ServerMovementOwnerController.Get() && Pawn == ServerMovementOwnerPawn.Get()
		&& Pawn->GetController() == PC && PC->GetPawn() == Pawn && PC->AcknowledgedPawn == Pawn
		&& Connection == ServerMovementOwnerConnection.Get() && PC->GetNetConnection() == Connection
		&& GGYGOMovementOwnerSync::IsConnectionLive(Connection);
}

bool UGGYGOCharacterMovementComponent::IsMovementInputBindingCurrent(
	const FGGYGOMovementInputConsumerBindingId& Binding) const
{
	return bMovementInputBindingActive && Binding.ConsumerBindingSerial != 0
		&& Binding.ConsumerBindingSerial == MovementInputBindingSerial
		&& Binding == MovementInputBinding;
}

void UGGYGOCharacterMovementComponent::RecordMovementInputSourceCheckpoint(const FGGYGOMovementInputFact& Fact)
{
	// This is an original-fact value cache, never an input producer or an admission gate.
	FGGYGOMovementInputSourceCheckpoint& Checkpoint = MovementInputSourceCheckpoint;
	if (!Checkpoint.bPresent || Checkpoint.BindingSerial != MovementInputBinding.ConsumerBindingSerial
		|| Fact.Request.Session != MovementInputBinding.SourceSession
		|| Fact.EventSerial != LastMovementInputFact.EventSerial || Fact.Request != LastMovementInputFact.Request
		|| Fact.Kind != LastMovementInputFact.Kind || Fact.Reason != LastMovementInputFact.Reason
		|| Fact.SessionMode != LastMovementInputFact.SessionMode || Fact.StartProof != LastMovementInputFact.StartProof) return;
	Checkpoint.ConsumerFenceSerial = MovementInputBindingSerial;
	Checkpoint.EventSerial = Fact.EventSerial;
	Checkpoint.FactRequestSerial = Fact.Request.RequestSerial;
	Checkpoint.LastFactKind = Fact.Kind;
	switch (Fact.Kind)
	{
	case EGGYGOMovementInputFactKind::SessionOpened:
		Checkpoint.SessionOpenedEventSerial = Fact.EventSerial;
		Checkpoint.SessionMode = Fact.SessionMode;
		break;
	case EGGYGOMovementInputFactKind::NeutralConfirmed:
		Checkpoint.NeutralEventSerial = Fact.EventSerial;
		break;
	case EGGYGOMovementInputFactKind::RequestStarted:
		// Preserve the actual preceding release and neutral before replacing the request.
		Checkpoint.StartReleaseRequestSerial = Checkpoint.RequestReleasedEventSerial != 0 ? Checkpoint.RequestSerial : 0;
		Checkpoint.StartReleaseEventSerial = Checkpoint.RequestReleasedEventSerial;
		Checkpoint.StartNeutralEventSerial = Checkpoint.NeutralEventSerial;
		Checkpoint.RequestSerial = Fact.Request.RequestSerial;
		Checkpoint.RequestStartedEventSerial = Fact.EventSerial;
		Checkpoint.StartProof = Fact.StartProof;
		Checkpoint.RequestReleasedEventSerial = 0;
		break;
	case EGGYGOMovementInputFactKind::RequestReleased:
		Checkpoint.RequestReleasedEventSerial = Fact.EventSerial;
		break;
	case EGGYGOMovementInputFactKind::SourceUnresolved:
		Checkpoint.SourceUnresolvedEventSerial = Fact.EventSerial;
		break;
	case EGGYGOMovementInputFactKind::SessionInvalidated:
		// Exact receiver teardown already captured its fence; this records the source fact.
		break;
	default:
		break; // No rejected/unknown fact reaches this successful-consume capture.
	}
}

EGGYGOMovementInitialRequestAdmissionResult UGGYGOCharacterMovementComponent::TryAdmitInitialMovementInputRequest(
	const FGGYGOMovementOwnerSyncObserverId& OriginalObserver,
	const FGGYGOMovementInputConsumerBindingId& OriginalBinding,
	const FGGYGOMovementInputRequestIdentity& OriginalSourceRequest,
	const FGGYGOMovementOwnerSyncNotice& OriginalReadyNotice, FString& OutError)
{
	using EResult = EGGYGOMovementInitialRequestAdmissionResult;
	OutError.Reset();
	const auto Reject = [this, &OriginalBinding, &OriginalSourceRequest, &OriginalReadyNotice, &OutError]
		(EResult Result, const TCHAR* Reason)
	{
		OutError = FString::Printf(
			TEXT("Movement.InitialAdmission: Consumer='%s', Producer='%s', Binding=%llu, Session=%llu, InputRequest=%llu, ExecutionRequest=%llu, Scope=%llu, Nonce=%llu, Generation=%llu, Notice=%llu, Reason='%s'."),
			*GetPathName(), *GetPathNameSafe(OriginalSourceRequest.Session.Producer.Get()),
			static_cast<unsigned long long>(OriginalBinding.ConsumerBindingSerial),
			static_cast<unsigned long long>(OriginalSourceRequest.Session.SessionSerial),
			static_cast<unsigned long long>(OriginalSourceRequest.RequestSerial),
			static_cast<unsigned long long>(LocomotionRequestSerial),
			static_cast<unsigned long long>(OriginalReadyNotice.Scope.ScopeSerial),
			static_cast<unsigned long long>(OriginalReadyNotice.NativeResponseNonce),
			static_cast<unsigned long long>(OriginalReadyNotice.ServerOwnerGeneration),
			static_cast<unsigned long long>(OriginalReadyNotice.NoticeSerial), Reason);
		return Result;
	};
	if (!IsInGameThread())
	{
		OutError = TEXT("Movement.InitialAdmission: initial admission requires the game thread.");
		return EResult::Rejected;
	}
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!IsValid(this) || IsBeingDestroyed() || !IsValid(GetOwner())
		|| !IsMovementInputBindingCurrent(OriginalBinding) || !bMovementInputSessionOpened
		|| !OriginalSourceRequest.Session.Producer.IsValid() || OriginalSourceRequest.RequestSerial == 0
		|| OriginalSourceRequest.Session != OriginalBinding.SourceSession
		|| OriginalSourceRequest != MovementInputRequest || LocomotionRequestSerial == 0
		|| !OriginalObserver.IsSet() || !IsMovementOwnerSyncContextCurrent(Context)
		|| MovementInputRequestOwnerScope != Context->Scope || OriginalObserver.Scope != Context->Scope)
	{
		return Reject(EResult::Stale, TEXT("original binding, request or native owner scope is no longer current"));
	}
	const TSharedPtr<FMovementOwnerSyncObserverRecord>* Found =
		Context->Observers.Find(OriginalObserver.ObserverSerial);
	if (!Found || !Found->IsValid() || (*Found)->bClosed || !(*Found)->Callback.IsBound()
		|| (*Found)->Id.Scope != OriginalObserver.Scope
		|| (*Found)->Id.ObserverSerial != OriginalObserver.ObserverSerial)
	{
		return Reject(EResult::Stale, TEXT("original observer is not a live subscription in this exact scope"));
	}
	const FGGYGOMovementOwnerSyncNotice& CurrentNotice = Context->Notice;
	if (!OriginalReadyNotice.IsSet() || OriginalReadyNotice.State != EGGYGOMovementOwnerSyncState::Ready
		|| CurrentNotice.State != EGGYGOMovementOwnerSyncState::Ready
		|| OriginalReadyNotice.Scope != Context->Scope
		|| OriginalReadyNotice.NoticeSerial != CurrentNotice.NoticeSerial
		|| OriginalReadyNotice.ServerOwnerGeneration == 0
		|| OriginalReadyNotice.ServerOwnerGeneration != CurrentNotice.ServerOwnerGeneration
		|| OriginalReadyNotice.NativeResponseNonce != Context->Scope.ResponseNonce
		|| OriginalReadyNotice.NativeResponseNonce != CurrentNotice.NativeResponseNonce
		|| OriginalReadyNotice.bInitialSynchronizationEligible != CurrentNotice.bInitialSynchronizationEligible
		|| OriginalReadyNotice.Reason != CurrentNotice.Reason)
	{
		return Reject(EResult::Stale, TEXT("Ready does not match the current original native notice, nonce and generation"));
	}
	if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
	{
		OutError = LocomotionRequestFailureReason;
		return EResult::ExecutionFailed;
	}
	if (!bMovementInputRequestOpen)
	{
		return Reject(EResult::Rejected, TEXT("the original Source request has already been released"));
	}
	if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted)
	{
		return EResult::AlreadyAdmitted;
	}
	if (LocomotionRequestAdmission != ELocomotionRequestAdmission::Waiting
		|| !Context->bRequiresNativeResponse || !Context->bInitialLocalScope
		|| !CurrentNotice.bInitialSynchronizationEligible)
	{
		return Reject(EResult::Rejected, TEXT("only the original ordinary first synchronization may admit Waiting; recovery requires real Release then Press"));
	}
	if (!HasAcceptedMovementSet())
	{
		FailLocomotionRequest(LocomotionRequestSerial, TEXT("initial waiting request has no accepted MovementSet at Ready"));
		OutError = LocomotionRequestFailureReason;
		return EResult::ExecutionFailed;
	}
	// The real Started already reserved this execution number and reset its motion.
	// Hero verifies physical Held through Source; CMC neither polls nor synthesizes it.
	LocomotionRequestAdmission = ELocomotionRequestAdmission::Admitted;
	bMovementInputAdmissionDiagnosticReported = false;
	PublishQualifiedMovementIntent();
	return EResult::Admitted;
}

EGGYGOQualifiedMovementIntentQueryResult UGGYGOCharacterMovementComponent::QueryQualifiedMovementIntent(
	const FGGYGOMovementOwnerSyncScopeId& OriginalScope,
	FGGYGOQualifiedMovementIntent& OutIntent, FString& OutError) const
{
	using EResult = EGGYGOQualifiedMovementIntentQueryResult;
	OutIntent = {};
	OutError.Reset();
	const auto Reject = [this, &OriginalScope, &OutError](EResult Result, const FString& Reason)
	{
		OutError = FString::Printf(TEXT("[Movement.QualifiedIntent] Consumer='%s' Scope=%llu Execution=%llu Reason='%s'"),
			*GetPathName(), static_cast<unsigned long long>(OriginalScope.GetScopeSerial()),
			static_cast<unsigned long long>(LocomotionRequestSerial), *Reason);
		return Result;
	};
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!IsInGameThread() || !IsValid(this) || IsBeingDestroyed()
		|| !OriginalScope.IsSet() || !IsMovementOwnerSyncContextCurrent(Context)
		|| OriginalScope != Context->Scope || !IsValid(CharacterOwner))
		return Reject(EResult::Unavailable, TEXT("original native owner scope is unavailable"));
	// Replayed historical moves are never a new live cancellation grant.
	if (CharacterOwner->bClientUpdating || PreparingLocomotionCurveReplayGroup)
		return Reject(EResult::Unavailable, TEXT("historical movement replay has no live input grant"));
	FGGYGOQualifiedMovementIntent Candidate;
	Candidate.Scope = OriginalScope;
	const bool bNativeRemote = CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled()
		&& Cast<APlayerController>(CharacterOwner->GetController());
	if (bNativeRemote)
	{
		const TSharedPtr<const FMovementInputNativeSource> Source = MovementInputNativeSource;
		if (!Source.IsValid() || !IsMovementOwnerSyncReceiptCurrent(Source->Receipt)
			|| Source->OwnerScope != OriginalScope || Source->ExecutionRequestSerial != LocomotionRequestSerial
			|| Source->ExecutionRequestSerial == 0 || MovementInputRequestOwnerScope != OriginalScope)
			return Reject(EResult::Unavailable, TEXT("original authenticated native request is unavailable"));
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
			return Reject(EResult::ExecutionFailed, LocomotionRequestFailureReason);
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Released && !bMovementInputRequestOpen)
			return EResult::NotHeld;
		if (MovementInputSourceCheckpoint.bConsumerInvalidated
			|| MovementInputSourceCheckpoint.SourceUnresolvedEventSerial > MovementInputSourceCheckpoint.RequestStartedEventSerial)
			return Reject(EResult::AwaitingPhysicalProof, TEXT("native source checkpoint is unresolved or invalidated"));
		if (!bMovementInputRequestOpen || LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
			|| IsMovementInputRequestBlocked())
			return Reject(EResult::WaitingForAdmission, TEXT("original native request has no current execution admission"));
		Candidate.Provenance = EGGYGOQualifiedMovementIntentProvenance::AuthenticatedNativeRequest;
		Candidate.BindingSerial = Source->StartCheckpoint.BindingSerial;
		Candidate.SessionSerial = Source->StartCheckpoint.SessionSerial;
		Candidate.RequestSerial = Source->StartCheckpoint.RequestSerial;
	}
	else
	{
		const UGGYGOPlayerInput* Producer = Cast<UGGYGOPlayerInput>(MovementInputBinding.SourceSession.Producer.Get());
		if (!Producer || !IsMovementInputBindingCurrent(MovementInputBinding) || !bMovementInputSessionOpened)
			return Reject(EResult::Unavailable, TEXT("original local Source session/binding is unavailable"));
		FGGYGOMovementInputRequestIdentity SourceRequest;
		FString SourceError;
		const EGGYGOMovementInputRequestQueryResult SourceResult =
			Producer->QueryMovementInputRequest(MovementInputBinding.SourceSession, SourceRequest, SourceError);
		switch (SourceResult)
		{
		case EGGYGOMovementInputRequestQueryResult::NotHeld: return EResult::NotHeld;
		case EGGYGOMovementInputRequestQueryResult::AwaitingPhysicalProof:
			return Reject(EResult::AwaitingPhysicalProof, SourceError);
		case EGGYGOMovementInputRequestQueryResult::Unavailable: return Reject(EResult::Unavailable, SourceError);
		case EGGYGOMovementInputRequestQueryResult::Held: break;
		default: return Reject(EResult::Unavailable, TEXT("Source returned an unsupported query result"));
		}
		if (SourceRequest != MovementInputRequest || SourceRequest.RequestSerial == 0
			|| MovementInputRequestOwnerScope != OriginalScope || LocomotionRequestSerial == 0)
			return Reject(EResult::WaitingForAdmission, TEXT("real Held has not committed this original CMC request"));
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
			return Reject(EResult::ExecutionFailed, LocomotionRequestFailureReason);
		if (!bMovementInputRequestOpen || LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
			|| IsMovementInputRequestBlocked())
			return Reject(EResult::WaitingForAdmission, TEXT("real Held is waiting for original CMC admission"));
		Candidate.Provenance = EGGYGOQualifiedMovementIntentProvenance::LocalSourceHeld;
		Candidate.BindingSerial = MovementInputBinding.ConsumerBindingSerial;
		Candidate.SessionSerial = SourceRequest.Session.SessionSerial;
		Candidate.RequestSerial = SourceRequest.RequestSerial;
	}
	Candidate.ExecutionRequestSerial = LocomotionRequestSerial;
	OutIntent = Candidate;
	return EResult::Qualified;
}

bool UGGYGOCharacterMovementComponent::SubscribeQualifiedMovementIntent(
	const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FGGYGOQualifiedMovementIntentDelegate Observer,
	FGGYGOQualifiedMovementIntentObserverId& OutObserver, FString& OutError)
{
	OutError.Reset();
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!IsInGameThread() || OutObserver.IsSet() || !Observer.IsBound() || !OriginalScope.IsSet()
		|| !IsMovementOwnerSyncContextCurrent(Context) || Context->Scope != OriginalScope
		|| QualifiedMovementIntentLastObserverSerial == MAX_uint64)
	{
		OutError = TEXT("[Movement.QualifiedIntent] subscription requires an empty output, original live scope, callback and non-exhausted identity");
		return false;
	}
	const TSharedPtr<FQualifiedMovementIntentObserver> Record = MakeShared<FQualifiedMovementIntentObserver>();
	Record->Id.Scope = OriginalScope;
	Record->Id.ObserverSerial = ++QualifiedMovementIntentLastObserverSerial;
	Record->Callback = MoveTemp(Observer);
	Record->LastResult = QueryQualifiedMovementIntent(OriginalScope, Record->LastIntent, Record->LastError);
	QualifiedMovementIntentObservers.Add(Record);
	OutObserver = Record->Id; // Never write the caller's resource member after replay.
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	++QualifiedMovementIntentReplayDepth;
	{
		const FGGYGOQualifiedMovementIntentDelegate Callback = Record->Callback;
		Callback.Execute(Record->Id, Record->LastResult, Record->LastIntent, Record->LastError);
	}
	if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get())
	{
		--Self->QualifiedMovementIntentReplayDepth;
		// A real Source fact may commit during replay. Publish its latest original state
		// after the outer replay, so admission changes do not wait for another input edge.
		if (Self->QualifiedMovementIntentReplayDepth == 0 && Self->IsMovementOwnerSyncContextCurrent(Context))
			Self->PublishQualifiedMovementIntent();
	}
	return true; // It was installed; the replay may already have closed it.
}

bool UGGYGOCharacterMovementComponent::UnsubscribeQualifiedMovementIntent(
	const FGGYGOQualifiedMovementIntentObserverId& OriginalObserver, FName Reason, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || !OriginalObserver.IsSet() || Reason.IsNone())
	{
		OutError = TEXT("[Movement.QualifiedIntent] unsubscribe requires original identity, reason and game thread");
		return false;
	}
	const int32 Index = QualifiedMovementIntentObservers.IndexOfByPredicate([&](const auto& Record)
	{
		return Record.IsValid() && Record->Id == OriginalObserver;
	});
	if (Index == INDEX_NONE) return true; // The exact record was already withdrawn; never search a successor.
	const auto Record = QualifiedMovementIntentObservers[Index];
	Record->bClosed = true;
	QualifiedMovementIntentObservers.RemoveAt(Index);
	const auto RetiredCallback = MoveTemp(Record->Callback); // No this/index access after capture destruction.
	return true;
}

void UGGYGOCharacterMovementComponent::PublishQualifiedMovementIntent()
{
	if (QualifiedMovementIntentReplayDepth != 0 || PreparingLocomotionCurveReplayGroup
		|| (CharacterOwner && CharacterOwner->bClientUpdating)) return;
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!IsMovementOwnerSyncContextCurrent(Context)) return;
	const uint64 Publication = ++QualifiedMovementIntentPublicationSerial;
	FGGYGOQualifiedMovementIntent Intent;
	FString Error;
	const auto Result = QueryQualifiedMovementIntent(Context->Scope, Intent, Error);
	const auto Records = QualifiedMovementIntentObservers;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	for (const auto& Record : Records)
	{
		if (!Record.IsValid() || Record->bClosed || Record->Id.Scope != Context->Scope
			|| (Record->LastResult == Result && Record->LastIntent == Intent && Record->LastError == Error)) continue;
		Record->LastResult = Result;
		Record->LastIntent = Intent;
		Record->LastError = Error;
		{
			const auto Callback = Record->Callback;
			if (Callback.IsBound()) Callback.Execute(Record->Id, Result, Intent, Error);
		}
		UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
		if (!Self || Self->QualifiedMovementIntentPublicationSerial != Publication
			|| !Self->IsMovementOwnerSyncContextCurrent(Context)) return;
	}
}

void UGGYGOCharacterMovementComponent::RetireQualifiedMovementIntent(
	const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FName Reason)
{
	++QualifiedMovementIntentPublicationSerial; // Invalidates an in-flight original publication.
	const auto Records = QualifiedMovementIntentObservers;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	const FString Error = FString::Printf(TEXT("[Movement.QualifiedIntent] Scope=%llu retired: %s"),
		static_cast<unsigned long long>(OriginalScope.GetScopeSerial()), *Reason.ToString());
	for (const auto& Record : Records)
	{
		if (!Record.IsValid() || Record->bClosed || Record->Id.Scope != OriginalScope) continue;
		Record->bClosed = true;
		QualifiedMovementIntentObservers.RemoveSingle(Record);
		{
			auto Callback = MoveTemp(Record->Callback);
			if (Callback.IsBound()) Callback.Execute(Record->Id,
				EGGYGOQualifiedMovementIntentQueryResult::Unavailable, FGGYGOQualifiedMovementIntent{}, Error);
		}
		if (!WeakSelf.IsValid()) return;
	}
}

bool UGGYGOCharacterMovementComponent::BindMovementInputSession(
	const FGGYGOMovementInputSessionIdentity& Session, uint64 ExpectedConsumerBindingSerial,
	FGGYGOMovementInputConsumerBindingId& OutBinding, FString& OutError)
{
	OutBinding = {};
	OutError.Reset();
	const auto Reject = [this, &Session, &OutError](const TCHAR* Reason)
	{
		OutError = FString::Printf(
			TEXT("Movement Bind rejected: Consumer='%s', Producer='%s', Session=%llu, Binding=%llu, Reason='%s'."),
			*GetPathName(), *GetPathNameSafe(Session.Producer.Get()),
			static_cast<unsigned long long>(Session.SessionSerial),
			static_cast<unsigned long long>(MovementInputBindingSerial), Reason);
		return false;
	};
	if (!IsValid(this) || IsBeingDestroyed() || !IsValid(GetOwner()))
	{
		return Reject(TEXT("consumer has no live owner or is being destroyed"));
	}
	if (ExpectedConsumerBindingSerial != MovementInputBindingSerial)
	{
		return Reject(TEXT("ExpectedConsumerBindingSerial does not match current consumer serial"));
	}
	if (Session.SessionSerial == 0 || !Session.Producer.IsValid())
	{
		return Reject(TEXT("source requires a live Producer and an allocated SessionSerial"));
	}
	if (!EnsureMovementOwnerSyncLifetime())
	{
		return Reject(TEXT("M2 consumer lifetime is unavailable or retired; Source binding cannot reopen it"));
	}
	const auto FinishBinding = [this, &OutBinding, &OutError]()
	{
		const FGGYGOMovementInputConsumerBindingId OriginalGrant = MovementInputBinding;
		const FString OriginalConsumerPath = GetPathName();
		const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
		RefreshMovementOwnerSyncContext(); // CMC owns synchronous binding/scope lifecycle, including invalidation callouts.
		UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
		if (!Self || Self->bMovementOwnerSyncClosed || !Self->IsMovementInputBindingCurrent(OriginalGrant))
		{
			OutBinding = {};
			OutError = FString::Printf(TEXT("Movement Bind rejected: Consumer='%s', Binding=%llu, Reason='original grant retired during owner-scope synchronization'."),
				*OriginalConsumerPath, static_cast<unsigned long long>(OriginalGrant.ConsumerBindingSerial));
			return false;
		}
		const bool bNeedsPlayerScope = Self->CharacterOwner && Self->CharacterOwner->IsLocallyControlled()
			&& Cast<APlayerController>(Self->CharacterOwner->GetController());
		if (bNeedsPlayerScope && !Self->IsMovementOwnerSyncContextCurrent(Self->MovementOwnerSyncContext))
		{
			OutBinding = {};
			OutError = FString::Printf(TEXT("Movement Bind rejected: Consumer='%s', Binding=%llu, Reason='no current native player owner scope after synchronization'."),
				*OriginalConsumerPath, static_cast<unsigned long long>(OriginalGrant.ConsumerBindingSerial));
			FString CleanupError;
			if (!Self->InvalidateMovementInputSession(OriginalGrant, FName(TEXT("OwnerScopeUnavailable")), CleanupError))
			{
				UE_LOG(LogGGYGOMovement, Error, TEXT("Movement binding cleanup rejected: Consumer='%s', Binding=%llu, Reason='%s'."),
					*OriginalConsumerPath, static_cast<unsigned long long>(OriginalGrant.ConsumerBindingSerial), *CleanupError);
			}
			return false;
		}
		OutBinding = OriginalGrant;
		return true;
	};
	if (Session == MovementInputBinding.SourceSession)
	{
		if (!bMovementInputBindingActive)
		{
			return Reject(TEXT("this source session has already been retired"));
		}
		return FinishBinding();
	}
	if (Session.Producer.HasSameIndexAndSerialNumber(MovementInputBinding.SourceSession.Producer)
		&& Session.SessionSerial <= MovementInputBinding.SourceSession.SessionSerial)
	{
		return Reject(TEXT("source session serial is older than the recorded source session"));
	}
	if (MovementInputBindingSerial == MAX_uint64)
	{
		return Reject(TEXT("consumer binding serial is exhausted"));
	}

	RevokeMovementInputRequest();
	++MovementInputBindingSerial;
	MovementInputBinding.Consumer = this;
	MovementInputBinding.ConsumerBindingSerial = MovementInputBindingSerial;
	MovementInputBinding.SourceSession = Session;
	bMovementInputBindingActive = true;
	bMovementInputSessionOpened = false;
	ConsumedMovementInputSessionMode = EGGYGOMovementInputSessionMode::Invalid;
	bMovementInputColdStartWindowOpen = false;
	bMovementInputNeutralConsumed = false;
	bMovementInputRequestOpen = false;
	LastMovementInputFact = {};
	LastMovementInputRequestSerial = 0;
	MovementInputRequest = {};
	MovementInputRequestOwnerScope = {};
	bMovementInputAdmissionDiagnosticReported = false;
	MovementInputSourceCheckpoint = {};
	MovementInputSourceCheckpoint.bPresent = true;
	MovementInputSourceCheckpoint.BindingSerial = MovementInputBinding.ConsumerBindingSerial;
	MovementInputSourceCheckpoint.ConsumerFenceSerial = MovementInputBindingSerial;
	MovementInputSourceCheckpoint.SessionSerial = Session.SessionSerial;
	return FinishBinding();
}

bool UGGYGOCharacterMovementComponent::InvalidateMovementInputSession(
	const FGGYGOMovementInputConsumerBindingId& Binding, FName Reason, FString& OutError)
{
	OutError.Reset();
	if (Binding.ConsumerBindingSerial == 0 || Binding != MovementInputBinding || Reason.IsNone())
	{
		OutError = FString::Printf(
			TEXT("Movement Invalidate rejected: Consumer='%s', Binding=%llu, Reason='%s'; exact original binding and a reason are required."),
			*GetPathName(), static_cast<unsigned long long>(Binding.ConsumerBindingSerial), *Reason.ToString());
		return false;
	}
	if (!bMovementInputBindingActive)
	{
		return true;
	}
	// Seal the grant before cleanup. Preserve the old value for exact idempotent teardown.
	bMovementInputBindingActive = false;
	ResetLocomotionSteeringObservation();
	bMovementInputSessionOpened = false;
	ConsumedMovementInputSessionMode = EGGYGOMovementInputSessionMode::Invalid;
	bMovementInputColdStartWindowOpen = false;
	bMovementInputNeutralConsumed = false;
	bMovementInputRequestOpen = false;
	if (MovementInputBindingSerial != MAX_uint64)
	{
		++MovementInputBindingSerial;
	}
	MovementInputSourceCheckpoint.bConsumerInvalidated = true;
	MovementInputSourceCheckpoint.ConsumerFenceSerial = MovementInputBindingSerial;
	RevokeMovementInputRequest();
	MovementInputRequestOwnerScope = {};
	// Seal the old scope now. A subsequent Bind issues its own current scope synchronously.
	// No writes follow external Invalidated callbacks; they may already install a successor.
	RetireMovementOwnerSyncScope(Reason);
	return true;
}

EGGYGOMovementInputConsumeResult UGGYGOCharacterMovementComponent::ConsumeMovementInputFact(
	const FGGYGOMovementInputConsumerBindingId& Binding,
	const FGGYGOMovementInputFact& Fact, FString& OutError)
{
	OutError.Reset();
	const auto Report = [this, &Binding, &Fact, &OutError](
		EGGYGOMovementInputConsumeResult Result, const TCHAR* Reason)
	{
		OutError = FString::Printf(
			TEXT("Movement Consume: Consumer='%s', Producer='%s', Binding=%llu, Session=%llu, InputRequest=%llu, Event=%llu, Kind=%u, Mode=%u, Proof=%u, Reason='%s'."),
			*GetPathName(), *GetPathNameSafe(Binding.SourceSession.Producer.Get()),
			static_cast<unsigned long long>(Binding.ConsumerBindingSerial),
			static_cast<unsigned long long>(Fact.Request.Session.SessionSerial),
			static_cast<unsigned long long>(Fact.Request.RequestSerial),
			static_cast<unsigned long long>(Fact.EventSerial), static_cast<uint32>(Fact.Kind),
			static_cast<uint32>(Fact.SessionMode), static_cast<uint32>(Fact.StartProof), Reason);
		return Result;
	};
	const bool bSameLastFact = Fact.EventSerial == LastMovementInputFact.EventSerial
		&& Fact.Request == LastMovementInputFact.Request && Fact.Kind == LastMovementInputFact.Kind
		&& Fact.Reason == LastMovementInputFact.Reason
		&& Fact.SessionMode == LastMovementInputFact.SessionMode
		&& Fact.StartProof == LastMovementInputFact.StartProof;
	if (Binding.ConsumerBindingSerial != 0 && Binding == MovementInputBinding
		&& !bMovementInputBindingActive && Fact.EventSerial != 0
		&& Fact.EventSerial == LastMovementInputFact.EventSerial)
	{
		if (!bSameLastFact)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("retired binding event serial was reused for different fact data"));
		}
		if (Fact.Kind == EGGYGOMovementInputFactKind::SessionInvalidated)
		{
			return EGGYGOMovementInputConsumeResult::Duplicate;
		}
	}
	if (!IsMovementInputBindingCurrent(Binding))
	{
		return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("binding is not the current active consumer grant"));
	}
	if (Fact.Request.Session != Binding.SourceSession || Fact.EventSerial == 0
		|| Fact.Kind == EGGYGOMovementInputFactKind::Invalid
		|| static_cast<uint8>(Fact.Kind) > static_cast<uint8>(EGGYGOMovementInputFactKind::SourceUnresolved))
	{
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("invalid fact kind/event or mismatched source session"));
	}
	const bool bValidSessionMode = Fact.Kind == EGGYGOMovementInputFactKind::SessionOpened
		? (Fact.SessionMode == EGGYGOMovementInputSessionMode::Cold || Fact.SessionMode == EGGYGOMovementInputSessionMode::Rearm)
		: Fact.SessionMode == EGGYGOMovementInputSessionMode::Invalid;
	const bool bValidStartProof = Fact.Kind == EGGYGOMovementInputFactKind::RequestStarted
		? (Fact.StartProof == EGGYGOMovementInputStartProof::ColdPhysicalPress
			|| Fact.StartProof == EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress)
		: Fact.StartProof == EGGYGOMovementInputStartProof::Invalid;
	if (!bValidSessionMode || !bValidStartProof)
	{
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("Mode/Proof is missing, out of range, or invalid for this fact kind"));
	}
	if (Fact.EventSerial == LastMovementInputFact.EventSerial)
	{
		return bSameLastFact ? EGGYGOMovementInputConsumeResult::Duplicate
			: Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("event serial was reused for different fact data"));
	}
	if (Fact.EventSerial < LastMovementInputFact.EventSerial)
	{
		return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("event precedes the consumed event watermark"));
	}
	if (Fact.Kind != EGGYGOMovementInputFactKind::SessionInvalidated
		&& (!Binding.SourceSession.Producer.IsValid() || IsBeingDestroyed() || !IsValid(GetOwner())))
	{
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("source or consumer is no longer live"));
	}
	if ((Fact.Kind == EGGYGOMovementInputFactKind::SessionInvalidated
		|| Fact.Kind == EGGYGOMovementInputFactKind::SourceUnresolved) && Fact.Reason.IsNone())
	{
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("invalidation/unresolved fact requires a source reason"));
	}
	if (Fact.Kind != EGGYGOMovementInputFactKind::SessionOpened
		&& Fact.Kind != EGGYGOMovementInputFactKind::SessionInvalidated && !bMovementInputSessionOpened)
	{
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("SessionOpened has not been consumed for this grant"));
	}

	switch (Fact.Kind)
	{
	case EGGYGOMovementInputFactKind::SessionOpened:
		if (bMovementInputSessionOpened || Fact.Request.RequestSerial != 0)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("SessionOpened is repeated or carries a request"));
		}
		bMovementInputSessionOpened = true;
		ConsumedMovementInputSessionMode = Fact.SessionMode;
		bMovementInputColdStartWindowOpen = Fact.SessionMode == EGGYGOMovementInputSessionMode::Cold
			&& MovementInputBinding.ConsumerBindingSerial == 1
			&& LocomotionRequestAdmission != ELocomotionRequestAdmission::Failed;
		break;
	case EGGYGOMovementInputFactKind::NeutralConfirmed:
		if (bMovementInputRequestOpen)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("neutral cannot replace the matching request release, including after execution failure"));
		}
		// Source proof only. It neither clears a failed request nor grants execution.
		bMovementInputNeutralConsumed = true;
		break;
	case EGGYGOMovementInputFactKind::RequestStarted:
	{
		if (Fact.Request.RequestSerial == 0)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("RequestStarted requires an allocated input request"));
		}
		if (Fact.Request.RequestSerial <= LastMovementInputRequestSerial)
		{
			return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("input request has already been consumed or retired"));
		}
		if (bMovementInputRequestOpen)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("RequestStarted cannot replace an unreleased source request"));
		}
		if (Fact.StartProof == EGGYGOMovementInputStartProof::ColdPhysicalPress)
		{
			if (ConsumedMovementInputSessionMode != EGGYGOMovementInputSessionMode::Cold
				|| !bMovementInputColdStartWindowOpen || LastMovementInputRequestSerial != 0
				|| LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
			{
				return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("ColdPhysicalPress requires the unused explicit Cold window and cannot rearm FAILED"));
			}
		}
		else if (!bMovementInputNeutralConsumed)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("ReleasedThenPhysicalPress requires consumed real Neutral and no unreleased source request"));
		}
		// Consume the start window before execution admission/configuration can fail.
		bMovementInputColdStartWindowOpen = false;
		if (LocomotionRequestSerial == MAX_uint64)
		{
			FailLocomotionRequest(LocomotionRequestSerial, TEXT("CMC execution request serial is exhausted"));
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("CMC execution request serial is exhausted"));
		}
		const TSharedPtr<FMovementOwnerSyncContext> OwnerContext = MovementOwnerSyncContext;
		const bool bHasOriginalOwnerScope = IsMovementOwnerSyncContextCurrent(OwnerContext);
		const bool bNeedsNativeOwnerReady = CharacterOwner && !CharacterOwner->HasAuthority()
			&& CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy;
		const bool bWaitForInitialOwner = bNeedsNativeOwnerReady && bHasOriginalOwnerScope
			&& OwnerContext->bRequiresNativeResponse && OwnerContext->bInitialLocalScope
			&& OwnerContext->Notice.State == EGGYGOMovementOwnerSyncState::Waiting
			&& OwnerContext->Notice.bInitialSynchronizationEligible;
		RevokeMovementInputRequest();
		LastMovementInputRequestSerial = Fact.Request.RequestSerial;
		MovementInputRequest = Fact.Request;
		MovementInputRequestOwnerScope = bHasOriginalOwnerScope
			? OwnerContext->Scope : FGGYGOMovementOwnerSyncScopeId{};
		bMovementInputNeutralConsumed = false;
		bMovementInputRequestOpen = true;
		++LocomotionRequestSerial;
		LocomotionRequestAdmission = bWaitForInitialOwner
			? ELocomotionRequestAdmission::Waiting : ELocomotionRequestAdmission::Admitted;
		LocomotionRequestFailureReason.Reset();
		// Waiting is a legitimate blocked state, not the ordinary missing-source error.
		bMovementInputAdmissionDiagnosticReported = bWaitForInitialOwner;
		const bool bKeepRunIntent = bWantsRunOnNextMove;
		const bool bKeepForceWalk = bForceWalkRequested;
		ResetLocomotionState();
		bWantsRunOnNextMove = bKeepRunIntent;
		bForceWalkRequested = bKeepForceWalk;
		LastMovementInputFact = Fact;
		RecordMovementInputSourceCheckpoint(Fact);
		if (bNeedsNativeOwnerReady && (!bHasOriginalOwnerScope
			|| (!bWaitForInitialOwner && (OwnerContext->Notice.State != EGGYGOMovementOwnerSyncState::Ready
				|| OwnerContext->Notice.ServerOwnerGeneration == 0
				|| OwnerContext->Notice.NativeResponseNonce != OwnerContext->Scope.ResponseNonce))
			|| (Fact.StartProof == EGGYGOMovementInputStartProof::ColdPhysicalPress
				&& !OwnerContext->Notice.bInitialSynchronizationEligible)))
		{
			FailLocomotionRequest(LocomotionRequestSerial,
				TEXT("source request lacks its current native owner Ready or eligible ordinary initial wait; recovery requires real Release then Press"));
			OutError = LocomotionRequestFailureReason;
		}
		else if (!HasAcceptedMovementSet())
		{
			FailLocomotionRequest(LocomotionRequestSerial, TEXT("new source request has no accepted MovementSet"));
			OutError = LocomotionRequestFailureReason;
		}
		else if (bWaitForInitialOwner)
		{
			UE_LOG(LogGGYGOMovement, Verbose,
				TEXT("Movement.InitialAdmission waiting: Consumer='%s', Producer='%s', Binding=%llu, Session=%llu, InputRequest=%llu, ExecutionRequest=%llu, Scope=%llu, Nonce=%llu, Reason='ordinary first native owner synchronization; execution remains blocked'."),
				*GetPathName(), *GetPathNameSafe(Fact.Request.Session.Producer.Get()),
				static_cast<unsigned long long>(Binding.ConsumerBindingSerial),
				static_cast<unsigned long long>(Fact.Request.Session.SessionSerial),
				static_cast<unsigned long long>(Fact.Request.RequestSerial),
				static_cast<unsigned long long>(LocomotionRequestSerial),
				static_cast<unsigned long long>(MovementInputRequestOwnerScope.ScopeSerial),
				static_cast<unsigned long long>(MovementInputRequestOwnerScope.ResponseNonce));
		}
		PublishQualifiedMovementIntent();
		return EGGYGOMovementInputConsumeResult::Recorded;
	}
	case EGGYGOMovementInputFactKind::RequestReleased:
		if (Fact.Request.RequestSerial == 0)
		{
			return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("RequestReleased requires an allocated input request"));
		}
		if (Fact.Request != MovementInputRequest)
		{
			return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("release does not name the current execution request source"));
		}
		if (!bMovementInputRequestOpen)
		{
			return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("source request release has already been consumed"));
		}
		bMovementInputRequestOpen = false;
		bMovementInputNeutralConsumed = false;
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Waiting)
		{
			RevokeMovementInputRequest(); // No admitted interval exists to manufacture Brake.
		}
		else if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted)
		{
			LocomotionRequestAdmission = ELocomotionRequestAdmission::Released;
		}
		break;
	case EGGYGOMovementInputFactKind::SessionInvalidated:
	{
		LastMovementInputFact = Fact;
		const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
		if (!InvalidateMovementInputSession(Binding, Fact.Reason, OutError))
		{
			return EGGYGOMovementInputConsumeResult::Rejected;
		}
		// Synchronous owner invalidation can end this consumer or install a successor.
		if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
			Self && !Self->bMovementOwnerSyncClosed && !Self->IsBeingDestroyed())
		{
			Self->RecordMovementInputSourceCheckpoint(Fact); // Original fact/binding guard rejects a successor.
		}
		return EGGYGOMovementInputConsumeResult::Recorded;
	}
	case EGGYGOMovementInputFactKind::SourceUnresolved:
		if (Fact.Request.RequestSerial != 0 && Fact.Request != MovementInputRequest)
		{
			return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("unresolved fact names an older request source"));
		}
		bMovementInputNeutralConsumed = false;
		RevokeMovementInputRequest();
		LastMovementInputFact = Fact;
		RecordMovementInputSourceCheckpoint(Fact);
		{
			const auto Result = Report(EGGYGOMovementInputConsumeResult::Recorded, *Fact.Reason.ToString());
			PublishQualifiedMovementIntent();
			return Result;
		}
	default:
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("unsupported fact kind"));
	}
	LastMovementInputFact = Fact;
	RecordMovementInputSourceCheckpoint(Fact);
	PublishQualifiedMovementIntent();
	return EGGYGOMovementInputConsumeResult::Recorded;
}

void UGGYGOCharacterMovementComponent::CancelMovementInputLocomotion()
{
	const auto CancelOwned = [](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
	{
		for (const TSharedPtr<FRootMotionSource>& Source : Sources)
		{
			if (Source.IsValid() && IsOwnedLocomotionCurveSource(*Source))
			{
				// Native accumulation does not skip MarkedForRemoval in this interval.
				Source->RootMotionParams.Set(FTransform::Identity);
				Source->Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
			}
		}
	};
	CancelOwned(CurrentRootMotion.RootMotionSources);
	CancelOwned(CurrentRootMotion.PendingAddRootMotionSources);
	RetireLocomotionCurveRootMotion();
	CurveMotion.Reset();
	if (IsMovingOnGround() && !HasIndependentGroundRootMotion() && !HasNativePassiveGroundVelocity())
	{
		Velocity = Velocity.ContainsNaN() ? FVector::ZeroVector : Velocity - ProjectToGravityFloor(Velocity);
	}
}

void UGGYGOCharacterMovementComponent::RevokeMovementInputRequest()
{
	bMovementInputColdStartWindowOpen = false;
	const bool bWasWaiting = LocomotionRequestAdmission == ELocomotionRequestAdmission::Waiting;
	if (LocomotionRequestAdmission != ELocomotionRequestAdmission::Failed)
	{
		LocomotionRequestAdmission = ELocomotionRequestAdmission::Revoked;
		MovementInputRequestOwnerScope = {};
	}
	if (bWasWaiting) bMovementInputAdmissionDiagnosticReported = false;
	CancelMovementInputLocomotion();
}

bool UGGYGOCharacterMovementComponent::FailLocomotionRequest(uint64 ExpectedRequestSerial, const FString& Reason)
{
	if (ExpectedRequestSerial == 0 || ExpectedRequestSerial != LocomotionRequestSerial || Reason.IsEmpty())
	{
		return false;
	}
	bMovementInputColdStartWindowOpen = false;
	if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
	{
		return true;
	}
	LocomotionRequestAdmission = ELocomotionRequestAdmission::Failed;
	LocomotionRequestFailureReason = FString::Printf(
		TEXT("Movement execution request rejected: Consumer='%s', Owner='%s', Producer='%s', Session=%llu, InputRequest=%llu, ExecutionRequest=%llu, MovementSet='%s', Reason='%s'."),
		*GetPathName(), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(MovementInputRequest.Session.Producer.Get()),
		static_cast<unsigned long long>(MovementInputRequest.Session.SessionSerial),
		static_cast<unsigned long long>(MovementInputRequest.RequestSerial),
		static_cast<unsigned long long>(LocomotionRequestSerial), *GetPathNameSafe(MovementSet.Get()), *Reason);
	bMovementInputAdmissionDiagnosticReported = true;
	CancelMovementInputLocomotion();
	UE_LOG(LogGGYGOMovement, Error, TEXT("%s"), *LocomotionRequestFailureReason);
	return true;
}

bool UGGYGOCharacterMovementComponent::IsMovementInputRequestBlocked() const
{
	const bool bOriginalReplay = MovementInputReplayCapture.IsSet()
		&& (PreparingLocomotionCurveReplayGroup != nullptr || (CharacterOwner && CharacterOwner->bClientUpdating));
	if (bOriginalReplay)
	{
		const FMovementInputReplayCapture& Capture = MovementInputReplayCapture.GetValue();
		if (Capture.Request.Binding.ConsumerBindingSerial != 0)
		{
			if (!Capture.Request.bExecutionEligible || Capture.Request.ExecutionRequestSerial == 0
				|| Capture.Request.ExecutionRequestSerial != LocomotionRequestSerial
				|| Capture.Request.Binding != MovementInputBinding || Capture.Request.Request != MovementInputRequest
				|| !IsMovementInputBindingCurrent(Capture.Request.Binding)
				|| (Capture.Request.Admission != static_cast<uint8>(ELocomotionRequestAdmission::Admitted)
					&& Capture.Request.Admission != static_cast<uint8>(ELocomotionRequestAdmission::Released))
				|| !Capture.Checkpoint.bPresent || Capture.Checkpoint.bConsumerInvalidated
				|| Capture.Checkpoint.BindingSerial != Capture.Request.Binding.ConsumerBindingSerial
				|| Capture.Checkpoint.SessionSerial != Capture.Request.Request.Session.SessionSerial
				|| Capture.Checkpoint.RequestSerial != Capture.Request.Request.RequestSerial) return true;
			if (Capture.OwnerScope.GetScopeSerial() != 0)
			{
				const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
				if (!IsMovementOwnerSyncContextCurrent(Context) || Capture.OwnerScope != Context->Scope
					|| Capture.OwnerScope != MovementInputRequestOwnerScope
					|| (Context->bRequiresNativeResponse && (Capture.OwnerGeneration == 0
						|| Capture.OwnerGeneration != Context->Notice.ServerOwnerGeneration))) return true;
			}
		}
		else if (MovementInputBindingSerial != 0 || (CharacterOwner && !CharacterOwner->HasAuthority()
			&& CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy)) return true;
	}
	const bool bNativeRemoteOwner = CharacterOwner && CharacterOwner->HasAuthority()
		&& !CharacterOwner->IsLocallyControlled() && Cast<APlayerController>(CharacterOwner->GetController());
	if (ActiveMovementOwnerSyncNativeMove || MovementInputNativeSource.IsValid() || bNativeRemoteOwner)
	{
		const TSharedPtr<const FMovementInputNativeSource> Source = MovementInputNativeSource;
		const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
		if (!Source.IsValid() || Source->ExecutionRequestSerial == 0
			|| Source->ExecutionRequestSerial != LocomotionRequestSerial
			|| !IsMovementOwnerSyncReceiptCurrent(Source->Receipt)
			|| !IsMovementOwnerSyncContextCurrent(Context) || Source->OwnerScope != Context->Scope
			|| Source->OwnerScope != MovementInputRequestOwnerScope
			|| (LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
				&& LocomotionRequestAdmission != ELocomotionRequestAdmission::Released)) return true;
		if (const FMovementOwnerSyncNativeMove* Frame = ActiveMovementOwnerSyncNativeMove)
		{
			if (!Frame->bEnteredNativeSimulation || !Frame->bSourceExecutionApplicable
				|| Frame->Move != GetCurrentNetworkMoveData() || !IsMovementOwnerSyncReceiptCurrent(Frame->Receipt)
				|| Frame->Receipt.ClientOwnerGeneration != Frame->Receipt.OwnerGeneration
				|| Frame->OriginalNonce != Source->Receipt.Nonce
				|| Frame->OriginalSourceCheckpoint != Frame->Move->MovementInputSourceCheckpoint
				|| Frame->OriginalSourceCheckpoint.BindingSerial != Source->StartCheckpoint.BindingSerial
				|| Frame->OriginalSourceCheckpoint.SessionSerial != Source->StartCheckpoint.SessionSerial
				|| Frame->OriginalSourceCheckpoint.RequestSerial != Source->StartCheckpoint.RequestSerial) return true;
		}
		return false;
	}
	// Lifetime serial 0 means this legacy call chain has not yet been migrated to the source interface.
	if (MovementInputBindingSerial == 0) return false;
	if (!IsMovementInputBindingCurrent(MovementInputBinding) || !bMovementInputSessionOpened
			|| !MovementInputBinding.SourceSession.Producer.IsValid()
			|| (LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
				&& LocomotionRequestAdmission != ELocomotionRequestAdmission::Released)) return true;
	if (MovementInputRequestOwnerScope.ScopeSerial != 0)
	{
		const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
		if (!IsMovementOwnerSyncContextCurrent(Context) || MovementInputRequestOwnerScope != Context->Scope) return true;
		if (Context->bRequiresNativeResponse
			&& (Context->Notice.State != EGGYGOMovementOwnerSyncState::Ready
				|| Context->Notice.ServerOwnerGeneration == 0
				|| Context->Notice.NativeResponseNonce != Context->Scope.ResponseNonce)) return true;
	}
	else if (CharacterOwner && !CharacterOwner->HasAuthority()
		&& CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy)
	{
		return true; // An autonomous request cannot borrow the authority/AI no-scope mode.
	}
	// Local authority/AI with no player sync scope retains its existing legal admission.
	return false;
}

bool UGGYGOCharacterMovementComponent::ShouldRejectMovementInputGroundLocomotion() const
{
	return IsMovingOnGround() && IsMovementInputRequestBlocked() && !HasIndependentGroundRootMotion();
}

bool UGGYGOCharacterMovementComponent::ShouldRejectUnownedCurveGroundLocomotion() const
{
	return IsMovingOnGround() && HasAcceptedMovementSet() && MovementSet->bUseCurveDrivenSpeed
		&& CharacterOwner && CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy
		&& LocomotionRequestSerial == 0 && !HasIndependentGroundRootMotion();
}

bool UGGYGOCharacterMovementComponent::RejectLocomotionEvaluation(uint64 ExpectedRequestSerial, const FString& Reason)
{
	if (ExpectedRequestSerial != 0)
	{
		// A stale execution failure must not clear a successor's sample or sources.
		FailLocomotionRequest(ExpectedRequestSerial, Reason);
	}
	else if (LocomotionRequestSerial == 0)
	{
		CancelMovementInputLocomotion();
		EnforceGroundLocomotionAdmission(TEXT("RejectLocomotionEvaluation"));
	}
	return false;
}

bool UGGYGOCharacterMovementComponent::HasNativePassiveGroundVelocity() const
{
	if (!IsValid(CharacterOwner) || !IsValid(UpdatedComponent) || Velocity.ContainsNaN()) return false;
	if (bNativeVelocityIntervalOpen && bNativeVelocityIntervalCanRetain && bMovementInProgress
		&& NativeVelocityIntervalCharacter.Get() == CharacterOwner
		&& NativeVelocityIntervalComponent.Get() == UpdatedComponent)
	{
		// Collision, floor projection and native braking may change velocity inside this one interval.
		return true;
	}
	if (!NativeMovementVelocityResult.IsSet()) return false;
	const FNativeMovementVelocityResult& Result = NativeMovementVelocityResult.GetValue();
	return Result.Character.IsValid() && Result.Character.Get() == CharacterOwner
		&& Result.Component.IsValid() && Result.Component.Get() == UpdatedComponent
		&& !Result.Velocity.ContainsNaN()
		&& ProjectToGravityFloor(Velocity).Equals(ProjectToGravityFloor(Result.Velocity), KINDA_SMALL_NUMBER);
}

void UGGYGOCharacterMovementComponent::BeginNativeMovementVelocityInterval()
{
	// An interval cannot carry a previous interval's in-progress permission across an owner change.
	NativeVelocityBeforeRootMotion.Reset();
	bNativeVelocityIntervalOpen = false;
	bNativeVelocityIntervalCanRetain = HasNativePassiveGroundVelocity();
	NativeVelocityIntervalCharacter = CharacterOwner;
	NativeVelocityIntervalComponent = UpdatedComponent.Get();
	bNativeVelocityIntervalOpen = IsValid(CharacterOwner) && IsValid(UpdatedComponent);
}

void UGGYGOCharacterMovementComponent::MarkNativeMovementVelocityResult()
{
	if (bNativeVelocityIntervalOpen && NativeVelocityIntervalCharacter.IsValid()
		&& NativeVelocityIntervalCharacter.Get() == CharacterOwner
		&& NativeVelocityIntervalComponent.IsValid() && NativeVelocityIntervalComponent.Get() == UpdatedComponent)
	{
		bNativeVelocityIntervalCanRetain = true;
	}
}

void UGGYGOCharacterMovementComponent::StoreNativeMovementVelocityResult()
{
	NativeMovementVelocityResult.Reset();
	if (!IsValid(CharacterOwner) || !IsValid(UpdatedComponent) || Velocity.ContainsNaN()) return;
	FNativeMovementVelocityResult Result;
	Result.Character = CharacterOwner;
	Result.Component = UpdatedComponent.Get();
	Result.Velocity = Velocity;
	NativeMovementVelocityResult = MoveTemp(Result);
}

void UGGYGOCharacterMovementComponent::EnforceMovementInputLocomotionAdmission(const TCHAR* Entry)
{
	if (!ShouldRejectMovementInputGroundLocomotion()) return;
	const bool bNativeMomentum = HasNativePassiveGroundVelocity();
	const FRootMotionSource* UnsupportedSource = GetUnsupportedGroundRootMotionSource();
	const bool bHasRequest = Acceleration.ContainsNaN() || ProjectToGravityFloor(Acceleration).SizeSquared() > KINDA_SMALL_NUMBER
		|| (bHasRequestedVelocity && (RequestedVelocity.ContainsNaN()
			|| ProjectToGravityFloor(RequestedVelocity).SizeSquared() > KINDA_SMALL_NUMBER))
		|| (!bNativeMomentum && (Velocity.ContainsNaN() || ProjectToGravityFloor(Velocity).SizeSquared() > KINDA_SMALL_NUMBER))
		|| UnsupportedSource;
	if (bHasRequest && !bMovementInputAdmissionDiagnosticReported)
	{
		bMovementInputAdmissionDiagnosticReported = true;
		UE_LOG(LogGGYGOMovement, Error,
			TEXT("Movement source admission rejected: Consumer='%s', Producer='%s', Binding=%llu, Session=%llu, ExecutionRequest=%llu, MovementSet='%s', SourceReason='%s', Entry='%s', Mode=%d, Acceleration='%s', HasRequestedVelocity=%d, RequestedVelocity='%s', Velocity='%s', NativeMomentum=%d, LastRetiredAction=%d, UnsupportedSource='%s', SourceType='%s', Reason='no live admitted source request; reset, zero acceleration and neutral cannot rearm it'."),
			*GetPathName(), *GetPathNameSafe(MovementInputBinding.SourceSession.Producer.Get()),
			static_cast<unsigned long long>(MovementInputBindingSerial),
			static_cast<unsigned long long>(MovementInputBinding.SourceSession.SessionSerial),
			static_cast<unsigned long long>(LocomotionRequestSerial), *GetPathNameSafe(MovementSet.Get()),
			*LastMovementInputFact.Reason.ToString(), Entry, static_cast<int32>(MovementMode),
			*Acceleration.ToString(), bHasRequestedVelocity ? 1 : 0, *RequestedVelocity.ToString(),
			*Velocity.ToString(), bNativeMomentum ? 1 : 0, LastRetiredMontageActionHandle,
			UnsupportedSource ? *UnsupportedSource->InstanceName.ToString() : TEXT("None"),
			*GetNameSafe(UnsupportedSource ? UnsupportedSource->GetScriptStruct() : nullptr));
	}
	if (!bNativeMomentum)
		Velocity = Velocity.ContainsNaN() ? FVector::ZeroVector : Velocity - ProjectToGravityFloor(Velocity);
}

bool UGGYGOCharacterMovementComponent::IsMontageActionMovementModeSupported() const
{
	return IsMovingOnGround() || IsFalling();
}

bool UGGYGOCharacterMovementComponent::BeginMontageActionMotion(
	const FGGYGOActionMotionSourceBindingPtr& OriginalSource, int32 OriginalMontageInstanceId,
	float MontagePositionSeconds, float EffectiveMontagePlayRate, float TranslationScale,
	int32& OutHandle, FString& OutError)
{
	OutHandle = INDEX_NONE;
	OutError.Reset();
	const auto Reject = [this, &OriginalSource, &OutError](const FString& Reason)
	{
		OutError = FString::Printf(TEXT("[Movement.ActionMotion] CMC='%s' Montage='%s' Slot='%s' Section='%s' Reason='%s'"),
			*GetPathName(), OriginalSource.IsValid() ? *GetPathNameSafe(OriginalSource->Montage.Get()) : TEXT("None"),
			OriginalSource.IsValid() ? *OriginalSource->SlotName.ToString() : TEXT("None"),
			OriginalSource.IsValid() ? *OriginalSource->SectionName.ToString() : TEXT("None"), *Reason);
		return false;
	};
	if (!IsInGameThread() || !IsValid(CharacterOwner) || IsBeingDestroyed() || !IsMontageActionMovementModeSupported()
		|| (!CharacterOwner->HasAuthority() && (CharacterOwner->GetLocalRole() != ROLE_AutonomousProxy
			|| !CharacterOwner->IsLocallyControlled())) || CharacterOwner->bClientUpdating)
		return Reject(TEXT("a live authority or original autonomous owner in Walking, NavWalking or Falling is required"));
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(CharacterOwner);
	const auto OriginalOwnerContext = MovementOwnerSyncContext;
	const int32 OriginalNextHandle = NextActionMotionHandle;
	CleanupFinishedActionMotion();
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || !OriginalCharacter.IsValid()
		|| Self->CharacterOwner != OriginalCharacter.Get() || Self->MovementOwnerSyncContext != OriginalOwnerContext
		|| Self->NextActionMotionHandle != OriginalNextHandle)
	{
		OutError = TEXT("[Movement.ActionMotion] original owner/action slot changed during prior action retirement");
		return false;
	}
	if (HasActiveActionMotion() || NextActionMotionHandle <= 0 || NextActionMotionHandle == MAX_int32)
		return Reject(TEXT("action execution slot is occupied or original token allocator is exhausted"));
	FString Error;
	if (!OriginalSource.IsValid() || !GGYGOActionMotionEvaluation::ValidateSource(*OriginalSource, Error))
		return Reject(Error.IsEmpty() ? TEXT("original source binding is missing") : Error);
	if (!FMath::IsFinite(MontagePositionSeconds) || MontagePositionSeconds < OriginalSource->MontageStartSeconds
		|| MontagePositionSeconds >= OriginalSource->MontageEndSeconds
		|| !FMath::IsFinite(EffectiveMontagePlayRate) || EffectiveMontagePlayRate <= 0.0f
		|| !FMath::IsFinite(TranslationScale) || TranslationScale < 0.0f)
		return Reject(TEXT("original position must be in the section; rate positive and translation scale non-negative, all finite"));
	USkeletalMeshComponent* Mesh = CharacterOwner->GetMesh();
	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	FAnimMontageInstance* Instance = Anim ? Anim->GetMontageInstanceForID(OriginalMontageInstanceId) : nullptr;
	const FGGYGOActionMotionSourceSection* InitialSection = Instance
		? OriginalSource->FindSection(Instance->GetCurrentSection()) : nullptr;
	if (!IsValid(Mesh) || !IsValid(Anim) || !Instance || Instance->Montage != OriginalSource->Montage.Get()
		|| !Instance->IsActive() || !Instance->IsPlaying() || !InitialSection
		|| MontagePositionSeconds < InitialSection->MontageStartSeconds
		|| MontagePositionSeconds >= InitialSection->MontageEndSeconds
		|| !FMath::IsNearlyEqual(Instance->GetPosition(), MontagePositionSeconds, UE_KINDA_SMALL_NUMBER)
		|| !FMath::IsFinite(Instance->GetPlayRate()) || Instance->GetPlayRate() <= 0.0f
		|| !FMath::IsNearlyEqual(Instance->GetPlayRate() * OriginalSource->MontageRateScale,
			EffectiveMontagePlayRate, UE_KINDA_SMALL_NUMBER) || CharacterOwner->IsPlayingRootMotion())
		return Reject(TEXT("original Montage/instance/section/position/rate is not current or native animation RootMotion is active"));
	const float Duration = (OriginalSource->MontageEndSeconds - MontagePositionSeconds) / EffectiveMontagePlayRate;
	const FQuat Rotation = CharacterOwner->GetActorQuat();
	const FVector Scale = Mesh->GetComponentScale() * TranslationScale; // Original avatar/Actor scale once; Actor basis, no Mesh rotation.
	if (!FMath::IsFinite(Duration) || Duration <= 0.0f || Rotation.ContainsNaN()
		|| !Rotation.IsNormalized() || Scale.ContainsNaN())
		return Reject(TEXT("native duration or original Actor transform cannot be represented"));
	const auto Resource = MakeShared<FGGYGOActionMotionResource>();
	Resource->Owner = this;
	Resource->Character = CharacterOwner;
	Resource->Mesh = Mesh;
	Resource->AnimInstance = Anim;
	Resource->Source = OriginalSource;
	Resource->MontageInstanceId = OriginalMontageInstanceId;
	Resource->InstancePlayRate = Instance->GetPlayRate();
	Resource->Handle = NextActionMotionHandle;
	const auto Source = MakeShared<FRootMotionSource_GGYGOActionCurve>();
	Source->SourceMode = EGGYGOActionCurveSourceMode::OriginalMontage;
	// The original XYZ trajectory owns all velocity axes while its native interval is active.
	// Sensitive native liftoff lets authored upward motion leave Walking without a jump impulse.
	Source->Settings.UnSetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
	Source->Settings.SetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
	Source->OriginalBinding = OriginalSource;
	Source->OriginalResource = Resource;
	Source->MontageStartSeconds = MontagePositionSeconds;
	Source->EntryActorRotation = Rotation;
	Source->TranslationScale = Scale;
	Source->PlayRate = EffectiveMontagePlayRate;
	Source->Duration = Duration;
	Source->InstanceName = GGYGOMovementConstants::ActionCurveSourceName;
	Source->Priority = GGYGOMovementConstants::ActionCurvePriority;
	// Retire only the previous completed native tail before its successor enters this one execution slot.
	if (CompletedMontageActionResource.IsValid()) LastRetiredMontageActionHandle = CompletedMontageActionResource->Handle;
	NeutralizeMontageActionSource(CompletedMontageActionResource);
	CompletedMontageActionResource.Reset();
	bCompletedMontageActionNativeContributionConsumed = false;
	const auto RetiredActionFailureCallback = MoveTemp(ActionMotionFailureCallback);
	ActionMotionFailureResource.Reset();
	const auto RetiredActionCompletionCallback = MoveTemp(ActionMotionCompletionCallback);
	ActionMotionCompletionResource.Reset();
	bMontageActionCompletionObserverInstalled = false;
	MontageActionNaturalBlendOutResource.Reset();
	const uint16 Id = ApplyRootMotionSource(Source);
	if (Id == static_cast<uint16>(ERootMotionSourceID::Invalid))
		return Reject(TEXT("native ApplyRootMotionSource rejected the original source"));
	ActiveMontageActionResource = Resource;
	ActiveActionMotionHandle = Resource->Handle;
	ActionMotionSourceID = Id;
	++NextActionMotionHandle;
	RetireLocomotionCurveRootMotion();
	RemoveRootMotionSource(GGYGOMovementConstants::CurveBrakeSourceName);
	RemoveRootMotionSource(GGYGOMovementConstants::CurveTurnBackSourceName);
	ResetTurnBack();
	bReplicatedTurnBackCurveDriven = false;
	ResolvedGait = EGGYGOGait::None;
	WalkHoldTimer = 0.0f;
	CurveMotion.Reset();
	StopMovementImmediately();
	OutHandle = Resource->Handle;
	return true;
}

bool UGGYGOCharacterMovementComponent::ValidateMontageActionRuntime(
	const FRootMotionSource_GGYGOActionCurve& Source, float SimulationTime, FString& OutError) const
{
	OutError.Reset();
	if (!IsValid(CharacterOwner) || !IsMontageActionMovementModeSupported() || !Source.OriginalBinding.IsValid()
		|| CharacterOwner->IsPlayingRootMotion())
	{
		OutError = TEXT("original XYZ action requires Walking, NavWalking or Falling, its source and no native animation RootMotion conflict");
		return false;
	}
	if (Source.Settings.HasFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate)
		|| !Source.Settings.HasFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck))
	{
		OutError = TEXT("original XYZ action source must override Z and use native sensitive liftoff");
		return false;
	}
	const auto Resource = Source.OriginalResource;
	if (!Resource.IsValid())
	{
		// Native authoritative RMS replication may drive clients, never grant authority-side actions.
		if (Source.bNativeImported && !CharacterOwner->HasAuthority()) return true;
		OutError = TEXT("original local execution resource is missing; imported data cannot grant authority execution");
		return false;
	}
	if (Resource->Owner.Get() != this || Resource->Character.Get() != CharacterOwner
		|| Resource->Source != Source.OriginalBinding)
	{
		OutError = TEXT("source does not belong to its original CMC, Character and immutable binding");
		return false;
	}
	// The saved native source already captures the original validated mapping and release state.
	// Replay never borrows today's AnimInstance/Montage position or a successor's resource.
	if (CharacterOwner->bClientUpdating) return true;
	UAnimInstance* Anim = Resource->AnimInstance.Get();
	USkeletalMeshComponent* Mesh = Resource->Mesh.Get();
	if (!IsValid(Mesh) || Mesh != CharacterOwner->GetMesh() || !IsValid(Anim) || Mesh->GetAnimInstance() != Anim)
	{
		OutError = TEXT("original Mesh/AnimInstance membership was retired or replaced");
		return false;
	}
	// The original Task's authenticated Completed request allows only this finite native tail.
	// It is not the consumed-final-interval fact, and never looks up a successor instance.
	if (Source.bCompletionRequested && CompletedMontageActionResource == Resource) return true;
	FAnimMontageInstance* Instance = Anim ? Anim->GetMontageInstanceForID(Resource->MontageInstanceId) : nullptr;
	const FGGYGOActionMotionSourceSection* CurrentSection = Instance
		? Source.OriginalBinding->FindSection(Instance->GetCurrentSection()) : nullptr;
	// Only the original Task's non-interrupted native BlendOut fact permits stopped playback.
	// UE still advances during that configured blend window, then holds End - KINDA_SMALL_NUMBER/2.
	// A stopped hold can consume only the final native interval; a pause/foreign Stop has no grant.
	const bool bNaturalBlendOut = Instance && CurrentSection && Instance->IsStopped()
		&& MontageActionNaturalBlendOutResource == Resource && Source.OriginalBinding->Sections.Num() > 0
		&& CurrentSection == &Source.OriginalBinding->Sections.Last()
		&& FMath::IsFinite(Instance->GetPosition())
		&& (Instance->IsPlaying()
			|| (FMath::IsNearlyEqual(Instance->GetPosition(), Source.OriginalBinding->MontageEndSeconds, UE_KINDA_SMALL_NUMBER)
				&& Source.GetTime() + SimulationTime >= Source.Duration));
	if (!Instance || Instance->Montage != Source.OriginalBinding->Montage.Get()
		|| !FMath::IsFinite(Instance->GetPosition()) || !CurrentSection
		|| Instance->GetPosition() < CurrentSection->MontageStartSeconds
		|| Instance->GetPosition() > CurrentSection->MontageEndSeconds
		|| ((!Instance->IsActive() || !Instance->IsPlaying()) && !bNaturalBlendOut)
		|| !FMath::IsFinite(Instance->GetPlayRate()) || Instance->GetPlayRate() != Resource->InstancePlayRate
		|| (!Source.bCompletionRequested && ActiveMontageActionResource != Resource))
	{
		OutError = TEXT("original runtime identity/playback/rate was retired, replaced, paused or dynamically changed");
		return false;
	}
	return true;
}

bool UGGYGOCharacterMovementComponent::NotifyMontageActionNaturalBlendOut(
	int32 OriginalHandle, int32 OriginalMontageInstanceId, FString& OutError)
{
	OutError.Reset();
	const auto Resource = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle
		? ActiveMontageActionResource : (CompletedMontageActionResource.IsValid()
			&& CompletedMontageActionResource->Handle == OriginalHandle ? CompletedMontageActionResource : nullptr);
	if (!IsInGameThread() || IsBeingDestroyed() || !Resource.IsValid()
		|| Resource->Owner.Get() != this || Resource->Character.Get() != CharacterOwner
		|| Resource->MontageInstanceId != OriginalMontageInstanceId || !Resource->Source.IsValid()
		|| Resource->Source->Sections.Num() == 0)
	{
		OutError = TEXT("[Movement.ActionMotion] natural BlendOut fact requires the original live handle and Montage instance identity");
		return false;
	}
	if (MontageActionNaturalBlendOutResource == Resource) return true;
	// A late original blend fact grants no execution after the genuine final native contribution.
	if (CompletedMontageActionResource == Resource && bCompletedMontageActionNativeContributionConsumed) return true;
	UAnimInstance* Anim = Resource->AnimInstance.Get();
	FAnimMontageInstance* Instance = Anim ? Anim->GetMontageInstanceForID(OriginalMontageInstanceId) : nullptr;
	const UAnimMontage* Montage = Resource->Source->Montage.Get();
	const auto& FinalSection = Resource->Source->Sections.Last();
	const float Rate = Instance ? Instance->GetPlayRate() * Resource->Source->MontageRateScale : 0.0f;
	const float Trigger = Montage && Instance ? (Montage->BlendOutTriggerTime >= 0.0f
		? Montage->BlendOutTriggerTime : Montage->GetDefaultBlendOutTime() * Instance->DefaultBlendTimeMultiplier) : 0.0f;
	if (!Instance || Instance->Montage != Montage || !Instance->IsStopped() || !Instance->bEnableAutoBlendOut
		|| Instance->GetCurrentSection() != FinalSection.SectionName || Instance->GetNextSection() != NAME_None
		|| !FMath::IsFinite(Instance->GetPosition()) || Instance->GetPosition() < FinalSection.MontageStartSeconds
		|| Instance->GetPosition() > FinalSection.MontageEndSeconds || !FMath::IsFinite(Rate) || Rate <= 0.0f
		|| !FMath::IsFinite(Trigger) || Trigger < 0.0f
		|| (FinalSection.MontageEndSeconds - Instance->GetPosition()) / Rate > FMath::Max(Trigger, UE_KINDA_SMALL_NUMBER))
	{
		OutError = TEXT("[Movement.ActionMotion] original non-interrupted BlendOut is outside its configured final-section auto-blend window");
		return false;
	}
	// The original native Task callback authenticates non-interruption; no private engine state is read.
	MontageActionNaturalBlendOutResource = Resource;
	return true;
}

bool UGGYGOCharacterMovementComponent::ObserveMontageActionMotionCompletion(
	int32 OriginalHandle, FGGYGOActionMotionCompletionDelegate Callback, FString& OutError)
{
	OutError.Reset();
	const auto Resource = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle
		? ActiveMontageActionResource : (CompletedMontageActionResource.IsValid()
			&& CompletedMontageActionResource->Handle == OriginalHandle ? CompletedMontageActionResource : nullptr);
	if (!IsInGameThread() || IsBeingDestroyed() || !Resource.IsValid() || !Callback.IsBound()
		|| Resource->Owner.Get() != this || Resource->Character.Get() != CharacterOwner
		|| bMontageActionCompletionObserverInstalled || ActionMotionCompletionCallback.IsBound() || ActionMotionCompletionResource.IsValid())
	{
		OutError = TEXT("[Movement.ActionMotion] completion observer requires the original resource and its sole callback");
		return false;
	}
	bMontageActionCompletionObserverInstalled = true;
	if (CompletedMontageActionResource == Resource && bCompletedMontageActionNativeContributionConsumed)
	{
		Callback.Execute(OriginalHandle);
		return true;
	}
	ActionMotionCompletionResource = Resource;
	ActionMotionCompletionCallback = MoveTemp(Callback);
	return true;
}

void UGGYGOCharacterMovementComponent::PublishMontageActionMotionCompletion()
{
	if (!CharacterOwner || CharacterOwner->bClientUpdating || PreparingLocomotionCurveReplayGroup) return;
	for (const auto& Base : CurrentRootMotion.RootMotionSources)
	{
		if (!Base.IsValid() || Base->GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()) continue;
		const auto& Source = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*Base);
		const auto Resource = Source.OriginalResource;
		if (Source.SourceMode != EGGYGOActionCurveSourceMode::OriginalMontage || Source.bExplicitlyCancelled
			|| !Source.bPreparedNaturalEnd || !Source.bPreparedContributionConsumed || !Resource.IsValid()
			|| Resource->Owner.Get() != this || Resource->Character.Get() != CharacterOwner
			|| (ActiveMontageActionResource != Resource && CompletedMontageActionResource != Resource)
			|| (CompletedMontageActionResource == Resource && bCompletedMontageActionNativeContributionConsumed)) continue;
		CompletedMontageActionResource = Resource;
		bCompletedMontageActionNativeContributionConsumed = true;
		if (ActiveMontageActionResource == Resource)
		{
			ActiveMontageActionResource.Reset();
			ActiveActionMotionHandle = INDEX_NONE;
			ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
		}
		// All native physics for this move has completed. Detach before external cleanup/reentry.
		if (ActionMotionCompletionResource == Resource)
		{
			FGGYGOActionMotionCompletionDelegate Callback = MoveTemp(ActionMotionCompletionCallback);
			ActionMotionCompletionResource.Reset();
			Callback.ExecuteIfBound(Resource->Handle);
		}
		return;
	}
}

bool UGGYGOCharacterMovementComponent::ObserveMontageActionMotionFailure(
	int32 OriginalHandle, FGGYGOActionMotionFailureDelegate Callback, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || !ActiveMontageActionResource.IsValid()
		|| ActiveMontageActionResource->Handle != OriginalHandle || !HasActiveActionMotion()
		|| !Callback.IsBound() || ActionMotionFailureCallback.IsBound() || ActionMotionFailureResource.IsValid())
	{
		OutError = TEXT("[Movement.ActionMotion] failure observer requires the original live action and its sole callback; no replacement");
		return false;
	}
	ActionMotionFailureResource = ActiveMontageActionResource;
	ActionMotionFailureCallback = MoveTemp(Callback);
	return true;
}

void UGGYGOCharacterMovementComponent::FailMontageActionMotion(
	const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error)
{
	if (!OriginalResource.IsValid() || (CharacterOwner && CharacterOwner->bClientUpdating)
		|| (ActiveMontageActionResource != OriginalResource && CompletedMontageActionResource != OriginalResource)) return;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	++MontageActionFailurePreparationDepth;
	{
		FGGYGOActionMotionFailureDelegate Callback;
		if (ActionMotionFailureResource == OriginalResource)
		{
			Callback = MoveTemp(ActionMotionFailureCallback);
			ActionMotionFailureResource.Reset();
		}
		FString ReleaseError;
		ReleaseMontageActionMotion(OriginalResource->Handle, EGGYGOActionMotionReleaseReason::OwnerInvalidated, ReleaseError);
		if (Callback.IsBound()) Callback.Execute(OriginalResource->Handle, Error);
	} // Destroy captures while native Prepare remains protected.
	if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get()) --Self->MontageActionFailurePreparationDepth;
}

void UGGYGOCharacterMovementComponent::NeutralizeMontageActionSource(
	const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource)
{
	if (!OriginalResource.IsValid()) return;
	const auto Retire = [&OriginalResource](const auto& Sources)
	{
		for (const auto& Base : Sources)
		{
			if (!Base.IsValid() || Base->GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()) continue;
			auto* Source = static_cast<FRootMotionSource_GGYGOActionCurve*>(Base.Get());
			if (Source->OriginalResource != OriginalResource) continue;
			Source->bExplicitlyCancelled = true;
			Source->RootMotionParams.Set(FTransform::Identity);
			Source->AccumulateMode = ERootMotionAccumulateMode::Additive;
			Source->Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
		}
	};
	Retire(CurrentRootMotion.RootMotionSources);
	Retire(CurrentRootMotion.PendingAddRootMotionSources);
	if (MontageActionFailurePreparationDepth != 0) return; // Native Prepare is still deriving group flags.
	// Native Prepare recalculates these flags. Also recompute them for a contribution cancelled after Prepare.
	CurrentRootMotion.bHasOverrideSources = false;
	CurrentRootMotion.bHasOverrideSourcesWithIgnoreZAccumulate = false;
	CurrentRootMotion.bHasAdditiveSources = false;
	for (const auto& Base : CurrentRootMotion.RootMotionSources)
	{
		if (!Base.IsValid() || !Base->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)) continue;
		if (Base->AccumulateMode == ERootMotionAccumulateMode::Override)
		{
			CurrentRootMotion.bHasOverrideSources = true;
			CurrentRootMotion.bHasOverrideSourcesWithIgnoreZAccumulate |=
				Base->Settings.HasFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
		}
		else CurrentRootMotion.bHasAdditiveSources = true;
	}
}

bool UGGYGOCharacterMovementComponent::ReleaseMontageActionMotion(
	int32 OriginalHandle, EGGYGOActionMotionReleaseReason Reason, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || OriginalHandle == INDEX_NONE
		|| static_cast<uint8>(Reason) > static_cast<uint8>(EGGYGOActionMotionReleaseReason::OwnerInvalidated))
	{
		OutError = TEXT("[Movement.ActionMotion] release requires the original issued token, valid reason and game thread");
		return false;
	}
	const auto Resource = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle
		? ActiveMontageActionResource : (CompletedMontageActionResource.IsValid()
			&& CompletedMontageActionResource->Handle == OriginalHandle ? CompletedMontageActionResource : nullptr);
	if (!Resource.IsValid())
	{
		if (OriginalHandle == LastRetiredMontageActionHandle) return true;
		OutError = FString::Printf(TEXT("[Movement.ActionMotion] token=%d is not the original active/completed resource"), OriginalHandle);
		return false;
	}
	if (Reason == EGGYGOActionMotionReleaseReason::Completed
		&& (Resource->Owner.Get() != this || Resource->Character.Get() != CharacterOwner || IsBeingDestroyed()))
	{
		OutError = TEXT("[Movement.ActionMotion] natural completion request no longer belongs to its original owner");
		return false;
	}
	FGGYGOActionMotionFailureDelegate RetiredCallback;
	FGGYGOActionMotionCompletionDelegate RetiredCompletionCallback;
	const bool bNativeComplete = CompletedMontageActionResource == Resource
		&& bCompletedMontageActionNativeContributionConsumed;
	if ((Reason != EGGYGOActionMotionReleaseReason::Completed || bNativeComplete)
		&& MontageActionNaturalBlendOutResource == Resource) MontageActionNaturalBlendOutResource.Reset();
	if ((Reason != EGGYGOActionMotionReleaseReason::Completed || bNativeComplete) && ActionMotionFailureResource == Resource)
	{
		RetiredCallback = MoveTemp(ActionMotionFailureCallback);
		ActionMotionFailureResource.Reset();
	}
	if (Reason != EGGYGOActionMotionReleaseReason::Completed && ActionMotionCompletionResource == Resource)
	{
		RetiredCompletionCallback = MoveTemp(ActionMotionCompletionCallback);
		ActionMotionCompletionResource.Reset();
	}
	if (Reason == EGGYGOActionMotionReleaseReason::Completed)
	{
		if (CompletedMontageActionResource == Resource) return true;
		const auto Complete = [&Resource](const auto& Sources)
		{
			for (const auto& Base : Sources)
				if (Base.IsValid() && Base->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
				{
					auto* Source = static_cast<FRootMotionSource_GGYGOActionCurve*>(Base.Get());
					if (Source->OriginalResource == Resource) Source->bCompletionRequested = true;
				}
		};
		Complete(CurrentRootMotion.RootMotionSources);
		Complete(CurrentRootMotion.PendingAddRootMotionSources);
		// The caller reports its authenticated original Task Completed, not a new motion grant.
		// Continue only the original bounded RMS clock until its final contribution is consumed.
		CompletedMontageActionResource = Resource;
		bCompletedMontageActionNativeContributionConsumed = false;
	}
	else
	{
		NeutralizeMontageActionSource(Resource);
		LastRetiredMontageActionHandle = OriginalHandle;
		if (CompletedMontageActionResource == Resource)
		{
			CompletedMontageActionResource.Reset();
			bCompletedMontageActionNativeContributionConsumed = false;
		}
	}
	if (ActiveMontageActionResource == Resource)
	{
		ActiveMontageActionResource.Reset();
		ActiveActionMotionHandle = INDEX_NONE;
		ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
		CurveMotion.Reset();
	}
	// Completed identity remains bounded until the next original action or owner teardown.
	return true;
}

bool UGGYGOCharacterMovementComponent::CancelMontageActionMotionForMovement(
	int32 OriginalHandle, const FGGYGOQualifiedMovementIntent& OriginalIntent, FString& OutError)
{
	FGGYGOQualifiedMovementIntent CurrentIntent;
	if (QueryQualifiedMovementIntent(OriginalIntent.Scope, CurrentIntent, OutError)
		!= EGGYGOQualifiedMovementIntentQueryResult::Qualified || !(CurrentIntent == OriginalIntent))
	{
		if (OutError.IsEmpty()) OutError = TEXT("[Movement.ActionMotion] original movement intent is no longer qualified/current");
		return false;
	}
	const bool bWasActive = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle;
	const auto OriginalResource = bWasActive ? ActiveMontageActionResource
		: (CompletedMontageActionResource.IsValid() && CompletedMontageActionResource->Handle == OriginalHandle
			? CompletedMontageActionResource : nullptr);
	const auto HasUnconsumedTail = [&OriginalResource](const auto& Sources)
	{
		return Sources.ContainsByPredicate([&OriginalResource](const auto& Base)
		{
			if (!Base.IsValid() || Base->GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()) return false;
			const auto& Source = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*Base);
			return OriginalResource.IsValid() && Source.OriginalResource == OriginalResource
				&& !Source.bExplicitlyCancelled && !Source.bPreparedContributionConsumed;
		});
	};
	const bool bHadUnconsumedTail = HasUnconsumedTail(CurrentRootMotion.RootMotionSources)
		|| HasUnconsumedTail(CurrentRootMotion.PendingAddRootMotionSources);
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(CharacterOwner);
	const int32 OriginalNextHandle = NextActionMotionHandle;
	if (!ReleaseMontageActionMotion(OriginalHandle, EGGYGOActionMotionReleaseReason::Cancelled, OutError)) return false;
	// Delegate capture destruction during release can replace the owner or install and end
	// a successor. Only clear this original cancellation's residual on the unchanged owner.
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	FGGYGOQualifiedMovementIntent AfterReleaseIntent;
	FString AfterReleaseError;
	if (Self && !Self->IsBeingDestroyed() && OriginalCharacter.IsValid()
		&& Self->CharacterOwner == OriginalCharacter.Get() && Self->NextActionMotionHandle == OriginalNextHandle
		&& Self->ActiveActionMotionHandle == INDEX_NONE && !Self->ActiveMontageActionResource.IsValid()
		&& (bWasActive || bHadUnconsumedTail)
		&& Self->QueryQualifiedMovementIntent(OriginalIntent.Scope, AfterReleaseIntent, AfterReleaseError)
			== EGGYGOQualifiedMovementIntentQueryResult::Qualified && AfterReleaseIntent == OriginalIntent)
	{
		Self->Velocity.X = 0.0; Self->Velocity.Y = 0.0;
		Self->NativeMovementVelocityResult.Reset();
		Self->NativeVelocityBeforeRootMotion.Reset();
		Self->bNativeVelocityIntervalCanRetain = false;
		// Airborne cancellation retains actual vertical momentum; native Falling resumes gravity.
		// Never restore the entry mode or manufacture a landing to admit this original input.
		Self->CurveMotion.Reset();
	}
	return true;
}

void UGGYGOCharacterMovementComponent::ResumeLocomotionAfterAction()
{
	if (ActionSkippedMovementTickTime <= 0.0f || HasRegisteredActionCurveSource()) return;
	const float OriginalTickTime = ActionSkippedMovementTickTime;
	ActionSkippedMovementTickTime = 0.0f; // The skipped original Before interval can be consumed only once.
	if (CharacterOwner && CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy
		&& HasAcceptedMovementSet() && !ShouldRejectMovementInputGroundLocomotion()
		&& !ShouldRejectUnownedCurveGroundLocomotion())
		TryUpdateLocomotion(OriginalTickTime, true, bPreviousHasMoveInput, ResolvedGait);
}

int32 UGGYGOCharacterMovementComponent::BeginActionMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate)
{
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(CharacterOwner);
	const auto OriginalOwnerContext = MovementOwnerSyncContext;
	const int32 OriginalNextHandle = NextActionMotionHandle;
	CleanupFinishedActionMotion();
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || Self->CharacterOwner != OriginalCharacter.Get()
		|| Self->MovementOwnerSyncContext != OriginalOwnerContext || Self->NextActionMotionHandle != OriginalNextHandle)
		return INDEX_NONE;
	FString Error;
	USkeletalMeshComponent* Mesh = CharacterOwner ? CharacterOwner->GetMesh() : nullptr;
	if (!CharacterOwner || !CharacterOwner->HasAuthority() || !Mesh || !IsMovingOnGround()
		|| HasActiveActionMotion() || CharacterOwner->IsPlayingRootMotion()
		|| NextActionMotionHandle <= 0 || NextActionMotionHandle == MAX_int32
		|| !Profile || !Profile->ValidateMotion(Error) || !FMath::IsFinite(PlayRate) || PlayRate <= UE_SMALL_NUMBER
		|| !FMath::IsFinite(Profile->Duration / PlayRate))
	{
		return INDEX_NONE;
	}

	TSharedPtr<FRootMotionSource_GGYGOActionCurve> Source = MakeShared<FRootMotionSource_GGYGOActionCurve>();
	Source->InstanceName = GGYGOMovementConstants::ActionCurveSourceName;
	Source->Priority = GGYGOMovementConstants::ActionCurvePriority;
	Source->TranslationCurve = Profile->TranslationCurve;
	Source->Duration = Profile->Duration / PlayRate;
	Source->PlayRate = PlayRate;
	Source->EntryMeshRotation = Mesh->GetComponentQuat();
	Source->TranslationScale = Profile->TranslationScale * Mesh->GetComponentScale();
	if (CompletedMontageActionResource.IsValid()) LastRetiredMontageActionHandle = CompletedMontageActionResource->Handle;
	NeutralizeMontageActionSource(CompletedMontageActionResource);
	CompletedMontageActionResource.Reset();
	bCompletedMontageActionNativeContributionConsumed = false;
	const auto RetiredActionFailureCallback = MoveTemp(ActionMotionFailureCallback);
	ActionMotionFailureResource.Reset();
	const auto RetiredActionCompletionCallback = MoveTemp(ActionMotionCompletionCallback);
	ActionMotionCompletionResource.Reset();
	bMontageActionCompletionObserverInstalled = false;
	MontageActionNaturalBlendOutResource.Reset();
	ActionMotionSourceID = ApplyRootMotionSource(Source);
	if (ActionMotionSourceID == static_cast<uint16>(ERootMotionSourceID::Invalid)) return INDEX_NONE;

	ActiveActionMotionHandle = NextActionMotionHandle;
	++NextActionMotionHandle;
	RemoveRootMotionSource(GGYGOMovementConstants::CurveBrakeSourceName);
	RemoveRootMotionSource(GGYGOMovementConstants::CurveTurnBackSourceName);
	ResetTurnBack();
	bReplicatedTurnBackCurveDriven = false;
	ResolvedGait = EGGYGOGait::None;
	WalkHoldTimer = 0.f;
	CurveMotion.Reset();
	StopMovementImmediately();
	return ActiveActionMotionHandle;
}

bool UGGYGOCharacterMovementComponent::HasActiveActionMotion() const
{
	if (ActiveActionMotionHandle == INDEX_NONE) return false;
	const auto MatchesActive = [this](const TSharedPtr<FRootMotionSource>& Source)
	{
		return Source.IsValid() && Source->LocalID == ActionMotionSourceID
			&& Source->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct()
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
	};
	return CurrentRootMotion.RootMotionSources.ContainsByPredicate(MatchesActive)
		|| CurrentRootMotion.PendingAddRootMotionSources.ContainsByPredicate(MatchesActive);
}

void UGGYGOCharacterMovementComponent::EndActionMotion(int32 Handle)
{
	if (Handle == INDEX_NONE || Handle != ActiveActionMotionHandle) return;
	if (ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == Handle)
	{
		FString Error;
		ReleaseMontageActionMotion(Handle, EGGYGOActionMotionReleaseReason::Cancelled, Error);
		return;
	}
	const TSharedPtr<FRootMotionSource> Source = GetRootMotionSourceByID(ActionMotionSourceID);
	if (Source.IsValid() && Source->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
	{
		RemoveRootMotionSourceByID(ActionMotionSourceID);
	}
	ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
	ActiveActionMotionHandle = INDEX_NONE;
	Velocity.X = Velocity.Y = 0.;
	NativeMovementVelocityResult.Reset();
	NativeVelocityBeforeRootMotion.Reset();
	bNativeVelocityIntervalCanRetain = false;
	CurveMotion.Reset();
	WalkHoldTimer = 0.f;
	bPreviousHasMoveInput = false;
}

void UGGYGOCharacterMovementComponent::CleanupFinishedActionMotion()
{
	if (ActiveMontageActionResource.IsValid())
	{
		const auto Resource = ActiveMontageActionResource;
		if (!IsMontageActionMovementModeSupported())
		{
			const FString Error = FString::Printf(
				TEXT("[Movement.ActionMotion] CMC='%s' Montage='%s' token=%d: original XYZ action entered unsupported MovementMode=%d CustomMode=%d"),
				*GetPathName(), *GetPathNameSafe(Resource->Source->Montage.Get()), Resource->Handle,
				static_cast<int32>(MovementMode), static_cast<int32>(CustomMovementMode));
			UE_LOG(LogGGYGOMovement, Error, TEXT("%s"), *Error);
			FailMontageActionMotion(Resource, Error);
		}
		else if (!HasActiveActionMotion())
		{
			// Natural expiry does not zero/retire an original Prepared final contribution.
			CompletedMontageActionResource = Resource;
			ActiveMontageActionResource.Reset();
			ActiveActionMotionHandle = INDEX_NONE;
			ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
		}
		return;
	}
	if (ActiveActionMotionHandle != INDEX_NONE && (!HasActiveActionMotion() || !IsMovingOnGround()))
	{
		EndActionMotion(ActiveActionMotionHandle);
	}
}

void UGGYGOCharacterMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	RefreshMovementOwnerSyncContext(); // Observe native lifecycle in the existing tick only.
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self || Self->bMovementOwnerSyncClosed) return;
	CleanupFinishedActionMotion();
	Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || Self->bMovementOwnerSyncClosed) return;
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

float UGGYGOCharacterMovementComponent::GetScaledCurveSpeed() const
{
	if (!IsCurveDrivingSpeed())
	{
		return 0.0f;
	}

	return CurveMotion.Speed * MovementSet->RootMotionScale;
}

bool UGGYGOCharacterMovementComponent::IsCurveDrivingSpeed() const
{
	if (HasActiveActionMotion() || !HasAcceptedMovementSet()
		|| !MovementSet->bUseCurveDrivenSpeed || !CurveMotion.bHasCurveSource)
	{
		return false;
	}

	const float Speed = CurveMotion.Speed;
	const float Scale = MovementSet->RootMotionScale;
	if (!FMath::IsFinite(Speed) || Speed < 0.0f || !FMath::IsFinite(Scale) || Scale < 0.0f)
	{
		return false;
	}

	const float Scaled = Speed * Scale;
	return FMath::IsFinite(Scaled) && Scaled >= 0.0f;
}

bool UGGYGOCharacterMovementComponent::HasCurveRootMotionSource() const
{
	// 按类型判定而不是按实例名：将来再加曲线驱动段（冲刺、受击位移）时不用改这里。
	for (const TSharedPtr<FRootMotionSource>& Source : CurrentRootMotion.RootMotionSources)
	{
		if (Source.IsValid() && Source->GetScriptStruct() == FRootMotionSource_GGYGOCurve::StaticStruct())
		{
			return true;
		}
	}

	return false;
}

void UGGYGOCharacterMovementComponent::CacheAbilitySystemComponent()
{
	// 只向原 Owner 的原 Extension 订阅身份通知；派生缓存由真实 Ready/Released 维护。
	UGGYGOPawnExtensionComponent* const PawnExtComp =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(GetOwner());
	FString Error;
	if (!PrepareLocalAbilitySystemSubscription(PawnExtComp, Error))
	{
		UE_LOG(LogGGYGOMovement, Error, TEXT("%s"), *Error);
	}
	// 订阅成立不等于 Ready；合法未就绪由后续真实通知推进，不轮询或重试。
}

bool UGGYGOCharacterMovementComponent::SetMovementSet(const UGGYGOMovementSet* InMovementSet, FString* OutError)
{
	bMovementSetConfigurationSubmitted = true;
	MovementSetConfigurationError.Reset();
	bGroundAdmissionDiagnosticReported = false;
	if (OutError)
	{
		OutError->Reset();
	}

	CaptureComponentDefaults();
	RestoreComponentDefaults();
	ResetLocomotionState();
	MovementSet = nullptr;

	if (!InMovementSet)
	{
		MovementSetConfigurationError = TEXT("original SetMovementSet entry explicitly submitted no configuration (unbound/detached).");
		return false;
	}

	FString Error;
	if (!IsValid(InMovementSet))
	{
		Error = TEXT("MovementSet object is invalid.");
	}
	else if (InMovementSet->ValidateMovementSet(Error))
	{
		MovementSet = InMovementSet;
		ApplyMovementSetToComponent();
		return true;
	}

	if (OutError)
	{
		*OutError = Error;
	}
	MovementSetConfigurationError = FString::Printf(TEXT("original SetMovementSet rejected '%s': %s"), *GetPathNameSafe(InMovementSet), *Error);
	bGroundAdmissionDiagnosticReported = true;
	UE_LOG(LogGGYGOMovement, Error,
		TEXT("Movement SetMovementSet rejected: Component='%s', Owner='%s', MovementSet='%s', Reason='%s'."),
		*GetPathName(), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(InMovementSet), *Error);
	return false;
}

bool UGGYGOCharacterMovementComponent::PublishLocomotionSourceBinding(
	const FGGYGOLocomotionSourceBinding& Binding, FString& OutError)
{
	OutError.Reset();
	const FGGYGOLocomotionSourceIdentity& Identity = Binding.Identity;
	UAnimInstance* Producer = Identity.Producer.Get();
	USkeletalMeshComponent* Mesh = Identity.Mesh.Get();
	ACharacter* SourceCharacter = Cast<ACharacter>(GetOwner());
	if (!IsInGameThread() || !SourceCharacter || Identity.Character.Get() != SourceCharacter
		|| !Mesh || Mesh != SourceCharacter->GetMesh() || !Producer || Mesh->GetAnimInstance() != Producer
		|| Producer->GetSkelMeshComponent() != Mesh || Identity.LifecycleGeneration == 0
		|| Identity.ConfigurationGeneration == 0)
	{
		OutError = FString::Printf(TEXT("Movement source publication rejected: CMC='%s', Character='%s', Mesh='%s', Producer='%s': original primary lifecycle identity is not current."),
			*GetPathName(), *GetPathNameSafe(Identity.Character.Get()), *GetPathNameSafe(Mesh), *GetPathNameSafe(Producer));
		return false;
	}
	if (LastLocomotionSourceProducer == Producer
		&& Identity.ConfigurationGeneration <= LastLocomotionSourceConfigurationGeneration)
	{
		OutError = TEXT("Movement source publication rejected: producer configuration generation did not advance.");
		return false;
	}
	// Publication is independent of MovementSet/input readiness. A rejected source replaces only its own publication.
	RetireLocomotionCurveRootMotion();
	CompletedLocomotionCurveOrigin.Reset();
	CurveMotion.Reset();
	LastLocomotionSourceProducer = Producer;
	LastLocomotionSourceConfigurationGeneration = Identity.ConfigurationGeneration;
	LocomotionSourceBinding = MakeShared<const FGGYGOLocomotionSourceBinding, ESPMode::ThreadSafe>(Binding);
	ResetLocomotionSteeringObservation();
	// Receipt acknowledges the original publication, including explicit Missing/Invalid states.
	// Curve execution alone requires Available + validated sources; Fixed/GA modes do not consume this optional capability.
	return true;
}

void UGGYGOCharacterMovementComponent::RetireLocomotionSourceBinding(
	UAnimInstance* OriginalProducer, uint64 OriginalConfigurationGeneration)
{
	if (!IsInGameThread() || !LocomotionSourceBinding.IsValid()
		|| LocomotionSourceBinding->Identity.Producer.Get() != OriginalProducer
		|| LocomotionSourceBinding->Identity.ConfigurationGeneration != OriginalConfigurationGeneration) return;
	RetireLocomotionCurveRootMotion();
	CompletedLocomotionCurveOrigin.Reset();
	CurveMotion.Reset();
	LocomotionSourceBinding.Reset();
	ResetLocomotionSteeringObservation();
}

bool UGGYGOCharacterMovementComponent::IsLocomotionSourceBindingCurrent(
	const FGGYGOLocomotionSourceBindingPtr& Binding, FString& OutError) const
{
	OutError.Reset();
	if (!Binding.IsValid() || !LocomotionSourceBinding.IsValid()
		|| !Binding->Identity.IsSameIdentity(LocomotionSourceBinding->Identity))
	{
		OutError = TEXT("Movement Animation source binding is missing or its original publication is stale.");
		return false;
	}
	const FGGYGOLocomotionSourceIdentity& Identity = Binding->Identity;
	USkeletalMeshComponent* Mesh = Identity.Mesh.Get();
	if (!CharacterOwner || Identity.Character.Get() != CharacterOwner || !Mesh
		|| Mesh != CharacterOwner->GetMesh() || !Identity.Producer.IsValid()
		|| Mesh->GetAnimInstance() != Identity.Producer.Get())
	{
		OutError = TEXT("Movement Animation source binding's original Character/Mesh/Producer lifecycle is stale.");
		return false;
	}
	return GGYGOLocomotionEvaluation::ValidateBinding(*Binding, OutError);
}

bool UGGYGOCharacterMovementComponent::GetLocomotionSourceBinding(
	FGGYGOLocomotionSourceBindingPtr& OutBinding, FString& OutError) const
{
	OutBinding = (CharacterOwner && CharacterOwner->bClientUpdating && MovementInputReplayCapture.IsSet())
		? ReplayLocomotionSourceBinding : LocomotionSourceBinding;
	if (!IsLocomotionSourceBindingCurrent(OutBinding, OutError)) { OutBinding.Reset(); return false; }
	return true;
}

void UGGYGOCharacterMovementComponent::ResetLocomotionState()
{
	ResetLocomotionSteeringObservation();
	RetireLocomotionCurveRootMotion();
	CompletedLocomotionCurveOrigin.Reset();
	EndLocomotionCurveReplay();
	ResolvedGait = EGGYGOGait::None;
	WalkHoldTimer = 0.0f;
	bWantsRunOnNextMove = false;
	bForceWalkRequested = false;
	bPreviousHasMoveInput = false;
	bPreviousMovementBlocked = false;
	LocomotionMotionType = EGGYGOLocomotionMotionType::None;
	StopMotionType = EGGYGOStopMotionType::None;
	WalkRunBlendAlpha = 0.0f;
	LocomotionMotionTime = 0.0f;
	WalkRunCyclePhase = 0.0f;
	++LocomotionMotionSequence;
	CurveMotion.Reset();
	ResetTurnBack();
	bTurnBackInputLatched = false;
	bReplicatedTurnBackCurveDriven = false;
	RemoveRootMotionSource(GGYGOMovementConstants::CurveBrakeSourceName);
	RemoveRootMotionSource(GGYGOMovementConstants::CurveTurnBackSourceName);
}

void UGGYGOCharacterMovementComponent::SetForceWalkRequested(bool bRequested)
{
	bForceWalkRequested = bRequested;
	if (bRequested)
	{
		WalkHoldTimer = 0.0f;
		bWantsRunOnNextMove = false;
	}
}

void UGGYGOCharacterMovementComponent::ApplyMovementSetToComponent()
{
	if (!MovementSet)
	{
		// 解绑或拒绝时只恢复组件基线；普通地面执行由准入入口拒绝。
		return;
	}

	MaxAcceleration = MovementSet->MaxAcceleration;
	BrakingDecelerationWalking = MovementSet->BrakingDecelerationWalking;
	GroundFriction = MovementSet->GroundFriction;

	bOrientRotationToMovement = MovementSet->bOrientRotationToMovement;

	// 只设 Yaw。Pitch / Roll 由动画负责，CMC 去转它们会和动画打架。
	RotationRate = FRotator(0.0f, MovementSet->RotationYawRate, 0.0f);

	// MaxWalkSpeed 有意**不在这里设置**。它由 GetMaxSpeed() 按步态动态返回，
	// 在这里写一个固定值只会造成"两个速度来源"的疑惑。
}

void UGGYGOCharacterMovementComponent::CaptureComponentDefaults()
{
	if (bComponentDefaultsCaptured)
	{
		return;
	}

	DefaultMaxAcceleration = MaxAcceleration;
	DefaultBrakingDecelerationWalking = BrakingDecelerationWalking;
	DefaultGroundFriction = GroundFriction;
	bDefaultOrientRotationToMovement = bOrientRotationToMovement;
	DefaultRotationRate = RotationRate;
	bComponentDefaultsCaptured = true;
}

void UGGYGOCharacterMovementComponent::RestoreComponentDefaults()
{
	if (!bComponentDefaultsCaptured)
	{
		return;
	}

	MaxAcceleration = DefaultMaxAcceleration;
	BrakingDecelerationWalking = DefaultBrakingDecelerationWalking;
	GroundFriction = DefaultGroundFriction;
	bOrientRotationToMovement = bDefaultOrientRotationToMovement;
	RotationRate = DefaultRotationRate;
}

float UGGYGOCharacterMovementComponent::GetMaxSpeed() const
{
	// 被禁止移动时返回 0 而不是提前 return 或清 Velocity。
	// 走 GetMaxSpeed 这条路，CMC 会用自己的制动减速度把速度平滑压到零，
	// 而 StopMovementImmediately 那种硬停会让角色瞬间定住，
	// 在联机下还会与服务器的重演结果不一致。
	if (IsMovementBlockedByTag())
	{
		return 0.0f;
	}

	// 非地面移动交回基类：游泳、飞行、下落各有自己的速度体系，
	// 步态与动画曲线只对地面移动有意义。
	if (MovementMode != MOVE_Walking && MovementMode != MOVE_NavWalking)
	{
		return Super::GetMaxSpeed();
	}

	if (!HasAcceptedMovementSet())
	{
		return 0.0f;
	}
	if (IsMovementInputRequestBlocked())
	{
		return 0.0f;
	}
	if (ShouldRejectUnownedCurveGroundLocomotion())
	{
		return 0.0f;
	}
	if (!MovementSet->bUseCurveDrivenSpeed)
	{
		return MovementSet->GetSpeedForGait(ResolvedGait);
	}

	// 成功曲线来源提供本帧速度，合法零值同样保留，不按速度大小选择来源。
	if (IsCurveDrivingSpeed())
	{
		const float CurveSpeed = GetScaledCurveSpeed();
		return bForceWalkRequested
			? FMath::Min(CurveSpeed, MovementSet->GetSpeedForGait(EGGYGOGait::Walk))
			: CurveSpeed;
	}

	// Curve mode without a successful source is rejected, never fixed gait speed.
	return 0.0f;
}

float UGGYGOCharacterMovementComponent::GetMinAnalogSpeed() const
{
	const float NativeMinimum = Super::GetMinAnalogSpeed();
	return IsMovingOnGround() ? FMath::Min(NativeMinimum, GetMaxSpeed()) : NativeMinimum;
}

bool UGGYGOCharacterMovementComponent::HasAcceptedMovementSet() const
{
	return IsValid(MovementSet.Get());
}

bool UGGYGOCharacterMovementComponent::IsOriginalMovementSetInitializationPending() const
{
	if (bMovementSetConfigurationSubmitted || LocomotionSteeringIntervalSerial != 0
		|| bMovementOwnerSyncClosed || !IsValid(CharacterOwner) || !GetWorld() || !GetWorld()->IsGameWorld()) return false;
	const UGGYGOPawnExtensionComponent* Extension =
		UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(CharacterOwner);
	if (!IsValid(Extension) || Extension->IsBeingDestroyed() || Extension->GetOwner() != CharacterOwner
		|| Extension->GetWorld() != GetWorld()
		|| Extension->HasReachedInitState(GGYGOGameplayTags::InitState_DataInitialized)) return false;
	// Mesh/Anim registration can precede the original Extension's registration/BeginPlay.
	// After native BeginPlay, only the original initializer's synchronous call in progress
	// can keep this presentation window open. A stalled feature tag never grants waiting.
	// Set submission or any actual movement
	// interval closes it permanently; the accepted pointer remains sole movement admission.
	if (!CharacterOwner->HasActorBegunPlay()) return true;
	return Extension->IsPawnDataInitializationInProgress();
}

bool UGGYGOCharacterMovementComponent::HasIndependentGroundRootMotion() const
{
	// Extracted animation data is the native first-priority source, including its final interval.
	if (HasAnimRootMotion())
	{
		return true;
	}

	if (GetUnsupportedGroundRootMotionSource())
	{
		return false;
	}
	return HasRegisteredActionCurveSource();
}

bool UGGYGOCharacterMovementComponent::HasRegisteredActionCurveSource() const
{
	const auto IsLiveActionCurve = [](const TSharedPtr<FRootMotionSource>& Source)
	{
		return Source.IsValid() && IsRegisteredActionCurveSource(*Source)
			&& Source->AccumulateMode == ERootMotionAccumulateMode::Override
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
	};
	const bool bHasPreparedOverride = CurrentRootMotion.HasOverrideVelocity();
	const auto IsCurrentActionCurve = [bHasPreparedOverride, &IsLiveActionCurve](const TSharedPtr<FRootMotionSource>& Source)
	{
		return IsLiveActionCurve(Source)
			|| (Source.IsValid() && IsRegisteredActionCurveSource(*Source)
				&& Source->AccumulateMode == ERootMotionAccumulateMode::Override
				&& Source->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)
				&& bHasPreparedOverride);
	};
	// A prepared Current source can finish before its native final application, including on proxies.
	// An ended or cancelled Pending source has no prepared contribution and cannot retain the gate.
	return CurrentRootMotion.RootMotionSources.ContainsByPredicate(IsCurrentActionCurve)
		|| CurrentRootMotion.PendingAddRootMotionSources.ContainsByPredicate(IsLiveActionCurve);
}

const FRootMotionSource* UGGYGOCharacterMovementComponent::GetUnsupportedGroundRootMotionSource() const
{
	const auto FindUnsupported = [](const TArray<TSharedPtr<FRootMotionSource>>& Sources) -> const FRootMotionSource*
	{
		for (const TSharedPtr<FRootMotionSource>& Source : Sources)
		{
			if (Source.IsValid() && !IsRegisteredActionCurveSource(*Source)
				&& !IsOwnedLocomotionCurveSource(*Source))
			{
				return Source.Get();
			}
		}
		return nullptr;
	};
	if (const FRootMotionSource* Source = FindUnsupported(CurrentRootMotion.RootMotionSources))
	{
		return Source;
	}
	return FindUnsupported(CurrentRootMotion.PendingAddRootMotionSources);
}

bool UGGYGOCharacterMovementComponent::ShouldRejectUnconfiguredGroundLocomotion() const
{
	return IsMovingOnGround() && !HasAcceptedMovementSet() && !HasIndependentGroundRootMotion();
}

void UGGYGOCharacterMovementComponent::EnforceGroundLocomotionAdmission(const TCHAR* Entry)
{
	EnforceMovementInputLocomotionAdmission(Entry);
	if (ShouldRejectUnownedCurveGroundLocomotion())
	{
		const auto HasOwnedSource = [](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
		{
			return Sources.ContainsByPredicate([](const TSharedPtr<FRootMotionSource>& Source)
			{
				return Source.IsValid() && IsOwnedLocomotionCurveSource(*Source)
					&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
			});
		};
		const bool bHasRequest = Acceleration.ContainsNaN() || ProjectToGravityFloor(Acceleration).SizeSquared() > KINDA_SMALL_NUMBER
			|| (bHasRequestedVelocity && (RequestedVelocity.ContainsNaN()
				|| ProjectToGravityFloor(RequestedVelocity).SizeSquared() > KINDA_SMALL_NUMBER))
			|| (!HasNativePassiveGroundVelocity()
				&& (Velocity.ContainsNaN() || ProjectToGravityFloor(Velocity).SizeSquared() > KINDA_SMALL_NUMBER))
			|| CurveMotion.bHasCurveSource || GetUnsupportedGroundRootMotionSource()
			|| HasOwnedSource(CurrentRootMotion.RootMotionSources)
			|| HasOwnedSource(CurrentRootMotion.PendingAddRootMotionSources);
		if (bHasRequest && !bGroundAdmissionDiagnosticReported && !bMovementInputAdmissionDiagnosticReported)
		{
			bGroundAdmissionDiagnosticReported = true;
			UE_LOG(LogGGYGOMovement, Error,
				TEXT("Movement curve execution rejected: Component='%s', Owner='%s', MovementSet='%s', ExecutionRequest=0, Reason='no issued CMC execution request; source production binding is required, no acceleration request or automatic retry is created'."),
				*GetPathName(), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(MovementSet.Get()));
		}
		CancelMovementInputLocomotion();
		return;
	}
	if (!ShouldRejectUnconfiguredGroundLocomotion())
	{
		return;
	}

	const FRootMotionSource* UnsupportedSource = GetUnsupportedGroundRootMotionSource();
	const bool bHasGroundRequest = UnsupportedSource
		|| Acceleration.ContainsNaN() || ProjectToGravityFloor(Acceleration).SizeSquared() > KINDA_SMALL_NUMBER
		|| (bHasRequestedVelocity && (RequestedVelocity.ContainsNaN()
			|| ProjectToGravityFloor(RequestedVelocity).SizeSquared() > KINDA_SMALL_NUMBER))
		|| (!HasNativePassiveGroundVelocity()
			&& (Velocity.ContainsNaN() || ProjectToGravityFloor(Velocity).SizeSquared() > KINDA_SMALL_NUMBER));
	if (bHasGroundRequest && MovementInputBindingSerial != 0
		&& (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted
			|| LocomotionRequestAdmission == ELocomotionRequestAdmission::Released))
	{
		FailLocomotionRequest(LocomotionRequestSerial, TEXT("ordinary ground execution has no accepted MovementSet"));
	}
	if (bHasGroundRequest && !bGroundAdmissionDiagnosticReported)
	{
		bGroundAdmissionDiagnosticReported = true;
		UE_LOG(LogGGYGOMovement, Error,
			TEXT("Movement ground locomotion rejected: Component='%s', Owner='%s', MovementSet='%s', ConfigState='%s', UnsupportedSource='%s', SourceType='%s', Reason='no accepted MovementSet for ground locomotion'."),
			*GetPathName(), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(MovementSet.Get()),
			MovementSet ? TEXT("invalid") : TEXT("unbound"),
			UnsupportedSource ? *UnsupportedSource->InstanceName.ToString() : TEXT("None"),
			*GetNameSafe(UnsupportedSource ? UnsupportedSource->GetScriptStruct() : nullptr));
	}

	// Preserve the gravity-axis component and all native mode/base/correction handling.
	if (!HasNativePassiveGroundVelocity())
		Velocity = Velocity.ContainsNaN() ? FVector::ZeroVector : Velocity - ProjectToGravityFloor(Velocity);
}

void UGGYGOCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	ResumeLocomotionAfterAction();
	if (ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		EnforceGroundLocomotionAdmission(TEXT("CalcVelocity"));
		if (!HasNativePassiveGroundVelocity()) return;
		// Only native braking consumes an admitted physical result. Keep the original request
		// values for diagnosis/source processing, but none can accelerate this rejected interval.
		const FVector OriginalAcceleration = Acceleration;
		const bool bOriginalRequestedVelocity = bHasRequestedVelocity;
		const bool bOriginalForceMaxAccel = bForceMaxAccel;
		const bool bOriginalRVOAvoidance = bUseRVOAvoidance;
		Acceleration = FVector::ZeroVector;
		bHasRequestedVelocity = false;
		bForceMaxAccel = false;
		bUseRVOAvoidance = false;
		Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
		Acceleration = OriginalAcceleration;
		bHasRequestedVelocity = bOriginalRequestedVelocity;
		bForceMaxAccel = bOriginalForceMaxAccel;
		bUseRVOAvoidance = bOriginalRVOAvoidance;
		return;
	}
	const FVector SteeringPreviousVelocity = Velocity;
	const bool bOrdinarySteering = CanExecuteOrdinaryLocomotionSteering();
	Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
	if (bOrdinarySteering && CanExecuteOrdinaryLocomotionSteering())
	{
		const float StartingYaw = SteeringPreviousVelocity.SizeSquared2D() > UE_KINDA_SMALL_NUMBER
			? SteeringPreviousVelocity.Rotation().Yaw : UpdatedComponent->GetComponentRotation().Yaw;
		const float DesiredError = FMath::FindDeltaAngleDegrees(StartingYaw, Acceleration.Rotation().Yaw);
		float Budget = 0.0f;
		FString Error;
		if (!SteeringPreviousVelocity.ContainsNaN() && !Velocity.ContainsNaN() && !Acceleration.ContainsNaN()
			&& FMath::IsFinite(SteeringPreviousVelocity.SizeSquared2D()) && FMath::IsFinite(Velocity.SizeSquared2D())
			&& GGYGOLocomotionSteeringEvaluation::EvaluateAngularBudget(*MovementSet, DesiredError, WalkRunBlendAlpha, DeltaTime, Budget, Error))
		{
			// Preserve native speed, Z, friction/braking and collision. Only heading has an angular budget.
			Velocity = GGYGOLocomotionSteeringEvaluation::ConstrainPlanarHeading(SteeringPreviousVelocity, Velocity, StartingYaw, Budget);
			bLocomotionSteeringIntervalOrdinary = bLocomotionSteeringIntervalOpen;
		}
		else
		{
			if (Error.IsEmpty()) Error = TEXT("native steering velocity is not finite.");
			Velocity = SteeringPreviousVelocity; // Original subinterval momentum; rejected input cannot accelerate it.
			RejectLocomotionSteering(Error);
		}
	}
	if (!GetUnsupportedGroundRootMotionSource()) MarkNativeMovementVelocityResult();
}

void UGGYGOCharacterMovementComponent::ApplyRootMotionToVelocity(float DeltaTime)
{
	if (HasAnimRootMotion() || CurrentRootMotion.HasOverrideVelocity() || CurrentRootMotion.HasAdditiveVelocity())
		bLocomotionSteeringIntervalExcluded = true;
	if (ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		// Reject at entry, including a removed Brake/TurnBack source's final application.
		// Native RMS application must not switch to Falling before this frame is rejected.
		EnforceGroundLocomotionAdmission(TEXT("ApplyRootMotionToVelocity.Entry"));
		if (HasNativePassiveGroundVelocity() && CurrentRootMotion.HasOverrideVelocity())
		{
			// Native Walking/NavWalking skips CalcVelocity for any prepared Override, even
			// one rejected here. Run its otherwise skipped braking once; never apply that RMS.
			CalcVelocity(DeltaTime, GroundFriction, false, GetMaxBrakingDeceleration());
		}
		return;
	}
	const bool bSupportedNativeSource = HasAnimRootMotion() || !GetUnsupportedGroundRootMotionSource();
	Super::ApplyRootMotionToVelocity(DeltaTime);
	if (bSupportedNativeSource) MarkNativeMovementVelocityResult();
	else bNativeVelocityIntervalCanRetain = false; // An unknown RMS cannot certify a passive result.
	for (const auto& Base : CurrentRootMotion.RootMotionSources)
	{
		if (Base.IsValid() && Base->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct()
			&& Base->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared))
			static_cast<FRootMotionSource_GGYGOActionCurve*>(Base.Get())->bPreparedContributionConsumed = true;
	}
	EnforceGroundLocomotionAdmission(TEXT("ApplyRootMotionToVelocity.Result"));
}

void UGGYGOCharacterMovementComponent::UpdateVelocityBeforeMovement(float DeltaSeconds)
{
	// PerformMovement applies Override directly before this hook, without calling our
	// ApplyRootMotionToVelocity. Consume only this move's post-Before/pre-Prepare snapshot.
	const TOptional<FVector> BeforeRootMotion = NativeVelocityBeforeRootMotion;
	NativeVelocityBeforeRootMotion.Reset();
	Super::UpdateVelocityBeforeMovement(DeltaSeconds);
	if (!CurrentRootMotion.HasOverrideVelocity()
		|| !(ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
			|| ShouldRejectUnownedCurveGroundLocomotion())) return;

	// This hook precedes StartNewPhysics, which alone raises bMovementInProgress.
	// Before's same-owner/component snapshot identifies this pre-physics interval.
	const bool bSameNativeInterval = bNativeVelocityIntervalOpen
		&& NativeVelocityIntervalCharacter.IsValid() && NativeVelocityIntervalCharacter.Get() == CharacterOwner
		&& NativeVelocityIntervalComponent.IsValid() && NativeVelocityIntervalComponent.Get() == UpdatedComponent;
	if (bSameNativeInterval && BeforeRootMotion.IsSet() && !BeforeRootMotion->ContainsNaN())
	{
		// Deny the rejected Override's whole contribution, including Z. Only already-proven
		// momentum can retain the snapshot's ground velocity; no previous frame is restored.
		Velocity = bNativeVelocityIntervalCanRetain ? BeforeRootMotion.GetValue()
			: BeforeRootMotion.GetValue() - ProjectToGravityFloor(BeforeRootMotion.GetValue());
	}
	else
	{
		// No current interval can certify this native override. Fail closed and diagnose below.
		bNativeVelocityIntervalCanRetain = false;
		NativeMovementVelocityResult.Reset();
		Velocity = FVector::ZeroVector;
	}
	EnforceGroundLocomotionAdmission(TEXT("UpdateVelocityBeforeMovement"));
}

void UGGYGOCharacterMovementComponent::UpdateCharacterStateAfterMovement(float DeltaSeconds)
{
	Super::UpdateCharacterStateAfterMovement(DeltaSeconds);
	if (bNativeVelocityIntervalOpen && NativeVelocityIntervalCharacter.IsValid()
		&& NativeVelocityIntervalCharacter.Get() == CharacterOwner
		&& NativeVelocityIntervalComponent.IsValid() && NativeVelocityIntervalComponent.Get() == UpdatedComponent
		&& (bNativeVelocityIntervalCanRetain || CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy))
	{
		StoreNativeMovementVelocityResult();
	}
	else NativeMovementVelocityResult.Reset();
	NativeVelocityBeforeRootMotion.Reset();
	bNativeVelocityIntervalOpen = false;
	bNativeVelocityIntervalCanRetain = false;
	NativeVelocityIntervalCharacter.Reset();
	NativeVelocityIntervalComponent.Reset();
	PublishMontageActionMotionCompletion();
}

bool UGGYGOCharacterMovementComponent::IsMovementBlockedByTag() const
{
	// 仅查询原本地资源仍 Ready 的 ASC；Tag 规则与无 ASC 的合法暂态保持。
	const UGGYGOAbilitySystemComponent* ASC = GetReadyLocalAbilitySystemComponent();
	return ASC && ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantMove);
}

bool UGGYGOCharacterMovementComponent::HasMoveInput() const
{
	if (MovementInputReplayCapture.IsSet()
		&& (PreparingLocomotionCurveReplayGroup != nullptr || (CharacterOwner && CharacterOwner->bClientUpdating))
		&& MovementInputReplayCapture->Request.Binding.ConsumerBindingSerial != 0)
	{
		// Read the original interval's consumed Started/Released disposition, never today's physical Held.
		return !IsMovementInputRequestBlocked()
			&& MovementInputReplayCapture->Request.Admission == static_cast<uint8>(ELocomotionRequestAdmission::Admitted)
			&& GetCurrentAcceleration().SizeSquared2D() > KINDA_SMALL_NUMBER;
	}
	if (ActiveMovementOwnerSyncNativeMove || MovementInputNativeSource.IsValid())
	{
		return !IsMovementInputRequestBlocked() && LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted
			&& GetCurrentAcceleration().SizeSquared2D() > KINDA_SMALL_NUMBER;
	}
	if (MovementInputBindingSerial != 0
		&& (IsMovementInputRequestBlocked() || LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted))
	{
		return false;
	}
	return GetCurrentAcceleration().SizeSquared2D() > KINDA_SMALL_NUMBER;
}

void UGGYGOCharacterMovementComponent::RequestRunOnNextMove()
{
	bWantsRunOnNextMove = true;
}

void UGGYGOCharacterMovementComponent::UpdateCharacterStateBeforeMovement(float DeltaSeconds)
{
	BeginLocomotionSteeringInterval();
	BeginNativeMovementVelocityInterval();
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> VelocityIntervalSelf(this);
	ON_SCOPE_EXIT
	{
		UGGYGOCharacterMovementComponent* Self = VelocityIntervalSelf.Get();
		if (Self && !Self->IsBeingDestroyed() && Self->bNativeVelocityIntervalOpen
			&& Self->NativeVelocityIntervalCharacter.IsValid()
			&& Self->NativeVelocityIntervalCharacter.Get() == Self->CharacterOwner
			&& Self->NativeVelocityIntervalComponent.IsValid()
			&& Self->NativeVelocityIntervalComponent.Get() == Self->UpdatedComponent
			&& !Self->Velocity.ContainsNaN())
		{
			Self->NativeVelocityBeforeRootMotion = Self->Velocity;
		}
	};
	if (CharacterOwner && CharacterOwner->bClientUpdating
		&& (ReplayLocomotionCurveInput.IsValid() || bLocomotionCurveReplayRejected))
	{
		CleanupFinishedActionMotion();
		if (HasActiveActionMotion() || HasIndependentGroundRootMotion())
		{
			CurveMotion.Reset();
			Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
			return;
		}
		if (ReplayLocomotionCurveInput.IsValid() && ReplayLocomotionCurveInput->Origin.IsValid()
			&& ReplayLocomotionCurveInput->Origin->ExecutionRequestSerial == LocomotionRequestSerial
			&& ReplayLocomotionCurveInput->Origin->Binding == MovementInputBinding
			&& ReplayLocomotionCurveInput->Origin->InputRequest == MovementInputRequest
			&& LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
		{
			bLocomotionCurveReplayRejected = true;
			ReplayLocomotionCurvePrepared.Reset();
		}
		if (ReplayLocomotionCurveInput.IsValid()
			&& (!ReplayLocomotionCurveInput->Origin.IsValid()
				|| ReplayLocomotionCurveInput->Origin->MovementSet.Get() != MovementSet.Get()
				|| !HasAcceptedMovementSet()))
		{
			bLocomotionCurveReplayRejected = true;
			ReplayLocomotionCurvePrepared.Reset();
		}
		if (!bLocomotionCurveReplayRejected && ReplayLocomotionCurvePrepared.IsValid()
			&& (!bReplayLocomotionFromAuthority || bReplayPreparedFromAuthority)
			&& ReplayLocomotionCurvePrepared->Input == ReplayLocomotionCurveInput)
		{
			// Super::PrepMoveFor already prepared this original native interval, or retained
			// its original prepared result. PerformMovement skips native Prepare during replay.
			ConsumeLocomotionCurvePrepared(ReplayLocomotionCurvePrepared);
		}
		else
		{
			CurveMotion.Reset();
			if (bLocomotionCurveReplayRejected)
			{
				for (const TSharedPtr<FRootMotionSource>& Source : CurrentRootMotion.RootMotionSources)
				{
					if (!Source.IsValid() || Source->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()) continue;
					Source->RootMotionParams.Set(FTransform::Identity);
					Source->AccumulateMode = ERootMotionAccumulateMode::Additive;
					Source->Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
				}
			}
			// No prepared interval means this source had not started in the original move.
			// Do not manufacture a simulation interval from the replay's full frame delta.
		}
		Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
		return;
	}
	LastLocomotionCurvePrepared.Reset();
	ConsumedLocomotionCurvePrepared.Reset();
	PendingLocomotionCurveInput.Reset();
	ActionSkippedMovementTickTime = 0.0f;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(CharacterOwner);
	const auto OriginalOwnerContext = MovementOwnerSyncContext;
	if (bMovementInputBindingActive && !MovementInputBinding.SourceSession.Producer.IsValid())
	{
		FString Error;
		InvalidateMovementInputSession(MovementInputBinding, FName(TEXT("ProducerDestroyed")), Error);
	}
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || Self->CharacterOwner != OriginalCharacter.Get()
		|| Self->MovementOwnerSyncContext != OriginalOwnerContext) return;
	CleanupFinishedActionMotion();
	Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || Self->CharacterOwner != OriginalCharacter.Get()
		|| Self->MovementOwnerSyncContext != OriginalOwnerContext) return;
	if (HasActiveActionMotion() || HasIndependentGroundRootMotion())
	{
		if (ActiveMontageActionResource.IsValid() || CompletedMontageActionResource.IsValid())
			ActionSkippedMovementTickTime = DeltaSeconds;
		RetireLocomotionCurveRootMotion();
		CurveMotion.Reset();
		Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
		return;
	}

	if (!HasAcceptedMovementSet() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		RetireLocomotionCurveRootMotion();
		CurveMotion.Reset();
		RemoveRootMotionSource(GGYGOMovementConstants::CurveBrakeSourceName);
		RemoveRootMotionSource(GGYGOMovementConstants::CurveTurnBackSourceName);
		// On authority/autonomous moves the native montage pose is extracted after this callback.
		// Defer that source's final decision to CalcVelocity/ApplyRootMotionToVelocity. Proxy
		// root-motion simulation already provides actual extracted data before this callback.
		const bool bWillExtractNetworkedMontage = CharacterOwner
			&& CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy
			&& CharacterOwner->IsPlayingNetworkedRootMotionMontage();
		if (!bWillExtractNetworkedMontage)
		{
			EnforceGroundLocomotionAdmission(TEXT("UpdateCharacterStateBeforeMovement"));
		}
		Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
		return;
	}

	const bool bSimulatedProxy = CharacterOwner
		&& CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy;
	if (!bSimulatedProxy)
	{
		const bool bHadMoveInput = bPreviousHasMoveInput;
		const EGGYGOGait PreviousGait = ResolvedGait;
		if (!TryUpdateLocomotion(DeltaSeconds, true, bHadMoveInput, PreviousGait))
		{
			Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
			return;
		}
		if (!PendingLocomotionCurveInput.IsValid())
		{
			RetireLocomotionCurveRootMotion();
			UpdateCurveBrake();
			ValidateClientLocomotionHint();
		}
	}

	// 转身段的位移源在所有角色上同步，所以放在本地控制判断之外。
	//
	// 必须在这里而不是更早或更晚：引擎紧接着就会在 `PerformMovement` 里
	// `CurrentRootMotion.PrepareRootMotion`，晚一步挂的 source 要等下一帧才生效。
	if (!PendingLocomotionCurveInput.IsValid()) UpdateTurnBackRootMotion();

	Super::UpdateCharacterStateBeforeMovement(DeltaSeconds);
}

UGGYGOCharacterMovementComponent::FLocomotionUpdateCandidate
UGGYGOCharacterMovementComponent::CaptureLocomotionCandidate() const
{
	return {ResolvedGait, WalkHoldTimer, bWantsRunOnNextMove, bPreviousHasMoveInput, bPreviousMovementBlocked,
		LocomotionMotionType, StopMotionType, LocomotionMotionTime, WalkRunCyclePhase, LocomotionMotionSequence,
		WalkRunBlendAlpha, TurnBackPhase, TurnBackElapsed, TurnBackEntryYaw, bTurnBackInputLatched, CurveMotion};
}

void UGGYGOCharacterMovementComponent::CommitLocomotionCandidate(const FLocomotionUpdateCandidate& Candidate)
{
	ResolvedGait = Candidate.Gait;
	WalkHoldTimer = Candidate.WalkHoldSeconds;
	bWantsRunOnNextMove = Candidate.bWantsRun;
	bPreviousHasMoveInput = Candidate.bPreviousInput;
	bPreviousMovementBlocked = Candidate.bPreviousBlocked;
	LocomotionMotionType = Candidate.MotionType;
	StopMotionType = Candidate.StopType;
	LocomotionMotionTime = Candidate.MotionTime;
	WalkRunCyclePhase = Candidate.CyclePhase;
	LocomotionMotionSequence = Candidate.MotionSequence;
	WalkRunBlendAlpha = Candidate.BlendAlpha;
	TurnBackPhase = Candidate.TurnPhase;
	TurnBackElapsed = Candidate.TurnElapsed;
	TurnBackEntryYaw = Candidate.TurnEntryYaw;
	bTurnBackInputLatched = Candidate.bTurnInputLatched;
	CurveMotion = Candidate.Motion;
}

bool UGGYGOCharacterMovementComponent::HasCurrentLocomotionCurveOrigin(
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin) const
{
	if (!Origin.IsValid() || Origin != LocomotionCurveOrigin || Origin->Owner.Get() != this
		|| Origin->ExecutionRequestSerial == 0 || Origin->ExecutionRequestSerial != LocomotionRequestSerial
		|| IsMovementInputRequestBlocked()) return false;
	if (MovementInputNativeSource.IsValid())
	{
		return Origin->Binding.ConsumerBindingSerial == 0 && Origin->InputRequest.RequestSerial == 0
			&& Origin->ExecutionRequestSerial == MovementInputNativeSource->ExecutionRequestSerial;
	}
	return Origin->Binding == MovementInputBinding && Origin->InputRequest == MovementInputRequest
		&& IsMovementInputBindingCurrent(Origin->Binding);
}

bool UGGYGOCharacterMovementComponent::ReportLocomotionCurveRootMotionFailure(
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin, const FString& Reason)
{
	if (!HasCurrentLocomotionCurveOrigin(Origin) || Reason.IsEmpty()) return false;
	return FailLocomotionRequest(Origin->ExecutionRequestSerial, FString::Printf(
		TEXT("CurveRMS Motion=%d Sequence=%u: %s"), static_cast<int32>(Origin->MotionType),
		Origin->MotionSequence, *Reason));
}

void UGGYGOCharacterMovementComponent::RetireLocomotionCurveRootMotion()
{
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin> Original = LocomotionCurveOrigin;
	const auto Retire = [&Original](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
	{
		for (const TSharedPtr<FRootMotionSource>& Source : Sources)
		{
			if (!Source.IsValid() || Source->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()) continue;
			FRootMotionSource_GGYGOCurve* Curve = static_cast<FRootMotionSource_GGYGOCurve*>(Source.Get());
			if (!Original.IsValid() || Curve->Origin != Original) continue;
			Curve->RootMotionParams.Set(FTransform::Identity);
			Curve->AccumulateMode = ERootMotionAccumulateMode::Additive;
			Curve->Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
		}
	};
	Retire(CurrentRootMotion.RootMotionSources);
	Retire(CurrentRootMotion.PendingAddRootMotionSources);
	LocomotionCurveOrigin.Reset();
	PendingLocomotionCurveInput.Reset();
}

bool UGGYGOCharacterMovementComponent::StageLocomotionCurveRootMotion(
	float MovementTickTime, const FLocomotionUpdateCandidate& Candidate, FString& OutError)
{
	OutError.Reset();
	if (!FMath::IsFinite(MovementTickTime) || MovementTickTime <= 0.0f
		|| !HasAcceptedMovementSet() || !MovementSet->bUseCurveDrivenSpeed || !UpdatedComponent
		|| LocomotionRequestSerial == 0 || IsMovementInputRequestBlocked())
	{
		OutError = TEXT("CurveRMS staging requires an admitted original request, accepted curve MovementSet, UpdatedComponent and positive finite movement tick.");
		return false;
	}
	FGGYGOLocomotionSourceBindingPtr SourceBinding;
	if (!GetLocomotionSourceBinding(SourceBinding, OutError)) return false;
	const auto SameSegment = [this, &Candidate, &SourceBinding](const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin)
	{
		return Origin.IsValid() && Origin->Owner.Get() == this
			&& Origin->ExecutionRequestSerial == LocomotionRequestSerial
			&& Origin->Binding == MovementInputBinding && Origin->InputRequest == MovementInputRequest
			&& Origin->MovementSet.Get() == MovementSet.Get() && Origin->MotionType == Candidate.MotionType
			&& Origin->SourceBinding == SourceBinding
			&& Origin->MotionSequence == Candidate.MotionSequence;
	};
	if (SameSegment(CompletedLocomotionCurveOrigin))
	{
		// Native completion closes this resource. The remaining clip semantics use the ordinary
		// CMC path, and cannot remount the same physical execution even if a later speed is positive.
		FLocomotionUpdateCandidate Remaining = Candidate;
		if (!EvaluateLocomotionSource(MovementTickTime, Remaining, OutError)) return false;
		CommitLocomotionCandidate(Remaining);
		return true;
	}
	if (!SameSegment(LocomotionCurveOrigin))
	{
		RetireLocomotionCurveRootMotion();
		CompletedLocomotionCurveOrigin.Reset();
		TSharedPtr<FGGYGOCurveRootMotionOrigin> Origin = MakeShared<FGGYGOCurveRootMotionOrigin>();
		Origin->Owner = this;
		Origin->Binding = MovementInputBinding;
		Origin->InputRequest = MovementInputRequest;
		Origin->ExecutionRequestSerial = LocomotionRequestSerial;
		Origin->MotionType = Candidate.MotionType;
		Origin->MotionSequence = Candidate.MotionSequence;
		Origin->MovementSet = MovementSet.Get();
		Origin->SourceBinding = SourceBinding;
		Origin->MotionTimeOrigin = Candidate.MotionTime;
		Origin->BaseYaw = Candidate.MotionType == EGGYGOLocomotionMotionType::TurnBack
			? Candidate.TurnEntryYaw : UpdatedComponent->GetComponentRotation().Yaw;
		Origin->RootMotionScale = MovementSet->RootMotionScale;
		Origin->bEndOnZeroSpeed = Candidate.MotionType != EGGYGOLocomotionMotionType::TurnBack;
		if (!FMath::IsFinite(Origin->BaseYaw) || !FMath::IsFinite(Origin->RootMotionScale)
			|| Origin->RootMotionScale < 0.0f || !FMath::IsFinite(Origin->MotionTimeOrigin)
			|| Origin->MotionTimeOrigin < 0.0f)
		{
			OutError = TEXT("CurveRMS original yaw, RootMotionScale or motion time is invalid.");
			return false;
		}
		LocomotionCurveOrigin = Origin;
	}
	TSharedPtr<FGGYGOCurveRootMotionMoveInput> Input = MakeShared<FGGYGOCurveRootMotionMoveInput>();
	Input->Origin = LocomotionCurveOrigin;
	Input->NativeSource = MovementInputNativeSource; // Original authenticated source, not a synthetic local binding.
	Input->StartCandidate = Candidate;
	if (Candidate.MotionType == EGGYGOLocomotionMotionType::TurnBack)
	{
		Input->StartCandidate.TurnEntryYaw = Input->Origin->BaseYaw;
	}
	Input->Acceleration = GetCurrentAcceleration();
	if (!IsFiniteLocomotionCurveVector(Input->Acceleration))
	{
		OutError = TEXT("CurveRMS original move acceleration is non-finite.");
		return false;
	}
	Input->MovementTickTime = MovementTickTime;
	Input->bHasInput = HasMoveInput();
	Input->bBlocked = IsMovementBlockedByTag();
	Input->bOnGround = IsMovingOnGround();
	Input->bForceWalk = bForceWalkRequested;
	Input->bHadMoveInput = bPreviousHasMoveInput;
	Input->PreviousGait = ResolvedGait;
	PendingLocomotionCurveInput = Input;
	CurveMotion.Reset();
	const bool bTurnBack = Candidate.MotionType == EGGYGOLocomotionMotionType::TurnBack;
	if (!ApplyCurveRootMotionSource(bTurnBack ? GGYGOMovementConstants::CurveTurnBackSourceName
		: GGYGOMovementConstants::CurveBrakeSourceName, bTurnBack ? GGYGOMovementConstants::CurveTurnBackPriority
		: GGYGOMovementConstants::CurveBrakePriority, Input->Origin->BaseYaw, Input->Origin->bEndOnZeroSpeed))
	{
		OutError = TEXT("CurveRMS native ApplyRootMotionSource rejected the original resource.");
		return false;
	}
	return true;
}

void UGGYGOCharacterMovementComponent::ConsumeLocomotionCurvePrepared(
	const TSharedPtr<const FGGYGOCurveRootMotionPrepared>& Prepared)
{
	if (!Prepared.IsValid() || !Prepared->Input.IsValid() || !Prepared->State.IsValid()
		|| Prepared == ConsumedLocomotionCurvePrepared) return;
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin> Origin = Prepared->Input->Origin;
	const bool bOriginalCurrentResource = Origin == LocomotionCurveOrigin || Origin == CompletedLocomotionCurveOrigin;
	const bool bOriginalReplayInput = Prepared->Input == ReplayLocomotionCurveInput
		&& MovementInputReplayCapture.IsSet() && !bLocomotionCurveReplayRejected;
	if (!Origin.IsValid() || Origin->Owner.Get() != this || Origin->ExecutionRequestSerial != LocomotionRequestSerial
		|| Origin->Binding != MovementInputBinding || Origin->InputRequest != MovementInputRequest
		|| (!bOriginalCurrentResource && !bOriginalReplayInput) || IsMovementInputRequestBlocked()
		|| Prepared->Input->NativeSource != MovementInputNativeSource) return;
	CommitLocomotionCandidate(Prepared->State->Candidate);
	ConsumedLocomotionCurvePrepared = Prepared;
	LastLocomotionCurvePrepared = Prepared;
	if ((bOriginalCurrentResource || bOriginalReplayInput) && Origin.IsValid() && Origin->ExecutionRequestSerial == LocomotionRequestSerial
		&& Origin->Binding == MovementInputBinding && Origin->InputRequest == MovementInputRequest
		&& (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted
			|| LocomotionRequestAdmission == ELocomotionRequestAdmission::Released))
	{
		if (Prepared->Result == EGGYGOCurveRootMotionPrepareResult::Finished)
		{
			CompletedLocomotionCurveOrigin = Origin;
			LocomotionCurveOrigin.Reset();
			PendingLocomotionCurveInput.Reset();
		}
		else if (Prepared->Result == EGGYGOCurveRootMotionPrepareResult::Prepared)
		{
			// Replay may restore an unfinished original source. This restores its resource,
			// never request admission, and cannot rearm a FAILED or successor request.
			LocomotionCurveOrigin = Origin;
			CompletedLocomotionCurveOrigin.Reset();
		}
	}
}

bool UGGYGOCharacterMovementComponent::BeginLocomotionCurveReplay(
	const FSavedMove_GGYGO& Move, FString& OutError)
{
	EndLocomotionCurveReplay();
	OutError.Reset();
	FMovementInputReplayCapture Capture;
	Capture.Request = Move.SavedMovementInputRequest;
	Capture.OwnerScope = Move.SavedMovementOwnerSyncScope;
	Capture.OwnerGeneration = Move.SavedMovementOwnerGeneration;
	Capture.Checkpoint = Move.SavedMovementInputSourceCheckpoint;
	MovementInputReplayCapture = MoveTemp(Capture);
	LastLocomotionCurvePrepared.Reset();
	ConsumedLocomotionCurvePrepared.Reset();
	PendingLocomotionCurveInput.Reset();
	ReplayLocomotionCurveInput = Move.SavedCurveRootMotionInput;
	ReplayLocomotionCurvePrepared = Move.SavedCurveRootMotionPrepared;
	ReplayLocomotionSourceBinding = Move.SavedLocomotionSourceBinding;
	bReplayPreparedFromAuthority = false;
	PreparingLocomotionCurveReplayGroup = &Move.SavedRootMotion;
	const auto Reject = [this, &OutError](const TCHAR* Reason)
	{
		bLocomotionCurveReplayRejected = true;
		ReplayLocomotionCurvePrepared.Reset();
		OutError = Reason;
		return false;
	};
	if (IsMovementInputRequestBlocked())
	{
		// Original Waiting/stale/FAILED applicability is normal rejection, not a curve/config failure.
		// The sole gate and Prep/Move guards preserve the current request and its resources.
		ReplayLocomotionCurveInput.Reset();
		ReplayLocomotionCurvePrepared.Reset();
		return true;
	}
	if (ReplayLocomotionCurveInput.IsValid())
	{
		const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin = ReplayLocomotionCurveInput->Origin;
		if (!Origin.IsValid() || Origin->Owner.Get() != this || Origin->ExecutionRequestSerial == 0
			|| !FMath::IsFinite(Move.DeltaTime) || Move.DeltaTime <= 0.0f
			|| ReplayLocomotionCurveInput->MovementTickTime != Move.DeltaTime)
		{
			return Reject(TEXT("SavedMove has no matching original CMC/input/native tick."));
		}
		if (Origin->MovementSet.Get() != MovementSet.Get() || !HasAcceptedMovementSet())
		{
			return Reject(TEXT("SavedMove original MovementSet is no longer the CMC's accepted configuration."));
		}
		if (Origin->SourceBinding != ReplayLocomotionSourceBinding)
		{
			return Reject(TEXT("SavedMove original animation publication does not match its source Origin."));
		}
		if (!IsLocomotionSourceBindingCurrent(Origin->SourceBinding, OutError))
		{
			bLocomotionCurveReplayRejected = true;
			ReplayLocomotionCurvePrepared.Reset();
			return false;
		}
		if (ReplayLocomotionCurvePrepared.IsValid()
			&& (ReplayLocomotionCurvePrepared->Input != ReplayLocomotionCurveInput
				|| !ReplayLocomotionCurvePrepared->State.IsValid()
				|| (ReplayLocomotionCurvePrepared->Result != EGGYGOCurveRootMotionPrepareResult::Prepared
					&& ReplayLocomotionCurvePrepared->Result != EGGYGOCurveRootMotionPrepareResult::Finished)))
		{
			return Reject(TEXT("SavedMove prepared interval belongs to a different original input."));
		}
	}
	bool bHasOriginalNativeSource = false;
	const auto HasOriginalSources = [this, &bHasOriginalNativeSource](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
	{
		for (const TSharedPtr<FRootMotionSource>& Source : Sources)
		{
			if (!Source.IsValid() || Source->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()
				|| Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)) continue;
			const FRootMotionSource_GGYGOCurve* Curve = static_cast<const FRootMotionSource_GGYGOCurve*>(Source.Get());
			if (!ReplayLocomotionCurveInput.IsValid() || Curve->Origin != ReplayLocomotionCurveInput->Origin) return false;
			bHasOriginalNativeSource = true;
		}
		return true;
	};
	if (!HasOriginalSources(Move.SavedRootMotion.RootMotionSources)
		|| !HasOriginalSources(Move.SavedRootMotion.PendingAddRootMotionSources))
	{
		return Reject(TEXT("SavedRootMotion clone has no matching local Origin/input; imported origins remain unsupported."));
	}
	if (ReplayLocomotionCurvePrepared.IsValid() && !bHasOriginalNativeSource)
	{
		return Reject(TEXT("SavedMove has an actual prepared interval but no original native source in SavedRootMotion; record the native group at the original move boundary."));
	}
	if (bReplayLocomotionFromAuthority && ReplayLocomotionCurvePrepared.IsValid())
	{
		// Retain only the actual original native interval. Old derived state/velocity cannot overwrite the correction.
		const TSharedPtr<const FGGYGOCurveRootMotionPrepared> OriginalInterval = ReplayLocomotionCurvePrepared;
		ReplayLocomotionCurvePrepared.Reset();
		if (CharacterOwner && !CharacterOwner->bClientResimulateRootMotionSources)
		{
			const auto Reprepare = [&](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
			{
				for (const TSharedPtr<FRootMotionSource>& Native : Sources)
				{
					if (!Native.IsValid() || Native->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()) continue;
					FRootMotionSource_GGYGOCurve* Curve = static_cast<FRootMotionSource_GGYGOCurve*>(Native.Get());
					if (Curve->Origin != OriginalInterval->Input->Origin
						|| Curve->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)) continue;
					Curve->SetTime(OriginalInterval->NativeStartTime);
					Curve->Status.UnSetFlag(ERootMotionSourceStatusFlags::Finished);
					Curve->PrepareRootMotion(OriginalInterval->SimulationTime, OriginalInterval->MovementTickTime, *CharacterOwner, *this);
					return true;
				}
				return false;
			};
			if (!Reprepare(Move.SavedRootMotion.RootMotionSources)
				&& !Reprepare(Move.SavedRootMotion.PendingAddRootMotionSources))
			{
				return Reject(TEXT("SavedMove authority replay could not prepare its original native source; the actual recorded interval cannot be replaced."));
			}
		}
	}
	return true;
}

void UGGYGOCharacterMovementComponent::FinishLocomotionCurveReplayPreparation()
{
	PreparingLocomotionCurveReplayGroup = nullptr;
}

void UGGYGOCharacterMovementComponent::EndLocomotionCurveReplay()
{
	PreparingLocomotionCurveReplayGroup = nullptr;
	ReplayLocomotionCurveInput.Reset();
	ReplayLocomotionCurvePrepared.Reset();
	ReplayLocomotionSourceBinding.Reset();
	bReplayPreparedFromAuthority = false;
	bLocomotionCurveReplayRejected = false;
}

EGGYGOCurveRootMotionPrepareResult UGGYGOCharacterMovementComponent::PrepareLocomotionCurveRootMotion(
	const FRootMotionSource_GGYGOCurve& Source, float SimulationTime, float MovementTickTime,
	TSharedPtr<const FGGYGOCurveRootMotionPrepared>& OutPrepared, FString& OutError)
{
	OutPrepared.Reset();
	OutError.Reset();
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin> Origin = Source.Origin;
	const bool bReplayPrepare = PreparingLocomotionCurveReplayGroup != nullptr;
	const TSharedPtr<const FGGYGOCurveRootMotionMoveInput> Input =
		bReplayPrepare ? ReplayLocomotionCurveInput : PendingLocomotionCurveInput;
	if (!Origin.IsValid() || Origin->Owner.Get() != this)
	{
		return EGGYGOCurveRootMotionPrepareResult::UnsupportedOrigin;
	}
	if (Origin->ExecutionRequestSerial == LocomotionRequestSerial
		&& Origin->Binding == MovementInputBinding && Origin->InputRequest == MovementInputRequest
		&& LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
	{
		if (bReplayPrepare)
		{
			bLocomotionCurveReplayRejected = true;
			ReplayLocomotionCurvePrepared.Reset();
		}
		return EGGYGOCurveRootMotionPrepareResult::Failed;
	}
	if (!Input.IsValid() || Input->Origin != Origin
		|| (bReplayPrepare ? bLocomotionCurveReplayRejected
			|| !IsLocomotionCurveSourceInGroup(*PreparingLocomotionCurveReplayGroup, &Source)
			: !HasCurrentLocomotionCurveOrigin(Origin) || !IsLocomotionCurveSourceInGroup(CurrentRootMotion, &Source)))
	{
		// A different retired source may still be traversed by native Prepare. It is not
		// the original input's consumer and must not invalidate a successor's replay result.
		return EGGYGOCurveRootMotionPrepareResult::Stale;
	}
	const auto Reject = [this, &Origin, &OutError, bReplayPrepare](const FString& Reason)
	{
		OutError = Reason;
		if (bReplayPrepare)
		{
			bLocomotionCurveReplayRejected = true;
			ReplayLocomotionCurvePrepared.Reset();
		}
		return ReportLocomotionCurveRootMotionFailure(Origin, Reason)
			? EGGYGOCurveRootMotionPrepareResult::Failed : EGGYGOCurveRootMotionPrepareResult::Stale;
	};
	if (!FMath::IsFinite(SimulationTime) || SimulationTime < 0.0f
		|| !FMath::IsFinite(MovementTickTime) || MovementTickTime <= 0.0f
		|| Input->MovementTickTime != MovementTickTime
		|| !FMath::IsFinite(Source.GetTime()) || Source.GetTime() < 0.0f)
	{
		return Reject(TEXT("CurveRMS native simulation interval/tick is invalid or does not belong to the original move."));
	}
	TSharedPtr<FGGYGOCurveRootMotionPrepared> Prepared = MakeShared<FGGYGOCurveRootMotionPrepared>();
	Prepared->Input = Input;
	Prepared->NativeStartTime = Source.GetTime();
	Prepared->NativeEndTime = Source.GetTime() + SimulationTime;
	Prepared->SimulationTime = SimulationTime;
	Prepared->MovementTickTime = MovementTickTime;
	Prepared->MotionStartTime = Origin->MotionTimeOrigin + (Prepared->NativeStartTime - Origin->SourceTimeOrigin);
	const bool bAuthorityReplay = bReplayPrepare && bReplayLocomotionFromAuthority;
	FLocomotionUpdateCandidate Candidate = bAuthorityReplay ? CaptureLocomotionCandidate() : Input->StartCandidate;
	if (bAuthorityReplay) Prepared->MotionStartTime = Candidate.MotionTime;
	Prepared->MotionEndTime = Prepared->MotionStartTime + SimulationTime;
	if (!FMath::IsFinite(Prepared->NativeEndTime) || !FMath::IsFinite(Prepared->MotionStartTime)
		|| Prepared->MotionStartTime < 0.0f || !FMath::IsFinite(Prepared->MotionEndTime))
	{
		return Reject(TEXT("CurveRMS native-to-motion interval mapping is non-finite or negative."));
	}
	const bool bGrounded = bReplayPrepare ? Input->bOnGround : IsMovingOnGround();
	const bool bNormalExit = !bGrounded
		|| Source.Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
		|| Source.Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
	if (bNormalExit)
	{
		if (!bGrounded)
		{
			ResetTurnBack(Candidate);
			Candidate.Gait = EGGYGOGait::None;
			Candidate.StopType = EGGYGOStopMotionType::None;
			SetLocomotionMotion(EGGYGOLocomotionMotionType::None, Candidate);
		}
		Candidate.Motion.Reset();
		Prepared->OverrideVelocity = Velocity;
		Prepared->Result = EGGYGOCurveRootMotionPrepareResult::Finished;
	}
	else
	{
		const UGGYGOMovementSet* OriginalSet = Origin->MovementSet.Get();
		if (!OriginalSet || OriginalSet != MovementSet.Get() || !HasAcceptedMovementSet()
			|| Candidate.MotionType != Origin->MotionType || Candidate.MotionSequence != Origin->MotionSequence
			|| !FMath::IsFinite(Source.BaseYaw) || Source.BaseYaw != Origin->BaseYaw
			|| !FMath::IsFinite(Source.SpeedScale) || Source.SpeedScale != Origin->RootMotionScale
			|| Source.bEndOnZeroSpeed != Origin->bEndOnZeroSpeed)
		{
			return Reject(TEXT("CurveRMS original configuration, segment or scalar parameters no longer match."));
		}
		if (!IsLocomotionSourceBindingCurrent(Origin->SourceBinding, OutError)) return Reject(OutError);
		const FGGYGOLocomotionSequenceSource* AnimationSource = Origin->SourceBinding->GetSingleSource(Origin->MotionType);
		if (!AnimationSource || AnimationSource->bLoop)
		{
			return Reject(TEXT("CurveRMS original non-loop animation source is missing or has loop semantics."));
		}
		FGGYGOLocomotionEvaluationResult Evaluation;
		if (!GGYGOLocomotionEvaluation::EvaluateSingleInterval(*AnimationSource,
			Prepared->MotionStartTime, Prepared->MotionEndTime, Origin->RootMotionScale, Evaluation, &OutError))
		{
			return Reject(OutError);
		}
		Prepared->Sample = Evaluation.Sample;
		Candidate.Motion = Evaluation.Sample;
		Candidate.MotionTime = Prepared->MotionEndTime;
		if (Candidate.MotionType == EGGYGOLocomotionMotionType::TurnBack
			&& (SimulationTime > 0.0f || OriginalSet->TurnBackDurationSeconds == 0.0f))
		{
			const float EndElapsed = Candidate.TurnElapsed + SimulationTime;
			if (!FMath::IsFinite(Candidate.TurnElapsed) || Candidate.TurnElapsed < 0.0f
				|| !FMath::IsFinite(EndElapsed))
			{
				return Reject(TEXT("CurveRMS actual turn elapsed interval is non-finite or negative."));
			}
			AdvanceTurnBackPhase(SimulationTime, Candidate, true);
		}
		if (!Evaluation.Sample.bHasCurveSource || !FMath::IsFinite(Candidate.TurnElapsed))
		{
			return Reject(TEXT("CurveRMS successful sample has no authored source or finite turn elapsed time."));
		}
		const bool bNaturalEnd = (SimulationTime > 0.0f && Origin->bEndOnZeroSpeed
			&& Evaluation.Sample.Speed == 0.0f)
			|| (Origin->MotionType == EGGYGOLocomotionMotionType::TurnBack
				&& Candidate.TurnPhase != EGGYGOTurnBackPhase::Turning
				&& Candidate.TurnPhase != EGGYGOTurnBackPhase::Braking);
		if (bNaturalEnd)
		{
			Prepared->OverrideVelocity = Velocity;
			Prepared->Result = EGGYGOCurveRootMotionPrepareResult::Finished;
		}
		else
		{
			// This is an endpoint sample at the actual native interval, not a translation integral.
			const float Ratio = SimulationTime / MovementTickTime;
			if (!FMath::IsFinite(Ratio)) return Reject(TEXT("CurveRMS native simulation/tick ratio is non-finite."));
			const float BasisYaw = bAuthorityReplay && Origin->MotionType == EGGYGOLocomotionMotionType::TurnBack
				? Candidate.TurnEntryYaw : Origin->BaseYaw;
			Prepared->OverrideVelocity = FRotator(0.0f, BasisYaw, 0.0f)
				.RotateVector(Evaluation.ScaledVelocity) * Ratio;
			Prepared->Result = EGGYGOCurveRootMotionPrepareResult::Prepared;
		}
	}
	if (!IsFiniteLocomotionCurveVector(Prepared->OverrideVelocity))
	{
		return Reject(TEXT("CurveRMS final velocity/momentum is non-finite."));
	}
	TSharedPtr<FGGYGOLocomotionPreparedState> State = MakeShared<FGGYGOLocomotionPreparedState>();
	State->Candidate = Candidate;
	Prepared->State = State;
	OutPrepared = Prepared;
	if (bReplayPrepare)
	{
		ReplayLocomotionCurvePrepared = Prepared;
		bReplayPreparedFromAuthority = bAuthorityReplay;
	}
	else
	{
		ConsumeLocomotionCurvePrepared(Prepared);
		PendingLocomotionCurveInput.Reset();
		if (Prepared->Result == EGGYGOCurveRootMotionPrepareResult::Finished)
		{
			CompletedLocomotionCurveOrigin = Origin;
			LocomotionCurveOrigin.Reset();
			PendingLocomotionCurveInput.Reset();
		}
	}
	return Prepared->Result;
}

void UGGYGOCharacterMovementComponent::ResolveGait(float DeltaSeconds)
{
	FLocomotionUpdateCandidate Candidate = CaptureLocomotionCandidate();
	ResolveGait(DeltaSeconds, Candidate);
	CommitLocomotionCandidate(Candidate);
}

void UGGYGOCharacterMovementComponent::UpdateWalkHoldTimer(
	EGGYGOGait FrameGait, bool bHasMoveInput, bool bBlocked, bool bOnGround,
	bool bMoveInputRising, bool bBlockReleased, float DeltaSeconds)
{
	UpdateWalkHoldTimer(FrameGait, bHasMoveInput, bBlocked, bOnGround,
		bMoveInputRising, bBlockReleased, DeltaSeconds, WalkHoldTimer);
}

void UGGYGOCharacterMovementComponent::SetLocomotionMotion(EGGYGOLocomotionMotionType NewType)
{
	FLocomotionUpdateCandidate Candidate = CaptureLocomotionCandidate();
	SetLocomotionMotion(NewType, Candidate);
	CommitLocomotionCandidate(Candidate);
}

void UGGYGOCharacterMovementComponent::UpdateWalkRunBlend(float DeltaSeconds)
{
	const uint64 ExpectedRequestSerial = LocomotionRequestSerial;
	FLocomotionUpdateCandidate Candidate = CaptureLocomotionCandidate();
	FString Error;
	if (!UpdateWalkRunBlend(DeltaSeconds, Candidate, Error))
	{
		RejectLocomotionEvaluation(ExpectedRequestSerial, Error);
		return;
	}
	CommitLocomotionCandidate(Candidate);
}

void UGGYGOCharacterMovementComponent::ResolveGait(float DeltaSeconds, FLocomotionUpdateCandidate& Candidate) const
{
	// 用 Acceleration 而非原始摇杆输入判定"有没有移动意图"。
	// 关键原因是 Acceleration 已被 FSavedMove_Character 保存，
	// move 回放时取值与首次执行一致；原始输入没有这个保证。
	const bool bHasMoveInput = HasMoveInput();
	const bool bBlocked = IsMovementBlockedByTag();
	const bool bOnGround = IsMovingOnGround();

	// 边沿：起步瞬间与解禁瞬间都要让计时器重新开始，
	// 否则"走两秒→松手→再按"会接着之前的两秒继续累加，等效于缩短了阈值。
	const bool bMoveInputRising = bHasMoveInput && !Candidate.bPreviousInput;
	const bool bBlockReleased = !bBlocked && Candidate.bPreviousBlocked;

	EGGYGOGait FrameGait = EGGYGOGait::None;

	if (bBlocked || !bOnGround)
	{
		// 被禁止移动时不消费 Run 契约 —— 禁止解除后玩家仍然期望闪避后直接跑。
		FrameGait = EGGYGOGait::None;
	}
	else if (!bHasMoveInput)
	{
		// 松手即结束本次移动，同时丢弃未使用的契约：
		// 闪避后如果玩家没有立刻接移动，那个"直接进 Run"的意图就已经过期了。
		Candidate.bWantsRun = false;
		FrameGait = EGGYGOGait::None;
	}
	else if (bForceWalkRequested)
	{
		FrameGait = EGGYGOGait::Walk;
	}
	else if (Candidate.bWantsRun)
	{
		// 契约只在这里消费。
		Candidate.bWantsRun = false;
		FrameGait = EGGYGOGait::Run;
	}
	else if (Candidate.Gait == EGGYGOGait::Run)
	{
		// 单向滞回：一旦进入 Run 就保持，直到完全停止移动才降档。
		// 若允许回落 Walk，摇杆幅度的轻微抖动会造成走跑反复切换。
		FrameGait = EGGYGOGait::Run;
	}
	else
	{
		FrameGait = EGGYGOGait::Walk;
	}

	UpdateWalkHoldTimer(FrameGait, bHasMoveInput, bBlocked || bForceWalkRequested,
		bOnGround, bMoveInputRising, bBlockReleased, DeltaSeconds, Candidate.WalkHoldSeconds);

	// 计时达标则本帧立即升档并归零，不等下一帧 —— 延后一帧会让升档时机
	// 与配置的阈值差一个帧时长，在低帧率下可感知。
	if (FrameGait == EGGYGOGait::Walk && bHasMoveInput && !bBlocked && !bForceWalkRequested && bOnGround
		&& MovementSet && Candidate.WalkHoldSeconds >= MovementSet->WalkToRunHoldSeconds)
	{
		FrameGait = EGGYGOGait::Run;
		Candidate.WalkHoldSeconds = 0.0f;
	}

	Candidate.Gait = FrameGait;

	Candidate.bPreviousInput = bHasMoveInput;
	Candidate.bPreviousBlocked = bBlocked;
}

void UGGYGOCharacterMovementComponent::UpdateWalkHoldTimer(EGGYGOGait FrameGait, bool bHasMoveInput, bool bBlocked, bool bOnGround, bool bMoveInputRising, bool bBlockReleased, float DeltaSeconds, float& Timer) const
{
	// DeltaTime 不可信时**什么都不做**，既不归零也不累加。
	// 归零会让偶发的坏帧白白清掉玩家已积累的行走时间。
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0f)
	{
		return;
	}

	if (!FMath::IsFinite(Timer))
	{
		Timer = 0.0f;
	}

	// 任一条成立就归零重来。两个"沿"条件不能省：
	// 没有它们的话，"走两秒 → 松手 → 立刻再按"会接着之前的两秒继续累加，
	// 等效于把阈值缩短了，玩家会觉得升跑时机飘忽。
	const bool bShouldReset =
		!bHasMoveInput
		|| bBlocked
		|| !bOnGround
		|| FrameGait == EGGYGOGait::Run
		|| bMoveInputRising
		|| bBlockReleased;

	if (bShouldReset)
	{
		Timer = 0.0f;
		return;
	}

	if (FrameGait == EGGYGOGait::Walk)
	{
		const float ClampedDelta = FMath::Clamp(DeltaSeconds, 0.0f, GGYGOMovementConstants::MaxClampedDelta);
		Timer = FMath::Clamp(Timer + ClampedDelta, 0.0f, GGYGOMovementConstants::MaxWalkHoldSeconds);
	}
}

void UGGYGOCharacterMovementComponent::SetLocomotionMotion(EGGYGOLocomotionMotionType NewType, FLocomotionUpdateCandidate& Candidate) const
{
	if (Candidate.MotionType == NewType)
	{
		return;
	}

	Candidate.MotionType = NewType;
	Candidate.MotionTime = 0.0f;
	if (NewType == EGGYGOLocomotionMotionType::WalkRun)
	{
		Candidate.CyclePhase = 0.0f;
	}
	++Candidate.MotionSequence;
}

bool UGGYGOCharacterMovementComponent::UpdateWalkRunBlend(
	float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const
{
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0f)
	{
		OutError = FString::Printf(TEXT("Locomotion DeltaSeconds=%.9g must be finite and non-negative."), DeltaSeconds);
		return false;
	}
	if (!HasMoveInput())
	{
		Candidate.BlendAlpha = 0.0f;
		return true;
	}
	if (!HasAcceptedMovementSet())
	{
		OutError = TEXT("WalkRun blend has no accepted MovementSet");
		return false;
	}
	const float Speed = MovementSet->WalkRunBlendInterpSpeed;
	if (!FMath::IsFinite(Speed) || Speed < 0.0f
		|| !FMath::IsFinite(Candidate.BlendAlpha) || Candidate.BlendAlpha < 0.0f || Candidate.BlendAlpha > 1.0f)
	{
		OutError = FString::Printf(
			TEXT("MovementSet '%s': WalkRunBlendInterpSpeed=%.9g must be finite and non-negative; current BlendAlpha=%.9g must be finite and in [0,1]."),
			*GetPathNameSafe(MovementSet.Get()), Speed, Candidate.BlendAlpha);
		return false;
	}
	const float Target = Candidate.Gait == EGGYGOGait::Run ? 1.0f : 0.0f;
	Candidate.BlendAlpha = Speed == 0.0f ? Target : FMath::Clamp(FMath::FInterpConstantTo(
		Candidate.BlendAlpha, Target, FMath::Min(DeltaSeconds, GGYGOMovementConstants::MaxClampedDelta), Speed), 0.0f, 1.0f);
	return true;
}

bool UGGYGOCharacterMovementComponent::IsCurrentMotionFinished(
	const FLocomotionUpdateCandidate& Candidate, bool& bOutFinished, FString& OutError) const
{
	bOutFinished = false;
	if (!MovementSet->bUseCurveDrivenSpeed)
	{
		// Explicit fixed mode has no source-driven start/stop timeline.
		bOutFinished = true;
		return true;
	}
	FGGYGOLocomotionSourceBindingPtr Binding;
	if (!GetLocomotionSourceBinding(Binding, OutError)) return false;
	const FGGYGOLocomotionSequenceSource* Source = Binding->GetSingleSource(Candidate.MotionType);
	if (!Source)
	{
		OutError = FString::Printf(TEXT("Animation source route missing for motion %d."), static_cast<int32>(Candidate.MotionType));
		return false;
	}
	FGGYGOLocomotionEvaluationResult CompletedInterval;
	if (!GGYGOLocomotionEvaluation::EvaluateSingleInterval(*Source, Candidate.MotionTime, Candidate.MotionTime,
		MovementSet->RootMotionScale, CompletedInterval, &OutError))
	{
		return false;
	}
	bOutFinished = static_cast<double>(Candidate.MotionTime) * Source->SequenceRateScale >= Source->PlayLength;
	return true;
}

bool UGGYGOCharacterMovementComponent::EvaluateWalkRunSource(
	float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const
{
	FGGYGOWalkRunEvaluationResult Result;
	FGGYGOLocomotionSourceBindingPtr Binding;
	if (!GetLocomotionSourceBinding(Binding, OutError)) return false;
	if (!GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(
		*Binding, Candidate.CyclePhase,
		DeltaSeconds, Candidate.BlendAlpha, MovementSet->RootMotionScale, Result, &OutError))
	{
		return false;
	}
	const float EndPhase = FMath::Fmod(Result.EndCyclePosition, 1.0f);
	if (!FMath::IsFinite(EndPhase) || EndPhase < 0.0f || EndPhase >= 1.0f)
	{
		OutError = FString::Printf(TEXT("WalkRun cycle commit rejected: EndCyclePosition=%.9g cannot produce a finite phase in [0,1)."),
			Result.EndCyclePosition);
		return false;
	}
	Candidate.Motion = Result.Motion.Sample;
	Candidate.CyclePhase = EndPhase;
	return true;
}

bool UGGYGOCharacterMovementComponent::EvaluateLocomotionSource(
	float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const
{
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0f)
	{
		OutError = FString::Printf(TEXT("Locomotion DeltaSeconds=%.9g must be finite and non-negative."), DeltaSeconds);
		return false;
	}
	const float AcceptedDelta = FMath::Min(DeltaSeconds, GGYGOMovementConstants::MaxClampedDelta);
	if (Candidate.MotionType == EGGYGOLocomotionMotionType::None)
	{
		Candidate.Motion.Reset();
		return true;
	}
	if (!MovementSet->bUseCurveDrivenSpeed)
	{
		if (Candidate.MotionType == EGGYGOLocomotionMotionType::WalkRun)
		{
			Candidate.Motion.Reset();
			return true;
		}
		const float EndTime = Candidate.MotionTime + AcceptedDelta;
		if (!FMath::IsFinite(EndTime) || EndTime < 0.0f)
		{
			OutError = FString::Printf(TEXT("Fixed-mode locomotion time [%.9g, %.9g] is invalid."),
				Candidate.MotionTime, EndTime);
			return false;
		}
		Candidate.Motion.Reset();
		Candidate.MotionTime = EndTime;
		return true;
	}
	if (Candidate.MotionType == EGGYGOLocomotionMotionType::WalkRun)
	{
		return EvaluateWalkRunSource(AcceptedDelta, Candidate, OutError);
	}

	FGGYGOLocomotionSourceBindingPtr Binding;
	if (!GetLocomotionSourceBinding(Binding, OutError)) return false;
	const FGGYGOLocomotionSequenceSource* Source = Binding->GetSingleSource(Candidate.MotionType);
	if (!Source)
	{
		OutError = FString::Printf(TEXT("Animation source route missing for motion %d."), static_cast<int32>(Candidate.MotionType));
		return false;
	}
	FGGYGOLocomotionEvaluationResult Result;
	const float EndTime = Candidate.MotionTime + AcceptedDelta;
	if (!GGYGOLocomotionEvaluation::EvaluateSingleInterval(*Source, Candidate.MotionTime, EndTime,
		MovementSet->RootMotionScale, Result, &OutError))
	{
		return false;
	}
	Candidate.Motion = Result.Sample;
	Candidate.MotionTime = EndTime;
	return true;
}

void UGGYGOCharacterMovementComponent::BeginTurnBack(FLocomotionUpdateCandidate& Candidate) const
{
	Candidate.TurnPhase = EGGYGOTurnBackPhase::Turning;
	Candidate.TurnElapsed = 0.0f;
	Candidate.bTurnInputLatched = true;
	Candidate.TurnEntryYaw = UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : 0.0f;
	SetLocomotionMotion(EGGYGOLocomotionMotionType::TurnBack, Candidate);
}

void UGGYGOCharacterMovementComponent::AdvanceTurnBackPhase(
	float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, bool bNativeInterval) const
{
	if (Candidate.TurnPhase == EGGYGOTurnBackPhase::None || !MovementSet)
	{
		return;
	}

	const float ClampedDelta = FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0.0f
		? (bNativeInterval ? DeltaSeconds : FMath::Min(DeltaSeconds, GGYGOMovementConstants::MaxClampedDelta))
		: 0.0f;
	Candidate.TurnElapsed += ClampedDelta;
	if (Candidate.TurnElapsed >= FMath::Max(MovementSet->TurnBackDurationSeconds, 0.0f))
	{
		ResetTurnBack(Candidate);
		return;
	}

	if (Candidate.TurnPhase == EGGYGOTurnBackPhase::Turning)
	{
		const bool bYawReached = FMath::Abs(Candidate.Motion.YawTotalDegrees)
			>= FMath::Max(MovementSet->TurnBackMinYawDegrees, 0.0f);
		const bool bYawSettled = FMath::Abs(Candidate.Motion.YawDeltaDegrees)
			<= FMath::Max(MovementSet->TurnBackYawSettleDegrees, 0.0f);
		if (bYawReached && bYawSettled)
		{
			Candidate.TurnPhase = EGGYGOTurnBackPhase::Braking;
		}
	}
	else if (Candidate.TurnPhase == EGGYGOTurnBackPhase::Braking)
	{
		const bool bForwardFlipped = Candidate.Motion.Direction.X
			<= -FMath::Clamp(MovementSet->TurnBackRunOutForwardThreshold, 0.0f, 1.0f);
		const bool bFastEnough = Candidate.Motion.Speed
			>= FMath::Max(MovementSet->TurnBackRunOutMinSpeed, 0.0f);
		if (bForwardFlipped && bFastEnough)
		{
			Candidate.TurnPhase = EGGYGOTurnBackPhase::RunOut;
		}
	}
}

bool UGGYGOCharacterMovementComponent::UpdateLocomotionMotion(
	float DeltaSeconds, bool bHadMoveInput, EGGYGOGait PreviousGait)
{
	return TryUpdateLocomotion(DeltaSeconds, false, bHadMoveInput, PreviousGait);
}

bool UGGYGOCharacterMovementComponent::TryUpdateLocomotion(
	float DeltaSeconds, bool bResolveFrameGait, bool bHadMoveInput, EGGYGOGait PreviousGait)
{
	if (IsMovementInputRequestBlocked() || ShouldRejectUnownedCurveGroundLocomotion())
	{
		EnforceGroundLocomotionAdmission(TEXT("TryUpdateLocomotion"));
		return false;
	}
	const uint64 ExpectedRequestSerial = LocomotionRequestSerial;
	FLocomotionUpdateCandidate Candidate = CaptureLocomotionCandidate();
	FString Error;
	const auto Reject = [this, ExpectedRequestSerial, &Error]()
	{
		return RejectLocomotionEvaluation(ExpectedRequestSerial, Error);
	};
	if (!HasAcceptedMovementSet())
	{
		Error = TEXT("locomotion update has no accepted MovementSet");
		return Reject();
	}
	if (bResolveFrameGait)
	{
		ResolveGait(DeltaSeconds, Candidate);
	}
	const bool bHasInput = HasMoveInput();
	const bool bCanUseTurnBack = MovementSet->bUseCurveDrivenSpeed;

	if (!IsMovingOnGround() || IsMovementBlockedByTag())
	{
		ResetTurnBack(Candidate);
		Candidate.StopType = EGGYGOStopMotionType::None;
		SetLocomotionMotion(EGGYGOLocomotionMotionType::None, Candidate);
		Candidate.Motion.Reset();
		CommitLocomotionCandidate(Candidate);
		return true;
	}
	if (!UpdateWalkRunBlend(DeltaSeconds, Candidate, Error))
	{
		return Reject();
	}

	if (!bCanUseTurnBack && Candidate.TurnPhase != EGGYGOTurnBackPhase::None)
	{
		ResetTurnBack(Candidate);
	}
	if (Candidate.TurnPhase == EGGYGOTurnBackPhase::RunOut)
	{
		if (!bHasInput)
		{
			ResetTurnBack(Candidate);
		}
		else
		{
			SetLocomotionMotion(EGGYGOLocomotionMotionType::WalkRun, Candidate);
		}
	}
	if (Candidate.TurnPhase == EGGYGOTurnBackPhase::None
		&& bCanUseTurnBack && IsReverseRunInput(Candidate.Gait))
	{
		if (!Candidate.bTurnInputLatched)
		{
			BeginTurnBack(Candidate);
		}
	}
	else if (!IsReverseRunInput(Candidate.Gait) && Candidate.TurnPhase == EGGYGOTurnBackPhase::None)
	{
		Candidate.bTurnInputLatched = false;
	}

	if (Candidate.TurnPhase == EGGYGOTurnBackPhase::Turning || Candidate.TurnPhase == EGGYGOTurnBackPhase::Braking)
	{
		Candidate.StopType = EGGYGOStopMotionType::None;
		SetLocomotionMotion(EGGYGOLocomotionMotionType::TurnBack, Candidate);
		if (!StageLocomotionCurveRootMotion(DeltaSeconds, Candidate, Error))
		{
			return Reject();
		}
		return true;
	}

	if (bHasInput)
	{
		Candidate.StopType = EGGYGOStopMotionType::None;
		const bool bCanStart = !bHadMoveInput && Candidate.Gait == EGGYGOGait::Walk
			&& MovementSet->bUseCurveDrivenSpeed;
		if (bCanStart)
		{
			SetLocomotionMotion(EGGYGOLocomotionMotionType::WalkStart, Candidate);
		}
		else if (Candidate.MotionType != EGGYGOLocomotionMotionType::WalkStart)
		{
			SetLocomotionMotion(EGGYGOLocomotionMotionType::WalkRun, Candidate);
		}
		else
		{
			bool bFinished = false;
			if (!IsCurrentMotionFinished(Candidate, bFinished, Error))
			{
				return Reject();
			}
			if (bFinished)
			{
				SetLocomotionMotion(EGGYGOLocomotionMotionType::WalkRun, Candidate);
			}
		}
	}
	else if (bHadMoveInput)
	{
		const bool bStoppedDuringStart = Candidate.MotionType == EGGYGOLocomotionMotionType::WalkStart
			&& Candidate.MotionTime <= FMath::Max(MovementSet->StartStopSelectionSeconds, 0.0f);
		if (bStoppedDuringStart)
		{
			Candidate.StopType = EGGYGOStopMotionType::StartStop;
			SetLocomotionMotion(EGGYGOLocomotionMotionType::StartStop, Candidate);
		}
		else if (PreviousGait == EGGYGOGait::Run)
		{
			Candidate.StopType = EGGYGOStopMotionType::RunStop;
			SetLocomotionMotion(EGGYGOLocomotionMotionType::RunStop, Candidate);
		}
		else
		{
			Candidate.StopType = EGGYGOStopMotionType::WalkStop;
			SetLocomotionMotion(EGGYGOLocomotionMotionType::WalkStop, Candidate);
		}
	}
	else if (Candidate.MotionType == EGGYGOLocomotionMotionType::StartStop
		|| Candidate.MotionType == EGGYGOLocomotionMotionType::WalkStop
		|| Candidate.MotionType == EGGYGOLocomotionMotionType::RunStop)
	{
		bool bFinished = false;
		if (!IsCurrentMotionFinished(Candidate, bFinished, Error))
		{
			return Reject();
		}
		if (bFinished)
		{
			SetLocomotionMotion(EGGYGOLocomotionMotionType::None, Candidate);
		}
	}
	if (MovementSet->bUseCurveDrivenSpeed
		&& (Candidate.MotionType == EGGYGOLocomotionMotionType::StartStop
			|| Candidate.MotionType == EGGYGOLocomotionMotionType::WalkStop
			|| Candidate.MotionType == EGGYGOLocomotionMotionType::RunStop))
	{
		if (!StageLocomotionCurveRootMotion(DeltaSeconds, Candidate, Error)) return Reject();
		return true;
	}
	if (!EvaluateLocomotionSource(DeltaSeconds, Candidate, Error))
	{
		return Reject();
	}
	CommitLocomotionCandidate(Candidate);
	return true;
}

void UGGYGOCharacterMovementComponent::ResetTurnBack(FLocomotionUpdateCandidate& Candidate) const
{
	Candidate.TurnPhase = EGGYGOTurnBackPhase::None;
	Candidate.TurnElapsed = 0.0f;
	Candidate.TurnEntryYaw = 0.0f;
	// The existing reverse-input latch is retained until input leaves its threshold.
}

void UGGYGOCharacterMovementComponent::ValidateClientLocomotionHint() const
{
	if (!CharacterOwner || CharacterOwner->GetLocalRole() != ROLE_Authority
		|| CharacterOwner->IsLocallyControlled())
	{
		return;
	}

	const FCharacterNetworkMoveData_GGYGO* MoveData =
		static_cast<const FCharacterNetworkMoveData_GGYGO*>(GetCurrentNetworkMoveData());
	if (!MoveData)
	{
		return;
	}

	// 这些值只用于发现客户端/服务端预测分歧。服务端不会从这里取动画源、速度或时间。
	if (MoveData->LocomotionMotionType != LocomotionMotionType
		|| MoveData->StopMotionType != StopMotionType)
	{
		UE_LOG(LogTemp, VeryVerbose,
			TEXT("Locomotion prediction hint mismatch. ClientMotion=%d ServerMotion=%d ClientStop=%d ServerStop=%d Sequence=%u"),
			static_cast<uint8>(MoveData->LocomotionMotionType),
			static_cast<uint8>(LocomotionMotionType),
			static_cast<uint8>(MoveData->StopMotionType),
			static_cast<uint8>(StopMotionType),
			MoveData->LocomotionMotionSequence);
	}
}

void UGGYGOCharacterMovementComponent::UpdateFromCompressedFlags(uint8 Flags)
{
	Super::UpdateFromCompressedFlags(Flags);
	// ForceWalk 是输入请求，可随 move 发送；步态、动作段、时间与曲线由服务端
	// 根据同一 Acceleration + MovementSet 自己推进，不采用客户端结果。
	bForceWalkRequested = (Flags & GGYGOMovementConstants::ForceWalkFlag) != 0;
}

void UGGYGOCharacterMovementComponent::PhysicsRotation(float DeltaTime)
{
	// The final PrepareRootMotion can mark Finished before this call. Retain the entry rotation for that tick.
	if (ActiveActionMotionHandle != INDEX_NONE || (IsMovingOnGround() && HasRegisteredActionCurveSource()))
	{
		bLocomotionSteeringIntervalExcluded = true;
		return;
	}
	if (ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		EnforceGroundLocomotionAdmission(TEXT("PhysicsRotation"));
		return;
	}
	// 转身的曲线接管段（Turning / Braking）朝向由 `RootMotion_Yaw` 曲线驱动。
	//
	// 在这里接管而不是在外部调 AddActorWorldRotation，是为了不与 CMC 的
	// 自动朝向对齐（bOrientRotationToMovement）互相争夺 Yaw ——
	// 绕过 Super 就等于这一帧只有曲线在转角色，不需要临时关掉那些开关再恢复。
	//
	// 累加增量而不是直接设成 `EntryYaw + YawTotal`：增量对入口朝向的误差不敏感，
	// 模拟代理那边的入口朝向是个近似值，用绝对值会让它一进入转身就跳一下。
	// 曲线的累计转角已解 ±180 折叠，所以差分不会出现一帧 358 度的假增量。
	const bool bCanApplyLocalCurveYaw = CharacterOwner
		&& CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy;
	if (bCanApplyLocalCurveYaw && IsTurnBackCurveDriven() && UpdatedComponent)
	{
		bLocomotionSteeringIntervalExcluded = true;
		const float YawDelta = FMath::IsFinite(CurveMotion.YawDeltaDegrees)
			? CurveMotion.YawDeltaDegrees
			: 0.0f;

		if (!FMath::IsNearlyZero(YawDelta))
		{
			FRotator NewRotation = UpdatedComponent->GetComponentRotation();
			NewRotation.Yaw += YawDelta;

			// 走 MoveUpdatedComponent 而不是直接设置变换：它会处理碰撞扫掠，
			// 也让这次旋转进入 CMC 的移动更新记录，回放时结果一致。
			MoveUpdatedComponent(FVector::ZeroVector, NewRotation.Quaternion(), /*bSweep=*/true);
		}

		return;
	}

	LocomotionSteeringRotationBudget.Reset();
	if (CanExecuteOrdinaryLocomotionSteering() && Velocity.SizeSquared2D() > UE_KINDA_SMALL_NUMBER)
	{
		float Budget = 0.0f;
		FString Error;
		const float ErrorDegrees = FMath::FindDeltaAngleDegrees(UpdatedComponent->GetComponentRotation().Yaw, Velocity.Rotation().Yaw);
		if (!GGYGOLocomotionSteeringEvaluation::EvaluateAngularBudget(*MovementSet, ErrorDegrees, WalkRunBlendAlpha, DeltaTime, Budget, Error))
		{
			RejectLocomotionSteering(Error);
			return;
		}
		LocomotionSteeringRotationBudget = Budget;
	}
	Super::PhysicsRotation(DeltaTime);
	LocomotionSteeringRotationBudget.Reset();
}

void UGGYGOCharacterMovementComponent::ResetLocomotionSteeringObservation()
{
	if (LocomotionSteeringSourceEpoch != MAX_uint64) ++LocomotionSteeringSourceEpoch;
	LocomotionSteeringSnapshot = {};
	LocomotionSteeringSnapshot.OriginalMovement = this;
	LocomotionSteeringSnapshot.OriginalCharacter = CharacterOwner;
	LocomotionSteeringSnapshot.OriginalUpdatedComponent = UpdatedComponent.Get();
	LocomotionSteeringSnapshot.SourceEpoch = LocomotionSteeringSourceEpoch;
	LocomotionSteeringOriginalSet = MovementSet.Get();
	LocomotionSteeringOriginalSource = LocomotionSourceBinding;
	bLocomotionSteeringIntervalOpen = false;
	bLocomotionSteeringIntervalOrdinary = false;
	bLocomotionSteeringIntervalExcluded = false;
	LocomotionSteeringIntervalError.Reset();
	LocomotionSteeringRotationBudget.Reset();
	bLocomotionSteeringDiagnosticReported = false;
}

void UGGYGOCharacterMovementComponent::BeginLocomotionSteeringInterval()
{
	if (LocomotionSteeringSnapshot.OriginalCharacter.Get() != CharacterOwner
		|| LocomotionSteeringSnapshot.OriginalUpdatedComponent.Get() != UpdatedComponent
		|| LocomotionSteeringOriginalSet.Get() != MovementSet
		|| LocomotionSteeringOriginalSource != LocomotionSourceBinding)
		ResetLocomotionSteeringObservation();
	bLocomotionSteeringIntervalOpen = IsValid(CharacterOwner) && IsValid(UpdatedComponent)
		&& !CharacterOwner->bClientUpdating;
	bLocomotionSteeringIntervalOrdinary = false;
	bLocomotionSteeringIntervalExcluded = HasActiveActionMotion() || HasIndependentGroundRootMotion()
		|| HasCurveRootMotionSource() || IsTurnBackCurveDriven();
	LocomotionSteeringIntervalError.Reset();
	LocomotionSteeringRotationBudget.Reset();
	LocomotionSteeringEntryYaw = UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : 0.0f;
}

bool UGGYGOCharacterMovementComponent::CanExecuteOrdinaryLocomotionSteering() const
{
	return IsValid(CharacterOwner) && IsValid(UpdatedComponent) && HasAcceptedMovementSet()
		&& MovementSet->bEnableLocomotionSteering && bOrientRotationToMovement && IsMovingOnGround()
		&& CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy && HasMoveInput()
		&& !ShouldRejectMovementInputGroundLocomotion() && !ShouldRejectUnownedCurveGroundLocomotion()
		&& !HasActiveActionMotion() && !HasIndependentGroundRootMotion() && !IsTurnBackCurveDriven()
		&& !CurrentRootMotion.HasOverrideVelocity() && !CurrentRootMotion.HasAdditiveVelocity()
		&& !GetUnsupportedGroundRootMotionSource();
}

void UGGYGOCharacterMovementComponent::RejectLocomotionSteering(const FString& Error)
{
	const FString Diagnostic = FString::Printf(
		TEXT("[Movement.LocomotionSteering] Component='%s', Character='%s', MovementSet='%s': %s"),
		*GetPathName(), *GetPathNameSafe(CharacterOwner), *GetPathNameSafe(MovementSet.Get()), *Error);
	const bool bReport = !bLocomotionSteeringDiagnosticReported;
	RejectLocomotionEvaluation(LocomotionRequestSerial, Diagnostic);
	LocomotionSteeringIntervalError = Diagnostic;
	LocomotionSteeringSnapshot.Status = EGGYGOLocomotionSteeringStatus::Invalid;
	LocomotionSteeringSnapshot.Diagnostic = Diagnostic;
	LocomotionSteeringSnapshot.bHasVelocityYawRate = false;
	bLocomotionSteeringDiagnosticReported = true;
	if (bReport) UE_LOG(LogGGYGOMovement, Error, TEXT("%s"), *Diagnostic);
}

FRotator UGGYGOCharacterMovementComponent::ComputeOrientToMovementRotation(
	const FRotator& CurrentRotation, float DeltaTime, FRotator& DeltaRotation) const
{
	if (LocomotionSteeringRotationBudget.IsSet())
	{
		DeltaRotation.Yaw = LocomotionSteeringRotationBudget.GetValue();
		return FVector(Velocity.X, Velocity.Y, 0.0).Rotation();
	}
	return Super::ComputeOrientToMovementRotation(CurrentRotation, DeltaTime, DeltaRotation);
}

void UGGYGOCharacterMovementComponent::OnMovementUpdated(
	float DeltaSeconds, const FVector& OldLocation, const FVector& OldVelocity)
{
	Super::OnMovementUpdated(DeltaSeconds, OldLocation, OldVelocity);
	// Historical prediction/correction never certifies a new live presentation sample.
	if (!bLocomotionSteeringIntervalOpen || !CharacterOwner || CharacterOwner->bClientUpdating) return;
	bLocomotionSteeringIntervalOpen = false;
	if (LocomotionSteeringIntervalSerial == MAX_uint64 || LocomotionSteeringSourceEpoch == MAX_uint64)
	{
		RejectLocomotionSteering(TEXT("derived observation serial/epoch exhausted; cannot certify a new native interval."));
		return;
	}
	FGGYGOLocomotionSteeringSnapshot Completed;
	Completed.OriginalMovement = this;
	Completed.OriginalCharacter = CharacterOwner;
	Completed.OriginalUpdatedComponent = UpdatedComponent.Get();
	Completed.SourceEpoch = LocomotionSteeringSourceEpoch;
	Completed.CompletedIntervalSerial = ++LocomotionSteeringIntervalSerial;
	Completed.CompletedFrame = GFrameCounter;
	Completed.NativeDeltaSeconds = DeltaSeconds;
	Completed.Status = EGGYGOLocomotionSteeringStatus::NotApplicable;
	if (!LocomotionSteeringIntervalError.IsEmpty())
	{
		Completed.Status = EGGYGOLocomotionSteeringStatus::Invalid;
		Completed.Diagnostic = LocomotionSteeringIntervalError;
	}
	else if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds <= 0.0f || !UpdatedComponent
		|| !FMath::IsFinite(LocomotionSteeringEntryYaw) || UpdatedComponent->GetComponentRotation().ContainsNaN()
		|| OldVelocity.ContainsNaN() || Velocity.ContainsNaN() || Acceleration.ContainsNaN())
	{
		Completed.Status = EGGYGOLocomotionSteeringStatus::Invalid;
		Completed.Diagnostic = TEXT("native completed steering interval has invalid dt/transform/velocity/acceleration.");
	}
	else if (bLocomotionSteeringIntervalExcluded || !bLocomotionSteeringIntervalOrdinary
		|| !CanExecuteOrdinaryLocomotionSteering())
	{
		Completed.Diagnostic = TEXT("original interval was not wholly ordinary ground steering (Action/TurnBack/root motion/passive/proxy/no input).");
	}
	else
	{
		Completed.Status = EGGYGOLocomotionSteeringStatus::Valid;
		Completed.ActualSignedYawRate = FMath::FindDeltaAngleDegrees(
			LocomotionSteeringEntryYaw, UpdatedComponent->GetComponentRotation().Yaw) / DeltaSeconds;
		Completed.bHasVelocityYawRate = GGYGOLocomotionSteeringEvaluation::CalculateVelocityYawRate(
			OldVelocity, Velocity, DeltaSeconds, Completed.ActualSignedVelocityYawRate);
		Completed.DesiredDirectionError = FMath::FindDeltaAngleDegrees(
			UpdatedComponent->GetComponentRotation().Yaw, Acceleration.Rotation().Yaw);
		if (!FMath::IsFinite(Completed.ActualSignedYawRate) || !FMath::IsFinite(Completed.DesiredDirectionError))
		{
			Completed.Status = EGGYGOLocomotionSteeringStatus::Invalid;
			Completed.bHasVelocityYawRate = false;
			Completed.Diagnostic = TEXT("native completed capsule yaw derivative or admitted direction error is not finite.");
		}
	}
	if (Completed.Status == EGGYGOLocomotionSteeringStatus::Invalid)
	{
		Completed.Diagnostic = FString::Printf(TEXT("[Movement.LocomotionSteering] Component='%s', Character='%s', MovementSet='%s': %s"),
			*GetPathName(), *GetPathNameSafe(CharacterOwner), *GetPathNameSafe(MovementSet.Get()), *Completed.Diagnostic);
	}
	LocomotionSteeringCompletedVelocity = Velocity;
	LocomotionSteeringCompletedYaw = UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : 0.0f;
	LocomotionSteeringSnapshot = MoveTemp(Completed);
}

FGGYGOLocomotionSteeringSnapshot UGGYGOCharacterMovementComponent::GetLocomotionSteeringSnapshot() const
{
	FGGYGOLocomotionSteeringSnapshot Result = LocomotionSteeringSnapshot;
	const auto Qualify = [this, &Result](EGGYGOLocomotionSteeringStatus Status, const FString& Reason)
	{
		Result.Status = Status;
		Result.bHasVelocityYawRate = false;
		Result.Diagnostic = FString::Printf(TEXT("[Movement.LocomotionSteering] Component='%s', Character='%s', MovementSet='%s': %s"),
			*GetPathName(), *GetPathNameSafe(CharacterOwner), *GetPathNameSafe(MovementSet.Get()), *Reason);
		return Result;
	};
	if (!IsInGameThread())
	{
		Result.Status = EGGYGOLocomotionSteeringStatus::Invalid;
		Result.Diagnostic = TEXT("Movement steering capture requires the game thread.");
		return Result;
	}
	if (!IsValid(CharacterOwner) || !IsValid(UpdatedComponent) || IsBeingDestroyed()
		|| CharacterOwner->IsActorBeingDestroyed() || bMovementOwnerSyncClosed)
		return Qualify(EGGYGOLocomotionSteeringStatus::Invalid, TEXT("original Character/UpdatedComponent lifecycle is missing or closed."));
	if (Result.CompletedIntervalSerial == 0)
	{
		Result.OriginalMovement = const_cast<UGGYGOCharacterMovementComponent*>(this);
		Result.OriginalCharacter = CharacterOwner;
		Result.OriginalUpdatedComponent = UpdatedComponent.Get();
		Result.SourceEpoch = LocomotionSteeringSourceEpoch;
	}
	if (!HasAcceptedMovementSet())
	{
		if (IsOriginalMovementSetInitializationPending())
			return Qualify(EGGYGOLocomotionSteeringStatus::Initial,
				TEXT("original PawnExtension configuration initialization is not complete; Set entry has not submitted and no native interval has executed."));
		return Qualify(EGGYGOLocomotionSteeringStatus::Invalid, MovementSetConfigurationError.IsEmpty()
			? TEXT("required accepted MovementSet is missing outside the original initialization window.")
			: MovementSetConfigurationError);
	}
	FString Error;
	if (!GGYGOLocomotionSteeringEvaluation::ValidateConfiguration(*MovementSet, Error))
		return Qualify(EGGYGOLocomotionSteeringStatus::Invalid, Error);
	if (!MovementSet->bEnableLocomotionSteering)
		return Qualify(EGGYGOLocomotionSteeringStatus::NotApplicable, TEXT("ordinary steering is explicitly disabled in MovementSet."));
	if (MovementSet->bUseCurveDrivenSpeed && !IsLocomotionSourceBindingCurrent(LocomotionSourceBinding, Error))
		return Qualify(EGGYGOLocomotionSteeringStatus::Invalid, Error);
	if (Result.Status == EGGYGOLocomotionSteeringStatus::Invalid) return Result;
	if (Result.CompletedIntervalSerial == 0)
	{
		Result.Status = EGGYGOLocomotionSteeringStatus::Initial;
		Result.Diagnostic = TEXT("original dependencies accepted; no completed native steering interval yet.");
		return Result;
	}
	if (Result.OriginalMovement.Get() != this || Result.OriginalCharacter.Get() != CharacterOwner
		|| Result.OriginalUpdatedComponent.Get() != UpdatedComponent
		|| LocomotionSteeringOriginalSet.Get() != MovementSet || LocomotionSteeringOriginalSource != LocomotionSourceBinding)
		return Qualify(EGGYGOLocomotionSteeringStatus::Invalid, TEXT("completed sample no longer belongs to the original movement dependencies."));
	if (CharacterOwner->bClientUpdating || GFrameCounter < Result.CompletedFrame
		|| GFrameCounter - Result.CompletedFrame > 1
		|| !Velocity.Equals(LocomotionSteeringCompletedVelocity, UE_KINDA_SMALL_NUMBER)
		|| !FMath::IsNearlyZero(FMath::FindDeltaAngleDegrees(LocomotionSteeringCompletedYaw, UpdatedComponent->GetComponentRotation().Yaw)))
		return Qualify(EGGYGOLocomotionSteeringStatus::NotApplicable, TEXT("original completed native sample is stale or native correction/replay replaced its result."));
	if (Result.Status == EGGYGOLocomotionSteeringStatus::Valid && !CanExecuteOrdinaryLocomotionSteering())
		return Qualify(EGGYGOLocomotionSteeringStatus::NotApplicable, TEXT("ordinary steering control was released or native mode/root motion took over."));
	return Result;
}

EGGYGOTurnBackPhase UGGYGOCharacterMovementComponent::GetTurnBackPhase() const
{
	// 模拟代理没有本地解算的相位，只有复制来的一个 bool。
	// 映射成 Turning 是因为动画层只判断 `!= None`，具体是哪一段它不关心。
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		return bReplicatedTurnBackCurveDriven ? EGGYGOTurnBackPhase::Turning : EGGYGOTurnBackPhase::None;
	}

	return TurnBackPhase;
}

bool UGGYGOCharacterMovementComponent::IsTurnBackRunOut() const
{
	// 模拟代理恒为 false：复制过来的 bool 只表达"曲线是否在接管"，
	// 而 RunOut 段的移动与普通移动一致，那一端不需要区分。
	return GetTurnBackPhase() == EGGYGOTurnBackPhase::RunOut;
}

bool UGGYGOCharacterMovementComponent::IsTurnBackCurveDriven() const
{
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		return bReplicatedTurnBackCurveDriven;
	}

	return TurnBackPhase == EGGYGOTurnBackPhase::Turning
		|| TurnBackPhase == EGGYGOTurnBackPhase::Braking;
}

void UGGYGOCharacterMovementComponent::ResetTurnBack()
{
	TurnBackPhase = EGGYGOTurnBackPhase::None;
	TurnBackElapsed = 0.0f;
	TurnBackEntryYaw = 0.0f;

	// 不清 bTurnBackInputLatched：它的解闩条件是"输入离开反向阈值"，
	// 而转身刚结束时玩家往往还按着同一个方向键。在这里清掉会让那个方向键
	// 立刻触发下一次转身，角色原地反复转身。
}

bool UGGYGOCharacterMovementComponent::ApplyCurveRootMotionSource(
	FName InstanceName, uint16 Priority, float BaseYaw, bool bEndOnZeroSpeed)
{
	if (!PendingLocomotionCurveInput.IsValid()
		|| !HasCurrentLocomotionCurveOrigin(PendingLocomotionCurveInput->Origin)) return false;
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin> Origin = PendingLocomotionCurveInput->Origin;
	if (BaseYaw != Origin->BaseYaw || bEndOnZeroSpeed != Origin->bEndOnZeroSpeed) return false;
	const TSharedPtr<FRootMotionSource> Existing = GetRootMotionSource(InstanceName);
	if (Existing.IsValid())
	{
		if (Existing->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()) return false;
		const FRootMotionSource_GGYGOCurve* Curve = static_cast<const FRootMotionSource_GGYGOCurve*>(Existing.Get());
		if (Curve->Origin == Origin && !Existing->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
			&& !Existing->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval))
		{
			return true;
		}
		if (!Curve->Origin.IsValid() || Curve->Origin->Owner.Get() != this
			|| (!Existing->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
				&& !Existing->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval))) return false;
	}

	TSharedPtr<FRootMotionSource_GGYGOCurve> Source = MakeShared<FRootMotionSource_GGYGOCurve>();
	Source->InstanceName = InstanceName;
	Source->Priority = Priority;
	Source->BaseYaw = Origin->BaseYaw;
	Source->SpeedScale = Origin->RootMotionScale;
	Source->bEndOnZeroSpeed = Origin->bEndOnZeroSpeed;
	Source->Origin = Origin;
	return ApplyRootMotionSource(Source) != static_cast<uint16>(ERootMotionSourceID::Invalid);
}
void UGGYGOCharacterMovementComponent::UpdateTurnBackRootMotion()
{
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		return;
	}

	FGGYGOLocomotionSourceBindingPtr Binding;
	FString Error;
	const bool bCurveDrivenEnabled = MovementSet && MovementSet->bUseCurveDrivenSpeed
		&& GetLocomotionSourceBinding(Binding, Error);
	if (CharacterOwner && CharacterOwner->GetLocalRole() == ROLE_Authority)
	{
		bReplicatedTurnBackCurveDriven = bCurveDrivenEnabled && IsTurnBackCurveDriven();
	}

	if (!bCurveDrivenEnabled || !IsTurnBackCurveDriven())
	{
		RemoveRootMotionSource(GGYGOMovementConstants::CurveTurnBackSourceName);
		return;
	}

	// 基准取相位入口朝向而不是当前朝向：角色在这一段一直在转，
	// 用当前朝向会把已经转过的角度重复计入，直线滑行被拖成弧线。
	//
	// 不自我终结：生命周期由相位机管。转身中间那几帧曲线速度为 0 时
	// source 输出零速度把角色压住，而不是把控制权交还给正按着反方向键的玩家。
	ApplyCurveRootMotionSource(
		GGYGOMovementConstants::CurveTurnBackSourceName,
		GGYGOMovementConstants::CurveTurnBackPriority,
		TurnBackEntryYaw,
		/*bEndOnZeroSpeed=*/false);
}

void UGGYGOCharacterMovementComponent::UpdateCurveBrake()
{
	// 曲线驱动被整体关掉时不参与。这个开关是调试用的总闸，
	// 关掉之后移动应当完全回退到配置的固定速度。
	const bool bCurveDrivenEnabled = MovementSet && MovementSet->bUseCurveDrivenSpeed;

	// 转身有自己的一套曲线接管（含 `RunOut` 段刻意把方向交回输入），
	// 让刹停在那期间插手会覆盖掉那个设计。
	//
	// 有输入就摘掉：刹停可以被随时打断，玩家按方向应当立刻恢复正常移动。
	// 判据用 Acceleration 而非原始摇杆输入 —— 它已被 FSavedMove_Character 保存，
	// 回放时取值与首次执行一致。
	const bool bHasMoveInput = HasMoveInput();

	if (!bCurveDrivenEnabled
		|| bHasMoveInput
		|| TurnBackPhase != EGGYGOTurnBackPhase::None
		|| (LocomotionMotionType != EGGYGOLocomotionMotionType::StartStop
			&& LocomotionMotionType != EGGYGOLocomotionMotionType::WalkStop
			&& LocomotionMotionType != EGGYGOLocomotionMotionType::RunStop)
		|| !IsMovingOnGround())
	{
		RemoveRootMotionSource(GGYGOMovementConstants::CurveBrakeSourceName);
		return;
	}

	// 无输入而曲线仍在给速度 —— 只有 `_End` 这类刹停动画会出现这种组合，
	// 站立与静止兜底姿势的 `RootMotion_Speed` 恒为 0。所以这一个条件就够了，
	// 不需要额外记"松手那一帧"的下降沿。
	//
	// 不需要在这里判断"曲线停了没"：source 自己会在速度归零时置 Finished，
	// 引擎下一帧的 `CleanUpInvalidRootMotion` 会把它摘掉。
	if (!CurveMotion.HasUsableSpeed())
	{
		return;
	}

	// 基准取当前朝向：松手那一刻角色的朝向就是刹停动画的段起点朝向。
	// 自我终结：曲线衰减到 0 即滑行结束，不需要外部状态机跟着。
	ApplyCurveRootMotionSource(
		GGYGOMovementConstants::CurveBrakeSourceName,
		GGYGOMovementConstants::CurveBrakePriority,
		UpdatedComponent ? UpdatedComponent->GetComponentRotation().Yaw : 0.0f,
		/*bEndOnZeroSpeed=*/true);
}

bool UGGYGOCharacterMovementComponent::IsReverseRunInput() const
{
	return IsReverseRunInput(ResolvedGait);
}

bool UGGYGOCharacterMovementComponent::IsReverseRunInput(EGGYGOGait Gait) const
{
	// 只有跑动中才触发。走路时的反向输入应当直接转身走回去，
	// 那个速度下不需要刹车动作。
	if (Gait != EGGYGOGait::Run || !MovementSet)
	{
		return false;
	}

	const FVector InputDirection = GetCurrentAcceleration().GetSafeNormal2D();
	if (InputDirection.IsNearlyZero() || !UpdatedComponent)
	{
		return false;
	}

	const FVector Forward = UpdatedComponent->GetForwardVector().GetSafeNormal2D();
	if (Forward.IsNearlyZero())
	{
		return false;
	}

	return FVector::DotProduct(Forward, InputDirection) <= MovementSet->TurnBackReverseInputDotThreshold;
}

void UGGYGOCharacterMovementComponent::ServerMove_PerformMovement(const FCharacterNetworkMoveData& MoveData)
{
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	RefreshMovementOwnerSyncContext();
	if (!WeakSelf.IsValid() || WeakSelf->bMovementOwnerSyncClosed) return;
	FMovementOwnerSyncNativeMove Frame;
	// Packed data identity, not a class-name guess or replay's CurrentNetworkMoveData.
	for (const FCharacterNetworkMoveData_GGYGO& Candidate : NetworkMoveDataContainer.MoveData)
	{
		if (&Candidate == &MoveData) Frame.Move = &Candidate;
	}
	if (Frame.Move)
	{
		Frame.OriginalSourceCheckpoint = Frame.Move->MovementInputSourceCheckpoint;
		Frame.OriginalNonce = Frame.Move->MovementOwnerSyncNonce;
		Frame.OriginalClientGeneration = Frame.Move->MovementOwnerGeneration;
		Frame.OriginalTimeStamp = Frame.Move->TimeStamp;
		Frame.OriginalFlags = Frame.Move->CompressedMoveFlags;
		Frame.bHasOriginalSync = Frame.Move->bHasMovementOwnerSync;
		Frame.bOriginalNewMove = Frame.Move->NetworkMoveType == FCharacterNetworkMoveData::ENetworkMoveType::NewMove;
	}
	FMovementOwnerSyncNativeMove* Previous = ActiveMovementOwnerSyncNativeMove;
	const bool bNativeRemote = Frame.Move && GetCurrentNetworkMoveData() == &MoveData
		&& CharacterOwner && CharacterOwner->HasAuthority() && !CharacterOwner->IsLocallyControlled();
	const bool bRemoteOwnerCall = CharacterOwner && CharacterOwner->HasAuthority()
		&& !CharacterOwner->IsLocallyControlled() && Cast<APlayerController>(CharacterOwner->GetController());
	ActiveMovementOwnerSyncNativeMove = bRemoteOwnerCall ? &Frame : nullptr;
	Super::ServerMove_PerformMovement(MoveData);
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self) return;
	Self->ActiveMovementOwnerSyncNativeMove = Self->bMovementOwnerSyncClosed ? nullptr : Previous;
	if (!bNativeRemote || !Frame.bEnteredNativeSimulation
		|| !Frame.bOriginalNewMove
		|| !Self->IsMovementOwnerSyncReceiptCurrent(Frame.Receipt)) return;
	const FNetworkPredictionData_Server_Character* ServerData = Self->GetPredictionData_Server_Character();
	// Timestamp only joins this native adjustment to its exact receipt; never issues an epoch.
	if (ServerData && ServerData->PendingAdjustment.TimeStamp == Frame.Receipt.TimeStamp)
		Self->MovementOwnerSyncPendingReceipt = Frame.Receipt;
}

bool UGGYGOCharacterMovementComponent::ConsumeNativeMovementInputCheckpoint(
	const FMovementOwnerSyncNativeMove& OriginalMove)
{
	const FMovementOwnerSyncNativeMove* Frame = &OriginalMove;
	const FMovementOwnerSyncNativeReceipt& Receipt = OriginalMove.Receipt;
	if (Frame != ActiveMovementOwnerSyncNativeMove || !Frame->Move || !Frame->bEnteredNativeSimulation
		|| Frame->Move != GetCurrentNetworkMoveData() || !IsMovementOwnerSyncReceiptCurrent(Receipt)
		|| Frame->OriginalSourceCheckpoint != Frame->Move->MovementInputSourceCheckpoint) return false;
	const FGGYGOMovementInputSourceCheckpoint& Checkpoint = Frame->OriginalSourceCheckpoint;
	const auto Reject = [this, Frame, &Checkpoint](const FString& Reason)
	{
		ReportMovementOwnerSyncOnce(FName(TEXT("NativeSourceCheckpointRejected")), FString::Printf(
			TEXT("Original native source rejected: Generation=%llu, Nonce=%llu, Binding=%llu, Fence=%llu, Session=%llu, Event=%llu, Request=%llu, Reason='%s'."),
			static_cast<unsigned long long>(Frame->Receipt.OwnerGeneration),
			static_cast<unsigned long long>(Frame->OriginalNonce),
			static_cast<unsigned long long>(Checkpoint.BindingSerial),
			static_cast<unsigned long long>(Checkpoint.ConsumerFenceSerial),
			static_cast<unsigned long long>(Checkpoint.SessionSerial),
			static_cast<unsigned long long>(Checkpoint.EventSerial),
			static_cast<unsigned long long>(Checkpoint.RequestSerial), *Reason));
		return false;
	};
	FString Error;
	if (!Checkpoint.IsValid(&Error)) return Reject(Error);
	if (!Checkpoint.bPresent) return false; // Original no-source/Waiting move, never promoted by current Ready.
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	if (!IsMovementOwnerSyncContextCurrent(Context) || Context->bRequiresNativeResponse
		|| Context->Notice.State != EGGYGOMovementOwnerSyncState::Ready
		|| Context->Notice.ServerOwnerGeneration != Receipt.OwnerGeneration
		|| MovementInputBindingSerial != 0)
	{
		return Reject(TEXT("native source has no original authority owner scope, or conflicts with a local Source binding"));
	}
	const TSharedPtr<const FMovementInputNativeSource> OriginalSource = MovementInputNativeSource;
	const FGGYGOMovementInputSourceCheckpoint Previous = MovementInputSourceCheckpoint;
	const bool bNewBinding = OriginalSource.IsValid()
		&& (Checkpoint.BindingSerial != Previous.BindingSerial || Checkpoint.SessionSerial != Previous.SessionSerial);
	const bool bNewNonce = OriginalSource.IsValid() && OriginalSource->Receipt.Nonce != Receipt.Nonce;
	if ((Receipt.ClientOwnerGeneration != 0 && Receipt.ClientOwnerGeneration != Receipt.OwnerGeneration)
		|| (Receipt.ClientOwnerGeneration == 0 && OriginalSource.IsValid()
			&& OriginalSource->ExecutionRequestSerial != 0)) return false;
	if (OriginalSource.IsValid())
	{
		if (OriginalSource->OwnerScope != Context->Scope
			|| OriginalSource->Receipt.OwnerGeneration != Receipt.OwnerGeneration)
			return Reject(TEXT("previous native provenance belongs to an unretired different owner scope"));
		if (Checkpoint.BindingSerial == Previous.BindingSerial && Checkpoint.SessionSerial != Previous.SessionSerial)
			return Reject(TEXT("an original consumer binding serial was reused for a different Source session"));
		if (Checkpoint.BindingSerial < Previous.ConsumerFenceSerial
			|| (bNewBinding && Checkpoint.BindingSerial <= Previous.ConsumerFenceSerial)
			|| (!bNewBinding && Previous.bConsumerInvalidated))
			return false; // A delayed original report cannot retire or rearm its successor.
		if (bNewBinding && Checkpoint.SessionSerial == Previous.SessionSerial)
			return Reject(TEXT("new consumer binding reused the retired Source session"));
		if (!bNewBinding)
		{
			if (Checkpoint.SessionOpenedEventSerial != Previous.SessionOpenedEventSerial
				|| Checkpoint.SessionMode != Previous.SessionMode)
				return Reject(TEXT("original session opening/mode was rewritten"));
			if (Checkpoint.EventSerial < Previous.EventSerial || Checkpoint.RequestSerial < Previous.RequestSerial)
				return false;
			if (Checkpoint.EventSerial == Previous.EventSerial && Checkpoint != Previous)
			{
				FGGYGOMovementInputSourceCheckpoint Retired = Previous;
				Retired.bConsumerInvalidated = true;
				Retired.ConsumerFenceSerial = Checkpoint.ConsumerFenceSerial;
				if (!Checkpoint.bConsumerInvalidated || Retired != Checkpoint)
					return Reject(TEXT("an original event serial was reused for different checkpoint values"));
			}
			if (Checkpoint.NeutralEventSerial < Previous.NeutralEventSerial
				|| Checkpoint.SourceUnresolvedEventSerial < Previous.SourceUnresolvedEventSerial)
				return Reject(TEXT("original neutral/unresolved watermarks regressed"));
			if (Checkpoint.RequestSerial == Previous.RequestSerial
				&& (Checkpoint.RequestStartedEventSerial != Previous.RequestStartedEventSerial
					|| Checkpoint.StartProof != Previous.StartProof
					|| Checkpoint.StartReleaseRequestSerial != Previous.StartReleaseRequestSerial
					|| Checkpoint.StartReleaseEventSerial != Previous.StartReleaseEventSerial
					|| Checkpoint.StartNeutralEventSerial != Previous.StartNeutralEventSerial
					|| (Previous.RequestReleasedEventSerial != 0
						&& Checkpoint.RequestReleasedEventSerial != Previous.RequestReleasedEventSerial)))
				return Reject(TEXT("the same original request/start/release anchors were rewritten"));
		}
	}
	const bool bNewRequest = Checkpoint.RequestSerial != 0 && (!OriginalSource.IsValid()
		|| bNewBinding || Checkpoint.RequestSerial != Previous.RequestSerial);
	const bool bTerminal = Checkpoint.bConsumerInvalidated
		|| Checkpoint.RequestReleasedEventSerial != 0
		|| Checkpoint.SourceUnresolvedEventSerial > Checkpoint.RequestStartedEventSerial;
	if (bNewNonce && !bNewBinding && !bNewRequest)
		return Reject(TEXT("a successor nonce cannot borrow the original open request"));
	if (bNewRequest)
	{
		if (Checkpoint.StartProof == EGGYGOMovementInputStartProof::ColdPhysicalPress)
		{
			if ((OriginalSource.IsValid() && (bNewBinding || bNewNonce || Previous.RequestSerial != 0
				|| Previous.SourceUnresolvedEventSerial != 0 || Previous.bConsumerInvalidated))
				|| Checkpoint.BindingSerial != 1 || !Receipt.bInitialOwnerGeneration
				|| !Context->bInitialLocalScope || LocomotionRequestSerial != 0
				|| LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed)
				return Reject(TEXT("Cold cannot recover a previous request, binding, nonce or owner"));
		}
		else if (OriginalSource.IsValid() && !bNewBinding && Previous.RequestSerial != 0)
		{
			if (Checkpoint.RequestStartedEventSerial <= Previous.EventSerial
				|| Checkpoint.StartReleaseRequestSerial < Previous.RequestSerial
				|| Checkpoint.StartReleaseEventSerial <= Previous.RequestStartedEventSerial
				|| Checkpoint.StartNeutralEventSerial <= Checkpoint.StartReleaseEventSerial
				|| (Checkpoint.StartReleaseRequestSerial == Previous.RequestSerial
					&& Previous.RequestReleasedEventSerial != 0
					&& Checkpoint.StartReleaseEventSerial != Previous.RequestReleasedEventSerial))
				return Reject(TEXT("successor start lacks its original ordered release then neutral basis"));
		}
	}
	const bool bOriginalReady = Receipt.ClientOwnerGeneration != 0
		&& Receipt.ClientOwnerGeneration == Receipt.OwnerGeneration;
	if (bNewRequest && !bTerminal && !bOriginalReady) return false;
	if (!OriginalSource.IsValid() || bNewBinding || bNewNonce || bNewRequest)
	{
		if (OriginalSource.IsValid() && OriginalSource->ExecutionRequestSerial != 0
			&& OriginalSource->ExecutionRequestSerial == LocomotionRequestSerial) RevokeMovementInputRequest();
		TSharedPtr<FMovementInputNativeSource> Source = MakeShared<FMovementInputNativeSource>();
		Source->Receipt = Receipt;
		Source->OwnerScope = Context->Scope;
		Source->StartCheckpoint = Checkpoint;
		if (bNewRequest && !bTerminal)
		{
			if (LocomotionRequestSerial == MAX_uint64)
			{
				FailLocomotionRequest(LocomotionRequestSerial, TEXT("native CMC execution request serial is exhausted"));
				return Reject(TEXT("native CMC execution request serial is exhausted"));
			}
			Source->ExecutionRequestSerial = ++LocomotionRequestSerial;
			MovementInputRequestOwnerScope = Context->Scope;
			LocomotionRequestAdmission = ELocomotionRequestAdmission::Admitted;
			LocomotionRequestFailureReason.Reset();
			bMovementInputRequestOpen = true;
			bMovementInputAdmissionDiagnosticReported = false;
			const bool bKeepRunIntent = bWantsRunOnNextMove;
			const bool bKeepForceWalk = bForceWalkRequested;
			ResetLocomotionState();
			bWantsRunOnNextMove = bKeepRunIntent;
			bForceWalkRequested = bKeepForceWalk;
		}
		MovementInputNativeSource = Source;
	}
	// Cache only the successfully validated original facts; no local Source identity is manufactured.
	MovementInputSourceCheckpoint = Checkpoint;
	LastMovementInputRequestSerial = Checkpoint.RequestSerial;
	const TSharedPtr<const FMovementInputNativeSource> Source = MovementInputNativeSource;
	if (!Source.IsValid() || Source->ExecutionRequestSerial == 0
		|| Source->ExecutionRequestSerial != LocomotionRequestSerial) return false;
	if (Checkpoint.bConsumerInvalidated || Checkpoint.SourceUnresolvedEventSerial > Checkpoint.RequestStartedEventSerial)
	{
		RevokeMovementInputRequest();
		return false;
	}
	if (Checkpoint.RequestReleasedEventSerial != 0)
	{
		bMovementInputRequestOpen = false;
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted)
			LocomotionRequestAdmission = ELocomotionRequestAdmission::Released;
	}
	if (LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
		&& LocomotionRequestAdmission != ELocomotionRequestAdmission::Released) return false;
	if (!HasAcceptedMovementSet())
	{
		FailLocomotionRequest(LocomotionRequestSerial, TEXT("authenticated native source request has no accepted MovementSet"));
		return false;
	}
	return bOriginalReady;
}

void UGGYGOCharacterMovementComponent::MoveAutonomous(
	float ClientTimeStamp, float DeltaTime, uint8 CompressedFlags, const FVector& NewAccel)
{
	FMovementOwnerSyncNativeMove* Frame = ActiveMovementOwnerSyncNativeMove;
	if (Frame && Frame->Move && Frame->Move == GetCurrentNetworkMoveData()
		&& Frame->OriginalTimeStamp == ClientTimeStamp && Frame->OriginalFlags == CompressedFlags
		&& Frame->Move->MovementOwnerSyncNonce == Frame->OriginalNonce
		&& Frame->Move->MovementOwnerGeneration == Frame->OriginalClientGeneration
		&& Frame->Move->MovementInputSourceCheckpoint == Frame->OriginalSourceCheckpoint
		&& FMath::IsFinite(DeltaTime) && DeltaTime > 0.0f && FMath::IsFinite(ClientTimeStamp)
		&& Frame->bHasOriginalSync && Frame->OriginalNonce != 0
		&& HasValidData() && IsActive() && CharacterOwner && CharacterOwner->HasAuthority()
		&& !CharacterOwner->IsLocallyControlled())
	{
		FMovementOwnerSyncNativeReceipt Receipt;
		Receipt.Pawn = CharacterOwner;
		Receipt.Controller = Cast<APlayerController>(CharacterOwner->GetController());
		Receipt.Connection = Receipt.Controller.IsValid() ? Receipt.Controller->GetNetConnection() : nullptr;
		Receipt.OwnerGeneration = ServerMovementOwnerGeneration;
		Receipt.ClientOwnerGeneration = Frame->OriginalClientGeneration;
		Receipt.Nonce = Frame->OriginalNonce;
		Receipt.TimeStamp = ClientTimeStamp;
		Receipt.bInitialOwnerGeneration = bServerMovementInitialOwnerGeneration;
		// Native Super has already accepted timestamp, positive delta, PC readiness and pause guards.
		if (IsMovementOwnerSyncReceiptCurrent(Receipt))
		{
			Frame->Receipt = Receipt;
			Frame->bEnteredNativeSimulation = true;
			Frame->bSourceExecutionApplicable = ConsumeNativeMovementInputCheckpoint(*Frame);
			if (LocomotionRequestSerial == 0 && !Frame->OriginalSourceCheckpoint.bPresent
				&& Receipt.ClientOwnerGeneration == 0 && Receipt.bInitialOwnerGeneration)
				bMovementInputAdmissionDiagnosticReported = true; // Ordinary initial native wait, not a missing-source Error.
		}
	}
	const bool bOriginalReplay = CharacterOwner && CharacterOwner->bClientUpdating && MovementInputReplayCapture.IsSet();
	if (Frame && Frame->bEnteredNativeSimulation && !bOriginalReplay)
	{
		const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
		const uint64 OriginalExecution = LocomotionRequestSerial;
		const auto OriginalSource = MovementInputNativeSource;
		PublishQualifiedMovementIntent(); // Only after the original native applicability bit was committed.
		UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
		if (!Self || Self->ActiveMovementOwnerSyncNativeMove != Frame
			|| Self->LocomotionRequestSerial != OriginalExecution || Self->MovementInputNativeSource != OriginalSource
			|| Self->bMovementOwnerSyncClosed) return;
	}
	if ((Frame || bOriginalReplay) && IsMovementInputRequestBlocked() && IsMovingOnGround()
		&& !HasIndependentGroundRootMotion() && !CharacterOwner->IsPlayingNetworkedRootMotionMontage()
		&& (LocomotionRequestSerial != 0 || bOriginalReplay))
	{
		// An inapplicable original request cannot simulate over a current successor. A trusted
		// native interval may still brake its current physical baseline when no motor is live.
		const bool bNoLiveLocomotion = LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
			&& LocomotionRequestAdmission != ELocomotionRequestAdmission::Released
			&& !LocomotionCurveOrigin.IsValid() && !PendingLocomotionCurveInput.IsValid();
		bool bCurrentNativeInterval = Frame && Frame->bEnteredNativeSimulation
			&& IsMovementOwnerSyncReceiptCurrent(Frame->Receipt);
		if (bOriginalReplay)
		{
			const FMovementInputReplayCapture& Capture = MovementInputReplayCapture.GetValue();
			const bool bSameOwnerScope = Capture.OwnerScope.GetScopeSerial() == 0
				? CharacterOwner->HasAuthority()
				: IsMovementOwnerSyncContextCurrent(MovementOwnerSyncContext)
					&& Capture.OwnerScope == MovementOwnerSyncContext->Scope;
			bCurrentNativeInterval = bSameOwnerScope && Capture.Request.Binding == MovementInputBinding
				&& Capture.Request.Request == MovementInputRequest
				&& Capture.Request.ExecutionRequestSerial == LocomotionRequestSerial;
		}
		// Keep the old rejection of new jump/crouch/custom commands. Pure braking can use
		// native simulation only when the supplied flags already match the current stance.
		const uint8 CurrentPhysicsFlags = static_cast<uint8>((bForceWalkRequested ? GGYGOMovementConstants::ForceWalkFlag : 0)
			| (bWantsToCrouch ? FSavedMove_Character::FLAG_WantsToCrouch : 0));
		if (!bNoLiveLocomotion || !bCurrentNativeInterval || !HasNativePassiveGroundVelocity()
			|| CharacterOwner->bPressedJump || CompressedFlags != CurrentPhysicsFlags) return;
	}
	const bool bOriginalReleased = (Frame && Frame->bSourceExecutionApplicable
		&& Frame->OriginalSourceCheckpoint.RequestReleasedEventSerial != 0)
		|| (bOriginalReplay && MovementInputReplayCapture->Request.Admission == static_cast<uint8>(ELocomotionRequestAdmission::Released));
	Super::MoveAutonomous(ClientTimeStamp, DeltaTime, CompressedFlags, bOriginalReleased ? FVector::ZeroVector : NewAccel);
}

void UGGYGOCharacterMovementComponent::ClientHandleMoveResponse(const FCharacterMoveResponseDataContainer& MoveResponse)
{
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	RefreshMovementOwnerSyncContext();
	if (!WeakSelf.IsValid() || WeakSelf->bMovementOwnerSyncClosed) return;
	const FCharacterMoveResponseDataContainer_GGYGO& OwnerResponse =
		static_cast<const FCharacterMoveResponseDataContainer_GGYGO&>(MoveResponse);
	const uint64 ResponseGeneration = OwnerResponse.ServerOwnerGeneration;
	const uint64 ResponseNonce = OwnerResponse.MovementOwnerSyncNonce;
	const bool bResponseInitial = OwnerResponse.bInitialSynchronizationEligible;
	const TSharedPtr<FMovementOwnerSyncContext> Context = MovementOwnerSyncContext;
	const bool bNativeClient = CharacterOwner && !CharacterOwner->HasAuthority()
		&& CharacterOwner->IsLocallyControlled() && CharacterOwner->GetLocalRole() == ROLE_AutonomousProxy;
	if (bNativeClient)
	{
		if (!IsMovementOwnerSyncContextCurrent(Context) || !Context->bRequiresNativeResponse
			|| !Context->bConnectionCaptured || !Context->bOwnerPairCaptured
			|| Context->Scope.OriginalPlayerController->AcknowledgedPawn != CharacterOwner
			|| !OwnerResponse.bHasMovementOwnerSync
			|| OwnerResponse.MovementOwnerSyncNonce != Context->Scope.ResponseNonce
			|| OwnerResponse.ServerOwnerGeneration == 0)
		{
			ReportMovementOwnerSyncOnce(FName(TEXT("StaleNativeResponse")),
				TEXT("Native response has no matching original owner scope/PC/nonce; discarded."));
			return;
		}
		const uint64 Known = Context->Notice.ServerOwnerGeneration != 0
			? Context->Notice.ServerOwnerGeneration : Context->ExpectedServerGeneration;
		if (Known != 0 && OwnerResponse.ServerOwnerGeneration < Known)
		{
			ReportMovementOwnerSyncOnce(FName(TEXT("OldServerGeneration")), TEXT("Delayed native owner generation discarded."));
			return;
		}
		const bool bTurnover = (Known != 0 && OwnerResponse.ServerOwnerGeneration != Known)
			|| (Known == 0 && Context->bInitialLocalScope && !OwnerResponse.bInitialSynchronizationEligible);
		if (bTurnover || !OwnerResponse.bMovementOwnerActive)
		{
			const uint64 NextGeneration = OwnerResponse.ServerOwnerGeneration;
			RetireMovementOwnerSyncScope(FName(TEXT("ServerOwnerGenerationChanged")));
			UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
			if (!Self || Self->bMovementOwnerSyncClosed || Self->MovementOwnerSyncContext.IsValid()) return;
			// New scope and new nonce; the old request/notice is never relabelled as first sync.
			APawn* Pawn = Context->Scope.OriginalPawn.Get();
			APlayerController* PC = Context->Scope.OriginalPlayerController.Get();
			if (IsValid(Pawn) && IsValid(PC) && Pawn == Self->CharacterOwner
				&& Pawn->GetController() == PC && PC->GetPawn() == Pawn
				&& Pawn->GetNetDriver() == Context->NetDriver.Get()
				&& GGYGOMovementOwnerSync::GetConnection(Pawn, PC) == Context->Connection.Get()
				&& GGYGOMovementOwnerSync::IsConnectionLive(Context->Connection.Get()))
				Self->OpenMovementOwnerSyncScope(Pawn, PC, Context->Connection.Get(),
					Context->NetDriver.Get(), false, NextGeneration);
			return; // Never apply the old-generation native body to the successor.
		}
		if (OwnerResponse.AdjustmentOwnerGeneration != OwnerResponse.ServerOwnerGeneration)
		{
			ReportMovementOwnerSyncOnce(FName(TEXT("OldAdjustmentGeneration")), TEXT("Native body belongs to an original earlier generation; discarded."));
			return;
		}
	}
	if (MoveResponse.IsCorrection() && !IsMovementInputRequestBlocked())
	{
		const FCharacterMoveResponseDataContainer_GGYGO& GGYGOResponse =
			static_cast<const FCharacterMoveResponseDataContainer_GGYGO&>(MoveResponse);
		LocomotionMotionType = GGYGOResponse.LocomotionMotionType;
		StopMotionType = GGYGOResponse.StopMotionType;
		TurnBackPhase = GGYGOResponse.TurnBackPhase;
		ResolvedGait = GGYGOResponse.Gait;
		LocomotionMotionTime = FMath::Max(GGYGOResponse.LocomotionMotionTime, 0.0f);
		WalkRunCyclePhase = FMath::Clamp(GGYGOResponse.WalkRunCyclePhase, 0.0f, 1.0f);
		WalkRunBlendAlpha = FMath::Clamp(GGYGOResponse.WalkRunBlendAlpha, 0.0f, 1.0f);
		WalkHoldTimer = FMath::Max(GGYGOResponse.WalkHoldTimer, 0.0f);
		TurnBackElapsed = FMath::Max(GGYGOResponse.TurnBackElapsed, 0.0f);
		TurnBackEntryYaw = FMath::IsFinite(GGYGOResponse.TurnBackEntryYaw) ? GGYGOResponse.TurnBackEntryYaw : 0.0f;
		LocomotionMotionSequence = GGYGOResponse.LocomotionMotionSequence;
		bForceWalkRequested = GGYGOResponse.bForceWalkRequested;
		bPreviousHasMoveInput = GGYGOResponse.bPreviousHasMoveInput;
		bPreviousMovementBlocked = GGYGOResponse.bPreviousMovementBlocked;
		bWantsRunOnNextMove = GGYGOResponse.bWantsRunOnNextMove;
		bTurnBackInputLatched = GGYGOResponse.bTurnBackInputLatched;
		CurveMotion.Reset();
		bHasPendingAuthoritativeLocomotionState = true;
	}

	const TWeakObjectPtr<ACharacter> CorrectionCharacter(CharacterOwner);
	const TWeakObjectPtr<USceneComponent> CorrectionComponent(UpdatedComponent.Get());
	Super::ClientHandleMoveResponse(MoveResponse);
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (bNativeClient && MoveResponse.IsCorrection() && Self && Self->IsMovementOwnerSyncContextCurrent(Context)
		&& CorrectionCharacter.IsValid() && Self->CharacterOwner == CorrectionCharacter.Get()
		&& CorrectionComponent.IsValid() && Self->UpdatedComponent == CorrectionComponent.Get())
	{
		const FNetworkPredictionData_Client_Character* ClientData = Self->GetPredictionData_Client_Character();
		if (ClientData && ClientData->bUpdatePosition && ClientData->LastAckedMove.IsValid()
			&& ClientData->LastAckedMove->TimeStamp == MoveResponse.ClientAdjustment.TimeStamp
			&& Self->Velocity.Equals(Self->GetLastUpdateVelocity(), KINDA_SMALL_NUMBER))
		{
			// Observe the actually accepted native correction, never its proposed payload or ACK.
			// Replays then continue this authoritative physical baseline, not a future local result.
			Self->StoreNativeMovementVelocityResult();
			Self->NativeVelocityBeforeRootMotion.Reset();
			Self->bNativeVelocityIntervalOpen = false;
			Self->bNativeVelocityIntervalCanRetain = false;
		}
	}
	if (bNativeClient && Self && Self->IsMovementOwnerSyncContextCurrent(Context)
		&& Context->Notice.State == EGGYGOMovementOwnerSyncState::Waiting)
	{
		// Native correction/ACK completes before external Ready can create a later request.
		Self->PublishMovementOwnerSyncNotice(Context, EGGYGOMovementOwnerSyncState::Ready,
			ResponseGeneration, ResponseNonce, Context->bInitialLocalScope && bResponseInitial,
			FName(TEXT("NativeOwnerResponseReady")));
	}
}

bool UGGYGOCharacterMovementComponent::ClientUpdatePositionAfterServerUpdate()
{
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(this);
	MovementInputReplayCapture.Reset();
	LocomotionCurveReplayEntryState = CaptureLocomotionCandidate();
	LocomotionCurveReplayEntryRequestSerial = LocomotionRequestSerial;
	bReplayLocomotionFromAuthority = bHasPendingAuthoritativeLocomotionState;
	const bool bUpdated = Super::ClientUpdatePositionAfterServerUpdate();
	if (!WeakSelf.IsValid()) return bUpdated;
	const bool bLastOriginalMoveInapplicable = MovementInputReplayCapture.IsSet()
		&& MovementInputReplayCapture->Request.Binding.ConsumerBindingSerial != 0
		&& (!MovementInputReplayCapture->Request.bExecutionEligible
			|| MovementInputReplayCapture->Request.ExecutionRequestSerial != LocomotionRequestSerial
			|| MovementInputReplayCapture->Request.Binding != MovementInputBinding
			|| MovementInputReplayCapture->Request.Request != MovementInputRequest
			|| MovementInputReplayCapture->OwnerScope != MovementInputRequestOwnerScope);
	if (LocomotionCurveReplayEntryState.IsSet()
		&& LocomotionCurveReplayEntryRequestSerial == LocomotionRequestSerial
		&& (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed || bLastOriginalMoveInapplicable
			|| (ReplayLocomotionCurveInput.IsValid() && ReplayLocomotionCurveInput->Origin.IsValid()
				&& ReplayLocomotionCurveInput->Origin->ExecutionRequestSerial != LocomotionRequestSerial)))
	{
		CommitLocomotionCandidate(LocomotionCurveReplayEntryState.GetValue());
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed) CurveMotion.Reset();
	}
	LocomotionCurveReplayEntryState.Reset();
	LocomotionCurveReplayEntryRequestSerial = 0;
	EndLocomotionCurveReplay();
	MovementInputReplayCapture.Reset();
	bReplayLocomotionFromAuthority = false;
	bHasPendingAuthoritativeLocomotionState = false;
	return bUpdated;
}

FNetworkPredictionData_Client* UGGYGOCharacterMovementComponent::GetPredictionData_Client() const
{
	if (!ClientPredictionData)
	{
		// const_cast 是 CMC 这个扩展点的既定写法：基类把该函数声明为 const，
		// 但延迟创建预测数据必须写成员。引擎自身的实现也这么做。
		UGGYGOCharacterMovementComponent* MutableThis = const_cast<UGGYGOCharacterMovementComponent*>(this);
		MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_GGYGO(*this);
	}

	return ClientPredictionData;
}

// ===== 供动画层读取的派生量 =====

float UGGYGOCharacterMovementComponent::GetHorizontalSpeed() const
{
	return Velocity.Size2D();
}

bool UGGYGOCharacterMovementComponent::IsMovingHorizontally() const
{
	return GetHorizontalSpeed() > GGYGOMovementConstants::MovingSpeedThreshold;
}

FVector UGGYGOCharacterMovementComponent::GetHorizontalVelocityDirection() const
{
	return FVector(Velocity.X, Velocity.Y, 0.0f).GetSafeNormal2D();
}

float UGGYGOCharacterMovementComponent::GetLocalVelocityAngle() const
{
	if (!CharacterOwner)
	{
		return 0.0f;
	}

	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
	const FVector LocalVelocity = CharacterOwner->GetActorTransform().InverseTransformVector(HorizontalVelocity);

	if (LocalVelocity.Size2D() <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	// 局部空间 X 为前、Y 为右，所以 Atan2(Y, X) 得到的就是"偏离正前多少度，右为正"。
	return FMath::RadiansToDegrees(FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
}

void UGGYGOCharacterMovementComponent::GetLocalVelocityAxes(float& OutForward, float& OutRight) const
{
	OutForward = 0.0f;
	OutRight = 0.0f;

	if (!CharacterOwner)
	{
		return;
	}

	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.0f);
	const FVector LocalVelocity = CharacterOwner->GetActorTransform().InverseTransformVector(HorizontalVelocity);
	const float LocalLength = LocalVelocity.Size2D();

	if (LocalLength <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	OutForward = LocalVelocity.X / LocalLength;
	OutRight = LocalVelocity.Y / LocalLength;
}

void UGGYGOCharacterMovementComponent::GetLocalVelocityBlend(float& OutBlendX, float& OutBlendY) const
{
	GetLocalVelocityAxes(OutBlendX, OutBlendY);
}

// Movement-LocalASC-A implementation begin.
// This record owns one native subscription, not an ASC binding or readiness state.
struct UGGYGOCharacterMovementComponent::FLocalAbilitySystemSubscription
{
	explicit FLocalAbilitySystemSubscription(UGGYGOPawnExtensionComponent* InExtension)
		: Extension(InExtension)
	{
	}

	~FLocalAbilitySystemSubscription() { Retire(); }
	FLocalAbilitySystemSubscription(const FLocalAbilitySystemSubscription&) = delete;
	FLocalAbilitySystemSubscription& operator=(const FLocalAbilitySystemSubscription&) = delete;

	void AcceptReturnedHandle(FDelegateHandle ReturnedHandle)
	{
		check(IsInGameThread());
		check(!NoticeHandle.IsValid());
		if (bRetired)
		{
			// An immediate replay can retire this record before RegisterAndCall returns.
			if (ReturnedHandle.IsValid())
			{
				if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get())
				{
					OriginalExtension->UnregisterLocalAbilitySystemNotice(ReturnedHandle);
				}
			}
			return;
		}
		NoticeHandle = ReturnedHandle;
	}

	void Retire()
	{
		// Seal local ownership before the external removal, including destructor cleanup.
		bRetired = true;
		Resource = FGGYGOPawnASCResourceHandle{};
		const FDelegateHandle OriginalHandle = NoticeHandle;
		NoticeHandle.Reset();
		if (OriginalHandle.IsValid())
		{
			if (UGGYGOPawnExtensionComponent* OriginalExtension = Extension.Get())
			{
				OriginalExtension->UnregisterLocalAbilitySystemNotice(OriginalHandle);
			}
		}
	}

	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
	FDelegateHandle NoticeHandle{};
	FGGYGOPawnASCResourceHandle Resource{};
	bool bRetired = false;
};

bool UGGYGOCharacterMovementComponent::PrepareLocalAbilitySystemSubscription(
	UGGYGOPawnExtensionComponent* Extension, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	const FString ErrorContext = FString::Printf(
		TEXT("Movement LocalASC subscription: Consumer='%s', Owner='%s', Extension='%s'"),
		*GetPathNameSafe(this), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(Extension));
	const auto Reject = [&OutError, &ErrorContext](const TCHAR* Reason)
	{
		OutError = FString::Printf(TEXT("%s, Reason='%s'."), *ErrorContext, Reason);
		return false;
	};
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalConsumer(this);
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(Extension);
	const TWeakObjectPtr<APawn> OriginalPawn(Cast<APawn>(GetOwner()));
	const auto IsOriginalTargetLive = [&]()
	{
		const UGGYGOCharacterMovementComponent* Consumer = OriginalConsumer.Get();
		const UGGYGOPawnExtensionComponent* Source = OriginalExtension.Get();
		const APawn* Pawn = OriginalPawn.Get();
		return Consumer && !Consumer->IsBeingDestroyed()
			&& !Consumer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Source && !Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& Pawn && !Pawn->IsActorBeingDestroyed()
			&& Consumer->GetOwner() == Pawn && Source->GetOwner() == Pawn;
	};
	if (!IsOriginalTargetLive())
	{
		return Reject(TEXT("subscription requires a live CMC and its original Pawn Extension"));
	}
	if (LocalAbilitySystemSubscription.IsValid() && !LocalAbilitySystemSubscription->bRetired
		&& LocalAbilitySystemSubscription->Extension.HasSameIndexAndSerialNumber(OriginalExtension))
	{
		return LocalAbilitySystemSubscription->NoticeHandle.IsValid()
			? true : Reject(TEXT("original registration has not returned its delegate handle"));
	}

	// Retirement can call the original source; never write through the old CMC tail.
	ReleaseLocalAbilitySystemSubscription();
	UGGYGOCharacterMovementComponent* Consumer = OriginalConsumer.Get();
	if (!IsOriginalTargetLive())
	{
		return Reject(TEXT("original consumer or source closed during subscription retirement"));
	}
	if (Consumer->LocalAbilitySystemSubscription.IsValid())
	{
		return Reject(TEXT("subscription was replaced during retirement; successor is retained"));
	}
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription =
		MakeShared<FLocalAbilitySystemSubscription>(OriginalExtension.Get());
	Consumer->LocalAbilitySystemSubscription = OriginalSubscription;
	const TWeakPtr<FLocalAbilitySystemSubscription> WeakSubscription(OriginalSubscription);
	const FDelegateHandle ReturnedHandle = OriginalExtension.Get()->RegisterLocalAbilitySystemNoticeAndCall(
		FGGYGOPawnASCLocalNoticeDelegate::FDelegate::CreateWeakLambda(Consumer,
			[OriginalConsumer, WeakSubscription](const FGGYGOPawnASCLocalNotice& Notice)
			{
				const TSharedPtr<FLocalAbilitySystemSubscription> Subscription = WeakSubscription.Pin();
				if (UGGYGOCharacterMovementComponent* LiveConsumer = OriginalConsumer.Get())
				{
					if (Subscription.IsValid())
					{
						LiveConsumer->ConsumeLocalAbilitySystemNotice(Subscription, Notice);
					}
				}
			}));
	// The stack copy survives an immediate replay replacing/retiring the component's slot.
	OriginalSubscription->AcceptReturnedHandle(ReturnedHandle);
	Consumer = OriginalConsumer.Get();
	if (!ReturnedHandle.IsValid() || !IsOriginalTargetLive()
		|| !Consumer || Consumer->LocalAbilitySystemSubscription != OriginalSubscription
		|| OriginalSubscription->bRetired)
	{
		if (Consumer && Consumer->LocalAbilitySystemSubscription == OriginalSubscription)
		{
			Consumer->ReleaseLocalAbilitySystemSubscription();
		}
		else
		{
			OriginalSubscription->Retire();
		}
		return Reject(ReturnedHandle.IsValid()
			? TEXT("original subscription changed during immediate replay; only its handle was retired")
			: TEXT("Extension did not return a valid delegate handle"));
	}
	// A registration is not Ready; only an authenticated notice can seed the derived cache.
	return true;
}

void UGGYGOCharacterMovementComponent::ReleaseLocalAbilitySystemSubscription()
{
	check(IsInGameThread());
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	if (!OriginalSubscription.IsValid()) return;
	OriginalSubscription->bRetired = true;
	const bool bHadConsumedResource = OriginalSubscription->Resource.HasResource();
	OriginalSubscription->Resource = FGGYGOPawnASCResourceHandle{};
	LocalAbilitySystemSubscription.Reset();
	if (bHadConsumedResource)
	{
		AbilitySystemComponent = nullptr;
		ResetLocomotionState();
	}
	// No component writes after native removal; a callback's successor remains untouched.
	OriginalSubscription->Retire();
}

void UGGYGOCharacterMovementComponent::ConsumeLocalAbilitySystemNotice(
	const TSharedPtr<FLocalAbilitySystemSubscription>& ExpectedSubscription,
	const FGGYGOPawnASCLocalNotice& Notice)
{
	check(IsInGameThread());
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = ExpectedSubscription;
	const FGGYGOPawnASCLocalNotice OwnNotice = Notice;
	if (!OriginalSubscription.IsValid() || OriginalSubscription->bRetired
		|| LocalAbilitySystemSubscription != OriginalSubscription) return;
	const FGGYGOPawnASCResourceHandle OriginalResource = OwnNotice.Resource;
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	const auto Reject = [&](const TCHAR* Reason)
	{
		const FString Diagnostic = FString::Printf(
			TEXT("Movement LocalASC notice rejected: Consumer='%s', Owner='%s', Extension='%s', ASC='%s', Pawn='%s', Binding=%llu, Kind=%u, Reason='%s'."),
			*GetPathNameSafe(this), *GetPathNameSafe(GetOwner()),
			*GetPathNameSafe(OriginalSubscription->Extension.Get()), *GetPathNameSafe(Identity.ASC.Get()),
			*GetPathNameSafe(Identity.Pawn.Get()), static_cast<unsigned long long>(Identity.Binding.Serial),
			static_cast<uint32>(OwnNotice.Kind), Reason);
		UE_LOG(LogGGYGOMovement, Verbose, TEXT("%s"), *Diagnostic);
	};
	if (!OriginalResource.HasResource())
	{
		Reject(TEXT("notice has no original opaque resource"));
		return;
	}
	if (OwnNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Released)
	{
		// Withdrawal makes Ready false and can outlive ASC/Pawn liveness.
		if (!OriginalSubscription->Resource.HasSameResource(OriginalResource))
		{
			Reject(TEXT("Released does not name the consumed original resource; successor is retained"));
			return;
		}
		OriginalSubscription->Resource = FGGYGOPawnASCResourceHandle{};
		AbilitySystemComponent = nullptr;
		ResetLocomotionState();
		return;
	}
	if (OwnNotice.Kind != EGGYGOPawnASCLocalNoticeKind::Ready
		&& OwnNotice.Kind != EGGYGOPawnASCLocalNoticeKind::Refreshed)
	{
		Reject(TEXT("notice kind is not Ready, Released or Refreshed"));
		return;
	}
	UGGYGOPawnExtensionComponent* Source = OriginalSubscription->Extension.Get();
	UGGYGOAbilitySystemComponent* ASC = Identity.ASC.Get();
	APawn* Pawn = Identity.Pawn.Get();
	if (IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Source || Source->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Pawn || Pawn->IsActorBeingDestroyed() || GetOwner() != Pawn || Source->GetOwner() != Pawn
		|| !ASC || !Source->IsLocalAbilitySystemResourceReady(OriginalResource)
		|| !Identity.Binding.HasSameIdentity(OwnNotice.PublishedContext.Binding)
		|| !ASC->IsAvatarBindingPublicationContextCurrent(OwnNotice.PublishedContext))
	{
		Reject(TEXT("original Extension/Pawn/resource or actual publication Context is no longer Ready"));
		return;
	}
	// Pure queries do not dispatch, but ownership must still be this original record.
	if (OriginalSubscription->bRetired || LocalAbilitySystemSubscription != OriginalSubscription) return;
	const bool bSameConsumedResource = OriginalSubscription->Resource.HasSameResource(OriginalResource);
	if (OwnNotice.Kind == EGGYGOPawnASCLocalNoticeKind::Refreshed || bSameConsumedResource)
	{
		if (!bSameConsumedResource || AbilitySystemComponent.Get() != ASC)
		{
			Reject(TEXT("Refresh/duplicate Ready requires the already consumed original resource and ASC"));
		}
		// No cache replacement, clock/sequence/Blend/TurnBack reset or resource cleanup.
		return;
	}
	// A different opaque record is distinct even with the same ASC/Pawn/Binding identity.
	OriginalSubscription->Resource = OriginalResource;
	AbilitySystemComponent = ASC;
	ResetLocomotionState();
}

const UGGYGOAbilitySystemComponent* UGGYGOCharacterMovementComponent::GetReadyLocalAbilitySystemComponent() const
{
	check(IsInGameThread());
	const TSharedPtr<FLocalAbilitySystemSubscription> OriginalSubscription = LocalAbilitySystemSubscription;
	if (!OriginalSubscription.IsValid() || OriginalSubscription->bRetired
		|| IsBeingDestroyed() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return nullptr;
	const FGGYGOPawnASCResourceHandle OriginalResource = OriginalSubscription->Resource;
	UGGYGOPawnExtensionComponent* Source = OriginalSubscription->Extension.Get();
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	UGGYGOAbilitySystemComponent* ASC = Identity.ASC.Get();
	APawn* Pawn = Identity.Pawn.Get();
	if (!OriginalResource.HasResource() || !Source || !Pawn || Pawn->IsActorBeingDestroyed()
		|| GetOwner() != Pawn || Source->GetOwner() != Pawn || !ASC
		|| AbilitySystemComponent.Get() != ASC
		|| !Source->IsLocalAbilitySystemResourceReady(OriginalResource)) return nullptr;
	return LocalAbilitySystemSubscription == OriginalSubscription && !OriginalSubscription->bRetired
		&& OriginalSubscription->Resource.HasSameResource(OriginalResource)
		&& AbilitySystemComponent.Get() == ASC ? ASC : nullptr;
}
// Movement-LocalASC-A implementation end.
