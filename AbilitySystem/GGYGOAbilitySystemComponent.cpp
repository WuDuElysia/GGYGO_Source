/**
 * @file GGYGOAbilitySystemComponent.cpp
 * @brief 项目 ASC 实现
 */
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilityTagRelationshipMapping.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupConfig.h"
#include "AbilitySystem/Private/GGYGOAbilityInputBuffer.h"
#include "AbilitySystem/Private/GGYGOAbilityStepInputRouter.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "System/GGYGOGameplayTags.h"
#include "Input/GGYGOPlayerInput.h"

#include "Animation/AnimInstance.h"
#include "CoreGlobals.h"
#include "GameFramework/MovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameplayEffect.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/Pawn.h"
#include "Templates/UnrealTemplate.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySystemComponent)

UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Gameplay_AbilityInputBlocked, "Gameplay.AbilityInputBlocked");

UGGYGOAbilitySystemComponent::UGGYGOAbilitySystemComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, InputBuffer(MakeUnique<FGGYGOAbilityInputBuffer>(*this))
	, StepInputRouter(MakeUnique<FGGYGOAbilityStepInputRouter>(*this))
{

	ActiveAbilitiesByGroup.Reset();
}

void UGGYGOAbilitySystemComponent::InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor)
{
	check(IsInGameThread());
	FGameplayAbilityActorInfo* ActorInfo = AbilityActorInfo.Get();
	check(ActorInfo);
	const bool bHasNewPawnAvatar = Cast<APawn>(InAvatarActor) && (InAvatarActor != ActorInfo->AvatarActor);
	// 输入请求不保存 Owner/Avatar 快照；任一端点变化都清空并递增 Revision，是请求失效的唯一方式。
	if (InAvatarActor != ActorInfo->AvatarActor || InOwnerActor != ActorInfo->OwnerActor)
	{
		ClearAbilityInput();
	}
	Super::InitAbilityActorInfo(InOwnerActor, InAvatarActor);

	if (bHasNewPawnAvatar)
	{
		// 通知已有能力实例重新绑定 Avatar 相关引用。
		for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
		{
			// 回放等场景下可能没有实例，空数组是正常情况。
			TArray<UGameplayAbility*> Instances = AbilitySpec.GetAbilityInstances();
			for (UGameplayAbility* AbilityInstance : Instances)
			{
				if (UGGYGOGameplayAbility* GGYGOAbilityInstance = Cast<UGGYGOGameplayAbility>(AbilityInstance))
				{
					GGYGOAbilityInstance->OnPawnAvatarSet();
				}
			}
		}

		// Lyra 在这里还会注册 GlobalAbilitySystem（队伍范围的 Buff 广播）
		// 并把 AnimInstance 与 ASC 关联，让动画层能直接查询 Tag。
		// 两者在本项目都尚未实现，等它们就位后在此处补上注册。

		// 放在最后：确保 ActorInfo 已完整、能力实例已收到 Avatar 通知。
		TryActivateAbilitiesOnSpawn();
	}
}

void UGGYGOAbilitySystemComponent::ClearActorInfo()
{
	check(IsInGameThread());
	ClearAbilityInput();
	Super::ClearActorInfo();
}

UGGYGOAbilitySystemComponent::~UGGYGOAbilitySystemComponent() = default;

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
	const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline)
{
	return InputBuffer->ReceiveAbilityInputRequest(InputTag, PreviousIdentity, OriginalDeadline);
}

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
	const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline,
	const FGGYGOMovementInputEvaluationToken& OriginalEvaluationToken, const FGGYGOAbilityInputSourceKey& Source)
{
	return InputBuffer->ReceiveAbilityInputRequest(InputTag, PreviousIdentity, OriginalDeadline,
		OriginalEvaluationToken, Source);
}

FGGYGOAbilityInputRequestResult UGGYGOAbilitySystemComponent::EndAbilityInputRequest(
	const FGGYGOAbilityInputRequestIdentity& Identity, EGGYGOAbilityInputRequestEndKind EndKind)
{
	return InputBuffer->EndAbilityInputRequest(Identity, EndKind);
}

void UGGYGOAbilitySystemComponent::EndAbilityInputRequestsFromSource(const FGGYGOAbilityInputSourceKey& Source)
{
	InputBuffer->EndAbilityInputRequestsFromSource(Source);
}

void UGGYGOAbilitySystemComponent::ProcessAbilityInput(float DeltaTime, bool bGamePaused)
{
	InputBuffer->ProcessAbilityInput(DeltaTime, bGamePaused);
}

void UGGYGOAbilitySystemComponent::ReleaseHeldInputForControlLoss()
{
	InputBuffer->ReleaseHeldInputForControlLoss();
}

bool UGGYGOAbilitySystemComponent::IsAbilityInputActivationAttempt(FGameplayAbilitySpecHandle Handle) const
{
	return InputBuffer->IsInputActivationAttempt(Handle);
}

void UGGYGOAbilitySystemComponent::ClearAbilityInput()
{
	InputBuffer->ClearAbilityInput();
}

FGGYGOAbilityInitialInputRoute UGGYGOAbilitySystemComponent::CaptureInitialInputRoute(
	UGGYGOGameplayAbility* Ability, const FGGYGOAbilityActivationHandle& Original, FString& OutError)
{
	return StepInputRouter->CaptureInitialInputRoute(Ability, Original, OutError);
}

