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

	/** 仅服务器；已有对象、装配/清理中或 EndPlay 后拒绝。
	 * 成功取得原三个实例；非空配置树须在原初始 Possess 中同步真实启动，失败统一回滚。
	 * 配置树为空是正常的仅装配模式，不启动备用树或等待重试。
	 */
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
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 停止关联并回收显式创建记录；生成失败和 EndPlay 共用，重复调用安全。 */
	void CleanupCreatedBoss();

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

private:
	// 创建责任独立于当前关联和 Actor Owner；换成外部 Avatar 不会转移回收责任。
	TWeakObjectPtr<AGGYGOBossState> CreatedBossState;
	TWeakObjectPtr<AGGYGOBossAIController> CreatedBossController;
	TWeakObjectPtr<AGGYGOBossCharacter> CreatedBossAvatar;

	// 仅保护同步生命周期回调；不参与 Boss 阶段或动作规则。
	bool bSpawningBoss = false;
	bool bCleaningUpBoss = false;
	bool bEndingPlay = false;
};
