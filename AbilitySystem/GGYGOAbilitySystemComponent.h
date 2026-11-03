/**
 * @file GGYGOAbilitySystemComponent.h
 * @brief 项目 ASC —— 输入缓存、组仲裁、Tag 关系扩展
 *
 * 在 GAS 原生 ASC 之上加三块能力：
 *
 * ## 1. 原输入请求与有序聚合缓存
 * 实现在私有协作者 `FGGYGOAbilityInputBuffer`（请求、按住、边沿、重试、每帧消费）与
 * `FGGYGOAbilityStepInputRouter`（分段输入来源认证、Initial 运输、TargetData RPC）；
 * ASC 只保留公开入口并委托。请求只靠"Revision 递增＋清空记录"失效：ActorInfo 端点变化、
 * ClearActorInfo 与输入阻断都走 ClearAbilityInput。
 * Pressed只送给原本已活跃的Spec，不把刚由同一按下激活的实例再当成第二次按下。
 *
 * ## 2. 组仲裁（表驱动）
 * 用 `ActiveAbilitiesByGroup` 按组 Tag 索引正在运行的能力，替代 Lyra 的三元素计数数组。
 *
 * 规则分两层，互不干扰：
 * - **跨组**：`SelfPolicy == Exclusive` 的能力压制所有组里优先级更低的能力。
 *   死亡、被击倒、大招演出用它做到"世界静止"。
 * - **同组**：查 `UGGYGOAbilityGroupConfig` 得到该组的 `EGGYGOAbilityGroupRule`，
 *   三种语义分别是放过、按优先级顶替、严格先来后到。
 *
 * `SingleInstance` 的平手归属由 `bNewcomerWinsOnTie` 决定，默认 true（决策 D4：
 * 同优先级后来者打断先激活者），受击组通常要配成 false 以免受击动画反复重播。
 * 未注入配置表时全部走内置默认规则。
 *
 * 仲裁只做拒绝；真实输入 Queued 失败许可保存在原 ASC 请求中。
 * 组释放唤醒已有许可并排入下一次 ProcessAbilityInput，不在广播栈中激活。
 * 切回续放队员期间（State.SwitchReturnPending）输入发起的激活失败同样登记许可，
 * 标签移除后的第一次 ProcessAbilityInput 唤醒。
 * OnAbilityGroupFreed 和 OnAbilityInputRetryable 是观察通知，不拥有输入等待。
 *
 * ## 3. Tag 关系扩展
 * 把 `UGGYGOAbilityTagRelationshipMapping` 的查询结果接进 GAS 的阻断/取消判定。
 *
 * ## 谁来调用 ProcessAbilityInput
 * PlayerController 的 PostProcessInput 是唯一帧末驱动；Hero 只记每个输入动作当前的请求身份，
 * 输入会话退役时按来源一次结束其签发的全部请求（EndAbilityInputRequestsFromSource）。
 */
#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "AbilitySystem/GGYGOAbilityMontagePlaybackTypes.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "FX/GGYGOMontageFXPublication.h"
#include "NativeGameplayTags.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

#include "GGYGOAbilitySystemComponent.generated.h"

class AActor;
class APlayerController;
class UAnimInstance;
class UAttributeSet;
class UMovementComponent;
class USkeletalMeshComponent;
class UGameplayAbility;
class UGGYGOAbilityGroupConfig;
class UGGYGOCharacterMovementComponent;
class UGGYGOAbilityTagRelationshipMapping;
class FGGYGOAbilityInputBuffer;
class FGGYGOAbilityStepInputRouter;
class UObject;
class UWorld;
struct FFrame;
struct FGameplayAbilityTargetDataHandle;
struct FGGYGOAbilityGroupRule;
struct FGGYGOMontagePlayGuardResult;
struct FGGYGOMotionExecutionSnapshot;

/**
 * 持有此 Tag 时整帧 Ability 输入被屏蔽。
 * 屏蔽会连 held 一起清掉，避免解除屏蔽后旧按键突然自动激活。
 */
GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_GGYGO_Gameplay_AbilityInputBlocked);

/**
 * 某个能力组的最后一个实例结束时广播。
 * @param GroupTag 空出来的组。
 *
 * ASC 先把原输入请求的真实 Queued 许可排入下次 Process，再发布此观察通知。
 * 订阅者不能由组名制造输入来源，输入能力不在这个广播栈中重试。
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FGGYGOAbilityGroupFreed, FGameplayTag /*GroupTag*/);

