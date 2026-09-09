/**
 * @file GGYGOAbilitySystemGlobals.h
 * @brief 项目级 AbilitySystemGlobals —— 让自定义 EffectContext 生效
 *
 * 这个类存在的唯一理由：GAS 创建 EffectContext 时走的是
 * `UAbilitySystemGlobals::AllocGameplayEffectContext()`，只有替换掉这个全局工厂，
 * 项目内所有 `MakeEffectContext` 才会产出 `FGGYGOGameplayEffectContext`。
 *
 * **必须在 `Config/DefaultGame.ini` 注册，否则不生效：**
 * ```ini
 * [/Script/GameplayAbilities.AbilitySystemGlobals]
 * AbilitySystemGlobalsClassName="/Script/GGYGO.GGYGOAbilitySystemGlobals"
 * ```
 * 漏掉这段配置不会报错，只会让 `ExtractEffectContext` 一直返回 nullptr，
 * 表现为伤害衰减和命中材质分流静默失效。
 */
#pragma once

#include "AbilitySystemGlobals.h"

#include "GGYGOAbilitySystemGlobals.generated.h"

class UObject;
struct FGameplayEffectContext;

UCLASS(Config = Game)
class UGGYGOAbilitySystemGlobals : public UAbilitySystemGlobals
{
	GENERATED_UCLASS_BODY()

	//~UAbilitySystemGlobals interface
	/**
	 * 分配 EffectContext。
	 * @return 堆上新建的 `FGGYGOGameplayEffectContext`，生命周期交由 `FGameplayEffectContextHandle` 管理。
	 * 服务器、客户端、预测路径都走同一实现，保证三端上下文类型一致。
	 */
	virtual FGameplayEffectContext* AllocGameplayEffectContext() const override;
	//~End of UAbilitySystemGlobals interface
};
