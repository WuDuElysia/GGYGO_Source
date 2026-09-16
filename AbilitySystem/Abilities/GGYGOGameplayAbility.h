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
#include "AbilitySystem/Groups/GGYGOAbilityGroupTypes.h"
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
class UGGYGOAbilitySystemComponent;
class UGGYGOCameraMode;
class UObject;
struct FFrame;
struct FGameplayAbilityActorInfo;
struct FGameplayEffectSpec;
struct FGameplayEventData;

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

UCLASS(Abstract, HideCategories = Input, Meta = (ShortTooltip = "GGYGO 项目的 GameplayAbility 基类。"))
class GGYGO_API UGGYGOGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

	// ASC 需要读写本类的组信息与运行期状态。
	friend class UGGYGOAbilitySystemComponent;

public:
	UGGYGOGameplayAbility(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

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

protected:
	/** 原生失败反馈：按 Tag 查表，广播文本消息与 Montage 消息。 */
	virtual void NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const;

	/** 蓝图失败反馈。在原生反馈之后触发。 */
	UFUNCTION(BlueprintImplementableEvent)
	void ScriptOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const;

	//~UGameplayAbility interface
	/** 在父类检查之后追加组仲裁检查。 */
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const override;

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
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;

	// ===== 相机模式 =====

	/**
	 * 激活期间接管相机。
	 *
	 * 只对被玩家操控的角色有效（AI 角色没有相机组件）。
	 *
	 * 不需要显式恢复：模式栈没有弹出操作，能力停止接管后默认模式的权重会平滑升回 1，
	 * 视角自然回去。但仍要在能力结束时调 `ClearCameraMode` 清掉运行时标记。
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
	 * 与 `SetCameraMode` 的区别是它不换模式，因此不会丢掉当前模式的状态
	 * （锁定的目标、穿墙规避的恢复进度）。攻击的镜头调整绝大多数属于这一类：
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
	 * 必须重写它而不是只在正常结束路径里清理：能力可能被组仲裁取消、
	 * 被死亡取消、或因 Avatar 销毁而结束，那些路径都不会走能力自己的收尾逻辑，
	 * 相机会永久停在演出视角。
	 */
	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;

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
	/** 何时尝试激活。默认输入触发。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|Ability Activation")
	EGGYGOAbilityActivationPolicy ActivationPolicy;

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
};
