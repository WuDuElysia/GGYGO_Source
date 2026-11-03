/**
 * @file GGYGOAnimInstanceBase.cpp
 * @brief 项目通用动画实例基类实现
 */
#include "Animation/Runtime/GGYGOAnimInstanceBase.h"

#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOAnimationSource, Log, All);

void UGGYGOAnimInstanceBase::GetNativePoseEvaluationFacts(int16& OutUpdateCounter,
	int16& OutEvaluationCounter, bool& bOutExtractRootMotion) const
{
	check(IsInGameThread());
	const FAnimInstanceProxy& Proxy = GetProxyOnGameThread<FAnimInstanceProxy>();
	OutUpdateCounter = Proxy.GetUpdateCounter().Get();
	OutEvaluationCounter = Proxy.GetEvaluationCounter().Get();
	bOutExtractRootMotion = Proxy.ShouldExtractRootMotion();
}

void UGGYGOAnimInstanceBase::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	ResetAnimationLifecycleState();

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	CharacterOwner = CurrentOwner;
	bHadCharacterOwner = CurrentOwner != nullptr;
	RefreshLocomotionSourceBinding();
	RefreshAnimationStateFrame();
}

void UGGYGOAnimInstanceBase::NativeUninitializeAnimation()
{
	ResetAnimationLifecycleState();
	Super::NativeUninitializeAnimation();
}

void UGGYGOAnimInstanceBase::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	ACharacter* CurrentOwner = Cast<ACharacter>(TryGetPawnOwner());
	const bool bPreviousOwnerExpired = bHadCharacterOwner && !CharacterOwner.IsValid();
	if (CharacterOwner.Get() != CurrentOwner || bPreviousOwnerExpired)
	{
		ResetAnimationLifecycleState();
		CharacterOwner = CurrentOwner;
		bHadCharacterOwner = CurrentOwner != nullptr;
	}

	RefreshLocomotionSourceBinding();
	RefreshAnimationStateFrame();
}

bool UGGYGOAnimInstanceBase::HasAnimationStateTag(FGameplayTag Tag) const
{
	return Tag.IsValid() && AnimationState.OwnedStateTags.HasTag(Tag);
}

bool UGGYGOAnimInstanceBase::IsAnimationRunGait() const
{
	return AnimationState.Gait == EGGYGOGait::Run;
}

bool UGGYGOAnimInstanceBase::IsTurnBackCurveDriven() const
{
	return AnimationState.TurnBackPhase == EGGYGOTurnBackPhase::Turning
		|| AnimationState.TurnBackPhase == EGGYGOTurnBackPhase::Braking;
}

bool UGGYGOAnimInstanceBase::TryGetMotionPose(EGGYGOLocomotionMotionType MotionType,
	UAnimSequence*& Sequence, float& SequenceSeconds, bool& bEnded) const
{
	if (RemoteMotionPose.bRemote)
	{
		// 远端观察端的姿态源只有权威复制记录；listen server 上本机完成区间是执行事实，不作显示姿态。
		return RemoteMotionPose.TryGetPose(MotionType, Sequence, SequenceSeconds, bEnded);
	}
	Sequence = nullptr;
	SequenceSeconds = 0.0f;
	bEnded = false;
	const FGGYGOAnimationMotionFrame& Motion = AnimationState.MotionExecution;
	if (Motion.Status != EGGYGOMotionExecutionStatus::Valid
		|| Motion.SourceKind != EGGYGOMotionExecutionSource::Locomotion || Motion.MotionType != MotionType
		|| !Motion.bHasSingleSequenceTime || !Motion.SingleSequence)
	{
		return false;
	}
	Sequence = Motion.SingleSequence;
	SequenceSeconds = static_cast<float>(Motion.SequenceEndSeconds);
	bEnded = Motion.bEnded;
	return true;
}

