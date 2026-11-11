/**
 * @file GGYGOGameplayAbility.cpp
 * @brief GameplayAbility 基类实现
 */
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "AbilitySystem/Abilities/GGYGOAbilityCost.h"
#include "AbilitySystem/Abilities/GGYGOAbilityFailureMessages.h"
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "AbilitySystem/Private/GGYGOAbilityInputBuffer.h"
#include "Camera/GGYGOCameraComponent.h"
#include "Character/Components/GGYGOHeroComponent.h"
#include "Physics/GGYGOPhysicalMaterialWithTags.h"
#include "System/GGYGOGameplayTags.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemGlobals.h"
#include "Engine/HitResult.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayAbilitySpec.h"

#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayAbility)

// 失败反馈消息的两个通道 Tag 在此定义。
UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Ability_SimpleFailureMessage, "Ability.UserFacingSimpleActivateFail.Message");
UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Ability_PlayMontageFailureMessage, "Ability.PlayMontageOnActivateFail.Message");

namespace
{
void AppendPhysicalMaterialTags(const FHitResult& HitResult, FGameplayTagContainer& OutTags)
{
	if (const UGGYGOPhysicalMaterialWithTags* PhysMatWithTags =
		Cast<const UGGYGOPhysicalMaterialWithTags>(HitResult.PhysMaterial.Get()))
	{
		OutTags.AppendTags(PhysMatWithTags->Tags);
	}
}
}

/** 一次原生激活的不可变来源；不缓存 Active 或 Spec 计数。 */
struct FGGYGOAbilityActivationHandle::FActivationProof
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
	FGameplayAbilitySpecHandle SpecHandle;
	FPredictionKey ActivationKey;
	TWeakPtr<const FGameplayAbilityActorInfo> Allocation;
	uint64 Serial = 0;
};

void UGGYGOGameplayAbility::IssueActivation(UGGYGOAbilitySystemComponent* OwnerASC, FGameplayAbilitySpecHandle Handle)
{
	check(IsInGameThread());
	// 新的原生激活总是取代旧身份，旧激活遗留的回调随之失效。
	CurrentActivation = {};
	bEndingActivation = false;
	if (!IsInstantiated() || !OwnerASC || CurrentSpecHandle != Handle
		|| !OwnerASC->AbilityActorInfo.IsValid() || OwnerASC->AbilityActorInfo.Get() != CurrentActorInfo)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem activation [%s] Spec [%s] ASC [%s] not issued: native activation arguments do not match the ASC ActorInfo."),
			*GetPathName(), *Handle.ToString(), *GetPathNameSafe(OwnerASC));
		return;
	}
	TSharedRef<FGGYGOAbilityActivationHandle::FActivationProof> Proof =
		MakeShared<FGGYGOAbilityActivationHandle::FActivationProof>();
	Proof->ASC = OwnerASC;
	Proof->Ability = this;
	Proof->SpecHandle = Handle;
	Proof->ActivationKey = CurrentActivationInfo.GetActivationPredictionKey();
	Proof->Allocation = OwnerASC->AbilityActorInfo;
	Proof->Serial = ++LastActivationSerial;
	CurrentActivation.Proof = Proof;
}

void UGGYGOGameplayAbility::RetireActivation()
{
	CurrentActivation = {};
	bEndingActivation = false;
}

FGGYGOAbilityActivationHandle UGGYGOGameplayAbility::CaptureCurrentActivation() const
{
	return IsActivationCurrent(CurrentActivation) ? CurrentActivation : FGGYGOAbilityActivationHandle{};
}

bool UGGYGOGameplayAbility::IsActivationCurrent(const FGGYGOAbilityActivationHandle& Activation) const
{
	return Activation.HasSameActivation(CurrentActivation) && IsActive() && !bEndingActivation;
}

FGGYGOAbilityTerminationResult UGGYGOGameplayAbility::RequestAbilityEnd(
	const FGGYGOAbilityActivationHandle& Original, bool bReplicateEndAbility, bool bWasCancelled)
{
	check(IsInGameThread());
	FGGYGOAbilityTerminationResult Result;
	if (!IsActivationCurrent(Original)) { return Result; }
	if (ScopeLockCount > 0)
	{
		// 原生 ScopeLock 期间延后执行；延后的请求只结束发起时的这次激活，执行时已被后继取代则不作用。
		WaitingToExecute.Add(FPostLockDelegate::CreateWeakLambda(this, [this, Original, bReplicateEndAbility, bWasCancelled]()
		{
			RequestAbilityEnd(Original, bReplicateEndAbility, bWasCancelled);
		}));
	}
	else
	{
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, bReplicateEndAbility, bWasCancelled);
	}
	Result.Outcome = EGGYGOAbilityTerminationOutcome::Accepted;
	return Result;
}

