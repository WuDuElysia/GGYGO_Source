/** @file GGYGOPlayerComboAbility.h @brief 一个 GAS 能力管理逐段 Main→End 普攻 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Abilities/GGYGOComboTypes.h"
#include "Components/SkinnedMeshComponent.h"
#include "GGYGOPlayerComboAbility.generated.h"

class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOAbilityTask_WaitComboInput;
class UGGYGOMeleeTraceComponent;
class USkeletalMeshComponent;

UCLASS(Blueprintable)
class GGYGO_API UGGYGOPlayerComboAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()
public:
	UGGYGOPlayerComboAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** ASC 只向匹配本次激活预测键的实例转发服务器纠正。 */
	void CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
		bool bWindowOpen, bool bWindowClosed, bool bAccepted);
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	int32 GetCurrentComboStep() const { return CurrentStep; }
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool IsComboWindowOpen() const { return Window.bOpen; }
	UFUNCTION(BlueprintPure, Category = "GGYGO|Combo")
	bool HasPendingComboRequest() const { return Window.PendingRequestId > 0; }
	bool ValidateComboConfiguration(FString& OutError) const;
#if WITH_EDITOR
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif

protected:
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
	bool IsStepPlayable(int32 Index) const;
	bool StartStep(int32 Index, float Position = 0.0f);
	void ReleaseMontageTask();
	void TryAdvanceCombo();
	void RejectRequest(int32 RequestId);
	void SendAuthoritativeStep(int32 RequestId, bool bAccepted);
	void HandleWatchdog();
	UFUNCTION()
	void HandleInputPressed(int32 SourceStep, int32 RequestId);
	UFUNCTION()
	void HandleMontageEvent(FGameplayTag EventTag, FGameplayEventData EventData);
	UFUNCTION()
	void HandleMontageCompleted(FGameplayTag EventTag, FGameplayEventData EventData);
	UFUNCTION()
	void HandleMontageInterrupted(FGameplayTag EventTag, FGameplayEventData EventData);
	UFUNCTION()
	void HandleMontageBlendOut(FGameplayTag EventTag, FGameplayEventData EventData);
	UFUNCTION()
	void HandleMeleeHit(AActor* HitActor, const FHitResult& HitResult);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	TArray<FGGYGOComboStep> ComboSteps;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InputBufferSeconds = 0.35f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	TSubclassOf<UGameplayEffect> DamageEffect;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo", meta = (Categories = "GameplayCue.Hit"))
	FGameplayTag HitCueTag;

private:
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> MontageTask;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilityTask_WaitComboInput> InputTask;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMeleeTraceComponent> TraceComponent;
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> ActiveMesh;
	FGGYGOComboWindowState Window;
	FTimerHandle WatchdogHandle;
	int32 CurrentStep = INDEX_NONE;
	int32 LastRequestId = 0;
	int32 StepSyncRevision = 0;
	bool bCleaningUp = false;
	bool bChangedMeshTick = false;
	bool bSavedUpdateRateOptimizations = false;
	EVisibilityBasedAnimTickOption SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
};
