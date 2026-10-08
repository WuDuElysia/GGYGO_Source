/**
 * @file GGYGOMovementPrediction.cpp
 * @brief Native saved-move capture, replay and unchanged Movement wire serialization.
 */
#include "Character/Components/GGYGOMovementPrediction.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOActionMotionExecution.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMovementPrediction, Log, All);

namespace GGYGOMovementOwnerSync
{
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
			UE_LOG(LogGGYGOMovementPrediction, Error, TEXT("Movement SavedMove rejected: CMC='%s', Reason='%s'."),
				*MoveComp->GetPathName(), *Error);
		}
		const auto HasSavedIndependentSource = [](const TArray<TSharedPtr<FRootMotionSource>>& Sources)
		{
			return Sources.ContainsByPredicate([](const TSharedPtr<FRootMotionSource>& Source)
			{
				return Source.IsValid() && FGGYGOActionMotionExecution::IsRegisteredSource(*Source);
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
		Result |= GGYGOMovementPrediction::ForceWalkFlag;
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
		UE_LOG(LogGGYGOMovementPrediction, Error, TEXT("Movement source checkpoint serialization rejected: CMC='%s', Reason='%s'."),
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