FGGYGOAbilityStepInputRequest UGGYGOAbilitySystemComponent::CaptureStepInputRequest(
	UGGYGOAbilityTask_WaitComboInput* Task, UGGYGOGameplayAbility* Ability,
	EGGYGOAbilityStepInputKind Kind, int32 SourceStep, int32 RequestId, FString& OutError)
{
	return StepInputRouter->CaptureStepInputRequest(Task, Ability, Kind, SourceStep, RequestId, OutError);
}

bool UGGYGOAbilitySystemComponent::CheckStepInputRequestCurrent(
	const FGGYGOAbilityStepInputRequest& Request, FString& OutError) const
{
	return StepInputRouter->CheckStepInputRequestCurrent(Request, OutError);
}

bool UGGYGOAbilitySystemComponent::CheckStepInputRequestForTask(const FGGYGOAbilityStepInputRequest& Request,
	const UGameplayAbility* Ability, const FGGYGOAbilityActivationHandle& Activation, FString& OutError) const
{
	return StepInputRouter->CheckStepInputRequestForTask(Request, Ability, Activation, OutError);
}

bool UGGYGOAbilitySystemComponent::CommitStepInputFact(UGGYGOAbilityTask_WaitComboInput* Task,
	const FGGYGOAbilityStepInputRequest& Request, const FGGYGOMovementInputStepFact& Fact,
	const FGGYGOAbilityInitialInputRoute& Route, FString& OutError)
{
	return StepInputRouter->CommitStepInputFact(Task, Request, Fact, Route, OutError);
}

void UGGYGOAbilitySystemComponent::NotifyInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route)
{
	StepInputRouter->NotifyInitialInputUnavailable(Route);
}

bool UGGYGOAbilitySystemComponent::ConsumeInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route,
	const FGGYGOAbilityActivationHandle& Original)
{
	return StepInputRouter->ConsumeInitialInputUnavailable(Route, Original);
}

void UGGYGOAbilitySystemComponent::ServerSetReplicatedTargetData_Implementation(
	FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey,
	const FGameplayAbilityTargetDataHandle& TargetData, FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey)
{
	StepInputRouter->HandleServerTargetData(AbilityHandle, AbilityOriginalPredictionKey, TargetData,
		ApplicationTag, CurrentPredictionKey);
}

void UGGYGOAbilitySystemComponent::ServerSetReplicatedTargetDataCancelled_Implementation(
	FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey, FPredictionKey CurrentPredictionKey)
{
	StepInputRouter->HandleServerTargetDataCancelled(AbilityHandle, AbilityOriginalPredictionKey, CurrentPredictionKey);
}

void UGGYGOAbilitySystemComponent::ForwardNativeTargetData(FGameplayAbilitySpecHandle AbilityHandle,
	FPredictionKey AbilityOriginalPredictionKey, const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey)
{
	Super::ServerSetReplicatedTargetData_Implementation(AbilityHandle, AbilityOriginalPredictionKey,
		TargetData, ApplicationTag, CurrentPredictionKey);
}

void UGGYGOAbilitySystemComponent::ForwardNativeTargetDataCancelled(FGameplayAbilitySpecHandle AbilityHandle,
	FPredictionKey AbilityOriginalPredictionKey, FPredictionKey CurrentPredictionKey)
{
	Super::ServerSetReplicatedTargetDataCancelled_Implementation(AbilityHandle, AbilityOriginalPredictionKey,
		CurrentPredictionKey);
}

float UGGYGOAbilitySystemComponent::PlayMontage(UGameplayAbility* InAnimatingAbility,
	FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* NewAnimMontage, float InPlayRate,
	FName StartSectionName, float StartTimeSeconds)
{
	return PlayAbilityMontage(InAnimatingAbility, ActivationInfo, NewAnimMontage, InPlayRate,
		StartSectionName, StartTimeSeconds).Duration;
}

