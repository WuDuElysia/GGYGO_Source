/**
 * @file GGYGOHealthSet.cpp
 * @brief 承受侧属性集实现
 */
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameplayMessageSubsystem.h"
#include "GameplayEffectAggregator.h"
#include "GameplayEffectExtension.h"
#include "Messages/GGYGOVerbMessage.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include <limits>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOHealthSet)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOHealthMessage, Log, All);

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

enum class UGGYGOHealthSet::EMessageDeliveryResult : uint8
{
	Delivered,
	Retired,
	DependencyFailure
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
	bool bClientMaxReevaluation = false;
	bool bObservedWrite = false;
	bool bRejectedInput = false;
	float ObservedCurrent = 0.0f;
};

struct UGGYGOHealthSet::FNetReceiveFrame
{
	TWeakObjectPtr<AActor> Owner;
	TWeakObjectPtr<UAbilitySystemComponent> ASC;
	bool bReevaluateHealth = false;
	bool bReevaluatePoise = false;
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

TSharedPtr<UGGYGOHealthSet::FExpectedAttributeChange> UGGYGOHealthSet::PushExpectedAttributeChange(
	const TSharedPtr<FModifierFrame>& Frame, const FGameplayAttribute& Attribute)
{
	TSharedPtr<FExpectedAttributeChange> Expected = MakeShared<FExpectedAttributeChange>();
	Expected->Frame = Frame;
	Expected->Attribute = Attribute;
	ExpectedAttributeChanges.Add(Expected);
	return Expected;
}

void UGGYGOHealthSet::PopExpectedAttributeChange()
{
	if (ensureMsgf(ExpectedAttributeChanges.Num() > 0, TEXT("HealthSet expected-attribute stack underflow.")))
	{
		ExpectedAttributeChanges.Pop(EAllowShrinking::No);
	}
}

TSharedPtr<UGGYGOHealthSet::FExpectedAttributeChange> UGGYGOHealthSet::ConsumeExpectedAttributeChange(const FGameplayAttribute& Attribute)
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
	return Expected;
}

bool UGGYGOHealthSet::IsCurrentAttributeSource(const FGameplayAttribute& Attribute,
	const AActor* Owner, const UAbilitySystemComponent* ASC) const
{
	const auto IdentityIsCurrent = [this, &Attribute, Owner, ASC]()
	{
		return IsValid(this) && !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& IsValid(Owner) && !Owner->IsActorBeingDestroyed() && GetOuter() == Owner
			&& IsValid(ASC) && !ASC->IsBeingDestroyed() && !ASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& ASC->GetAttributeSet(Attribute.GetAttributeSetClass()) == this;
	};
	if (!IdentityIsCurrent())
	{
		return false;
	}
	// An owner's GAS interface can run project code. Revalidate after querying it.
	return GetOwningAbilitySystemComponent() == ASC && IdentityIsCurrent();
}

