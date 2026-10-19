/**
 * @file GGYGORemoteMotionPose.cpp
 * @brief 原 CMC 完成区间到远端观察端的姿态发布与接受；只读原生样本，不执行位移或 Yaw，不建时钟。
 *
 * 发布：OnMovementUpdated 留下完成区间候选；本 CMC 原生 Tick 返回后，若其后没有新的
 * PerformMovement 入口、发布端身份未变、胶囊仍等于原生写时间戳时的 LastUpdate 变换，
 * 则以原生 ServerLastTransformUpdateTimeStamp 为键发布；每个新键都重新发布当前事实。
 * 接受：原生 SmoothCorrection 实参被证明正是当前原生复制字段的呈现时，记录该呈现的键与变换；
 * 记录只与同键且变换在原生量化容差内一致的呈现配对，从不按键大小推断新旧。
 */
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOLocomotionEvaluation.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/ReplicatedState.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGORemoteMotionPose, Log, All);

namespace
{
	/** FRotator::SerializeCompressedShort 的单分量量化步长（度）。 */
	constexpr float CompressedShortAxisStep = 360.0f / 65536.0f;

	template<typename EnumType>
	bool SerializeEnum(FArchive& Ar, EnumType& Value)
	{
		uint8 Raw = static_cast<uint8>(Value);
		Ar << Raw;
		if (Ar.IsLoading())
		{
			if (!StaticEnum<EnumType>()->IsValidEnumValue(Raw)) return false;
			Value = static_cast<EnumType>(Raw);
		}
		return true;
	}

	const TCHAR* LexRemoteReason(EGGYGORemoteMotionPoseReason Reason)
	{
		switch (Reason)
		{
		case EGGYGORemoteMotionPoseReason::LaterMoveWithoutCompletion: return TEXT("a later native move has no completed interval");
		case EGGYGORemoteMotionPoseReason::TransformChangedAfterCompletion: return TEXT("capsule changed after the completed interval or outside native movement");
		case EGGYGORemoteMotionPoseReason::NoNativeSampleKey: return TEXT("native transform timestamp is not replicated for this instance");
		case EGGYGORemoteMotionPoseReason::SourceIdentityFailed: return TEXT("source identity failed or differs");
		case EGGYGORemoteMotionPoseReason::SerialExhausted: return TEXT("publication/segment/epoch serial exhausted");
		case EGGYGORemoteMotionPoseReason::ResourceRetired: return TEXT("segment resource retired outside native movement");
		case EGGYGORemoteMotionPoseReason::RequestRetired: return TEXT("original locomotion request/segment retired");
		case EGGYGORemoteMotionPoseReason::SourceInvalid: return TEXT("original source binding became invalid");
		case EGGYGORemoteMotionPoseReason::PublisherIdentityChanged: return TEXT("publisher owner/controller/role changed");
		default: return TEXT("none");
		}
	}

	bool IsTurnBackCurvePhase(EGGYGOTurnBackPhase Phase)
	{
		return Phase == EGGYGOTurnBackPhase::Turning || Phase == EGGYGOTurnBackPhase::Braking;
	}

	/** 本端动画源资格类等待；与"待配对记录"类等待（NativeSample/NativeSampleUnverified）分开。 */
	bool IsProviderWaitReason(EGGYGORemoteMotionPoseWaitReason Reason)
	{
		return Reason == EGGYGORemoteMotionPoseWaitReason::ProviderNotPublished
			|| Reason == EGGYGORemoteMotionPoseWaitReason::ProviderRetiring
			|| Reason == EGGYGORemoteMotionPoseWaitReason::ProviderContentPending;
	}
}

bool FGGYGORemoteMotionPoseRecord::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	if (Ar.IsLoading()) *this = FGGYGORemoteMotionPoseRecord();
	uint8 Flags = (bKeyed ? 1 : 0) | (bRelativeTransform ? 2 : 0);
	Ar << PublicationSerial;
	Ar << PublisherEpoch;
	Ar << Flags;
	// 每个字段都必须读写，不能因前一个枚举非法而短路，否则后续位流错位。
	const bool bKindValid = SerializeEnum(Ar, Kind);
	const bool bClosureValid = SerializeEnum(Ar, Closure);
	const bool bReasonValid = SerializeEnum(Ar, Reason);
	if (Ar.IsLoading())
	{
		bKeyed = (Flags & 1) != 0;
		bRelativeTransform = (Flags & 2) != 0;
	}
	bool bValid = bKindValid && bClosureValid && bReasonValid && (Flags & ~3) == 0;
	bool bLocation = true;
	if (bKeyed)
	{
		Ar << NativeSampleKey;
		SampleLocation.NetSerialize(Ar, Map, bLocation);
		SampleRotation.SerializeCompressedShort(Ar);
	}
	if (Kind == EGGYGORemoteMotionPoseKind::Sample || Kind == EGGYGORemoteMotionPoseKind::Terminated)
		Ar << SegmentSerial;
	if (Kind == EGGYGORemoteMotionPoseKind::Sample)
	{
		Ar << ConfigurationDigest;
		Ar << CurveContentKind;
		Ar << CurveContentDigest;
		bValid = bValid && CurveContentKind >= static_cast<uint8>(EGGYGOLocomotionCurveContentKind::CompressedCodecStream)
			&& CurveContentKind <= static_cast<uint8>(EGGYGOLocomotionCurveContentKind::EditorDataModel);
		const bool bMotionValid = SerializeEnum(Ar, MotionType);
		const bool bPhaseValid = SerializeEnum(Ar, TurnBackPhase);
		Ar << SequenceStartSeconds;
		Ar << SequenceEndSeconds;
		bValid = bValid && bMotionValid && bPhaseValid;
	}
	if (Ar.IsLoading())
	{
		bValid = bValid && FMath::IsFinite(NativeSampleKey) && FMath::IsFinite(SequenceStartSeconds)
			&& FMath::IsFinite(SequenceEndSeconds) && !SampleRotation.ContainsNaN();
	}
	bOutSuccess = bValid && bLocation && !Ar.IsError();
	return true;
}

bool FGGYGORemoteMotionPoseRecord::HasSameContent(const FGGYGORemoteMotionPoseRecord& Other) const
{
	return PublisherEpoch == Other.PublisherEpoch && NativeSampleKey == Other.NativeSampleKey && bKeyed == Other.bKeyed
		&& Kind == Other.Kind && Closure == Other.Closure && Reason == Other.Reason && SegmentSerial == Other.SegmentSerial
		&& ConfigurationDigest == Other.ConfigurationDigest && CurveContentDigest == Other.CurveContentDigest
		&& CurveContentKind == Other.CurveContentKind && MotionType == Other.MotionType && TurnBackPhase == Other.TurnBackPhase
		&& SequenceStartSeconds == Other.SequenceStartSeconds && SequenceEndSeconds == Other.SequenceEndSeconds
		&& bRelativeTransform == Other.bRelativeTransform && SampleLocation == Other.SampleLocation
		&& SampleRotation == Other.SampleRotation;
}

bool UGGYGOCharacterMovementComponent::FRemoteMotionPoseSegmentKey::Matches(const FGGYGOMotionExecutionSnapshot& Snapshot) const
{
	return ExecutionRequestSerial == Snapshot.ExecutionRequestSerial && MotionSequence == Snapshot.MotionSequence
		&& MotionType == Snapshot.MotionType && Binding == Snapshot.OriginalLocomotionBinding;
}

bool UGGYGOCharacterMovementComponent::FRemoteMotionPoseOwnerIdentity::operator==(const FRemoteMotionPoseOwnerIdentity& Other) const
{
	return Character == Other.Character && Component == Other.Component && Controller == Other.Controller
		&& LocalRole == Other.LocalRole && RemoteRole == Other.RemoteRole;
}

