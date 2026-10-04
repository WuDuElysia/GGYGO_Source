#pragma once

#include "AbilitySystem/Tests/GGYGOHealthMessageTestTypes.h"

#include "GGYGOHealthRepNotifyPersistentEffectTestTypes.generated.h"

enum class EGGYGOHealthRepNotifyObservation : uint8
{
	AggregatorCreated,
	PostAttributeChange,
	PreGameplayEffectExecute,
	PreAttributeBaseChange
};

/** Value-only observations; no aggregator, delegate-data or effect-spec pointers are retained. */
struct FGGYGOHealthRepNotifyObservation
{
	EGGYGOHealthRepNotifyObservation Kind;
	FGameplayAttribute Attribute;
	float OldValue = 0.0f;
	float NewValue = 0.0f;
	bool bAggregatorWasNonNull = false;
};

/** Reuses the public OnRep simulation; the four overrides only record and call Super. */
UCLASS(Transient)
class UGGYGOHealthRepNotifyPersistentEffectTestSet : public UGGYGOHealthMessageRepNotifyTestSet
{
	GENERATED_BODY()

public:
	mutable TArray<FGGYGOHealthRepNotifyObservation> Observations;

protected:
	virtual void OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const override;
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
};
