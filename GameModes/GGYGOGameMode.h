/**
 * @file GGYGOGameMode.h
 * @brief 按 Experience 装配一局游戏
 *
 * 本类不含任何玩法规则，只做一件事：读 Experience 定义，按它生成玩家队伍。
 * 换玩法模式应当换 Experience 资产，而不是派生新的 GameMode。
 *
 * ## 为什么要接管角色生成
 * 引擎默认的生成流程是"按 DefaultPawnClass 生成一个 Pawn 并附身"。
 * 队伍制需要的是"生成 N 个 Pawn、登记进队伍、只附身第一个"，
 * 这与默认流程冲突，所以关掉默认生成（`DefaultPawnClass` 置空不够，
 * 引擎会回退到基类 Pawn），改为在 `HandleStartingNewPlayer` 里自己做。
 */
#pragma once

#include "GameFramework/GameModeBase.h"

#include "GGYGOGameMode.generated.h"

class APlayerController;
class AGGYGOCharacterBase;
class AGGYGOCharacterSlot;
class UGGYGOExperienceDefinition;
class UGGYGOPawnData;
class UObject;

// 只作为 const 引用参数出现，前向声明即可，不必把 GameFeaturesSubsystem.h
// 拉进本头文件（Lyra 的 ExperienceManagerComponent 同样这样处理）。
namespace UE::GameFeatures { struct FResult; }

UCLASS(Config = Game, meta = (ShortTooltip = "按 Experience 装配的 GameMode"))
class GGYGO_API AGGYGOGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGGYGOGameMode(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 本局的玩法定义。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|GameMode")
	const UGGYGOExperienceDefinition* GetExperience() const { return Experience; }

protected:
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

	/** 接管角色生成：按 Experience 建整支队伍。 */
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

	/**
	 * 激活 Experience 列出的 GameFeature 插件。
	 *
	 * 在 `InitGame` 调用，尽早开始异步加载。每个插件的完成回调会递减
	 * `PendingGameFeatureCount`，归零时放行等待中的玩家。
	 *
	 * 插件列表为空时立即就绪，装配流程不受影响。
	 */
	void ActivateGameFeatures();

	/**
	 * 单个 GameFeature 插件激活结束。
	 *
	 * **加载失败也算结束**：否则一个装不上的插件会让所有玩家永远停在等待里，
	 * 症状是"进游戏没有角色"，比缺一个插件的内容严重得多。
	 * 失败只报错，不阻断放行。
	 */
	void OnGameFeatureActivated(const UE::GameFeatures::FResult& Result, FString PluginURL);

	/** GameFeature 是否已全部激活完毕（或本局没有插件要激活）。 */
	bool AreGameFeaturesReady() const { return PendingGameFeatureCount <= 0; }

	/** 给所有还没有队伍的玩家补做装配。插件就绪后调用。 */
	void SpawnSquadForPendingPlayers();

	/** 按 Experience 为该玩家生成队伍并登记。分两阶段：先建位置，再建实体。 */
	void SpawnSquadForPlayer(APlayerController* NewPlayer);

	/**
	 * 生成一个队伍位置并装载角色定义。
	 *
	 * 位置持有 ASC 与属性集，必须先于实体存在。
	 * `OwningPlayer` 会被设为位置的 Owner —— GAS 的预测链依赖它，不能省。
	 */
	AGGYGOCharacterSlot* SpawnSquadSlot(APlayerController* OwningPlayer, const UGGYGOPawnData* PawnData);

	/** 生成单个成员并完成 PawnData 注入。 */
	AGGYGOCharacterBase* SpawnSquadMember(APlayerController* OwningPlayer, const UGGYGOPawnData* PawnData, const FTransform& SpawnTransform);

	/**
	 * 本局的玩法定义。
	 *
	 * 配在 GameMode 上而不是关卡里，是为了让同一张地图能承载不同玩法
	 * （同一个训练场既能练连招也能打靶）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|GameMode")
	TObjectPtr<const UGGYGOExperienceDefinition> Experience;

	/**
	 * 尚未完成激活的 GameFeature 插件数。
	 *
	 * 装配必须等它归零：插件里的 `GameFeatureAction` 可能要往角色类上注入组件或
	 * 授予能力，那些动作在激活完成时才执行。先生成角色就会拿到一个缺内容的角色，
	 * 而且不报错 —— 表现为"技能偶发缺失"，只在慢盘或大插件上出现。
	 *
	 * 不用 bool 而用计数：多个插件各自异步回调，只有全部结束才算就绪。
	 */
	int32 PendingGameFeatureCount = 0;
};