FGGYGOAnimationMotionPoseView UGGYGOAnimInstanceBase::QueryMotionPose(EGGYGOLocomotionMotionType MotionType) const
{
	FGGYGOAnimationMotionPoseView View;
	// Provider 身份已在同次 GT Capture 固定；图线程只复制弱身份，不回查 Actor/CMC。
	if (AnimationLifecycleGeneration == 0 || bSourceGenerationExhausted || RemotePosePreviousProvider.IsExplicitlyNull())
	{
		return View;
	}
	View.Instance = this;
	View.Provider = RemotePosePreviousProvider;
	View.LifecycleGeneration = AnimationLifecycleGeneration;
	View.bRemote = RemoteMotionPose.bRemote;
	View.bContextAvailable = true;

	UAnimSequence* Sequence = nullptr;
	float Seconds = 0.0f;
	bool bEnded = false;
	const bool bHasPose = TryGetMotionPose(MotionType, Sequence, Seconds, bEnded);
	if (RemoteMotionPose.bRemote)
	{
		if (RemoteMotionPose.MotionType != MotionType || (!bHasPose && !RemoteMotionPose.bRetainedWait)
			|| RemoteMotionPose.PublisherEpoch == 0 || RemoteMotionPose.SegmentSerial == 0)
		{
			return View;
		}
		View.bHasSource = true;
		View.MotionType = MotionType;
		View.PublisherEpoch = RemoteMotionPose.PublisherEpoch;
		View.SegmentSerial = RemoteMotionPose.SegmentSerial;
		View.bRetainedWait = RemoteMotionPose.bRetainedWait;
		View.bCanEnter = bHasPose && RemoteMotionPose.IsAdmissible(MotionType);
		View.bEnded = RemoteMotionPose.bEnded;
		View.TurnBackPhase = RemoteMotionPose.TurnBackPhase;
	}
	else
	{
		const FGGYGOAnimationMotionFrame& Motion = AnimationState.MotionExecution;
		if (!bHasPose || Motion.ExecutionRequestSerial == 0 || Motion.CompletedIntervalSerial == 0)
		{
			return View;
		}
		View.bHasSource = true;
		View.MotionType = MotionType;
		View.ExecutionRequestSerial = Motion.ExecutionRequestSerial;
		View.MotionSequence = Motion.MotionSequence;
		View.bCanEnter = !bEnded;
		View.bEnded = bEnded;
		View.TurnBackPhase = Motion.TurnBackPhase;
	}
	if (bHasPose)
	{
		View.bHasPose = true;
		View.Sequence = Sequence;
		View.SequenceSeconds = Seconds;
	}
	return View;
}

bool UGGYGOAnimInstanceBase::AreMotionPoseContextsEqual(
	const FGGYGOAnimationMotionPoseView& A, const FGGYGOAnimationMotionPoseView& B)
{
	return A.LifecycleGeneration != 0 && B.LifecycleGeneration != 0
		&& !A.Instance.IsExplicitlyNull() && !A.Provider.IsExplicitlyNull()
		&& A.Instance.HasSameIndexAndSerialNumber(B.Instance) && A.Provider.HasSameIndexAndSerialNumber(B.Provider)
		&& A.LifecycleGeneration == B.LifecycleGeneration && A.bRemote == B.bRemote;
}

bool UGGYGOAnimInstanceBase::AreMotionPoseSourcesEqual(
	const FGGYGOAnimationMotionPoseView& A, const FGGYGOAnimationMotionPoseView& B)
{
	if (!A.bHasSource || !B.bHasSource || !AreMotionPoseContextsEqual(A, B) || A.MotionType != B.MotionType)
	{
		return false;
	}
	return A.bRemote ? (A.PublisherEpoch == B.PublisherEpoch && A.SegmentSerial == B.SegmentSerial)
		: (A.ExecutionRequestSerial == B.ExecutionRequestSerial && A.MotionSequence == B.MotionSequence);
}

bool UGGYGOAnimInstanceBase::IsMotionPoseEnded(EGGYGOLocomotionMotionType MotionType) const
{
	if (RemoteMotionPose.bRemote)
	{
		return RemoteMotionPose.HasEndedFact(MotionType);
	}
	UAnimSequence* Sequence = nullptr;
	float SequenceSeconds = 0.0f;
	bool bEnded = false;
	return TryGetMotionPose(MotionType, Sequence, SequenceSeconds, bEnded) && bEnded;
}

bool UGGYGOAnimInstanceBase::IsMotionPoseActive(EGGYGOLocomotionMotionType MotionType) const
{
	if (RemoteMotionPose.bRemote)
	{
		return RemoteMotionPose.IsAdmissible(MotionType);
	}
	UAnimSequence* Sequence = nullptr;
	float SequenceSeconds = 0.0f;
	bool bEnded = false;
	return TryGetMotionPose(MotionType, Sequence, SequenceSeconds, bEnded) && !bEnded;
}

void UGGYGOAnimInstanceBase::OnAnimationLifecycleReset()
{
}

FGGYGORemoteMotionPoseSnapshot UGGYGOAnimInstanceBase::CaptureRemoteMotionPoseSnapshot(
	const UGGYGOCharacterMovementComponent& Movement) const
{
	return Movement.GetRemoteMotionPoseSnapshot();
}