/**
 * A real B0 Queued failure for this exact ASC-issued request.
 * Initial press and finite retry both carry the original finite deadline.
 * Subscribers own only copied IDs/deadlines; no Tag inference or new window.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FGGYGOAbilityInputRetryable,
	const FGGYGOAbilityInputRetryRequest& /*OriginalRequest*/);

UCLASS()
class GGYGO_API UGGYGOAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UGGYGOAbilitySystemComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
	virtual ~UGGYGOAbilitySystemComponent() override;

	//~UAbilitySystemComponent interface
	/**
	 * Owner / Avatar 绑定或切换时调用。
	 * 检测到**新的 Pawn Avatar** 时通知所有能力实例，并按 OnSpawn 策略尝试激活。
	 */
	virtual void InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor) override;
	/** 清空能力输入后清除 ActorInfo。 */
	virtual void ClearActorInfo() override;
	/** 原生入口转到 PlayAbilityMontage，返回其 Duration。 */
	virtual float PlayMontage(UGameplayAbility* InAnimatingAbility, FGameplayAbilityActivationInfo ActivationInfo,
		UAnimMontage* NewAnimMontage, float InPlayRate, FName StartSectionName = NAME_None,
		float StartTimeSeconds = 0.0f) override;
	//~End of UAbilitySystemComponent interface

	/**
	 * 项目唯一的能力 Montage 播放入口。只支持 UGGYGOMontageGuardAnimInstance：Guard 识别本次调用
	 * 创建的实例并处理播放期间的重入。非 Guard、参数无效、ActorInfo 写入期间或 Guard 拒绝时
	 * Playback 为空，不回退原生播放。IsCallerContextCurrent 可为空；非空时必须是无副作用的
	 * 游戏线程查询，捕获引用须活过本次调用。项目能力必须处于当前激活。
	 */
	FGGYGOAbilityMontagePlaybackResult PlayAbilityMontage(UGameplayAbility* Ability,
		FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* Montage, float PlayRate,
		FName StartSection, float StartTimeSeconds, TFunction<bool()> IsCallerContextCurrent = nullptr);
	/** Playback 仍是 Local 当前播放（能力、Montage、PlayInstanceId 一致），且能力仍处于记录的激活。 */
	bool IsPlaybackCurrent(const FGGYGOAbilityMontagePlayback& Playback) const;
	/** Playback 仍是当前播放时清除 AnimatingAbility 与能力 CurrentMontage；不停止 Montage。 */
	bool ClearPlayback(const FGGYGOAbilityMontagePlayback& Playback);

	/** 取消谓词。返回 true 表示该实例应被取消。 */
	typedef TFunctionRef<bool(const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle)> TShouldCancelAbilityFunc;

	/**
	 * 按谓词取消正在运行的能力。
	 * 遍历期间会锁住能力列表，因此谓词里不要再授予或移除能力。
	 */
	void CancelAbilitiesByFunc(TShouldCancelAbilityFunc ShouldCancelFunc, bool bReplicateCancelAbility);

	/** 服务器纠正拥有者本次激活的能力状态；目标数据解释由具体能力负责。 */
	UFUNCTION(Client, Reliable)
	void ClientCorrectAbilityState(FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey ActivationKey,
		const FGameplayAbilityTargetDataHandle& Correction);

	/**
	 * Empty PreviousIdentity starts a real request; an assigned value only revalidates that request.
	 * Freeze first matching Specs, the issuing Source and the absolute retry deadline.
	 * Validity is only Revision + live record: ActorInfo changes clear all requests and bump the Revision.
	 * Expiring a retry deadline never releases held input. Only edits ASC tables and Spec.InputPressed; no callouts.
	 */
	FGGYGOAbilityInputRequestResult ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
		const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline);
	FGGYGOAbilityInputRequestResult ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
		const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline,
		const FGGYGOMovementInputEvaluationToken& OriginalEvaluationToken,
		const FGGYGOAbilityInputSourceKey& Source = FGGYGOAbilityInputSourceKey{});
	virtual void ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
		FPredictionKey AbilityOriginalPredictionKey, const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey) override;
	/** Initialize 最早阶段保存原激活运输来源，不依赖 Task、edge 或方向就绪。 */
	FGGYGOAbilityInitialInputRoute CaptureInitialInputRoute(UGGYGOGameplayAbility* Ability,
		const FGGYGOAbilityActivationHandle& Original, FString& OutError);
	virtual void ServerSetReplicatedTargetDataCancelled_Implementation(FGameplayAbilitySpecHandle AbilityHandle,
		FPredictionKey AbilityOriginalPredictionKey, FPredictionKey CurrentPredictionKey) override;

	/** Exact retirement. Released retains a finite tap; Invalidated never emits InputReleased. No callouts. */
	FGGYGOAbilityInputRequestResult EndAbilityInputRequest(const FGGYGOAbilityInputRequestIdentity& Identity,
		EGGYGOAbilityInputRequestEndKind EndKind);

	/** 来源会话退役：以 Invalidated 结束该来源签发、仍在表中的全部请求；其它来源不受影响。无外调。 */
	void EndAbilityInputRequestsFromSource(const FGGYGOAbilityInputSourceKey& Source);

	/** Invalid deadline sentinel only; never accepted as an initial or retry request deadline. */
	static constexpr double NoAbilityInputRetryDeadline = -1.0;

	/** 每帧消费输入缓存。需要外部驱动，见文件头说明。 */
	void ProcessAbilityInput(float DeltaTime, bool bGamePaused);

	/**
	 * 清空全部输入缓存（含 held 与 retry），并使正在消费的旧快照失效。
	 *
	 * 同步纯容器写入：只改本 ASC 的输入请求、边沿、retry、held、输入修订号、
	 * 激活尝试来源及 Spec.InputPressed；不广播、不调用 GA／BP、不写 ActorInfo。
	 * 调用方的后置核验是否需要，取决于其谓词是否读取上述状态，而不是本调用会外调。
	 * 若以后在此加入原生事件或委托派发，须同步恢复各调用方的返栈核验。
	 */
	void ClearAbilityInput();

	/**
	 * 拥有者失去玩家控制时调用（换人换出）。对仍按住的活动 Spec 派发一次真实释放
	 * （AbilitySpecInputReleased 与 InputReleased 复制事件，WaitInputRelease 会收到），
	 * 然后 ClearAbilityInput。释放回调可以同步结束能力；未激活 Spec 只清 InputPressed。
	 */
	void ReleaseHeldInputForControlLoss();

	/**
	 * 无副作用查询某个 Spec 此刻能否激活。AI 决策用它复用 GAS 的
	 * Tag、Cooldown、Cost 与组仲裁，不在 BT 里拷贝第二套规则。
	 */
	bool CanActivateAbilityByHandle(FGameplayAbilitySpecHandle Handle, FGameplayTagContainer& OutFailureTags) const;

	// ===== 组仲裁 =====

	/**
	 * 判断请求激活的能力是否被组规则阻断。
	 * @return true 表示应拒绝激活。
	 */
	bool IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability) const;

	/**
	 * 带原因的重载。原因用于区分"重试有意义"（`GroupOccupiedQueued`）
	 * 与"条件不满足"（其余），供意图层决定是否把请求留在缓冲里。
	 *
	 * 判定顺序：
	 *   1. **跨组排斥**：任何组里存在 `SelfPolicy == Exclusive` 且优先级更高的能力 → 阻断
	 *   2. **同组规则**（查 `UGGYGOAbilityGroupConfig`）：
	 *      - `Coexist` → 放过
	 *      - `SingleInstance` → 按优先级比较，`bNewcomerWinsOnTie` 决定平手归属
	 *      - `SingleInstanceQueued` → 组内有人就阻断，不比优先级
	 */
	bool IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability, EGGYGOAbilityGroupBlockReason& OutReason) const;

	/** PreActivate 中登记本次准入预留；只由 `NotifyAbilityActivated` 调用。 */
	void AddAbilityToActivationGroup(UGGYGOGameplayAbility* Ability);

	/** 在 GAS 增加 Spec.ActiveCount 后执行最终组裁决；由项目 GA 的 ActivateAbility 入口调用。 */
	bool FinalizeAbilityGroupAdmission(UGGYGOGameplayAbility* Ability, uint64 AdmissionSequence);

	/** 能力结束后从组中摘除；组变空时可选择广播 `OnAbilityGroupFreed`。由 `NotifyAbilityEnded` 调用。 */
	bool RemoveAbilityFromActivationGroup(UGGYGOGameplayAbility* Ability, bool bBroadcastGroupFreed = true);

	/**
	 * 注入组规则配置表。传 nullptr 清除，之后降级为内置默认规则。
	 * 正常由 `UGGYGOPawnData` 在角色初始化时设置。
	 */
	void SetAbilityGroupConfig(const UGGYGOAbilityGroupConfig* InConfig);

	/** 当前的组规则配置表，可能为 nullptr。 */
	const UGGYGOAbilityGroupConfig* GetAbilityGroupConfig() const { return AbilityGroupConfig; }

	/** 某个组当前正在运行的能力数量。诊断与 UI 用。 */
	int32 GetActiveAbilityCountInGroup(FGameplayTag GroupTag) const;

	/** 组的最后一个实例结束时广播。见 `FGGYGOAbilityGroupFreed` 说明。 */
	FGGYGOAbilityGroupFreed OnAbilityGroupFreed;

	/** 请求被拒但值得重试时广播。见 `FGGYGOAbilityInputRetryable` 说明。 */
	FGGYGOAbilityInputRetryable OnAbilityInputRetryable;

	// ===== Tag 关系 =====

	/** 设置 Tag 关系表。传 nullptr 清除，之后不再扩展关系 Tag。 */
	void SetTagRelationshipMapping(UGGYGOAbilityTagRelationshipMapping* NewMapping);

	/** 查询关系表带来的额外激活必需 / 阻断 Tag。追加语义，不清空输出。 */
	void GetAdditionalActivationTagRequirements(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer& OutActivationRequired, FGameplayTagContainer& OutActivationBlocked) const;

	/**
	 * 取某次能力激活关联的目标数据。
	 * 用 SpecHandle + 激活预测键做键，因此能区分同一能力的多次预测激活。
	 * 查不到时**不修改**输出参数。
	 */
	void GetAbilityTargetData(const FGameplayAbilitySpecHandle AbilityHandle, FGameplayAbilityActivationInfo ActivationInfo, FGameplayAbilityTargetDataHandle& OutTargetDataHandle);

	/** Avatar 就位后，让所有 `OnSpawn` 策略的能力尝试激活。 */
	void TryActivateAbilitiesOnSpawn();