FGGYGOAbilityTerminationResult UGGYGOGameplayAbility::RequestAbilityCancel(
	const FGGYGOAbilityActivationHandle& Original, bool bReplicateCancelAbility)
{
	check(IsInGameThread());
	FGGYGOAbilityTerminationResult Result;
	if (!IsActivationCurrent(Original)) { return Result; }
	if (!CanBeCanceled())
	{
		Result.Outcome = EGGYGOAbilityTerminationOutcome::NotCancelable;
		return Result;
	}
	if (ScopeLockCount > 0)
	{
		// 同 RequestAbilityEnd：延后的取消只作用于发起时的这次激活。
		WaitingToExecute.Add(FPostLockDelegate::CreateWeakLambda(this, [this, Original, bReplicateCancelAbility]()
		{
			RequestAbilityCancel(Original, bReplicateCancelAbility);
		}));
	}
	else
	{
		CancelAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, bReplicateCancelAbility);
	}
	Result.Outcome = EGGYGOAbilityTerminationOutcome::Accepted;
	return Result;
}

UGGYGOGameplayAbility::UGGYGOGameplayAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 不复制能力对象本身，规格与激活状态由 ASC 复制。
	ReplicationPolicy = EGameplayAbilityReplicationPolicy::ReplicateNo;

	// 每个 Actor 一个实例，运行期状态可以安全放在成员变量里。
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	bRetriggerInstancedAbility = false; // 真实原生 End 后可显式 Try，不启用引擎自动 retrigger。

	// 客户端先预测再由服务器确认。动作游戏的输入响应感依赖这一条。
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// 两端都能发起请求，具体校验交给 GAS 的网络验证流程。
	NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ClientOrServer;

	ActivationPolicy = EGGYGOAbilityActivationPolicy::OnInputTriggered;

	// 默认不参与组仲裁：GroupTag 为空 + Coexist + 最低优先级。
	// 派生能力必须显式配置这三项才会进入仲裁。
	ActivationPriority = GGYGOAbilityGroupDefaults::Priority_Passive;
	SelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
}

UGGYGOAbilitySystemComponent* UGGYGOGameplayAbility::GetGGYGOAbilitySystemComponentFromActorInfo() const
{
	return (CurrentActorInfo ? Cast<UGGYGOAbilitySystemComponent>(CurrentActorInfo->AbilitySystemComponent.Get()) : nullptr);
}

APlayerController* UGGYGOGameplayAbility::GetPlayerControllerFromActorInfo() const
{
	return (CurrentActorInfo ? Cast<APlayerController>(CurrentActorInfo->PlayerController.Get()) : nullptr);
}

AController* UGGYGOGameplayAbility::GetControllerFromActorInfo() const
{
	if (!CurrentActorInfo)
	{
		return nullptr;
	}

	// 最直接的来源，避免遍历。
	if (AController* PC = CurrentActorInfo->PlayerController.Get())
	{
		return PC;
	}

	// 两级 ASC 布局下角色 ASC 的 Owner 就是角色自己，ActorInfo 里没有 PlayerController，
	// 所以要沿 Owner 链找，最终靠 Pawn->GetController() 拿到控制器。
	AActor* TestActor = CurrentActorInfo->OwnerActor.Get();
	while (TestActor)
	{
		if (AController* C = Cast<AController>(TestActor))
		{
			return C;
		}

		if (APawn* Pawn = Cast<APawn>(TestActor))
		{
			return Pawn->GetController();
		}

		TestActor = TestActor->GetOwner();
	}

	return nullptr;
}

ACharacter* UGGYGOGameplayAbility::GetCharacterFromActorInfo() const
{
	return (CurrentActorInfo ? Cast<ACharacter>(CurrentActorInfo->AvatarActor.Get()) : nullptr);
}

void UGGYGOGameplayAbility::NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
{
	// 文本只发一条：同时缺耐力又在冷却时，弹两条提示反而更糟。
	bool bSimpleFailureFound = false;

	for (const FGameplayTag& Reason : FailedReason)
	{
		if (!bSimpleFailureFound)
		{
			if (const FText* UserFacingMessage = FailureTagToUserFacingMessages.Find(Reason))
			{
				FGGYGOAbilitySimpleFailureMessage Message;
				Message.PlayerController = GetActorInfo().PlayerController.Get();
				// 带上完整原因集合，接收方可以做更细的判断。
				Message.FailureTags = FailedReason;
				Message.UserFacingReason = *UserFacingMessage;

				UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(GetWorld());
				MessageSystem.BroadcastMessage(TAG_GGYGO_Ability_SimpleFailureMessage, Message);

				bSimpleFailureFound = true;
			}
		}

		// Montage 逐个 Tag 都发：不同失败原因可能配了不同的失败动作。
		if (UAnimMontage* Montage = FailureTagToAnimMontage.FindRef(Reason))
		{
			FGGYGOAbilityMontageFailureMessage Message;
			Message.PlayerController = GetActorInfo().PlayerController.Get();
			Message.AvatarActor = GetActorInfo().AvatarActor.Get();
			Message.FailureTags = FailedReason;
			Message.FailureMontage = Montage;

			UGameplayMessageSubsystem& MessageSystem = UGameplayMessageSubsystem::Get(GetWorld());
			MessageSystem.BroadcastMessage(TAG_GGYGO_Ability_PlayMontageFailureMessage, Message);
		}
	}
}

