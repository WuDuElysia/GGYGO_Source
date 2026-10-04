/**
 * @file GGYGOHealthSet.cpp
 * @brief 承受侧属性集实现
 */
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameplayEffectExtension.h"
#include "Messages/GGYGOVerbMessage.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthSet)

enum class UGGYGOHealthSet::EQueuedResultType : uint8
{
	HealthChanged,
	MaxHealthChanged,
	OutOfHealth,
	PoiseChanged,
	PoiseBroken,
	DamageMessage,
	PoiseBreakMessage
};

struct UGGYGOHealthSet::FModifierFrame
{
	const FGameplayEffectModCallbackData* CallbackData = nullptr;
	FGameplayAttribute Attribute;
	float OriginalMagnitude = 0.0f;
	float MetaContribution = 0.0f;
	float MinimumHealth = 0.0f;
	bool bAwaitingInitialWrite = true;
	bool bMetaConsumed = false;
	TSharedPtr<FGameplayEffectSpec> EffectSpec;
	TWeakObjectPtr<AActor> OriginalInstigator;
	TWeakObjectPtr<AActor> EffectCauser;
	TWeakObjectPtr<AActor> Target;
	FGameplayTagContainer SourceTags;
	FGameplayTagContainer TargetTags;
};

struct UGGYGOHealthSet::FExpectedAttributeChange
{
	TSharedPtr<FModifierFrame> Frame;
	FGameplayAttribute Attribute;
	bool bConsumed = false;
};

struct UGGYGOHealthSet::FRepNotifyFrame
{
	enum class EStage : uint8
	{
		Undetermined,
		AwaitingFinalWrite,
		RealChanges
	};

	FGameplayAttribute Attribute;
	TWeakObjectPtr<AActor> OuterActor;
	TWeakObjectPtr<UAbilitySystemComponent> OwningASC;
	EStage Stage = EStage::Undetermined;
	float EffectiveNewValue = 0.0f;
	int32 ExplicitBaseChangeCount = 0;
	bool bEffectiveValueCaptured = false;
	bool bClassificationEnabled = true;
};

struct UGGYGOHealthSet::FQueuedResult
{
	EQueuedResultType Type = EQueuedResultType::HealthChanged;
	TSharedPtr<FModifierFrame> Frame;
	TWeakObjectPtr<AActor> Target;
	float Magnitude = 0.0f;
	float OldValue = 0.0f;
	float NewValue = 0.0f;
};

UGGYGOHealthSet::UGGYGOHealthSet()
	: Health(100.0f)
	, MaxHealth(100.0f)
	, Poise(100.0f)
	, MaxPoise(100.0f)
{
	bOutOfHealth = false;
	bPoiseBroken = false;
}

TSharedPtr<UGGYGOHealthSet::FModifierFrame> UGGYGOHealthSet::FindFrame(const FGameplayEffectModCallbackData& Data) const
{
	for (int32 Index = ModifierFrames.Num() - 1; Index >= 0; --Index)
	{
		if (ModifierFrames[Index].IsValid() && ModifierFrames[Index]->CallbackData == &Data)
		{
			return ModifierFrames[Index];
		}
	}
	return nullptr;
}

TSharedPtr<UGGYGOHealthSet::FModifierFrame> UGGYGOHealthSet::FindAwaitingFrame(const FGameplayAttribute& Attribute) const
{
	for (int32 Index = ModifierFrames.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<FModifierFrame>& Frame = ModifierFrames[Index];
		if (Frame.IsValid() && Frame->bAwaitingInitialWrite && Frame->Attribute == Attribute)
		{
			return Frame;
		}
	}
	return nullptr;
}

