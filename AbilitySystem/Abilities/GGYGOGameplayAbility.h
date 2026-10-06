/**
 * @file GGYGOGameplayAbility.h
 * @brief 项目所有 GameplayAbility 的基类
 *
 * 在 GAS 原生 `UGameplayAbility` 之上加了四件事：
 *   1. **激活时机策略**（`ActivationPolicy`）：输入触发 / 输入按住 / 被授予时自动激活
 *   2. **并发控制三维度**（`GroupTag` + `ActivationPriority` + `SelfPolicy`）：见 `Groups/GGYGOAbilityGroupTypes.h`
 *   3. **可插拔额外消耗**（`AdditionalCosts`）：耐力、能量、连段计数，支持"只在命中后扣"
 *   4. **失败反馈**（`FailureTagToUserFacingMessages` / `FailureTagToAnimMontage`）：让玩家知道为什么没打出来
 *
 * ## 与 Lyra 的差异
 * Lyra 用三值枚举 `ELyraAbilityActivationGroup` 做并发控制。本项目**不照抄那个枚举**，
 * 直接用组 Tag + 优先级模型，避免先引入一套马上要被替换的类型。
 *
 * ## 尚未实现的部分
 *
 * ## 默认策略
 * 构造函数里设的四个 GAS 策略值得留意：
 * - `InstancedPerActor`：每个 Actor 一个实例，运行期状态可以存在成员变量里
 * - `LocalPredicted`：客户端先预测播放，服务器验证。动作游戏的响应感靠它
 * - `ReplicateNo`：不复制能力对象本身，只由 ASC 复制规格与激活状态
 */
#pragma once

#include "Abilities/GameplayAbility.h"
#include "AbilitySystem/GGYGOAbilityMontagePlaybackTypes.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "GameplayPrediction.h"
#include "GameplayAbilitySpecHandle.h"
// FGGYGOCameraOffset 是值成员，需要完整定义而非前向声明。
#include "Camera/GGYGOCameraMode.h"

#include "GGYGOGameplayAbility.generated.h"

struct FGameplayAbilityActivationInfo;
struct FGameplayAbilitySpec;
struct FGameplayAbilitySpecHandle;

class AActor;
class ACharacter;
class AController;
class APlayerController;
class FText;
class IGGYGOAbilitySourceInterface;
class UAnimMontage;
class UGGYGOAbilityCost;
class UGGYGOGameplayAbility;
class UGGYGOAbilitySystemComponent;
class UGGYGOCameraComponent;
class UGGYGOCameraMode;
class UGGYGOHeroComponent;
class UGameplayEffect;
class UObject;
struct FFrame;
struct FGameplayAbilityActorInfo;
struct FGameplayEffectSpec;
struct FGameplayEventData;
struct FGameplayAbilityTargetDataHandle;
struct FHitResult;

/**
 * GA issues provenance and owns each original termination; ASC supplies native exit witnesses.
 * These values do not own GAS Active/spec state.
 * Handles share immutable issuer-created history. Empty/copy/equality never prove liveness.
 */
class GGYGO_API FGGYGOAbilityActivationHandle
{
public:
	FGGYGOAbilityActivationHandle() = default;
	bool HasActivation() const { return Proof.IsValid(); }
	bool HasSameActivation(const FGGYGOAbilityActivationHandle& Other) const
	{
		return Proof.IsValid() && Proof == Other.Proof;
	}

private:
	// GA-issued nonreused identity for the original ASC/instance/Spec/ActorInfo source.
	// A zero prediction key is not an identity; this proof must distinguish each activation.
	struct FActivationProof;
	TSharedPtr<const FActivationProof> Proof{};

	friend class UGGYGOGameplayAbility;
};

class GGYGO_API FGGYGOAbilityTerminationHandle
{
public:
	FGGYGOAbilityTerminationHandle() = default;
	bool HasTermination() const { return Proof.IsValid(); }
	bool HasSameTermination(const FGGYGOAbilityTerminationHandle& Other) const
	{
		return Proof.IsValid() && Proof == Other.Proof;
	}

private:
	// GA-issued identity for the first accepted request against one original activation.
	struct FTerminationProof;
	TSharedPtr<const FTerminationProof> Proof{};

	friend class UGGYGOGameplayAbility;
};

enum class EGGYGOAbilityTerminationRequestKind : uint8
{
	None = 0,
	End,
	Cancel
};

enum class EGGYGOAbilityActivationRequestOutcome : uint8
{
	Rejected = 0,
	/** Native Try accepted its request; does not imply local activation or Commit success. */
	Accepted,
	Busy,
	Stale,
	Failed
};