bool UGGYGOHealthSet::TrySetCurrentValue(const FGameplayAttribute& Attribute, float DesiredCurrent,
	const TSharedPtr<FModifierFrame>& Frame)
{
	AActor* Owner = Cast<AActor>(GetOuter());
	UGGYGOAbilitySystemComponent* ASC = IsValid(Owner) ? Cast<UGGYGOAbilitySystemComponent>(GetOwningAbilitySystemComponent()) : nullptr;
	if (!IsCurrentAttributeSource(Attribute, Owner, ASC)
		|| FScopedAggregatorOnDirtyBatch::GlobalBatchCount != 0)
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] CurrentWriteRejected Reason=InvalidSourceOrOpenBatch Set=%s ASC=%s Attribute=%s Effect=%s"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(),
			*GetPathNameSafe(Frame.IsValid() && Frame->EffectSpec.IsValid() ? Frame->EffectSpec->Def.Get() : nullptr));
		return false;
	}

	const float ExpectedCurrent = ASC->GetNumericAttribute(Attribute);
	const FGGYGOAttributeBaseCalculationResult Calculation = ASC->TryCalculateNumericAttributeBaseForCurrentValue(
		this, Attribute, ExpectedCurrent, DesiredCurrent);
	if (Calculation.Outcome != EGGYGOAttributeBaseCalculationOutcome::Ready
		|| Calculation.Reason != EGGYGOAttributeBaseCalculationReason::None)
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] CurrentWriteRejected Outcome=%d Reason=%d Set=%s ASC=%s Attribute=%s Effect=%s Current=%g Desired=%g"),
			static_cast<int32>(Calculation.Outcome), static_cast<int32>(Calculation.Reason),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(),
			*GetPathNameSafe(Frame.IsValid() && Frame->EffectSpec.IsValid() ? Frame->EffectSpec->Def.Get() : nullptr),
			ExpectedCurrent, DesiredCurrent);
		return false;
	}

	const TSharedPtr<FExpectedAttributeChange> Expected = PushExpectedAttributeChange(Frame, Attribute);
	ASC->SetNumericAttributeBase(Attribute, Calculation.CalculatedBase);
	PopExpectedAttributeChange();
	// Native delegates can already have made later writes. Only this marker's Post fact counts.
	const double NativeFloatTolerance = FMath::Max(static_cast<double>(UE_KINDA_SMALL_NUMBER),
		4.0 * std::numeric_limits<float>::epsilon() * FMath::Max(1.0, FMath::Abs(static_cast<double>(DesiredCurrent))));
	if (!Expected->bObservedWrite || Expected->bRejectedInput || !FMath::IsFinite(Expected->ObservedCurrent)
		|| FMath::Abs(static_cast<double>(Expected->ObservedCurrent) - DesiredCurrent) > NativeFloatTolerance)
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] CurrentWriteUnconfirmed Set=%s ASC=%s Attribute=%s Effect=%s Observed=%d Current=%g Desired=%g"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(),
			*GetPathNameSafe(Frame.IsValid() && Frame->EffectSpec.IsValid() ? Frame->EffectSpec->Def.Get() : nullptr),
			Expected->bObservedWrite, Expected->ObservedCurrent, DesiredCurrent);
		return false;
	}
	return true;
}

void UGGYGOHealthSet::PreNetReceive()
{
	const TSharedPtr<FNetReceiveFrame> Frame = MakeShared<FNetReceiveFrame>();
	Frame->Owner = Cast<AActor>(GetOuter());
	if (Frame->Owner.IsValid())
	{
		Frame->ASC = GetOwningAbilitySystemComponent();
	}
	NetReceiveFrames.Add(Frame);
	Super::PreNetReceive();
}

void UGGYGOHealthSet::PostNetReceive()
{
	const TSharedPtr<FNetReceiveFrame> Frame = NetReceiveFrames.IsEmpty() ? nullptr : NetReceiveFrames.Last();
	// This drains native dirty work. Requests raised by that work still belong to this receive.
	Super::PostNetReceive();
	if (!Frame.IsValid())
	{
		UE_LOG(LogGGYGOHealthMessage, Error, TEXT("[Messages][HealthSet] NetReceiveSourceMissing Set=%s"), *GetPathNameSafe(this));
		return;
	}
	NetReceiveFrames.RemoveSingle(Frame);
	// Retire the request before invoking GAS, so nested callbacks cannot replay it.
	if (Frame->bReevaluateHealth)
	{
		ReevaluateClientMax(GetHealthAttribute(), Frame->Owner, Frame->ASC);
	}
	if (Frame->bReevaluatePoise)
	{
		ReevaluateClientMax(GetPoiseAttribute(), Frame->Owner, Frame->ASC);
	}
}

void UGGYGOHealthSet::RequestClientMaxReevaluation(const FGameplayAttribute& Attribute)
{
	AActor* Owner = Cast<AActor>(GetOuter());
	UAbilitySystemComponent* ASC = IsValid(Owner) ? GetOwningAbilitySystemComponent() : nullptr;
	if (!NetReceiveFrames.IsEmpty())
	{
		const TSharedPtr<FNetReceiveFrame>& Frame = NetReceiveFrames.Last();
		if (Frame->Owner.Get() == Owner && Frame->ASC.Get() == ASC && IsCurrentAttributeSource(Attribute, Owner, ASC))
		{
			Frame->bReevaluateHealth |= Attribute == GetHealthAttribute();
			Frame->bReevaluatePoise |= Attribute == GetPoiseAttribute();
			return;
		}
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] ClientMaxRequestRejected Reason=SourceChanged Set=%s ASC=%s Attribute=%s"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName());
		return;
	}
	ReevaluateClientMax(Attribute, Owner, ASC);
}

