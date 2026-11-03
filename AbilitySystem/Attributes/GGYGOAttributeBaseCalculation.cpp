/**
 * @file GGYGOAttributeBaseCalculation.cpp
 * @brief 属性 Base 反算实现
 */
#include "AbilitySystem/Attributes/GGYGOAttributeBaseCalculation.h"

#include "AbilitySystem/GGYGOAttributeCalculation.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "GameplayEffectAggregator.h"

FGGYGOAttributeBaseCalculationResult GGYGOAttributeBase::CalculateBaseForCurrent(
	UAbilitySystemComponent* ASC, const UAttributeSet* ExpectedAttributeSet,
	const FGameplayAttribute& Attribute, float ExpectedCurrent, float DesiredCurrent)
{
	using EOutcome = EGGYGOAttributeBaseCalculationOutcome;
	using EReason = EGGYGOAttributeBaseCalculationReason;
	const auto Fail = [](EOutcome Outcome, EReason Reason)
	{
		FGGYGOAttributeBaseCalculationResult Result;
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		return Result;
	};
	if (!IsInGameThread()) { return Fail(EOutcome::Rejected, EReason::WrongThread); }
	if (!IsValid(ASC) || ASC->IsBeingDestroyed() || ASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Fail(EOutcome::Rejected, EReason::InvalidASC);
	}
	AActor* ComponentOwner = ASC->GetOwner();
	if (!IsValid(ComponentOwner) || ComponentOwner->IsActorBeingDestroyed())
	{
		return Fail(EOutcome::Rejected, EReason::InvalidASC);
	}
	if (!ASC->IsOwnerActorAuthoritative() || !ComponentOwner->HasAuthority())
	{
		return Fail(EOutcome::Rejected, EReason::NotAuthority);
	}
	if (!Attribute.IsValid() || Attribute.IsSystemAttribute())
	{
		return Fail(EOutcome::Rejected, EReason::InvalidAttribute);
	}
	// Struct attributes have distinct native Base/Current storage. Legacy float projection is not
	// a supported business mode and must not enter native ReverseEvaluate's float fallback.
	if (!FGameplayAttribute::IsGameplayAttributeDataProperty(Attribute.GetUProperty()))
	{
		return Fail(EOutcome::Rejected, EReason::UnsupportedAttribute);
	}
	if (!FMath::IsFinite(ExpectedCurrent) || !FMath::IsFinite(DesiredCurrent))
	{
		return Fail(EOutcome::Rejected, EReason::NonFiniteInput);
	}
	if (!IsValid(ExpectedAttributeSet) || ExpectedAttributeSet->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| ASC->GetAttributeSet(Attribute.GetAttributeSetClass()) != ExpectedAttributeSet)
	{
		return Fail(EOutcome::Rejected, EReason::MissingAttributeSet);
	}
	AActor* SetOwner = Cast<AActor>(ExpectedAttributeSet->GetOuter());
	if (!IsValid(SetOwner) || SetOwner->IsActorBeingDestroyed()
		|| ExpectedAttributeSet->GetOwningAbilitySystemComponent() != ASC)
	{
		return Fail(EOutcome::Rejected, EReason::MissingAttributeSet);
	}
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalASC(ASC);
	const TWeakObjectPtr<const UAttributeSet> OriginalSet(ExpectedAttributeSet);
	const TWeakObjectPtr<AActor> OriginalComponentOwner(ComponentOwner);
	const TWeakObjectPtr<AActor> OriginalSetOwner(SetOwner);
	// The owning-ASC query above can invoke the owner's interface. Do not read native default/missing
	// attribute values if that call retired or replaced the original host.
	if (!OriginalASC.IsValid() || !OriginalSet.IsValid() || !OriginalComponentOwner.IsValid() || !OriginalSetOwner.IsValid()
		|| ASC->GetAttributeSet(Attribute.GetAttributeSetClass()) != OriginalSet.Get()
		|| ASC->GetOwner() != OriginalComponentOwner.Get())
	{
		return Fail(EOutcome::Stale, EReason::SourceChanged);
	}
	const float OriginalBase = ASC->GetNumericAttributeBase(Attribute);
	if (!FMath::IsFinite(OriginalBase) || !FMath::IsFinite(ASC->GetNumericAttribute(Attribute)))
	{
		return Fail(EOutcome::Rejected, EReason::NonFiniteSource);
	}
	const auto SourceIdentityIsCurrent = [&Attribute, &OriginalASC, &OriginalSet, &OriginalComponentOwner,
		&OriginalSetOwner]()
	{
		const UAbilitySystemComponent* Component = OriginalASC.Get();
		const UAttributeSet* Set = OriginalSet.Get();
		const AActor* Owner = OriginalComponentOwner.Get();
		const AActor* AttributeOwner = OriginalSetOwner.Get();
		return IsValid(Component) && !Component->IsBeingDestroyed() && !Component->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& IsValid(Set) && !Set->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& IsValid(Owner) && !Owner->IsActorBeingDestroyed() && Owner->HasAuthority()
			&& IsValid(AttributeOwner) && !AttributeOwner->IsActorBeingDestroyed()
			&& Component->GetOwner() == Owner && Set->GetOuter() == AttributeOwner && Component->IsOwnerActorAuthoritative()
			&& Component->GetAttributeSet(Attribute.GetAttributeSetClass()) == Set;
	};
	const auto SourceIsCurrent = [&Attribute, &OriginalASC, &OriginalSet, &SourceIdentityIsCurrent,
		OriginalBase, ExpectedCurrent]()
	{
		if (!SourceIdentityIsCurrent()) { return false; }
		// The generated attribute setter resolves ASC through this very owner interface. Recheck it
		// too, and repeat the pure identity check after any code invoked by the owner's query.
		if (OriginalSet.Get()->GetOwningAbilitySystemComponent() != OriginalASC.Get() || !SourceIdentityIsCurrent()) { return false; }
		return OriginalASC->GetNumericAttributeBase(Attribute) == OriginalBase
			&& OriginalASC->GetNumericAttribute(Attribute) == ExpectedCurrent;
	};
	if (!SourceIsCurrent()) { return Fail(EOutcome::Stale, EReason::SourceChanged); }

	const FGameplayEffectAttributeCaptureDefinition Definition(Attribute,
		EGameplayEffectAttributeCaptureSource::Target, false);
	FGameplayEffectAttributeCaptureSpec Capture(Definition);
	ASC->CaptureAttributeForGameplayEffect(Capture);
	if (!SourceIsCurrent()) { return Fail(EOutcome::Stale, EReason::SourceChanged); }
	if (!Capture.HasValidCapture()) { return Fail(EOutcome::Rejected, EReason::CaptureFailed); }
	// Matches authority OnAttributeAggregatorDirty: null execution tags, no predicted modifiers.
	const FAggregatorEvaluateParameters Parameters;
	TMap<EGameplayModEvaluationChannel, const TArray<FAggregatorMod>*> LiveMods;
	const bool bGathered = Capture.AttemptGatherAttributeMods(Parameters, LiveMods);
	if (!SourceIsCurrent()) { return Fail(EOutcome::Stale, EReason::SourceChanged); }
	FAggregator Snapshot;
	if (!bGathered || !Capture.AttemptGetAttributeAggregatorSnapshot(Snapshot))
	{
		return Fail(EOutcome::Rejected, EReason::CaptureFailed);
	}
	if (Snapshot.GetBaseValue() != OriginalBase) { return Fail(EOutcome::Stale, EReason::SourceChanged); }
	FAggregatorModChannelContainer Channels;
	TArray<EGameplayModEvaluationChannel> Order;
	const EReason Preparation = GGYGOAttributeCalculation::PrepareNativeAttributeCalculationChannels(Snapshot, Parameters, Channels, Order);
	if (Preparation != EReason::None) { return Fail(EOutcome::Rejected, Preparation); }
	if (!FMath::IsFinite(Channels.EvaluateWithBase(OriginalBase, Parameters)))
	{
		return Fail(EOutcome::Rejected, EReason::NonFiniteResult);
	}
	float CalculatedBase = DesiredCurrent;
	for (int32 Index = Order.Num() - 1; Index >= 0; --Index)
	{
		float PreviousChannelValue = 0.0f;
		if (!Channels.FindOrAddModChannel(Order[Index]).ReverseEvaluate(CalculatedBase, Parameters, PreviousChannelValue))
		{
			return Fail(EOutcome::Rejected, EReason::NonInvertibleChannel);
		}
		if (!FMath::IsFinite(PreviousChannelValue)) { return Fail(EOutcome::Rejected, EReason::NonFiniteResult); }
		CalculatedBase = PreviousChannelValue;
	}
	if (!GGYGOAttributeCalculation::IsAttributeForwardValueConfirmed(Channels.EvaluateWithBase(CalculatedBase, Parameters), DesiredCurrent))
	{
		return Fail(EOutcome::Rejected, EReason::ForwardMismatch);
	}

	// Recapture the actual current aggregator, not merely the retained original ref: removing/readding
	// an attribute set can retire a ref without invalidating its native capture. Native evaluation may
	// call custom qualification code, so recheck both registration and frozen aggregation afterwards.
	FGameplayEffectAttributeCaptureSpec CurrentCapture(Definition);
	ASC->CaptureAttributeForGameplayEffect(CurrentCapture);
	if (!SourceIsCurrent()) { return Fail(EOutcome::Stale, EReason::SourceChanged); }
	float NativeForwardValue = 0.0f;
	const bool bEvaluated = CurrentCapture.AttemptCalculateAttributeMagnitudeWithBase(
		Parameters, CalculatedBase, NativeForwardValue);
	if (!SourceIsCurrent()) { return Fail(EOutcome::Stale, EReason::SourceChanged); }
	FAggregator CurrentSnapshot;
	if (!bEvaluated || !CurrentCapture.AttemptGetAttributeAggregatorSnapshot(CurrentSnapshot))
	{
		return Fail(EOutcome::Rejected, EReason::CaptureFailed);
	}
	if (!GGYGOAttributeCalculation::HasSameAttributeAggregation(Snapshot, CurrentSnapshot))
	{
		return Fail(EOutcome::Stale, EReason::AggregationChanged);
	}
	if (!FMath::IsFinite(NativeForwardValue)) { return Fail(EOutcome::Rejected, EReason::NonFiniteResult); }
	if (!GGYGOAttributeCalculation::IsAttributeForwardValueConfirmed(NativeForwardValue, DesiredCurrent))
	{
		return Fail(EOutcome::Rejected, EReason::ForwardMismatch);
	}
	FGGYGOAttributeBaseCalculationResult Result;
	Result.Outcome = EOutcome::Ready;
	Result.Reason = EReason::None;
	Result.CalculatedBase = CalculatedBase;
	return Result;
}
