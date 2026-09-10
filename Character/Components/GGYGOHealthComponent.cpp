/**
 * @file GGYGOHealthComponent.cpp
 * @brief 生命与韧性门面 + 死亡状态机实现
 */
#include "Character/Components/GGYGOHealthComponent.h"

#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "GameplayEffect.h"
#include "GameplayEffectExtension.h"
#include "GameplayEffectTypes.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthComponent)

class FLifetimeProperty;

UGGYGOHealthComponent::UGGYGOHealthComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 纯事件驱动，不需要 Tick。
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	SetIsReplicatedByDefault(true);

	AbilitySystemComponent = nullptr;
	HealthSet = nullptr;
	DeathState = EGGYGODeathState::NotDead;
}

void UGGYGOHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGGYGOHealthComponent, DeathState);
}

void UGGYGOHealthComponent::OnUnregister()
{
	UninitializeFromAbilitySystem();

	Super::OnUnregister();
}

void UGGYGOHealthComponent::InitializeWithAbilitySystem(UGGYGOAbilitySystemComponent* InASC)
{
	AActor* Owner = GetOwner();
	check(Owner);

	if (AbilitySystemComponent)
	{
		if (AbilitySystemComponent == InASC)
		{
			// 幂等：OnAbilitySystemInitialized_RegisterAndCall 有可能补发一次广播，
			// 加上正常广播就是两次调用，这里必须容忍。
			return;
		}

		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeWithAbilitySystem: [%s] 已绑定到另一个 ASC，先解绑再重新绑定。"),
			*GetNameSafe(Owner));
		return;
	}

	AbilitySystemComponent = InASC;
	if (!AbilitySystemComponent)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeWithAbilitySystem: [%s] 收到空 ASC。"), *GetNameSafe(Owner));
		return;
	}

	HealthSet = AbilitySystemComponent->GetSet<UGGYGOHealthSet>();
	if (!HealthSet)
	{
		// 常见原因：PawnData 的 AbilitySets 里没有配 GGYGOHealthSet。
		// 这里只报错不崩，让角色仍能生成（便于排查），但生命相关功能全部失效。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitializeWithAbilitySystem: [%s] 的 ASC 上找不到 UGGYGOHealthSet，请检查 PawnData 的 AbilitySets 配置。"),
			*GetNameSafe(Owner));
		AbilitySystemComponent = nullptr;
		return;
	}

	// 把 AttributeSet 的原生 C++ 委托接到本组件的处理器上。
	// 委托是 mutable 的，所以 const HealthSet 也能绑定。
	HealthSet->OnHealthChanged.AddUObject(this, &UGGYGOHealthComponent::HandleHealthChanged);
	HealthSet->OnMaxHealthChanged.AddUObject(this, &UGGYGOHealthComponent::HandleMaxHealthChanged);
	HealthSet->OnOutOfHealth.AddUObject(this, &UGGYGOHealthComponent::HandleOutOfHealth);
	HealthSet->OnPoiseChanged.AddUObject(this, &UGGYGOHealthComponent::HandlePoiseChanged);
	HealthSet->OnPoiseBroken.AddUObject(this, &UGGYGOHealthComponent::HandlePoiseBroken);

	// 不在这里重置属性初值。见头文件"与 Lyra 的差异"第 2 条。

	ClearGameplayTags();

	// 补发一次当前值。UI 在本组件初始化后才绑定委托，
	// 不补发的话血条会停在 0 直到第一次受伤。
	// Instigator 传 nullptr：这不是任何人造成的变化，只是一次状态同步。
	OnHealthChanged.Broadcast(this, 0.0f, HealthSet->GetHealth(), nullptr);
	OnMaxHealthChanged.Broadcast(this, 0.0f, HealthSet->GetMaxHealth(), nullptr);
	OnPoiseChanged.Broadcast(this, 0.0f, HealthSet->GetPoise(), nullptr);
}

void UGGYGOHealthComponent::UninitializeFromAbilitySystem()
{
	ClearGameplayTags();

	if (HealthSet)
	{
		// 必须逐个解绑。ASC 可能被换到别的 Avatar 上继续使用，
		// 留着悬空绑定会在下次属性变化时访问已销毁的本组件。
		HealthSet->OnHealthChanged.RemoveAll(this);
		HealthSet->OnMaxHealthChanged.RemoveAll(this);
		HealthSet->OnOutOfHealth.RemoveAll(this);
		HealthSet->OnPoiseChanged.RemoveAll(this);
		HealthSet->OnPoiseBroken.RemoveAll(this);
	}

	HealthSet = nullptr;
	AbilitySystemComponent = nullptr;
}

void UGGYGOHealthComponent::ClearGameplayTags()
{
	if (AbilitySystemComponent)
	{
		// 用 SetLooseGameplayTagCount(0) 而不是 RemoveLooseGameplayTag：
		// 后者只减 1，若因某种原因加了两次就清不干净。
		AbilitySystemComponent->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 0);
		AbilitySystemComponent->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dead, 0);
	}
}

