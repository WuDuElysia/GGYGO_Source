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
#include "Teams/GGYGOCharacterSlot.h"
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

	// 两阶段装配。
	//
	// 阶段一只建位置，不建实体：位置持有 ASC 与属性集，走完这一阶段
	// 全队的属性、冷却、组规则就都已就绪。阶段二生成的 Pawn 无论以什么顺序
	// 初始化，都不会遇到"属性集还没到"的情况 —— 这是 ASC 放在位置上的目的。
	TArray<AGGYGOCharacterSlot*> SpawnedSlots;
	SpawnedSlots.Reserve(Experience->SquadMembers.Num());

	for (const TObjectPtr<const UGGYGOPawnData>& PawnData : Experience->SquadMembers)
	{
		if (!PawnData)
		{
			continue;
		}

		if (AGGYGOCharacterSlot* Slot = SpawnSquadSlot(NewPlayer, PawnData))
		{
			SpawnedSlots.Add(Slot);
		}
	}

	// 阶段二：为每个位置生成实体并互相绑定。
	//
	// 登记放在这里而不是阶段一：`RegisterSlot` 会把第一个位置设为出战并附身它的 Pawn，
	// 那要求 Pawn 已经存在。
	for (AGGYGOCharacterSlot* Slot : SpawnedSlots)
	{
		const UGGYGOPawnData* PawnData = Slot->GetPawnData();
		if (!PawnData)
		{
			continue;
		}

		AGGYGOCharacterBase* Member = SpawnSquadMember(NewPlayer, PawnData, SpawnTransform);
		if (!Member)
		{
			continue;
		}

		// 把位置的 ASC 注入 Pawn 的协调者：Owner 是位置，Avatar 是 Pawn。
		if (UGGYGOPawnExtensionComponent* PawnExtComp = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Member))
		{
			PawnExtComp->InitializeAbilitySystem(Slot->GetGGYGOAbilitySystemComponent(), Slot);
		}

		// 反向绑定：让位置的 ASC 知道自己的 Avatar 是谁。
		Slot->SetAvatar(Member);

		// 第一个登记的位置会由 SquadComponent 自动设为出战并被附身。
		SquadComponent->RegisterSlot(Slot);
	}

	// 装配结果留一条记录：这条链路跨 GameMode、位置、Pawn、SquadComponent 四方，
	// 出问题时"到底装了几个位置、谁在出战"是第一个要回答的问题，
	// 没有它就只能靠断点或逐个 Actor 翻查。
	const AGGYGOCharacterBase* ActiveCharacter = SquadComponent->GetActiveCharacter();
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("SpawnSquadForPlayer: 装配完成，位置 %d 个，出战 [%s]。"),
		SquadComponent->GetSlotCount(), *GetNameSafe(ActiveCharacter));
}

AGGYGOCharacterSlot* AGGYGOGameMode::SpawnSquadSlot(APlayerController* OwningPlayer, const UGGYGOPawnData* PawnData)
{
	UWorld* World = GetWorld();
	if (!World || !PawnData)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;

	// Owner 必须是 PlayerController。
	//
	// GAS 的客户端预测靠 `ASC->GetOwnerActor()->GetNetOwningPlayer()` 找到玩家连接，
	// 而位置本身不是 Pawn 也不是 PlayerState，这条链只能靠 Owner 建立。
	// 设错的症状是 PredictionKey 生成不出来、所有 LocalPredicted 能力退化成
	// 纯服务器执行（输入延迟一个 RTT），而且**不会报任何错**。
	SpawnParams.Owner = OwningPlayer;

	// 位置没有空间存在感，出生变换取单位变换即可。
	AGGYGOCharacterSlot* Slot = World->SpawnActor<AGGYGOCharacterSlot>(
		AGGYGOCharacterSlot::StaticClass(),
		FTransform::Identity,
		SpawnParams);

	if (!Slot)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SpawnSquadSlot: 为 [%s] 生成队伍位置失败。"), *GetNameSafe(PawnData));
		return nullptr;
	}

	// 装载角色定义：注入组规则与 Tag 关系表，并授予该角色的 AbilitySet。
	// 这一步完成后本位置的属性与能力就已可用，与实体是否存在无关。
	Slot->InitializeForPawnData(PawnData);

	return Slot;
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