FGGYGOAbilityMontagePlaybackResult UGGYGOAbilitySystemComponent::PlayAbilityMontage(UGameplayAbility* Ability,
	FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* Montage, float PlayRate, FName StartSection,
	float StartTimeSeconds, TFunction<bool()> IsCallerContextCurrent)
{
	check(IsInGameThread());
	FGGYGOAbilityMontagePlaybackResult Result;
	if (!IsValid(Ability) || !Ability->IsInstantiated() || !IsValid(Montage)
		|| !FMath::IsFinite(PlayRate) || PlayRate <= 0.0f
		|| !FMath::IsFinite(StartTimeSeconds) || StartTimeSeconds < 0.0f)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[ASC Montage] Play rejected invalid request: ASC=%s Ability=%s Montage=%s Rate=%f Start=%f."),
			*GetPathName(), *GetPathNameSafe(Ability), *GetPathNameSafe(Montage), PlayRate, StartTimeSeconds);
		return Result;
	}
	UGGYGOMontageGuardAnimInstance* Guard = AbilityActorInfo.IsValid()
		? Cast<UGGYGOMontageGuardAnimInstance>(AbilityActorInfo->GetAnimInstance()) : nullptr;
	if (!Guard)
	{
		// 项目动画蓝图都继承 Guard；非 Guard 无法识别本次调用创建的实例，不回退原生播放。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[ASC Montage] Play rejected: AnimInstance [%s] is not a UGGYGOMontageGuardAnimInstance. ASC=%s Ability=%s Montage=%s."),
			AbilityActorInfo.IsValid() ? *GetPathNameSafe(AbilityActorInfo->GetAnimInstance()) : TEXT("None"),
			*GetPathName(), *GetPathNameSafe(Ability), *GetPathNameSafe(Montage));
		Result.Guard.Outcome = EGGYGOMontagePlayGuardOutcome::Unsupported;
		return Result;
	}
	const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Ability);
	const FGGYGOAbilityActivationHandle Activation = ProjectAbility
		? ProjectAbility->CaptureCurrentActivation() : FGGYGOAbilityActivationHandle{};
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const TWeakObjectPtr<UGameplayAbility> OriginalAbility(Ability);
	const TWeakObjectPtr<UAnimInstance> OriginalAnim(Guard);
	FGGYGOMontagePlayGuardRequest Request;
	Request.OriginalAnimInstance = OriginalAnim;
	Request.RequestedMontage = Montage;
	Request.CallerIdentity = OriginalASC;
	// Guard 在原生播放前后调用；播放期间的回调可能结束本次激活或替换 AnimInstance。
	Request.IsCallerContextCurrent = [OriginalASC, OriginalAbility, OriginalAnim, Activation,
		bProjectAbility = ProjectAbility != nullptr, Additional = MoveTemp(IsCallerContextCurrent)]()
	{
		if (Additional && !Additional()) { return false; }
		const UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get();
		const UGameplayAbility* LiveAbility = OriginalAbility.Get();
		if (!ASC || !LiveAbility || !ASC->AbilityActorInfo.IsValid()
			|| ASC->AbilityActorInfo->GetAnimInstance() != OriginalAnim.Get()) { return false; }
		const UGGYGOGameplayAbility* LiveProjectAbility = Cast<UGGYGOGameplayAbility>(LiveAbility);
		return bProjectAbility ? LiveProjectAbility && LiveProjectAbility->IsActivationCurrent(Activation)
			: LiveAbility->IsActive();
	};
	{
		FGGYGOMontagePlayGuardScope Scope(MoveTemp(Request));
		float CallerReturn = 0.0f;
		if (Scope.CanExecute())
		{
			// scope 覆盖完整的 GAS 写回：Local／GA CurrentMontage、段跳转、复制与预测绑定。
			CallerReturn = Super::PlayMontage(Ability, ActivationInfo, Montage, PlayRate, StartSection, StartTimeSeconds);
		}
		Scope.Complete(CallerReturn);
		Result.Guard = Scope.GetResult();
	}
	if (!OriginalASC.IsValid() || Result.Guard.Outcome != EGGYGOMontagePlayGuardOutcome::Accepted)
	{
		return Result;
	}
	Result.Duration = Result.Guard.CallerReturnValue;
	UGameplayAbility* LiveAbility = OriginalAbility.Get();
	const UGGYGOMontageGuardAnimInstance* LiveGuard = Cast<UGGYGOMontageGuardAnimInstance>(OriginalAnim.Get());
	if (!LiveAbility || !LiveGuard || Result.Guard.Identity.CreatedInstanceId == INDEX_NONE
		|| !LiveGuard->IsMontagePlayGuardIdentityCurrent(Result.Guard.Identity)
		|| LocalAnimMontageInfo.AnimatingAbility.Get() != LiveAbility
		|| LocalAnimMontageInfo.AnimMontage.Get() != Montage || LiveAbility->GetCurrentMontage() != Montage)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[ASC Montage] Play accepted by Guard but the GAS Local write does not belong to this call: ASC=%s Ability=%s Montage=%s Instance=%d."),
			*GetPathName(), *GetPathNameSafe(LiveAbility), *GetPathNameSafe(Montage), Result.Guard.Identity.CreatedInstanceId);
		return Result;
	}
	Result.Playback.Guard = Result.Guard.Identity;
	Result.Playback.Activation = Activation;
	Result.Playback.Ability = LiveAbility;
	Result.Playback.Montage = Montage;
	Result.Playback.LocalPlayInstanceId = LocalAnimMontageInfo.PlayInstanceId;
	return Result;
}

bool UGGYGOAbilitySystemComponent::IsPlaybackCurrent(const FGGYGOAbilityMontagePlayback& Playback) const
{
	check(IsInGameThread());
	const UGameplayAbility* Ability = Playback.Ability.Get();
	// Avatar 变化会重置 Local 播放信息，PlayInstanceId 随之从头计数；同一能力在新 AnimInstance 上
	// 播放同一资产时序号可能重合，因此还要求 Local 播放仍在本次播放所属的 AnimInstance 上。
	if (!Playback.HasPlayback() || !Ability || !AbilityActorInfo.IsValid()
		|| AbilityActorInfo->GetAnimInstance() != Playback.Guard.OriginalAnimInstance.Get()
		|| LocalAnimMontageInfo.AnimatingAbility.Get() != Ability
		|| LocalAnimMontageInfo.AnimMontage.Get() != Playback.Montage.Get()
		|| LocalAnimMontageInfo.PlayInstanceId != Playback.LocalPlayInstanceId)
	{
		return false;
	}
	const UGGYGOGameplayAbility* ProjectAbility = Cast<UGGYGOGameplayAbility>(Ability);
	return ProjectAbility ? ProjectAbility->IsActivationCurrent(Playback.Activation) : Ability->IsActive();
}

bool UGGYGOAbilitySystemComponent::ClearPlayback(const FGGYGOAbilityMontagePlayback& Playback)
{
	check(IsInGameThread());
	if (!IsPlaybackCurrent(Playback)) { return false; }
	UGameplayAbility* Ability = Playback.Ability.Get();
	// 基类 setter 只写字段；不进入派生业务钩子，避免钩子返回后误清后继的 Local 指针。
	Ability->UGameplayAbility::SetCurrentMontage(nullptr);
	LocalAnimMontageInfo.AnimatingAbility = nullptr;
	return true;
}

