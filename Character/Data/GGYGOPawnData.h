/**
 * @file GGYGOPawnData.h
 * @brief 一个可操控单位的静态配置
 *
 * 回答"这个角色是什么"：用哪个 Pawn 类、有哪些能力、并发规则怎么配、Tag 之间什么关系。
 * 不含任何运行时状态。
 *
 * ## 为什么要这层间接
 * 没有它的话，"角色有哪些能力"这件事只能配在 Pawn 蓝图的默认值上，于是：
 * - 想给同一个 Pawn 类换一套能力（同角色的不同形态、PVP 削弱版）就得复制整个蓝图
 * - 队伍换人时拿不到"下一个角色的配置"，因为配置和已实例化的 Pawn 绑死了
 *
 * 抽成独立资产后，`UGGYGOPawnExtensionComponent` 只认 PawnData 这一个输入，
 * 换角色就是换一份 PawnData。这对多角色队伍（决策 D2）是必需的。
 *
 * ## 与 Lyra 的差异
 * Lyra 的 `ULyraPawnData` 还带 `InputConfig` 与 `DefaultCameraMode`，
 * 由 HeroComponent 与相机系统消费。本项目那两个系统尚未建立，
 * 相应字段等到有消费方时再加 —— 提前加只会得到谁都不读的配置项。
 */
#pragma once

#include "Engine/DataAsset.h"
#include "Templates/SubclassOf.h"

#include "GGYGOPawnData.generated.h"

class APawn;
class UGGYGOAbilityGroupConfig;
class UGGYGOAbilitySet;
class UGGYGOAbilityTagRelationshipMapping;
class UGGYGOMovementSet;
class UObject;

UCLASS(BlueprintType, Const, meta = (DisplayName = "GGYGO Pawn Data", ShortTooltip = "一个可操控单位的静态配置"))
class GGYGO_API UGGYGOPawnData : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	UGGYGOPawnData(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/**
	 * 要生成的 Pawn 类。
	 *
	 * 由生成方（GameMode / 队伍管理器）读取，`UGGYGOPawnExtensionComponent` 自己不用它 ——
	 * 组件运行时 Pawn 早就存在了。放在这里是为了让"角色"这个概念只有一个配置入口：
	 * 拿到 PawnData 就能既生成实体、又初始化它的能力。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Pawn")
	TSubclassOf<APawn> PawnClass;

	/**
	 * 该单位默认拥有的能力集合。仅服务器授予。
	 *
	 * 用数组而不是单个资产，是为了让"通用能力"（受击、死亡、闪避）与
	 * "角色专属能力"分开成资产各自复用，而不是每个角色抄一份通用能力列表。
	 * 按数组顺序授予，`UGGYGOAbilitySet::GiveToAbilitySystem` 内部再按
	 * AttributeSet → Ability → GE 排序。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TArray<TObjectPtr<UGGYGOAbilitySet>> AbilitySets;

	/**
	 * 组并发规则表。注入给 ASC 的 `SetAbilityGroupConfig`。
	 *
	 * 配在 PawnData 上而不是全局单例，是因为不同单位的并发语义确实不同：
	 * 玩家角色的普攻组要能连段（`SingleInstanceQueued`），
	 * 而杂兵的攻击被打断重置反而更合理（`SingleInstance`）。
	 *
	 * 留空则 ASC 走内置默认规则，不会报错。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TObjectPtr<const UGGYGOAbilityGroupConfig> AbilityGroupConfig;

	/**
	 * Tag 关系表。注入给 ASC 的 `SetTagRelationshipMapping`。
	 *
	 * 留空则不做任何 Tag 关系扩展，能力的阻断/取消只按各自资产上声明的 Tag 生效。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
	TObjectPtr<UGGYGOAbilityTagRelationshipMapping> TagRelationshipMapping;

	/**
	 * 移动参数。注入给 `UGGYGOCharacterMovementComponent`。
	 *
	 * 放在 PawnData 而不是 Pawn 蓝图上，是为了让"同一角色的不同移动手感"
	 * （轻甲/重甲、负伤状态）能靠换资产实现，而不是复制整个角色蓝图。
	 *
	 * 留空则 CMC 全部走引擎默认值，角色仍能移动（速度是 CMC 的默认 600）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement")
	TObjectPtr<const UGGYGOMovementSet> MovementSet;
};
