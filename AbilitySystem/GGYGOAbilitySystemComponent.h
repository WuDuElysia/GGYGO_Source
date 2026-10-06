/**
 * @file GGYGOAbilitySystemComponent.h
 * @brief 项目 ASC —— 输入缓存、组仲裁、Tag 关系扩展
 *
 * 在 GAS 原生 ASC 之上加三块能力：
 *
 * ## 1. 原输入请求与有序聚合缓存
 * ASC发行输入请求身份，首次精确匹配InputTag并固定Spec集合与原截止/Actor/World。
 * 同Spec任一原来源held即保持；只缓存首按/末真实释放边沿，Invalidated不伪造释放。
 * ProcessAbilityInput沿边沿顺序消费，再处理精确retry与held，每帧每Spec最多一次Try。
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
 * 仲裁只做"拒绝"，不做"排队"。`SingleInstanceQueued` 被拒时返回
 * `GroupOccupiedQueued` 原因，重试由意图层的输入缓冲负责，
 * 或订阅 `OnAbilityGroupFreed` 在组空出瞬间重试。
 * ASC 只暂存意图层提交给下一次输入消费的 retry 请求，不维护第二套组排队状态。
 *
 * ## 3. Tag 关系扩展
 * 把 `UGGYGOAbilityTagRelationshipMapping` 的查询结果接进 GAS 的阻断/取消判定。
 *
 * ## 谁来调用 ProcessAbilityInput
 * PlayerController 的 PostProcessInput 是唯一帧末驱动；Hero 负责物理输入与重试意图。
 */
#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAvatarBindingTypes.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "AbilitySystem/GGYGOAbilityMontagePlaybackTypes.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "NativeGameplayTags.h"
#include "Templates/SharedPointer.h"

#include "GGYGOAbilitySystemComponent.generated.h"

class AActor;
class APlayerController;
class UAnimInstance;
class UMovementComponent;
class USkeletalMeshComponent;
class UGameplayAbility;
class UGGYGOAbilityGroupConfig;
class UGGYGOAbilityTagRelationshipMapping;
class UObject;
class UWorld;
struct FFrame;
struct FGameplayAbilityTargetDataHandle;
struct FGGYGOAbilityGroupRule;
struct FGGYGOMontagePlayGuardResult;

enum class EGGYGOAvatarSwitchAbilityExitOutcome : uint8
{
	Rejected = 0,
	/** Pure query: no current cancellation is required. Not termination history. */
	Ready,
	/** All requested original cancellations actually completed in this invocation. */
	Completed,
	Busy,
	Stale,
	Failed
};

enum class EGGYGOAvatarSwitchAbilityExitReason : uint8
{
	InvalidRequest = 0,
	None,
	InvalidASC,
	NotAuthority,
	MissingContextQuery,
	BindingChanged,
	CallerExpired,
	NativeWriteBusy,
	PendingAbilityChanges,
	UnsupportedAbility,
	InvalidExitPolicy,
	AbilityNotReady,
	ActiveCancellationRequired,
	ActivationChanged,
	TerminationNotCompleted
};

/** Synchronous result/diagnostic only; does not retain a caller or own native ability state. */
struct GGYGO_API FGGYGOAvatarSwitchAbilityExitResult
{
	EGGYGOAvatarSwitchAbilityExitOutcome Outcome = EGGYGOAvatarSwitchAbilityExitOutcome::Rejected;
	EGGYGOAvatarSwitchAbilityExitReason Reason = EGGYGOAvatarSwitchAbilityExitReason::InvalidRequest;
	FGameplayAbilitySpecHandle BlockingSpec;
	EGGYGOAbilityTerminationReason TerminationReason = EGGYGOAbilityTerminationReason::None;
	EGGYGOAvatarBindingReason BindingReason = EGGYGOAvatarBindingReason::None;
};

/**
 * Subscriber-only view of immutable original termination history.
 * As with K4, private inheritance prevents callers from reaching native Broadcast/Clear;
 * only ASC may dispatch. Subscription/removal does not grant completion or restart authority.
 */
class FGGYGOAbilityTerminationCompletedEvent : private TMulticastDelegate<
	void(const FGGYGOAbilityTerminationCompletedNotice&)>
{
	using FBase = TMulticastDelegate<void(const FGGYGOAbilityTerminationCompletedNotice&)>;

public:
	using FDelegate = FBase::FDelegate;
	using FBase::Add;
	using FBase::AddLambda;
	using FBase::AddWeakLambda;
	using FBase::AddUObject;
	using FBase::AddSP;
	using FBase::AddStatic;
	using FBase::Remove;
	using FBase::RemoveAll;
	using FBase::IsBound;
	using FBase::IsBoundToObject;

	FGGYGOAbilityTerminationCompletedEvent() = default;
	FGGYGOAbilityTerminationCompletedEvent(const FGGYGOAbilityTerminationCompletedEvent&) = delete;
	FGGYGOAbilityTerminationCompletedEvent& operator=(const FGGYGOAbilityTerminationCompletedEvent&) = delete;

private:
	friend class UGGYGOAbilitySystemComponent;
};

/**
 * Copyable ASC-created publication evidence; not a current binding or publication permission.
 * Default construction is empty. Copies share immutable history without retaining actors,
 * caller queries or ActorInfo allocations. Only ASC can create and install the private proof.
 */
class GGYGO_API FGGYGOAvatarBindingPublicationReceipt final
{
public:
	FGGYGOAvatarBindingPublicationReceipt() = default;

	/**
	 * Game thread only. Reset both independent caller outputs before reading.
	 * Empty returns false with default Rejected/InvalidRequest and Invalid notice.
	 * True only means historical copies exist; it does not validate current state or readiness.
	 * Writable output copies cannot modify the stored proof or authorize publication.
	 */
	bool TryGetCommittedEvidence(FGGYGOAvatarBindingResult& OutResult,
		FGGYGOAvatarBindingNotice& OutNotice) const;

private:
	struct FCommitPublicationProof;
	TSharedPtr<const FCommitPublicationProof> Proof{};