void UGGYGOAnimInstanceBase::ResolveLocomotionSourceBinding(FGGYGOLocomotionSourceBinding& OutBinding) const
{
	OutBinding = {};
	OutBinding.Status = EGGYGOLocomotionSourceStatus::Missing;
	OutBinding.Error = FString::Printf(TEXT("[Animation][LocomotionSource] Producer=%s has no configured source resolver"), *GetPathName());
}

bool UGGYGOAnimInstanceBase::IsLocomotionSourceConfigurationCurrent(const FGGYGOLocomotionSourceBinding& Binding) const
{
	return false;
}

void UGGYGOAnimInstanceBase::ResetAnimationLifecycleState()
{
	RetireLocomotionSourceBinding();
	if (AnimationLifecycleGeneration == MAX_uint64)
	{
		if (!bSourceGenerationExhausted)
		{
			UE_LOG(LogGGYGOAnimationSource, Error,
				TEXT("[Animation][LocomotionSource] Producer=%s lifecycle generation exhausted"), *GetPathName());
		}
		bSourceGenerationExhausted = true;
	}
	else
	{
		++AnimationLifecycleGeneration;
	}
	CharacterOwner.Reset();
	bHadCharacterOwner = false;
	AnimationState = {};
	AnimationDebug = {};
	// 旧生命周期的远端姿态、消费记录与已进入段一并退役；新 Owner 不能继承旧 Held。
	// 发布号计数不复位，旧记录不可能匹配新生命周期的帧。
	RemoteMotionPose = {};
	RemotePoseContinuity.Reset();
	RemotePoseWitness = {};
	RemotePosePreviousProvider.Reset();
	bHasRemotePoseUpdateCounter = false;
	RemotePoseLastUpdateCounter = 0;
	LastLoggedRemotePoseDiagnostic.Reset();
	bLoggedInvalidPoseConsumer = false;
	OnAnimationLifecycleReset();
}

void UGGYGOAnimInstanceBase::RefreshAnimationStateFrame()
{
	StateCapture.Capture(AnimationState, AnimationDebug, CharacterOwner.Get());
	CaptureRemoteMotionPose();
}

void UGGYGOAnimInstanceBase::CaptureRemoteMotionPose()
{
	check(IsInGameThread());
	// 上一轮并行图更新已在本次 NativeUpdate 前完成；只据它的真实节点消费记录确认已进入段。
	int16 UpdateCounter = 0;
	int16 EvaluationCounter = 0;
	bool bExtractRootMotion = false;
	GetNativePoseEvaluationFacts(UpdateCounter, EvaluationCounter, bExtractRootMotion);
	const bool bGraphUpdated = bHasRemotePoseUpdateCounter && UpdateCounter != RemotePoseLastUpdateCounter;
	GGYGOAnimationRemotePose::InheritConsumption(bGraphUpdated, RemoteMotionPose.PublicationToken,
		RemotePosePreviousProvider.Get(), RemotePoseWitness, RemotePoseContinuity);
	RemotePoseWitness = {};
	RemotePoseLastUpdateCounter = UpdateCounter;
	bHasRemotePoseUpdateCounter = true;

	const ACharacter* Character = CharacterOwner.Get();
	const UGGYGOCharacterMovementComponent* Movement = Character
		? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	RemotePosePreviousProvider = Movement;
	if (!Movement)
	{
		// 缺少项目 CMC 时 StateCapture 已发布明确失败；这里只确保不留下旧远端事实。
		RemoteMotionPose = {};
		RemotePoseContinuity.Reset();
		return;
	}
	GGYGOAnimationRemotePose::Resolve(CaptureRemoteMotionPoseSnapshot(*Movement), Movement, ++RemotePoseTokenCounter,
		RemotePoseContinuity, RemoteMotionPose);
	if (!RemoteMotionPose.bRemote)
	{
		LastLoggedRemotePoseDiagnostic.Reset();
		return;
	}
	// 相位须与姿态出自同一条已接受记录：listen server 上 CMC 实时相位是执行值，模拟代理也不得混用
	// 未经真实节点消费进入的 Held。无姿态时为 None，由图按原取消/等待规则处理，不发布 Finished。
	// Provider 资格保留等待沿用该段保留的认证相位，等待不被当作取消或完成而混回 WalkRun。
	const bool bSegmentFacts = RemoteMotionPose.bHasPose || RemoteMotionPose.bRetainedWait;
	AnimationState.TurnBackPhase = bSegmentFacts ? RemoteMotionPose.TurnBackPhase : EGGYGOTurnBackPhase::None;
	AnimationState.bTurnBackRunOut = bSegmentFacts && RemoteMotionPose.bTurnBackRunOut;
	const bool bInvalid = RemoteMotionPose.Status == static_cast<uint8>(EGGYGORemoteMotionPoseStatus::Invalid);
	if (!bInvalid && !RemoteMotionPose.bConsumerRejected)
	{
		LastLoggedRemotePoseDiagnostic.Reset();
	}
	else if (LastLoggedRemotePoseDiagnostic != RemoteMotionPose.Diagnostic)
	{
		// Provider Invalid 是失败；本端拒绝无进入证据的 Held 属于合法网络时序，只记录。
		if (bInvalid)
		{
			UE_LOG(LogGGYGOAnimationSource, Warning, TEXT("[Animation][RemotePose] Instance=%s Character=%s Status=%d Epoch=%u Segment=%u Reason=%s"),
				*GetPathName(), *GetPathNameSafe(Character), RemoteMotionPose.Status, RemoteMotionPose.PublisherEpoch,
				RemoteMotionPose.SegmentSerial, *RemoteMotionPose.Diagnostic);
		}
		else
		{
			UE_LOG(LogGGYGOAnimationSource, Log, TEXT("[Animation][RemotePose] Instance=%s Character=%s Status=%d Epoch=%u Segment=%u Reason=%s"),
				*GetPathName(), *GetPathNameSafe(Character), RemoteMotionPose.Status, RemoteMotionPose.PublisherEpoch,
				RemoteMotionPose.SegmentSerial, *RemoteMotionPose.Diagnostic);
		}
		LastLoggedRemotePoseDiagnostic = RemoteMotionPose.Diagnostic;
	}
}

