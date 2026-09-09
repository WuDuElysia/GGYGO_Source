/**
 * @file GGYGOGameplayAbility.cpp
 * @brief GameplayAbility 基类实现
 */
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystem/Abilities/GGYGOAbilityCost.h"
#include "AbilitySystem/Abilities/GGYGOAbilityFailureMessages.h"
#include "AbilitySystem/GGYGOAbilitySourceInterface.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOGameplayEffectContext.h"
#include "Engine/HitResult.h"
#include "GameFramework/Character.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Physics/GGYGOPhysicalMaterialWithTags.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameplayAbility)

// 失败反馈消息的两个通道 Tag 在此定义。
UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Ability_SimpleFailureMessage, "Ability.UserFacingSimpleActivateFail.Message");
UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Ability_PlayMontageFailureMessage, "Ability.PlayMontageOnActivateFail.Message");

UGGYGOGameplayAbility::UGGYGOGameplayAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 不复制能力对象本身，规格与激活状态由 ASC 复制。
	ReplicationPolicy = EGameplayAbilityReplicationPolicy::ReplicateNo;

	// 每个 Actor 一个实例，运行期状态可以安全放在成员变量里。
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

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
	if (!ActorInfo || !ActorInfo->AbilitySystemComponent.IsValid())
	{
		return false;
	}

	// 父类先做冷却、消耗、Tag 需求等通用检查。
	if (!Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
	{
		return false;
	}

	// 再做组仲裁。用 Cast 而不是 CastChecked：Ability 可能被授予到非项目 ASC 上
	// （例如测试用的裸 ASC），那种情况下跳过组检查而不是崩掉。
	if (const UGGYGOAbilitySystemComponent* GGYGOASC = Cast<UGGYGOAbilitySystemComponent>(ActorInfo->AbilitySystemComponent.Get()))
	{
		if (GGYGOASC->IsActivationBlockedByGroup(this))
		{
			if (OptionalRelevantTags)
			{
				OptionalRelevantTags->AddTag(GGYGOGameplayTags::Ability_ActivateFail_ActivationGroup);
			}
			return false;
		}
	}

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
		if (const UGGYGOPhysicalMaterialWithTags* PhysMatWithTags = Cast<const UGGYGOPhysicalMaterialWithTags>(HitResult->PhysMaterial.Get()))
		{
			Spec.CapturedTargetTags.GetSpecTags().AppendTags(PhysMatWithTags->Tags);
		}
	}
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
		if (ASC && AvatarActor && !AvatarActor->GetTearOff() && (AvatarActor->GetLifeSpan() <= 0.0f))
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