bool UGGYGOGameplayAbility::CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	const UGGYGOAbilitySystemComponent* ASC = ActorInfo
		? Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
	if (!IsValid(ASC) || ASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem activation [%s] Spec [%s] rejected: a live project ASC/ActorInfo is required."),
			*GetPathName(), *Handle.ToString());
		return false;
	}
	FGGYGOAbilityInputBuffer::FScopedCanActivateAbility CanScope(*ASC->InputBuffer);
	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Handle);
	if (!Spec || Spec->PendingRemove) { return false; }
	// InstancedPerActor 的同一 Spec 同时只有一次激活；AI 查询也经过这里，激活期间如实返回不可用。
	if (GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::InstancedPerActor && Spec->IsActive()) { return false; }

	// 父类先做冷却、消耗、Tag 需求等通用检查。
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
	{
		return false;
	}

	// 切回仍在续放的队员期间只拦输入缓冲发起的激活；事件触发（受击、死亡）与续放能力
	// 自身的后续激活不受影响。失败 Tag 让输入缓冲保留这次按下，标签移除后重试。
	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_SwitchReturnPending)
		&& ASC->IsAbilityInputActivationAttempt(Handle))
	{
		if (OptionalRelevantTags) { OptionalRelevantTags->AddTag(GGYGOGameplayTags::Ability_ActivateFail_SwitchReturnPending); }
		return false;
	}

	EGGYGOAbilityGroupBlockReason BlockReason = EGGYGOAbilityGroupBlockReason::NotBlocked;
	if (ASC->IsActivationBlockedByGroup(this, BlockReason))
	{
		if (OptionalRelevantTags)
		{
			// 把"该不该重试"编码进失败 Tag，意图层不必反查配置表就能决定
			// 把请求留在缓冲里还是丢弃。
			OptionalRelevantTags->AddTag(BlockReason == EGGYGOAbilityGroupBlockReason::GroupOccupiedQueued
				? GGYGOGameplayTags::Ability_ActivateFail_ActivationGroupQueued
				: GGYGOGameplayTags::Ability_ActivateFail_ActivationGroup);
		}
		return false;
	}
	return CanActivateAbilityAdditional(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}

bool UGGYGOGameplayAbility::CanActivateAbilityAdditional(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	// 明确的默认模式：没有附加准入条件；核心检查由 final 入口先完成。
	return true;
}

void UGGYGOGameplayAbility::SetCanBeCanceled(bool bCanBeCanceled)
{
	// 只有 Exclusive 能力可以拒绝被取消。
	// Coexist 能力随时可能被同组高优先级或跨组 Exclusive 顶掉，
	// 如果它声明自己不可取消，仲裁就无法执行，组规则会失效。
	if (!bCanBeCanceled && (SelfPolicy != EGGYGOAbilitySelfPolicy::Exclusive))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SetCanBeCanceled: 能力 [%s] 不能拒绝取消，因为它的 SelfPolicy 不是 Exclusive。"),
			*GetName());
		return;
	}

	Super::SetCanBeCanceled(bCanBeCanceled);
}

void UGGYGOGameplayAbility::OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	Super::OnGiveAbility(ActorInfo, Spec);

	K2_OnAbilityAdded();

	TryActivateAbilityOnSpawn(ActorInfo, Spec);
}

void UGGYGOGameplayAbility::OnRemoveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	// 先让蓝图在状态还完整时做清理，再交给父类。
	K2_OnAbilityRemoved();

	Super::OnRemoveAbility(ActorInfo, Spec);
}

bool UGGYGOGameplayAbility::CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, OUT FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CheckCost(Handle, ActorInfo, OptionalRelevantTags) || !ActorInfo)
	{
		return false;
	}

	// 按资产配置顺序检查，保证行为可预期。
	for (const TObjectPtr<UGGYGOAbilityCost>& AdditionalCost : AdditionalCosts)
	{
		// 允许数组里有空槽，不当作失败。
		if (AdditionalCost != nullptr)
		{
			if (!AdditionalCost->CheckCost(this, Handle, ActorInfo, /*inout*/ OptionalRelevantTags))
			{
				// 一项付不起就直接拒绝，不必检查剩下的。
				return false;
			}
		}
	}

	return true;
}

