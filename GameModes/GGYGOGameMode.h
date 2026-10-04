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
#include "Templates/SharedPointer.h"

#include "GGYGOGameMode.generated.h"

class APlayerController;
class AGGYGOCharacterBase;
class AGGYGOCharacterSlot;
class AGGYGOPlayerState;
class UGGYGOSquadComponent;
class FGGYGOGameFeatureSession;
class UGGYGOExperienceDefinition;
class UWorld;
class UGGYGOPawnData;
class UObject;

UCLASS(Config = Game, meta = (ShortTooltip = "按 Experience 装配的 GameMode"))
class GGYGO_API AGGYGOGameMode : public AGameModeBase
{
	GENERATED_BODY()

private:
	struct FSquadCreationContext;
	struct FUntransferredSquadActors;

public:
	AGGYGOGameMode(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 本局的玩法定义。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|GameMode")
	const UGGYGOExperienceDefinition* GetExperience() const { return Experience; }

protected:
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

	/** 接管角色生成：按 Experience 建整支队伍。 */
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;

	/** GT-only current qualification; native Active readiness belongs to the original Session. */
	bool AreGameFeaturesReady() const;

	/** 给所有还没有队伍的玩家补做装配。插件就绪后调用。 */
	void SpawnSquadForPendingPlayers();

	/** 按 Experience 为该玩家生成队伍并登记。分两阶段：先建位置，再建实体。 */
	void SpawnSquadForPlayer(APlayerController* NewPlayer);

	/** Native preparation result only; failure still exposes any original spawned Actor in OutActors. */
	bool SpawnSquadSlot(const FSquadCreationContext& Context, const UGGYGOPawnData* PawnData,
		FUntransferredSquadActors& OutActors);

	/** Does not transfer creation responsibility; the original Pawn remains in OutActors on failure. */
	bool SpawnSquadMember(const FSquadCreationContext& Context, const UGGYGOPawnData* PawnData,
		const FTransform& SpawnTransform, FUntransferredSquadActors& OutActors);
	/**
	 * 本局的玩法定义。
	 *
	 * 配在 GameMode 上而不是关卡里，是为了让同一张地图能承载不同玩法
	 * （同一个训练场既能练连招也能打靶）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GGYGO|GameMode")
	TObjectPtr<const UGGYGOExperienceDefinition> Experience;

private:
	using FGameFeatureSessionPtr = TSharedPtr<FGGYGOGameFeatureSession, ESPMode::ThreadSafe>;

	/** Actual once-only Experience -> Session startup; synchronous errors go to InitGame. */
	void InitializeGameFeatureSession(FString& ErrorMessage);
	/** GT-only shared consumer for pre-BeginPlay destruction and normal EndPlay. */
	void CloseGameFeatureSession();
	bool IsGameFeatureCallerContextCurrent(
		const UWorld& ExpectedWorld, const FGameFeatureSessionPtr& ExpectedSession) const;
	bool CanAssembleForGameFeatureContext(
		const UWorld& ExpectedWorld, const FGameFeatureSessionPtr& ExpectedSession) const;

	/** Read-only identities for one synchronous creation request, never a second assembly/Ready state. */
	struct FSquadCreationContext
	{
		TWeakObjectPtr<AGGYGOGameMode> GameMode;
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<AGGYGOPlayerState> PlayerState;
		TWeakObjectPtr<UGGYGOSquadComponent> Squad;
		TWeakObjectPtr<const UGGYGOExperienceDefinition> Experience;
		FGameFeatureSessionPtr Session;
		FString CreatorPath;
	};

	/** Only Actors actually allocated by this creator and not yet accepted by Squad. */
	struct FUntransferredSquadActors
	{
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<const UGGYGOPawnData> PawnData;
		TWeakObjectPtr<AActor> OriginalSlot;
		TWeakObjectPtr<AActor> OriginalPawn;
		FString ControllerPath;
		FString PawnDataPath;
	};

	bool IsSquadCreationContextCurrent(const FSquadCreationContext& Context) const;
	/** The single Actor.Destroy request implementation for untransferred resources. */
	static void RequestUntransferredSquadActorDestruction(
		TArray<FUntransferredSquadActors>& OriginalActors, const FString& CreatorPath);
	/** Consume stack-owned resources; retain rejected originals only in the surviving creator. */
	static bool FinishUntransferredSquadActors(TArray<FUntransferredSquadActors>& OriginalActors,
		const TWeakObjectPtr<AGGYGOGameMode>& OriginalCreator, const FString& CreatorPath,
		bool bFinalDestruction);
	void ConsumeUntransferredSquadActors(bool bFinalDestruction);

	/** Rejected live originals only; no roster, binding, current Avatar or completion state. */
	TArray<FUntransferredSquadActors> UntransferredSquadActors;
	/** Synchronous same-Controller reentry exclusion, retired by the original request stack. */
	TArray<TWeakObjectPtr<APlayerController>> ControllersCreatingSquads;
	bool bConsumingUntransferredSquadActors = false;
	bool bFinalSquadCreatorDestructionRequested = false;

	/** Own resource only; GI Loaded and native plugin state are not owned by GameMode. */
	FGameFeatureSessionPtr GameFeatureSession;
	bool bGameFeatureStartupAttempted = false;
	bool bGameFeatureCallerClosed = false;
	/** Set only by successful conversion of both empty configuration arrays; not plugin Ready. */
	bool bConfiguredNoGameFeatures = false;
};