TSharedPtr<UGGYGOHealthSet::FRepNotifyFrame> UGGYGOHealthSet::BeginRepNotifyFrame(const FGameplayAttribute& Attribute)
{
	for (const TSharedPtr<FRepNotifyFrame>& ActiveFrame : RepNotifyFrames)
	{
		CanClassifyRepNotifyFrame(ActiveFrame);
	}
	TSharedPtr<FRepNotifyFrame> Frame = MakeShared<FRepNotifyFrame>();
	Frame->Attribute = Attribute;
	// UAttributeSet's owning-ASC getter CastChecked's Outer to Actor. Validate it first.
	AActor* OuterActor = Cast<AActor>(GetOuter());
	if (IsValid(OuterActor))
	{
		Frame->OuterActor = OuterActor;
		UAbilitySystemComponent* OwningASC = GetOwningAbilitySystemComponent();
		if (IsValid(OwningASC))
		{
			Frame->OwningASC = OwningASC;
		}
	}
	CanClassifyRepNotifyFrame(Frame);
	RepNotifyFrames.Add(Frame);
	return Frame;
}

void UGGYGOHealthSet::EndRepNotifyFrame(const TSharedPtr<FRepNotifyFrame>& Frame)
{
	const int32 FrameIndex = RepNotifyFrames.IndexOfByPredicate([&Frame](const TSharedPtr<FRepNotifyFrame>& Candidate)
	{
		return Candidate == Frame;
	});
	if (ensureMsgf(FrameIndex != INDEX_NONE, TEXT("HealthSet RepNotify frame disappeared before its macro scope exited.")))
	{
		RepNotifyFrames.RemoveAt(FrameIndex, 1, EAllowShrinking::No);
	}
}

bool UGGYGOHealthSet::CanClassifyRepNotifyFrame(const TSharedPtr<FRepNotifyFrame>& Frame) const
{
	if (!Frame.IsValid() || !Frame->bClassificationEnabled)
	{
		return false;
	}

	AActor* OuterActor = Cast<AActor>(GetOuter());
	if (!IsValid(OuterActor) || Frame->OuterActor.Get() != OuterActor || !Frame->OwningASC.IsValid())
	{
		Frame->bClassificationEnabled = false;
		return false;
	}

	UAbilitySystemComponent* OwningASC = GetOwningAbilitySystemComponent();
	if (!IsValid(OwningASC) || Frame->OwningASC.Get() != OwningASC
		|| OwningASC->GetAttributeSet(Frame->Attribute.GetAttributeSetClass()) != this)
	{
		// Once observed, a source/registration mismatch disables this frame until scope exit.
		Frame->bClassificationEnabled = false;
		return false;
	}
	return true;
}

TSharedPtr<UGGYGOHealthSet::FRepNotifyFrame> UGGYGOHealthSet::FindRepNotifyFrame(const FGameplayAttribute& Attribute) const
{
	TSharedPtr<FRepNotifyFrame> MatchingFrame;
	for (int32 Index = RepNotifyFrames.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<FRepNotifyFrame>& Frame = RepNotifyFrames[Index];
		// Validate outer frames too; a nested call must not hide an observed source mismatch.
		if (CanClassifyRepNotifyFrame(Frame) && !MatchingFrame.IsValid() && Frame->Attribute == Attribute)
		{
			MatchingFrame = Frame;
		}
	}
	return MatchingFrame;
}

void UGGYGOHealthSet::OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const
{
	Super::OnAttributeAggregatorCreated(Attribute, NewAggregator);
	for (const TSharedPtr<FRepNotifyFrame>& Frame : RepNotifyFrames)
	{
		if (CanClassifyRepNotifyFrame(Frame) && Frame->Attribute == Attribute
			&& Frame->Stage == FRepNotifyFrame::EStage::Undetermined)
		{
			// This macro entered without an aggregator. Its later dirty writes are real changes.
			// Mark every matching undecided frame, including outer frames of nested OnRep calls.
			Frame->Stage = FRepNotifyFrame::EStage::RealChanges;
		}
	}
}

void UGGYGOHealthSet::PushExpectedAttributeChange(const TSharedPtr<FModifierFrame>& Frame, const FGameplayAttribute& Attribute)
{
	TSharedPtr<FExpectedAttributeChange> Expected = MakeShared<FExpectedAttributeChange>();
	Expected->Frame = Frame;
	Expected->Attribute = Attribute;
	ExpectedAttributeChanges.Add(MoveTemp(Expected));
}

