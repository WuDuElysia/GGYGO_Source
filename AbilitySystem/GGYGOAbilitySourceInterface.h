/**
 * @file GGYGOAbilitySourceInterface.h
 * @brief 能力来源接口 —— 伤害结算时的衰减来源
 *
 * 由"发出这次伤害的东西"实现：武器实例、技能数据资产、陷阱等。
 * `FGGYGOGameplayEffectContext` 持有它的弱引用，`GGYGODamageExecution` 在服务器结算时
 * 通过它取得距离衰减与物理材质衰减系数。
 *
 * 这是纯 C++ 接口，不持有数据、没有 Tick，也不参与网络复制
 * （EffectContext 只在服务器保存来源对象，客户端拿不到）。
 */
#pragma once

#include "UObject/Interface.h"

#include "GGYGOAbilitySourceInterface.generated.h"

class UObject;
class UPhysicalMaterial;
struct FGameplayTagContainer;

/** 供 Unreal 反射系统识别的接口包装类型，不含业务逻辑。 */
UINTERFACE()
class UGGYGOAbilitySourceInterface : public UInterface
{
	GENERATED_UINTERFACE_BODY()
};

/**
 * 纯 C++ 侧接口本体。
 * 实现方需要在服务器上给出确定性的衰减系数；如果客户端预测也要用同样的公式，
 * 实现方自己负责保证两端输入一致。
 */
class IGGYGOAbilitySourceInterface
{
	GENERATED_IINTERFACE_BODY()

	/**
	 * 按距离计算衰减乘数。
	 * @param Distance   来源到目标的距离（cm）。
	 * @param SourceTags 来源侧聚合 Tag，可为空表示不提供上下文。
	 * @param TargetTags 目标侧聚合 Tag，可为空。
	 * @return 施加到基础数值上的乘数。近战动作游戏通常恒返回 1.0，远程武器才做真实衰减。
	 */
	virtual float GetDistanceAttenuation(float Distance, const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr) const = 0;

	/**
	 * 按命中的物理材质计算衰减乘数。
	 * @param PhysicalMaterial 命中材质，可为空（未命中或材质缺失）。
	 * @param SourceTags       来源侧聚合 Tag，可为空。
	 * @param TargetTags       目标侧聚合 Tag，可为空。
	 * @return 施加到基础数值上的乘数。动作游戏可用它表达"打到护甲部位减伤"。
	 */
	virtual float GetPhysicalMaterialAttenuation(const UPhysicalMaterial* PhysicalMaterial, const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr) const = 0;
};
