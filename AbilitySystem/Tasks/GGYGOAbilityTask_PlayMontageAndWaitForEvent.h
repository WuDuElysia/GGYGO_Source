/**
 * @file GGYGOAbilityTask_PlayMontageAndWaitForEvent.h
 * @brief 播放 Montage 并同时等待动画里发出的 GameplayEvent
 *
 * 一个攻击能力需要同时知道两件事：Montage 播到哪了（结束/被打断），
 * 以及动画在什么时刻通知了什么（判定窗口开、连段窗口开、可取消点）。
 *
 * ## 为什么不用两个现成的 Task 拼
 * 引擎有 `PlayMontageAndWait` 与 `WaitGameplayEvent`，但把它们并联会出问题：
 * 两个 Task 的生命周期各自独立，Montage 被打断时事件 Task 还在等，
 * 于是后续动画里的同名事件会被这个已经过期的等待接到。
 *
 * 合成一个 Task 后，Montage 一结束事件监听就一起结束，不会有跨动画的串音。
 *
 * ## 四种结束路径的区别
 * | 委托 | 触发时机 | 能力通常该做什么 |
 * |---|---|---|
 * | `OnCompleted`   | 原实例播到自然结尾 | 调用方验证原能力生命周期后正常收尾 |
 * | `OnBlendOut`    | 开始混出（还没播完） | 提前允许下一个动作衔接 |
 * | `OnInterrupted` | 被别的 Montage 顶掉 | 中断收尾，不结算未完成的判定 |
 * | `OnCancelled`   | 能力自身被取消 | 同上，且要清理已施加的状态 |
 *
 * 分开而不是合成一个"结束了"回调，是因为动作游戏里这四种情况的后续处理不同：
 * 自然结束要结算收招硬直，被打断则不该结算。
 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "AbilitySystem/GGYGOAbilityMontagePlaybackTypes.h"
#include "GameplayTagContainer.h"

#include "GGYGOAbilityTask_PlayMontageAndWaitForEvent.generated.h"

class UAnimMontage;
class UAnimInstance;
class AActor;
class ACharacter;
class UAbilitySystemComponent;
class UGameplayAbility;
class USkeletalMeshComponent;
class UObject;
struct FAnimMontageInstance;
struct FGameplayAbilityActorInfo;
struct FGameplayEventData;

/**
 * Montage 任务的回调。
 *
 * @param EventTag 事件标签。Montage 结束类回调传空 Tag。
 * @param EventData 事件负载。结束类回调传空数据。
 */
DECLARE_DELEGATE_TwoParams(FGGYGOPlayMontageAndWaitForEventDelegate, FGameplayTag /*EventTag*/, FGameplayEventData /*EventData*/);

