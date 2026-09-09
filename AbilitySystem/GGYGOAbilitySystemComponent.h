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
 * ## 2. 组仲裁（当前是最小实现）
 * 用 `ActiveAbilitiesByGroup` 按组 Tag 索引正在运行的能力，替代 Lyra 的三元素计数数组。
 * 阶段 1 的规则是硬编码的：全局 Exclusive 排斥 + 同组按 `SingleInstance` 处理。
 * 阶段 3 接入 `UGGYGOAbilityGroupConfig` 后，每个组才能配自己的规则
 * （`Coexist` / `SingleInstance` / `SingleInstanceQueued`）。
 *
 * 优先级比较一律用 `>`（决策 D4）：同优先级时后来者胜出并打断先激活者。
 *
 * ## 3. Tag 关系扩展
 * 把 `UGGYGOAbilityTagRelationshipMapping` 的查询结果接进 GAS 的阻断/取消判定。
 *
 * ## 谁来调用 ProcessAbilityInput
 * 目前**没有调用方**。它需要每帧被驱动，正常应由 `UGGYGOHeroComponent`
 * 或 PlayerController 在 Tick 里调用，那两个都还不存在（阶段 4 / 7）。
 * 在此之前输入链路是断的，这是已知边界，不是漏实现。
 */
#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "NativeGameplayTags.h"

#include "GGYGOAbilitySystemComponent.generated.h"

class AActor;
class UGameplayAbility;
class UGGYGOAbilityTagRelationshipMapping;
class UObject;
struct FFrame;
struct FGameplayAbilityTargetDataHandle;

/**
 * 持有此 Tag 时整帧 Ability 输入被屏蔽。
 * 屏蔽会连 held 一起清掉，避免解除屏蔽后旧按键突然自动激活。
 */
GGYGO_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_GGYGO_Gameplay_AbilityInputBlocked);

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
	 *
	 * 判定两步：
	 *   1. 全局排斥：任何组里存在 `SelfPolicy == Exclusive` 且优先级**更高**的能力 → 阻断
	 *   2. 同组冲突：同组内存在优先级**更高**的能力 → 阻断
	 *
	 * 两步都用 `>`，所以同优先级不阻断——配合 `AddAbilityToActivationGroup` 里取消旧实例，
	 * 共同实现 D4 的"后来者打断先激活者"。
	 *
	 * @return true 表示应拒绝激活。
	 */
	bool IsActivationBlockedByGroup(const UGGYGOGameplayAbility* Ability) const;

	/**
	 * 能力激活成功后登记到组，并取消被它顶掉的能力。
	 * 由 `NotifyAbilityActivated` 调用，不要手动调。
	 */
	void AddAbilityToActivationGroup(UGGYGOGameplayAbility* Ability);

	/** 能力结束后从组中摘除。由 `NotifyAbilityEnded` 调用。 */
	void RemoveAbilityFromActivationGroup(UGGYGOGameplayAbility* Ability);

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

protected:
	/** Tag 关系表。为空时不做任何关系扩展。 */
	UPROPERTY()
	TObjectPtr<UGGYGOAbilityTagRelationshipMapping> TagRelationshipMapping;

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
