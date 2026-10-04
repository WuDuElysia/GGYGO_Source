/**
 * @file GGYGOCharacterMovementComponent.cpp
 * @brief 项目 CMC 实现
 */
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Character/Data/GGYGOLocomotionEvaluation.h"
#include "Character/Data/GGYGOLocomotionMotionProfile.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Character/Components/GGYGOCurveRootMotionSource.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOMovementSet.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"
#include "UObject/Class.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCharacterMovementComponent)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMovement, Log, All);

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

	/** ForceWalk 是客户端输入请求；步态与动作段由服务端按自身 MovementSet/Profile 重算。 */
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
		return Source.GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct()
			&& Source.InstanceName == GGYGOMovementConstants::ActionCurveSourceName
			&& Source.Priority == GGYGOMovementConstants::ActionCurvePriority
			&& Source.AccumulateMode == ERootMotionAccumulateMode::Override;
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
	SavedMovementInputSourceCheckpoint = {};
	SavedCurveRootMotionInput.Reset();
	SavedCurveRootMotionPrepared.Reset();

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
	SavedMovementInputSourceCheckpoint = {};
	SavedCurveRootMotionInput.Reset();
	SavedCurveRootMotionPrepared.Reset();

	if (const UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr)
	{
		SavedMovementInputSourceCheckpoint = MoveComp->MovementInputSourceCheckpoint;
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
		if (MoveComp->HasCurveRootMotionSource())
		{
			bForceNoCombine = true;
		}
	}
}