/** 蓝图侧的同名委托。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FGGYGOPlayMontageAndWaitForEventBPDelegate, FGameplayTag, EventTag, FGameplayEventData, EventData);

UCLASS()
class GGYGO_API UGGYGOAbilityTask_PlayMontageAndWaitForEvent : public UAbilityTask
{
	GENERATED_BODY()

public:
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent(const FObjectInitializer& ObjectInitializer);
	/** 统一计算 Montage Task 速率快照：TaskRate 只含一次全局缩放，EffectiveRate 再乘资产 RateScale。 */
	static bool ResolvePlayRate(const UAnimMontage* Montage, float RequestedRate, float& OutTaskPlayRate, float& OutEffectivePlayRate);
	/** 返回此任务准备播放的有效速率快照，供同一次播放的 watchdog 使用。 */
	float GetEffectivePlayRate() const { return EffectivePlayRate; }

	virtual void Activate() override;
	virtual void ExternalCancel() override;
	virtual FString GetDebugString() const override;
	virtual void OnDestroy(bool AbilityEnded) override;
	/** 纠正预测位置时在 ReadyForActivation 前设置；普通播放保持 0。 */
	void SetStartTimeSeconds(float InTime) { StartTimeSeconds = InTime; }

	/**
	 * 播放 Montage 并等待事件。
	 *
	 * @param EventTags 要监听的事件标签。**留空表示监听全部事件** ——
	 *                  写清楚要听哪些更好，能避免收到不相关的通知。
	 * @param bStopWhenAbilityEnds 能力结束时是否停掉 Montage。
	 *                             攻击应当为 true（能力被打断时动作不该继续播）；
	 *                             死亡演出应当为 false（能力结束后动画要播完）。
	 * @param AnimRootMotionTranslationScale 动画自带 root motion 的缩放。
	 *                                       本项目移动走曲线驱动，通常保持 1。
	 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks", meta = (HidePin = "OwningAbility", DefaultToSelf = "OwningAbility", BlueprintInternalUseOnly = "TRUE"))
	static UGGYGOAbilityTask_PlayMontageAndWaitForEvent* PlayMontageAndWaitForEvent(
		UGameplayAbility* OwningAbility,
		FName TaskInstanceName,
		UAnimMontage* MontageToPlay,
		FGameplayTagContainer EventTags,
		float Rate = 1.0f,
		FName StartSection = NAME_None,
		bool bStopWhenAbilityEnds = true,
		float AnimRootMotionTranslationScale = 1.0f);

	/** 原实例播到自然结尾的事实；不证明它仍为 ASC 当前播放，也不授予结束后继 GA 的权力。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOPlayMontageAndWaitForEventBPDelegate OnCompleted;

	/** 开始混出。此时动画还没播完。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOPlayMontageAndWaitForEventBPDelegate OnBlendOut;

	/** 被别的 Montage 顶掉。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOPlayMontageAndWaitForEventBPDelegate OnInterrupted;

	/** 能力自身被取消。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOPlayMontageAndWaitForEventBPDelegate OnCancelled;

	/** 收到监听的事件。同一次播放可能触发多次。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOPlayMontageAndWaitForEventBPDelegate EventReceived;

private:
	friend class FGGYGOMontageTaskLifecycleTest;
	struct FInFlightMontagePlayCleanup;

	/** 当前 Montage 是否仍由本任务驱动。 */
	bool IsNotifyValid() const;
	/** 原 Task/Ability/ASC 关联及 ActorInfo 分配、Owner、Avatar、Mesh、AnimInstance 是否仍一致。 */
	bool IsActivatedActorInfoCurrent() const;
	/** 排队的实例委托仍对应本任务保存的原 Guard 身份；不查询 ASC 当前播放。 */
	bool MatchesOriginalMontageCallback(const FGGYGOMontagePlayGuardIdentity& Original) const;
	/** 按捕获的 AnimInstance 和 instance ID 取本任务的播放实例。 */
	FAnimMontageInstance* GetTaskMontageInstance() const;
	/** 只解除属于本 Task 的实例委托。 */
	void UnbindTaskMontageDelegates(FAnimMontageInstance* Instance);
	/** 只记录本次外调返回后须清理原实例的义务，不发行播放身份。 */
	void RequestInFlightMontageStop();
	/** 可定位失败后只取消并结束本 Task；外调返回时重检原 Task。 */
	void FailAndEndTask(const TCHAR* Reason, const FGGYGOAbilityMontagePlaybackResult* Result = nullptr);
	/** 释放本任务持有的 root motion scale token。 */
	void ReleaseRootMotionScaleLease();

	/** Montage 混出回调。 */
	void OnMontageBlendingOut(UAnimMontage* Montage, bool bInterrupted);
	void OnMontageBlendingOutForInstance(UAnimMontage* Montage, bool bInterrupted,
		FGGYGOMontagePlayGuardIdentity Original);

	/** Montage 结束回调。 */
	void OnMontageEnded(UAnimMontage* Montage, bool bInterrupted);
	void OnMontageEndedForInstance(UAnimMontage* Montage, bool bInterrupted,
		FGGYGOMontagePlayGuardIdentity Original);

	/** 能力被取消回调。 */
	void OnAbilityCancelled();

	/** 收到 GameplayEvent。 */
	void OnGameplayEvent(FGameplayTag EventTag, const FGameplayEventData* Payload);

	/** 停止播放。仅当 Montage 仍是本任务播的那个时才停。 */
	bool StopPlayingMontage();

	/** 要播的 Montage。 */
	UPROPERTY()
	TObjectPtr<UAnimMontage> MontageToPlay;

	/** 监听的事件标签。 */
	UPROPERTY()
	FGameplayTagContainer EventTags;

	/** 播放速率。 */
	UPROPERTY()
	float Rate;

	/** Montage RateScale 参与计算后的同次播放速率快照。 */
	UPROPERTY()
	float EffectivePlayRate = 1.0f;

	/** 工厂已验证播放配置；无效时 Activate 会取消并结束任务。 */
	bool bHasValidConfiguration = false;

	/** 起始 Section。 */
	UPROPERTY()
	FName StartSection;

	/** root motion 位移缩放。 */
	UPROPERTY()
	float AnimRootMotionTranslationScale;

	/** 能力结束时是否停掉 Montage。 */
	UPROPERTY()
	bool bStopWhenAbilityEnds;

	/** 能力取消委托的句柄。 */
	FDelegateHandle CancelledHandle;

	/** 事件监听委托的句柄。 */
	FDelegateHandle EventHandle;

	/** Montage 混出委托。 */
	FOnMontageBlendingOutStarted BlendingOutDelegate;

	/** Montage 结束委托。 */
	FOnMontageEnded MontageEndedDelegate;
	float StartTimeSeconds = 0.0f;
	int32 MontageInstanceId = INDEX_NONE;
	bool bEndingTask = false;
	bool bBlendingOut = false;
	/** 本 Task 的取消通知只执行一次，阻止 BP 回调重入重复释放/广播。 */
	bool bCancellationRequested = false;
	FGGYGOAbilityMontagePlaybackHandle OriginalPlayback;
	FGGYGOMontagePlayGuardIdentity OriginalGuardIdentity;
	/** 与调用栈共持停止义务，Task 被结束/销毁也不丢失；完整播放返回后释放。 */
	TSharedPtr<FInFlightMontagePlayCleanup> InFlightMontagePlayCleanup;
	TWeakPtr<FGameplayAbilityActorInfo> ActivatedActorInfo;
	TWeakObjectPtr<UGameplayAbility> ActivatedAbility;
	TWeakObjectPtr<AActor> ActivatedOwnerActor;
	TWeakObjectPtr<USkeletalMeshComponent> ActivatedMesh;
	TWeakObjectPtr<AActor> ActivatedAvatarActor;
	TWeakObjectPtr<ACharacter> ActivatedCharacter;
	TWeakObjectPtr<UAnimInstance> ActivatedAnimInstance;
	TWeakObjectPtr<UAbilitySystemComponent> ActivatedASC;
	uint64 RootMotionScaleLeaseToken = 0;
};