void UGGYGOGameplayAbility::ApplyCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const
{
	Super::ApplyCost(Handle, ActorInfo, ActivationInfo);

	check(ActorInfo);

	// 判断本次能力是否真的命中了目标。只有服务器有权威的命中数据，
	// 客户端预测端不能据此扣除"命中才扣"的消耗。
	auto DetermineIfAbilityHitTarget = [&]()
	{
		if (ActorInfo->IsNetAuthority())
		{
			if (UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
			{
				FGameplayAbilityTargetDataHandle TargetData;
				ASC->GetAbilityTargetData(Handle, ActivationInfo, TargetData);

				for (int32 TargetDataIdx = 0; TargetDataIdx < TargetData.Data.Num(); ++TargetDataIdx)
				{
					if (UAbilitySystemBlueprintLibrary::TargetDataHasHitResult(TargetData, TargetDataIdx))
					{
						return true;
					}
				}
			}
		}

		return false;
	};

	// 缓存命中判定结果，避免多个"命中才扣"的消耗重复查询同一份目标数据。
	bool bAbilityHitTarget = false;
	bool bHasDeterminedIfAbilityHitTarget = false;

	for (const TObjectPtr<UGGYGOAbilityCost>& AdditionalCost : AdditionalCosts)
	{
		if (AdditionalCost != nullptr)
		{
			if (AdditionalCost->ShouldOnlyApplyCostOnHit())
			{
				// 惰性求值：没有"命中才扣"的消耗时完全不查目标数据。
				if (!bHasDeterminedIfAbilityHitTarget)
				{
					bAbilityHitTarget = DetermineIfAbilityHitTarget();
					bHasDeterminedIfAbilityHitTarget = true;
				}

				if (!bAbilityHitTarget)
				{
					continue;
				}
			}

			AdditionalCost->ApplyCost(this, Handle, ActorInfo, ActivationInfo);
		}
	}
}

FGameplayEffectContextHandle UGGYGOGameplayAbility::MakeEffectContext(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo) const
{
	FGameplayEffectContextHandle ContextHandle = Super::MakeEffectContext(Handle, ActorInfo);

	// 这里 check 而不是判空跳过：拿不到自定义上下文说明
	// DefaultGame.ini 里的 AbilitySystemGlobalsClassName 没配好，
	// 静默降级会让伤害衰减和材质分流失效且很难排查。
	FGGYGOGameplayEffectContext* EffectContext = FGGYGOGameplayEffectContext::ExtractEffectContext(ContextHandle);
	check(EffectContext);

	check(ActorInfo);

	AActor* EffectCauser = nullptr;
	const IGGYGOAbilitySourceInterface* AbilitySource = nullptr;
	float SourceLevel = 0.0f;
	GetAbilitySource(Handle, ActorInfo, /*out*/ SourceLevel, /*out*/ AbilitySource, /*out*/ EffectCauser);

	UObject* SourceObject = GetSourceObject(Handle, ActorInfo);

	AActor* Instigator = ActorInfo->OwnerActor.Get();

	EffectContext->SetAbilitySource(AbilitySource, SourceLevel);
	EffectContext->AddInstigator(Instigator, EffectCauser);
	EffectContext->AddSourceObject(SourceObject);

	return ContextHandle;
}

void UGGYGOGameplayAbility::ApplyAbilityTagsToGameplayEffectSpec(FGameplayEffectSpec& Spec, FGameplayAbilitySpec* AbilitySpec) const
{
	Super::ApplyAbilityTagsToGameplayEffectSpec(Spec, AbilitySpec);

	// 把命中表面的 Tag 并进目标 Tag，让 Cue 能按材质分流、Execution 能按材质减伤。
	if (const FHitResult* HitResult = Spec.GetContext().GetHitResult())
	{
		AppendPhysicalMaterialTags(*HitResult, Spec.CapturedTargetTags.GetSpecTags());
	}
}

bool UGGYGOGameplayAbility::BuildHitEffectPayload(UAbilitySystemComponent* TargetAbilitySystemComponent,
	TSubclassOf<UGameplayEffect> DamageEffectClass, float EffectLevel,
	const FHitResult& HitResult, const FVector& Origin,
	FGGYGOHitEffectPayload& OutPayload) const
{
	OutPayload = FGGYGOHitEffectPayload();
	FGGYGOHitEffectPayload Payload;
	const FGameplayAbilitySpecHandle OriginalSpecHandle = CurrentSpecHandle;
	UAbilitySystemComponent* SourceAbilitySystemComponent = CurrentActorInfo
		? GetAbilitySystemComponentFromActorInfo() : nullptr;
	const TWeakObjectPtr<const UGGYGOGameplayAbility> OriginalAbility(this);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalSource(SourceAbilitySystemComponent);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalTarget(TargetAbilitySystemComponent);
	const bool bRequiresEffectSpec = DamageEffectClass != nullptr;
	const TWeakObjectPtr<UClass> OriginalEffectClass(DamageEffectClass.Get());
	// Diagnostic strings survive real extension callbacks destroying their source objects.
	const FString AbilityPath = GetPathNameSafe(this);
	const FString SourcePath = GetPathNameSafe(SourceAbilitySystemComponent);
	const FString TargetPath = GetPathNameSafe(TargetAbilitySystemComponent);
	const FString EffectClassPath = GetPathNameSafe(DamageEffectClass.Get());
	const auto Fail = [&](const TCHAR* Reason)
	{
		OutPayload = FGGYGOHitEffectPayload();
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem BuildHitEffectPayload Ability [%s] Source [%s] Target [%s] GE [%s] failed: %s."),
			*AbilityPath, *SourcePath, *TargetPath, *EffectClassPath, Reason);
		return false;
	};
	const auto HasOriginalObjects = [&]()
	{
		return OriginalAbility.IsValid() && OriginalSource.IsValid() && OriginalTarget.IsValid();
	};
	const auto HasValidRequiredSpec = [&]()
	{
		return OriginalEffectClass.IsValid() && Payload.EffectSpec.IsValid()
			&& IsValid(Payload.EffectSpec.Data->Def.Get())
			&& Payload.EffectSpec.Data->GetContext().IsValid();
	};
	if (!CurrentActorInfo) { return Fail(TEXT("MissingActorInfo")); }
	if (!OriginalSource.IsValid()) { return Fail(TEXT("InvalidSourceASC")); }
	if (!OriginalTarget.IsValid()) { return Fail(TEXT("InvalidTargetASC")); }
	if (bRequiresEffectSpec && !OriginalEffectClass.IsValid()) { return Fail(TEXT("InvalidRequiredGEClass")); }

	Payload.EffectContext = MakeEffectContext(OriginalSpecHandle, CurrentActorInfo);
	if (!Payload.EffectContext.IsValid()) { return Fail(TEXT("InvalidEffectContext")); }

	// AddHitResult(reset=true) 会用 TraceStart 改写 Origin，所以显式 Origin 必须最后写入。
	Payload.EffectContext.AddHitResult(HitResult, /*bReset=*/true);
	FGGYGOGameplayEffectContext* GGYGOContext =
		FGGYGOGameplayEffectContext::ExtractEffectContext(Payload.EffectContext);
	check(GGYGOContext);
	GGYGOContext->SetSourceOriginSnapshot(Origin);

	if (bRequiresEffectSpec)
	{
		Payload.EffectSpec = SourceAbilitySystemComponent->MakeOutgoingSpec(
			DamageEffectClass, EffectLevel, Payload.EffectContext);
		if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterMakeOutgoingSpec")); }
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecCreationFailed")); }
		FGameplayAbilitySpec* AbilitySpec = SourceAbilitySystemComponent->FindAbilitySpecFromHandle(OriginalSpecHandle);
		const bool bHasAbilitySpec = AbilitySpec != nullptr;
		const TMap<FGameplayTag, float> OriginalSetByCallerMagnitudes = AbilitySpec
			? AbilitySpec->SetByCallerTagMagnitudes : TMap<FGameplayTag, float>{};
		ApplyAbilityTagsToGameplayEffectSpec(*Payload.EffectSpec.Data.Get(), AbilitySpec);
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecInvalidAfterApplyAbilityTags")); }

		// The extension may remove/reenter the native Spec; use this hit's original values.
		if (bHasAbilitySpec)
		{
			Payload.EffectSpec.Data->SetByCallerTagMagnitudes = OriginalSetByCallerMagnitudes;
		}
		BP_EditSpecValues(Payload.EffectSpec);
		if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterBP_EditSpecValues")); }
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecInvalidAfterBP_EditSpecValues")); }
		UAbilitySystemGlobals::Get().InitGameplayCueParameters_GESpec(
			Payload.CueParameters, *Payload.EffectSpec.Data.Get());
		if (!HasValidRequiredSpec()) { return Fail(TEXT("RequiredSpecInvalidAfterCueInitialization")); }
	}
	else
	{
		UAbilitySystemGlobals::Get().InitGameplayCueParameters(
			Payload.CueParameters, Payload.EffectContext);
	}
	if (!HasOriginalObjects()) { return Fail(TEXT("SourceOrTargetInvalidAfterCueInitialization")); }
	if (!Payload.EffectContext.IsValid()) { return Fail(TEXT("EffectContextInvalidAfterCueInitialization")); }

	// 带输出参数的 GetOwnedGameplayTags 会先 Reset 容器；这里读取 const 集合后追加，
	// 才不会覆盖 GESpec 已聚合的 Tag。物理材质最后追加，保证有/无 GE 两条路径一致。
	Payload.CueParameters.AggregatedTargetTags.AppendTags(
		TargetAbilitySystemComponent->GetOwnedGameplayTags());
	AppendPhysicalMaterialTags(HitResult, Payload.CueParameters.AggregatedTargetTags);
	Payload.CueParameters.Location = HitResult.ImpactPoint;
	Payload.CueParameters.Normal = HitResult.ImpactNormal;
	Payload.CueParameters.Instigator = Payload.EffectContext.GetInstigator();
	Payload.CueParameters.EffectCauser = Payload.EffectContext.GetEffectCauser();
	OutPayload = MoveTemp(Payload);

	return true;
}