void UGGYGOHealthSet::PopExpectedAttributeChange()
{
	if (ensureMsgf(ExpectedAttributeChanges.Num() > 0, TEXT("HealthSet expected-attribute stack underflow.")))
	{
		ExpectedAttributeChanges.Pop(EAllowShrinking::No);
	}
}

TSharedPtr<UGGYGOHealthSet::FModifierFrame> UGGYGOHealthSet::ConsumeExpectedAttributeChange(const FGameplayAttribute& Attribute)
{
	if (ExpectedAttributeChanges.IsEmpty())
	{
		return nullptr;
	}

	const TSharedPtr<FExpectedAttributeChange> Expected = ExpectedAttributeChanges.Last();
	if (!Expected.IsValid() || Expected->Attribute != Attribute)
	{
		return nullptr;
	}

	if (Expected->bConsumed)
	{
		return nullptr;
	}

	Expected->bConsumed = true;
	return Expected->Frame;
}

void UGGYGOHealthSet::QueueAttributeResult(EQueuedResultType ResultType, const TSharedPtr<FModifierFrame>& Frame,
	float Magnitude, float OldValue, float NewValue)
{
	TSharedPtr<FQueuedResult> Result = MakeShared<FQueuedResult>();
	Result->Type = ResultType;
	Result->Frame = Frame;
	Result->Target = Frame.IsValid() ? Frame->Target : GetOwningActor();
	Result->Magnitude = Magnitude;
	Result->OldValue = OldValue;
	Result->NewValue = NewValue;
	PendingResults.Add(MoveTemp(Result));
}

void UGGYGOHealthSet::QueueMessageResult(const FGameplayTag& Verb, const TSharedPtr<FModifierFrame>& Frame, float Magnitude)
{
	const EQueuedResultType ResultType = Verb == GGYGOGameplayTags::Message_Damage
		? EQueuedResultType::DamageMessage : EQueuedResultType::PoiseBreakMessage;
	QueueAttributeResult(ResultType, Frame, Magnitude, 0.0f, 0.0f);
}

void UGGYGOHealthSet::FlushPendingResults()
{
	if (ModifierFrames.Num() > 0 || AttributeChangeDepth > 0 || PendingResults.IsEmpty())
	{
		return;
	}

	// 清空成员后再发通知。回调引发的新 GE 会取得独立 root frame 与结果缓冲。
	TArray<TSharedPtr<FQueuedResult>> Results;
	Swap(Results, PendingResults);

	for (const TSharedPtr<FQueuedResult>& Result : Results)
	{
		if (!Result.IsValid())
		{
			continue;
		}

		const FModifierFrame* Frame = Result->Frame.Get();
		const FGameplayEffectSpec* EffectSpec = Frame && Frame->EffectSpec.IsValid() ? Frame->EffectSpec.Get() : nullptr;
		AActor* OriginalInstigator = Frame ? Frame->OriginalInstigator.Get() : nullptr;
		AActor* EffectCauser = Frame ? Frame->EffectCauser.Get() : nullptr;

		switch (Result->Type)
		{
		case EQueuedResultType::DamageMessage:
		case EQueuedResultType::PoiseBreakMessage:
			if (UWorld* World = GetWorld(); World && UGameplayMessageSubsystem::HasInstance(this))
			{
				FGGYGOVerbMessage Message;
				Message.Verb = Result->Type == EQueuedResultType::DamageMessage
					? GGYGOGameplayTags::Message_Damage : GGYGOGameplayTags::Message_PoiseBreak;
				Message.Instigator = EffectCauser;
				Message.Target = Result->Target.Get();
				if (Frame)
				{
					Message.InstigatorTags = Frame->SourceTags;
					Message.TargetTags = Frame->TargetTags;
				}
				Message.Magnitude = Result->Magnitude;
				UGameplayMessageSubsystem::Get(World).BroadcastMessage(Message.Verb, Message);
			}
			break;
		case EQueuedResultType::HealthChanged:
			OnHealthChanged.Broadcast(OriginalInstigator, EffectCauser, EffectSpec, Result->Magnitude, Result->OldValue, Result->NewValue);
			break;
		case EQueuedResultType::MaxHealthChanged:
			OnMaxHealthChanged.Broadcast(OriginalInstigator, EffectCauser, EffectSpec, Result->Magnitude, Result->OldValue, Result->NewValue);
			break;
		case EQueuedResultType::OutOfHealth:
			OnOutOfHealth.Broadcast(OriginalInstigator, EffectCauser, EffectSpec, Result->Magnitude, Result->OldValue, Result->NewValue);
			break;
		case EQueuedResultType::PoiseChanged:
			OnPoiseChanged.Broadcast(OriginalInstigator, EffectCauser, EffectSpec, Result->Magnitude, Result->OldValue, Result->NewValue);
			break;
		case EQueuedResultType::PoiseBroken:
			OnPoiseBroken.Broadcast(OriginalInstigator, EffectCauser, EffectSpec, Result->Magnitude, Result->OldValue, Result->NewValue);
			break;
		default:
			break;
		}
	}
}