bool UGGYGOAnimInstanceBase::ApplyMotionPoseToEvaluator(const FAnimUpdateContext& UpdateContext,
	const FSequenceEvaluatorReference& Evaluator, EGGYGOLocomotionMotionType MotionType)
{
	// 消费证据只来自本实例图更新中实际被更新、并由本函数写入姿态的 SequenceEvaluator。
	const FAnimationUpdateContext* NativeContext = UpdateContext.GetContext();
	// 节点类型由原生 ConvertToSequenceEvaluator 保证；这里只取导出的基类地址作身份。
	const FAnimNode_Base* Node = Evaluator.GetAnimNodePtr<FAnimNode_Base>();
	const auto RejectConsumer = [this](const TCHAR* Reason)
	{
		if (!bLoggedInvalidPoseConsumer)
		{
			UE_LOG(LogGGYGOAnimationSource, Error, TEXT("[Animation][RemotePose] Instance=%s ApplyMotionPoseToEvaluator: %s"),
				*GetPathName(), Reason);
			bLoggedInvalidPoseConsumer = true;
		}
		return false;
	};
	if (!NativeContext || !NativeContext->AnimInstanceProxy
		|| NativeContext->AnimInstanceProxy->GetAnimInstanceObject() != this || !Node)
	{
		return RejectConsumer(TEXT("requires this instance's update context and a converted SequenceEvaluator node"));
	}
	UAnimSequence* Sequence = nullptr;
	float SequenceSeconds = 0.0f;
	bool bEnded = false;
	// 节点地址只作本实例内的身份，不解引用、不跨生命周期保存为指针。
	const uint64 ConsumerNode = static_cast<uint64>(reinterpret_cast<UPTRINT>(Node));
	if (RemoteMotionPose.bRemote && RemoteMotionPose.IsRetainedWaitConsumer(MotionType, ConsumerNode))
	{
		// Provider 资格合法等待：原进入节点被真实更新，保持其原生输出姿态，不调用 setter；只记录参与。
		GGYGOAnimationRemotePose::RecordConsumption(RemoteMotionPose, ConsumerNode, RemotePoseWitness);
		return false;
	}
	const bool bHasPose = RemoteMotionPose.bRemote
		? RemoteMotionPose.TryGetPoseForConsumer(MotionType, ConsumerNode, Sequence, SequenceSeconds, bEnded)
		: TryGetMotionPose(MotionType, Sequence, SequenceSeconds, bEnded);
	if (!bHasPose)
	{
		// 无可用姿态时不写节点：既不写 null/0，也不换另一动作或时刻。
		return false;
	}
	USequenceEvaluatorLibrary::SetSequence(Evaluator, Sequence);
	USequenceEvaluatorLibrary::SetExplicitTime(Evaluator, SequenceSeconds);
	// 原生读回确认该节点确实是接受了本姿态的 SequenceEvaluator，才算真实消费。
	if (USequenceEvaluatorLibrary::GetSequence(Evaluator) != Sequence)
	{
		return RejectConsumer(TEXT("the node did not accept the pose sequence; it is not a SequenceEvaluator"));
	}
	if (RemoteMotionPose.bRemote)
	{
		GGYGOAnimationRemotePose::RecordConsumption(RemoteMotionPose, ConsumerNode, RemotePoseWitness);
	}
	return true;
}

