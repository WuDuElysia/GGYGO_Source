/**
 * @file GGYGOAttributeBaseCalculation.h
 * @brief AttributeSet 侧的属性 Base 反算：给定期望 Current，求应写入的 Base。
 */
#pragma once

#include "AbilitySystem/GGYGOAttributeBaseCalculationTypes.h"

class UAbilitySystemComponent;
class UAttributeSet;
struct FGameplayAttribute;

namespace GGYGOAttributeBase
{
	/**
	 * 游戏线程、仅权威端，用于已注册的 FGameplayAttributeData 属性。持续 GE 存在时，AttributeSet
	 * 用它把"当前值改为 DesiredCurrent"换算成 Base 写入（修复清单决定 #2）。
	 *
	 * 走原生捕获、资格判定、按通道逆算再正向复核；不写属性、不做玩法 Clamp。可能创建 GAS 的
	 * 惰性聚合器并调用其原生资格钩子。ExpectedAttributeSet 必须仍是 ASC 上该属性的宿主，
	 * ExpectedCurrent 必须仍等于当前值。Ready 只是供调用方立刻写入原生 Base 的有限候选，
	 * 不是结算历史；Clamp 策略与真实 PostAttributeChange 的确认由调用方负责。
	 * 不要跨回调或异步保留结果，捕获与聚合数据都不逃出本调用。
	 */
	GGYGO_API FGGYGOAttributeBaseCalculationResult CalculateBaseForCurrent(
		UAbilitySystemComponent* ASC, const UAttributeSet* ExpectedAttributeSet,
		const FGameplayAttribute& Attribute, float ExpectedCurrent, float DesiredCurrent);
}