UGGYGOCharacterMovementComponent::FRemoteMotionPoseOwnerIdentity
UGGYGOCharacterMovementComponent::CaptureRemoteMotionPoseOwnerIdentity() const
{
	FRemoteMotionPoseOwnerIdentity Identity;
	Identity.Character = CharacterOwner;
	Identity.Component = UpdatedComponent.Get();
	if (IsValid(CharacterOwner))
	{
		Identity.Controller = CharacterOwner->GetController();
		Identity.LocalRole = CharacterOwner->GetLocalRole();
		Identity.RemoteRole = CharacterOwner->GetRemoteRole();
	}
	return Identity;
}

// ============================================================================
// 发布端
// ============================================================================

bool UGGYGOCharacterMovementComponent::IsRemoteMotionPosePublisher() const
{
	const ENetMode Mode = GetNetMode();
	return !bRemoteMotionPosePublicationClosed && IsValid(CharacterOwner) && CharacterOwner->HasAuthority()
		&& (Mode == NM_DedicatedServer || Mode == NM_ListenServer);
}

bool UGGYGOCharacterMovementComponent::RefreshRemoteMotionPosePublisherIdentity()
{
	if (!IsRemoteMotionPosePublisher()) return false;
	const FRemoteMotionPoseOwnerIdentity Current = CaptureRemoteMotionPoseOwnerIdentity();
	if (RemoteMotionPosePublisherIdentity.IsSet() && *RemoteMotionPosePublisherIdentity == Current) return true;
	// 控制者/远端角色决定原生时间戳来源（World 时间或远端客户端累计时间戳）；换代后旧候选与段全部退役。
	if (RemoteMotionPosePublisherIdentity.IsSet())
		CloseRemoteMotionPoseSegment(EGGYGORemoteMotionPoseReason::PublisherIdentityChanged);
	RemoteMotionPoseCandidate.Reset();
	RemoteMotionPoseSegment.Reset();
	if (RemoteMotionPosePublisherEpoch == MAX_uint32)
	{
		FGGYGORemoteMotionPoseRecord Exhausted;
		Exhausted.Kind = EGGYGORemoteMotionPoseKind::Invalid;
		Exhausted.Reason = EGGYGORemoteMotionPoseReason::SerialExhausted;
		PublishRemoteMotionPoseRecord(Exhausted);
		bRemoteMotionPosePublicationClosed = true;
		return false;
	}
	++RemoteMotionPosePublisherEpoch;
	RemoteMotionPosePublisherIdentity = Current;
	return true;
}

void UGGYGOCharacterMovementComponent::CaptureRemoteMotionPoseCandidate()
{
	if (!RefreshRemoteMotionPosePublisherIdentity()) return;
	FRemoteMotionPoseCandidate Candidate;
	Candidate.Snapshot = MotionExecutionSnapshot;
	Candidate.MoveEntrySerial = RemoteMotionPoseMoveEntrySerial;
	Candidate.PublisherEpoch = RemoteMotionPosePublisherEpoch;
	RemoteMotionPoseCandidate = MoveTemp(Candidate);
}

void UGGYGOCharacterMovementComponent::PublishRemoteMotionPoseRecord(FGGYGORemoteMotionPoseRecord Content)
{
	if (bRemoteMotionPosePublicationClosed) return;
	Content.PublisherEpoch = RemoteMotionPosePublisherEpoch;
	if (RemoteMotionPoseRecord.PublicationSerial != 0 && RemoteMotionPoseRecord.HasSameContent(Content)) return;
	if (RemoteMotionPoseRecord.PublicationSerial >= MAX_uint32 - 1)
	{
		Content = {};
		Content.PublisherEpoch = RemoteMotionPosePublisherEpoch;
		Content.Kind = EGGYGORemoteMotionPoseKind::Invalid;
		Content.Reason = EGGYGORemoteMotionPoseReason::SerialExhausted;
		bRemoteMotionPosePublicationClosed = true;
	}
	if (Content.Kind == EGGYGORemoteMotionPoseKind::Invalid && !bRemoteMotionPosePublisherDiagnosticReported)
	{
		bRemoteMotionPosePublisherDiagnosticReported = true;
		UE_LOG(LogGGYGORemoteMotionPose, Error, TEXT("[Movement.RemoteMotionPose] CMC='%s' Character='%s' published Invalid: %s."),
			*GetPathName(), *GetPathNameSafe(CharacterOwner), LexRemoteReason(Content.Reason));
	}
	Content.PublicationSerial = RemoteMotionPoseRecord.PublicationSerial + 1;
	RemoteMotionPoseRecord = Content;
	// listen server 远端控制角色与代理共用同一接受路径；本地没有网络接收。
	if (IsRemoteMotionPoseConsumer()) ReceiveRemoteMotionPoseRecord(RemoteMotionPoseRecord);
}

