#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "Character/GGYGOCharacterBase.h"
#include "GameplayEffect.h"

#include "GGYGOSquadSwitchTestTypes.generated.h"

class UGGYGOHealthComponent;
class UGGYGOInputComponent;
class UGGYGOInputTestHeroComponent;

/** Real Character/Extension/Health; only its fixture input component and observations differ. */
UCLASS(Transient)
class AGGYGOSquadSwitchTestPawn : public AGGYGOCharacterBase
{
	GENERATED_BODY()

public:
	AGGYGOSquadSwitchTestPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	UGGYGOInputTestHeroComponent* GetHeroForTest() const { return Hero; }
	UGGYGOInputComponent* GetInputComponentForTest() const;
	void ResetControlObservationsForTest();
	int32 PossessCalls = 0;
	int32 UnpossessCalls = 0;
	int32 ActiveCharacterNotices = 0;
	TWeakObjectPtr<AGGYGOCharacterBase> LastActiveCharacter;
	UFUNCTION()
	void ObserveActiveCharacterForTest(AGGYGOCharacterBase* NewCharacter);

protected:
	virtual UInputComponent* CreatePlayerInputComponent() override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void UnPossessed() override;

private:
	UPROPERTY()
	TObjectPtr<UGGYGOInputTestHeroComponent> Hero;
};

/** Configured cooldown uses a real duration GE and granted cooldown tag. */
UCLASS(Transient)
class UGGYGOSquadSwitchTestCooldown : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchTestCooldown(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** Holds a real authoritative GAS activation; observations never supply lifecycle proof. */
UCLASS(Transient)
class UGGYGOSquadSwitchTestAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()

public:
	UGGYGOSquadSwitchTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** Invoke the inherited project setter from the legal derived scope; no native bypass. */
	void SetCanBeCanceledForTest(bool bCanBeCanceled) { SetCanBeCanceled(bCanBeCanceled); }
	void ArmCleanupForTest(FSimpleDelegate Hook) { CleanupHook = MoveTemp(Hook); }
	void DisarmCleanupForTest() { CleanupHook.Unbind(); }
	/** 结束当前激活；不可取消的夹具实例先恢复可取消。 */
	bool EndForTest();
	int32 ActivationCalls = 0;
	int32 CleanupCalls = 0;
	int32 InputReleasedCalls = 0;
	int32 ControlLostCalls = 0;
	int32 ControlRegainedCalls = 0;
	/** 收到失去控制通知时 Spec 的 InputPressed；补发释放应已把它清掉。 */
	bool bInputPressedAtControlLost = false;
	/** 收到失去控制通知时 InputReleased 已触发的次数。 */
	int32 InputReleasedBeforeControlLost = -1;
	FGGYGOAbilityActivationHandle ActivatedOriginal;
	FGGYGOAbilityActivationHandle CleanedOriginal;

	virtual void InputReleased(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo) override;

protected:
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	virtual void NativeOnOwnerControlLost() override;
	virtual void NativeOnOwnerControlRegained() override;

private:
	FSimpleDelegate CleanupHook;
};

/** The fixture CDO explicitly selects the same policy that a character Blueprint configures. */
UCLASS(Transient)
class UGGYGOSquadSwitchContinueTestAbility : public UGGYGOSquadSwitchTestAbility
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchContinueTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** 按下时激活一次的默认 Cancel 能力，用于切回后的缓冲按下。 */
UCLASS(Transient)
class UGGYGOSquadSwitchTriggeredTestAbility : public UGGYGOSquadSwitchTestAbility
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchTriggeredTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** 由 Event.Squad.SwitchOut 触发的退场能力；保持激活直到测试结束或取消它。 */
UCLASS(Transient)
class UGGYGOSquadSwitchExitTestAbility : public UGGYGOSquadSwitchTestAbility
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchExitTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** 由 Event.Squad.SwitchIn 触发的进场能力。 */
UCLASS(Transient)
class UGGYGOSquadSwitchEnterTestAbility : public UGGYGOSquadSwitchTestAbility
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchEnterTestAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** 持续 60 秒、MaxHealth −20 的原生 GE。原生类按名字复制，客户端能解析活动 GE 的定义。 */
UCLASS(Transient)
class UGGYGOSquadSwitchTestMaxHealthDrop : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UGGYGOSquadSwitchTestMaxHealthDrop(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};

/** 记录一个 HealthComponent 广播的生命、上限、韧性变化与破韧；只观察，不改状态。 */
UCLASS(Transient)
class UGGYGOSquadSwitchHealthObserver : public UObject
{
	GENERATED_BODY()

public:
	void Observe(UGGYGOHealthComponent* InHealth);
	void Release();
	int32 HealthNotices = 0;
	float LastHealth = 0.0f;
	int32 MaxHealthNotices = 0;
	float LastMaxHealth = 0.0f;
	int32 PoiseNotices = 0;
	float LastPoise = 0.0f;
	int32 PoiseBrokenNotices = 0;

	UFUNCTION()
	void HandleHealthChanged(UGGYGOHealthComponent* HealthComponent, float OldValue, float NewValue, AActor* Instigator);
	UFUNCTION()
	void HandleMaxHealthChanged(UGGYGOHealthComponent* HealthComponent, float OldValue, float NewValue, AActor* Instigator);
	UFUNCTION()
	void HandlePoiseChanged(UGGYGOHealthComponent* HealthComponent, float OldValue, float NewValue, AActor* Instigator);
	UFUNCTION()
	void HandlePoiseBroken(AActor* OwningActor);

private:
	TWeakObjectPtr<UGGYGOHealthComponent> Observed;
};