void UGGYGOHealthSet::ReevaluateClientMax(const FGameplayAttribute& Attribute,
	const TWeakObjectPtr<AActor>& Owner, const TWeakObjectPtr<UAbilitySystemComponent>& SourceASC)
{
	UAbilitySystemComponent* ASC = SourceASC.Get();
	if (!IsCurrentAttributeSource(Attribute, Owner.Get(), ASC))
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] ClientMaxRequestRetired Reason=SourceChanged Set=%s ASC=%s Attribute=%s"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName());
		return;
	}
	if (ASC->IsOwnerActorAuthoritative() || Owner->HasAuthority() || FScopedAggregatorOnDirtyBatch::GlobalBatchCount != 0)
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] ClientMaxRequestRejected Reason=AuthorityOrOpenBatch Set=%s ASC=%s Attribute=%s"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName());
		return;
	}
	const float Current = ASC->GetNumericAttribute(Attribute);
	const float Base = ASC->GetNumericAttributeBase(Attribute);
	const float Maximum = Attribute == GetHealthAttribute() ? GetMaxHealth() : GetMaxPoise();
	if (!FMath::IsFinite(Current) || !FMath::IsFinite(Base) || !FMath::IsFinite(Maximum) || Maximum < 1.0f)
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] ClientMaxRequestRejected Reason=InvalidNumericSource Set=%s ASC=%s Attribute=%s"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName());
		return;
	}
	if (Current <= Maximum)
	{
		return;
	}
	const TSharedPtr<FExpectedAttributeChange> Expected = PushExpectedAttributeChange(nullptr, Attribute);
	Expected->bClientMaxReevaluation = true;
	// This explicitly selected client mode asks native GAS to reevaluate the same real Base.
	// It does not reverse the replicated Current or write a cached projected value.
	ASC->SetNumericAttributeBase(Attribute, Base);
	PopExpectedAttributeChange();
	if (!Expected->bObservedWrite || Expected->bRejectedInput || !FMath::IsFinite(Expected->ObservedCurrent)
		|| Expected->ObservedCurrent < 0.0f || Expected->ObservedCurrent > Maximum)
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] ClientMaxWriteUnconfirmed Set=%s ASC=%s Attribute=%s Observed=%d Current=%g Max=%g"),
			*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(),
			Expected->bObservedWrite, Expected->ObservedCurrent, Maximum);
	}
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