void UGGYGOHealthSet::ApplyModifierMinimumHealth(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute != GetHealthAttribute())
	{
		return;
	}

	const TSharedPtr<FModifierFrame> Frame = FindAwaitingFrame(Attribute);
	if (Frame.IsValid())
	{
		NewValue = FMath::Clamp(NewValue, Frame->MinimumHealth, GetMaxHealth());
	}
}

void UGGYGOHealthSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 只复制持久状态。三个元属性是一次性输入，复制它们没有意义且会浪费带宽。
	// REPNOTIFY_Always 保证即使数值相同也走 OnRep，让客户端有机会重新广播表现事件。
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, Poise, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGGYGOHealthSet, MaxPoise, COND_None, REPNOTIFY_Always);
}

void UGGYGOHealthSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	const float IncomingValue = GetHealth();
	const bool bWasOutOfHealth = bOutOfHealth;
	// No-aggregator 路径不会调用 PostAttributeChange，因此先提交 incoming 锁存；
	// 已有聚合器的临时回退不动锁存，最终重算会在原生委托前修正它。
	bOutOfHealth = IncomingValue <= 0.0f;
	const TSharedPtr<FRepNotifyFrame> RepFrame = BeginRepNotifyFrame(GetHealthAttribute());
	{
		ON_SCOPE_EXIT { EndRepNotifyFrame(RepFrame); };
		// Avoid the macro's checked owning-ASC lookup when the property has no live Actor/ASC source.
		if (RepFrame->OuterActor.IsValid() && RepFrame->OwningASC.IsValid())
		{
			GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, Health, OldValue);
		}
	}
	const float EffectiveHealth = RepFrame->bEffectiveValueCaptured ? RepFrame->EffectiveNewValue : IncomingValue;

	// OnRep 的旧值/新值是一对本次复制事实；宏内任何 GE 重入已经按各自属性回调提交状态。
	const float ReplicatedDelta = EffectiveHealth - OldValue.GetCurrentValue();

	// 客户端没有 EffectSpec，来源三参数只能传 nullptr。监听方必须判空。
	OnHealthChanged.Broadcast(nullptr, nullptr, nullptr, ReplicatedDelta, OldValue.GetCurrentValue(), EffectiveHealth);

	if (!bWasOutOfHealth && OldValue.GetCurrentValue() > 0.0f && EffectiveHealth <= 0.0f)
	{
		OnOutOfHealth.Broadcast(nullptr, nullptr, nullptr, ReplicatedDelta, OldValue.GetCurrentValue(), EffectiveHealth);
	}
}

void UGGYGOHealthSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)
{
	const float IncomingValue = GetMaxHealth();
	const TSharedPtr<FRepNotifyFrame> RepFrame = BeginRepNotifyFrame(GetMaxHealthAttribute());
	{
		ON_SCOPE_EXIT { EndRepNotifyFrame(RepFrame); };
		if (RepFrame->OuterActor.IsValid() && RepFrame->OwningASC.IsValid())
		{
			GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, MaxHealth, OldValue);
		}
	}
	const float EffectiveMaxHealth = RepFrame->bEffectiveValueCaptured ? RepFrame->EffectiveNewValue : IncomingValue;

	OnMaxHealthChanged.Broadcast(nullptr, nullptr, nullptr, EffectiveMaxHealth - OldValue.GetCurrentValue(), OldValue.GetCurrentValue(), EffectiveMaxHealth);
}

void UGGYGOHealthSet::OnRep_Poise(const FGameplayAttributeData& OldValue)
{
	const float IncomingValue = GetPoise();
	const bool bWasPoiseBroken = bPoiseBroken;
	bPoiseBroken = IncomingValue <= 0.0f;
	const TSharedPtr<FRepNotifyFrame> RepFrame = BeginRepNotifyFrame(GetPoiseAttribute());
	{
		ON_SCOPE_EXIT { EndRepNotifyFrame(RepFrame); };
		if (RepFrame->OuterActor.IsValid() && RepFrame->OwningASC.IsValid())
		{
			GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, Poise, OldValue);
		}
	}
	const float EffectivePoise = RepFrame->bEffectiveValueCaptured ? RepFrame->EffectiveNewValue : IncomingValue;

	const float ReplicatedDelta = EffectivePoise - OldValue.GetCurrentValue();

	OnPoiseChanged.Broadcast(nullptr, nullptr, nullptr, ReplicatedDelta, OldValue.GetCurrentValue(), EffectivePoise);

	if (!bWasPoiseBroken && OldValue.GetCurrentValue() > 0.0f && EffectivePoise <= 0.0f)
	{
		OnPoiseBroken.Broadcast(nullptr, nullptr, nullptr, ReplicatedDelta, OldValue.GetCurrentValue(), EffectivePoise);
	}
}

void UGGYGOHealthSet::OnRep_MaxPoise(const FGameplayAttributeData& OldValue)
{
	const TSharedPtr<FRepNotifyFrame> RepFrame = BeginRepNotifyFrame(GetMaxPoiseAttribute());
	{
		ON_SCOPE_EXIT { EndRepNotifyFrame(RepFrame); };
		if (RepFrame->OuterActor.IsValid() && RepFrame->OwningASC.IsValid())
		{
			GAMEPLAYATTRIBUTE_REPNOTIFY(UGGYGOHealthSet, MaxPoise, OldValue);
		}
	}
}

bool UGGYGOHealthSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
	if (!Super::PreGameplayEffectExecute(Data))
	{
		return false;
	}

	const FGameplayAttribute Attribute = Data.EvaluatedData.Attribute;
	const bool bIsDamageFromSelfDestruct = Data.EffectSpec.GetDynamicAssetTags().HasTagExact(GGYGOGameplayTags::Gameplay_Damage_SelfDestruct);

	// 只拦截正向伤害。负值不当作"伤害"处理，避免用负伤害绕过免疫来治疗。
	if (Attribute == GetDamageAttribute() && Data.EvaluatedData.Magnitude > 0.0f)
	{
		// 闪避无敌帧就是靠这个 Tag 生效的。
		if (Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity) && !bIsDamageFromSelfDestruct)
		{
			Data.EvaluatedData.Magnitude = 0.0f;
			return false;
		}

#if !UE_BUILD_SHIPPING
		if (Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_GodMode) && !bIsDamageFromSelfDestruct)
		{
			Data.EvaluatedData.Magnitude = 0.0f;
			return false;
		}