bool UGGYGOGameplayAbility::DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& AbilitySystemComponent, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, OUT FGameplayTagContainer* OptionalRelevantTags) const
{
	// 相比父类多做两件事：展开 ASC 的 Tag 关系表，以及把"因死亡失败"单独标记出来。

	bool bBlocked = false;
	bool bMissing = false;

	UAbilitySystemGlobals& AbilitySystemGlobals = UAbilitySystemGlobals::Get();
	const FGameplayTag& BlockedTag = AbilitySystemGlobals.ActivateFailTagsBlockedTag;
	const FGameplayTag& MissingTag = AbilitySystemGlobals.ActivateFailTagsMissingTag;

	if (AbilitySystemComponent.AreAbilityTagsBlocked(GetAssetTags()))
	{
		bBlocked = true;
	}

	const UGGYGOAbilitySystemComponent* GGYGOASC = Cast<UGGYGOAbilitySystemComponent>(&AbilitySystemComponent);

	// 复制一份再扩展，绝不能直接改资产上的 ActivationRequiredTags / ActivationBlockedTags。
	FGameplayTagContainer AllRequiredTags = ActivationRequiredTags;
	FGameplayTagContainer AllBlockedTags = ActivationBlockedTags;

	if (GGYGOASC)
	{
		GGYGOASC->GetAdditionalActivationTagRequirements(GetAssetTags(), AllRequiredTags, AllBlockedTags);
	}

	if (AllBlockedTags.Num() || AllRequiredTags.Num())
	{
		FGameplayTagContainer AbilitySystemComponentTags;
		AbilitySystemComponent.GetOwnedGameplayTags(AbilitySystemComponentTags);

		if (AbilitySystemComponentTags.HasAny(AllBlockedTags))
		{
			// 死亡是最常见的失败原因，单独给一个 Tag，方便表现层区别对待
			// （死亡时不该弹"耐力不足"这类提示）。
			if (OptionalRelevantTags && AbilitySystemComponentTags.HasTag(GGYGOGameplayTags::State_Dead))
			{
				OptionalRelevantTags->AddTag(GGYGOGameplayTags::Ability_ActivateFail_IsDead);
			}

			bBlocked = true;
		}

		if (!AbilitySystemComponentTags.HasAll(AllRequiredTags))
		{
			bMissing = true;
		}
	}

	if (SourceTags != nullptr)
	{
		if (SourceBlockedTags.Num() || SourceRequiredTags.Num())
		{
			if (SourceTags->HasAny(SourceBlockedTags))
			{
				bBlocked = true;
			}

			if (!SourceTags->HasAll(SourceRequiredTags))
			{
				bMissing = true;
			}
		}
	}

	if (TargetTags != nullptr)
	{
		if (TargetBlockedTags.Num() || TargetRequiredTags.Num())
		{
			if (TargetTags->HasAny(TargetBlockedTags))
			{
				bBlocked = true;
			}

			if (!TargetTags->HasAll(TargetRequiredTags))
			{
				bMissing = true;
			}
		}
	}

	// blocked 优先于 missing：两者同时成立时只报阻断，反馈更准确。
	if (bBlocked)
	{
		if (OptionalRelevantTags && BlockedTag.IsValid())
		{
			OptionalRelevantTags->AddTag(BlockedTag);
		}
		return false;
	}

	if (bMissing)
	{
		if (OptionalRelevantTags && MissingTag.IsValid())
		{
			OptionalRelevantTags->AddTag(MissingTag);
		}
		return false;
	}

	return true;
}

