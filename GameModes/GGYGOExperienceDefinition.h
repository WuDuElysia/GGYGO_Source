/**
 * @file GGYGOExperienceDefinition.h
 * @brief 一局游戏的玩法定义
 *
 * 回答"这一局怎么玩"：默认带哪支队伍、启用哪些 GameFeature 插件。
 *
 * ## 与编队的分工
 * 本资产里的队伍配置是**默认编队**，不是本局的最终阵容。
 * 玩家的编成结果放在 `UGGYGOSquadComponent` 的编队名单上，装配时它优先。
 *
 * 这样分是因为两者的变化频率与归属完全不同：玩法配置属于关卡与模式，
 * 由策划改；编队属于玩家，每局都可能不一样。写在同一处会导致
 * "改玩家阵容要改玩法资产"。
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
	 * **默认编队**：玩家没有做过编成时用的成员配置，按出场顺序排列。
	 *
	 * 这不是"本局一定会用的阵容"。实际阵容由
	 * `UGGYGOSquadComponent::SetRoster` 设置的编队名单决定，
	 * 只有名单为空时才回落到这里（新档、调试关卡、自动化测试）。
	 *
	 * 之所以保留这份默认值而不是要求必须先编成：没有编成界面的场景
	 * （单元测试、直接从编辑器起某个关卡）也应当能跑出可操作的角色，
	 * 否则每次调试都要先走一遍编成流程。
	 *
	 * 数组长度决定默认队伍规模，上限见 `GGYGO_MAX_SQUAD_SIZE`。
	 * 空数组是合法的（观战、纯剧情场景），此时若玩家也没有编队则不生成任何角色。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Squad", meta = (TitleProperty = "PawnClass"))
	TArray<TObjectPtr<const UGGYGOPawnData>> SquadMembers;
};
