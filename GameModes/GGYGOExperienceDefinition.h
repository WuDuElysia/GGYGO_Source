/**
 * @file GGYGOExperienceDefinition.h
 * @brief 一局游戏的玩法定义
 *
 * 回答"这一局怎么玩"：玩家带哪支队伍、启用哪些 GameFeature 插件。
 *
 * ## 为什么需要它
 * 没有这层抽象时，"玩家生成什么角色"只能写在 GameMode 里。于是每加一个模式
 * （训练场、剧情关、PVP）都要派生一个 GameMode，而它们的区别往往只是
 * 队伍配置不同。
 *
 * Experience 把这些差异抽成数据资产，GameMode 退化为"加载 Experience 并照它执行"。
 * 换模式变成换一个资产引用，不需要新的 C++ 类。
 *
 * ## 与 GameFeature 的关系
 * `GameFeaturesToEnable` 列出本局要激活的插件。插件可以在不改动主模块的前提下
 * 往场景里注入组件、能力、输入映射 —— 这是"新角色作为独立插件交付"的基础，
 * 也让未启用的内容完全不参与加载。
 */
#pragma once

#include "Engine/DataAsset.h"

#include "GGYGOExperienceDefinition.generated.h"

class UGGYGOPawnData;
class UObject;

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Experience Definition", ShortTooltip = "一局游戏的玩法定义"))
class GGYGO_API UGGYGOExperienceDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOExperienceDefinition(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

#if WITH_EDITOR
	/** 编辑期校验。配置错误在这里暴露，而不是等到运行时角色生成失败。 */
	virtual EDataValidationResult IsDataValid(class FDataValidationContext& Context) const override;
#endif

	/**
	 * 本局要启用的 GameFeature 插件名。
	 *
	 * 只写插件名，不写路径 —— GameFeature 子系统按名字查找已注册的插件。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Game Features")
	TArray<FString> GameFeaturesToEnable;

	/**
	 * 玩家队伍的成员配置，按出场顺序排列。
	 *
	 * 数组长度决定队伍规模。空数组是合法的（观战、纯剧情场景），
	 * 此时不生成任何角色。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Squad", meta = (TitleProperty = "PawnClass"))
	TArray<TObjectPtr<const UGGYGOPawnData>> SquadMembers;
};
