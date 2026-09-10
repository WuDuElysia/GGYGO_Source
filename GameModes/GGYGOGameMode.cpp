/**
 * @file GGYGOGameMode.cpp
 * @brief GameMode 实现
 */
#include "GameModes/GGYGOGameMode.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Character/GGYGOCharacterBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameFeaturesSubsystem.h"
#include "GameModes/GGYGOExperienceDefinition.h"
#include "Player/GGYGOPlayerController.h"
#include "Player/GGYGOPlayerState.h"
#include "Teams/GGYGOSquadComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameMode)

AGGYGOGameMode::AGGYGOGameMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PlayerControllerClass = AGGYGOPlayerController::StaticClass();
	PlayerStateClass = AGGYGOPlayerState::StaticClass();

	// 关掉引擎的默认生成。队伍制要生成 N 个 Pawn 并只附身第一个，
	// 而默认流程只生成一个并立刻附身。
	//
	// 置空 DefaultPawnClass 不够 —— 引擎会回退到基类 APawn 生成一个空壳，
	// 所以还要覆盖 HandleStartingNewPlayer 来彻底绕开那条路径。
	DefaultPawnClass = nullptr;
	bStartPlayersAsSpectators = false;
}

void AGGYGOGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	if (!Experience)
	{
		// 没有 Experience 就不会生成任何角色，玩家会停在一个空场景里。
		// 这几乎总是配置遗漏，所以报错而不是静默。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("InitGame: GameMode [%s] 没有配置 Experience，不会生成任何角色。"), *GetNameSafe(this));
		return;
	}

	// 尽早激活插件，给异步加载留出时间。
	ActivateGameFeatures();
}

void AGGYGOGameMode::ActivateGameFeatures()
{
	if (!Experience)
	{
		return;
	}

	UGameFeaturesSubsystem& Subsystem = UGameFeaturesSubsystem::Get();

	for (const FString& PluginName : Experience->GameFeaturesToEnable)
	{
		if (PluginName.IsEmpty())
		{
			continue;
		}

		FString PluginURL;
		if (!Subsystem.GetPluginURLByName(PluginName, PluginURL))
		{
			// 插件名写错或插件未安装。报错而不是静默跳过 ——
			// 静默会让"技能没生效"这种问题追查到完全无关的地方。
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("ActivateGameFeatures: 找不到名为 [%s] 的 GameFeature 插件。"), *PluginName);
			continue;
		}

		Subsystem.LoadAndActivateGameFeaturePlugin(PluginURL, FGameFeaturePluginLoadComplete());
	}
}

void AGGYGOGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// 有意不调用 Super：父类会走默认的"生成一个 Pawn 并附身"流程。
	if (!NewPlayer)
	{
		return;
	}

	SpawnSquadForPlayer(NewPlayer);
}

void AGGYGOGameMode::SpawnSquadForPlayer(APlayerController* NewPlayer)
{
	if (!Experience || Experience->SquadMembers.Num() == 0)
	{
		return;
	}

	AGGYGOPlayerState* GGYGOPlayerState = NewPlayer->GetPlayerState<AGGYGOPlayerState>();
	UGGYGOSquadComponent* SquadComponent = GGYGOPlayerState ? GGYGOPlayerState->GetSquadComponent() : nullptr;
	if (!SquadComponent)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SpawnSquadForPlayer: PlayerState 上没有 SquadComponent，无法生成队伍。"));
		return;
	}

	// 全员生成在同一个出生点。
	//
	// 非出战成员会被立刻隐藏并关闭碰撞，所以位置重叠不会造成挤压。
	// 分散生成反而有害：切人时角色会从别处瞬移过来。
	const AActor* StartSpot = ChoosePlayerStart(NewPlayer);
	const FTransform SpawnTransform = StartSpot
		? StartSpot->GetActorTransform()
		: FTransform::Identity;

	for (const TObjectPtr<const UGGYGOPawnData>& PawnData : Experience->SquadMembers)
	{
		if (!PawnData)
		{
			continue;
		}

		if (AGGYGOCharacterBase* Member = SpawnSquadMember(NewPlayer, PawnData, SpawnTransform))
		{
			// 第一个登记的成员会由 SquadComponent 自动设为出战并被附身。
			SquadComponent->RegisterMember(Member);
		}
	}
}

AGGYGOCharacterBase* AGGYGOGameMode::SpawnSquadMember(APlayerController* OwningPlayer, const UGGYGOPawnData* PawnData, const FTransform& SpawnTransform)
{
	if (!PawnData || !PawnData->PawnClass)
	{
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = OwningPlayer;

	// AdjustIfPossibleButAlwaysSpawn：全员共用一个出生点，必然重叠。
	// 用默认策略会让第二、三个成员因碰撞而生成失败。
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AGGYGOCharacterBase* Member = World->SpawnActor<AGGYGOCharacterBase>(
		PawnData->PawnClass.Get(),
		SpawnTransform,
		SpawnParams);

	if (!Member)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SpawnSquadMember: 生成 [%s] 失败。"), *GetNameSafe(PawnData->PawnClass.Get()));
		return nullptr;
	}

	// 必须在生成后立刻注入 PawnData：InitState 的 DataAvailable 以它为前提，
	// 晚一步会让角色卡在 Spawned 直到下一次 CheckDefaultInitialization。
	if (UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Member))
	{
		PawnExtComp->SetPawnData(PawnData);
	}
	else
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SpawnSquadMember: [%s] 上没有 PawnExtensionComponent，PawnData 无法注入。"), *GetNameSafe(Member));
	}

	return Member;
}