#endif
	}

	// 削韧同样受免疫约束：无敌帧期间不该被削韧。
	if (Attribute == GetPoiseDamageAttribute() && Data.EvaluatedData.Magnitude > 0.0f)
	{
		if (Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Gameplay_Damage_Immunity))
		{
			Data.EvaluatedData.Magnitude = 0.0f;
			return false;
		}
	}

	TSharedPtr<FModifierFrame> Frame = MakeShared<FModifierFrame>();
	Frame->CallbackData = &Data;
	Frame->Attribute = Attribute;
	Frame->OriginalMagnitude = Data.EvaluatedData.Magnitude;
	Frame->EffectSpec = MakeShared<FGameplayEffectSpec>(Data.EffectSpec);
	Frame->Target = GetOwningActor();

	const FGameplayEffectContextHandle& EffectContext = Data.EffectSpec.GetEffectContext();
	Frame->OriginalInstigator = EffectContext.GetOriginalInstigator();
	Frame->EffectCauser = EffectContext.GetEffectCauser();
	if (const FGameplayTagContainer* SourceTags = Data.EffectSpec.CapturedSourceTags.GetAggregatedTags())
	{
		Frame->SourceTags = *SourceTags;
	}
	if (const FGameplayTagContainer* TargetTags = Data.EffectSpec.CapturedTargetTags.GetAggregatedTags())
	{
		Frame->TargetTags = *TargetTags;
	}

#if !UE_BUILD_SHIPPING
	if (!bIsDamageFromSelfDestruct &&
		(Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_GodMode)
			|| Data.Target.HasMatchingGameplayTag(GGYGOGameplayTags::Cheat_UnlimitedHealth)))
	{
		Frame->MinimumHealth = 1.0f;
	}
#endif

	ModifierFrames.Add(MoveTemp(Frame));

	return true;
}

void UGGYGOHealthSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	const TSharedPtr<FModifierFrame> Frame = FindFrame(Data);
	if (!Frame.IsValid())
	{
		return;
	}

	Frame->bAwaitingInitialWrite = false;
	const FGameplayAttribute Attribute = Frame->Attribute;
	if (Attribute == GetDamageAttribute())
	{
		// Damage.Message 保留本 Modifier 的 EvaluatedData 原始幅度。结果暂存到 root 退出，
		// 因而消息回调能看到所有本次嵌套贡献已消费后的状态。
		if (Frame->OriginalMagnitude > 0.0f)
		{
			QueueMessageResult(GGYGOGameplayTags::Message_Damage, Frame, Frame->OriginalMagnitude);
		}

		if (!Frame->bMetaConsumed)
		{
			Frame->bMetaConsumed = true;
			const float Contribution = Frame->MetaContribution;
			PushExpectedAttributeChange(Frame, GetDamageAttribute());
			SetDamage(GetDamage() - Contribution);
			PopExpectedAttributeChange();

			PushExpectedAttributeChange(Frame, GetHealthAttribute());
			SetHealth(FMath::Clamp(GetHealth() - Contribution, Frame->MinimumHealth, GetMaxHealth()));
			PopExpectedAttributeChange();
		}
	}
	else if (Attribute == GetHealingAttribute())
	{
		if (!Frame->bMetaConsumed)
		{
			Frame->bMetaConsumed = true;
			const float Contribution = Frame->MetaContribution;
			PushExpectedAttributeChange(Frame, GetHealingAttribute());
			SetHealing(GetHealing() - Contribution);
			PopExpectedAttributeChange();

			PushExpectedAttributeChange(Frame, GetHealthAttribute());
			SetHealth(FMath::Clamp(GetHealth() + Contribution, Frame->MinimumHealth, GetMaxHealth()));
			PopExpectedAttributeChange();
		}
	}
	else if (Attribute == GetPoiseDamageAttribute())
	{
		if (!Frame->bMetaConsumed)
		{
			Frame->bMetaConsumed = true;
			const float Contribution = Frame->MetaContribution;
			PushExpectedAttributeChange(Frame, GetPoiseDamageAttribute());
			SetPoiseDamage(GetPoiseDamage() - Contribution);
			PopExpectedAttributeChange();

			PushExpectedAttributeChange(Frame, GetPoiseAttribute());
			SetPoise(FMath::Clamp(GetPoise() - Contribution, 0.0f, GetMaxPoise()));
			PopExpectedAttributeChange();
		}
	}

	const int32 FrameIndex = ModifierFrames.IndexOfByPredicate([&Frame](const TSharedPtr<FModifierFrame>& Candidate)
	{
		return Candidate == Frame;
	});
	if (ensureMsgf(FrameIndex != INDEX_NONE, TEXT("HealthSet modifier frame disappeared before Post.")))
	{
		ModifierFrames.RemoveAt(FrameIndex, 1, EAllowShrinking::No);
	}
	FlushPendingResults();
}

void UGGYGOHealthSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);

	ClampAttribute(Attribute, NewValue);
	ApplyModifierMinimumHealth(Attribute, NewValue);

	// Ordinary ASC base writes pass through this hook. RepNotify's internal rewind and
	// aggregator recompute write the numeric value directly and do not.
	if (const TSharedPtr<FRepNotifyFrame> RepFrame = FindRepNotifyFrame(Attribute))
	{
		++RepFrame->ExplicitBaseChangeCount;
	}
}

void UGGYGOHealthSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	ClampAttribute(Attribute, NewValue);
	ApplyModifierMinimumHealth(Attribute, NewValue);
}

void UGGYGOHealthSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	++AttributeChangeDepth;
	Super::PostAttributeChange(Attribute, OldValue, NewValue);

	const TSharedPtr<FRepNotifyFrame> ActiveRepFrame = FindRepNotifyFrame(Attribute);
	bool bExplicitBaseWrite = false;
	if (ActiveRepFrame.IsValid() && ActiveRepFrame->ExplicitBaseChangeCount > 0)
	{
		--ActiveRepFrame->ExplicitBaseChangeCount;
		bExplicitBaseWrite = true;
	}

	// 新嵌套 GE 的初始写入优先配对；其后才消费 HealthSet setter 的一次性标记。
	TSharedPtr<FModifierFrame> Frame = FindAwaitingFrame(Attribute);
	if (Frame.IsValid())
	{
		Frame->bAwaitingInitialWrite = false;
		if (Attribute == GetDamageAttribute() || Attribute == GetHealingAttribute() || Attribute == GetPoiseDamageAttribute())
		{
			Frame->MetaContribution = NewValue - OldValue;
		}
	}
	else
	{
		Frame = ConsumeExpectedAttributeChange(Attribute);
	}

	TSharedPtr<FRepNotifyFrame> RepFrame = Frame.IsValid() || bExplicitBaseWrite ? nullptr : ActiveRepFrame;
	if (RepFrame.IsValid() && RepFrame->Stage == FRepNotifyFrame::EStage::Undetermined)
	{
		// GAMEPLAYATTRIBUTE_REPNOTIFY with an Aggregator first rewinds to OldEvaluatedValue.
		// That temporary value is not a real result and must not affect caps or latches.
		RepFrame->Stage = FRepNotifyFrame::EStage::AwaitingFinalWrite;
		--AttributeChangeDepth;
		if (AttributeChangeDepth == 0)
		{
			FlushPendingResults();
		}
		return;
	}

	const bool bRepNotifyEffectiveWrite = RepFrame.IsValid() && RepFrame->Stage == FRepNotifyFrame::EStage::AwaitingFinalWrite;
	if (bRepNotifyEffectiveWrite)
	{
		// Capture the engine's final effective value once; later real writes cannot replace this history.
		RepFrame->bEffectiveValueCaptured = true;
		RepFrame->EffectiveNewValue = NewValue;
		RepFrame->Stage = FRepNotifyFrame::EStage::RealChanges;
	}
	const bool bSuppressProjectResults = bRepNotifyEffectiveWrite;
	const float EventMagnitude = Frame.IsValid() ? Frame->OriginalMagnitude : (NewValue - OldValue);

	// 上限下调时必须同步压低当前值，否则会出现 Health 大于 MaxHealth。
	// 走 ASC 的 ApplyModToAttribute 而不是直接 SetHealth，是为了让这次修改仍然经过
	// 完整的属性变更生命周期与通知。
	if (Attribute == GetMaxHealthAttribute())
	{
		if (GetHealth() > NewValue)
		{
			UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent();
			check(GGYGOASC);

			PushExpectedAttributeChange(Frame, GetHealthAttribute());
			GGYGOASC->ApplyModToAttribute(GetHealthAttribute(), EGameplayModOp::Override, NewValue);
			PopExpectedAttributeChange();
		}

		if (OldValue != NewValue && !bSuppressProjectResults)
		{
			QueueAttributeResult(EQueuedResultType::MaxHealthChanged, Frame, EventMagnitude, OldValue, NewValue);
		}
	}
	else if (Attribute == GetMaxPoiseAttribute())
	{
		if (GetPoise() > NewValue)
		{
			UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent();
			check(GGYGOASC);

			PushExpectedAttributeChange(Frame, GetPoiseAttribute());
			GGYGOASC->ApplyModToAttribute(GetPoiseAttribute(), EGameplayModOp::Override, NewValue);
			PopExpectedAttributeChange();
		}
	}

	if (Attribute == GetHealthAttribute())
	{
		if (bRepNotifyEffectiveWrite)
		{
			bOutOfHealth = NewValue <= 0.0f;
		}
		bool bNotifyOutOfHealth = false;
		if (NewValue > 0.0f)
		{
			bOutOfHealth = false;
		}
		else if (OldValue > 0.0f)
		{
			const bool bWasOutOfHealth = bOutOfHealth;
			bOutOfHealth = true;
			bNotifyOutOfHealth = !bWasOutOfHealth;
		}

		if (OldValue != NewValue && !bSuppressProjectResults)
		{
			QueueAttributeResult(EQueuedResultType::HealthChanged, Frame, EventMagnitude, OldValue, NewValue);
		}
		if (bNotifyOutOfHealth && !bSuppressProjectResults)
		{
			QueueAttributeResult(EQueuedResultType::OutOfHealth, Frame, EventMagnitude, OldValue, NewValue);
		}
	}
	else if (Attribute == GetPoiseAttribute())
	{
		if (bRepNotifyEffectiveWrite)
		{
			bPoiseBroken = NewValue <= 0.0f;
		}
		bool bNotifyPoiseBroken = false;
		float BreakMagnitude = FMath::Max(OldValue - NewValue, 0.0f);
		if (NewValue > 0.0f)
		{
			bPoiseBroken = false;
		}
		else if (OldValue > 0.0f)
		{
			const bool bWasPoiseBroken = bPoiseBroken;
			bPoiseBroken = true;
			bNotifyPoiseBroken = !bWasPoiseBroken;
			if (Frame.IsValid() && Frame->Attribute == GetPoiseDamageAttribute())
			{
				BreakMagnitude = Frame->OriginalMagnitude;
			}
		}

		if (OldValue != NewValue && !bSuppressProjectResults)
		{
			QueueAttributeResult(EQueuedResultType::PoiseChanged, Frame, EventMagnitude, OldValue, NewValue);
		}
		if (bNotifyPoiseBroken && !bSuppressProjectResults)
		{
			QueueAttributeResult(EQueuedResultType::PoiseBroken, Frame, BreakMagnitude, OldValue, NewValue);
			QueueMessageResult(GGYGOGameplayTags::Message_PoiseBreak, Frame, BreakMagnitude);
		}
	}

	--AttributeChangeDepth;
	if (AttributeChangeDepth == 0)
	{
		FlushPendingResults();
	}
}

void UGGYGOHealthSet::ClampAttribute(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		// 上限至少 1，否则 Health 的 Clamp 区间会退化成 [0, 0]。
		NewValue = FMath::Max(NewValue, 1.0f);
	}
	else if (Attribute == GetPoiseAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxPoise());
	}
	else if (Attribute == GetMaxPoiseAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
}
