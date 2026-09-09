/**
 * @file GGYGOPhysicalMaterialWithTags.h
 * @brief 带 GameplayTag 的物理材质
 *
 * 让"打到什么东西上"变成可查询的 GameplayTag，从而支持：
 *   - **命中反馈分流**：打金属出火花、打肉出血、打石头出碎屑。
 *     `UGGYGOGameplayAbility::ApplyAbilityTagsToGameplayEffectSpec` 会把这些 Tag
 *     并入 GE 的捕获目标 Tag，Cue 侧据此选择播哪个特效。
 *   - **材质减伤**：`GGYGODamageExecution` 可以按材质 Tag 调整最终伤害
 *     （配合 `IGGYGOAbilitySourceInterface::GetPhysicalMaterialAttenuation`）。
 *
 * 用法：给场景 / 角色的碰撞体指定本类型的物理材质资产，并在资产上配好 Tag。
 * 用普通 `UPhysicalMaterial` 时不会有任何 Tag，链路自动降级为无材质信息。
 */
#pragma once

#include "GameplayTagContainer.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#include "GGYGOPhysicalMaterialWithTags.generated.h"

class UObject;

UCLASS()
class GGYGO_API UGGYGOPhysicalMaterialWithTags : public UPhysicalMaterial
{
	GENERATED_BODY()

public:
	UGGYGOPhysicalMaterialWithTags(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 该表面携带的 Tag。命中此材质时会被并入 GE 的目标 Tag。
	 * 建议按 `SurfaceType.Metal` / `SurfaceType.Flesh` 这样的层级组织。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = PhysicalProperties, meta = (Categories = "SurfaceType"))
	FGameplayTagContainer Tags;
};
