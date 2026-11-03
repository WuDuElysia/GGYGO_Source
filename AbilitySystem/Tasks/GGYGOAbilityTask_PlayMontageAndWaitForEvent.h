/**
 * @file GGYGOAbilityTask_PlayMontageAndWaitForEvent.h
 * @brief 播放 Montage，等待 GameplayEvent 与具名原生通知
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
 * ## 结束路径的区别
 * | 委托 | 触发时机 | 能力通常该做什么 |
 * |---|---|---|
 * | `OnCompleted`   | 原实例播到自然结尾 | 调用方验证原能力生命周期后正常收尾 |
 * | `OnBlendOut`    | 开始混出（还没播完） | 按原能力条件消费混出事实 |
 * | `OnInterrupted` | 被别的 Montage 顶掉 | 中断收尾，不结算未完成的判定 |
 * | `OnCancelled`   | 能力自身被取消 | 同上，且要清理已施加的状态 |
 * | `OnFailed`      | 必需姿态依赖失败，原 Task 已清理 | 核原激活与资源后按必需故障结束能力 |
 *
 * 分开而不是合成一个"结束了"回调，是因为这些情况的后续处理不同：
 * 自然结束要结算收招硬直，被打断则不该结算。
 */
#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "AbilitySystem/GGYGOAbilityMontagePlaybackTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/Runtime/GGYGOActionPoseContract.h"
#include "GameplayTagContainer.h"

#include "GGYGOAbilityTask_PlayMontageAndWaitForEvent.generated.h"

class UAnimMontage;
class UAnimSequenceBase;
class UAnimInstance;
class AActor;
class ACharacter;
class UAbilitySystemComponent;
class UGameplayAbility;
class USkeletalMeshComponent;
class UObject;
class UAnimNotifyState;
class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
struct FAnimMontageInstance;
struct FGameplayAbilityActorInfo;
struct FGameplayEventData;

/** Historical section notification from this task's exact original montage instance.
 * The engine does not retain the position at which a queued section event occurred. */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageSectionFact
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	int32 MontageInstanceId = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FName SectionName = NAME_None;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	bool bLooped = false;
};

/** Instantaneous read of the original instance, including after a nonzero start position.
 * Neither this snapshot nor a section fact grants authority over the current GA/ASC resource. */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageSectionSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	int32 MontageInstanceId = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FName SectionName = NAME_None;
	/** Actual montage timeline position; never the position of an earlier queued event. */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float PositionSeconds = 0.0f;
	/** Raw instance rate, excluding Montage.RateScale; not TimeStretch's actual advancement. */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float InstancePlayRate = 0.0f;
};

DECLARE_DELEGATE_OneParam(FGGYGOMontageSectionFactDelegate, const FGGYGOMontageSectionFact&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOMontageSectionFactBPDelegate, FGGYGOMontageSectionFact, SectionFact);

/** 原资产的具名通知声明；静态时间数据不认证播放实例或业务许可。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageNotifyDeclaration
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FName NotifyName = NAME_None;
	/** 作者在 Montage 时间线上的位置，即 FAnimNotifyEvent::GetTime()。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float AuthoredPositionSeconds = 0.0f;
	/** 含原生 offset 的实际触发时刻，即 GetTriggerTime()。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float TriggerPositionSeconds = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float TriggerTimeOffsetSeconds = 0.0f;

private:
	friend class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
	/** Task 的配置数组持有原 UObject，不持有 Notifies 数组中的裸指针。 */
	UPROPERTY()
	TObjectPtr<UAnimNotify> NativeNotify = nullptr;
};