void UGGYGOCharacterMovementComponent::FinalizeRemoteMotionPosePublication()
{
	if (!RefreshRemoteMotionPosePublisherIdentity() || !RemoteMotionPoseCandidate.IsSet()
		|| RemoteMotionPoseCandidate->bFinalized) return;
	RemoteMotionPoseCandidate->bFinalized = true;
	const FRemoteMotionPoseCandidate Candidate = *RemoteMotionPoseCandidate;
	const FGGYGOMotionExecutionSnapshot& Snapshot = Candidate.Snapshot;
	FGGYGORemoteMotionPoseRecord Record;
	const auto PublishKeyless = [this, &Record](EGGYGORemoteMotionPoseKind Kind, EGGYGORemoteMotionPoseReason Reason)
	{
		Record = {};
		Record.Kind = Kind;
		Record.Reason = Reason;
		PublishRemoteMotionPoseRecord(Record);
	};
	if (NetworkSmoothingMode != ENetworkSmoothingMode::Linear && !bNetworkAlwaysReplicateTransformUpdateTimestamp)
	{
		// 原生 Character::PreReplication 此时复制 0，观察端没有任何样本身份可配对。
		PublishKeyless(EGGYGORemoteMotionPoseKind::Invalid, EGGYGORemoteMotionPoseReason::NoNativeSampleKey);
		return;
	}
	// 时间戳只在 PerformMovement 尾部写入，每次写入之前都经过 UpdateCharacterStateBeforeMovement。
	// 入口序号与 Epoch 不变即证明当前时间戳是本候选那次移动写下的，或胶囊按位未变时沿用的上一个样本。
	if (Candidate.PublisherEpoch != RemoteMotionPosePublisherEpoch
		|| Candidate.MoveEntrySerial != RemoteMotionPoseMoveEntrySerial || bMotionExecutionIntervalOpen)
	{
		PublishKeyless(EGGYGORemoteMotionPoseKind::Unkeyed, EGGYGORemoteMotionPoseReason::LaterMoveWithoutCompletion);
		return;
	}
	if (!IsValid(UpdatedComponent) || UpdatedComponent->GetComponentLocation() != LastUpdateLocation
		|| !UpdatedComponent->GetComponentQuat().Equals(LastUpdateRotation, 0.0))
	{
		PublishKeyless(EGGYGORemoteMotionPoseKind::Unkeyed, EGGYGORemoteMotionPoseReason::TransformChangedAfterCompletion);
		return;
	}
	Record.bKeyed = true;
	Record.NativeSampleKey = GetServerLastTransformUpdateTimeStamp();
	const FBasedMovementInfo& Based = CharacterOwner->GetBasedMovement();
	if (Based.HasRelativeLocation())
	{
		// 与 Character::PreReplication 填写 ReplicatedBasedMovement 的规则一致。
		Record.bRelativeTransform = true;
		Record.SampleLocation = Based.Location;
		Record.SampleRotation = Based.HasRelativeRotation() ? Based.Rotation : CharacterOwner->GetActorRotation();
	}
	else
	{
		// 与 AActor::GatherCurrentMovement 一致：零原点位置与组件旋转。
		Record.SampleLocation = FRepMovement::RebaseOntoZeroOrigin(LastUpdateLocation, CharacterOwner);
		Record.SampleRotation = LastUpdateRotation.Rotator();
	}

	const bool bSingleSequence = Snapshot.SourceKind == EGGYGOMotionExecutionSource::Locomotion
		&& Snapshot.bHasSingleSequenceTime && Snapshot.OriginalSequence.IsValid() && Snapshot.OriginalLocomotionBinding.IsValid();
	if (bSingleSequence && Snapshot.Status == EGGYGOMotionExecutionStatus::Valid)
	{
		// 完成区间之后的外部 delegate 若移动了胶囊，原生时间戳记录的是被改后的变换，不能冒认本区间。
		if (Snapshot.OriginalMovement.Get() != this || Snapshot.OriginalCharacter.Get() != CharacterOwner
			|| Snapshot.OriginalUpdatedComponent.Get() != UpdatedComponent || Snapshot.LocationAfter != LastUpdateLocation
			|| !Snapshot.RotationAfter.Equals(LastUpdateRotation, 0.0))
		{
			PublishKeyless(EGGYGORemoteMotionPoseKind::Unkeyed, EGGYGORemoteMotionPoseReason::TransformChangedAfterCompletion);
			return;
		}
		FString Error;
		const EGGYGOMotionExecutionStatus Membership = QualifyLocomotionExecutionMembership(Snapshot, Error);
		// 已自然完成的终点是既成事实；其后新请求的出现不能把它改写成取消。
		const bool bCompletedEndpoint = Snapshot.bEnded && Snapshot.EndReason == EGGYGOMotionExecutionEndReason::Completed;
		if (Membership == EGGYGOMotionExecutionStatus::Invalid
			|| (Membership == EGGYGOMotionExecutionStatus::NotApplicable && !bCompletedEndpoint))
		{
			const bool bFailed = Membership == EGGYGOMotionExecutionStatus::Invalid;
			Record.Kind = EGGYGORemoteMotionPoseKind::Terminated;
			Record.Closure = bFailed ? EGGYGORemoteMotionPoseClosure::Failed : EGGYGORemoteMotionPoseClosure::Cancelled;
			Record.Reason = bFailed ? EGGYGORemoteMotionPoseReason::SourceInvalid : EGGYGORemoteMotionPoseReason::RequestRetired;
			Record.SegmentSerial = RemoteMotionPoseSegment.IsSet() && RemoteMotionPoseSegment->Matches(Snapshot) ? RemoteMotionPoseLastSegmentSerial : 0;
			RemoteMotionPoseSegment.Reset();
			PublishRemoteMotionPoseRecord(Record);
			return;
		}
		const FGGYGOLocomotionSequenceSource* Source = Snapshot.OriginalLocomotionBinding->GetSingleSource(Snapshot.MotionType);
		FGGYGOLocomotionSourceIdentityDigest Identity;
		if (!Source || Source->Sequence.Get() != Snapshot.OriginalSequence.Get()
			|| !GGYGOLocomotionEvaluation::ComputeSourceIdentity(*Source, Identity, Error))
		{
			UE_LOG(LogGGYGORemoteMotionPose, Error, TEXT("[Movement.RemoteMotionPose] CMC='%s' Motion=%d: authority source identity failed: %s"),
				*GetPathName(), static_cast<int32>(Snapshot.MotionType), Error.IsEmpty() ? TEXT("original route changed") : *Error);
			RemoteMotionPoseSegment.Reset();
			PublishKeyless(EGGYGORemoteMotionPoseKind::Invalid, EGGYGORemoteMotionPoseReason::SourceIdentityFailed);
			return;
		}
		if (!RemoteMotionPoseSegment.IsSet() || !RemoteMotionPoseSegment->Matches(Snapshot))
		{
			if (RemoteMotionPoseLastSegmentSerial == MAX_uint32)
			{
				PublishKeyless(EGGYGORemoteMotionPoseKind::Invalid, EGGYGORemoteMotionPoseReason::SerialExhausted);
				bRemoteMotionPosePublicationClosed = true;
				return;
			}
			FRemoteMotionPoseSegmentKey Segment;
			Segment.ExecutionRequestSerial = Snapshot.ExecutionRequestSerial;
			Segment.MotionSequence = Snapshot.MotionSequence;
			Segment.MotionType = Snapshot.MotionType;
			Segment.Binding = Snapshot.OriginalLocomotionBinding;
			RemoteMotionPoseSegment = MoveTemp(Segment);
			++RemoteMotionPoseLastSegmentSerial;
		}
		Record.Kind = EGGYGORemoteMotionPoseKind::Sample;
		Record.SegmentSerial = RemoteMotionPoseLastSegmentSerial;
		Record.ConfigurationDigest = Identity.ConfigurationDigest;
		Record.CurveContentDigest = Identity.CurveContentDigest;
		Record.CurveContentKind = static_cast<uint8>(Identity.ContentKind);
		Record.MotionType = Snapshot.MotionType;
		Record.TurnBackPhase = Snapshot.TurnBackPhase;
		Record.SequenceStartSeconds = Snapshot.SequenceStartSeconds;
		Record.SequenceEndSeconds = Snapshot.SequenceEndSeconds;
		Record.Closure = bCompletedEndpoint ? EGGYGORemoteMotionPoseClosure::Completed : EGGYGORemoteMotionPoseClosure::None;
		PublishRemoteMotionPoseRecord(Record);
		return;
	}
	if (Snapshot.SourceKind == EGGYGOMotionExecutionSource::Locomotion && Snapshot.bEnded
		&& (Snapshot.EndReason == EGGYGOMotionExecutionEndReason::Cancelled || Snapshot.EndReason == EGGYGOMotionExecutionEndReason::Failed))
	{
		const bool bFailed = Snapshot.EndReason == EGGYGOMotionExecutionEndReason::Failed;
		Record.Kind = EGGYGORemoteMotionPoseKind::Terminated;
		Record.Closure = bFailed ? EGGYGORemoteMotionPoseClosure::Failed : EGGYGORemoteMotionPoseClosure::Cancelled;
		Record.Reason = bFailed ? EGGYGORemoteMotionPoseReason::SourceInvalid : EGGYGORemoteMotionPoseReason::RequestRetired;
		Record.SegmentSerial = RemoteMotionPoseSegment.IsSet() && RemoteMotionPoseSegment->Matches(Snapshot) ? RemoteMotionPoseLastSegmentSerial : 0;
		RemoteMotionPoseSegment.Reset();
		PublishRemoteMotionPoseRecord(Record);
		return;
	}
	// WalkRun、普通无曲线区间、Montage 动作等：此样本没有单 Sequence 段。
	// 每个新键都发布，使观察端可在任一后续原生样本上同键配对，而不依赖键排序。
	Record.Kind = EGGYGORemoteMotionPoseKind::NoSegment;
	RemoteMotionPoseSegment.Reset();
	PublishRemoteMotionPoseRecord(Record);
}

void UGGYGOCharacterMovementComponent::CloseRemoteMotionPoseSegment(EGGYGORemoteMotionPoseReason Reason)
{
	if (!IsRemoteMotionPosePublisher()) return;
	RemoteMotionPoseCandidate.Reset();
	RemoteMotionPoseSegment.Reset();
	if (RemoteMotionPoseRecord.Kind != EGGYGORemoteMotionPoseKind::Sample
		|| RemoteMotionPoseRecord.Closure != EGGYGORemoteMotionPoseClosure::None) return;
	// 段资源在 move 之外退役：与样本无关的段事实，观察端接收即关闭。
	FGGYGORemoteMotionPoseRecord Record;
	Record.Kind = EGGYGORemoteMotionPoseKind::Terminated;
	Record.Closure = EGGYGORemoteMotionPoseClosure::Cancelled;
	Record.Reason = Reason;
	Record.SegmentSerial = RemoteMotionPoseRecord.SegmentSerial;
	PublishRemoteMotionPoseRecord(Record);
}

