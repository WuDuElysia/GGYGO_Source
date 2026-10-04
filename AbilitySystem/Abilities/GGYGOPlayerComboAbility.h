/** @file GGYGOPlayerComboAbility.h @brief 一个 GAS 能力管理逐段 Main→End 普攻 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Abilities/GGYGOComboTypes.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "GGYGOPlayerComboAbility.generated.h"

class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
class UGGYGOAbilityTask_WaitComboInput;
class UGGYGOMeleeTraceComponent;
class USkeletalMeshComponent;
class UWorld;
struct FGameplayAbilityTargetDataHandle;
struct FGGYGOPlayerComboLifecycleFixture;

UCLASS(Blueprintable)
class GGYGO_API UGGYGOPlayerComboAbility : public UGGYGOGameplayAbility
{
	GENERATED_BODY()
public:
	UGGYGOPlayerComboAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	/** ASC 只向匹配本次激活预测键的实例转发服务器纠正。 */
	void CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
		bool bWindowOpen, bool bWindowClosed, bool bAccepted);
	virtual void ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction) override;
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
	virtual bool CanActivateAbilityAdditional(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags,
		FGameplayTagContainer* OptionalRelevantTags) const override;
	virtual void NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const override;
	virtual void InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original) override;
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context) override;
	bool IsStepPlayable(int32 Index) const;
	bool StartStep(const FGGYGOAbilityActivationHandle& Original, int32 Index, float Position = 0.0f);
	void ReleaseMontageTask(const FGGYGOAbilityActivationHandle& Original);
	void OpenTraceWindow(const FGGYGOAbilityActivationHandle& Original);
	void ReleaseTraceWindow(const FGGYGOAbilityActivationHandle& Original);
	void TryAdvanceCombo(const FGGYGOAbilityActivationHandle& Original);
	void RejectRequest(const FGGYGOAbilityActivationHandle& Original, int32 RequestId);
	void SendAuthoritativeStep(const FGGYGOAbilityActivationHandle& Original, int32 RequestId, bool bAccepted);
	void HandleWatchdog(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken);
	bool IsActivationCurrent(const FGGYGOAbilityActivationHandle& Original) const;
	bool IsStepCurrent(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken,
		const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ExpectedTask) const;
	void HandleInputPressed(const FGGYGOAbilityActivationHandle& Original, int32 SourceStep, int32 RequestId);
	void HandleMontageEvent(const FGGYGOAbilityActivationHandle& Original, FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMontageCompleted(const FGGYGOAbilityActivationHandle& Original, FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMontageInterrupted(const FGGYGOAbilityActivationHandle& Original, FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMontageBlendOut(const FGGYGOAbilityActivationHandle& Original, FGameplayTag EventTag, FGameplayEventData EventData);
	void HandleMeleeHit(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken,
		const FGGYGOMeleeTraceWindowHandle& OriginalWindow, AActor* HitActor, const FHitResult& HitResult);
	void CorrectPredictedStepForActivation(const FGGYGOAbilityActivationHandle& Original,
		int32 Revision, int32 RequestId, int32 ServerStep, float Position,
		bool bWindowOpen, bool bWindowClosed, bool bAccepted);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	TArray<FGGYGOComboStep> ComboSteps;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float InputBufferSeconds = 0.35f;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	TSubclassOf<UGameplayEffect> DamageEffect;
	/** 仅DamageEffect为空时选择共享预载GE；默认false为明确无GE、仍播放命中Cue的模式。
	 *  非空DamageEffect始终优先；必需GE在准入或提交前不可用/非法时拒绝激活。
	 *  命中期间必需GE无效或载荷构造失败时，中止原激活且不发送该命中的Cue。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo")
	bool bUseSharedDamageEffectWhenUnset = false;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Combo", meta = (Categories = "GameplayCue.Hit"))
	FGameplayTag HitCueTag;

private:
	friend struct FGGYGOPlayerComboLifecycleFixture;
	/** 验证本次选择的GE依赖，可输出同一次解析的成功结果；成功时空结果仅用于显式无GE模式。
	 *  不加载、重试、缓存或记录激活状态；失败清空输出。 */
	bool ValidateDamageEffectDependency(FString& OutError, TSubclassOf<UGameplayEffect>* OutResolvedEffect = nullptr) const;

	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> MontageTask;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOAbilityTask_WaitComboInput> InputTask;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMeleeTraceComponent> TraceComponent;
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMeshComponent> ActiveMesh;
	/** Copies of issuer-owned identity and our resources, never another activation/end state machine. */
	FGGYGOAbilityActivationHandle ResourceActivation;
	FDelegateHandle MontageCallbackRegistration;
	FDelegateHandle InputCallbackRegistration;
	FGGYGOMeleeTraceWindowHandle TraceWindow;
	FDelegateHandle TraceHitSubscription;
	TWeakObjectPtr<UWorld> OriginalWorld;
	FGGYGOComboWindowState Window;
	FTimerHandle WatchdogHandle;
	int32 CurrentStep = INDEX_NONE;
	int32 LastRequestId = 0;
	int32 StepSyncRevision = 0;
	bool bChangedMeshTick = false;
	bool bAddedMeshPrerequisite = false;
	bool bSavedUpdateRateOptimizations = false;
	uint64 StepTokenCounter = 0;
	uint64 CurrentStepToken = 0;
	EVisibilityBasedAnimTickOption SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
};
