/**
 * @file GGYGOAbilitySystemComponent.cpp
 * @brief 项目 ASC 实现
 */
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilityTagRelationshipMapping.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOAbilitySystemComponent)

UE_DEFINE_GAMEPLAY_TAG(TAG_GGYGO_Gameplay_AbilityInputBlocked, "Gameplay.AbilityInputBlocked");

UGGYGOAbilitySystemComponent::UGGYGOAbilitySystemComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	InputPressedSpecHandles.Reset();
	InputReleasedSpecHandles.Reset();
	InputHeldSpecHandles.Reset();

	ActiveAbilitiesByGroup.Reset();
}

void UGGYGOAbilitySystemComponent::InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor)
{
	FGameplayAbilityActorInfo* ActorInfo = AbilityActorInfo.Get();
	check(ActorInfo);
	check(InOwnerActor);

	// 必须在 Super 之前比较，因为 Super 会把新 Avatar 写进 ActorInfo。
	// 只有"确实换了一个 Pawn"才算新 Avatar，避免重复初始化。
	const bool bHasNewPawnAvatar = Cast<APawn>(InAvatarActor) && (InAvatarActor != ActorInfo->AvatarActor);

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

		// Lyra 在这里还会注册 GlobalAbilitySystem 并把 AnimInstance 与 ASC 关联。
		// 两者在 GGYGO 都还不存在：
		//   - UGGYGOGlobalAbilitySystem 尚未实现（队伍 Buff 广播，阶段 10 需要）
		//   - UZZZAnimInstance 目前从 Pipeline 接收推送，阶段 5b 改为自拉时再接 ASC
		// 等它们就位后在此处补上注册。

		// 放在最后：确保 ActorInfo 已完整、能力实例已收到 Avatar 通知。
		TryActivateAbilitiesOnSpawn();
	}
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