void UGGYGOAbilitySystemComponent::TryActivateAbilitiesOnSpawn()
{
	// 锁住列表：激活过程可能授予或移除能力，会让遍历中的容器失效。
	ABILITYLIST_SCOPE_LOCK();
	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		// 用 CDO 读配置，它只提供策略，不是运行时实例。
		if (const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(AbilitySpec.Ability))
		{
			AbilityCDO->TryActivateAbilityOnSpawn(AbilityActorInfo.Get(), AbilitySpec);
		}
	}
}

bool UGGYGOAbilitySystemComponent::CanActivateAbilityByHandle(
	FGameplayAbilitySpecHandle Handle, FGameplayTagContainer& OutFailureTags) const
{
	OutFailureTags.Reset();
	const FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	return Spec && Spec->Ability && AbilityActorInfo.IsValid() &&
		Spec->Ability->CanActivateAbility(
			Handle, AbilityActorInfo.Get(), nullptr, nullptr, &OutFailureTags);
}

void UGGYGOAbilitySystemComponent::CancelAbilitiesByFunc(TShouldCancelAbilityFunc ShouldCancelFunc, bool bReplicateCancelAbility)
{
	ABILITYLIST_SCOPE_LOCK();
	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		// 非项目能力不参与本取消流程。用 Cast 而非 CastChecked：
		// 第三方或引擎能力被授予到同一个 ASC 上是允许的，不该因此崩掉。
		const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(AbilitySpec.Ability);
		if (!AbilityCDO)
		{
			continue;
		}

		// 一个 Spec 可能有多个运行时实例，逐个判断。
		TArray<UGameplayAbility*> Instances = AbilitySpec.GetAbilityInstances();
		for (UGameplayAbility* AbilityInstance : Instances)
		{
			UGGYGOGameplayAbility* GGYGOAbilityInstance = Cast<UGGYGOGameplayAbility>(AbilityInstance);
			if (!GGYGOAbilityInstance || !GGYGOAbilityInstance->IsActive()
				|| GGYGOAbilityInstance->IsCurrentAbilityGroupAdmissionRejected()
				|| !ShouldCancelFunc(GGYGOAbilityInstance, AbilitySpec.Handle))
			{
				continue;
			}

			if (GGYGOAbilityInstance->CanBeCanceled())
			{
				if (GGYGOAbilityInstance->IsAbilityGroupAdmissionPending())
				{
					// Pending attempt 仍在 ActivateAbility 准入调用栈中。这里只记录拒绝，
					// 由对应 attempt 在自身安全点结束，避免同步重入误减另一实例计数。
					GGYGOAbilityInstance->RejectCurrentAbilityGroupAdmission();
				}
				else
				{
					GGYGOAbilityInstance->CancelAbility(AbilitySpec.Handle, AbilityActorInfo.Get(),
						GGYGOAbilityInstance->GetCurrentActivationInfo(), bReplicateCancelAbility);
				}
			}
			else
			{
				// 不强制打断，只记录。能力声明自己不可取消时强行取消会破坏它的状态机。
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("CancelAbilitiesByFunc: 无法取消能力 [%s]，它的 CanBeCanceled 为 false。"),
					*GGYGOAbilityInstance->GetName());
			}
		}
	}
}

void UGGYGOAbilitySystemComponent::ClientCorrectAbilityState_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
	FPredictionKey ActivationKey, const FGameplayAbilityTargetDataHandle& Correction)
{
	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(AbilityHandle);
	if (!Spec || !Spec->IsActive())
	{
		return;
	}

	// 同一 Spec 可能有多个实例（例如 InstancedPerExecution）；只把纠正交给当前仍活跃且
	// 激活预测键匹配的 GGYGO 实例。ASC 不解释 TargetData 的业务类型。
	for (UGameplayAbility* AbilityInstance : Spec->GetAbilityInstances())
	{
		UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(AbilityInstance);
		if (GGYGOAbility && GGYGOAbility->IsActive()
			&& GGYGOAbility->GetCurrentAbilitySpecHandle() == AbilityHandle
			&& GGYGOAbility->GetCurrentActivationInfo().GetActivationPredictionKey() == ActivationKey)
		{
			GGYGOAbility->ReceiveAbilityCorrection(Correction);
			return;
		}
	}
}


void UGGYGOAbilitySystemComponent::AbilitySpecInputPressed(FGameplayAbilitySpec& Spec)
{
	const FGGYGOAbilityStepInputRouter::FDispatch* OriginalDispatch = StepInputRouter->GetActiveDispatch();
	const bool bOriginalNativeEntry = OriginalDispatch && !OriginalDispatch->bInitial
		&& OriginalDispatch->Spec == Spec.Handle && !OriginalDispatch->bNativePressEntered;
	if (bOriginalNativeEntry) OriginalDispatch->bNativePressEntered = true;
	Super::AbilitySpecInputPressed(Spec);

	// 不用 bReplicateInputDirectly，改走 replicated event，
	// 这样 WaitInputPress 之类的 AbilityTask 才能收到。
	if (Spec.IsActive())
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		const UGameplayAbility* Instance = Spec.GetPrimaryInstance();
		// 必须用激活时的原始预测键，否则服务器无法把这个事件对应到正确的那次激活。
		FPredictionKey OriginalPredictionKey = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : Spec.ActivationInfo.GetActivationPredictionKey();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		// 原边沿第一次进入原生按下时，复制一份标为投递中，供 InputPressed 事件期间的步骤请求认领。
		TOptional<FGGYGOAbilityStepInputRouter::FDispatch> Delivery;
		TOptional<FGGYGOAbilityStepInputRouter::FScopedDispatch> DeliveryScope;
		if (bOriginalNativeEntry && OriginalDispatch == StepInputRouter->GetActiveDispatch() && !OriginalDispatch->bDelivering)
		{
			Delivery.Emplace(*OriginalDispatch);
			Delivery->bDelivering = true;
			DeliveryScope.Emplace(*StepInputRouter, Delivery.GetValue());
		}
		InvokeReplicatedEvent(EAbilityGenericReplicatedEvent::InputPressed, Spec.Handle, OriginalPredictionKey);
	}
}