void UGGYGOGameplayAbility::OnPawnAvatarSet()
{
	K2_OnPawnAvatarSet();
}

EGGYGOSwitchOutDecision UGGYGOGameplayAbility::OnAvatarSwitchOut_Implementation(const FGGYGOSwitchContext& Context)
{
	return AvatarExitPolicy == EGGYGOAbilityAvatarExitPolicy::ContinueInBackground
		? EGGYGOSwitchOutDecision::ContinueOnField : EGGYGOSwitchOutDecision::Cancel;
}

void UGGYGOGameplayAbility::NotifyOwnerControlLost()
{
	NativeOnOwnerControlLost();
	K2_OnOwnerControlLost();
}

void UGGYGOGameplayAbility::NotifyOwnerControlRegained()
{
	NativeOnOwnerControlRegained();
	K2_OnOwnerControlRegained();
}

void UGGYGOGameplayAbility::ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction)
{
	// 默认基类只提供安全分发入口；业务载荷由派生能力解释。
}

uint64 UGGYGOGameplayAbility::BeginAbilityGroupAdmissionAttempt(uint64 AdmissionSequence)
{
	// ASC 分配的序号跨实例单调递增；0 表示分配失败，按拒绝处理。
	FAbilityGroupAdmissionAttempt& Attempt = AbilityGroupAdmissionAttempts.AddDefaulted_GetRef();
	Attempt.Sequence = AdmissionSequence;
	Attempt.bRejected = AdmissionSequence == 0;
	return Attempt.Sequence;
}

uint64 UGGYGOGameplayAbility::GetCurrentAbilityGroupAdmissionSequence() const
{
	return AbilityGroupAdmissionAttempts.IsEmpty() ? 0 : AbilityGroupAdmissionAttempts.Last().Sequence;
}

void UGGYGOGameplayAbility::RejectCurrentAbilityGroupAdmission()
{
	RejectAbilityGroupAdmission(GetCurrentAbilityGroupAdmissionSequence());
}

void UGGYGOGameplayAbility::RejectAbilityGroupAdmission(uint64 AdmissionSequence)
{
	if (FAbilityGroupAdmissionAttempt* Attempt = AbilityGroupAdmissionAttempts.FindByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; }))
	{
		Attempt->bRejected = true;
	}
}

bool UGGYGOGameplayAbility::IsCurrentAbilityGroupAdmissionRejected() const
{
	return !AbilityGroupAdmissionAttempts.IsEmpty() && AbilityGroupAdmissionAttempts.Last().bRejected;
}

bool UGGYGOGameplayAbility::IsAbilityGroupAdmissionRejected(uint64 AdmissionSequence) const
{
	const FAbilityGroupAdmissionAttempt* Attempt = AbilityGroupAdmissionAttempts.FindByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; });
	return Attempt && Attempt->bRejected;
}

bool UGGYGOGameplayAbility::IsAbilityGroupAdmissionPending(uint64 AdmissionSequence) const
{
	return AbilityGroupAdmissionAttempts.ContainsByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; });
}

bool UGGYGOGameplayAbility::ConsumeAbilityGroupAdmissionRejection(uint64 AdmissionSequence)
{
	FAbilityGroupAdmissionAttempt* Attempt = AbilityGroupAdmissionAttempts.FindByPredicate(
		[AdmissionSequence](const FAbilityGroupAdmissionAttempt& Candidate) { return Candidate.Sequence == AdmissionSequence; });
	const bool bRejected = Attempt && Attempt->bRejected;
	if (Attempt)
	{
		Attempt->bRejected = false;
	}
	return bRejected;
}