void UGGYGOAbilitySystemComponent::CancelAbilitiesByFunc(TShouldCancelAbilityFunc ShouldCancelFunc, bool bReplicateCancelAbility)
{
	ABILITYLIST_SCOPE_LOCK();
	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		if (!AbilitySpec.IsActive())
		{
			continue;
		}

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
			if (!GGYGOAbilityInstance)
			{
				continue;
			}

			if (ShouldCancelFunc(GGYGOAbilityInstance, AbilitySpec.Handle))
			{
				if (GGYGOAbilityInstance->CanBeCanceled())
				{
					GGYGOAbilityInstance->CancelAbility(AbilitySpec.Handle, AbilityActorInfo.Get(), GGYGOAbilityInstance->GetCurrentActivationInfo(), bReplicateCancelAbility);
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
}

void UGGYGOAbilitySystemComponent::CancelInputActivatedAbilities(bool bReplicateCancelAbility)
{
	auto ShouldCancelFunc = [](const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle)
	{
		const EGGYGOAbilityActivationPolicy Policy = Ability->GetActivationPolicy();
		return (Policy == EGGYGOAbilityActivationPolicy::OnInputTriggered) || (Policy == EGGYGOAbilityActivationPolicy::WhileInputActive);
	};

	CancelAbilitiesByFunc(ShouldCancelFunc, bReplicateCancelAbility);
}

void UGGYGOAbilitySystemComponent::AbilityInputTagPressed(const FGameplayTag& InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	// 精确匹配（HasTagExact）而非层级匹配：InputTag 是具体按键语义，
	// 用层级匹配会让 InputTag.Attack 的按键误触发 InputTag.Attack.Heavy 的能力。
	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		if (AbilitySpec.Ability && AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			InputPressedSpecHandles.AddUnique(AbilitySpec.Handle);
			InputHeldSpecHandles.AddUnique(AbilitySpec.Handle);
		}
	}
}

void UGGYGOAbilitySystemComponent::AbilityInputTagReleased(const FGameplayTag& InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	for (const FGameplayAbilitySpec& AbilitySpec : ActivatableAbilities.Items)
	{
		if (AbilitySpec.Ability && AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			InputReleasedSpecHandles.AddUnique(AbilitySpec.Handle);

			// 从 held 移除，否则 WhileInputActive 能力下一帧还会继续激活。
			InputHeldSpecHandles.Remove(AbilitySpec.Handle);
		}
	}
}

void UGGYGOAbilitySystemComponent::ProcessAbilityInput(float DeltaTime, bool bGamePaused)
{
	if (HasMatchingGameplayTag(TAG_GGYGO_Gameplay_AbilityInputBlocked))
	{
		// 连 held 一起清掉：否则屏蔽解除的那一帧，之前按住的键会突然触发一堆能力。
		ClearAbilityInput();
		return;
	}

	TArray<FGameplayAbilitySpecHandle> AbilitiesToActivate;

	// 第一阶段：按住持续激活的能力。只收集，不激活。
	for (const FGameplayAbilitySpecHandle& SpecHandle : InputHeldSpecHandles)
	{
		// 每次都重新查：能力可能在输入与处理之间被移除。
		if (const FGameplayAbilitySpec* AbilitySpec = FindAbilitySpecFromHandle(SpecHandle))
		{
			if (AbilitySpec->Ability && !AbilitySpec->IsActive())
			{
				const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(AbilitySpec->Ability);
				if (AbilityCDO && AbilityCDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::WhileInputActive)
				{
					AbilitiesToActivate.AddUnique(AbilitySpec->Handle);
				}
			}
		}
	}

	// 第二阶段：本帧按下。已激活的收事件，未激活的收集待激活。
	for (const FGameplayAbilitySpecHandle& SpecHandle : InputPressedSpecHandles)
	{
		if (FGameplayAbilitySpec* AbilitySpec = FindAbilitySpecFromHandle(SpecHandle))
		{
			if (AbilitySpec->Ability)
			{
				AbilitySpec->InputPressed = true;

				if (AbilitySpec->IsActive())
				{
					// 已在运行，不重新激活，只把输入事件传进去（连段、蓄力靠这个）。
					AbilitySpecInputPressed(*AbilitySpec);
				}
				else
				{
					const UGGYGOGameplayAbility* AbilityCDO = Cast<UGGYGOGameplayAbility>(AbilitySpec->Ability);
					if (AbilityCDO && AbilityCDO->GetActivationPolicy() == EGGYGOAbilityActivationPolicy::OnInputTriggered)
					{
						AbilitiesToActivate.AddUnique(AbilitySpec->Handle);
					}
				}
			}
		}
	}

	// 第三阶段：统一激活。
	// 必须放在 held 与 pressed 收集之后：否则 held 先激活能力，
	// 紧接着 pressed 又会把同一次按下当成输入事件发给刚创建的实例。
	for (const FGameplayAbilitySpecHandle& AbilitySpecHandle : AbilitiesToActivate)
	{
		// 权限、Tag 需求、组仲裁、预测键都在 TryActivateAbility 内部处理。
		TryActivateAbility(AbilitySpecHandle);
	}

	// 第四阶段：本帧释放。
	for (const FGameplayAbilitySpecHandle& SpecHandle : InputReleasedSpecHandles)
	{
		if (FGameplayAbilitySpec* AbilitySpec = FindAbilitySpecFromHandle(SpecHandle))
		{
			if (AbilitySpec->Ability)
			{
				AbilitySpec->InputPressed = false;

				if (AbilitySpec->IsActive())
				{
					AbilitySpecInputReleased(*AbilitySpec);
				}
			}
		}
	}

	// pressed / released 是本帧事件，用完即清；held 保留到真正释放为止。
	InputPressedSpecHandles.Reset();
	InputReleasedSpecHandles.Reset();
}

void UGGYGOAbilitySystemComponent::ClearAbilityInput()
{
	InputPressedSpecHandles.Reset();
	InputReleasedSpecHandles.Reset();
	InputHeldSpecHandles.Reset();
}

void UGGYGOAbilitySystemComponent::AbilitySpecInputPressed(FGameplayAbilitySpec& Spec)
{
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

bool UGGYGOAbilitySystemComponent::IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability) const
{
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
			if (!Active || Active == Ability)
			{
				continue;
			}

			if (Active->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive && Active->GetActivationPriority() > RequestPriority)
			{
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

	// 第二步：同组冲突。
	// 阶段 1 硬编码按 SingleInstance 处理；阶段 3 改为查 UGGYGOAbilityGroupConfig，
	// 届时 Coexist 组会直接放过，SingleInstanceQueued 组会转为入队而不是拒绝。
	if (const TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag))
	{
		for (const TWeakObjectPtr<UGGYGOGameplayAbility>& WeakActive : *Group)
		{
			const UGGYGOGameplayAbility* Active = WeakActive.Get();
			if (!Active || Active == Ability)
			{
				continue;
			}

			// 用 > 而非 >=：同优先级不阻断，让后来者能激活并在下一步取消旧实例（D4）。
			if (Active->GetActivationPriority() > RequestPriority)
			{
				return true;
			}
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
	const int32 NewPriority = Ability->GetActivationPriority();

	// 先登记再取消，这样取消回调里查询到的组状态是最新的。
	if (GroupTag.IsValid())
	{
		ActiveAbilitiesByGroup.FindOrAdd(GroupTag).AddUnique(Ability);
	}

	// Exclusive 能力压制所有组里优先级更低的能力。
	if (Ability->GetSelfPolicy() == EGGYGOAbilitySelfPolicy::Exclusive)
	{
		auto ShouldCancelFunc = [Ability, NewPriority](const UGGYGOGameplayAbility* Other, FGameplayAbilitySpecHandle Handle)
		{
			return (Other != Ability) && (Other->GetActivationPriority() < NewPriority);
		};

		CancelAbilitiesByFunc(ShouldCancelFunc, /*bReplicateCancelAbility=*/true);
	}

	// 同组顶替：取消同组里优先级**小于等于**自己的其他实例。
	// 这里用 <= 而 IsActivationBlockedByGroup 用 >，两者拼起来正好是 D4：
	// 同优先级时新的能激活（不被阻断），旧的被取消。
	if (GroupTag.IsValid())
	{
		auto ShouldCancelFunc = [Ability, NewPriority, GroupTag](const UGGYGOGameplayAbility* Other, FGameplayAbilitySpecHandle Handle)
		{
			return (Other != Ability)
				&& (Other->GetGroupTag() == GroupTag)
				&& (Other->GetActivationPriority() <= NewPriority);
		};

		CancelAbilitiesByFunc(ShouldCancelFunc, /*bReplicateCancelAbility=*/true);
	}
}

void UGGYGOAbilitySystemComponent::RemoveAbilityFromActivationGroup(UGGYGOGameplayAbility* Ability)
{
	if (!Ability)
	{
		return;
	}

	const FGameplayTag GroupTag = Ability->GetGroupTag();
	if (!GroupTag.IsValid())
	{
		return;
	}

	if (TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>* Group = ActiveAbilitiesByGroup.Find(GroupTag))
	{
		Group->Remove(Ability);

		// 顺手清掉已经失效的弱引用，避免长期运行后表里堆积空项。
		Group->RemoveAll([](const TWeakObjectPtr<UGGYGOGameplayAbility>& Weak) { return !Weak.IsValid(); });

		if (Group->Num() == 0)
		{
			ActiveAbilitiesByGroup.Remove(GroupTag);
		}
	}

	// 阶段 3 补充：如果该组规则是 SingleInstanceQueued，
	// 这里要通知输入缓冲队列尝试出队下一个请求（连段的下一段）。
}

void UGGYGOAbilitySystemComponent::NotifyAbilityActivated(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability)
{
	Super::NotifyAbilityActivated(Handle, Ability);

	if (UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(Ability))
	{
		AddAbilityToActivationGroup(GGYGOAbility);
	}
}

void UGGYGOAbilitySystemComponent::NotifyAbilityFailed(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason)
{
	Super::NotifyAbilityFailed(Handle, Ability, FailureReason);

	// 服务器上非本地控制的能力失败时，反馈要在玩家自己的客户端播，所以发 RPC 过去。
	if (APawn* Avatar = Cast<APawn>(GetAvatarActor()))
	{
		if (!Avatar->IsLocallyControlled() && Ability->IsSupportedForNetworking())
		{
			ClientNotifyAbilityFailed(Ability, FailureReason);
			return;
		}
	}

	HandleAbilityFailed(Ability, FailureReason);
}

void UGGYGOAbilitySystemComponent::NotifyAbilityEnded(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, bool bWasCancelled)
{
	Super::NotifyAbilityEnded(Handle, Ability, bWasCancelled);

	if (UGGYGOGameplayAbility* GGYGOAbility = Cast<UGGYGOGameplayAbility>(Ability))
	{
		RemoveAbilityFromActivationGroup(GGYGOAbility);
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