protected:
	//~UAbilitySystemComponent interface
	/** 转发 InputPressed 预测事件，让 `WaitInputPress` 类 AbilityTask 能收到。 */
	virtual void AbilitySpecInputPressed(FGameplayAbilitySpec& Spec) override;

	/** 转发 InputReleased 预测事件。 */
	virtual void AbilitySpecInputReleased(FGameplayAbilitySpec& Spec) override;

	/** 激活成功后登记组。 */
	virtual void NotifyAbilityActivated(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability) override;

	/** 激活失败后把原因送到正确的一端处理。 */
	virtual void NotifyAbilityFailed(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason) override final;

	/** 结束后从组中摘除。 */
	virtual void NotifyAbilityEnded(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, bool bWasCancelled) override;

	/** 用关系表扩展阻断 / 取消 Tag 后再交给父类执行。 */
	virtual void ApplyAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags, UGameplayAbility* RequestingAbility, bool bEnableBlockTags, const FGameplayTagContainer& BlockTags, bool bExecuteCancelTags, const FGameplayTagContainer& CancelTags) override;
	//~End of UAbilitySystemComponent interface

	/**
	 * 把失败原因发到拥有客户端。
	 * 服务器上非本地控制的能力失败时，表现要在玩家自己的客户端播，所以需要这条 RPC。
	 * 不可靠：失败反馈丢一次不影响游戏状态。
	 */
	UFUNCTION(Client, Unreliable)
	void ClientNotifyAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason);

	/** 本地处理失败：转发给能力自己的失败反馈。 */
	void HandleAbilityFailed(const UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason);

	/**
	 * 取某个组的生效规则。
	 *
	 * 未注入配置表时返回内置默认规则（`SingleInstance` + `bNewcomerWinsOnTie = true`），
	 * 使"没配表"与"配了表但没配这个组"两种情况行为一致。
	 */
	const FGGYGOAbilityGroupRule& ResolveGroupRule(FGameplayTag GroupTag) const;