void UGGYGOGameplayAbility::CompleteAbilityGroupAdmissionAttempt(uint64 AdmissionSequence)
{
	AbilityGroupAdmissionAttempts.RemoveAll([AdmissionSequence](const FAbilityGroupAdmissionAttempt& Attempt)
	{
		return Attempt.Sequence == AdmissionSequence;
	});
}

void UGGYGOGameplayAbility::GetAbilitySource(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, float& OutSourceLevel, const IGGYGOAbilitySourceInterface*& OutAbilitySource, AActor*& OutEffectCauser) const
{
	// 先给确定的默认值，避免调用方读到未初始化数据。
	OutSourceLevel = 0.0f;
	OutAbilitySource = nullptr;
	OutEffectCauser = nullptr;

	// 默认由 Avatar 承担"造成伤害的物体"。武器类能力可以在派生实现里换成武器 Actor。
	OutEffectCauser = ActorInfo->AvatarActor.Get();

	// SourceObject 实现了来源接口时才提供衰减信息（例如武器实例）。
	UObject* SourceObject = GetSourceObject(Handle, ActorInfo);
	OutAbilitySource = Cast<IGGYGOAbilitySourceInterface>(SourceObject);
}

void UGGYGOGameplayAbility::TryActivateAbilityOnSpawn(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) const
{
	if (ActorInfo && !Spec.IsActive() && (ActivationPolicy == EGGYGOAbilityActivationPolicy::OnSpawn))
	{
		UAbilitySystemComponent* ASC = ActorInfo->AbilitySystemComponent.Get();
		const AActor* AvatarActor = ActorInfo->AvatarActor.Get();

		// 正在断开或即将销毁的 Avatar 不激活，等新 Avatar 绑定后重新走授予流程。
		if (AvatarActor && !AvatarActor->GetTearOff() && (AvatarActor->GetLifeSpan() <= 0.0f))
		{
			const bool bIsLocalExecution = (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::LocalPredicted) || (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::LocalOnly);
			const bool bIsServerExecution = (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::ServerOnly) || (NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::ServerInitiated);

			const bool bClientShouldActivate = ActorInfo->IsLocallyControlled() && bIsLocalExecution;
			const bool bServerShouldActivate = ActorInfo->IsNetAuthority() && bIsServerExecution;

			// 只有角色与策略匹配的一端发起，避免两端重复激活。
			if (bClientShouldActivate || bServerShouldActivate)
			{
				ASC->TryActivateAbility(Spec.Handle);
			}
		}
	}
}

void UGGYGOGameplayAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	// 身份已在原生 PreActivate 的 NotifyAbilityActivated 中签发。
	const FGGYGOAbilityActivationHandle Original = CaptureCurrentActivation();
	const uint64 AdmissionSequence = GetCurrentAbilityGroupAdmissionSequence();
	if (!Original.HasActivation())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("AbilitySystem Activate [%s] Spec [%s] has no issued activation; ending it as cancelled."),
			*GetPathName(), *Handle.ToString());
		CompleteAbilityGroupAdmissionAttempt(AdmissionSequence);
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}
	// 初始化、组准入（会取消其它能力）与镜头接收者都可能外调并结束本次激活，之后各核对一次。
	InitializeAbilityActivation(Original);
	if (!IsActivationCurrent(Original))
	{
		CompleteAbilityGroupAdmissionAttempt(AdmissionSequence);
		return;
	}
	const bool bRejectedBeforeFinalization = IsAbilityGroupAdmissionRejected(AdmissionSequence);
	bool bResolverAdmitted = true;
	if (!bRejectedBeforeFinalization && AdmissionSequence != 0)
	{
		UGGYGOAbilitySystemComponent* GGYGOASC = Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get());
		bResolverAdmitted = GGYGOASC && GGYGOASC->FinalizeAbilityGroupAdmission(this, AdmissionSequence);
	}
	if (!IsActivationCurrent(Original))
	{
		CompleteAbilityGroupAdmissionAttempt(AdmissionSequence);
		return;
	}
	const bool bRejectedAfterFinalization = ConsumeAbilityGroupAdmissionRejection(AdmissionSequence);
	const bool bAdmissionRejected = bRejectedBeforeFinalization || !bResolverAdmitted || bRejectedAfterFinalization;
	CompleteAbilityGroupAdmissionAttempt(AdmissionSequence);
	if (bAdmissionRejected)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
		return;
	}
	// 保持"镜头 → 父类／蓝图激活"的顺序。
	if (AbilityCameraMode)
	{
		SetCameraMode(AbilityCameraMode);
		if (!IsActivationCurrent(Original)) { return; }
	}
	if (!CameraOffset.IsNearlyZero())
	{
		ApplyCameraOffset(CameraOffset);
		if (!IsActivationCurrent(Original)) { return; }
	}
	ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
}

void UGGYGOGameplayAbility::InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original)
{
	// Normal default: this ability has no additional per-activation initialization.
}

void UGGYGOGameplayAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	Super::ActivateAbility(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}