UGGYGOHealthSet::EMessageDeliveryResult UGGYGOHealthSet::PublishMessageResult(const FQueuedResult& Result)
{
	const FModifierFrame* Frame = Result.Frame.Get();
	const FGameplayTag Verb = Result.Type == EQueuedResultType::DamageMessage
		? GGYGOGameplayTags::Message_Damage : GGYGOGameplayTags::Message_PoiseBreak;
	AActor* Owner = Cast<AActor>(GetOuter());
	UWorld* World = nullptr;
	UGameInstance* GameInstance = nullptr;
	const auto FailDelivery = [&](uint8 ReasonBit, const TCHAR* Reason)
	{
		if ((ReportedMessageDependencyFailures & ReasonBit) == 0)
		{
			ReportedMessageDependencyFailures |= ReasonBit;
			const UGameplayEffect* Effect = Frame && Frame->EffectSpec.IsValid() ? Frame->EffectSpec->Def.Get() : nullptr;
			UE_LOG(LogGGYGOHealthMessage, Error,
				TEXT("[Messages][HealthSet] DeliveryFailed Reason=%s Set=%s Owner=%s World=%s GameInstance=%s Verb=%s Effect=%s"),
				Reason, *GetPathNameSafe(this), *GetPathNameSafe(Owner), *GetPathNameSafe(World),
				*GetPathNameSafe(GameInstance), *Verb.ToString(), *GetPathNameSafe(Effect));
		}
		return EMessageDeliveryResult::DependencyFailure;
	};

	// Null context and PreBeginPlay are not destruction evidence. Check real lifecycle facts first,
	// including the owner's world before a derived GetWorld() can report missing context.
	if (HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| (GetOuter() && GetOuter()->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		|| (Owner && Owner->IsActorBeingDestroyed()))
	{
		return EMessageDeliveryResult::Retired;
	}
	if (!IsValid(Owner))
	{
		return FailDelivery(1 << 0, TEXT("InvalidOwner"));
	}
	const UWorld* OwnerWorld = Owner->GetWorld();
	if (OwnerWorld && (OwnerWorld->bIsTearingDown || OwnerWorld->IsBeingCleanedUp() || OwnerWorld->IsCleanedUp()
		|| OwnerWorld->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)))
	{
		return EMessageDeliveryResult::Retired;
	}

	World = GetWorld();
	if (World && (World->bIsTearingDown || World->IsBeingCleanedUp() || World->IsCleanedUp()
		|| World->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)))
	{
		return EMessageDeliveryResult::Retired;
	}
	if (!IsValid(World))
	{
		return FailDelivery(1 << 1, TEXT("MissingWorld"));
	}
	GameInstance = World->GetGameInstance();
	if (GameInstance && GameInstance->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return EMessageDeliveryResult::Retired;
	}
	if (!IsValid(GameInstance))
	{
		return FailDelivery(1 << 2, TEXT("MissingGameInstance"));
	}
	UGameplayMessageSubsystem* Router = UGameInstance::GetSubsystem<UGameplayMessageSubsystem>(GameInstance);
	if (!IsValid(Router))
	{
		return FailDelivery(1 << 3, TEXT("MissingRouter"));
	}

	ReportedMessageDependencyFailures = 0;
	FGGYGOVerbMessage Message;
	Message.Verb = Verb;
	Message.Instigator = Frame ? Frame->EffectCauser.Get() : nullptr;
	Message.Target = Result.Target.Get();
	if (Frame)
	{
		Message.InstigatorTags = Frame->SourceTags;
		Message.TargetTags = Frame->TargetTags;
	}
	Message.Magnitude = Result.Magnitude;
	Router->BroadcastMessage(Message.Verb, Message);
	return EMessageDeliveryResult::Delivered;
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
			// Delivery failure/retirement does not undo settlement or skip attribute delegates.
			PublishMessageResult(*Result);
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

	TSharedPtr<FModifierFrame> Frame = FindAwaitingFrame(Attribute);
	if (!Frame.IsValid() && !ExpectedAttributeChanges.IsEmpty())
	{
		const TSharedPtr<FExpectedAttributeChange>& Expected = ExpectedAttributeChanges.Last();
		if (Expected.IsValid() && !Expected->bConsumed && Expected->Attribute == Attribute)
		{
			Frame = Expected->Frame;
		}
	}
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
	// Without an aggregator the native macro only broadcasts, so no PostAttributeChange
	// requests this dependency. Always inspect the final source after the macro too.
	if (CanClassifyRepNotifyFrame(RepFrame) && !RepFrame->OwningASC->IsOwnerActorAuthoritative()
		&& GetHealth() > GetMaxHealth())
	{
		RequestClientMaxReevaluation(GetHealthAttribute());
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
	if (CanClassifyRepNotifyFrame(RepFrame) && !RepFrame->OwningASC->IsOwnerActorAuthoritative()
		&& GetPoise() > GetMaxPoise())
	{
		RequestClientMaxReevaluation(GetPoiseAttribute());
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
	if (Attribute == GetHealthAttribute() || Attribute == GetPoiseAttribute()
		|| Attribute == GetMaxHealthAttribute() || Attribute == GetMaxPoiseAttribute())
	{
		const float ProspectiveBase = FAggregator::StaticExecModOnBaseValue(
			Data.Target.GetNumericAttributeBase(Attribute), Data.EvaluatedData.ModifierOp, Data.EvaluatedData.Magnitude);
		if (!FMath::IsFinite(Data.EvaluatedData.Magnitude) || !FMath::IsFinite(ProspectiveBase)
			|| (Data.EvaluatedData.ModifierOp == EGameplayModOp::DivideAdditive && FMath::IsNearlyZero(Data.EvaluatedData.Magnitude)))
		{
			UE_LOG(LogGGYGOHealthMessage, Error,
				TEXT("[Messages][HealthSet] DirectResourceRejected Reason=InvalidNumericInput Set=%s ASC=%s Attribute=%s Effect=%s"),
				*GetPathNameSafe(this), *GetPathNameSafe(&Data.Target), *Attribute.GetName(), *GetPathNameSafe(Data.EffectSpec.Def.Get()));
			return false;
		}
	}

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

	if (Attribute == GetDamageAttribute() || Attribute == GetHealingAttribute() || Attribute == GetPoiseDamageAttribute())
	{
		UGGYGOAbilitySystemComponent* ASC = Cast<UGGYGOAbilitySystemComponent>(&Data.Target);
		const FGameplayAttribute Resource = Attribute == GetPoiseDamageAttribute() ? GetPoiseAttribute() : GetHealthAttribute();
		if (!ASC || !FMath::IsFinite(Data.EvaluatedData.Magnitude)
			|| !FMath::IsFinite(Data.Target.GetNumericAttribute(Attribute))
			|| FScopedAggregatorOnDirtyBatch::GlobalBatchCount != 0)
		{
			UE_LOG(LogGGYGOHealthMessage, Error,
				TEXT("[Messages][HealthSet] MetaRejected Reason=InvalidInputOrOpenBatch Set=%s ASC=%s Attribute=%s Effect=%s"),
				*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(), *GetPathNameSafe(Data.EffectSpec.Def.Get()));
			return false;
		}
		const float Current = ASC->GetNumericAttribute(Resource);
		if (Data.EvaluatedData.ModifierOp == EGameplayModOp::DivideAdditive && FMath::IsNearlyZero(Data.EvaluatedData.Magnitude))
		{
			UE_LOG(LogGGYGOHealthMessage, Error,
				TEXT("[Messages][HealthSet] MetaRejected Reason=InvalidDivisor Set=%s ASC=%s Attribute=%s Effect=%s"),
				*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(), *GetPathNameSafe(Data.EffectSpec.Def.Get()));
			return false;
		}
		const float ProspectiveMeta = FAggregator::StaticExecModOnBaseValue(
			ASC->GetNumericAttributeBase(Attribute), Data.EvaluatedData.ModifierOp, Data.EvaluatedData.Magnitude);
		const double ProspectiveContribution = static_cast<double>(ProspectiveMeta) - ASC->GetNumericAttribute(Attribute);
		if (!FMath::IsFinite(ProspectiveMeta) || !FMath::IsFinite(ProspectiveContribution)
			|| FMath::Abs(ProspectiveContribution) > MAX_flt)
		{
			UE_LOG(LogGGYGOHealthMessage, Error,
				TEXT("[Messages][HealthSet] MetaRejected Reason=NonFiniteMetaResult Set=%s ASC=%s Attribute=%s Effect=%s"),
				*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(), *GetPathNameSafe(Data.EffectSpec.Def.Get()));
			return false;
		}
		// Validate support before accepting the meta write. This candidate is never saved or committed.
		const FGGYGOAttributeBaseCalculationResult Preflight = ASC->TryCalculateNumericAttributeBaseForCurrentValue(
			this, Resource, Current, Current);
		if (Preflight.Outcome != EGGYGOAttributeBaseCalculationOutcome::Ready
			|| Preflight.Reason != EGGYGOAttributeBaseCalculationReason::None)
		{
			UE_LOG(LogGGYGOHealthMessage, Error,
				TEXT("[Messages][HealthSet] MetaRejected Outcome=%d Reason=%d Set=%s ASC=%s Attribute=%s Resource=%s Effect=%s"),
				static_cast<int32>(Preflight.Outcome), static_cast<int32>(Preflight.Reason),
				*GetPathNameSafe(this), *GetPathNameSafe(ASC), *Attribute.GetName(), *Resource.GetName(),
				*GetPathNameSafe(Data.EffectSpec.Def.Get()));
			return false;
		}
	}

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

			const float DesiredCurrent = static_cast<float>(FMath::Clamp(static_cast<double>(GetHealth()) - Contribution,
				static_cast<double>(Frame->MinimumHealth), static_cast<double>(GetMaxHealth())));
			if (!TrySetCurrentValue(GetHealthAttribute(), DesiredCurrent, Frame))
			{
				// Preserve original queue order, but never publish a rejected frame as settled damage.
				PendingResults.RemoveAll([&Frame](const TSharedPtr<FQueuedResult>& Result)
				{
					return Result.IsValid() && Result->Frame == Frame && Result->Type == EQueuedResultType::DamageMessage;
				});
			}
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

			const float DesiredCurrent = static_cast<float>(FMath::Clamp(static_cast<double>(GetHealth()) + Contribution,
				static_cast<double>(Frame->MinimumHealth), static_cast<double>(GetMaxHealth())));
			TrySetCurrentValue(GetHealthAttribute(), DesiredCurrent, Frame);
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

			const float DesiredCurrent = static_cast<float>(FMath::Clamp(static_cast<double>(GetPoise()) - Contribution,
				0.0, static_cast<double>(GetMaxPoise())));
			TrySetCurrentValue(GetPoiseAttribute(), DesiredCurrent, Frame);
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

	// Health/Poise Base is internal aggregation input, not the visible resource boundary.
	// A finite negative Base may be required by an active additive effect.
	if (!FMath::IsFinite(NewValue))
	{
		// Native setters have a void Pre hook. Decline the invalid Base, retain its prior value,
		// and report failure explicitly; never choose a replacement gameplay value.
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] BaseWriteRejected Reason=NonFiniteInput Set=%s Attribute=%s Requested=%g"),
			*GetPathNameSafe(this), *Attribute.GetName(), NewValue);
		NewValue = Attribute.GetGameplayAttributeDataChecked(this)->GetBaseValue();
		if (!ExpectedAttributeChanges.IsEmpty())
		{
			const TSharedPtr<FExpectedAttributeChange>& Expected = ExpectedAttributeChanges.Last();
			if (Expected.IsValid() && !Expected->bConsumed && Expected->Attribute == Attribute) { Expected->bRejectedInput = true; }
		}
	}
	if (Attribute != GetHealthAttribute() && Attribute != GetPoiseAttribute())
	{
		ClampAttribute(Attribute, NewValue);
	}

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

	if (!FMath::IsFinite(NewValue))
	{
		UE_LOG(LogGGYGOHealthMessage, Error,
			TEXT("[Messages][HealthSet] CurrentWriteRejected Reason=NonFiniteAggregate Set=%s Attribute=%s Requested=%g"),
			*GetPathNameSafe(this), *Attribute.GetName(), NewValue);
		NewValue = Attribute.GetGameplayAttributeDataChecked(this)->GetCurrentValue();
		if (!ExpectedAttributeChanges.IsEmpty())
		{
			const TSharedPtr<FExpectedAttributeChange>& Expected = ExpectedAttributeChanges.Last();
			if (Expected.IsValid() && !Expected->bConsumed && Expected->Attribute == Attribute) { Expected->bRejectedInput = true; }
		}
	}
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
	TSharedPtr<FExpectedAttributeChange> Expected;
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
		Expected = ConsumeExpectedAttributeChange(Attribute);
		if (Expected.IsValid())
		{
			Frame = Expected->Frame;
			Expected->bObservedWrite = true;
			Expected->ObservedCurrent = NewValue;
		}
	}

	TSharedPtr<FRepNotifyFrame> RepFrame = Frame.IsValid() || Expected.IsValid() || bExplicitBaseWrite ? nullptr : ActiveRepFrame;
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

	// Authority commits a Current target through the inverse calculation. Client replication
	// retains its real Base and reevaluates it after the native receive batch has drained.
	if (Attribute == GetMaxHealthAttribute())
	{
		if (GetHealth() > NewValue)
		{
			UGGYGOAbilitySystemComponent* GGYGOASC = GetGGYGOAbilitySystemComponent();
			check(GGYGOASC);

			if (GGYGOASC->IsOwnerActorAuthoritative())
			{
				TrySetCurrentValue(GetHealthAttribute(), NewValue, Frame);
			}
			else
			{
				RequestClientMaxReevaluation(GetHealthAttribute());
			}
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

			if (GGYGOASC->IsOwnerActorAuthoritative())
			{
				TrySetCurrentValue(GetPoiseAttribute(), NewValue, Frame);
			}
			else
			{
				RequestClientMaxReevaluation(GetPoiseAttribute());
			}
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
			if (!Expected.IsValid() || !Expected->bClientMaxReevaluation)
			{
				QueueMessageResult(GGYGOGameplayTags::Message_PoiseBreak, Frame, BreakMagnitude);
			}
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