enum class EGGYGOAbilityActivationRequestReason : uint8
{
	InvalidRequest = 0,
	None,
	InvalidASC,
	InvalidSpec,
	InvalidAbility,
	InvalidActorInfo,
	WrongIssuer,
	IdentityExhausted,
	MissingActivationBoundary,
	SameInstanceRetrigger,
	TerminationInProgress,
	NativeActivationRejected,
	UnsupportedEntry
};

enum class EGGYGOAbilityTerminationOutcome : uint8
{
	Rejected = 0,
	Completed,
	Accepted,
	Deferred,
	AlreadyPending,
	Busy,
	Stale,
	Failed
};

enum class EGGYGOAbilityTerminationReason : uint8
{
	InvalidRequest = 0,
	None,
	WrongIssuer,
	InvalidASC,
	InvalidAbility,
	InvalidActorInfo,
	IdentityExhausted,
	ActivationChanged,
	NotActive,
	NotCancelable,
	ScopeLocked,
	TerminationInProgress,
	ActivationCallInProgress,
	MontageCaptureFailed,
	NativeEndNotObserved,
	UnsupportedEntry,
	/** GAS resources were retired by a trusted native cleanup, not a completed gameplay request. */
	NativeCleanup
};

/**
 * Immutable first-request snapshot, created only by GA; not an alternate execution state.
 * Capture precedes cancellation broadcast/cleanup. Repeated requests cannot change these
 * parameters or recapture resources. Deferred resume carries this same original identity.
 */
class GGYGO_API FGGYGOAbilityTerminationContext
{
public:
	FGGYGOAbilityTerminationContext() = default;

	const FGGYGOAbilityActivationHandle& GetOriginalActivation() const { return OriginalActivation; }
	const FGGYGOAbilityTerminationHandle& GetOriginalTermination() const { return OriginalTermination; }
	EGGYGOAbilityTerminationRequestKind GetRequestKind() const { return RequestKind; }
	bool GetReplicateEndAbility() const { return bReplicateEndAbility; }
	bool GetReplicateCancelAbility() const { return bReplicateCancelAbility; }
	bool WasCancelled() const { return bWasCancelled; }
	const FGGYGOAbilityMontageOwnershipCheck& GetOriginalMontageCapture() const { return OriginalMontageCapture; }

private:
	FGGYGOAbilityActivationHandle OriginalActivation{};
	FGGYGOAbilityTerminationHandle OriginalTermination{};
	EGGYGOAbilityTerminationRequestKind RequestKind = EGGYGOAbilityTerminationRequestKind::None;
	// Only the replication flag for RequestKind is an input; the other remains false.
	bool bReplicateEndAbility = false;
	bool bReplicateCancelAbility = false;
	// End preserves its original bWasCancelled argument; Cancel records true.
	bool bWasCancelled = false;
	// Default remains K3 Rejected/InvalidRequest. Explicit NoOwnedPlayback is normal;
	// every other capture failure stays visible and never substitutes current playback.
	FGGYGOAbilityMontageOwnershipCheck OriginalMontageCapture{};

	friend class UGGYGOGameplayAbility;
};

/**
 * ASC-created original-source completion history, never current-idle or restart permission.
 * Only publish after original native End, outer Cancel and related controlled activation
 * calls have exited. Retire that original record and release its Busy fence before dispatch.
 * Receivers match the original activation; a successor cannot replace this copied history.
 */
class GGYGO_API FGGYGOAbilityTerminationCompletedNotice
{
public:
	FGGYGOAbilityTerminationCompletedNotice() = default;

	bool HasCompletion() const
	{
		return Outcome == EGGYGOAbilityTerminationOutcome::Completed
			&& Original.GetOriginalActivation().HasActivation()
			&& Original.GetOriginalTermination().HasTermination();
	}
	const FGGYGOAbilityTerminationContext& GetOriginal() const { return Original; }
	EGGYGOAbilityTerminationOutcome GetOutcome() const { return Outcome; }
	EGGYGOAbilityTerminationReason GetReason() const { return Reason; }

private:
	FGGYGOAbilityTerminationContext Original{};
	EGGYGOAbilityTerminationOutcome Outcome = EGGYGOAbilityTerminationOutcome::Rejected;
	EGGYGOAbilityTerminationReason Reason = EGGYGOAbilityTerminationReason::InvalidRequest;

	friend class UGGYGOAbilitySystemComponent;
};