void UGGYGOAbilitySystemComponent::AbilitySpecInputReleased(FGameplayAbilitySpec& Spec)
{
	Super::AbilitySpecInputReleased(Spec);

	if (Spec.IsActive())
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		const UGameplayAbility* Instance = Spec.GetPrimaryInstance();
		FPredictionKey OriginalPredictionKey = Instance ? Instance->GetCurrentActivationInfo().GetActivationPredictionKey() : Spec.ActivationInfo.GetActivationPredictionKey();
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		InvokeReplicatedEvent(EAbilityGenericReplicatedEvent::InputReleased, Spec.Handle, OriginalPredictionKey);
	}
}

const FGGYGOAbilityGroupRule& UGGYGOAbilitySystemComponent::ResolveGroupRule(FGameplayTag GroupTag) const
{
	// 未注入配置表时的兜底规则。static 保证能安全返回引用。
	// 取字段默认值（SingleInstance + 平手后来者胜），使"没配表"与
	// "配了表但没配这个组"两种情况行为一致。
	static const FGGYGOAbilityGroupRule FallbackRule;

	if (AbilityGroupConfig)
	{
		return AbilityGroupConfig->GetRuleForGroup(GroupTag);
	}

	return FallbackRule;
}

void UGGYGOAbilitySystemComponent::SetAbilityGroupConfig(const UGGYGOAbilityGroupConfig* InConfig)
{
	AbilityGroupConfig = InConfig;
}

int32 UGGYGOAbilitySystemComponent::GetActiveAbilityCountInGroup(FGameplayTag GroupTag) const
{
	const TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag);
	if (!Group)
	{
		return 0;
	}

	// 不能直接用 Num()：表里可能残留已被 GC 的弱引用（清理只在 Remove 时做）。
	int32 Count = 0;
	for (const TWeakObjectPtr<UGGYGOGameplayAbility>& Weak : *Group)
	{
		if (Weak.IsValid())
		{
			++Count;
		}
	}

	return Count;
}

bool UGGYGOAbilitySystemComponent::IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability) const
{
	EGGYGOAbilityGroupBlockReason UnusedReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
	return IsActivationBlockedByGroup(Ability, UnusedReason);
}

bool UGGYGOAbilitySystemComponent::IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability, EGGYGOAbilityGroupBlockReason& OutReason) const
{
	OutReason = EGGYGOAbilityGroupBlockReason::NotBlocked;

	if (!Ability)
	{
		return false;
	}

	const int32 RequestPriority = Ability->GetActivationPriority();

	// 第一步：全局 Exclusive 排斥。跨组也生效，用于死亡、被击倒、大招这类"世界静止"的能力。
	for (const TPair<FGameplayTag, TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>>& GroupPair : ActiveAbilitiesByGroup)
	{
		for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakActive : GroupPair.Value)
		{
			const UGGYGOGameplayAbility* Active = WeakActive.Get();
			if (!Active || Active == Ability || Active->IsCurrentAbilityGroupAdmissionRejected())
			{
				continue;
			}

			if (Active->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive && Active->GetActivationPriority() > RequestPriority)
			{
				OutReason = EGGYGOAbilityGroupBlockReason::ExclusiveActive;
#if WITH_DEV_AUTOMATION_TESTS
				const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("Admission blocker reason=ExclusiveActive self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu selfPriority=%d otherPriority=%d otherPending=%d otherRejected=%d"),
					*GetNameSafe(Ability), static_cast<const void*>(Ability),
					static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
					*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
					RequestPriority, Active->GetActivationPriority(), Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
					Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
				return true;
			}
		}
	}

	// GroupTag 为空表示不参与组仲裁。
	const FGameplayTag GroupTag = Ability->GetGroupTag();
	if (!GroupTag.IsValid())
	{
		return false;
	}

	// 第二步：同组冲突，按该组配置的规则判定。
	const FGGYGOAbilityGroupRule& Rule = ResolveGroupRule(GroupTag);
	if (Rule.Rule == EGGYGOAbilityGroupRule::Coexist)
	{
		// 组内无并发限制，连遍历都不必做。
		return false;
	}

	const TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag);
	if (!Group)
	{
		return false;
	}

	for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakActive : *Group)
	{
		const UGGYGOGameplayAbility* Active = WeakActive.Get();
		if (!Active || Active == Ability || Active->IsCurrentAbilityGroupAdmissionRejected())
		{
			continue;
		}

		if (Rule.Rule == EGGYGOAbilityGroupRule::SingleInstanceQueued)
		{
			// 严格先来后到：不比优先级，组里有人在跑就一律拒绝。
			// 配合 AddAbilityToActivationGroup 里"Queued 组不取消同组实例"，
			// 已激活的那一个必然能播完 —— 这是连段不被自己打断的保证。
			OutReason = EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued;
#if WITH_DEV_AUTOMATION_TESTS
			const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("Admission blocker reason=GroupOccupiedQueued self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu otherPending=%d otherRejected=%d"),
				*GetNameSafe(Ability), static_cast<const void*>(Ability),
				static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
				*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
				Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
				Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
			return true;
		}

		// SingleInstance：不可取消的旧实例保留槽位；其余情况按优先级比较。
		// bNewcomerWinsOnTie 为 true 时用 >（平手不阻断，放新的进来顶掉旧的，D4）；
		// 为 false 时用 >=（平手也阻断，先激活者守住位置，受击组要的是这个）。
		if (!Active->CanBeCanceled())
		{
			OutReason = EGGYGOAbilityGroupBlockReason::UncancelableActive;
#if WITH_DEV_AUTOMATION_TESTS
			const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("Admission blocker reason=UncancelableActive self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu selfPriority=%d otherPriority=%d otherPending=%d otherRejected=%d"),
				*GetNameSafe(Ability), static_cast<const void*>(Ability),
				static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
				*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
				RequestPriority, Active->GetActivationPriority(), Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
				Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
			return true;
		}

		const int32 ActivePriority = Active->GetActivationPriority();
		const bool bBlockedByPriority = Rule.bNewcomerWinsOnTie
			? (ActivePriority > RequestPriority)
			: (ActivePriority >= RequestPriority);

		if (bBlockedByPriority)
		{
			OutReason = EGGYGOAbilityGroupBlockReason::LowerPriority;
#if WITH_DEV_AUTOMATION_TESTS
			const uint64 ActiveSequence = Active->GetCurrentAbilityGroupAdmissionSequence();
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("Admission blocker reason=LowerPriority self=%s@%p selfSeq=%llu other=%s@%p otherSeq=%llu selfPriority=%d otherPriority=%d newcomerWinsOnTie=%d otherPending=%d otherRejected=%d"),
				*GetNameSafe(Ability), static_cast<const void*>(Ability),
				static_cast<unsigned long long>(Ability->GetCurrentAbilityGroupAdmissionSequence()),
				*GetNameSafe(Active), static_cast<const void*>(Active), static_cast<unsigned long long>(ActiveSequence),
				RequestPriority, ActivePriority, Rule.bNewcomerWinsOnTie ? 1 : 0,
				Active->IsAbilityGroupAdmissionPending() ? 1 : 0,
				Active->IsAbilityGroupAdmissionRejected(ActiveSequence) ? 1 : 0);
#endif
			return true;
		}
	}

	return false;
}

