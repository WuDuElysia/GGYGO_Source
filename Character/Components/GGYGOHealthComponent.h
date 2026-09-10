/**
 * @file GGYGOHealthComponent.h
 * @brief 生命与韧性的对外门面 + 死亡状态机
 *
 * ## 与 AttributeSet 的分工
 * `UGGYGOHealthSet` 管**数值**：属性怎么算、边界怎么钳、什么时候算跨过零。
 * 本组件管**流程**：把数值变化翻译成游戏事件，并持有"死到哪一步了"这个状态。
 *
 * 分开的理由是 AttributeSet 做不了这些事：
 * - 它不是 Actor，没有复制通道能承载死亡状态机（属性复制的是数值，不是流程阶段）
 * - 它的原生 C++ 委托蓝图绑不了，UI 与表现层需要 `BlueprintAssignable` 的动态委托
 * - "死亡"不是一个属性值，它是一段有开始和结束的过程
 *
 * 所以本组件做三件事：**原生委托转蓝图委托**、**持有可复制的死亡状态机**、**只读转发数值**。
 *
 * ## 为什么死亡要两阶段
 * `DeathStarted` 到 `FinishDeath` 之间是死亡演出区间：角色还在场、还在播动画、
 * 还可能被继续追打，但已经不该响应输入、不该被再次击杀。
 * 一个 bool 表达不了这段区间，于是"倒地动画播完了没"这个判断就会散落到各处。
 *
 * 状态机复制到客户端（`ReplicatedUsing = OnRep_DeathState`），
 * 且 `OnRep` 会把跳跃式的状态变化补成逐级调用 —— 客户端可能一次收到
 * `NotDead → DeathFinished`（两次变化合并在一个网络包里），
 * 若不补一次 `StartDeath`，绑在 `OnDeathStarted` 上的表现就整个丢失。
 *
 * ## 与 Lyra 的差异
 * 1. **多管韧性**。`OnPoiseChanged` / `OnPoiseBroken` 一并在这里转发，
 *    因为破韧和受伤是同一次命中的两个结果（决策：韧性并入 HealthSet），
 *    拆成两个组件会让监听方为一次命中绑两处。
 * 2. **不重置属性**。Lyra 在初始化时有一段 `SetNumericAttributeBase(Health, MaxHealth)`
 *    并自标为 TEMP。属性初值应由 PawnData 里的初始化 GE 给，写在这里会覆盖掉
 *    "残血复活""继承上一场血量"这类合理需求。
 * 3. **自毁 GE 配在组件上**。Lyra 从 `ULyraGameData` 全局资产取伤害 GE，
 *    本项目没有那层全局资产管理，所以做成组件的 `EditDefaultsOnly` 字段。
 */
#pragma once

#include "Components/GameFrameworkComponent.h"
#include "Templates/SubclassOf.h"

#include "GGYGOHealthComponent.generated.h"

class AActor;
class UGameplayEffect;
class UGGYGOAbilitySystemComponent;
class UGGYGOHealthComponent;
class UGGYGOHealthSet;
class UObject;
struct FFrame;
struct FGameplayEffectSpec;

/** 死亡流程事件。@param OwningActor 死亡的 Actor。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FGGYGOHealth_DeathEvent, AActor*, OwningActor);

/**
 * 属性变化事件。
 * @param Instigator 造成变化的 Actor。**客户端路径下可能为 nullptr**，绑定方必须判空。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FGGYGOHealth_AttributeChanged, UGGYGOHealthComponent*, HealthComponent, float, OldValue, float, NewValue, AActor*, Instigator);

/** 死亡流程的三个阶段。 */
UENUM(BlueprintType)
enum class EGGYGODeathState : uint8
{
	/** 活着。 */
	NotDead = 0,

	/** 生命归零，死亡演出进行中。仍在场，不响应输入。 */
	DeathStarted,

	/** 演出结束，可以销毁或进入复活流程。 */
	DeathFinished
};

