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
#include "Teams/GGYGOSquadPresets.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameMode)

namespace
{
	/**
	 * 用本地玩家存档里的出战编队填充名单。
	 *
	 * 服务器上的远程玩家取不到 LocalPlayer，直接返回 false —— 别人的编队存在
	 * 他自己的磁盘上，服务器读不到，只能由客户端上报（尚未实现）。
	 * 所以联机时远程玩家目前走默认编队，这不是错误路径。
	 *
	 * @return 是否填充成功。没有编队、出战编队为空、成员全都解析不出来都算失败。
	 */
	bool TryApplySavedRoster(const APlayerController* PlayerController, UGGYGOSquadComponent* SquadComponent)
	{
		UGGYGOSquadPresets* Presets = UGGYGOSquadPresets::GetForPlayerController(PlayerController);
		if (!Presets)
		{
			return false;
		}

		TArray<UGGYGOPawnData*> ResolvedRoster;
		if (Presets->ResolveActivePresetRoster(ResolvedRoster) == 0)
		{
			return false;
		}

		return SquadComponent->SetRoster(ResolvedRoster);
	}
}

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

		// 计数在发起前递增：回调可能同步触发（插件已加载过时），
		// 先增后调才能保证递减不会把计数打到负数。
		++PendingGameFeatureCount;

		Subsystem.LoadAndActivateGameFeaturePlugin(
			PluginURL,
			FGameFeaturePluginLoadComplete::CreateUObject(
				this, &ThisClass::OnGameFeatureActivated, PluginURL));
	}
}

void AGGYGOGameMode::OnGameFeatureActivated(const UE::GameFeatures::FResult& Result, FString PluginURL)
{
	if (Result.HasError())
	{
		// 只报错不阻断：让一个装不上的插件卡住所有玩家的进场，
		// 比缺这个插件的内容严重得多。
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("GameFeature [%s] 激活失败：%s。依赖它的内容将缺失。"),
			*PluginURL, *Result.GetError());
	}

	--PendingGameFeatureCount;

	if (AreGameFeaturesReady())
	{
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("GameFeature 全部激活完毕，放行等待中的玩家。"));

		SpawnSquadForPendingPlayers();
	}
}

void AGGYGOGameMode::SpawnSquadForPendingPlayers()
{
	// 遍历当前所有玩家而不是维护一份等待名单：等待期间玩家可能断线，
	// 名单里就会留下悬垂指针。迭代器给出的是此刻真实存在的 Controller。
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PlayerController = It->Get();
		if (!PlayerController)
		{
			continue;
		}

		// 已经有队伍的跳过，避免重复装配出两套位置。
		const AGGYGOPlayerState* GGYGOPlayerState = PlayerController->GetPlayerState<AGGYGOPlayerState>();
		const UGGYGOSquadComponent* SquadComponent =
			GGYGOPlayerState ? GGYGOPlayerState->GetSquadComponent() : nullptr;

		if (SquadComponent && SquadComponent->IsSquadAssembled())
		{
			continue;
		}

		SpawnSquadForPlayer(PlayerController);
	}
}

void AGGYGOGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// 有意不调用 Super：父类会走默认的"生成一个 Pawn 并附身"流程。
	if (!NewPlayer)
	{
		return;
	}

	// 插件还没激活完就先不生成。插件里的 Action 可能要往角色类上注入组件、
	// 授予能力，早生成的角色会缺这些内容且不报错。
	// 就绪回调里会通过 SpawnSquadForPendingPlayers 补上。
	if (!AreGameFeaturesReady())
	{
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("HandleStartingNewPlayer: 尚有 %d 个 GameFeature 未激活完毕，[%s] 的队伍延后装配。"),
			PendingGameFeatureCount, *GetNameSafe(NewPlayer));
		return;
	}

	SpawnSquadForPlayer(NewPlayer);
}

void AGGYGOGameMode::SpawnSquadForPlayer(APlayerController* NewPlayer)
{
	if (!Experience)
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

	// 名单来源按优先级三级回落：
	//
	// 1. 已经设好的名单 —— 有别的流程（将来的客户端上报、自动化测试）
	//    在装配前调过 SetRoster，那就以它为准，不要用存档覆盖。
	// 2. 本地玩家存档里的出战编队 —— 单机与主机端的正常路径。
	// 3. Experience 的默认编队 —— 新档、调试关卡、自动化测试的兜底，
	//    否则"没有编成界面"就等于空场景。
	const TCHAR* RosterSource = TEXT("Experience 默认编队");
	if (!SquadComponent->GetRoster().IsEmpty())
	{
		RosterSource = TEXT("玩家编队（已设置）");
	}
	else if (TryApplySavedRoster(NewPlayer, SquadComponent))
	{
		RosterSource = TEXT("玩家编队（本地存档）");
	}

	// 上面两级都没结果时 GetRoster() 仍为空，此时用默认编队。
	const TArray<TObjectPtr<const UGGYGOPawnData>>& SquadRoster =
		SquadComponent->GetRoster().IsEmpty() ? Experience->SquadMembers : SquadComponent->GetRoster();

	if (SquadRoster.IsEmpty())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SpawnSquadForPlayer: 既没有编队名单，Experience [%s] 也没有配默认编队，不会生成任何角色。"),
			*GetNameSafe(Experience));
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
	SpawnedSlots.Reserve(SquadRoster.Num());

	for (const TObjectPtr<const UGGYGOPawnData>& PawnData : SquadRoster)
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
		TEXT("SpawnSquadForPlayer: 装配完成，名单来源 [%s]，位置 %d 个，出战 [%s]。"),
		RosterSource,
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