void UGGYGOAnimInstanceBase::RetireLocomotionSourceBinding()
{
	check(IsInGameThread());
	if (PublishedLocomotionSource)
	{
		if (UGGYGOCharacterMovementComponent* Consumer = LocomotionSourceConsumer.Get())
		{
			Consumer->RetireLocomotionSourceBinding(this, PublishedLocomotionSource->Identity.ConfigurationGeneration);
		}
	}
	PublishedLocomotionSource.Reset();
	LocomotionSourceConsumer.Reset();
}

void UGGYGOAnimInstanceBase::RefreshLocomotionSourceBinding()
{
	check(IsInGameThread());
	ACharacter* Character = CharacterOwner.Get();
	USkeletalMeshComponent* Mesh = GetSkelMeshComponent();
	UGGYGOCharacterMovementComponent* Consumer = Character
		? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	// Preview instances, post-process instances and detached/replaced producers cannot publish.
	if (!Character || !Consumer || !Mesh || Character->GetMesh() != Mesh || Mesh->GetAnimInstance() != this)
	{
		RetireLocomotionSourceBinding();
		return;
	}
	if (bSourceGenerationExhausted)
	{
		return;
	}

	const bool bSameRecipient = PublishedLocomotionSource && LocomotionSourceConsumer.Get() == Consumer
		&& PublishedLocomotionSource->Identity.Character.Get() == Character
		&& PublishedLocomotionSource->Identity.Mesh.Get() == Mesh
		&& PublishedLocomotionSource->Identity.LifecycleGeneration == AnimationLifecycleGeneration;
	if (bSameRecipient && IsLocomotionSourceConfigurationCurrent(*PublishedLocomotionSource))
	{
		return;
	}

	FGGYGOLocomotionSourceBinding Candidate;
	ResolveLocomotionSourceBinding(Candidate);
	if (bSameRecipient && PublishedLocomotionSource->HasSameConfiguration(Candidate))
	{
		return;
	}

	RetireLocomotionSourceBinding();
	if (bSourceGenerationExhausted || LocomotionSourceConfigurationGeneration == MAX_uint64)
	{
		// Exhaustion is permanent for this producer; no wraparound or borrowed successor identity.
		if (!bSourceGenerationExhausted)
		{
			UE_LOG(LogGGYGOAnimationSource, Error,
				TEXT("[Animation][LocomotionSource] Producer=%s Character=%s Mesh=%s configuration generation exhausted"),
				*GetPathName(), *GetPathNameSafe(Character), *GetPathNameSafe(Mesh));
		}
		bSourceGenerationExhausted = true;
		return;
	}
	Candidate.Identity.Producer = this;
	Candidate.Identity.Character = Character;
	Candidate.Identity.Mesh = Mesh;
	Candidate.Identity.LifecycleGeneration = AnimationLifecycleGeneration;
	Candidate.Identity.ConfigurationGeneration = ++LocomotionSourceConfigurationGeneration;
	PublishedLocomotionSource = MakeShared<FGGYGOLocomotionSourceBinding, ESPMode::ThreadSafe>(MoveTemp(Candidate));
	LocomotionSourceConsumer = Consumer;

	FString PublishError;
	const bool bAccepted = Consumer->PublishLocomotionSourceBinding(*PublishedLocomotionSource, PublishError);
	// Receipt acknowledges identity/configuration transport, including stored Missing/Invalid states.
	// Movement rejects those states when curve execution is required; Fixed/GA modes need no source.
	if (!bAccepted)
	{
		UE_LOG(LogGGYGOAnimationSource, Error,
			TEXT("[Animation][LocomotionSource] Producer=%s Character=%s Mesh=%s Generation=%llu Status=%d SourceError=%s PublishError=%s"),
			*GetPathName(), *GetPathNameSafe(Character), *GetPathNameSafe(Mesh),
			PublishedLocomotionSource->Identity.ConfigurationGeneration,
			static_cast<uint8>(PublishedLocomotionSource->Status), *PublishedLocomotionSource->Error, *PublishError);
	}
	// A rejected publication is diagnosed once. Re-publication requires an actual configuration/lifecycle change.
}