float UGGYGOHealthComponent::GetHealth() const
{
	return HealthSet ? HealthSet->GetHealth() : 0.0f;
}

float UGGYGOHealthComponent::GetMaxHealth() const
{
	return HealthSet ? HealthSet->GetMaxHealth() : 0.0f;
}

float UGGYGOHealthComponent::GetHealthNormalized() const
{
	if (HealthSet)
	{
		const float Health = HealthSet->GetHealth();
		const float MaxHealth = HealthSet->GetMaxHealth();

		return (MaxHealth > 0.0f) ? (Health / MaxHealth) : 0.0f;
	}

	return 0.0f;
}

float UGGYGOHealthComponent::GetPoise() const
{
	return HealthSet ? HealthSet->GetPoise() : 0.0f;
}

float UGGYGOHealthComponent::GetMaxPoise() const
{
	return HealthSet ? HealthSet->GetMaxPoise() : 0.0f;
}

float UGGYGOHealthComponent::GetPoiseNormalized() const
{
	if (HealthSet)
	{
		const float Poise = HealthSet->GetPoise();
		const float MaxPoise = HealthSet->GetMaxPoise();

		return (MaxPoise > 0.0f) ? (Poise / MaxPoise) : 0.0f;
	}

	return 0.0f;
}

void UGGYGOHealthComponent::HandleHealthChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	OnHealthChanged.Broadcast(this, OldValue, NewValue, Instigator);
}

void UGGYGOHealthComponent::HandleMaxHealthChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	OnMaxHealthChanged.Broadcast(this, OldValue, NewValue, Instigator);
}

void UGGYGOHealthComponent::HandlePoiseChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	OnPoiseChanged.Broadcast(this, OldValue, NewValue, Instigator);
}

void UGGYGOHealthComponent::HandlePoiseBroken(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
	// 本组件只转发信号，不施加破韧硬直。
	// 硬直时长、能否被特定攻击强制破韧、破韧后的受击表现都是战斗设计的一部分，
	// 应由监听方（战斗组件或破韧 GA）决定。写在这里会把设计参数固化进底层组件。
	OnPoiseBroken.Broadcast(GetOwner());
}

void UGGYGOHealthComponent::HandleOutOfHealth(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue)
{
#if WITH_SERVER_CODE
	if (!AbilitySystemComponent || !Spec)
	{
		return;
	}

	// 发 GameplayEvent 而不是直接调 StartDeath()。
	// 死亡是一段有表现的流程（倒地动画、掉落、镜头），应该由一个死亡 GA 承载，
	// 而 GA 的启动方式在 GAS 里就是 GameplayEvent。
	// 本组件直接推进状态机会绕过 GA，那些表现就没有地方挂。
	FGameplayEventData Payload;
	Payload.EventTag = GGYGOGameplayTags::Event_Death;
	Payload.Instigator = Instigator;
	Payload.Target = AbilitySystemComponent->GetAvatarActor();
	Payload.OptionalObject = Spec->Def;
	Payload.ContextHandle = Spec->GetEffectContext();
	Payload.InstigatorTags = *Spec->CapturedSourceTags.GetAggregatedTags();
	Payload.TargetTags = *Spec->CapturedTargetTags.GetAggregatedTags();
	Payload.EventMagnitude = Magnitude;

	// 预测窗口：死亡 GA 可能带客户端预测的表现（受击僵直转倒地）。
	FScopedPredictionWindow NewScopedWindow(AbilitySystemComponent, true);
	AbilitySystemComponent->HandleGameplayEvent(Payload.EventTag, &Payload);
#endif // WITH_SERVER_CODE
}

void UGGYGOHealthComponent::OnRep_DeathState(EGGYGODeathState OldDeathState)
{
	const EGGYGODeathState NewDeathState = DeathState;

	// 先回滚到旧值：状态推进必须经过 StartDeath / FinishDeath，
	// 它们除了改状态还要施加 Tag 和广播事件。直接接受复制值会跳过那些副作用。
	DeathState = OldDeathState;

	if (OldDeathState > NewDeathState)
	{
		// 本地已经预测到比服务器更靠后的阶段。不回退 ——
		// 死亡表现已经播了，倒回去会出现"起身又倒下"。
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("OnRep_DeathState: [%s] 本地已预测超过服务器状态 [%d] -> [%d]，忽略。"),
			*GetNameSafe(GetOwner()), (uint8)OldDeathState, (uint8)NewDeathState);
		return;
	}

	// 把跳级变化补成逐级调用。
	// 两次状态变化可能被合并进同一个网络包（角色秒死时很常见），
	// 客户端直接看到 NotDead → DeathFinished，不补 StartDeath 就会丢掉整段死亡表现。
	if (OldDeathState == EGGYGODeathState::NotDead)
	{
		if (NewDeathState == EGGYGODeathState::DeathStarted)
		{
			StartDeath();
		}
		else if (NewDeathState == EGGYGODeathState::DeathFinished)
		{
			StartDeath();
			FinishDeath();
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("OnRep_DeathState: [%s] 非法状态转换 [%d] -> [%d]。"),
				*GetNameSafe(GetOwner()), (uint8)OldDeathState, (uint8)NewDeathState);
		}
	}
	else if (OldDeathState == EGGYGODeathState::DeathStarted)
	{
		if (NewDeathState == EGGYGODeathState::DeathFinished)
		{
			FinishDeath();
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("OnRep_DeathState: [%s] 非法状态转换 [%d] -> [%d]。"),
				*GetNameSafe(GetOwner()), (uint8)OldDeathState, (uint8)NewDeathState);
		}
	}

	ensureMsgf((DeathState == NewDeathState),
		TEXT("OnRep_DeathState: [%s] 状态机未能对齐服务器，期望 [%d] 实际 [%d]。"),
		*GetNameSafe(GetOwner()), (uint8)NewDeathState, (uint8)DeathState);
}