/** Copied request result. Editing a copy cannot alter issuer history or authorize execution. */
struct GGYGO_API FGGYGOAbilityActivationRequestResult
{
	EGGYGOAbilityActivationRequestOutcome Outcome = EGGYGOAbilityActivationRequestOutcome::Rejected;
	EGGYGOAbilityActivationRequestReason Reason = EGGYGOAbilityActivationRequestReason::InvalidRequest;
	// Exact native Try return when entered; false when admission prevented the call.
	bool bNativeAccepted = false;
	// Empty for a remote-only request; a nonempty handle is observed local history, not Active.
	FGGYGOAbilityActivationHandle OriginalActivation{};
	// Exact original history if it completed synchronously, even if dispatch started a successor.
	FGGYGOAbilityTerminationCompletedNotice OriginalTerminationCompleted{};
};

/**
 * Copied first-request result. Completed requires the original completion boundary;
 * Deferred uses one original-bound delegate in native WaitingToExecute, not a new queue.
 * AlreadyPending returns the same first context; Busy never schedules activation/restart.
 */
struct GGYGO_API FGGYGOAbilityTerminationResult
{
	EGGYGOAbilityTerminationOutcome Outcome = EGGYGOAbilityTerminationOutcome::Rejected;
	EGGYGOAbilityTerminationReason Reason = EGGYGOAbilityTerminationReason::InvalidRequest;
	FGGYGOAbilityTerminationContext Original{};
};

/**
 * 一次命中共享的 GAS 载荷。
 *
 * Context、GE Spec 与 Cue 参数必须从同一次命中构造，避免不同能力各自拼装后
 * 丢失 Origin、物理材质 Tag 或目标当前 Tag。成功载荷中，EffectSpec 仅在显式
 * 未配置伤害 GE 时可以无效；非空 GE 的 Spec 构造失败不得作为 Cue 载荷返回。
 */
struct FGGYGOHitEffectPayload
{
	FGameplayEffectContextHandle EffectContext;
	FGameplayEffectSpecHandle EffectSpec;
	FGameplayCueParameters CueParameters;
};

/** 能力何时尝试激活。 */
UENUM(BlueprintType)
enum class EGGYGOAbilityActivationPolicy : uint8
{
	/** 输入按下时尝试激活一次。绝大多数动作能力用这个。 */
	OnInputTriggered,

	/** 输入按住期间持续尝试激活。用于需要反复触发的能力。 */
	WhileInputActive,

	/** 被授予 Avatar 时自动激活。用于被动能力。 */
	OnSpawn
};

/** Explicit ability configuration for giving up player control of its original Avatar. */
UENUM(BlueprintType)
enum class EGGYGOAbilityAvatarExitPolicy : uint8
{
	Cancel,
	ContinueInBackground
};

UCLASS(Abstract, HideCategories = Input, Meta = (ShortTooltip = "GGYGO 项目的 GameplayAbility 基类。"))
class GGYGO_API UGGYGOGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

	// ASC 需要读写本类的组信息与运行期状态。
	friend class UGGYGOAbilitySystemComponent;