void UGGYGOGameplayAbility::SetCameraMode(TSubclassOf<UGGYGOCameraMode> CameraMode)
{
	if (bEndingActivation)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("AbilitySystem camera mode [%s] asset [%s] rejected while the activation is ending."),
			*GetPathName(), *GetNameSafe(CameraMode.Get()));
		return;
	}
	// 能力实例是 InstancedPerActor；显式替换时先释放它自己保存的旧接收者请求。
	ClearCameraMode();

	if (!CameraMode)
	{
		return;
	}

	// HeroComponent 保存“当前有效模式”及其所有者。CameraComponent 每次被引擎拉取时
	// 再读取这个结果，因此能力不需要自己每帧重复 Push。
	if (UGGYGOHeroComponent* HeroComponent = UGGYGOHeroComponent::FindHeroComponent(GetAvatarActorFromActorInfo()))
	{
		const uint64 RequestGeneration = HeroComponent->SetAbilityCameraMode(CameraMode, CurrentSpecHandle);
		if (RequestGeneration != 0)
		{
			AppliedCameraModeHeroComponent = HeroComponent;
			AppliedCameraModeSpecHandle = CurrentSpecHandle;
			AppliedCameraModeRequestGeneration = RequestGeneration;
			ActiveCameraMode = CameraMode;
		}
	}
}

void UGGYGOGameplayAbility::ClearCameraMode()
{
	if (AppliedCameraModeRequestGeneration != 0)
	{
		if (UGGYGOHeroComponent* HeroComponent = AppliedCameraModeHeroComponent.Get())
		{
			HeroComponent->ClearAbilityCameraMode(AppliedCameraModeSpecHandle, AppliedCameraModeRequestGeneration);
		}
	}

	// 不按当前 Avatar 重新查找接收者。Avatar 变化后旧请求不转移到新 Pawn。
	AppliedCameraModeHeroComponent.Reset();
	AppliedCameraModeSpecHandle = FGameplayAbilitySpecHandle();
	AppliedCameraModeRequestGeneration = 0;
	ActiveCameraMode = nullptr;
}

void UGGYGOGameplayAbility::ApplyCameraOffset(const FGGYGOCameraOffset& Offset)
{
	if (bEndingActivation)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("AbilitySystem camera offset [%s] rejected while the activation is ending."), *GetPathName());
		return;
	}
	// Offset 是相机组件的单槽资源；替换时只撤销本能力持有的旧 token。
	ClearCameraOffset();

	if (UGGYGOCameraComponent* CameraComponent = UGGYGOCameraComponent::FindCameraComponent(GetAvatarActorFromActorInfo()))
	{
		const FGGYGOCameraOffsetHandle Handle = CameraComponent->SetCameraOffset(Offset);
		if (Handle.IsValid())
		{
			AppliedCameraOffsetHandle = Handle;
			AppliedCameraOffsetComponent = CameraComponent;
		}
	}
}

void UGGYGOGameplayAbility::ClearCameraOffset()
{
	if (UGGYGOCameraComponent* CameraComponent = AppliedCameraOffsetComponent.Get())
	{
		CameraComponent->ClearCameraOffset(AppliedCameraOffsetHandle);
	}

	// 清理只归还申请时保存的相机与 token；当前 Avatar 不参与资源查找。
	AppliedCameraOffsetComponent.Reset();
	AppliedCameraOffsetHandle = FGGYGOCameraOffsetHandle();
}

void UGGYGOGameplayAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	// 只归还申请时保存的接收者与凭证；当前 Avatar 不参与查找。
	ClearCameraOffset();
	ClearCameraMode();
}

void UGGYGOGameplayAbility::EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled)
{
	if (!IsEndAbilityValid(Handle, ActorInfo) || ScopeLockCount > 0)
	{
		// 无效调用由父类忽略；ScopeLock 期间父类把同一调用排进 WaitingToExecute，执行时再次进入这里。
		Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
		return;
	}
	if (bEndingActivation)
	{
		return; // 资源清理回调内的重复结束；外层这次结束继续执行原生 EndAbility。
	}
	const FGGYGOAbilityActivationHandle Ending = CurrentActivation;
	bEndingActivation = true;
	CleanupAbilityResourcesForTermination(FGGYGOAbilityTerminationContext(Ending, bWasCancelled));
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
	// 正常路径已在 ASC::NotifyAbilityEnded 中退役；原生没有到达 ASC 通知（如 ASC 失效）时在此退役。
	// 结束广播内签发的后继激活不受影响。
	if (!CurrentActivation.HasActivation() || CurrentActivation.HasSameActivation(Ending))
	{
		RetireActivation();
	}
}

#if WITH_EDITOR
EDataValidationResult UGGYGOGameplayAbility::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	if (GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::NonInstanced)
	{
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("%s: 项目能力必须实例化；激活身份与结束清理保存在能力实例上。"), *GetPathName())));
		Result = EDataValidationResult::Invalid;
	}
	if (bRetriggerInstancedAbility)
	{
		Context.AddError(FText::FromString(FString::Printf(
			TEXT("%s: 项目能力不得启用 bRetriggerInstancedAbility；同一 Spec 同时只允许一次激活，结束后由调用方显式重新激活。"), *GetPathName())));
		Result = EDataValidationResult::Invalid;
	}
	return Result;
}
#endif