// ============================================================================
// 观察端：身份、接收与原生呈现样本
// ============================================================================

bool UGGYGOCharacterMovementComponent::IsRemoteMotionPoseConsumer() const
{
	if (!IsValid(CharacterOwner) || !IsValid(UpdatedComponent)) return false;
	if (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy) return GetNetMode() == NM_Client;
	// listen server 看远端客户端：原生每个 ServerMove 后在本 CMC 上做平滑，本地快照的一帧窗口不覆盖无 move 帧。
	return CharacterOwner->HasAuthority() && GetNetMode() == NM_ListenServer
		&& CharacterOwner->GetRemoteRole() == ROLE_AutonomousProxy && !CharacterOwner->IsLocallyControlled();
}

bool UGGYGOCharacterMovementComponent::EnsureRemoteMotionPoseConsumerIdentity()
{
	if (!IsRemoteMotionPoseConsumer())
	{
		if (RemoteMotionPoseConsumerIdentity.IsSet() || RemoteMotionPoseAccepted.IsSet() || RemoteMotionPosePending.IsSet())
			ResetRemoteMotionPoseConsumer();
		return false;
	}
	const FRemoteMotionPoseOwnerIdentity Current = CaptureRemoteMotionPoseOwnerIdentity();
	if (!RemoteMotionPoseConsumerIdentity.IsSet() || !(*RemoteMotionPoseConsumerIdentity == Current))
	{
		// 控制者、网络角色、角色或组件换代：旧记录、样本、上一样本映射全部退役，不补造样本。
		ResetRemoteMotionPoseConsumer();
		RemoteMotionPoseConsumerIdentity = Current;
	}
	return true;
}

void UGGYGOCharacterMovementComponent::ResetRemoteMotionPoseConsumer()
{
	RemoteMotionPosePending.Reset();
	RemoteMotionPoseAccepted.Reset();
	RemoteMotionPosePrevious.Reset();
	RemoteMotionPoseNativeSample = {};
	RemoteMotionPoseConsumerIdentity.Reset();
	RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::None;
	RemoteMotionPoseDiagnostic.Reset();
	RemoteMotionPoseLastSeenSerial = 0;
	bRemoteMotionPoseSeen = false;
}

void UGGYGOCharacterMovementComponent::OnRep_RemoteMotionPoseRecord()
{
	ReceiveRemoteMotionPoseRecord(RemoteMotionPoseRecord);
}

void UGGYGOCharacterMovementComponent::ReceiveRemoteMotionPoseRecord(const FGGYGORemoteMotionPoseRecord& Record)
{
	if (!IsInGameThread() || Record.PublicationSerial == 0 || !EnsureRemoteMotionPoseConsumerIdentity()) return;
	// 只用发布序号判断重复/乱序；有效期由原生样本配对与段事实决定。
	if (bRemoteMotionPoseSeen && Record.PublicationSerial <= RemoteMotionPoseLastSeenSerial) return;
	bRemoteMotionPoseSeen = true;
	RemoteMotionPoseLastSeenSerial = Record.PublicationSerial;
	if (RemoteMotionPoseAccepted.IsSet() && RemoteMotionPoseAccepted->Record.PublisherEpoch != Record.PublisherEpoch)
	{
		// 发布端身份换代：时间戳来源可能改变，旧 Epoch 的事实与插值映射不再可比。
		RemoteMotionPoseAccepted.Reset();
		RemoteMotionPosePrevious.Reset();
	}
	RemoteMotionPosePending = Record;
	TryAcceptRemoteMotionPose();
}

void UGGYGOCharacterMovementComponent::SmoothCorrection(const FVector& OldLocation, const FQuat& OldRotation,
	const FVector& NewLocation, const FQuat& NewRotation)
{
	Super::SmoothCorrection(OldLocation, OldRotation, NewLocation, NewRotation);
	if (!HasValidData()) return;
	CaptureRemoteMotionPoseNativeSample(NewLocation, NewRotation);
}

void UGGYGOCharacterMovementComponent::CaptureRemoteMotionPoseNativeSample(const FVector& NewLocation, const FQuat& NewRotation)
{
	if (!IsInGameThread() || !EnsureRemoteMotionPoseConsumerIdentity()) return;
	FRemoteMotionPoseNativeSample Sample;
	Sample.EventSerial = ++RemoteMotionPoseNativeEventSerial;
	if (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy)
	{
		// 原生 SmoothCorrection 在同一次调用内读取这一复制时间戳作为本呈现的样本时间。
		Sample.Key = CharacterOwner->GetReplicatedServerLastTransformUpdateTimeStamp();
		const FBasedMovementInfo& Based = CharacterOwner->GetReplicatedBasedMovement();
		const FRepMovement& Rep = CharacterOwner->GetReplicatedMovement();
		if (CurrentRootMotion.HasActiveRootMotionSources() || CharacterOwner->IsPlayingNetworkedRootMotionMontage())
		{
			// 根运动接收路径的实参来自 RootMotionRepMoves，不是本时间戳所属的普通复制呈现。
			Sample.Diagnostic = TEXT("native sample was presented through the root-motion receive path");
		}
		else if (Based.HasRelativeLocation())
		{
			// OnRep_ReplicatedBasedMovement：实参 = OldBase 变换作用于复制的相对位置/旋转。
			const FTransform BaseTransform(OldBaseQuat, OldBaseLocation);
			const FVector ExpectedLocation = BaseTransform.TransformPositionNoScale(Based.Location);
			FRotator ExpectedRotation = Based.Rotation;
			if (Based.HasRelativeRotation())
			{
				ExpectedRotation = (FRotationMatrix(Based.Rotation) * FQuatRotationMatrix(OldBaseQuat)).Rotator();
				if (ShouldRemainVertical())
				{
					ExpectedRotation.Pitch = 0.0f;
					ExpectedRotation.Roll = 0.0f;
				}
			}
			if (NewLocation.Equals(ExpectedLocation, 1.e-3f) && NewRotation.Equals(ExpectedRotation.Quaternion(), 1.e-5f))
			{
				Sample.bCertifiable = true;
				Sample.bRelativeTransform = true;
				Sample.Location = BaseTransform.InverseTransformPositionNoScale(NewLocation);
				Sample.Rotation = Based.HasRelativeRotation()
					? FRotator(OldBaseQuat.Inverse() * NewRotation) : NewRotation.Rotator();
				// 记录与复制底座位置都按 NetQuantize100；旋转两端各一次 16 位压缩。
				Sample.LocationTolerance = 0.02f;
				Sample.RotationTolerance = 2.0f * CompressedShortAxisStep + 1.e-3f;
			}
			else Sample.Diagnostic = TEXT("SmoothCorrection arguments are not the replicated based-movement presentation");
		}
		else
		{
			// PostNetReceiveLocationAndRotation：实参 = 复制位置重定位到本地原点、复制旋转。
			const FVector ExpectedLocation = FRepMovement::RebaseOntoLocalOrigin(Rep.Location, CharacterOwner);
			if (NewLocation.Equals(ExpectedLocation, 1.e-3f) && NewRotation.Equals(Rep.Rotation.Quaternion(), 1.e-5f))
			{
				Sample.bCertifiable = true;
				Sample.Location = FRepMovement::RebaseOntoZeroOrigin(NewLocation, CharacterOwner);
				Sample.Rotation = NewRotation.Rotator();
				const float LocationStep = Rep.LocationQuantizationLevel == EVectorQuantization::RoundWholeNumber ? 1.0f
					: Rep.LocationQuantizationLevel == EVectorQuantization::RoundOneDecimal ? 0.1f : 0.01f;
				const float RotationStep = Rep.RotationQuantizationLevel == ERotatorQuantization::ByteComponents
					? 360.0f / 256.0f : CompressedShortAxisStep;
				Sample.LocationTolerance = LocationStep + 0.01f;
				Sample.RotationTolerance = RotationStep + CompressedShortAxisStep + 1.e-3f;
			}
			else Sample.Diagnostic = TEXT("SmoothCorrection arguments are not the replicated movement presentation");
		}
	}
	else
	{
		// listen server 的 SmoothCorrection 紧随本次 MoveAutonomous 的 PerformMovement；
		// 实参必须正是原生刚写时间戳时记下的 LastUpdate 变换。
		Sample.Key = GetServerLastTransformUpdateTimeStamp();
		if (NewLocation == LastUpdateLocation && NewRotation.Equals(LastUpdateRotation, 0.0))
		{
			const FBasedMovementInfo& Based = CharacterOwner->GetBasedMovement();
			Sample.bCertifiable = true;
			Sample.bRelativeTransform = Based.HasRelativeLocation();
			Sample.Location = Sample.bRelativeTransform ? FVector(Based.Location) : FRepMovement::RebaseOntoZeroOrigin(NewLocation, CharacterOwner);
			Sample.Rotation = Sample.bRelativeTransform && Based.HasRelativeRotation() ? Based.Rotation : NewRotation.Rotator();
			Sample.LocationTolerance = 0.01f;
			Sample.RotationTolerance = 1.e-3f;
		}
		else Sample.Diagnostic = TEXT("listen-server SmoothCorrection arguments are not the latest native movement result");
	}
	RemoteMotionPoseNativeSample = Sample;
	// 同一已认证记录的同键合法重复呈现仍是该样本；否则已接受样本自然变为 Held。
	if (RemoteMotionPoseAccepted.IsSet() && RemoteMotionPoseAccepted->Record.bKeyed
		&& DoesRemoteMotionPoseMatchNativeSample(RemoteMotionPoseAccepted->Record))
		RemoteMotionPoseAccepted->MatchedEventSerial = Sample.EventSerial;
	TryAcceptRemoteMotionPose();
}

