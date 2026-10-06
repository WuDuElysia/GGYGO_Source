#pragma once

#include "Abilities/GameplayAbility.h"
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "GameplayCueNotify_Static.h"

#include "GGYGOAvatarActorInfoTransactionTestTypes.generated.h"

DECLARE_DELEGATE_TwoParams(FGGYGOAvatarActorInfoNativeHook,
	const FGameplayAbilityActorInfo*, const FGameplayAbilitySpec&);

/** A real per-actor GAS instance; the native avatar callback can be armed once after GiveAbility. */
UCLASS(Transient)
class UGGYGOAvatarActorInfoTransactionTestAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOAvatarActorInfoTransactionTestAbility(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void ArmNativeAvatarHookForTest(FGGYGOAvatarActorInfoNativeHook InHook);
	void DisarmNativeAvatarHookForTest();
	int32 GetArmedNativeCallsForTest() const { return ArmedNativeCalls; }

protected:
	virtual void OnAvatarSet(const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilitySpec& Spec) override;

private:
	FGGYGOAvatarActorInfoNativeHook NativeAvatarHook;
	int32 ArmedNativeCalls = 0;
};

DECLARE_DELEGATE(FGGYGOAvatarBindingPublicationPawnHook);

/** Publication probe records the real callback; an optional one-shot hook drives ownership tests. */
UCLASS(Transient)
class UGGYGOAvatarBindingPublicationTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOAvatarBindingPublicationTestAbility(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void ArmPawnNoticeHookForTest(FGGYGOAvatarBindingPublicationPawnHook InHook);
	void DisarmPawnNoticeHookForTest();
	int32 GetPawnNoticeCallsForTest() const { return PawnNoticeCalls; }
	TWeakObjectPtr<AActor> GetLastNotifiedAvatarForTest() const { return LastNotifiedAvatar; }
	void HoldControlledActivationForTest();
	int32 GetControlledCleanupCallsForTest() const { return ControlledCleanupCalls; }
	const FGGYGOAbilityActivationHandle& GetLastCleanupActivationForTest() const { return LastCleanupActivation; }

protected:
	virtual void OnPawnAvatarSet() override;
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;

private:
	int32 PawnNoticeCalls = 0;
	TWeakObjectPtr<AActor> LastNotifiedAvatar;
	FGGYGOAvatarBindingPublicationPawnHook PawnNoticeHook;
	bool bHoldControlledActivation = false;
	int32 ControlledCleanupCalls = 0;
	FGGYGOAbilityActivationHandle LastCleanupActivation;
};

/** Dedicated authoritative activation fixture; its own CDO and instances use the same policy. */
UCLASS(Transient)
class UGGYGOAvatarBindingRefreshEndTestAbility : public UGGYGOAvatarBindingPublicationTestAbility
{
	GENERATED_BODY()

public:
	UGGYGOAvatarBindingRefreshEndTestAbility(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Ordinary native GAS probe: real activation/cancellation, no project admission or custom callbacks. */
UCLASS(Transient)
class UGGYGOAvatarBindingCancelTestAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOAvatarBindingCancelTestAbility(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

DECLARE_DELEGATE_TwoParams(FGGYGOAvatarBindingCueRemovalHook,
	AActor*, const FGameplayCueParameters&);

/** Native CueSet calls the inherited dispatcher; this probe observes its real event implementations. */
UCLASS(Transient)
class UGGYGOAvatarBindingCueRemovalTestNotify : public UGameplayCueNotify_Static
{
	GENERATED_BODY()

public:
	UGGYGOAvatarBindingCueRemovalTestNotify(
		const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	void ArmRemovedHookForTest(FGGYGOAvatarBindingCueRemovalHook InHook);
	void DisarmRemovedHookForTest();
	bool IsRemovedHookArmedForTest() const { return RemovedHook.IsBound(); }
	int32 GetWhileActiveCallsForTest() const { return WhileActiveCalls; }
	int32 GetRemovedCallsForTest() const { return RemovedCalls; }
	TWeakObjectPtr<AActor> GetLastWhileActiveTargetForTest() const { return LastWhileActiveTarget; }
	TWeakObjectPtr<AActor> GetLastRemovedTargetForTest() const { return LastRemovedTarget; }

protected:
	virtual bool WhileActive_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;
	virtual bool OnRemove_Implementation(AActor* MyTarget, const FGameplayCueParameters& Parameters) const override;

private:
	mutable int32 WhileActiveCalls = 0;
	mutable int32 RemovedCalls = 0;
	mutable TWeakObjectPtr<AActor> LastWhileActiveTarget;
	mutable TWeakObjectPtr<AActor> LastRemovedTarget;
	mutable FGGYGOAvatarBindingCueRemovalHook RemovedHook;
};