protected:
	/** Tag 关系表。为空时不做任何关系扩展。 */
	UPROPERTY()
	TObjectPtr<UGGYGOAbilityTagRelationshipMapping> TagRelationshipMapping;

	/**
	 * 组并发规则表。为空时所有组走内置默认规则。
	 *
	 * `const` 是有意的：规则是只读数据，运行时改一个组的并发语义会让
	 * 已在运行的能力处于按旧规则登记、按新规则退出的不一致状态。
	 */
	UPROPERTY()
	TObjectPtr<const UGGYGOAbilityGroupConfig> AbilityGroupConfig;

	/**
	 * 按组 Tag 索引的正在运行能力。
	 *
	 * 用弱引用是因为这里只做仲裁查询，不该延长能力实例的生命周期；
	 * 能力实例的所有权归 AbilitySpec。查询时遇到失效项直接跳过。
	 *
	 * 没有 UPROPERTY：`TMap<Tag, TArray<...>>` 这种嵌套容器不被反射支持，
	 * 而弱引用本身也不需要 GC 保护。
	 */
	TMap<FGameplayTag, TArray<TWeakObjectPtr<UGGYGOGameplayAbility>>> ActiveAbilitiesByGroup;

	/** 复核取消回调后已登记权威组状态中的活跃实例，并复用同一取消谓词。 */
	bool HasActiveAbilityMatching(TShouldCancelAbilityFunc ShouldCancelFunc) const;

	/** 仅当组当前仍为空时广播，允许同步回调先登记新的激活。 */
	void BroadcastAbilityGroupFreedIfEmpty(FGameplayTag GroupTag);