void UGGYGOAbilitySystemComponent::AddAbilityToActivationGroup(UGGYGOGameplayAbility* Ability)
{
	if (!Ability)
	{
		return;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	// PreActivate 只登记准入预留；竞争取消留给 ActiveCount 已递增后的 Finalize。
	if (GroupTag.IsValid())
	{
		ActiveAbilitiesByGroup.FindOrAdd(GroupTag).AddUnique(Ability);
	}
}

uint64 UGGYGOAbilitySystemComponent::AllocateAbilityGroupAdmissionSequence()
{
	// Do not wrap to zero or reuse a sequence while older attempts may still be on the stack.
	if (NextAbilityGroupAdmissionSequence == MAX_uint64)
	{
		ensureMsgf(false, TEXT("Ability group admission sequence exhausted on ASC [%s]."), *GetName());
		return 0;
	}

	return ++NextAbilityGroupAdmissionSequence;
}

bool UGGYGOAbilitySystemComponent::FinalizeAbilityGroupAdmission(UGGYGOGameplayAbility* Ability, uint64 AdmissionSequence)
{
	if (!Ability || !Ability->IsAbilityGroupAdmissionPending(AdmissionSequence)
		|| Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence))
	{
		return false;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	const int32 NewPriority = Ability->GetActivationPriority();

	// 复用组准入的取消阈值。SingleInstanceQueued 与 Coexist 不取消同组实例；
	// Exclusive 仍按既有规则取消所有更低优先级实例。
	const bool bCancelExclusiveTargets = Ability->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive;
	const FGGYGOAbilityGroupRule* Rule = GroupTag.IsValid() ? &ResolveGroupRule(GroupTag) : nullptr;
	const bool bCancelSameGroupTargets = Rule && Rule->Rule == EGGYGOAbilityGroupRule::SingleInstance;
	const bool bCancelTies = !Rule || Rule->bNewcomerWinsOnTie;
	auto ShouldCancelFunc = [Ability, AdmissionSequence, NewPriority, GroupTag, bCancelExclusiveTargets, bCancelSameGroupTargets, bCancelTies](
		const UGGYGOGameplayAbility* Other, FGameplayAbilitySpecHandle Handle)
	{
		// 新尝试可在同步取消回调中拒绝当前 attempt；旧裁决随即停止继续取消目标。
		if (Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence) || Other == Ability)
		{
			return false;
		}

		const bool bExclusiveTarget = bCancelExclusiveTargets && Other->GetActivationPriority() < NewPriority;
		const bool bSameGroupTarget = bCancelSameGroupTargets && Other->GetGroupTag() == GroupTag
			&& (bCancelTies ? Other->GetActivationPriority() <= NewPriority : Other->GetActivationPriority() < NewPriority);
		return bExclusiveTarget || bSameGroupTarget;
	};

	// Resolve conflicting pending attempts before touching settled abilities. Their order is
	// ASC-wide, so a nested newer request can reject this older attempt without the older
	// resolver later cancelling the newer instance when its callback stack resumes.
	if (bCancelExclusiveTargets || bCancelSameGroupTargets)
	{
		ABILITYLIST_SCOPE_LOCK();
		for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
		{
			if (!Cast<UGGYGOGameplayAbility>(AbilitySpec.Ability))
			{
				continue;
			}

			const TArray<UGameplayAbility*> Instances = AbilitySpec.GetAbilityInstances();
			for (UGameplayAbility* AbilityInstance : Instances)
			{
				UGGYGOGameplayAbility* Other = Cast<UGGYGOGameplayAbility>(AbilityInstance);
				if (!Other || Other == Ability || !Other->IsActive() || !Other->IsAbilityGroupAdmissionPending())
				{
					continue;
				}

				const uint64 OtherSequence = Other->GetCurrentAbilityGroupAdmissionSequence();
				if (OtherSequence == 0 || Other->IsAbilityGroupAdmissionRejected(OtherSequence)
					|| !ShouldCancelFunc(Other, AbilitySpec.Handle))
				{
					continue;
				}

				if (OtherSequence < AdmissionSequence)
				{
					Other->RejectAbilityGroupAdmission(OtherSequence);
				}
				else
				{
					// Equal sequences are an invariant violation; fail closed like a newer competitor.
					Ability->RejectAbilityGroupAdmission(AdmissionSequence);
					return false;
				}
			}
		}
	}

	if (bCancelExclusiveTargets || bCancelSameGroupTargets)
	{
		CancelAbilitiesByFunc(ShouldCancelFunc, /*bReplicateCancelAbility=*/true);
	}
	if (Ability->IsAbilityGroupAdmissionRejected(AdmissionSequence))
	{
		return false;
	}

	// CancelAbility 可能被 CanBeCanceled 拒绝，也可能因同步回调改变了占用。
	// 取消后检查仍活跃的竞争者，并复用同一组准入规则判断是否应拒绝当前尝试。
	if (HasActiveAbilityMatching(ShouldCancelFunc) || IsActivationBlockedByGroup(Ability))
	{
		Ability->RejectAbilityGroupAdmission(AdmissionSequence);
		return false;
	}

	return true;
}