UCLASS(Blueprintable, meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOHealthComponent : public UGameFrameworkComponent
{
	GENERATED_BODY()

public:
	UGGYGOHealthComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取某个 Actor 上的本组件。没有则返回 nullptr。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Health")
	static UGGYGOHealthComponent* FindHealthComponent(const AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UGGYGOHealthComponent>() : nullptr;
	}

	/**
	 * 绑定到 ASC 上的 HealthSet。
	 *
	 * 由拥有者 Pawn 在 `UGGYGOPawnExtensionComponent::OnAbilitySystemInitialized_RegisterAndCall`
	 * 的回调里调用，不要手工排在 BeginPlay 里 —— ASC 就绪时机不固定。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Health")
	void InitializeWithAbilitySystem(UGGYGOAbilitySystemComponent* InASC);

	/** 解绑。会清掉本组件施加的死亡 Tag。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Health")
	void UninitializeFromAbilitySystem();

	// ===== 只读转发。未初始化时一律返回 0，不崩 =====

	/** 当前生命值。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Health")
	float GetHealth() const;

	/** 生命上限。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Health")
	float GetMaxHealth() const;

	/** 生命值归一化到 [0, 1]。上限为 0 时返回 0 而不是除零。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Health")
	float GetHealthNormalized() const;

	/** 当前韧性。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Poise")
	float GetPoise() const;

	/** 韧性上限。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Poise")
	float GetMaxPoise() const;

	/** 韧性归一化到 [0, 1]。供韧性条 UI 使用。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Poise")
	float GetPoiseNormalized() const;

	// ===== 死亡状态机 =====

	/** 当前死亡阶段。 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Health")
	EGGYGODeathState GetDeathState() const { return DeathState; }

	/** 是否已死或正在死。能力的可激活性判断用这个，不要单独判 `DeathFinished`。 */
	UFUNCTION(BlueprintCallable, BlueprintPure = false, Category = "GGYGO|Health", meta = (ExpandBoolAsExecs = "ReturnValue"))
	bool IsDeadOrDying() const { return DeathState > EGGYGODeathState::NotDead; }

	/** 开始死亡：施加 `State.Dying`，广播 `OnDeathStarted`。重复调用是空操作。 */
	virtual void StartDeath();

	/** 结束死亡：施加 `State.Dead`，广播 `OnDeathFinished`。未开始时调用是空操作。 */
	virtual void FinishDeath();

	/**
	 * 对自己施加足以致死的伤害。用于掉出世界、区域灭杀这类不经过正常战斗流程的死亡。
	 *
	 * 走 GE 而不是直接把生命置零，是为了让死亡链路只有一条 ——
	 * 直接改属性会绕过 `PreGameplayEffectExecute` 里的免疫判定与元属性消费，
	 * 于是"无敌帧内掉出世界"这类边界情况的行为会和正常受伤不一致。
	 *
	 * 免疫穿透是无条件的：spec 上会带 `Gameplay.Damage.SelfDestruct`，
	 * `UGGYGOHealthSet` 见到它就跳过无敌帧与开发期 GodMode。自毁的定义就是必须死成。
	 *
	 * @param bFellOutOfWorld 掉出世界时为 true。只作为**死因标记**加进 spec，
	 *                        供死亡表现区分（掉出世界不播倒地动画），不影响伤害计算。
	 */
	virtual void DamageSelfDestruct(bool bFellOutOfWorld = false);

	// ===== 蓝图可绑定事件 =====

	/** 生命值变化。客户端也会触发，但 Instigator 可能为 nullptr。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOHealth_AttributeChanged OnHealthChanged;

	/** 生命上限变化。只表示上限变了，不代表受伤或治疗。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOHealth_AttributeChanged OnMaxHealthChanged;

	/** 韧性变化。供韧性条 UI。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOHealth_AttributeChanged OnPoiseChanged;

	/** 韧性归零。破韧演出与硬直 GE 由监听方发起，本组件不代劳。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOHealth_DeathEvent OnPoiseBroken;

	/** 死亡演出开始。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOHealth_DeathEvent OnDeathStarted;

	/** 死亡演出结束。销毁或复活逻辑挂这里。 */
	UPROPERTY(BlueprintAssignable)
	FGGYGOHealth_DeathEvent OnDeathFinished;

protected:
	virtual void OnUnregister() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 清掉本组件施加的 `State.Dying` / `State.Dead`。 */
	void ClearGameplayTags();

	//~HealthSet 原生委托的处理器。签名必须与 FGGYGOAttributeEvent 一致
	virtual void HandleHealthChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue);
	virtual void HandleMaxHealthChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue);
	virtual void HandleOutOfHealth(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue);
	virtual void HandlePoiseChanged(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue);
	virtual void HandlePoiseBroken(AActor* Instigator, AActor* Causer, const FGameplayEffectSpec* Spec, float Magnitude, float OldValue, float NewValue);
	//~End of handlers

	/** 死亡状态复制到达。会把跳级变化补成逐级调用。 */
	UFUNCTION()
	virtual void OnRep_DeathState(EGGYGODeathState OldDeathState);

	/**
	 * 自毁用的伤害 GE，覆盖项目默认值。
	 *
	 * 留空则用 `UGGYGOGameData::SelfDestructGameplayEffect` —— 自毁对所有角色
	 * 是同一件事，通常不需要逐个配。这个字段留给特例（例如爆炸型敌人
	 * 死亡时要连带范围伤害）。
	 */
	UPROPERTY(EditDefaultsOnly, Category = "GGYGO|Health")
	TSubclassOf<UGameplayEffect> SelfDestructEffectOverride;

	/** 本组件绑定的 ASC。 */
	UPROPERTY()
	TObjectPtr<UGGYGOAbilitySystemComponent> AbilitySystemComponent;

	/** ASC 上的 HealthSet。`const` 因为本组件只读它，写入必须走 GE。 */
	UPROPERTY()
	TObjectPtr<const UGGYGOHealthSet> HealthSet;

	/** 死亡阶段。复制以便客户端播放死亡表现。 */
	UPROPERTY(ReplicatedUsing = OnRep_DeathState)
	EGGYGODeathState DeathState;
};