public:
	UGGYGOGameplayAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// ===== Controlled activation identity and original termination =====

	/** Copy the actual activation only while its original resource-work source still matches.
	 * A committed same-Binding Refresh may retain End eligibility without renewing this source. */
	FGGYGOAbilityActivationHandle CaptureCurrentActivation() const;

	/**
	 * End only the authenticated original activation. First parameters/resources stay fixed;
	 * A committed same-Binding Refresh preserves termination eligibility only, not old resource work.
	 * Stale/invalid sources fail explicitly. Completed is later than native OnAbilityEnded.
	 */
	FGGYGOAbilityTerminationResult RequestAbilityEnd(const FGGYGOAbilityActivationHandle& Original,
		bool bReplicateEndAbility, bool bWasCancelled);

	/** Capture the original before cancellation broadcast; no replacement activation or retry queue. */
	FGGYGOAbilityTerminationResult RequestAbilityCancel(const FGGYGOAbilityActivationHandle& Original,
		bool bReplicateCancelAbility);

	// ===== 上下文便利查询（都可能返回 nullptr） =====

	/** 取项目 ASC。上下文未绑定或类型不符时返回 nullptr。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability")
	UGGYGOAbilitySystemComponent* GetGGYGOAbilitySystemComponentFromActorInfo() const;

	/** 取玩家控制器。AI 拥有的能力返回 nullptr。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability")
	APlayerController* GetPlayerControllerFromActorInfo() const;

	/**
	 * 取控制器。先看 ActorInfo 的 PlayerController，再沿 Owner 链向上找。
	 * 两级 ASC 布局下角色 ASC 的 Owner 是自己，所以要靠 Pawn->GetController() 那一步。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability")
	AController* GetControllerFromActorInfo() const;

	/**
	 * 取 Avatar 角色。
	 *
	 * 返回 `ACharacter*` 而不是具体项目角色类：能力应当能作用于任意角色类型，
	 * 绑定具体类会让同一个能力无法复用到载具或非玩家单位上。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability")
	ACharacter* GetCharacterFromActorInfo() const;

	// ===== 并发控制配置读取 =====

	/** 激活时机策略。 */
	EGGYGOAbilityActivationPolicy GetActivationPolicy() const { return ActivationPolicy; }

	/** Continue is an explicit class default; it never renews old resource-work provenance. */
	EGGYGOAbilityAvatarExitPolicy GetAvatarExitPolicy() const { return AvatarExitPolicy; }

	/** 所属组。为空表示不参与组仲裁。 */
	FGameplayTag GetGroupTag() const { return GroupTag; }

	/** 优先级，越大越强。跨组和同组冲突都用它比较。 */
	int32 GetActivationPriority() const { return ActivationPriority; }

	/** 自身排斥策略。 */
	EGGYGOAbilitySelfPolicy GetSelfPolicy() const { return SelfPolicy; }

	/**
	 * 被授予时尝试激活（仅 `OnSpawn` 策略）。
	 * 会跳过正在销毁的 Avatar，并按网络执行策略判断该由客户端还是服务器发起。
	 */
	void TryActivateAbilityOnSpawn(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) const;

	/**
	 * 激活失败时的反馈入口。由 ASC 调用。
	 * 先执行原生反馈（文本 / Montage 消息），再触发蓝图事件，两条链共享同一份失败原因。
	 */
	void OnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
	{
		NativeOnAbilityFailedToActivate(FailedReason);
		ScriptOnAbilityFailedToActivate(FailedReason);
	}

	/** ASC 对当前激活分发的通用纠正数据；具体载荷解释由派生能力实现。 */
	virtual void ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction);