bool UGGYGOAbilitySystemComponent::RemoveAbilityFromActivationGroup(UGGYGOGameplayAbility* Ability, bool bBroadcastGroupFreed)
{
	if (!Ability)
	{
		return false;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	if (!GroupTag.IsValid())
	{
		return false;
	}

	bool bGroupBecameEmpty = false;

	if (TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag))
	{
		Group->Remove(Ability);

		// 顺手清掉已经失效的弱引用，避免长期运行后表里堆积空项。
		Group->RemoveAll([](const TWeakObjectPtr<UGGYGOGameplayAbility>& Weak) { return !Weak.IsValid(); });

		if (Group->Num() == 0)
		{
			ActiveAbilitiesByGroup.Remove(GroupTag);
			bGroupBecameEmpty = true;
		}
	}

	if (bGroupBecameEmpty && bBroadcastGroupFreed)
	{
		BroadcastAbilityGroupFreedIfEmpty(GroupTag);
	}

	return bGroupBecameEmpty;
}

void UGGYGOAbilitySystemComponent::BroadcastAbilityGroupFreedIfEmpty(FGameplayTag GroupTag)
{
	if (!GroupTag.IsValid() || GetActiveAbilityCountInGroup(GroupTag) > 0)
	{
		return;
	}

	// 清掉弱引用空槽后再广播；同步订阅者可能立即激活并重新登记同组能力。
	ActiveAbilitiesByGroup.Remove(GroupTag);
	// Queued wait belongs to the original ASC request; listeners observe after work is queued.
	InputBuffer->WakeQueuedAbilityInputRequests();
	OnAbilityGroupFreed.Broadcast(GroupTag);
}

bool UGGYGOAbilitySystemComponent::HasActiveAbilityMatching(TShouldCancelAbilityFunc ShouldCancelFunc) const
{
	for (const TPair<FGameplayTag, TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>>& GroupPair : ActiveAbilitiesByGroup)
	{
		for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakAbility : GroupPair.Value)
		{
			const UGGYGOGameplayAbility* GGYGOAbility = WeakAbility.Get();
			if (!GGYGOAbility || !GGYGOAbility->IsActive() || GGYGOAbility->IsCurrentAbilityGroupAdmissionRejected())
			{
				continue;
			}

			const FGameplayAbilitySpecHandle AbilityHandle = GGYGOAbility->GetCurrentAbilitySpecHandle();
			if (ShouldCancelFunc(GGYGOAbility, AbilityHandle))
			{
#if WITH_DEV_AUTOMATION_TESTS
				const uint64 AdmissionSequence = GGYGOAbility->GetCurrentAbilityGroupAdmissionSequence();
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("Admission HasActiveAbilityMatching hit other=%s@%p sequence=%llu pending=%d rejected=%d spec=%s"),
					*GetNameSafe(GGYGOAbility), static_cast<const void*>(GGYGOAbility),
					static_cast<unsigned long long>(AdmissionSequence),
					GGYGOAbility->IsAbilityGroupAdmissionPending(AdmissionSequence) ? 1 : 0,
					GGYGOAbility->IsAbilityGroupAdmissionRejected(AdmissionSequence) ? 1 : 0,
					*AbilityHandle.ToString());
#endif
				return true;
			}
		}
	}

	return false;
}

void UGGYGOAbilitySystemComponent::NotifyAbilityActivated(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability)
{
	InputBuffer->NotifyAbilityActivated(Handle);
	if (UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(Ability))
	{
		// 原生 PreActivate 在 ActivateAbility 之前调用这里，是激活身份的唯一签发点。
		GGYGOAbility->IssueActivation(this, Handle);
		GGYGOAbility->BeginAbilityGroupAdmissionAttempt(AllocateAbilityGroupAdmissionSequence());
		AddAbilityToActivationGroup(GGYGOAbility);
	}

	// Register the reservation before GAS broadcasts activation; listeners can synchronously
	// request another ability while this Spec's ActiveCount is still zero.
	Super::NotifyAbilityActivated(Handle, Ability);
}