/** An authored native point from this task's exact original playback.
 * This fact and its authored position grant no ability, input or movement authority. */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageNotifyFact
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<USkeletalMeshComponent> Mesh = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	int32 MontageInstanceId = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FName NotifyName = NAME_None;
	/** Native trigger time, including its offset; not a later callback-time position. */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float NotifyPositionSeconds = 0.0f;
	/** 原声明的作者位置；不得把提前触发的 offset 当业务边界已经到达。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float NotifyAuthoredPositionSeconds = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float NotifyTriggerTimeOffsetSeconds = 0.0f;
};

DECLARE_DELEGATE_OneParam(FGGYGOMontageNotifyFactDelegate, const FGGYGOMontageNotifyFact&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOMontageNotifyFactBPDelegate, FGGYGOMontageNotifyFact, NotifyFact);

UENUM(BlueprintType)
enum class EGGYGOMontageWindowPhase : uint8
{
	Begin,
	End
};

/** Notify 发送栈的本地原始事实；不进入公开 GameplayEvent 的 TargetData 或任何网络载荷。 */
struct GGYGO_API FGGYGOMontageWindowSourceData
{
	TSharedPtr<const void> DispatchIdentity;
	FGameplayTag EventTag;
	TWeakObjectPtr<UAnimSequenceBase> Animation;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UAnimInstance> AnimInstance;
	TWeakObjectPtr<UAnimNotifyState> Notify;
	EGGYGOMontageWindowPhase Phase = EGGYGOMontageWindowPhase::Begin;
	int32 MontageInstanceId = INDEX_NONE;
	bool bHasMontageInstanceContext = false;
	float BeginSeconds = 0.0f;
	float EndSeconds = 0.0f;
	float BeginTriggerSeconds = 0.0f;
	float EndTriggerSeconds = 0.0f;
	bool bQueued = false;
	bool bNoMergeOnConcurrentPlay = false;
};

/** 原 Montage 的唯一作者窗口声明；由公共解析器签出，不表示运行中的窗口许可。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageWindowDeclaration
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FGameplayTag BeginEventTag;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FGameplayTag EndEventTag;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float BeginSeconds = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float EndSeconds = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float BeginTriggerSeconds = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	float EndTriggerSeconds = 0.0f;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	bool bQueued = false;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	bool bNoMergeOnConcurrentPlay = false;
	/** 原作者条目的值身份，不证明当前播放或工作资格。 */
	bool HasSameWindow(const FGGYGOMontageWindowDeclaration& Other) const
	{
		return NativeNotify && NativeNotify == Other.NativeNotify && Montage == Other.Montage
			&& NotifyIndex == Other.NotifyIndex && BeginEventTag == Other.BeginEventTag
			&& EndEventTag == Other.EndEventTag && BeginSeconds == Other.BeginSeconds && EndSeconds == Other.EndSeconds
			&& BeginTriggerSeconds == Other.BeginTriggerSeconds && EndTriggerSeconds == Other.EndTriggerSeconds
			&& bQueued == Other.bQueued && bNoMergeOnConcurrentPlay == Other.bNoMergeOnConcurrentPlay;
	}

private:
	friend class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
	UPROPERTY()
	TObjectPtr<UAnimNotifyState> NativeNotify = nullptr;
	UPROPERTY()
	int32 NotifyIndex = INDEX_NONE;
	UPROPERTY()
	float RangeStartSeconds = 0.0f;
	UPROPERTY()
	float RangeEndSeconds = 0.0f;
};

/** 公共 Task 认证的原窗口事件。工作资格只在该 Task 的同步派发期间有效。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageWindowFact
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<USkeletalMeshComponent> Mesh = nullptr;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	int32 MontageInstanceId = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FGGYGOMontageWindowDeclaration Declaration;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	EGGYGOMontageWindowPhase Phase = EGGYGOMontageWindowPhase::Begin;
	/** 原 World 在实际事件派发时的时间采样，不把作者距离或 Montage 速率换算成 Age。 */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	double DispatchWorldTimeSeconds = 0.0;

private:
	friend class UGGYGOAbilityTask_PlayMontageAndWaitForEvent;
	UPROPERTY()
	TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> Issuer;
	UPROPERTY()
	uint64 DispatchSerial = 0;
};

DECLARE_DELEGATE_OneParam(FGGYGOMontageWindowFactDelegate, const FGGYGOMontageWindowFact&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOMontageWindowFactBPDelegate, FGGYGOMontageWindowFact, WindowFact);

UENUM(BlueprintType)
enum class EGGYGOMontageTaskFailureStage : uint8
{
	Startup,
	Playback
};

/** Required pose dependency failure from this task's original resource, after its cleanup.
 * A consumer must authenticate its original ability activation/step/task before ending it.
 * Native EndTask cleared Task.Ability and marked this sender Garbage. A historical receiver
 * can use its captured weak task's Get(true), rejecting RF destruction, without live-task checks. */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOMontageTaskFailureFact
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	TObjectPtr<UAnimMontage> Montage = nullptr;
	/** Startup has no issued playback and reports INDEX_NONE. */
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	int32 MontageInstanceId = INDEX_NONE;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FName SlotName = NAME_None;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	EGGYGOMontageTaskFailureStage Stage = EGGYGOMontageTaskFailureStage::Startup;
	UPROPERTY(BlueprintReadOnly, Category = "Ability|Tasks")
	FString Diagnostic;
};

