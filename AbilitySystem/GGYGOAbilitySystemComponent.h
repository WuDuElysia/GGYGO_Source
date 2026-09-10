/**
 * @file GGYGOAbilitySystemComponent.h
 * @brief 项目 ASC —— 输入缓存、组仲裁、Tag 关系扩展
 *
 * 在 GAS 原生 ASC 之上加三块能力：
 *
 * ## 1. 输入三阶段缓存
 * GAS 原生没有"按住持续尝试激活"的概念，也没有把输入与 Ability 解耦的机制。
 * 这里用 InputTag 精确匹配 AbilitySpec 的动态源标签（由 `UGGYGOAbilitySet` 授予时写入），
 * 把输入分成 pressed / held / released 三个缓存，每帧由 `ProcessAbilityInput` 统一消费。
 *
 * 分三阶段而不是按下就激活的原因：如果 held 先激活了能力，随后 pressed 又把同一次按下
 * 当成输入事件发给刚创建的实例，能力会收到一次它不该收到的 InputPressed。
 * 所以 held 和 pressed 只收集句柄，第三阶段才统一 `TryActivateAbility`。
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
 * ASC 内不设第二个队列 —— 两个队列会在"哪个才是真实待激活列表"上产生歧义。
 *
 * ## 3. Tag 关系扩展
 * 把 `UGGYGOAbilityTagRelationshipMapping` 的查询结果接进 GAS 的阻断/取消判定。
 *
 * ## 谁来调用 ProcessAbilityInput
 * 目前**没有调用方**，输入链路因此是断的。
 * 它需要每帧被驱动，归属是 `UGGYGOHeroComponent` 或 PlayerController 的 Tick，
 * 而那两处都还没有建立。这是已知边界，不是漏实现。
 */
#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
#include "NativeGameplayTags.h"

#include "GGYGOAbilitySystemComponent.generated.h"

class AActor;
class UGameplayAbility;
class UGGYGOAbilityGroupConfig;
class UGGYGOAbilityTagRelationshipMapping;
class UObject;
struct FFrame;
struct FGameplayAbilityTargetDataHandle;
struct FGGYGOAbilityGroupRule;

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

UCLASS()
class GGYGO_API UGGYGOAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UGGYGOAbilitySystemComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//~UAbilitySystemComponent interface
	/**
	 * Owner / Avatar 绑定或切换时调用。
	 * 检测到**新的 Pawn Avatar** 时通知所有能力实例，并按 OnSpawn 策略尝试激活。
	 */
	virtual void InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor) override;
	//~End of UAbilitySystemComponent interface

	/** 取消谓词。返回 true 表示该实例应被取消。 */
	typedef TFunctionRef<bool(const UGGYGOGameplayAbility* Ability, FGameplayAbilitySpecHandle Handle)> TShouldCancelAbilityFunc;

	/**
	 * 按谓词取消正在运行的能力。
	 * 遍历期间会锁住能力列表，因此谓词里不要再授予或移除能力。
	 */
	void CancelAbilitiesByFunc(TShouldCancelAbilityFunc ShouldCancelFunc, bool bReplicateCancelAbility);

	/** 取消所有由输入激活的能力（`OnInputTriggered` 与 `WhileInputActive`）。 */
	void CancelInputActivatedAbilities(bool bReplicateCancelAbility);

	/** 输入按下。把匹配该 InputTag 的 Spec 放进 pressed 与 held 缓存，不立即激活。 */
	void AbilityInputTagPressed(const FGameplayTag& InputTag);

	/** 输入释放。放进 released 缓存并从 held 移除。 */
	void AbilityInputTagReleased(const FGameplayTag& InputTag);

	/** 每帧消费输入缓存。需要外部驱动，见文件头说明。 */
	void ProcessAbilityInput(float DeltaTime, bool bGamePaused);

	/** 清空全部输入缓存（含 held）。切换 Pawn、屏蔽输入或重置时调用。 */
	void ClearAbilityInput();

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

	/**
	 * 能力激活成功后登记到组，并取消被它顶掉的能力。
	 * 由 `NotifyAbilityActivated` 调用，不要手动调。
	 */
	void AddAbilityToActivationGroup(UGGYGOGameplayAbility* Ability);

	/** 能力结束后从组中摘除；组变空时广播 `OnAbilityGroupFreed`。由 `NotifyAbilityEnded` 调用。 */
	void RemoveAbilityFromActivationGroup(UGGYGOGameplayAbility* Ability);

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
	virtual void NotifyAbilityFailed(const FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability, const FGameplayTagContainer& FailureReason) override;

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

	/** 本帧按下的句柄。`ProcessAbilityInput` 处理后清空。 */
	TArray<FGameplayAbilitySpecHandle> InputPressedSpecHandles;

	/** 本帧释放的句柄。处理后清空。 */
	TArray<FGameplayAbilitySpecHandle> InputReleasedSpecHandles;

	/** 仍在按住的句柄。**跨帧保留**，`WhileInputActive` 能力从这里持续尝试激活。 */
	TArray<FGameplayAbilitySpecHandle> InputHeldSpecHandles;

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
};