	friend class UGGYGOAbilitySystemComponent;
};

/**
 * UE5.8 DECLARE_EVENT does not restrict Broadcast. This native subscriber view does.
 * Only ASC can dispatch; receivers still validate the receipt and their own resource.
 */
class FGGYGOAvatarBindingNoticeEvent final : private TMulticastDelegate<
	void(const FGGYGOAvatarBindingPublicationReceipt&, const FGGYGOAvatarBindingNotice&)>
{
	using FBase = TMulticastDelegate<
		void(const FGGYGOAvatarBindingPublicationReceipt&, const FGGYGOAvatarBindingNotice&)>;

public:
	using FDelegate = FBase::FDelegate;
	using FBase::Add;
	using FBase::AddLambda;
	using FBase::AddWeakLambda;
	using FBase::AddUObject;
	using FBase::AddSP;
	using FBase::AddStatic;
	using FBase::Remove;
	using FBase::RemoveAll;
	using FBase::IsBound;
	using FBase::IsBoundToObject;

	FGGYGOAvatarBindingNoticeEvent() = default;
	FGGYGOAvatarBindingNoticeEvent(const FGGYGOAvatarBindingNoticeEvent&) = delete;
	FGGYGOAvatarBindingNoticeEvent& operator=(const FGGYGOAvatarBindingNoticeEvent&) = delete;

private:
	friend class UGGYGOAbilitySystemComponent;
};

/**
 * 持有此 Tag 时整帧 Ability 输入被屏蔽。
 * 屏蔽会连 held 一起清掉，避免解除屏蔽后旧按键突然自动激活。
 */
GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_GGYGO_Gameplay_AbilityInputBlocked);

