/** @file GGYGOHealthAcceptanceTestTypes.h @brief 原角色生命与死亡广播的只读验收观察者。 */
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GGYGOHealthAcceptanceTestTypes.generated.h"

class UGGYGOHealthComponent;
class UGGYGOAbilitySystemComponent;
class UGGYGOHealthSet;

/** 回调当时的值；原 Pawn 退役后仍可核对，不能用替补的属性覆盖它。 */
struct FGGYGOHealthAcceptanceEvent
{
	FString Kind;
	float OldValue = 0.0f;
	float NewValue = 0.0f;
	float ASCHealth = 0.0f;
	float ComponentHealth = 0.0f;
	uint8 DeathState = 0;
	bool bOriginalActor = false;
};

UCLASS(Transient)
class UGGYGOHealthAcceptanceObserver : public UObject
{
	GENERATED_BODY()
public:
	void Observe(UGGYGOHealthComponent* InHealth, UGGYGOAbilitySystemComponent* InASC, const FString& InSide);
	void Release();
	int32 Count(const TCHAR* Kind) const;
	const FGGYGOHealthAcceptanceEvent* Find(const TCHAR* Kind) const;
	TArray<FGGYGOHealthAcceptanceEvent> Events;

private:
	void Record(const TCHAR* Kind, float OldValue, float NewValue, AActor* OriginalActor);
	UFUNCTION() void HealthChanged(UGGYGOHealthComponent* Sender, float OldValue, float NewValue, AActor* Instigator);
	UFUNCTION() void PoiseChanged(UGGYGOHealthComponent* Sender, float OldValue, float NewValue, AActor* Instigator);
	UFUNCTION() void PoiseBroken(AActor* Actor);
	UFUNCTION() void DeathStarted(AActor* Actor);
	UFUNCTION() void DeathFinished(AActor* Actor);
	TWeakObjectPtr<UGGYGOHealthComponent> Health;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<const UGGYGOHealthSet> Set;
	FDelegateHandle OutOfHealthHandle;
	FString Side;
};