DECLARE_DELEGATE_OneParam(FGGYGOMontageTaskFailureDelegate, const FGGYGOMontageTaskFailureFact&);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOMontageTaskFailureBPDelegate, FGGYGOMontageTaskFailureFact, FailureFact);

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

	/** Optional C++ callbacks. Callers capture their original activation and weak task by value. */
	struct FNativeCallbacks
	{
		FGGYGOPlayMontageAndWaitForEventDelegate OnCompleted;
		FGGYGOPlayMontageAndWaitForEventDelegate OnBlendOut;
		FGGYGOPlayMontageAndWaitForEventDelegate OnInterrupted;
		FGGYGOPlayMontageAndWaitForEventDelegate OnCancelled;
		FGGYGOPlayMontageAndWaitForEventDelegate EventReceived;
		FGGYGOMontageSectionFactDelegate SectionReceived;
		FGGYGOMontageNotifyFactDelegate NotifyReceived;
		FGGYGOMontageWindowFactDelegate WindowReceived;
		FGGYGOMontageTaskFailureDelegate OnFailed;
	};

	/** One nonempty registration, only in native AwaitingActivation before ReadyForActivation.
	 * Invalid/duplicate/late registration fails explicitly; it never replaces another package. */
	FDelegateHandle RegisterNativeCallbacks(FNativeCallbacks Callbacks);
	/** Detach only this registration. Destruction of its captures cannot erase a reentrant successor. */
	bool UnregisterNativeCallbacks(FDelegateHandle Registration);
	/** 统一计算 Montage Task 速率快照：TaskRate 只含一次全局缩放，EffectiveRate 再乘资产 RateScale。 */
	static bool ResolvePlayRate(const UAnimMontage* Montage, float RequestedRate, float& OutTaskPlayRate, float& OutEffectivePlayRate);
	/** 同次播放的名义速率乘积，供 watchdog 使用；不表达动态 instance rate 或 TimeStretch 推进。 */
	float GetEffectivePlayRate() const { return EffectivePlayRate; }
	/** Read after ReadyForActivation and rechecking the caller's original activation/step/task.
	 * False clears the output; no cached replay or lookup through a same-asset successor.
	 * An extant original instance remains readable during blend-out; this is not playback authority.
	 * NAME_None and finite zero/negative rates are raw facts for the caller to interpret. */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Ability|Tasks")
	bool TryGetOriginalSectionSnapshot(FGGYGOMontageSectionSnapshot& OutSnapshot) const;

	/** Ready 前一次配置单个原生点；保持原单点调用行为，不追加或替换已配置集合。 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks")
	bool ConfigureOriginalMontageNotify(FName InName, FString& OutDiagnostic);
	/** Ready 前一次配置非空、不同非 None 名字集；任一点无效则不提交任何配置。
	 * Task 只持声明及自身订阅，各点的业务许可由原 GA 消费者解释。 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks")
	bool ConfigureOriginalMontageNotifies(const TArray<FName>& InNames, FString& OutDiagnostic);
	/** Shared declaration validation for callers checking their authored configurations.
	 * No playback, registration, Blueprint callback or business-state mutation occurs. */
	static bool ResolveOriginalMontageNotify(const UAnimMontage* Montage, FName NotifyName,
		float& OutNotifyPositionSeconds, FString& OutDiagnostic);
	/** 原资产与名字 → 唯一 stock exact UAnimNotify_PlayMontageNotify 的时间声明。
	 * 要求原 LinkedMontage、BranchingPoint、零 duration，无同名 Window/子类/重复；
	 * 作者时间/offset/trigger 有限且时间位于 timeline，所选 trigger 不碰撞其它 Begin/StateEnd。
	 * 失败清输出并诊断；不认证 live 播放，不移动通知或合成事件。 */
	static bool ResolveOriginalMontageNotifyDeclaration(const UAnimMontage* Montage, FName NotifyName,
		FGGYGOMontageNotifyDeclaration& OutDeclaration, FString& OutDiagnostic);
	/** Actual position of this task's fixed-ID original instance, including natural blend-out.
	 * Used after Ready and original caller authentication; false clears both outputs.
	 * This read does not synthesize a Notify or decide whether a business gate is open.
	 * 仅单配置点保持旧无名入口；多点须显式使用 ByName，不能猜第一点。 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Ability|Tasks")
	bool TryGetOriginalMontageNotifySnapshot(FGGYGOMontageNotifyFact& OutFact, float& OutPositionSeconds) const;
	/** 对应名字的原配置声明及固定原 ID 实例的实际位置；包括非零起点和自然混出。
	 * 失败清两输出，不从另一名字、同资产后继或 Section 事实借许可。 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Ability|Tasks")
	bool TryGetOriginalMontageNotifySnapshotByName(FName NotifyName,
		FGGYGOMontageNotifyFact& OutFact, float& OutPositionSeconds) const;

	/** 纯解析唯一作者窗并保留实际 Queued/NoMerge flag；失败清输出，不创建播放或运行期许可。 */
	static bool ResolveOriginalMontageWindowDeclaration(const UAnimMontage* Montage,
		FGameplayTag BeginEventTag, FGameplayTag EndEventTag, float RangeStartSeconds, float RangeEndSeconds,
		FGGYGOMontageWindowDeclaration& OutDeclaration, FString& OutDiagnostic);
	/** 只收集原资产真实存在且命中订阅词汇的窗口；没有匹配窗时成功返回空集。 */
	static bool ResolveOriginalMontageWindows(const UAnimMontage* Montage, const FGameplayTagContainer& Tags,
		float RangeStartSeconds, float RangeEndSeconds,
		TArray<FGGYGOMontageWindowDeclaration>& OutDeclarations, FString& OutDiagnostic);
	/** Ready 前一次提交完整声明集；允许显式空集，任何无效或重复原条目均不提交。 */
	UFUNCTION(BlueprintCallable, Category = "Ability|Tasks")
	bool ConfigureOriginalMontageWindows(const TArray<FGGYGOMontageWindowDeclaration>& Declarations,
		FString& OutDiagnostic);
	/** 只读原配置声明及固定实例位置/速率，不合成 Begin，不认证较早的排队事件。 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "Ability|Tasks")
	bool TryGetOriginalMontageWindowSnapshot(FGameplayTag BeginEventTag, FGameplayTag EndEventTag,
		FGGYGOMontageWindowDeclaration& OutDeclaration, FGGYGOMontageSectionSnapshot& OutPlayback) const;
	/** 同步 WindowReceived 内认证原事实；派发结束即失效，历史副本不授予新工作权。 */
	bool IsOriginalMontageWindowFact(const FGGYGOMontageWindowFact& Fact) const;

	virtual void Activate() override;
	/** Required contracts only: read Animation's original lease through UE's task scheduler. */
	virtual void TickTask(float DeltaTime) override;
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

	/** Original instance section fact, possibly queued before blend-out; not current playback authority. */
	UPROPERTY(BlueprintAssignable)
	FGGYGOMontageSectionFactBPDelegate SectionReceived;

	/** Exact original native point; may arrive during natural blend-out, never from ASC events. */
	UPROPERTY(BlueprintAssignable)
	FGGYGOMontageNotifyFactBPDelegate NotifyReceived;

	/** 原作者窗口的 Begin/End；通用 EventReceived 保持独立的既有接口。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOMontageWindowFactBPDelegate WindowReceived;

	/** Required pose failure after this original task has stopped and released its resources.
	 * This is independent of user cancellation and may be received from a finished task. */
	UPROPERTY(BlueprintAssignable)
	FGGYGOMontageTaskFailureBPDelegate OnFailed;