protected:
	/** 原生失败反馈：按 Tag 查表，广播文本消息与 Montage 消息。 */
	virtual void NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const;

	/** 蓝图失败反馈。在原生反馈之后触发。 */
	UFUNCTION(BlueprintImplementableEvent)
	void ScriptOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const;

	/**
	 * 仅追加业务准入条件；final 入口已完成 ActorInfo、原生/BP 和组规则检查。
	 * 默认明确没有附加条件。派生只可追加拒绝/失败 tags，不替代核心检查，
	 * 不用本扩展点掩盖必需配置或依赖缺失。
	 */
	virtual bool CanActivateAbilityAdditional(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const;

	//~UGameplayAbility interface
	/** 在父类检查之后追加组仲裁检查。 */
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const override final;

	/** 拒绝让非 Exclusive 的能力变成不可取消（它随时可能被顶掉，必须能取消）。 */
	virtual void SetCanBeCanceled(bool bCanBeCanceled) override;

	/** 授予时通知蓝图，并尝试 OnSpawn 激活。 */
	virtual void OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) override;

	/** 移除时先通知蓝图，再交给父类清理。 */
	virtual void OnRemoveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) override;

	/** 检查基础消耗与全部 `AdditionalCosts`，任一失败即拒绝。 */
	virtual bool CheckCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, OUT FGameplayTagContainer* OptionalRelevantTags) const override;

	/** 扣除基础消耗与全部 `AdditionalCosts`。"仅命中时扣"的判定在这里统一处理。 */
	virtual void ApplyCost(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo) const override;

	/** 创建带能力来源与命中信息的自定义 EffectContext。 */
	virtual FGameplayEffectContextHandle MakeEffectContext(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo) const override;

	/** 把命中的物理材质 Tag 并入 GE 的目标 Tag，供材质分流与减伤使用。 */
	virtual void ApplyAbilityTagsToGameplayEffectSpec(FGameplayEffectSpec& Spec, FGameplayAbilitySpec* AbilitySpec) const override;

	/**
	 * 从单次碰撞统一构造 Context、可选 GE Spec 与 Cue 参数。
	 *
	 * Origin 由调用者在命中回调时显式提供并固化到 Context。物理材质 Tag 同时进入
	 * GE 的目标 Spec Tag 与 Cue 的目标 Tag；目标 ASC 的当前 Tag 也只在这里汇入 Cue。
	 * DamageEffectClass 为空是正常的无 GE 碰撞 Cue 模式。非空时必须构造有效 Spec，
	 * 包括能力级 Spec 扩展返回后的有效性；失败诊断并返回 false，OutPayload 清空。
	 * 成功返回后，GE 实际应用的免疫／拒绝结果及碰撞 Cue 政策仍由调用方处理。
	 */
	bool BuildHitEffectPayload(UAbilitySystemComponent* TargetAbilitySystemComponent,
		TSubclassOf<UGameplayEffect> DamageEffectClass, float EffectLevel,
		const FHitResult& HitResult, const FVector& Origin,
		FGGYGOHitEffectPayload& OutPayload) const;

	/** 展开 ASC 的 Tag 关系表后再判定，并把"因死亡而失败"单独标记出来。 */
	virtual bool DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& AbilitySystemComponent, const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr, OUT FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	//~End of UGameplayAbility interface

	/** Avatar 绑定完成。转发给蓝图。 */
	virtual void OnPawnAvatarSet();

	/**
	 * 应用配置好的相机接管与镜头微调，然后交给父类（父类会触发蓝图的激活事件）。
	 *
	 * 在这里做而不是留给每个能力蓝图自己调，是因为 `AbilityCameraMode` 与
	 * `CameraOffset` 是声明式配置 —— 配了就该生效，不该再要求配套写一遍调用。
	 */
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override final;

	// ===== 相机模式 =====

	/**
	 * 激活期间接管相机。
	 *
	 * 只对被玩家操控的角色有效（AI 角色没有相机组件）。
	 *
	 * HeroComponent 记录本能力的 SpecHandle 作为接管所有者。结束时释放所有权后，
	 * 相机组件会在下一次求值时把 PawnData 默认模式平滑混合回来。
	 *
	 * 运行中途可以再调一次换成别的模式（例如命中瞬间切到特写）。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability|Camera")
	void SetCameraMode(TSubclassOf<UGGYGOCameraMode> CameraMode);

	/** 停止接管相机。能力被打断时也会经 `EndAbility` 自动调用。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability|Camera")
	void ClearCameraMode();

	/**
	 * 施加一份镜头微调，叠加在**当前模式**的求值结果上。
	 *
	 * 与 `SetCameraMode` 的区别是它不换模式，因此不会丢掉当前模式的构图或锁定目标；
	 * 最终穿透恢复状态由 CameraComponent 唯一持有，不属于模式。攻击的镜头调整绝大多数属于这一类：
	 * 只是想收一点 FOV、拉近一点距离，而不是换一个机位。
	 *
	 * 能力结束时自动撤销。运行中途可以再调一次覆盖上一份。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability|Camera")
	void ApplyCameraOffset(const FGGYGOCameraOffset& Offset);

	/** 立即撤销本能力施加的镜头微调，按 `BlendOutTime` 回落。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Ability|Camera")
	void ClearCameraOffset();

	/**
	 * 能力结束时清理相机接管。
	 *
	 * final 入口统一受理；派生在 CleanupAbilityResourcesForTermination 清自身原资源。
	 * 能力可能被组仲裁取消、死亡取消或因 Avatar 销毁而结束，
	 * 相机会永久停在演出视角。
	 */
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override final;
	virtual void CancelAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateCancelAbility) override final;

	/**
	 * 激活时接管的相机模式。留空表示不换模式。
	 *
	 * 用于"换机位"级别的需求：大招演出、处决特写、锁定视角。
	 * 只是想微调距离或 FOV 的话用 `CameraOffset`，换模式会丢掉当前模式的状态。
	 *
	 * 配在能力上而非由代码指定，是为了让同一个能力类在不同角色上
	 * 能有不同的演出镜头。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Camera")
	TSubclassOf<UGGYGOCameraMode> AbilityCameraMode;

	/**
	 * 激活时施加的镜头微调，叠加在当前模式上。
	 *
	 * 这是每段攻击各配一份的地方：轻攻击可以留空，重攻击收 FOV、
	 * 冲刺攻击往后拉一点。全零表示不调整。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Camera")
	FGGYGOCameraOffset CameraOffset;

	/**
	 * 当前实际接管的模式。**运行时状态，不要在编辑器里配它**。
	 *
	 * 与配置字段 `AbilityCameraMode` 分开是必需的：本类是 `InstancedPerActor`，
	 * 实例会被复用，而 `ClearCameraMode` 要把接管状态清空。
	 * 两者共用一个字段时，第一次结束就会把配置一起清掉，之后永远不再接管相机。
	 */
	UPROPERTY(Transient)
	TSubclassOf<UGGYGOCameraMode> ActiveCameraMode;

	/**
	 * 解析能力来源，用于构造 EffectContext。
	 * 默认施加者是 Avatar；来源接口来自 Spec 的 SourceObject（如果它实现了对应接口）。
	 */
	virtual void GetAbilitySource(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, float& OutSourceLevel, const IGGYGOAbilitySourceInterface*& OutAbilitySource, AActor*& OutEffectCauser) const;

	/** 能力被授予后触发。 */
	UFUNCTION(BlueprintImplementableEvent, Category = Ability, DisplayName = "OnAbilityAdded")
	void K2_OnAbilityAdded();

	/** 能力被移除前触发。 */
	UFUNCTION(BlueprintImplementableEvent, Category = Ability, DisplayName = "OnAbilityRemoved")
	void K2_OnAbilityRemoved();

	/** Avatar 绑定后触发。 */
	UFUNCTION(BlueprintImplementableEvent, Category = Ability, DisplayName = "OnPawnAvatarSet")
	void K2_OnPawnAvatarSet();

