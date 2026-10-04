/**
 * @file GGYGOGameData.h
 * @brief 项目级共享资产的引用入口
 *
 * 存放项目级共享资产的软引用配置，目前包括通用GameplayEffect。
 * 是否使用共享GE由消费者显式选择；提供无状态覆盖选择接口，不擅自启用共享默认。
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
 * 资产被移动后静默失效；无法按模式差异化；加载时机分散。
 *
 * 走 PrimaryDataAsset + AssetManager 之后，引用关系是编辑器可见的，
 * 资产移动可通过编辑器更新引用，启动加载与强引用由AssetManager统一管理。
 * 运行getter只读取预载结果；Cook包含规则仍需配置，运行加载不会设置Cook规则。
 */
#pragma once

#include "Engine/DataAsset.h"
#include "Templates/SubclassOf.h"

#include "GGYGOGameData.generated.h"

class UGameplayEffect;

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Game Data", ShortTooltip = "项目级共享资产"))
class GGYGO_API UGGYGOGameData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOGameData();

	/**
	 * 取启动预载的全局实例，仅限游戏线程，不触发加载或重试。
	 *
	 * 预载未完成、Manager不可用、路径未配置或加载失败时返回nullptr。
	 * 调用方必须判空并给出指向自身功能的错误信息。
	 */
	static const UGGYGOGameData* Get();

	/** 仅转发Manager的共享GE快照；未就绪/失败返回空，不实现覆盖或回退选择。 */
	static TSubclassOf<UGameplayEffect> GetSharedDamageGameplayEffect();
	static TSubclassOf<UGameplayEffect> GetSharedHealGameplayEffect();
	static TSubclassOf<UGameplayEffect> GetSharedSelfDestructGameplayEffect();

	/**
	 * 选择伤害/治疗GE类，不施加效果，不加载、重试或缓存。
	 * 非空EffectOverride始终优先；空覆盖且bUseSharedWhenUnset为false时返回空，
	 * 为true才读取对应共享getter；共享未就绪/缺失时仍返回空，调用方负责处理。
	 * bool必须显式传入；调用方的配置开关应默认false，保持既有空覆盖不启用共享的语义。
	 * 共享读取遵循getter的游戏线程与启动快照边界；覆盖路径不查询Manager。
	 */
	static TSubclassOf<UGameplayEffect> ResolveDamageGameplayEffect(
		TSubclassOf<UGameplayEffect> EffectOverride, bool bUseSharedWhenUnset);
	static TSubclassOf<UGameplayEffect> ResolveHealGameplayEffect(
		TSubclassOf<UGameplayEffect> EffectOverride, bool bUseSharedWhenUnset);

	/**
	 * 伤害的载体 GE。
	 *
	 * 需要配一个用 `SetByCaller.Damage` 传数值、Execution 为
	 * `GGYGODamageExecution` 的即时 GE。选择共享配置的伤害来源
	 * 通过 SetByCaller 与源角色的 CombatSet 表达差异。
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