void FSavedMove_GGYGO::PostUpdate(ACharacter* C, EPostUpdateMode PostUpdateMode)
{
	Super::PostUpdate(C, PostUpdateMode);
	if (const UGGYGOCharacterMovementComponent* MoveComp = C ? Cast<UGGYGOCharacterMovementComponent>(C->GetCharacterMovement()) : nullptr)
	{
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
		// 回放这一帧之前把状态还原到当时的样子。
		// 不还原计时器的话，回放多帧时计时器会从"现在"的值继续累加，
		// 于是回放中途可能升档，而首次执行时并没有 —— 预测就失配了。
		// 普通预测回放恢复本 move 的起始状态。服务器校正后的重放则必须从
		// response 带回的权威时间连续推进，不能再覆盖成旧的预测时钟。
		if (!MoveComp->bReplayLocomotionFromAuthority)
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
	if (NewGGYGOMove && NewGGYGOMove->SavedMovementInputSourceCheckpoint != SavedMovementInputSourceCheckpoint)
	{
		return false;
	}

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
	MovementInputSourceCheckpoint = GGYGOMove.SavedMovementInputSourceCheckpoint;
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
	if (const UGGYGOCharacterMovementComponent* MoveComp = Cast<UGGYGOCharacterMovementComponent>(&CharacterMovement))
	{
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

void UGGYGOCharacterMovementComponent::BeginPlay()
{
	Super::BeginPlay();
	CaptureComponentDefaults();

	CacheAbilitySystemComponent();
}

void UGGYGOCharacterMovementComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RetireLocomotionCurveRootMotion();
	CompletedLocomotionCurveOrigin.Reset();
	LastLocomotionCurvePrepared.Reset();
	ConsumedLocomotionCurvePrepared.Reset();
	LocomotionCurveReplayEntryState.Reset();
	EndLocomotionCurveReplay();
	ReleaseLocalAbilitySystemSubscription();

	if (bMovementInputBindingActive)
	{
		FString Error;
		InvalidateMovementInputSession(MovementInputBinding, FName(TEXT("ConsumerEndPlay")), Error);
	}
	Super::EndPlay(EndPlayReason);
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
	if (Session == MovementInputBinding.SourceSession)
	{
		if (!bMovementInputBindingActive)
		{
			return Reject(TEXT("this source session has already been retired"));
		}
		OutBinding = MovementInputBinding;
		return true;
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
	bMovementInputAdmissionDiagnosticReported = false;
	MovementInputSourceCheckpoint = {};
	MovementInputSourceCheckpoint.bPresent = true;
	MovementInputSourceCheckpoint.BindingSerial = MovementInputBinding.ConsumerBindingSerial;
	MovementInputSourceCheckpoint.ConsumerFenceSerial = MovementInputBindingSerial;
	MovementInputSourceCheckpoint.SessionSerial = Session.SessionSerial;
	OutBinding = MovementInputBinding;
	return true;
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
		RevokeMovementInputRequest();
		LastMovementInputRequestSerial = Fact.Request.RequestSerial;
		MovementInputRequest = Fact.Request;
		bMovementInputNeutralConsumed = false;
		bMovementInputRequestOpen = true;
		++LocomotionRequestSerial;
		LocomotionRequestAdmission = ELocomotionRequestAdmission::Admitted;
		LocomotionRequestFailureReason.Reset();
		bMovementInputAdmissionDiagnosticReported = false;
		const bool bKeepRunIntent = bWantsRunOnNextMove;
		const bool bKeepForceWalk = bForceWalkRequested;
		ResetLocomotionState();
		bWantsRunOnNextMove = bKeepRunIntent;
		bForceWalkRequested = bKeepForceWalk;
		LastMovementInputFact = Fact;
		RecordMovementInputSourceCheckpoint(Fact);
		if (!HasAcceptedMovementSet())
		{
			FailLocomotionRequest(LocomotionRequestSerial, TEXT("new source request has no accepted MovementSet"));
			OutError = LocomotionRequestFailureReason;
		}
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
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted)
		{
			LocomotionRequestAdmission = ELocomotionRequestAdmission::Released;
		}
		break;
	case EGGYGOMovementInputFactKind::SessionInvalidated:
		LastMovementInputFact = Fact;
		if (!InvalidateMovementInputSession(Binding, Fact.Reason, OutError))
		{
			return EGGYGOMovementInputConsumeResult::Rejected;
		}
		RecordMovementInputSourceCheckpoint(Fact);
		return EGGYGOMovementInputConsumeResult::Recorded;
	case EGGYGOMovementInputFactKind::SourceUnresolved:
		if (Fact.Request.RequestSerial != 0 && Fact.Request != MovementInputRequest)
		{
			return Report(EGGYGOMovementInputConsumeResult::Stale, TEXT("unresolved fact names an older request source"));
		}
		bMovementInputNeutralConsumed = false;
		RevokeMovementInputRequest();
		LastMovementInputFact = Fact;
		RecordMovementInputSourceCheckpoint(Fact);
		return Report(EGGYGOMovementInputConsumeResult::Recorded, *Fact.Reason.ToString());
	default:
		return Report(EGGYGOMovementInputConsumeResult::Rejected, TEXT("unsupported fact kind"));
	}
	LastMovementInputFact = Fact;
	RecordMovementInputSourceCheckpoint(Fact);
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
	if (IsMovingOnGround() && !HasIndependentGroundRootMotion())
	{
		Velocity = Velocity.ContainsNaN() ? FVector::ZeroVector : Velocity - ProjectToGravityFloor(Velocity);
	}
}

void UGGYGOCharacterMovementComponent::RevokeMovementInputRequest()
{
	bMovementInputColdStartWindowOpen = false;
	if (LocomotionRequestAdmission != ELocomotionRequestAdmission::Failed)
	{
		LocomotionRequestAdmission = ELocomotionRequestAdmission::Revoked;
	}
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
	// Lifetime serial 0 means this legacy call chain has not yet been migrated to the source interface.
	return MovementInputBindingSerial != 0
		&& (!IsMovementInputBindingCurrent(MovementInputBinding) || !bMovementInputSessionOpened
			|| !MovementInputBinding.SourceSession.Producer.IsValid()
			|| (LocomotionRequestAdmission != ELocomotionRequestAdmission::Admitted
				&& LocomotionRequestAdmission != ELocomotionRequestAdmission::Released));
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
		EnforceGroundLocomotionAdmission();
	}
	return false;
}

void UGGYGOCharacterMovementComponent::EnforceMovementInputLocomotionAdmission()
{
	if (!ShouldRejectMovementInputGroundLocomotion()) return;
	const bool bHasRequest = ProjectToGravityFloor(Acceleration).SizeSquared() > KINDA_SMALL_NUMBER
		|| (bHasRequestedVelocity && ProjectToGravityFloor(RequestedVelocity).SizeSquared() > KINDA_SMALL_NUMBER)
		|| ProjectToGravityFloor(Velocity).SizeSquared() > KINDA_SMALL_NUMBER;
	if (bHasRequest && !bMovementInputAdmissionDiagnosticReported)
	{
		bMovementInputAdmissionDiagnosticReported = true;
		UE_LOG(LogGGYGOMovement, Error,
			TEXT("Movement source admission rejected: Consumer='%s', Producer='%s', Binding=%llu, Session=%llu, ExecutionRequest=%llu, MovementSet='%s', SourceReason='%s', Reason='no live admitted source request; reset, zero acceleration and neutral cannot rearm it'."),
			*GetPathName(), *GetPathNameSafe(MovementInputBinding.SourceSession.Producer.Get()),
			static_cast<unsigned long long>(MovementInputBindingSerial),
			static_cast<unsigned long long>(MovementInputBinding.SourceSession.SessionSerial),
			static_cast<unsigned long long>(LocomotionRequestSerial), *GetPathNameSafe(MovementSet.Get()),
			*LastMovementInputFact.Reason.ToString());
	}
	Velocity = Velocity.ContainsNaN() ? FVector::ZeroVector : Velocity - ProjectToGravityFloor(Velocity);
}