bool UGGYGOCharacterMovementComponent::DoesRemoteMotionPoseMatchNativeSample(const FGGYGORemoteMotionPoseRecord& Record) const
{
	const FRemoteMotionPoseNativeSample& Sample = RemoteMotionPoseNativeSample;
	return Sample.EventSerial != 0 && Sample.bCertifiable && Record.bKeyed && Sample.Key == Record.NativeSampleKey
		&& Sample.bRelativeTransform == Record.bRelativeTransform
		&& Sample.Location.Equals(Record.SampleLocation, Sample.LocationTolerance)
		&& Sample.Rotation.Equals(Record.SampleRotation, Sample.RotationTolerance);
}

// ============================================================================
// 观察端：源资格与接受
// ============================================================================

UGGYGOCharacterMovementComponent::ERemoteMotionPoseSourceCheck UGGYGOCharacterMovementComponent::VerifyRemoteMotionPoseSource(
	const FGGYGORemoteMotionPoseRecord& Record, FGGYGOLocomotionSourceBindingPtr& OutBinding,
	EGGYGORemoteMotionPoseIdentity& OutIdentity, EGGYGORemoteMotionPoseWaitReason& OutWait, FString& OutError) const
{
	OutBinding.Reset();
	OutIdentity = EGGYGORemoteMotionPoseIdentity::None;
	OutWait = EGGYGORemoteMotionPoseWaitReason::None;
	OutError.Reset();
	const FGGYGOLocomotionSourceBindingPtr Binding = LocomotionSourceBinding;
	if (!Binding.IsValid())
	{
		OutWait = EGGYGORemoteMotionPoseWaitReason::ProviderNotPublished;
		OutError = TEXT("observer Animation source binding is not published");
		return ERemoteMotionPoseSourceCheck::Awaiting;
	}
	const FGGYGOLocomotionSourceIdentity& Id = Binding->Identity;
	if (Id.Character.Get() != CharacterOwner)
	{
		OutError = TEXT("observer Animation source binding belongs to another character");
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	const USkeletalMeshComponent* Mesh = Id.Mesh.Get();
	if (!Mesh || Mesh != CharacterOwner->GetMesh() || !Id.Producer.IsValid() || Mesh->GetAnimInstance() != Id.Producer.Get())
	{
		// 原生 Mesh/AnimInstance 生命周期正在替换，原发布者的退役与新发布尚未完成。
		OutWait = EGGYGORemoteMotionPoseWaitReason::ProviderRetiring;
		OutError = TEXT("observer Animation source publication is being retired by its native producer lifecycle");
		return ERemoteMotionPoseSourceCheck::Awaiting;
	}
	if (!GGYGOLocomotionEvaluation::ValidateBinding(*Binding, OutError)) return ERemoteMotionPoseSourceCheck::Invalid;
	const FGGYGOLocomotionSequenceSource* Source = Binding->GetSingleSource(Record.MotionType);
	if (!Source || Source->bLoop)
	{
		OutError = FString::Printf(TEXT("observer has no non-loop single source for motion %d"), static_cast<int32>(Record.MotionType));
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	FGGYGOLocomotionSourceIdentityDigest Identity;
	if (!GGYGOLocomotionEvaluation::ComputeSourceIdentity(*Source, Identity, OutError)) return ERemoteMotionPoseSourceCheck::Invalid;
	if (Identity.ConfigurationDigest != Record.ConfigurationDigest)
	{
		OutError = FString::Printf(TEXT("source configuration differs: Sequence='%s' Motion=%d Authority=%016llx Observer=%016llx"),
			*GetPathNameSafe(Source->Sequence.Get()), static_cast<int32>(Record.MotionType),
			static_cast<unsigned long long>(Record.ConfigurationDigest), static_cast<unsigned long long>(Identity.ConfigurationDigest));
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	const uint8 ObserverKind = static_cast<uint8>(Identity.ContentKind);
	if (ObserverKind != Record.CurveContentKind)
	{
		// 表达不同不能互相证明；只在编辑器一侧压缩尚未完成时合法出现，随原生压缩生命周期收敛。
		const bool bEditorSidePending = Identity.ContentKind == EGGYGOLocomotionCurveContentKind::EditorDataModel
			|| Record.CurveContentKind == static_cast<uint8>(EGGYGOLocomotionCurveContentKind::EditorDataModel);
		OutError = FString::Printf(TEXT("source curve representation differs: Sequence='%s' Motion=%d AuthorityKind=%u ObserverKind=%u"),
			*GetPathNameSafe(Source->Sequence.Get()), static_cast<int32>(Record.MotionType),
			static_cast<uint32>(Record.CurveContentKind), static_cast<uint32>(ObserverKind));
		if (!bEditorSidePending) return ERemoteMotionPoseSourceCheck::Invalid;
		OutWait = EGGYGORemoteMotionPoseWaitReason::ProviderContentPending;
		return ERemoteMotionPoseSourceCheck::Awaiting;
	}
	if (Identity.CurveContentDigest != Record.CurveContentDigest)
	{
		OutError = FString::Printf(TEXT("source curve content differs: Sequence='%s' Motion=%d Kind=%u Authority=%016llx Observer=%016llx"),
			*GetPathNameSafe(Source->Sequence.Get()), static_cast<int32>(Record.MotionType), static_cast<uint32>(ObserverKind),
			static_cast<unsigned long long>(Record.CurveContentDigest), static_cast<unsigned long long>(Identity.CurveContentDigest));
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	OutIdentity = Identity.ContentKind == EGGYGOLocomotionCurveContentKind::CompressedCodecStream
		? EGGYGORemoteMotionPoseIdentity::CompressedCurveContent : EGGYGORemoteMotionPoseIdentity::EditorModelCurveContent;
	if (!FMath::IsFinite(Record.SequenceStartSeconds) || !FMath::IsFinite(Record.SequenceEndSeconds)
		|| Record.SequenceStartSeconds < 0.0 || Record.SequenceEndSeconds < Record.SequenceStartSeconds
		|| Record.SequenceEndSeconds > static_cast<double>(Source->PlayLength))
	{
		OutError = FString::Printf(TEXT("certified sequence interval [%.9g, %.9g] is outside the observer source length %.9g"),
			Record.SequenceStartSeconds, Record.SequenceEndSeconds, Source->PlayLength);
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	OutBinding = Binding;
	return ERemoteMotionPoseSourceCheck::Verified;
}

UGGYGOCharacterMovementComponent::ERemoteMotionPoseSourceCheck UGGYGOCharacterMovementComponent::CheckRemoteMotionPoseBindingCurrent(
	const FRemoteMotionPoseAccepted& Accepted, const FGGYGOLocomotionSequenceSource*& OutSource,
	EGGYGORemoteMotionPoseWaitReason& OutWait, FString& OutError) const
{
	OutSource = nullptr;
	OutWait = EGGYGORemoteMotionPoseWaitReason::None;
	OutError.Reset();
	const FGGYGOLocomotionSourceBindingPtr& Binding = Accepted.Binding;
	if (!LocomotionSourceBinding.IsValid())
	{
		OutWait = EGGYGORemoteMotionPoseWaitReason::ProviderNotPublished;
		OutError = TEXT("observer Animation source binding was retired");
		return ERemoteMotionPoseSourceCheck::Awaiting;
	}
	if (!Binding.IsValid() || Binding != LocomotionSourceBinding)
	{
		// 不可变发布已被替换；新发布的内容身份由下一次原生 Tick 重验，查询不沿用旧结论。
		OutWait = EGGYGORemoteMotionPoseWaitReason::ProviderRetiring;
		OutError = TEXT("observer Animation source binding changed and awaits re-verification");
		return ERemoteMotionPoseSourceCheck::Awaiting;
	}
	const FGGYGOLocomotionSourceIdentity& Id = Binding->Identity;
	if (Id.Character.Get() != CharacterOwner)
	{
		OutError = TEXT("observer Animation source binding belongs to another character");
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	const USkeletalMeshComponent* Mesh = Id.Mesh.Get();
	if (!Mesh || Mesh != CharacterOwner->GetMesh() || !Id.Producer.IsValid() || Mesh->GetAnimInstance() != Id.Producer.Get())
	{
		OutWait = EGGYGORemoteMotionPoseWaitReason::ProviderRetiring;
		OutError = TEXT("observer Animation source publication is being retired by its native producer lifecycle");
		return ERemoteMotionPoseSourceCheck::Awaiting;
	}
	if (Binding->Status != EGGYGOLocomotionSourceStatus::Available)
	{
		OutError = Binding->Error.IsEmpty() ? FString(TEXT("observer Animation source publication is not available")) : Binding->Error;
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	const FGGYGOLocomotionSequenceSource* Source = Binding->GetSingleSource(Accepted.Record.MotionType);
	if (!Source || Source->bLoop)
	{
		OutError = TEXT("observer single source route is missing or loops");
		return ERemoteMotionPoseSourceCheck::Invalid;
	}
	// 配置当前性（资产存活、长度/速率未变、必需曲线存在）在每次查询确认；曲线内容摘要由原生 Tick 重算。
	if (!GGYGOLocomotionEvaluation::ValidateSource(*Source, OutError)) return ERemoteMotionPoseSourceCheck::Invalid;
	OutSource = Source;
	return ERemoteMotionPoseSourceCheck::Verified;
}

void UGGYGOCharacterMovementComponent::TryAcceptRemoteMotionPose()
{
	if (!RemoteMotionPosePending.IsSet()) return;
	const FGGYGORemoteMotionPoseRecord Record = *RemoteMotionPosePending;
	// 已接受 Sample 正因本端源资格等待时，快照状态由该资格决定；待配对记录的等待原因不得覆盖它。
	const bool bProviderWaitPinned = RemoteMotionPoseAccepted.IsSet()
		&& RemoteMotionPoseAccepted->Record.Kind == EGGYGORemoteMotionPoseKind::Sample
		&& !RemoteMotionPoseAccepted->Binding.IsValid() && IsProviderWaitReason(RemoteMotionPoseWaitReason);
	const auto Accept = [this, &Record](uint64 EventSerial, EGGYGORemoteMotionPoseIdentity Identity,
		const FGGYGOLocomotionSourceBindingPtr& Binding)
	{
		const bool bContinuesSegment = Record.Kind == EGGYGORemoteMotionPoseKind::Sample && RemoteMotionPoseAccepted.IsSet()
			&& RemoteMotionPoseAccepted->Record.Kind == EGGYGORemoteMotionPoseKind::Sample
			&& RemoteMotionPoseAccepted->Record.PublisherEpoch == Record.PublisherEpoch
			&& RemoteMotionPoseAccepted->Record.SegmentSerial == Record.SegmentSerial;
		if (!bContinuesSegment) RemoteMotionPosePrevious.Reset();
		else if (RemoteMotionPoseAccepted->Record.NativeSampleKey != Record.NativeSampleKey) RemoteMotionPosePrevious = RemoteMotionPoseAccepted;
		FRemoteMotionPoseAccepted Accepted;
		Accepted.Record = Record;
		Accepted.MatchedEventSerial = EventSerial;
		Accepted.Identity = Identity;
		Accepted.Binding = Binding;
		Accepted.AcceptedFrame = GFrameCounter;
		RemoteMotionPoseAccepted = MoveTemp(Accepted);
		RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::None;
		RemoteMotionPoseDiagnostic.Reset();
		RemoteMotionPosePending.Reset();
	};
	if (!Record.bKeyed)
	{
		if (Record.Kind == EGGYGORemoteMotionPoseKind::Unkeyed)
		{
			// 无法证明的样本不替换已有事实，也不把上一样本延长成新样本。
			if (!bProviderWaitPinned)
			{
				RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::NativeSampleUnverified;
				RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] authority sample unverified: %s"), LexRemoteReason(Record.Reason));
			}
			RemoteMotionPosePending.Reset();
			return;
		}
		// 段关闭或发布端协议失败与显示样本无关，接收即生效；不携带姿态时间。
		Accept(0, EGGYGORemoteMotionPoseIdentity::None, nullptr);
		if (Record.Kind == EGGYGORemoteMotionPoseKind::Invalid || Record.Closure == EGGYGORemoteMotionPoseClosure::Failed)
			RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] authority reported: %s"), LexRemoteReason(Record.Reason));
		return;
	}
	if (!DoesRemoteMotionPoseMatchNativeSample(Record))
	{
		// 只等真实同键呈现；更新的记录会替换本待配对记录，从不按键大小提前接受。
		if (bProviderWaitPinned) return;
		const FRemoteMotionPoseNativeSample& Sample = RemoteMotionPoseNativeSample;
		const bool bSameKeyRejected = Sample.EventSerial != 0 && Sample.Key == Record.NativeSampleKey;
		RemoteMotionPoseWaitReason = bSameKeyRejected
			? EGGYGORemoteMotionPoseWaitReason::NativeSampleUnverified : EGGYGORemoteMotionPoseWaitReason::NativeSample;
		if (bSameKeyRejected)
		{
			RemoteMotionPoseDiagnostic = Sample.bCertifiable
				? FString::Printf(TEXT("[Movement.RemoteMotionPose] same-key native presentation differs: Native=%s/%s Record=%s/%s Relative=%d/%d"),
					*Sample.Location.ToString(), *Sample.Rotation.ToString(), *Record.SampleLocation.ToString(),
					*Record.SampleRotation.ToString(), Sample.bRelativeTransform ? 1 : 0, Record.bRelativeTransform ? 1 : 0)
				: FString::Printf(TEXT("[Movement.RemoteMotionPose] %s"), *Sample.Diagnostic);
		}
		return;
	}
	const uint64 EventSerial = RemoteMotionPoseNativeSample.EventSerial;
	if (Record.Kind != EGGYGORemoteMotionPoseKind::Sample)
	{
		Accept(EventSerial, EGGYGORemoteMotionPoseIdentity::None, nullptr);
		if (Record.Kind == EGGYGORemoteMotionPoseKind::Invalid || Record.Closure == EGGYGORemoteMotionPoseClosure::Failed)
			RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] authority segment failed: %s"), LexRemoteReason(Record.Reason));
		return;
	}
	FGGYGOLocomotionSourceBindingPtr Binding;
	EGGYGORemoteMotionPoseIdentity Identity = EGGYGORemoteMotionPoseIdentity::None;
	EGGYGORemoteMotionPoseWaitReason Wait = EGGYGORemoteMotionPoseWaitReason::None;
	FString Error;
	const ERemoteMotionPoseSourceCheck Check = VerifyRemoteMotionPoseSource(Record, Binding, Identity, Wait, Error);
	if (Check == ERemoteMotionPoseSourceCheck::Awaiting)
	{
		RemoteMotionPoseWaitReason = Wait;
		RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] %s"), *Error);
		return;
	}
	if (Check == ERemoteMotionPoseSourceCheck::Invalid)
	{
		// 本端源已发布但与权威源身份不一致：已证明失败，不保留旧段姿态。
		FGGYGORemoteMotionPoseRecord Failed = Record;
		Failed.Kind = EGGYGORemoteMotionPoseKind::Invalid;
		Failed.Reason = EGGYGORemoteMotionPoseReason::SourceIdentityFailed;
		RemoteMotionPosePrevious.Reset();
		FRemoteMotionPoseAccepted Accepted;
		Accepted.Record = Failed;
		Accepted.MatchedEventSerial = EventSerial;
		Accepted.AcceptedFrame = GFrameCounter;
		RemoteMotionPoseAccepted = MoveTemp(Accepted);
		RemoteMotionPosePending.Reset();
		RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::None;
		RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] Character='%s' %s"), *GetPathNameSafe(CharacterOwner), *Error);
		UE_LOG(LogGGYGORemoteMotionPose, Error, TEXT("%s"), *RemoteMotionPoseDiagnostic);
		return;
	}
	Accept(EventSerial, Identity, Binding);
}

void UGGYGOCharacterMovementComponent::RefreshRemoteMotionPoseConsumer()
{
	if (!EnsureRemoteMotionPoseConsumerIdentity()) return;
	// 首个复制值可能在组件拥有原生角色前到达，或身份重置后需重新消费当前复制值；仍走序号与样本配对。
	if (CharacterOwner->GetLocalRole() == ROLE_SimulatedProxy && RemoteMotionPoseRecord.PublicationSerial != 0
		&& (!bRemoteMotionPoseSeen || RemoteMotionPoseRecord.PublicationSerial > RemoteMotionPoseLastSeenSerial))
		ReceiveRemoteMotionPoseRecord(RemoteMotionPoseRecord);
	if (RemoteMotionPoseAccepted.IsSet() && RemoteMotionPoseAccepted->Record.Kind == EGGYGORemoteMotionPoseKind::Sample)
	{
		// 曲线与本端源在会话内可变；每个原生 Tick 对已接受样本重算身份，不以绑定指针不变替代内容检查。
		FGGYGOLocomotionSourceBindingPtr Binding;
		EGGYGORemoteMotionPoseIdentity Identity = EGGYGORemoteMotionPoseIdentity::None;
		EGGYGORemoteMotionPoseWaitReason Wait = EGGYGORemoteMotionPoseWaitReason::None;
		FString Error;
		const ERemoteMotionPoseSourceCheck Check = VerifyRemoteMotionPoseSource(RemoteMotionPoseAccepted->Record, Binding, Identity, Wait, Error);
		if (Check == ERemoteMotionPoseSourceCheck::Verified)
		{
			RemoteMotionPoseAccepted->Binding = Binding;
			RemoteMotionPoseAccepted->Identity = Identity;
			// 本端源已真实就绪：撤销此前的资格等待原因，之后只剩待配对记录类原因（由下方 TryAccept 重新给出）。
			if (IsProviderWaitReason(RemoteMotionPoseWaitReason))
			{
				RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::None;
				RemoteMotionPoseDiagnostic.Reset();
			}
		}
		else if (Check == ERemoteMotionPoseSourceCheck::Awaiting)
		{
			// 保存真实资格原因（未发布/退役/内容表达未收敛）；快照在 Binding 为空时以它为准。
			RemoteMotionPoseAccepted->Binding.Reset();
			RemoteMotionPoseAccepted->Identity = EGGYGORemoteMotionPoseIdentity::None;
			RemoteMotionPoseWaitReason = Wait;
			RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] %s"), *Error);
		}
		else
		{
			RemoteMotionPoseAccepted->Record.Kind = EGGYGORemoteMotionPoseKind::Invalid;
			RemoteMotionPoseAccepted->Record.Reason = EGGYGORemoteMotionPoseReason::SourceIdentityFailed;
			RemoteMotionPoseAccepted->Binding.Reset();
			RemoteMotionPoseAccepted->Identity = EGGYGORemoteMotionPoseIdentity::None;
			RemoteMotionPosePrevious.Reset();
			// 已证明失败不是等待：清除任何资格等待原因，避免快照把失败报成可保持的 Awaiting。
			RemoteMotionPoseWaitReason = EGGYGORemoteMotionPoseWaitReason::None;
			RemoteMotionPoseDiagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] Character='%s' %s"), *GetPathNameSafe(CharacterOwner), *Error);
			UE_LOG(LogGGYGORemoteMotionPose, Error, TEXT("%s"), *RemoteMotionPoseDiagnostic);
		}
	}
	TryAcceptRemoteMotionPose();
}

