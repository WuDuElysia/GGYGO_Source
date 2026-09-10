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
 * | `OnCompleted`   | 播到自然结尾 | 正常收尾，结束能力 |
 * | `OnBlendOut`    | 开始混出（还没播完） | 提前允许下一个动作衔接 |
 * | `OnInterrupted` | 被别的 Montage 顶掉 | 中断收尾，不结算未完成的判定 |
 * | `OnCancelled`   | 能力自身被取消 | 同上，且要清理已施加的状态 |
 *
 * 分开而不是合成一个"结束了"回调，是因为动作游戏里这四种情况的后续处理不同：
 * 自然结束要结算收招硬直，被打断则不该结算。
 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "GameplayTagContainer.h"

#include "GGYGOAbilityTask_PlayMontageAndWaitForEvent.generated.h"

class UAnimMontage;
class UGameplayAbility;
class UObject;
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

	virtual void Activate() override;
	virtual void ExternalCancel() override;
	virtual FString GetDebugString() const override;
	virtual void OnDestroy(bool AbilityEnded) override;

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

	/** 播到自然结尾。 */
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
	/** 当前 Montage 是否仍由本任务驱动。 */
	bool IsNotifyValid() const;

	/** Montage 混出回调。 */
	void OnMontageBlendingOut(UAnimMontage* Montage, bool bInterrupted);

	/** Montage 结束回调。 */
	void OnMontageEnded(UAnimMontage* Montage, bool bInterrupted);

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
};