protected:
	/** Per-activation initialization before group finalization/camera/body. Receives only the
	 * already-issued original; may end that original, after which no body may run. */
	virtual void InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original);

	/** Business body at the original parent activation position. Default calls native Super
	 * once, preserving BP activation. Original is authenticated existing native history;
	 * this hook neither issues identity nor authorizes execution from Spec/key. */
	virtual void ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData);

	/** Once per authenticated original termination, before native End. Overrides call Super.
	 * Task playback execution remains owned by the task; this hook owns only original resources. */
	virtual void CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context);

#if WITH_DEV_AUTOMATION_TESTS
	/** Synchronous read-only sampling of this invocation. No lifecycle reentry/state mutation;
	 * borrowed parameters must not escape. Return is not a native Try/full-exit witness. */
	virtual void ObserveAbilityActivationEntryForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData);
	virtual void ObserveAbilityActivationReturnForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData);
	virtual void ObserveAbilityEndEntryForTest(FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled);
#endif

	/** 何时尝试激活。默认输入触发。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Ability Activation")
	EGGYGOAbilityActivationPolicy ActivationPolicy;

	/** Default switch behavior is cancellation. Explicit Continue may be noncancelable. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Avatar Exit")
	EGGYGOAbilityAvatarExitPolicy AvatarExitPolicy = EGGYGOAbilityAvatarExitPolicy::Cancel;

	/**
	 * 所属组，取 `AbilityGroup.*`。为空表示不参与组仲裁（等价于独立运行）。
	 * 组规则配在独立 DataAsset 上，不在这里。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Ability Activation", meta = (Categories = "AbilityGroup"))
	FGameplayTag GroupTag;

	/**
	 * 优先级，越大越强。建议取 `GGYGOAbilityGroupDefaults` 里的分段值。
	 * 同优先级的默认行为（后来者是否打断先来者）由组规则的 `bNewcomerWinsOnTie` 决定。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Ability Activation")
	int32 ActivationPriority;

	/** 自身排斥策略。`Exclusive` 会压制所有组的低优先级能力。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Ability Activation")
	EGGYGOAbilitySelfPolicy SelfPolicy;

	/** 基础 Cost 之外的额外消耗。按数组顺序检查与扣除。 */
	UPROPERTY(EditDefaultsOnly, Instanced, Category = Costs)
	TArray<TObjectPtr<UGGYGOAbilityCost>> AdditionalCosts;

	/** 失败原因 Tag → 玩家可见文本。一次失败只广播第一个命中的文本。 */
	UPROPERTY(EditDefaultsOnly, Category = "Advanced")
	TMap<FGameplayTag, FText> FailureTagToUserFacingMessages;

	/** 失败原因 Tag → 失败动画。每个命中的 Tag 各广播一次。 */
	UPROPERTY(EditDefaultsOnly, Category = "Advanced")
	TMap<FGameplayTag, TObjectPtr<UAnimMontage>> FailureTagToAnimMontage;