void UGGYGOHealthComponent::StartDeath()
{
	if (DeathState != EGGYGODeathState::NotDead)
	{
		return;
	}

	DeathState = EGGYGODeathState::DeathStarted;

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dying, 1);
	}

	AActor* Owner = GetOwner();
	check(Owner);

	OnDeathStarted.Broadcast(Owner);

	// 立即同步。死亡是玩家能立刻看到的事件，走默认复制节流会有可感知的延迟。
	Owner->ForceNetUpdate();
}

void UGGYGOHealthComponent::FinishDeath()
{
	if (DeathState != EGGYGODeathState::DeathStarted)
	{
		return;
	}

	DeathState = EGGYGODeathState::DeathFinished;

	if (AbilitySystemComponent)
	{
		// 保留 State.Dying 不清：两个 Tag 同时存在表示"死了且演出已结束"，
		// 而只有 Dead 没有 Dying 是个不该出现的状态。清掉由 ClearGameplayTags 统一做（复活时）。
		AbilitySystemComponent->SetLooseGameplayTagCount(GGYGOGameplayTags::State_Dead, 1);
	}

	AActor* Owner = GetOwner();
	check(Owner);

	OnDeathFinished.Broadcast(Owner);

	Owner->ForceNetUpdate();
}

void UGGYGOHealthComponent::DamageSelfDestruct(bool bFellOutOfWorld)
{
	if (DeathState != EGGYGODeathState::NotDead)
	{
		// 已经在死亡流程里，再补一次伤害没有意义。
		return;
	}

	if (!AbilitySystemComponent)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageSelfDestruct: [%s] 未绑定 ASC。"), *GetNameSafe(GetOwner()));
		return;
	}

	// 组件上的覆盖值优先，没配则用项目默认。
	TSubclassOf<UGameplayEffect> EffectToApply = SelfDestructEffectOverride;
	if (!EffectToApply)
	{
		if (const UGGYGOGameData* GameData = UGGYGOGameData::Get())
		{
			EffectToApply = GameData->SelfDestructGameplayEffect.LoadSynchronous();
		}
	}

	if (!EffectToApply)
	{
		// 显式报错而不是退化为直接改属性。直接改会绕过免疫判定与元属性消费，
		// 让"无敌帧内掉出世界"的行为与正常受伤不一致。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageSelfDestruct: [%s] 既没有 SelfDestructEffectOverride，GameData 里也没配 SelfDestructGameplayEffect。"),
			*GetNameSafe(GetOwner()));
		return;
	}

	FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
	Context.AddSourceObject(this);

	const FGameplayEffectSpecHandle SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(EffectToApply, 1.0f, Context);
	if (!SpecHandle.IsValid() || !SpecHandle.Data.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("DamageSelfDestruct: [%s] 无法为 [%s] 创建 GE Spec。"),
			*GetNameSafe(GetOwner()), *GetNameSafe(EffectToApply));
		return;
	}

	FGameplayEffectSpec* Spec = SpecHandle.Data.Get();

	// 必须加这个动态 Tag，否则自毁会被无敌帧和开发期 GodMode 挡住。
	// `UGGYGOHealthSet::PreGameplayEffectExecute` 检查的正是 spec 的动态资产 Tag：
	// 有 SelfDestruct 就跳过 Gameplay.Damage.Immunity 与 Cheat.GodMode 两道拦截。
	// 注意它对 bFellOutOfWorld 无条件生效 —— 自毁的定义就是"必须死成"。
	Spec->AddDynamicAssetTag(GGYGOGameplayTags::Gameplay_Damage_SelfDestruct);

	if (bFellOutOfWorld)
	{
		// 死因标记。免疫穿透已由上面的 SelfDestruct Tag 负责，这个 Tag 只用于区分死法，
		// 供死亡 Cue 与死亡消息选择表现（掉出世界不该播倒地动画）。
		Spec->AddDynamicAssetTag(GGYGOGameplayTags::Gameplay_Damage_FellOutOfWorld);
	}

	// 伤害量给足以致死的值。用当前最大生命而不是一个魔数，
	// 这样上限被 Buff 抬高时也仍然致死。
	const float DamageAmount = FMath::Max(GetMaxHealth(), 1.0f);
	Spec->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, DamageAmount);

	AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*Spec);
}