// ============================================================================
// 观察端：查询
// ============================================================================

FGGYGORemoteMotionPoseSnapshot UGGYGOCharacterMovementComponent::GetRemoteMotionPoseSnapshot() const
{
	FGGYGORemoteMotionPoseSnapshot Result;
	if (!IsInGameThread())
	{
		Result.Status = EGGYGORemoteMotionPoseStatus::Invalid;
		Result.Diagnostic = TEXT("[Movement.RemoteMotionPose] capture requires the game thread");
		return Result;
	}
	if (!IsRemoteMotionPoseConsumer())
	{
		Result.Status = EGGYGORemoteMotionPoseStatus::NotRemote;
		return Result;
	}
	if (!RemoteMotionPoseConsumerIdentity.IsSet() || !(*RemoteMotionPoseConsumerIdentity == CaptureRemoteMotionPoseOwnerIdentity()))
	{
		// 身份换代后的清理在原生 Tick/接收入口完成；查询不沿用旧身份下的任何事实。
		Result.Status = EGGYGORemoteMotionPoseStatus::Initial;
		Result.Diagnostic = TEXT("[Movement.RemoteMotionPose] observer identity changed; previous facts are retired");
		return Result;
	}
	Result.WaitReason = RemoteMotionPoseWaitReason;
	Result.Diagnostic = RemoteMotionPoseDiagnostic;
	if (!RemoteMotionPoseAccepted.IsSet())
	{
		Result.Status = RemoteMotionPosePending.IsSet() || RemoteMotionPoseWaitReason != EGGYGORemoteMotionPoseWaitReason::None
			? EGGYGORemoteMotionPoseStatus::Awaiting : EGGYGORemoteMotionPoseStatus::Initial;
		return Result;
	}
	const FRemoteMotionPoseAccepted& Accepted = *RemoteMotionPoseAccepted;
	const FGGYGORemoteMotionPoseRecord& Record = Accepted.Record;
	Result.PublicationSerial = Record.PublicationSerial;
	Result.PublisherEpoch = Record.PublisherEpoch;
	Result.SegmentSerial = Record.SegmentSerial;
	Result.NativeSampleKey = Record.NativeSampleKey;
	Result.AcceptedLocalFrame = Accepted.AcceptedFrame;
	Result.Closure = Record.Closure;
	switch (Record.Kind)
	{
	case EGGYGORemoteMotionPoseKind::Sample:
		break;
	case EGGYGORemoteMotionPoseKind::NoSegment:
		Result.Status = EGGYGORemoteMotionPoseStatus::NotApplicable;
		return Result;
	case EGGYGORemoteMotionPoseKind::Terminated:
		Result.Status = Record.Closure == EGGYGORemoteMotionPoseClosure::Failed
			? EGGYGORemoteMotionPoseStatus::Invalid : EGGYGORemoteMotionPoseStatus::NotApplicable;
		return Result;
	case EGGYGORemoteMotionPoseKind::Invalid:
		Result.Status = EGGYGORemoteMotionPoseStatus::Invalid;
		if (Result.Diagnostic.IsEmpty())
			Result.Diagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] %s"), LexRemoteReason(Record.Reason));
		return Result;
	default:
		Result.Status = EGGYGORemoteMotionPoseStatus::Initial;
		return Result;
	}
	const FGGYGOLocomotionSequenceSource* Source = nullptr;
	EGGYGORemoteMotionPoseWaitReason Wait = EGGYGORemoteMotionPoseWaitReason::None;
	FString Error;
	const ERemoteMotionPoseSourceCheck Check = CheckRemoteMotionPoseBindingCurrent(Accepted, Source, Wait, Error);
	if (Check == ERemoteMotionPoseSourceCheck::Invalid)
	{
		// 已证明失败：不保留段事实，调用方必须退出。
		Result.Status = EGGYGORemoteMotionPoseStatus::Invalid;
		Result.WaitReason = EGGYGORemoteMotionPoseWaitReason::None;
		Result.Diagnostic = FString::Printf(TEXT("[Movement.RemoteMotionPose] %s"), *Error);
		return Result;
	}
	// 段阶段与关闭事实同出这条已配对 Sample；Current/Held/资格等待共用，资格由下方状态决定。
	Result.MotionType = Record.MotionType;
	Result.TurnBackPhase = Record.TurnBackPhase;
	Result.bTurnBackCurveDriven = IsTurnBackCurvePhase(Record.TurnBackPhase);
	Result.bTurnBackRunOut = Record.TurnBackPhase == EGGYGOTurnBackPhase::RunOut;
	Result.bEnded = Record.Closure == EGGYGORemoteMotionPoseClosure::Completed;
	if (Check == ERemoteMotionPoseSourceCheck::Awaiting || Accepted.Identity == EGGYGORemoteMotionPoseIdentity::None)
	{
		// 本端动画源资格合法未就绪：保留该段历史已认证事实，不提供 Sequence/时间/TimeDomain/Identity，
		// 不授予入场、新时间、本地执行或 Finished；也不是新的 NoSegment。
		// 原因优先级：查询时的当前事实（未发布、生命周期退役）优先；Binding 因每帧重验而清空时，
		// 采用重验记下的真实资格原因（含 ProviderContentPending）；待配对记录类原因不参与。
		EGGYGORemoteMotionPoseWaitReason ProviderWait = Check == ERemoteMotionPoseSourceCheck::Awaiting
			? Wait : EGGYGORemoteMotionPoseWaitReason::ProviderRetiring;
		if (!Accepted.Binding.IsValid() && ProviderWait == EGGYGORemoteMotionPoseWaitReason::ProviderRetiring
			&& IsProviderWaitReason(RemoteMotionPoseWaitReason))
			ProviderWait = RemoteMotionPoseWaitReason;
		Result.Status = EGGYGORemoteMotionPoseStatus::Awaiting;
		Result.WaitReason = ProviderWait;
		Result.bSegmentFactsRetained = true;
		Result.Diagnostic = IsProviderWaitReason(RemoteMotionPoseWaitReason) && !RemoteMotionPoseDiagnostic.IsEmpty()
			? RemoteMotionPoseDiagnostic : FString::Printf(TEXT("[Movement.RemoteMotionPose] %s"),
				Error.IsEmpty() ? TEXT("accepted sample source identity is re-verifying") : *Error);
		return Result;
	}
	Result.Identity = Accepted.Identity;
	Result.Sequence = Source->Sequence.Get();
	Result.CertifiedSequenceSeconds = Record.SequenceEndSeconds;
	Result.SequenceSeconds = Record.SequenceEndSeconds;
	Result.TimeDomain = EGGYGORemoteMotionPoseTimeDomain::AuthorityCompletedEndpoint;
	const bool bCurrentSample = Accepted.MatchedEventSerial != 0
		&& Accepted.MatchedEventSerial == RemoteMotionPoseNativeSample.EventSerial;
	if (!bCurrentSample)
	{
		Result.Status = EGGYGORemoteMotionPoseStatus::Held;
		if (Result.WaitReason == EGGYGORemoteMotionPoseWaitReason::None)
			Result.WaitReason = EGGYGORemoteMotionPoseWaitReason::NativeSample;
		return Result;
	}
	Result.Status = EGGYGORemoteMotionPoseStatus::Current;
	// 原生 Linear 平滑把 Mesh 显示坐标 SmoothingClientTimeStamp 放在 server 样本键同一时域，并朝当前目标样本插值。
	// 成立条件：同 Epoch（时间戳来源未变）、同段、两样本都已真实配对、键不同且递增、终点不倒退，且原生目标正是本样本。
	// 同键多区间只替换本样本终点，不形成新的插值区间；跨段与 Held 不插值。
	const FNetworkPredictionData_Client_Character* ClientData = NetworkSmoothingMode == ENetworkSmoothingMode::Linear
		&& HasPredictionData_Client() ? GetPredictionData_Client_Character() : nullptr;
	const FRemoteMotionPoseAccepted* Previous = RemoteMotionPosePrevious.GetPtrOrNull();
	if (ClientData && Previous && ClientData->LastCorrectionTime != 0.0f
		&& ClientData->SmoothingServerTimeStamp == static_cast<double>(Record.NativeSampleKey)
		&& Previous->Record.Kind == EGGYGORemoteMotionPoseKind::Sample
		&& Previous->Record.PublisherEpoch == Record.PublisherEpoch && Previous->Record.SegmentSerial == Record.SegmentSerial
		&& Previous->Identity == Accepted.Identity
		&& Previous->Record.NativeSampleKey < Record.NativeSampleKey
		&& Previous->Record.SequenceEndSeconds <= Record.SequenceEndSeconds)
	{
		const double PreviousKey = static_cast<double>(Previous->Record.NativeSampleKey);
		const double Span = static_cast<double>(Record.NativeSampleKey) - PreviousKey;
		const double Alpha = FMath::Clamp((ClientData->SmoothingClientTimeStamp - PreviousKey) / Span, 0.0, 1.0);
		if (FMath::IsFinite(Alpha))
		{
			Result.SequenceSeconds = FMath::Lerp(Previous->Record.SequenceEndSeconds, Record.SequenceEndSeconds, Alpha);
			Result.TimeDomain = EGGYGORemoteMotionPoseTimeDomain::NativeLinearDisplay;
		}
	}
	return Result;
}
