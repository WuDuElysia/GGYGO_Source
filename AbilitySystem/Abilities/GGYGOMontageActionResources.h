/** @file GGYGOMontageActionResources.h @brief GA 持有的原 Montage、CMC 和表现资源 */
#pragma once

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Abilities/GGYGOMontageWindowFXTypes.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "FX/GGYGOMontageFXPublication.h"
#include "UObject/StrongObjectPtr.h"
#include "TimerManager.h"

class USkeletalMeshComponent;
class UWorld;

/** GA 持有的一次原播放及其 CMC／表现资源；来源和完成事实均来自原 Task／CMC。
 * 不管理 GAS 状态、动画时钟、曲线求值或输入政策。GA 先摘除本资源，再调用外部清理。 */
struct GGYGO_API FGGYGOMontageActionResources
{
	FGGYGOAbilityActivationHandle Activation;
	TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> Task;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
	FGGYGOActionMotionSourceBindingPtr Source;
	FGGYGOMovementOwnerSyncScopeId Scope;
	int32 MontageInstanceId = INDEX_NONE;
	int32 MotionHandle = INDEX_NONE;
	float EffectiveRate = 0.0f;
	float TranslationScale = 0.0f;
	FString AbilityPath;
	bool bMontageCompleted = false;
	bool bMotionCompleted = false;
	FGGYGOComboStepFX FX;
	FGGYGOMontageFXWindowDeclaration FXWindow;
	bool bFXBeginConsumed = false;
	bool bFXWindowClosed = false;
	bool bExpectFXBegin = false;

	/** 成功时返回唯一作者声明；Disabled 或失败时清空声明，不发布部分校验结果。 */
	static bool ValidateFXConfiguration(const UAnimMontage* Montage, const FGGYGOComboStepFX& Config,
		float RangeStart, float RangeEnd, FString& OutError,
		FGGYGOMontageFXWindowDeclaration* OutWindow = nullptr);
	bool AdoptTask(UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask);
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* GetTask() const;
	UGGYGOAbilityTask_PlayNiagaraEffect* GetFXTask() const;
	bool RegisterCallbacks(UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks);
	bool HasTaskRegistration() const { return TaskRegistration.IsValid(); }
	bool MatchesFailure(const FGGYGOMontageTaskFailureFact& Fact) const;
	bool ReadOriginalSnapshot(FGGYGOMontageSectionSnapshot& OutSnapshot, FString& OutError) const;
	bool BeginMotion(const FGGYGOMontageSectionSnapshot& Snapshot, FString& OutError);
	/** 将原段的朝向请求交给 CMC，不读取输入或保存方向策略。
	 * OutEntryActorRotation须为独立调用栈值；仅成功后由 GA 重核原段并保存。 */
	bool BeginMotion(const FGGYGOMontageSectionSnapshot& Snapshot,
		const FGGYGOActionMotionStartFacing& StartFacing, FQuat& OutEntryActorRotation, FString& OutError);
	bool ObserveMotion(FGGYGOActionMotionFailureDelegate Failure,
		FGGYGOActionMotionCompletionDelegate Completion, TFunctionRef<bool()> IsCurrent, FString& OutError);
	bool AcceptNaturalBlendOut(FString& OutError);
	bool AcceptMontageCompletion(FString& OutError);
	bool IsComplete() const { return bMontageCompleted && bMotionCompleted; }
	bool ArmWatchdog(USkeletalMeshComponent* Mesh, UWorld* World, float StartPosition, float GraceSeconds,
		FTimerDelegate Callback, FString& OutError);
	FTimerHandle GetWatchdogHandle() const { return WatchdogHandle; }
	void ClearWatchdog();
	/** 消费 GA 已在同步回调内经原 Task 认证的窗口事实；不重复解析资产或普通事件负载。
	 * IsCurrent 只在本调用栈核对原资源；异步启动观察者另持原资源弱身份。 */
	bool ConsumeFXWindow(UGGYGOGameplayAbility* Owner, FName FailureFunction,
		const FGGYGOMontageWindowFact& Fact,
		FGGYGOMontageFXLocalStartReadyDelegate ReadyCallback,
		TFunctionRef<bool()> IsCurrent, FString& OutError);
	/** Ready 事实只唤醒本原 Ticket；真实许可由 Task 当次消费，等待不等于 Spawn。 */
	bool ContinueFXLocalStart(const FGGYGOMontageFXLocalStartReadyFact& Fact,
		TFunctionRef<bool()> IsCurrent, FString& OutError);
	void ReleaseFX(bool bNormalExit);
	void ReleaseMotion(EGGYGOActionMotionReleaseReason Reason);
	void ReleaseTask(bool bOwnerEnded);

private:
	TStrongObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> TaskOwner;
	TStrongObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> FXTaskOwner;
	/** 同一作者窗逐单元持有原 lease；发布状态归 FX owner，终结不查当前 ActorInfo。 */
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> FXPublicationOwner;
	TArray<FGGYGOMontageFXLease> FXPublicationLeases;
	/** Task 签发的本地请求身份；许可及原生资源事实只由 FX 写入。 */
	FGGYGOMontageFXLocalStartTicket FXLocalStartTicket;
	/** 首次真实关闭的原因，用于发布外调重入时归还尚未接纳的原 lease。 */
	TOptional<EGGYGOMontageFXCloseReason> FXTerminalReason;
	bool PublishFXWindow(UGGYGOGameplayAbility* Owner, UGGYGOAbilitySystemComponent* Issuer, AActor* Avatar,
		const FGGYGOMontageWindowFact& Fact, const TArray<FGGYGONiagaraEffectUnit>& Units, const FTransform& PlaybackTransform,
		TFunctionRef<bool()> IsCurrent, FString& OutError);
	bool TryActivateFXLocalTask(TFunctionRef<bool()> IsCurrent, FString& OutError);
	/** 本 holder 安装的确切弱对象＋函数绑定；清理不移除其它订阅。 */
	FScriptDelegate FXFailureCallback;
	FDelegateHandle TaskRegistration;
	TWeakObjectPtr<UWorld> WatchdogWorld;
	FTimerHandle WatchdogHandle;
};