void UGGYGOAbilitySystemComponent::NotifyAbilityFailed(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(this);
	const TWeakObjectPtr<UGameplayAbility> FailedAbility(Ability);
	// Native GAS shares its failure-tag storage across nested activation attempts.
	const FGameplayTagContainer FailureTags = FailureReason;
	// 真实失败许可在原生观察者可能释放阻塞方之前登记；外部重试通知在 Super 之后重新核验。
	const FGGYGOAbilityInputBuffer::FQueuedFailure QueuedFailure =
		InputBuffer->ClaimQueuedFailure(Handle, FailureTags, FailedAbility);
	Super::NotifyAbilityFailed(Handle, Ability, FailureTags);
	if (!OriginalASC.IsValid())
	{
		return;
	}
	InputBuffer->BroadcastQueuedFailure(Handle, QueuedFailure, FailedAbility);

	if (!OriginalASC.IsValid() || !FailedAbility.IsValid())
	{
		return;
	}

	// Keep the existing local feedback and client RPC route; neither carries input authority.
	if (APawn* Avatar = Cast<APawn>(GetAvatarActor()))
	{
		if (!Avatar->IsLocallyControlled() && FailedAbility->IsSupportedForNetworking())
		{
			ClientNotifyAbilityFailed(FailedAbility.Get(), FailureTags);
			return;
		}
	}
	HandleAbilityFailed(FailedAbility.Get(), FailureTags);
}

void UGGYGOAbilitySystemComponent::NotifyAbilityEnded(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, bool bWasCancelled)
{
	UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(Ability);
	if (GGYGOAbility)
	{
		// 在 Super 广播 OnAbilityEnded 之前退役；广播内重新激活同一实例会签发新的身份。
		GGYGOAbility->RetireActivation();
	}
	const FGameplayTag GroupTag = GGYGOAbility ? GGYGOAbility->GetGroupTag() : FGameplayTag();
	const bool bRemovedLastAbilityInGroup = GGYGOAbility
		? RemoveAbilityFromActivationGroup(GGYGOAbility, /*bBroadcastGroupFreed=*/false)
		: false;

	// Super 减少 Spec.ActiveCount 并同步广播 OnAbilityEnded。回调可以重激活同一
	// 实例；旧登记已先移除，新的 Add 不会被本次结束覆盖。
	Super::NotifyAbilityEnded(Handle, Ability, bWasCancelled);

	if (bRemovedLastAbilityInGroup)
	{
		// 仅在 Super 回调后组仍为空才通知排队方，且此时旧 Spec 已完成 ActiveCount--。
		BroadcastAbilityGroupFreedIfEmpty(GroupTag);
	}
}

void UGGYGOAbilitySystemComponent::ApplyAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags, UGameplayAbility* RequestingAbility, bool bEnableBlockTags, const FGameplayTagContainer& BlockTags, bool bExecuteCancelTags, const FGameplayTagContainer& CancelTags)
{
	// 拷贝后再扩展，不能改调用方传进来的容器（那可能直接指向能力资产上的配置）。
	FGameplayTagContainer ModifiedBlockTags = BlockTags;
	FGameplayTagContainer ModifiedCancelTags = CancelTags;

	if (TagRelationshipMapping)
	{
		TagRelationshipMapping->GetAbilityTagsToBlockAndCancel(AbilityTags, &ModifiedBlockTags, &ModifiedCancelTags);
	}

	Super::ApplyAbilityBlockAndCancelTags(AbilityTags, RequestingAbility, bEnableBlockTags, ModifiedBlockTags, bExecuteCancelTags, ModifiedCancelTags);
}

void UGGYGOAbilitySystemComponent::GetAdditionalActivationTagRequirements(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer& OutActivationRequired, FGameplayTagContainer& OutActivationBlocked) const
{
	if (TagRelationshipMapping)
	{
		TagRelationshipMapping->GetRequiredAndBlockedActivationTags(AbilityTags, &OutActivationRequired, &OutActivationBlocked);
	}
}

void UGGYGOAbilitySystemComponent::SetTagRelationshipMapping(UGGYGOAbilityTagRelationshipMapping* NewMapping)
{
	// 不回溯修改已经应用的阻断状态，只影响之后的激活查询。
	TagRelationshipMapping = NewMapping;
}

void UGGYGOAbilitySystemComponent::ClientNotifyAbilityFailed_Implementation(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	HandleAbilityFailed(Ability, FailureReason);
}

void UGGYGOAbilitySystemComponent::HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	if (const UGGYGOGameplayAbility* GGYGOAbility = Cast<const UGGYGOGameplayAbility>(Ability))
	{
		GGYGOAbility->OnAbilityFailedToActivate(FailureReason);
	}
}

void UGGYGOAbilitySystemComponent::GetAbilityTargetData(const FGameplayAbilitySpecHandle AbilityHandle, FGameplayAbilityActivationInfo ActivationInfo, FGameplayAbilityTargetDataHandle& OutTargetDataHandle)
{
	// 键是 SpecHandle + 激活预测键的组合，这样同一能力的多次预测激活不会互相串数据。
	TSharedPtr<FAbilityReplicatedDataCache> ReplicatedData = AbilityTargetDataMap.Find(FGameplayAbilitySpecHandleAndPredictionKey(AbilityHandle, ActivationInfo.GetActivationPredictionKey()));
	if (ReplicatedData.IsValid())
	{
		OutTargetDataHandle = ReplicatedData->TargetData;
	}
	// 查不到时不动输出参数，保留调用方的原值。
}