private:
	/** Provenance only: GAS remains the sole activation/ending authority. */
	FGGYGOAbilityActivationHandle IssueControlledActivation(UGGYGOAbilitySystemComponent* OriginalASC,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		bool bHasControlledTryBoundary, EGGYGOAbilityActivationRequestReason& OutReason);
	void RetireControlledActivation();
	void RetireControlledActivationForNativeEnd(FGameplayAbilitySpecHandle Handle);
	enum class EControlledActivationValidationPurpose : uint8 { ResourceWork, Termination };
	FGGYGOAbilityActivationHandle CaptureCurrentActivationForTermination() const;
	/** Read-only admission; does not install a termination or release resources. */
	EGGYGOAbilityTerminationReason CheckAvatarSwitchExitPreflight(bool bRequiresCancellation,
		FGGYGOAbilityActivationHandle& OutOriginal) const;
	/** Shared pure admission. None certifies only the live original; End/Cancel also capture resources. */
	EGGYGOAbilityTerminationReason CheckOriginalTerminationAdmission(
		const FGGYGOAbilityActivationHandle& Original, EGGYGOAbilityTerminationRequestKind Kind,
		FGGYGOAbilityMontageOwnershipCheck& OutMontageCapture) const;
	FGGYGOAbilityActivationHandle ValidateCurrentControlledActivation(bool bRequireActive = true,
		bool bRequireSpec = true,
		EControlledActivationValidationPurpose Purpose = EControlledActivationValidationPurpose::ResourceWork) const;

	struct FOriginalTerminationRecord;
	/** Actual final End stack span; owns a dispatch only when this entry created the record. */
	class FScopedControlledActivationEnd
	{
	public:
		explicit FScopedControlledActivationEnd(UGGYGOGameplayAbility* InAbility, bool bInOwnsDispatch = false);
		~FScopedControlledActivationEnd();
		FScopedControlledActivationEnd(const FScopedControlledActivationEnd&) = delete;
		FScopedControlledActivationEnd& operator=(const FScopedControlledActivationEnd&) = delete;

	private:
		TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
		FScopedControlledActivationEnd* Previous = nullptr;
		FGGYGOAbilityActivationHandle Original;
		TSharedPtr<FOriginalTerminationRecord> Termination;
		bool bOwnsDispatch = false;
		friend class UGGYGOGameplayAbility;
	};

	/** Stack-local final Activate span, not another activation state. A termination begun
	 * inside Initialize/body retains this exact return obligation, including raw entries. */
	class FScopedAbilityActivationCall
	{
	public:
		explicit FScopedAbilityActivationCall(UGGYGOGameplayAbility* InAbility,
			const FGGYGOAbilityActivationHandle& InOriginal);
		~FScopedAbilityActivationCall();
		FScopedAbilityActivationCall(const FScopedAbilityActivationCall&) = delete;
		FScopedAbilityActivationCall& operator=(const FScopedAbilityActivationCall&) = delete;
	private:
		TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
		FScopedAbilityActivationCall* Previous = nullptr;
		FGGYGOAbilityActivationHandle Original;
		TSharedPtr<FOriginalTerminationRecord> Termination;
		friend class UGGYGOGameplayAbility;
	};
	FScopedAbilityActivationCall* AbilityActivationCall = nullptr;

	bool IsControlledActivationTerminationBusy() const;
	uint64 LastControlledActivationSerial = 0;
	FGGYGOAbilityActivationHandle CurrentControlledActivation{};
	/** Original resource lease only; business retirement cannot erase native cleanup provenance.
	 * GAS Active/spec state remains authoritative. Retired by its actual native End notification. */
	FGGYGOAbilityActivationHandle NativeCleanupActivation{};
	FScopedControlledActivationEnd* ControlledActivationEndScope = nullptr;

	/** One original resource/return obligation. GAS alone owns Active, counts and task execution.
	 * Open counters describe actual native call spans, never another activation/ending state. */
	struct FOriginalTerminationRecord
	{
		FGGYGOAbilityTerminationContext Context;
		TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
		TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
		TWeakPtr<const FGameplayAbilityActorInfo> ActorInfo;
		FGameplayAbilitySpecHandle SpecHandle;
		FGameplayAbilityActivationInfo ActivationInfo;
		TWeakObjectPtr<UGGYGOCameraComponent> CameraOffsetComponent;
		FGGYGOCameraOffsetHandle CameraOffsetHandle;
		TWeakObjectPtr<UGGYGOHeroComponent> CameraModeHero;
		FGameplayAbilitySpecHandle CameraModeSpec;
		uint64 CameraModeGeneration = 0;
		uint32 OpenDispatches = 0;
		uint32 OpenTryCalls = 0;
		bool bContinuationUsed = false;
		bool bContinuationQueued = false;
		bool bContinuationReady = false;
		bool bDriving = false;
		bool bCancelEntered = false;
		bool bCancelReturned = false;
		bool bCleanupStarted = false;
		bool bNativeEndStarted = false;
		bool bNativeEndObserved = false;
		bool bNativeEndReturned = false;
		bool bFullEndReturned = false;
		bool bSealed = false;
		/** Set only by an exact ASC native cleanup source; never qualifies business Completed. */
		bool bHasNativeCleanupSource = false;
		EGGYGOAbilityTerminationOutcome Outcome = EGGYGOAbilityTerminationOutcome::Accepted;
		EGGYGOAbilityTerminationReason Reason = EGGYGOAbilityTerminationReason::None;
		TArray<TWeakPtr<FGGYGOAbilityTerminationCompletedNotice>> TryCompletionSlots;
	};
	TSharedPtr<FOriginalTerminationRecord> BeginOriginalTermination(
		const FGGYGOAbilityActivationHandle& Original, EGGYGOAbilityTerminationRequestKind Kind,
		bool bReplicate, bool bWasCancelled, FGGYGOAbilityTerminationResult& OutResult);
	TSharedPtr<FOriginalTerminationRecord> InstallOriginalTerminationRecord(
		const FGGYGOAbilityTerminationContext& Context, UGGYGOAbilitySystemComponent* ASC,
		const TSharedPtr<const FGameplayAbilityActorInfo>& ActorInfo,
		FGameplayAbilitySpecHandle Handle, FGameplayAbilityActivationInfo ActivationInfo);
	bool HandleNativeAbilityCleanup(EGGYGOAbilityTerminationRequestKind Kind,
		FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicate, bool bWasCancelled);
	void ResumeOriginalTermination(TSharedPtr<FOriginalTerminationRecord> Record);
	void DeferOriginalTermination(const TSharedPtr<FOriginalTerminationRecord>& Record);
	EGGYGOAbilityTerminationReason CheckOriginalTerminationSource(const FOriginalTerminationRecord& Record) const;
	void ObserveOriginalNativeEnd(UGGYGOAbilitySystemComponent* OriginalASC, FGameplayAbilitySpecHandle Handle);
	void InvalidateOriginalTerminationForActivation();
	static void FailOriginalTermination(const TSharedPtr<FOriginalTerminationRecord>& Record,
		EGGYGOAbilityTerminationReason Reason);
	static void TryCompleteOriginalTermination(const TSharedPtr<FOriginalTerminationRecord>& Record);
	static FGGYGOAbilityTerminationResult GetOriginalTerminationResult(const FOriginalTerminationRecord& Record);
	uint64 LastOriginalTerminationSerial = 0;
	TSharedPtr<FOriginalTerminationRecord> OriginalTermination;

	/** 镜头微调凭证及申请时的相机接收者。 */
	TWeakObjectPtr<UGGYGOCameraComponent> AppliedCameraOffsetComponent;
	FGGYGOCameraOffsetHandle AppliedCameraOffsetHandle;

	/** 模式请求及申请时的 Hero、Spec 与代次。 */
	TWeakObjectPtr<UGGYGOHeroComponent> AppliedCameraModeHeroComponent;
	FGameplayAbilitySpecHandle AppliedCameraModeSpecHandle;
	uint64 AppliedCameraModeRequestGeneration = 0;

	/** ASC 在 PreActivate 期间开始新的准入尝试，并在同步取消回调中标记被取代的尝试。 */
	uint64 BeginAbilityGroupAdmissionAttempt(uint64 AdmissionSequence);
	uint64 GetCurrentAbilityGroupAdmissionSequence() const;
	void RejectCurrentAbilityGroupAdmission();
	void RejectAbilityGroupAdmission(uint64 AdmissionSequence);
	bool IsCurrentAbilityGroupAdmissionRejected() const;
	bool IsAbilityGroupAdmissionPending() const { return !AbilityGroupAdmissionAttempts.IsEmpty(); }
	bool IsAbilityGroupAdmissionPending(uint64 AdmissionSequence) const;
	bool IsAbilityGroupAdmissionRejected(uint64 AdmissionSequence) const;

	/** 读取并消费本次拒绝；pending 保持到最终裁决全部完成。 */
	bool ConsumeAbilityGroupAdmissionRejection(uint64 AdmissionSequence);
	void CompleteAbilityGroupAdmissionAttempt(uint64 AdmissionSequence);

	/** ASC-assigned sequences preserve attempt order across instances and synchronous reentry. */
	struct FAbilityGroupAdmissionAttempt
	{
		uint64 Sequence = 0;
		bool bRejected = false;
	};
	TArray<FAbilityGroupAdmissionAttempt> AbilityGroupAdmissionAttempts;
};
