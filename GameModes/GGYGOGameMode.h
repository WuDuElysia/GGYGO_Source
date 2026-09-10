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
class UGGYGOExperienceDefinition;
class UGGYGOPawnData;
class UObject;

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
	 * **不等待激活完成**。插件加载是异步的，而本函数在 `InitGame` 里调用、
	 * 角色生成在 `HandleStartingNewPlayer`，中间隔着若干帧，通常够用。
	 * 若插件里含有首个角色就需要的内容（能力、组件注入），
	 * 需要改为等待激活回调后再放行玩家进入。
	 */
	void ActivateGameFeatures();

	/** 按 Experience 为该玩家生成队伍并登记。 */
	void SpawnSquadForPlayer(APlayerController* NewPlayer);

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
};
