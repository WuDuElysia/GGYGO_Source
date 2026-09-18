/** @file GGYGOBossEncounter.h @brief 阶段 B 的最小 Boss 装配入口 */
#pragma once

#include "GameFramework/Actor.h"

#include "GGYGOBossEncounter.generated.h"

class AGGYGOBossAIController;
class AGGYGOBossCharacter;
class AGGYGOBossState;
class UGGYGOBossDefinition;
class USceneComponent;

/**
 * 关卡可放置的最小生成方。只装配 BossState、Controller 与初始 Avatar；
 * 不负责逐个技能选择、仇恨、阶段阈值、奖励或转形态。
 */
UCLASS(Blueprintable)
class GGYGO_API AGGYGOBossEncounter : public AActor
{
	GENERATED_BODY()

public:
	AGGYGOBossEncounter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 仅服务器、仅一次。成功后三个实例都可通过下方访问器取得。 */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "GGYGO|Boss")
	bool SpawnBoss();

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AGGYGOBossState* GetBossState() const { return BossState; }

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AGGYGOBossCharacter* GetBossAvatar() const { return BossAvatar; }

	UFUNCTION(BlueprintPure, Category = "GGYGO|Boss")
	AGGYGOBossAIController* GetBossController() const { return BossController; }

protected:
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<const UGGYGOBossDefinition> BossDefinition;

	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	bool bSpawnOnBeginPlay = true;

	/** AI 随机决策种子；同一配置可复现相同选择序列。 */
	UPROPERTY(EditInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	int32 EncounterSeed = 1337;

	UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<AGGYGOBossState> BossState;

	UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<AGGYGOBossAIController> BossController;

	UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "GGYGO|Boss")
	TObjectPtr<AGGYGOBossCharacter> BossAvatar;
};