private:
	friend class FGGYGOAbilityInputBuffer;
	friend class FGGYGOAbilityStepInputRouter;
	friend class FGGYGOAbilityStepInputRequest;
	friend class FGGYGOAbilityInitialInputRoute;
	friend class UGGYGOAbilityTask_WaitComboInput;

	/** 分段输入路由的入口（供请求／路由值类型与 Task 调用）；实现见 FGGYGOAbilityStepInputRouter。 */
	FGGYGOAbilityStepInputRequest CaptureStepInputRequest(UGGYGOAbilityTask_WaitComboInput* Task,
		UGGYGOGameplayAbility* Ability,
		EGGYGOAbilityStepInputKind Kind, int32 SourceStep, int32 RequestId, FString& OutError);
	bool CheckStepInputRequestCurrent(const FGGYGOAbilityStepInputRequest& Request, FString& OutError) const;
	bool CheckStepInputRequestForTask(const FGGYGOAbilityStepInputRequest& Request,
		const UGameplayAbility* Ability, const FGGYGOAbilityActivationHandle& Activation, FString& OutError) const;
	bool CommitStepInputFact(UGGYGOAbilityTask_WaitComboInput* Task,
		const FGGYGOAbilityStepInputRequest& Request, const FGGYGOMovementInputStepFact& Fact,
		const FGGYGOAbilityInitialInputRoute& Route, FString& OutError);
	void NotifyInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route);
	bool ConsumeInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route,
		const FGGYGOAbilityActivationHandle& Original);
	/** 路由需要原生 TargetData 处理时调用原生实现。 */
	void ForwardNativeTargetData(FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey,
		const FGameplayAbilityTargetDataHandle& TargetData, FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey);
	void ForwardNativeTargetDataCancelled(FGameplayAbilitySpecHandle AbilityHandle,
		FPredictionKey AbilityOriginalPredictionKey, FPredictionKey CurrentPredictionKey);

	/** 输入缓冲与分段输入路由：ASC 私有协作者，只在 ASC 调用时执行。 */
	TUniquePtr<FGGYGOAbilityInputBuffer> InputBuffer;
	TUniquePtr<FGGYGOAbilityStepInputRouter> StepInputRouter;

	friend class UGGYGOGameplayAbility;

	/** GA 准入用：Handle 正处于输入缓冲发起、尚未被认领的那次 TryActivate 内。 */
	bool IsAbilityInputActivationAttempt(FGameplayAbilitySpecHandle Handle) const;

	/** 为本 ASC 的每次 PreActivate 分配跨实例唯一且单调递增的准入序号；0 保留为无效值。 */
	uint64 AllocateAbilityGroupAdmissionSequence();
	uint64 NextAbilityGroupAdmissionSequence = 0;

};