private:
	friend class FGGYGOMontageTaskLifecycleTest;
	struct FInFlightMontagePlayCleanup;
	struct FNativeCallbackRegistration;
	enum class ENativeCallback : uint8 { Completed, BlendOut, Interrupted, Cancelled, EventReceived };
	/** Path-specific original task facts; cancellation may precede ActorInfo/Guard installation. */
	bool CanDispatchOriginalCallback(ENativeCallback Kind, const FGGYGOMontagePlayGuardIdentity& Original) const;
	/** Common original-source envelope for Ended/blend-out/section facts, including queued facts. */
	bool CanDispatchOriginalInstanceFact(const FGGYGOMontagePlayGuardIdentity& Original) const;
	/** Native snapshot first, then recheck the original task before its existing BP callback. */
	void DispatchOriginalCallback(ENativeCallback Kind, FGameplayTag EventTag, FGameplayEventData EventData);
	void DispatchOriginalSectionFact(const FGGYGOMontageSectionFact& Fact,
		const FGGYGOMontagePlayGuardIdentity& Original);
	const FGGYGOMontageNotifyDeclaration* FindConfiguredMontageNotify(FName NotifyName) const;
	bool IsOriginalMontageNotifySourceCurrent(const FGGYGOMontageNotifyDeclaration& Declaration) const;
	bool AreOriginalMontageNotifySourcesCurrent() const;
	bool CanDispatchOriginalMontageNotifyFact(const FGGYGOMontageNotifyFact& Fact,
		const FGGYGOMontagePlayGuardIdentity& Original) const;
	/** Caller validates the payload/source on this stack; only the external callback return needs revalidation. */
	void DispatchValidatedOriginalMontageNotifyFact(const FGGYGOMontageNotifyFact& Fact,
		const FGGYGOMontagePlayGuardIdentity& Original);
	UFUNCTION()
	void OnOriginalMontageNotifyBegin(FName NotifyName, const FBranchingPointNotifyPayload& Payload);
	bool IsOriginalMontageWindowDeclarationCurrent(const FGGYGOMontageWindowDeclaration& Declaration) const;
	static bool ResolveOriginalMontageWindowAtIndex(const UAnimMontage* Montage, int32 NotifyIndex,
		float RangeStartSeconds, float RangeEndSeconds, FGGYGOMontageWindowDeclaration& OutDeclaration,
		FString& OutDiagnostic);
	void DispatchOriginalMontageWindowFact(FGameplayTag EventTag, const FGameplayEventData& Payload);

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
	/** Pose failures stop this original resource even when owner-end playback is configured off. */
	void FailActionPoseContract(EGGYGOMontageTaskFailureStage Stage, FName SlotName, const FString& Diagnostic);
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
	void OnMontageSectionChangedForInstance(UAnimMontage* Montage, FName SectionName, bool bLooped,
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

	/** Ready 前提交的唯一声明集合，GC 跟踪原 Notify UObject；不保存业务门或进度。 */
	UPROPERTY()
	TArray<FGGYGOMontageNotifyDeclaration> ConfiguredMontageNotifyDeclarations;

	UPROPERTY()
	TArray<FGGYGOMontageWindowDeclaration> ConfiguredMontageWindowDeclarations;
	bool bMontageWindowsConfigured = false;
	/** 只保同步派发的原事实，外调后据序号复核；不保存窗口进度或 GA 业务门。 */
	UPROPERTY()
	FGGYGOMontageWindowFact DispatchingWindowFact;
	uint64 LastWindowDispatchSerial = 0;
	bool bWindowFactDispatching = false;
	/** 仅弱观察仍存活的原 producer 派发；清理已退出源，不保留历史或发行新时间线。 */
	TArray<TWeakPtr<const void>> ConsumedWindowSourceDispatches;

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
	FOnMontageSectionChanged MontageSectionChangedDelegate;
	float StartTimeSeconds = 0.0f;
	int32 MontageInstanceId = INDEX_NONE;
	bool bEndingTask = false;
	bool bBlendingOut = false;
	/** 本 Task 的取消通知只执行一次，阻止 native/BP 回调重入重复释放/广播。 */
	bool bCancellationRequested = false;
	/** Terminal delivery guard only; Animation owns the pose contract and its failure state. */
	bool bPoseFailureRequested = false;
	/** Subscription resource only; no GA activation identity, playback state or execution authority. */
	TSharedPtr<FNativeCallbackRegistration> NativeCallbackRegistration;
	/** Animation owns validation/failure state; this opaque lease never upgrades to new context. */
	FGGYGOActionPoseContractTicket ActionPoseContractTicket;
	/** ASC 签发的本次播放身份；Guard 字段是本次调用创建的实例。 */
	FGGYGOAbilityMontagePlayback OriginalPlayback;
	/** 与调用栈共持停止义务，Task 被结束/销毁也不丢失；完整播放返回后释放。 */
	TSharedPtr<FInFlightMontagePlayCleanup> InFlightMontagePlayCleanup;
	/** 激活时的能力与 ASC：解除本 Task 的取消／事件订阅，并判断能力是否仍是本 Task 的拥有者。 */
	TWeakObjectPtr<UGameplayAbility> ActivatedAbility;
	/** 根运动缩放租约与动作姿态契约的接收者。 */
	TWeakObjectPtr<USkeletalMeshComponent> ActivatedMesh;
	TWeakObjectPtr<ACharacter> ActivatedCharacter;
	TWeakObjectPtr<UAnimInstance> ActivatedAnimInstance;
	TWeakObjectPtr<UAbilitySystemComponent> ActivatedASC;
	uint64 RootMotionScaleLeaseToken = 0;
};
