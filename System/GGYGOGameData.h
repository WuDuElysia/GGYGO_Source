/**
 * @file GGYGOGameData.h
 * @brief 项目级共享资产的引用入口
 *
 * 存放"整个项目只有一份、且被 C++ 直接需要"的资产。目前主要是通用 GameplayEffect：
 * 伤害与治疗的载体 GE 不属于任何具体能力，它们是所有伤害来源共用的管道。
 *
 * ## 哪些资产该放这里
 * 判断标准是**由 C++ 代码直接应用、且与具体角色或能力无关**。
 *
 * - 伤害 GE：`GGYGODamageExecution` 是它的计算逻辑，但施加它的可能是攻击能力、
 *   环境伤害、跌落伤害。放在能力上会让环境伤害拿不到。
 * - 自毁 GE：掉出世界时由 HealthComponent 直接应用，没有对应的能力。
 *
 * 反过来，冷却 GE **不该**放这里 —— GAS 原生就支持在能力上配
 * `CooldownGameplayEffectClass`，每个能力的冷却时长本就不同。
 * 限制类 GE（禁止移动、无敌帧）也不该放这里，它们属于施加它们的那个能力。
 *
 * ## 为什么不用全局变量加硬编码路径
 * 那种做法有四个问题：路径拼错只能在运行时发现；无法在编辑器里看到引用关系，
 * 资产被移动后静默失效；无法按模式差异化；同步加载拖慢启动。
 *
 * 走 PrimaryDataAsset + AssetManager 之后，引用关系是编辑器可见的，
 * 资产移动会自动更新，加载时机由 AssetManager 统一管理。
 */
#pragma once

#include "Engine/DataAsset.h"
#include "Templates/SubclassOf.h"

#include "GGYGOGameData.generated.h"

class UGameplayEffect;
class UObject;

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Game Data", ShortTooltip = "项目级共享资产"))
class GGYGO_API UGGYGOGameData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOGameData();

	/**
	 * 取全局实例。由 `UGGYGOAssetManager` 按需加载并缓存。
	 *
	 * **可能返回 nullptr**：这份资产在编辑器里创建，项目早期可能还不存在。
	 * 调用方必须判空并给出指向自身功能的错误信息。
	 */
	static const UGGYGOGameData* Get();

	/**
	 * 伤害的载体 GE。
	 *
	 * 需要配一个用 `SetByCaller.Damage` 传数值、Execution 为
	 * `GGYGODamageExecution` 的即时 GE。所有伤害来源共用它，
	 * 差异通过 SetByCaller 与源角色的 CombatSet 表达。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TSoftClassPtr<UGameplayEffect> DamageGameplayEffect_SetByCaller;

	/** 治疗的载体 GE。用 `SetByCaller.Heal` 传数值。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Heal")
	TSoftClassPtr<UGameplayEffect> HealGameplayEffect_SetByCaller;

	/**
	 * 自毁用的伤害 GE。
	 *
	 * 与普通伤害 GE 分开，因为它必须穿透免疫与开发期保命规则 ——
	 * spec 上会带 `Gameplay.Damage.SelfDestruct`，而普通伤害不该带。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TSoftClassPtr<UGameplayEffect> SelfDestructGameplayEffect;
};