int32 UGGYGOCharacterMovementComponent::BeginActionMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate)
{
	CleanupFinishedActionMotion();
	FString Error;
	USkeletalMeshComponent* Mesh = CharacterOwner ? CharacterOwner->GetMesh() : nullptr;
	if (!CharacterOwner || !CharacterOwner->HasAuthority() || !Mesh || !IsMovingOnGround()
		|| HasActiveActionMotion() || CharacterOwner->IsPlayingRootMotion()
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
	ActionMotionSourceID = ApplyRootMotionSource(Source);
	if (ActionMotionSourceID == static_cast<uint16>(ERootMotionSourceID::Invalid)) return INDEX_NONE;

	ActiveActionMotionHandle = NextActionMotionHandle;
	NextActionMotionHandle = NextActionMotionHandle == MAX_int32 ? 1 : NextActionMotionHandle + 1;
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
	const TSharedPtr<FRootMotionSource> Source = GetRootMotionSourceByID(ActionMotionSourceID);
	if (Source.IsValid() && Source->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
	{
		RemoveRootMotionSourceByID(ActionMotionSourceID);
	}
	ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
	ActiveActionMotionHandle = INDEX_NONE;
	Velocity.X = Velocity.Y = 0.;
	CurveMotion.Reset();
	WalkHoldTimer = 0.f;
	bPreviousHasMoveInput = false;
}

void UGGYGOCharacterMovementComponent::CleanupFinishedActionMotion()
{
	if (ActiveActionMotionHandle != INDEX_NONE && (!HasActiveActionMotion() || !IsMovingOnGround()))
	{
		EndActionMotion(ActiveActionMotionHandle);
	}
}

void UGGYGOCharacterMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	CleanupFinishedActionMotion();
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
	bGroundAdmissionDiagnosticReported = true;
	UE_LOG(LogGGYGOMovement, Error,
		TEXT("Movement SetMovementSet rejected: Component='%s', Owner='%s', MovementSet='%s', Reason='%s'."),
		*GetPathName(), *GetPathNameSafe(GetOwner()), *GetPathNameSafe(InMovementSet), *Error);
	return false;
}

void UGGYGOCharacterMovementComponent::ResetLocomotionState()
{
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
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
	};
	const bool bHasPreparedOverride = CurrentRootMotion.HasOverrideVelocity();
	const auto IsCurrentActionCurve = [bHasPreparedOverride, &IsLiveActionCurve](const TSharedPtr<FRootMotionSource>& Source)
	{
		return IsLiveActionCurve(Source)
			|| (Source.IsValid() && IsRegisteredActionCurveSource(*Source)
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

void UGGYGOCharacterMovementComponent::EnforceGroundLocomotionAdmission()
{
	EnforceMovementInputLocomotionAdmission();
	if (ShouldRejectUnownedCurveGroundLocomotion())
	{
		const auto HasOwnedSource = [](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
		{
			return Sources.ContainsByPredicate([](const TSharedPtr<FRootMotionSource>& Source)
			{
				return Source.IsValid() && IsOwnedLocomotionCurveSource(*Source);
			});
		};
		const bool bHasRequest = ProjectToGravityFloor(Acceleration).SizeSquared() > KINDA_SMALL_NUMBER
			|| (bHasRequestedVelocity && ProjectToGravityFloor(RequestedVelocity).SizeSquared() > KINDA_SMALL_NUMBER)
			|| ProjectToGravityFloor(Velocity).SizeSquared() > KINDA_SMALL_NUMBER
			|| LocomotionMotionType != EGGYGOLocomotionMotionType::None
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
		|| ProjectToGravityFloor(Acceleration).SizeSquared() > KINDA_SMALL_NUMBER
		|| (bHasRequestedVelocity && ProjectToGravityFloor(RequestedVelocity).SizeSquared() > KINDA_SMALL_NUMBER)
		|| ProjectToGravityFloor(Velocity).SizeSquared() > KINDA_SMALL_NUMBER;
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
	Velocity -= ProjectToGravityFloor(Velocity);
}

void UGGYGOCharacterMovementComponent::CalcVelocity(float DeltaTime, float Friction, bool bFluid, float BrakingDeceleration)
{
	if (ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		EnforceGroundLocomotionAdmission();
		return;
	}
	Super::CalcVelocity(DeltaTime, Friction, bFluid, BrakingDeceleration);
}

void UGGYGOCharacterMovementComponent::ApplyRootMotionToVelocity(float DeltaTime)
{
	if (ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		// Reject at entry, including a removed Brake/TurnBack source's final application.
		// Native RMS application must not switch to Falling before this frame is rejected.
		EnforceGroundLocomotionAdmission();
		return;
	}
	Super::ApplyRootMotionToVelocity(DeltaTime);
	EnforceGroundLocomotionAdmission();
}

bool UGGYGOCharacterMovementComponent::IsMovementBlockedByTag() const
{
	// 仅查询原本地资源仍 Ready 的 ASC；Tag 规则与无 ASC 的合法暂态保持。
	const UGGYGOAbilitySystemComponent* ASC = GetReadyLocalAbilitySystemComponent();
	return ASC && ASC->HasMatchingGameplayTag(GGYGOGameplayTags::Restriction_CantMove);
}

bool UGGYGOCharacterMovementComponent::HasMoveInput() const
{
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
	if (bMovementInputBindingActive && !MovementInputBinding.SourceSession.Producer.IsValid())
	{
		FString Error;
		InvalidateMovementInputSession(MovementInputBinding, FName(TEXT("ProducerDestroyed")), Error);
	}
	CleanupFinishedActionMotion();
	if (HasActiveActionMotion() || HasIndependentGroundRootMotion())
	{
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
			EnforceGroundLocomotionAdmission();
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
	return Origin.IsValid() && Origin == LocomotionCurveOrigin && Origin->Owner.Get() == this
		&& Origin->ExecutionRequestSerial != 0 && Origin->ExecutionRequestSerial == LocomotionRequestSerial
		&& Origin->Binding == MovementInputBinding && Origin->InputRequest == MovementInputRequest
		&& IsMovementInputBindingCurrent(Origin->Binding)
		&& (LocomotionRequestAdmission == ELocomotionRequestAdmission::Admitted
			|| LocomotionRequestAdmission == ELocomotionRequestAdmission::Released);
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
	const auto SameSegment = [this, &Candidate](const TSharedPtr<const FGGYGOCurveRootMotionOrigin>& Origin)
	{
		return Origin.IsValid() && Origin->Owner.Get() == this
			&& Origin->ExecutionRequestSerial == LocomotionRequestSerial
			&& Origin->Binding == MovementInputBinding && Origin->InputRequest == MovementInputRequest
			&& Origin->MovementSet.Get() == MovementSet.Get() && Origin->MotionType == Candidate.MotionType
			&& Origin->MotionSequence == Candidate.MotionSequence;
	};
	if (SameSegment(CompletedLocomotionCurveOrigin))
	{
		// Native completion closes this resource. The remaining clip semantics use the ordinary
		// CMC path, and cannot remount the same physical execution even if a later speed is positive.
		FLocomotionUpdateCandidate Remaining = Candidate;
		if (!EvaluateLocomotionProfile(MovementTickTime, Remaining, OutError)) return false;
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
	CommitLocomotionCandidate(Prepared->State->Candidate);
	ConsumedLocomotionCurvePrepared = Prepared;
	LastLocomotionCurvePrepared = Prepared;
	const TSharedPtr<const FGGYGOCurveRootMotionOrigin> Origin = Prepared->Input->Origin;
	const bool bOriginalCurrentResource = Origin == LocomotionCurveOrigin || Origin == CompletedLocomotionCurveOrigin;
	if (bOriginalCurrentResource && Origin.IsValid() && Origin->ExecutionRequestSerial == LocomotionRequestSerial
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
	LastLocomotionCurvePrepared.Reset();
	ConsumedLocomotionCurvePrepared.Reset();
	PendingLocomotionCurveInput.Reset();
	ReplayLocomotionCurveInput = Move.SavedCurveRootMotionInput;
	ReplayLocomotionCurvePrepared = Move.SavedCurveRootMotionPrepared;
	PreparingLocomotionCurveReplayGroup = &Move.SavedRootMotion;
	const auto Reject = [this, &OutError](const TCHAR* Reason)
	{
		bLocomotionCurveReplayRejected = true;
		ReplayLocomotionCurvePrepared.Reset();
		OutError = Reason;
		return false;
	};
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
		if (ReplayLocomotionCurvePrepared.IsValid()
			&& (ReplayLocomotionCurvePrepared->Input != ReplayLocomotionCurveInput
				|| !ReplayLocomotionCurvePrepared->State.IsValid()
				|| (ReplayLocomotionCurvePrepared->Result != EGGYGOCurveRootMotionPrepareResult::Prepared
					&& ReplayLocomotionCurvePrepared->Result != EGGYGOCurveRootMotionPrepareResult::Finished)))
		{
			return Reject(TEXT("SavedMove prepared interval belongs to a different original input."));
		}
	}
	const auto HasOriginalSources = [this](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
	{
		for (const TSharedPtr<FRootMotionSource>& Source : Sources)
		{
			if (!Source.IsValid() || Source->GetScriptStruct() != FRootMotionSource_GGYGOCurve::StaticStruct()
				|| Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval)) continue;
			const FRootMotionSource_GGYGOCurve* Curve = static_cast<const FRootMotionSource_GGYGOCurve*>(Source.Get());
			if (!ReplayLocomotionCurveInput.IsValid() || Curve->Origin != ReplayLocomotionCurveInput->Origin) return false;
		}
		return true;
	};
	if (!HasOriginalSources(Move.SavedRootMotion.RootMotionSources)
		|| !HasOriginalSources(Move.SavedRootMotion.PendingAddRootMotionSources))
	{
		return Reject(TEXT("SavedRootMotion clone has no matching local Origin/input; imported origins remain unsupported."));
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
	Prepared->MotionEndTime = Prepared->MotionStartTime + SimulationTime;
	if (!FMath::IsFinite(Prepared->NativeEndTime) || !FMath::IsFinite(Prepared->MotionStartTime)
		|| Prepared->MotionStartTime < 0.0f || !FMath::IsFinite(Prepared->MotionEndTime))
	{
		return Reject(TEXT("CurveRMS native-to-motion interval mapping is non-finite or negative."));
	}
	FLocomotionUpdateCandidate Candidate = Input->StartCandidate;
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
		const UGGYGOLocomotionMotionProfile* Profile = OriginalSet->GetProfileForMotion(Origin->MotionType);
		if (IsValid(Profile) && Profile->bLoop)
		{
			return Reject(FString::Printf(TEXT("CurveRMS non-loop segment has loop Profile='%s'."), *Profile->GetPathName()));
		}
		FGGYGOLocomotionEvaluationResult Evaluation;
		if (!GGYGOLocomotionEvaluation::EvaluateSingleInterval(Profile,
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
			&& (!Evaluation.Sample.HasUsableSpeed() || Evaluation.ScaledSpeed <= UE_KINDA_SMALL_NUMBER))
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
			Prepared->OverrideVelocity = FRotator(0.0f, Origin->BaseYaw, 0.0f)
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
		&& MovementSet && Candidate.WalkHoldSeconds >= MovementSet->GetSanitizedWalkToRunHoldSeconds())
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
	const UGGYGOLocomotionMotionProfile* Profile = MovementSet->GetProfileForMotion(Candidate.MotionType);
	if (!MovementSet->bUseCurveDrivenSpeed)
	{
		// Profiles are optional in the explicitly selected fixed-speed mode.
		bOutFinished = !Profile || !FMath::IsFinite(Profile->Duration)
			|| Profile->Duration <= UE_SMALL_NUMBER || Candidate.MotionTime >= Profile->Duration;
		return true;
	}
	if (IsValid(Profile) && FMath::IsFinite(Profile->Duration) && Profile->Duration > UE_SMALL_NUMBER
		&& FMath::IsFinite(Candidate.MotionTime) && Candidate.MotionTime >= 0.0f
		&& Candidate.MotionTime < Profile->Duration)
	{
		return true;
	}
	// Do not turn missing/invalid data into a completed clip and skip its error.
	if (IsValid(Profile) && Profile->bLoop)
	{
		OutError = FString::Printf(TEXT("Non-loop motion %d Profile='%s' must use bLoop=false."),
			static_cast<int32>(Candidate.MotionType), *GetPathNameSafe(Profile));
		return false;
	}
	FGGYGOLocomotionEvaluationResult CompletedInterval;
	if (!GGYGOLocomotionEvaluation::EvaluateSingleInterval(Profile, Candidate.MotionTime, Candidate.MotionTime,
		MovementSet->RootMotionScale, CompletedInterval, &OutError))
	{
		return false;
	}
	bOutFinished = Candidate.MotionTime >= Profile->Duration;
	return true;
}

bool UGGYGOCharacterMovementComponent::EvaluateWalkRunProfiles(
	float DeltaSeconds, FLocomotionUpdateCandidate& Candidate, FString& OutError) const
{
	FGGYGOWalkRunEvaluationResult Result;
	if (!GGYGOLocomotionEvaluation::EvaluateWalkRunInterval(
		MovementSet->WalkLoopProfile, MovementSet->RunLoopProfile, Candidate.CyclePhase,
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

bool UGGYGOCharacterMovementComponent::EvaluateLocomotionProfile(
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
		return EvaluateWalkRunProfiles(AcceptedDelta, Candidate, OutError);
	}

	const UGGYGOLocomotionMotionProfile* Profile = MovementSet->GetProfileForMotion(Candidate.MotionType);
	if (IsValid(Profile) && Profile->bLoop)
	{
		OutError = FString::Printf(TEXT("Non-loop motion %d Profile='%s' must use bLoop=false."),
			static_cast<int32>(Candidate.MotionType), *GetPathNameSafe(Profile));
		return false;
	}
	FGGYGOLocomotionEvaluationResult Result;
	const float EndTime = Candidate.MotionTime + AcceptedDelta;
	if (!GGYGOLocomotionEvaluation::EvaluateSingleInterval(Profile, Candidate.MotionTime, EndTime,
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
		EnforceGroundLocomotionAdmission();
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
			&& (MovementSet->bUseCurveDrivenSpeed || MovementSet->WalkStartProfile);
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
	if (!EvaluateLocomotionProfile(DeltaSeconds, Candidate, Error))
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

	// 这些值只用于发现客户端/服务端预测分歧。服务端不会从这里取 Profile、速度或时间。
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
	if (ActiveActionMotionHandle != INDEX_NONE || (IsMovingOnGround() && HasRegisteredActionCurveSource())) return;
	if (ShouldRejectUnconfiguredGroundLocomotion() || ShouldRejectMovementInputGroundLocomotion()
		|| ShouldRejectUnownedCurveGroundLocomotion())
	{
		EnforceGroundLocomotionAdmission();
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

	Super::PhysicsRotation(DeltaTime);
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

	const bool bCurveDrivenEnabled = MovementSet
		&& MovementSet->bUseCurveDrivenSpeed
		&& MovementSet->TurnBackProfile;
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

void UGGYGOCharacterMovementComponent::ClientHandleMoveResponse(const FCharacterMoveResponseDataContainer& MoveResponse)
{
	if (MoveResponse.IsCorrection())
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

	Super::ClientHandleMoveResponse(MoveResponse);
}

bool UGGYGOCharacterMovementComponent::ClientUpdatePositionAfterServerUpdate()
{
	LocomotionCurveReplayEntryState = CaptureLocomotionCandidate();
	LocomotionCurveReplayEntryRequestSerial = LocomotionRequestSerial;
	bReplayLocomotionFromAuthority = bHasPendingAuthoritativeLocomotionState;
	const bool bUpdated = Super::ClientUpdatePositionAfterServerUpdate();
	if (LocomotionCurveReplayEntryState.IsSet()
		&& LocomotionCurveReplayEntryRequestSerial == LocomotionRequestSerial
		&& (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed
			|| (ReplayLocomotionCurveInput.IsValid() && ReplayLocomotionCurveInput->Origin.IsValid()
				&& ReplayLocomotionCurveInput->Origin->ExecutionRequestSerial != LocomotionRequestSerial)))
	{
		CommitLocomotionCandidate(LocomotionCurveReplayEntryState.GetValue());
		if (LocomotionRequestAdmission == ELocomotionRequestAdmission::Failed) CurveMotion.Reset();
	}
	LocomotionCurveReplayEntryState.Reset();
	LocomotionCurveReplayEntryRequestSerial = 0;
	EndLocomotionCurveReplay();
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