/**
 * 某个能力组的最后一个实例结束时广播。
 * @param GroupTag 空出来的组。
 *
 * 主要给 `SingleInstanceQueued` 组用：意图层订阅它就能在组空出的瞬间立即重试
 * 被缓冲的请求，不必每帧轮询。连段的衔接感依赖这个即时性。
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
	/** Actual GAS destruction supplies a synchronous native cleanup source. */
	virtual void DestroyActiveState() override;

	/**
	 * Identity-only APIs; game thread only. No native binding or write-window execution.
	 * The getter returns the last committed value, including a revoked value.
	 * Query success proves current recorded identity/fields, never binding execution or authority.
	 * OutReason is reset on entry. Output arguments must be independent caller-stack values.
	 */
	FGGYGOAvatarBindingContext GetAvatarBindingContext() const;
	EGGYGOAvatarBindingOutcome CheckAvatarBindingIdentity(
		const FGGYGOAvatarBindingIdentity& Expected, EGGYGOAvatarBindingReason& OutReason) const;
	EGGYGOAvatarBindingOutcome CheckAvatarBindingContext(
		const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason& OutReason) const;
	/**
	 * Original committed-resource cleanup only, including a still allocated destroying actor.
	 * Exact revoked provenance may authorize cleanup; this never grants binding/Ready admission.
	 * Real ActorInfo writes retire the old snapshot. No mutation or callback in this query.
	 */
	EGGYGOAvatarBindingOutcome CheckAvatarBindingCleanupContext(
		const FGGYGOAvatarBindingContext& Expected, EGGYGOAvatarBindingReason& OutReason) const;
	/**
	 * Revoke only the exact committed context; never revoke an operation which refers to it.
	 * LifecycleClosed/ActorInfoMismatch/OperationInvalidated/RequestContextExpired are valid reasons.
	 * Repeated matching revocation is idempotent. No ActorInfo writes, callbacks or native cleanup.
	 */
	bool InvalidateAvatarBinding(const FGGYGOAvatarBindingContext& Expected,
		EGGYGOAvatarBindingReason Reason, EGGYGOAvatarBindingReason& OutRejectionReason);

	/**
	 * ActorInfo transactions; game thread only. Reset the independent caller-stack
	 * OutPublication on entry; only this operation's commit creates historical evidence.
	 * Init/Clear/Refresh use the real native write window and qualified Super calls.
	 * Ordinary, never-committed Bootstrap and exact revoked-context replacement are
	 * distinct admissions. Clear consumes committed cleanup provenance and keeps its explicit mode;
	 * PreserveOwner rejects a closing Owner/ASC host. Commit is not Host publication or readiness.
	 */
	FGGYGOAvatarBindingResult TryExecuteAvatarActorInfoTransaction(
		const FGGYGOAvatarBindingRequest& Request,
		FGGYGOAvatarBindingPublicationReceipt& OutPublication);
	FGGYGOAvatarBindingResult TryBootstrapAvatarActorInfoTransaction(
		const FGGYGOAvatarBindingRequest& Request,
		FGGYGOAvatarBindingPublicationReceipt& OutPublication);
	FGGYGOAvatarBindingResult TryReplaceRevokedAvatarActorInfoTransaction(
		const FGGYGOAvatarBindingRequest& Request,
		FGGYGOAvatarBindingPublicationReceipt& OutPublication);
	/**
	 * Consume only the stored actual write of an original returned, never-committed failed Init.
	 * Required pure cleanup-scope query runs only inside the synchronous native write window.
	 * No rollback, binding commit, Receipt, Notice or Ready; the original Init remains failed.
	 * Later actual ActorInfo writers retire this resource. No source is inferred on cleanup entry.
	 */
	FGGYGOAvatarBindingResult TryCleanupFailedAvatarActorInfoInit(
		const FGGYGOAvatarBindingOperationIdentity& OriginalOperation,
		TFunction<bool()> IsOriginalCallerCurrent);

	/**
	 * Cancel in the exact original context under one synchronous native Busy window.
	 * Required pure caller query is invoked only inside that window and never retained.
	 * Snapshot both tag values and null modes: null WithTags matches all; empty matches none.
	 * Uses committed cleanup provenance, including Destroy/revoked originals; never new-work admission.
	 * Success means native return with original ownership, not that every ability has ended.
	 * Close matching publication permission only; no binding commit, Receipt or Notice.
	 */
	FGGYGOAvatarBindingResult TryCancelAvatarBindingAbilities(
		const FGGYGOAvatarBindingContext& Expected,
		const FGameplayTagContainer* WithTags,
		const FGameplayTagContainer* WithoutTags,
		TFunction<bool()> IsOriginalCallerCurrent);

	/**
	 * Classify and preflight every native active instance before any cancellation.
	 * Cancel only exact originals through RequestAbilityCancel; explicit Continue is not canceled,
	 * including noncancelable Continue. Only actual Completed results permit Completed here.
	 * The required synchronous pure query proves the original switch request before/after callbacks.
	 * No input release, ActorInfo commit, automatic retry, rollback or second ability executor.
	 */
	FGGYGOAvatarSwitchAbilityExitResult TryExitAbilitiesForAvatarSwitch(
		const FGGYGOAvatarBindingContext& Expected, TFunction<bool()> IsOriginalCallerCurrent);

	/** Pure current check: same complete Context and only supported Continue instances remain.
	 * Ready is not proof of a previous cancellation. Recheck after each external control callback. */
	FGGYGOAvatarSwitchAbilityExitResult CheckAvatarSwitchAbilitiesExited(
		const FGGYGOAvatarBindingContext& Expected) const;

	/**
	 * Remove native Cues in the exact original context under one synchronous Busy window.
	 * Required pure caller query proves its original cleanup scope independently of Ready.
	 * Uses committed cleanup provenance, including Destroy/revoked originals; never new-work admission.
	 * Preserve native Active cue tag, authority/prediction and deferred Notify semantics.
	 * Success proves synchronous return with original ownership; no binding commit or Notice.
	 */
	FGGYGOAvatarBindingResult TryRemoveAvatarBindingGameplayCues(
		const FGGYGOAvatarBindingContext& Expected,
		TFunction<bool()> IsOriginalCallerCurrent);

	/** Real native stack window; independent of the I1 identity slot. No automatic queue. */
	bool IsAvatarBindingNativeWriteBusy() const;

	/**
	 * Publish the exact current ASC-created proof after caller publication.
	 * The required pure game-thread query lives only for this synchronous call.
	 * Historical commit remains true on later publication failure; no rollback or replay.
	 */
	FGGYGOAvatarBindingResult PublishAvatarBindingNotice(
		const FGGYGOAvatarBindingPublicationReceipt& Publication,
		TFunction<bool()> IsPublicationContextCurrent);
	/** Only the exact real Dispatching proof qualifies; history and Pending never do. */
	bool IsAvatarBindingNoticeDispatching(
		const FGGYGOAvatarBindingPublicationReceipt& Publication) const;
	/** Current Dispatching/successful Consumed publication only; not global Ready. */
	bool IsAvatarBindingPublicationContextCurrent(
		const FGGYGOAvatarBindingContext& Expected) const;
	/** Native subscriptions only; external code cannot Broadcast. Game thread only. */
	FGGYGOAvatarBindingNoticeEvent& OnAvatarBindingNotice();

	//~UAbilitySystemComponent interface
	/**
	 * Owner / Avatar 绑定或切换时调用。
	 * 检测到**新的 Pawn Avatar** 时通知所有能力实例，并按 OnSpawn 策略尝试激活。
	 */
	virtual void InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor) override;
	/** Legacy native clear: window-guarded; revoke exact evidence before any write. */
	virtual void ClearActorInfo() override;
	/**
	 * Shadows the nonvirtual base method for project-typed callers; NOT an override.
	 * Base-typed or qualified base calls cannot be intercepted by this entry.
	 */
	void RefreshAbilityActorInfo();
	/** Guard Anim 使用受保护入口；非 Guard 保留唯一原生调用，明确不提供重入保护。 */
	virtual float PlayMontage(UGameplayAbility* InAnimatingAbility, FGameplayAbilityActivationInfo ActivationInfo,
		UAnimMontage* NewAnimMontage, float InPlayRate, FName StartSectionName = NAME_None,
		float StartTimeSeconds = 0.0f) override;
	//~End of UAbilitySystemComponent interface

	/**
	 * 仅供 C++ 消费的受保护播放；非 Guard 返回 Unsupported/0，绝不回退原生调用。
	 * 固定以本 ASC 协调，scope 覆盖完整 GAS Super 调用。OutResult 是本次结果副本，
	 * 调用方应使用栈上输出，不能绑定同步回调中可能已结束对象的成员。
	 * AdditionalQuery 可为空；非空时必须是无副作用的游戏线程查询，捕获引用须活过调用。
	 * 需要有效的原 ActorInfo/Avatar/Mesh/Anim 及活跃的已授予能力实例。
	 */
	float PlayMontageWithGuard(UGameplayAbility* InAnimatingAbility, FGameplayAbilityActivationInfo ActivationInfo,
		UAnimMontage* NewAnimMontage, float InPlayRate, FName StartSectionName, float StartTimeSeconds,
		FGGYGOMontagePlayGuardResult& OutResult, TFunction<bool()> IsAdditionalCallerContextCurrent);

	/** Guarded play; missing caller query/unsupported Guard never falls back to legacy play. */
	FGGYGOAbilityMontagePlaybackResult TryPlayMontageWithOwnership(UGameplayAbility* Ability,
		FGameplayAbilityActivationInfo ActivationInfo, UAnimMontage* Montage, float PlayRate,
		FName StartSection, float StartTimeSeconds, TFunction<bool()> IsOriginalCallerCurrent);
	/** Pure original-write ownership, including a stopped/removed engine instance. */
	FGGYGOAbilityMontageOwnershipCheck CheckMontagePlaybackOwnership(
		const FGGYGOAbilityMontagePlaybackHandle& Original) const;
	/** Capture only an existing authenticated record; no identity inferred from GA or asset. */
	FGGYGOAbilityMontageOwnershipCheck CaptureMontagePlaybackOwnership(UGameplayAbility* OriginalAbility,
		FGameplayAbilitySpecHandle OriginalSpecHandle, FGameplayAbilityActivationInfo OriginalActivationInfo) const;
	/** Clear only this authenticated native write, at the caller's original blend-out boundary. */
	FGGYGOAbilityMontageClearResult TryClearMontageAnimatingAbility(
		const FGGYGOAbilityMontagePlaybackHandle& Original);
	/** Observe real legacy/native mutation; these entries create no authenticated handle. */
	virtual void ClearAnimatingAbility(UGameplayAbility* Ability) override;
	virtual float PlayMontageSimulated(UAnimMontage* Montage, float PlayRate,
		FName StartSectionName = NAME_None) override;
	virtual UAnimMontage* PlaySlotAnimationAsDynamicMontage_WithFractionalLoops(UGameplayAbility* Ability,
		FGameplayAbilityActivationInfo ActivationInfo, UAnimSequenceBase* AnimAsset, FName SlotName,
		float BlendInTime, float BlendOutTime, float PlayRate = 1.0f,
		float StartTimeSeconds = 0.0f, float PlayCount = 1.0f) override;
	virtual void OnRep_ReplicatedAnimMontage() override;
	virtual void OnUnregister() override;

	/** 取消谓词。返回 true 表示该实例应被取消。 */
	typedef TFunctionRef<bool(const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle)> TShouldCancelAbilityFunc;

	/**
	 * 按谓词取消正在运行的能力。
	 * 遍历期间会锁住能力列表，因此谓词里不要再授予或移除能力。
	 */
	void CancelAbilitiesByFunc(TShouldCancelAbilityFunc ShouldCancelFunc, bool bReplicateCancelAbility);

	/** 取消所有由输入激活的能力（`OnInputTriggered` 与 `WhileInputActive`）。 */
	void CancelInputActivatedAbilities(bool bReplicateCancelAbility);

	/** 服务器纠正拥有者本次激活的能力状态；目标数据解释由具体能力负责。 */
	UFUNCTION(Client, Reliable)
	void ClientCorrectAbilityState(FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey ActivationKey,
		const FGameplayAbilityTargetDataHandle& Correction);

	/**
	 * Empty PreviousIdentity starts a real request; an assigned value only revalidates that request.
	 * Freeze first matching Specs, original Owner/Avatar/World and absolute retry deadline.
	 * Expiring a retry deadline never releases held input. No legacy Tag-to-ID adapter.
	 */
	FGGYGOAbilityInputRequestResult ReceiveAbilityInputRequest(const FGameplayTag& InputTag,
		const FGGYGOAbilityInputRequestIdentity& PreviousIdentity, double OriginalDeadline);

	/** Exact retirement. Released retains a finite tap; Invalidated never emits InputReleased. */
	FGGYGOAbilityInputRequestResult EndAbilityInputRequest(const FGGYGOAbilityInputRequestIdentity& Identity,
		EGGYGOAbilityInputRequestEndKind EndKind);

	/** Queue only an exact original request with real B0 Queued admission; never writes held. */
	FGGYGOAbilityInputRequestResult QueueAbilityInputRetry(const FGGYGOAbilityInputRetryRequest& Request);

	/** Invalid deadline sentinel only; never accepted as an initial or retry request deadline. */
	static constexpr double NoAbilityInputRetryDeadline = -1.0;

	/** 每帧消费输入缓存。需要外部驱动，见文件头说明。 */
	void ProcessAbilityInput(float DeltaTime, bool bGamePaused);

	/** 清空全部输入缓存（含 held 与 retry），并使正在消费的旧快照失效。 */
	void ClearAbilityInput();

	/**
	 * 无副作用查询某个 Spec 此刻能否激活。AI 决策用它复用 GAS 的
	 * Tag、Cooldown、Cost 与组仲裁，不在 BT 里拷贝第二套规则。
	 */
	bool CanActivateAbilityByHandle(FGameplayAbilitySpecHandle Handle, FGameplayTagContainer& OutFailureTags) const;

	// ===== Controlled native Try boundary and original termination completion =====

	/**
	 * Project entry covering one complete native Try call; not an override/interceptor of
	 * nonvirtual native Try/InternalTry/CallActivate. Reject active same-instance automatic
	 * Retrigger and termination Busy explicitly; no automatic restart/queue or alternate path.
	 * Native acceptance can be remote-only. Returned local/completed history stays original
	 * across synchronous callbacks. QueryOnly and existing B0 input-origin ownership remain separate.
	 */
	FGGYGOAbilityActivationRequestResult TryActivateAbilityWithTerminationBoundary(
		FGameplayAbilitySpecHandle Handle, bool bAllowRemoteActivation = true);

	/** Subscribable C++ event; only ASC may publish original completion after proven exits. */
	FGGYGOAbilityTerminationCompletedEvent& OnAbilityTerminationCompleted();

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

	/** These real GAS calls supply cleanup provenance, not a second ability lifecycle. */
	virtual void OnRemoveAbility(FGameplayAbilitySpec& AbilitySpec) override;

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
	/** Derived from live ASC request records only; deadline expiry does not release held. */
	TArray<FGameplayAbilitySpecHandle> InputHeldSpecHandles;

	/** Sole request/held authority; specs are fixed at first acceptance. */
	struct FAbilityInputRequestRecord
	{
		FGGYGOAbilityInputRetryRequest Request;
		TWeakObjectPtr<AActor> OwnerActor;
		TWeakObjectPtr<AActor> AvatarActor;
		TWeakObjectPtr<UWorld> World;
		TArray<FGameplayAbilitySpecHandle> SpecHandles;
		TArray<FGameplayAbilitySpecHandle> RetryableSpecHandles;
		bool bHeld = true;
	};

	/** Ordered aggregate facts, not another held authority or frame dispatcher. */
	struct FAbilityInputEdge
	{
		FGameplayAbilitySpecHandle Handle;
		uint64 InputRevision = 0;
		bool bPressed = false;
		TWeakObjectPtr<AActor> OwnerActor;
		TWeakObjectPtr<AActor> AvatarActor;
		TWeakObjectPtr<UWorld> World;
		TArray<FGGYGOAbilityInputRequestIdentity> Sources;
	};

	FGGYGOAbilityInputRequestResult MakeAbilityInputRequestFailure(
		EGGYGOAbilityInputRequestOutcome Outcome, EGGYGOAbilityInputRequestReason Reason,
		const FGGYGOAbilityInputRequestIdentity& Identity, FGameplayTag Tag, double Deadline) const;
	const FAbilityInputRequestRecord* FindAbilityInputRequest(
		const FGGYGOAbilityInputRequestIdentity& Identity) const;
	bool IsAbilityInputContextCurrent(const TWeakObjectPtr<AActor>& Owner,
		const TWeakObjectPtr<AActor>& Avatar, const TWeakObjectPtr<UWorld>& World) const;
	bool IsAbilityInputRequestCurrent(const FGGYGOAbilityInputRetryRequest& Request,
		FGameplayAbilitySpecHandle Handle, bool bRequireDeadline, bool bRequireQueued, bool bRequireHeld) const;
	bool IsAbilityInputEdgeCurrent(const FAbilityInputEdge& Edge) const;
	void RebuildAbilityInputHeldHandles();
	void PruneAbilityInputRequests();
	void ConsumeSuccessfulAbilityInputRequests(const TArray<FGGYGOAbilityInputRetryRequest>& Requests);

	TMap<uint64, FAbilityInputRequestRecord> AbilityInputRequests;
	uint64 LastAbilityInputRequestSerial = 0; // Never reset or borrowed from evaluation serials.
	TArray<FAbilityInputEdge> PendingAbilityInputEdges;
	TArray<FGGYGOAbilityInputRetryRequest> PendingAbilityInputRetries;

	enum class EAbilityActivationInputOrigin : uint8
	{
		None,
		InitialPress,
		FiniteRetry
	};

	struct FAbilityActivationFailureOrigin
	{
		EAbilityActivationInputOrigin Kind = EAbilityActivationInputOrigin::None;
		FGameplayAbilitySpecHandle Handle;
		TArray<FGGYGOAbilityInputRetryRequest> Requests;
		TWeakObjectPtr<AActor> OwnerActor;
	};

	struct FAbilityActivationEvaluationToken
	{
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> Issuer{};
		uint64 Serial = 0;
		uint64 ParentSerial = 0;
		uint64 InputAttemptSerial = 0;
		TWeakObjectPtr<const UGGYGOGameplayAbility> Ability{};
		FGameplayAbilitySpecHandle Handle{};
		FAbilityActivationFailureOrigin Origin{};
	};

	struct FAbilityInputActivationAttempt
	{
		uint64 Serial = 0;
		uint64 ParentEvaluationSerial = 0;
		uint32 QueryDepth = 0;
		FGameplayAbilitySpecHandle Handle{};
		FAbilityActivationFailureOrigin Origin{};
		bool bClaimed = false;
	};

	/** Scope metadata only. Never restore a consumed input permit or completed result. */
	class FScopedAbilityInputActivation final
	{
	public:
		FScopedAbilityInputActivation(UGGYGOAbilitySystemComponent* InASC,
			FGameplayAbilitySpecHandle Handle, const FAbilityActivationFailureOrigin& Origin);
		~FScopedAbilityInputActivation();
		FScopedAbilityInputActivation(const FScopedAbilityInputActivation&) = delete;
		FScopedAbilityInputActivation& operator=(const FScopedAbilityInputActivation&) = delete;
	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
		uint64 Serial = 0;
	};

	class FScopedAbilityActivationEvaluation final
	{
	public:
		FScopedAbilityActivationEvaluation(UGGYGOAbilitySystemComponent* InASC,
			const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle,
			const FGameplayAbilityActorInfo* ActorInfo);
		~FScopedAbilityActivationEvaluation();
		void Complete(bool bCanActivate);
		FScopedAbilityActivationEvaluation(const FScopedAbilityActivationEvaluation&) = delete;
		FScopedAbilityActivationEvaluation& operator=(const FScopedAbilityActivationEvaluation&) = delete;
	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
		FAbilityActivationEvaluationToken Token{};
		bool bFinished = false;
	};

	class FScopedAbilityActivationQuery final
	{
	public:
		explicit FScopedAbilityActivationQuery(const UGGYGOAbilitySystemComponent* InASC);
		~FScopedAbilityActivationQuery();
		FScopedAbilityActivationQuery(const FScopedAbilityActivationQuery&) = delete;
		FScopedAbilityActivationQuery& operator=(const FScopedAbilityActivationQuery&) = delete;
	private:
		TWeakObjectPtr<const UGGYGOAbilitySystemComponent> ASC{};
		uint32 Depth = 0;
	};

	bool BeginAbilityActivationEvaluation(const UGGYGOGameplayAbility* Ability,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FAbilityActivationEvaluationToken& OutToken);
	void FinishAbilityActivationEvaluation(const FAbilityActivationEvaluationToken& Token, bool bCanActivate);
	void AbortAbilityActivationEvaluation(const FAbilityActivationEvaluationToken& Token);
	bool ConsumeAbilityActivationFailureOrigin(FGameplayAbilitySpecHandle Handle,
		const UGameplayAbility* Ability, FAbilityActivationFailureOrigin& OutOrigin);
	bool IsAbilityActivationFailureOriginCurrent(const FAbilityActivationFailureOrigin& Origin) const;
	uint64 AllocateAbilityActivationOriginSerial();

	friend class UGGYGOGameplayAbility;

	struct FAvatarSwitchAbilityExitCandidate
	{
		TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
		FGameplayAbilitySpecHandle Spec;
		FGGYGOAbilityActivationHandle Original;
		EGGYGOAbilityAvatarExitPolicy Policy = EGGYGOAbilityAvatarExitPolicy::Cancel;
	};
	/** Stack-only candidate/write exclusion lease; published Ready reads remain valid.
	 * GAS still owns Active/CanBeCanceled/termination. This is not an ActorInfo native write. */
	class FScopedAvatarSwitchAbilityExit
	{
	public:
		FScopedAvatarSwitchAbilityExit(UGGYGOAbilitySystemComponent* InASC,
			const TArray<FAvatarSwitchAbilityExitCandidate>& InCandidates);
		~FScopedAvatarSwitchAbilityExit();
		FScopedAvatarSwitchAbilityExit(const FScopedAvatarSwitchAbilityExit&) = delete;
		FScopedAvatarSwitchAbilityExit& operator=(const FScopedAvatarSwitchAbilityExit&) = delete;
		bool bCancellationReservationsActive = false;
		const TArray<FAvatarSwitchAbilityExitCandidate>& Candidates;
	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	};
	FScopedAvatarSwitchAbilityExit* AvatarSwitchAbilityExitScope = nullptr;
	bool IsAvatarSwitchCancellationReserved(const UGGYGOGameplayAbility* Ability) const;
	bool CollectAvatarSwitchAbilityExitCandidates(const FGGYGOAvatarBindingContext& Expected,
		bool bRequireExited, TArray<FAvatarSwitchAbilityExitCandidate>& OutCandidates,
		FGGYGOAvatarSwitchAbilityExitResult& OutResult) const;
	/** Read-only targets captured before a native cleanup traversal, valid only in its stack. */
	struct FNativeAbilityCleanupTarget
	{
		TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
		FGameplayAbilitySpecHandle Handle;
		FPredictionKey ActivationKey;
		FGGYGOAbilityActivationHandle Activation;
	};
	enum class ENativeAbilityCleanupSource : uint8 { AvatarBindingRelease, DestroyActiveState, SpecRemoval };
	class FScopedNativeAbilityCleanup final
	{
	public:
		FScopedNativeAbilityCleanup(UGGYGOAbilitySystemComponent* InASC,
			ENativeAbilityCleanupSource InSource, const FGameplayAbilitySpec* OnlySpec = nullptr);
		~FScopedNativeAbilityCleanup();
		FScopedNativeAbilityCleanup(const FScopedNativeAbilityCleanup&) = delete;
		FScopedNativeAbilityCleanup& operator=(const FScopedNativeAbilityCleanup&) = delete;
	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		FScopedNativeAbilityCleanup* Previous = nullptr;
		ENativeAbilityCleanupSource Source;
		FGameplayAbilitySpecHandle RemovedSpec;
		TSharedPtr<const FGameplayAbilityActorInfo> Allocation;
		TArray<FNativeAbilityCleanupTarget> Targets;
		friend class UGGYGOAbilitySystemComponent;
	};
	FScopedNativeAbilityCleanup* NativeAbilityCleanupScope = nullptr;
	bool GetNativeAbilityCleanupSource(const UGGYGOGameplayAbility* Ability,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, FGGYGOAbilityActivationHandle& OutActivation,
		bool bRequireActive = true) const;
	bool IsSpecUnderNativeAbilityCleanup(FGameplayAbilitySpecHandle Handle) const;
	uint64 LastAbilityActivationOriginSerial = 0;
	FAbilityInputActivationAttempt AbilityInputActivationAttempt{};
	TArray<FAbilityActivationEvaluationToken> AbilityActivationEvaluations;
	mutable FAbilityActivationEvaluationToken PendingAbilityActivationFailure{};
	mutable uint32 AbilityActivationQueryDepth = 0;
	uint64 AbilityInputRevision = 0;
	bool bProcessingAbilityInput = false;

	/** 为本 ASC 的每次 PreActivate 分配跨实例唯一且单调递增的准入序号；0 保留为无效值。 */
	uint64 AllocateAbilityGroupAdmissionSequence();
	uint64 NextAbilityGroupAdmissionSequence = 0;

	/** Metadata validity only; not an Avatar authority, native phase or gameplay lifecycle. */
	enum class EAvatarBindingIdentityState : uint8
	{
		Unissued,
		Current,
		Revoked
	};

	enum class EAvatarBindingIdentityAdmission : uint8
	{
		Invalid = 0,
		MatchCurrentContext,
		MatchCommittedCleanupContext,
		BootstrapNeverCommitted,
		ReplaceRevokedContext
	};

	enum class EAvatarBindingSnapshotPurpose : uint8
	{
		WorkingBinding,
		CommittedCleanup
	};

	/** Read-only proof. Logical revocation retains cleanup provenance; real writes retire it. */
	struct FActualAvatarBindingActorInfoSnapshot
	{
		TSharedPtr<const FGameplayAbilityActorInfo> Allocation;
		TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
		TWeakObjectPtr<AActor> OwnerActor;
		TWeakObjectPtr<AActor> AvatarActor;
		TWeakObjectPtr<APlayerController> PlayerController;
		TWeakObjectPtr<USkeletalMeshComponent> SkeletalMeshComponent;
		TWeakObjectPtr<UMovementComponent> MovementComponent;
		TWeakObjectPtr<UAnimInstance> ActorInfoAnimInstance;
		TWeakObjectPtr<UAnimInstance> ActualAnimInstance;
		TWeakObjectPtr<AActor> CachedOwnerActor;
		TWeakObjectPtr<AActor> CachedAvatarActor;
		FName ActorInfoAffectedAnimInstanceTag = NAME_None;
		FName ASCAffectedAnimInstanceTag = NAME_None;
	};

	/** One stack-local native Try request. This is provenance, never another GAS activation state. */
	struct FControlledAbilityActivationCall
	{
		FControlledAbilityActivationCall* Previous = nullptr;
		FGameplayAbilitySpecHandle Handle;
		TWeakObjectPtr<UGameplayAbility> SpecAbility;
		TWeakObjectPtr<UGameplayAbility> EvaluationAbility;
		FActualAvatarBindingActorInfoSnapshot OriginalActual;
		FGGYGOAvatarBindingContext OriginalContext;
		uint64 ParentEvaluationSerial = 0;
		uint32 QueryDepth = 0;
		uint64 EvaluationSerial = 0;
		bool bEvaluationClaimed = false;
		bool bCanAdmitted = false;
		bool bLocalWitnessSeen = false;
		EGGYGOAbilityActivationRequestReason Failure = EGGYGOAbilityActivationRequestReason::None;
		FGGYGOAbilityActivationHandle OriginalActivation{};
		// References to GA's one original record; no ASC execution state or deferred queue.
		TArray<TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord>> TerminationExitDependencies;
		TSharedPtr<FGGYGOAbilityTerminationCompletedNotice> OriginalTerminationCompleted;
	};

	class FScopedControlledAbilityActivationCall
	{
	public:
		FScopedControlledAbilityActivationCall(UGGYGOAbilitySystemComponent* InASC,
			FControlledAbilityActivationCall& InCall);
		~FScopedControlledAbilityActivationCall();
		FScopedControlledAbilityActivationCall(const FScopedControlledAbilityActivationCall&) = delete;
		FScopedControlledAbilityActivationCall& operator=(const FScopedControlledAbilityActivationCall&) = delete;

	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		FControlledAbilityActivationCall* Call = nullptr;
	};

	EGGYGOAbilityActivationRequestReason CheckControlledAbilityActivationStart(
		const FControlledAbilityActivationCall& Call) const;
	uint64 BeginControlledAbilityActivationEvaluation(const UGGYGOGameplayAbility* Ability,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo);
	bool CompleteControlledAbilityActivationEvaluation(uint64 EvaluationSerial, bool bNativeRulesPassed);
	/** Native notification issues GA history regardless of a controlled Try; only an exact
	 * admitted Try receives an outer-return witness. Raw history never fabricates that witness. */
	void ObserveControlledAbilityActivation(FGameplayAbilitySpecHandle Handle, UGGYGOGameplayAbility* Ability);
	void RegisterOriginalTerminationTryDependencies(
		const TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord>& Record);
	void TryPublishOriginalTerminationCompleted(
		const TSharedPtr<UGGYGOGameplayAbility::FOriginalTerminationRecord>& Record);
	FControlledAbilityActivationCall* ControlledAbilityActivationCall = nullptr;
	FGGYGOAbilityTerminationCompletedEvent AbilityTerminationCompletedEvent;

	/** One pending identity receipt. It owns no predicate, callback, queue or native phase. */
	struct FAvatarBindingIdentityOperation
	{
		FGGYGOAvatarBindingOperationIdentity Identity;
		EGGYGOAvatarBindingKind Kind = EGGYGOAvatarBindingKind::Invalid;
		EGGYGOAvatarBindingClearMode ClearMode = EGGYGOAvatarBindingClearMode::None;
		EAvatarBindingIdentityAdmission Admission = EAvatarBindingIdentityAdmission::Invalid;
		FGGYGOAvatarBindingContext BeforeContext;
		FActualAvatarBindingActorInfoSnapshot BeforeActual;
		TWeakObjectPtr<AActor> ExpectedOwnerActor;
		TWeakObjectPtr<AActor> ExpectedAvatarActor;
	};

	/**
	 * Game-thread identity helpers; the execution layer owns native calls and caller queries.
	 * All outputs reset on entry and must be independent stack values.
	 * Reserve validates shape/predicate presence, but never invokes or retains the predicate.
	 * Reserve success is only a receipt. Commit requires full native return and
	 * caller-context revalidation by the execution layer; it performs neither itself.
	 */
	bool TryReserveAvatarBindingIdentityOperation(const FGGYGOAvatarBindingRequest& Request,
		EAvatarBindingIdentityAdmission Admission, FGGYGOAvatarBindingOperationIdentity& OutOperation,
		EGGYGOAvatarBindingReason& OutReason);
	bool TryCommitAvatarBindingActorInfoIdentity(const FGGYGOAvatarBindingOperationIdentity& Operation,
		FGGYGOAvatarBindingContext& OutCommittedContext, EGGYGOAvatarBindingReason& OutReason);
	bool TryCompleteAvatarBindingIdentityOperation(const FGGYGOAvatarBindingOperationIdentity& Operation,
		EGGYGOAvatarBindingReason& OutReason);
	bool InvalidateAvatarBindingIdentityOperation(const FGGYGOAvatarBindingOperationIdentity& ExpectedOperation,
		EGGYGOAvatarBindingReason Reason);

	bool CaptureAvatarBindingActualSnapshot(FActualAvatarBindingActorInfoSnapshot& OutSnapshot,
		EGGYGOAvatarBindingReason& OutReason) const;
	bool ValidateAvatarBindingActualSnapshot(const FActualAvatarBindingActorInfoSnapshot& Snapshot,
		EGGYGOAvatarBindingReason& OutReason) const;
	bool ValidateAvatarBindingActualSnapshotForPurpose(const FActualAvatarBindingActorInfoSnapshot& Snapshot,
		EAvatarBindingSnapshotPurpose Purpose, EGGYGOAvatarBindingReason& OutReason) const;
	bool IsAvatarBindingNewWorkLifecycleOpen(EGGYGOAvatarBindingReason& OutReason) const;

	/** Immutable actual-write provenance; owns no native phase, callback, retry or binding identity. */
	struct FFailedAvatarActorInfoInitCleanupProof
	{
		FGGYGOAvatarBindingOperationIdentity OriginalOperation;
		FGGYGOAvatarBindingContext Before;
		FActualAvatarBindingActorInfoSnapshot WrittenActual;
	};
	TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof> FailedAvatarActorInfoInitCleanupProof;
	TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof> CaptureReturnedAvatarActorInfoInitCleanup(
		const FAvatarBindingIdentityOperation& Original, EGGYGOAvatarBindingReason& OutReason) const;
	bool ValidateFailedAvatarActorInfoInitCleanupSource(
		const TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof>& Original,
		EGGYGOAvatarBindingReason& OutReason) const;
	bool RetainFailedAvatarActorInfoInitCleanup(
		const TSharedPtr<const FFailedAvatarActorInfoInitCleanupProof>& Original,
		EGGYGOAvatarBindingReason& OutReason);
	void RetireFailedAvatarActorInfoInitCleanup();
	/** Termination-only continuation of an already-issued ability Binding. Never renews work sources. */
	bool CheckOriginalAbilityBindingForTermination(const FGGYGOAvatarBindingContext& OriginalContext,
		const FActualAvatarBindingActorInfoSnapshot& OriginalSource) const;
	bool HasSameAvatarBindingActualSnapshot(const FActualAvatarBindingActorInfoSnapshot& First,
		const FActualAvatarBindingActorInfoSnapshot& Second) const;
	void DiscardActiveAvatarBindingIdentityOperation(bool bRevokeBeforeContext);

	/** One provenance resource for the current native Local write, not playback execution state. */
	struct FMontagePlaybackProvenance
	{
		FGGYGOAbilityMontagePlaybackHandle Playback;
		FActualAvatarBindingActorInfoSnapshot ActorInfo;
	};
	FMontagePlaybackProvenance MontagePlaybackProvenance;
	float ExecuteMontagePlayWithGuard(UGameplayAbility* Ability, FGameplayAbilityActivationInfo ActivationInfo,
		UAnimMontage* Montage, float PlayRate, FName StartSectionName, float StartTimeSeconds,
		FGGYGOMontagePlayGuardResult& OutResult, TFunction<bool()> IsAdditionalCallerContextCurrent,
		FGGYGOAbilityMontagePlaybackHandle* OutPlayback, EGGYGOAbilityMontagePlaybackReason* OutOwnershipReason);
	void RetireMontagePlaybackOwnership();
	void ReconcileMontagePlaybackOwnership();

	enum class EAvatarBindingPublicationPhase : uint8
	{
		None = 0,
		Pending,
		Dispatching,
		Consumed,
		Closed
	};

	/** One publication resource; Context is derived from its commit, never binding authority. */
	struct FAvatarBindingPublicationRecord
	{
		FGGYGOAvatarBindingPublicationReceipt Publication;
		FGGYGOAvatarBindingContext Context;
		EAvatarBindingPublicationPhase Phase = EAvatarBindingPublicationPhase::None;
	};

	/** Failure closes only this stack's exact Pending/Dispatching proof, never a successor. */
	class FScopedAvatarBindingPublication final
	{
	public:
		FScopedAvatarBindingPublication(UGGYGOAbilitySystemComponent* InASC,
			const FGGYGOAvatarBindingPublicationReceipt& InPublication);
		~FScopedAvatarBindingPublication();
		FScopedAvatarBindingPublication(const FScopedAvatarBindingPublication&) = delete;
		FScopedAvatarBindingPublication& operator=(const FScopedAvatarBindingPublication&) = delete;
	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
		FGGYGOAvatarBindingPublicationReceipt Publication;
	};

	/** One native stack window, shared by typed execution and legacy entry points. */
	class FScopedAvatarBindingNativeWrite final
	{
	public:
		FScopedAvatarBindingNativeWrite(UGGYGOAbilitySystemComponent* InASC,
			const FGGYGOAvatarBindingOperationIdentity& InOperation = {});
		~FScopedAvatarBindingNativeWrite();
		FScopedAvatarBindingNativeWrite(const FScopedAvatarBindingNativeWrite&) = delete;
		FScopedAvatarBindingNativeWrite& operator=(const FScopedAvatarBindingNativeWrite&) = delete;
		bool HasEntered() const { return bEntered; }
	private:
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC{};
		FGGYGOAvatarBindingOperationIdentity Operation{};
		bool bEntered = false;
	};

	FGGYGOAvatarBindingResult ExecuteAvatarActorInfoTransaction(
		const FGGYGOAvatarBindingRequest& Request, EAvatarBindingIdentityAdmission Admission,
		FGGYGOAvatarBindingPublicationReceipt& OutPublication);
	bool RecheckAvatarBindingExecutionOperation(
		const FGGYGOAvatarBindingOperationIdentity& Operation, bool bCheckBeforeActual,
		EGGYGOAvatarBindingReason& OutReason) const;
	/** Value-only failure mapping; does not require a live issuer after an external call. */
	static FGGYGOAvatarBindingResult MakeAvatarBindingExecutionFailure(
		FGGYGOAvatarBindingResult Result, EGGYGOAvatarBindingReason Reason);
	bool IsValidAvatarBindingPublicationProof(
		const FGGYGOAvatarBindingPublicationReceipt& Publication,
		EGGYGOAvatarBindingReason& OutReason) const;
	bool RecheckAvatarBindingPublication(
		const FGGYGOAvatarBindingPublicationReceipt& Publication,
		EAvatarBindingPublicationPhase ExpectedPhase, EGGYGOAvatarBindingReason& OutReason) const;
	void CloseAvatarBindingPublicationIfMatching(
		const FGGYGOAvatarBindingPublicationReceipt& Publication);
	void InvalidateAvatarBindingForLegacyActorInfoWrite();
	void ReleaseAvatarBindingPublicationForContext(const FGGYGOAvatarBindingContext& Expected);
	void LogLegacyAvatarActorInfoWriteRejected(const TCHAR* Entry,
		EGGYGOAvatarBindingReason Reason) const;

	bool bAvatarBindingNativeWriteBusy = false;
	/** One current reference plus exact consumption metadata; no history table or caller query. */
	FAvatarBindingPublicationRecord AvatarBindingPublicationRecord;
	FGGYGOAvatarBindingNoticeEvent AvatarBindingNoticeEvent;

	/** Binding and operation identities share this issuer; zero is invalid, exhaustion never wraps. */
	uint64 LastIssuedAvatarBindingSerial = 0;
	FGGYGOAvatarBindingContext AvatarBindingContext;
	EAvatarBindingIdentityState AvatarBindingIdentityState = EAvatarBindingIdentityState::Unissued;
	FActualAvatarBindingActorInfoSnapshot AvatarBindingActorInfoSnapshot;
	FAvatarBindingIdentityOperation ActiveAvatarBindingIdentityOperation;
};
