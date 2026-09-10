/**
 * @file GGYGOPlayerState.h
 * @brief 玩家状态 —— 队伍的宿主
 *
 * 队伍挂在这里而不是角色上，因为队伍是玩家的属性：出战角色会换，
 * 队伍名单不会。挂在角色上会让"我的队伍有谁"在换人瞬间失去答案。
 *
 * PlayerState 还有一个必要性质：它跨越角色死亡与重生而存在。
 * 全员倒下后重开一局时，队伍配置应当还在。
 */
#pragma once

#include "GameFramework/PlayerState.h"

#include "GGYGOPlayerState.generated.h"

class UGGYGOSquadComponent;
class UObject;

UCLASS(Config = Game, meta = (ShortTooltip = "GGYGO 玩家状态"))
class GGYGO_API AGGYGOPlayerState : public APlayerState
{
	GENERATED_BODY()

public:
	AGGYGOPlayerState(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 队伍组件。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|PlayerState")
	UGGYGOSquadComponent* GetSquadComponent() const { return SquadComponent; }

private:
	/** 队伍与出战切换。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|PlayerState", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UGGYGOSquadComponent> SquadComponent;
};
