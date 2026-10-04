/**
 * @file GGYGOGameMode.cpp
 * @brief GameMode 实现
 */
#include "GameModes/GGYGOGameMode.h"

#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Character/GGYGOCharacterBase.h"
#include "CoreGlobals.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerStart.h"
#include "GameModes/GGYGOExperienceDefinition.h"
#include "GameModes/GGYGOGameFeatureSession.h"
#include "Misc/ScopeExit.h"
#include "Player/GGYGOLocalPlayer.h"
#include "Player/GGYGOPlayerController.h"
#include "Player/GGYGOPlayerState.h"
#include "Teams/GGYGOCharacterSlot.h"
#include "Teams/GGYGOSquadComponent.h"
#include "Teams/GGYGOSquadPresets.h"
#include "Teams/GGYGOSquadTypes.h"
#include "Templates/Function.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameMode)

namespace
{
	enum class ESavedRosterApplyStatus : uint8
	{
		Applied,
		Unconfigured,
		Invalid
	};

	struct FSavedRosterApplyResult
	{
		ESavedRosterApplyStatus Status = ESavedRosterApplyStatus::Invalid;
		FString Error;
	};

	/** Apply only a complete saved roster; absence and failure remain distinct synchronous outcomes. */
	FSavedRosterApplyResult TryApplySavedRoster(
		const TWeakObjectPtr<APlayerController>& OriginalController,
		const TWeakObjectPtr<UGGYGOSquadComponent>& OriginalSquad,
		TFunctionRef<bool()> ContextIsCurrent)
	{
		const FString ControllerPath = GetPathNameSafe(OriginalController.Get());
		const FString SquadPath = GetPathNameSafe(OriginalSquad.Get());
		const auto Reject = [&ControllerPath, &SquadPath](FString Error)
		{
			FSavedRosterApplyResult Result;
			Result.Error = FString::Printf(TEXT("Controller=%s Squad=%s: %s"),
				*ControllerPath, *SquadPath, *Error);
			return Result;
		};
		if (!ContextIsCurrent() || !OriginalController.IsValid() || !OriginalSquad.IsValid())
		{
			return Reject(TEXT("Original creation qualification is invalid before local source lookup."));
		}

		const ULocalPlayer* const LocalPlayer = OriginalController->GetLocalPlayer();
		if (!ContextIsCurrent() || !OriginalController.IsValid()
			|| OriginalController->GetLocalPlayer() != LocalPlayer)
		{
			return Reject(TEXT("Original Controller/LocalPlayer qualification changed during source selection."));
		}
		if (!LocalPlayer)
		{
			// An explicitly absent LP is the existing normal server remote-player source mode.
			return { ESavedRosterApplyStatus::Unconfigured, FString() };
		}
		if (!IsValid(LocalPlayer) || LocalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return Reject(TEXT("A real LocalPlayer exists but is invalid or being destroyed."));
		}

		const TWeakObjectPtr<const ULocalPlayer> OriginalLocalPlayer(LocalPlayer);
		const FString LocalPlayerPath = LocalPlayer->GetPathName();
		const auto CheckOriginalSource = [OriginalController, OriginalSquad, OriginalLocalPlayer,
			ContextIsCurrent]() -> FString
		{
			if (!ContextIsCurrent() || !OriginalController.IsValid() || !OriginalSquad.IsValid())
			{
				return TEXT("Original World/GF session/Controller/PlayerState/Squad/Experience qualification was lost.");
			}
			const ULocalPlayer* const CurrentLocalPlayer = OriginalLocalPlayer.Get();
			if (!CurrentLocalPlayer || CurrentLocalPlayer->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
				|| OriginalController->GetLocalPlayer() != CurrentLocalPlayer)
			{
				return TEXT("Original LocalPlayer became unavailable or the Controller now has another source.");
			}
			return FString();
		};

		FString SourceError = CheckOriginalSource();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s before TryGetSquadPresets: %s"),
				*LocalPlayerPath, *SourceError));
		}
		const UGGYGOLocalPlayer* const PresetsHost = Cast<UGGYGOLocalPlayer>(OriginalLocalPlayer.Get());
		if (!PresetsHost)
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s is not GGYGOLocalPlayer; check DefaultEngine.ini LocalPlayerClassName."),
				*LocalPlayerPath));
		}
		UGGYGOSquadPresets* ReturnedPresets = nullptr;
		FString AcquisitionError;
		const bool bAcquiredPresets = PresetsHost->TryGetSquadPresets(ReturnedPresets, AcquisitionError);
		SourceError = CheckOriginalSource();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s after TryGetSquadPresets: %s; AcquisitionError=%s"),
				*LocalPlayerPath, *SourceError, *AcquisitionError));
		}
		if (!bAcquiredPresets)
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s: TryGetSquadPresets rejected the original source; AcquisitionError=%s"),
				*LocalPlayerPath, *AcquisitionError));
		}
		if (!IsValid(ReturnedPresets) || ReturnedPresets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s: TryGetSquadPresets returned success without valid presets; AcquisitionError=%s"),
				*LocalPlayerPath, *AcquisitionError));
		}

		const TWeakObjectPtr<UGGYGOSquadPresets> OriginalPresets(ReturnedPresets);
		const FString PresetsPath = ReturnedPresets->GetPathName();
		const auto CheckOriginalPresets = [&CheckOriginalSource, OriginalPresets]() -> FString
		{
			FString QualificationError = CheckOriginalSource();
			if (!QualificationError.IsEmpty())
			{
				return QualificationError;
			}
			const UGGYGOSquadPresets* const CurrentPresets = OriginalPresets.Get();
			if (!CurrentPresets || CurrentPresets->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return TEXT("The original getter-produced presets became unavailable or began destruction.");
			}
			return FString();
		};

		SourceError = CheckOriginalPresets();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s Presets=%s before ResolveActivePresetRoster: %s"),
				*LocalPlayerPath, *PresetsPath, *SourceError));
		}
		TArray<UGGYGOPawnData*> ResolvedRoster;
		const FGGYGOSquadPresetRosterResolveResult Resolution =
			OriginalPresets->ResolveActivePresetRoster(ResolvedRoster);
		const auto DescribeResolution = [&Resolution]()
		{
			return FString::Printf(TEXT("Presets=%s PresetIndex=%d MemberIndex=%d MemberId=%s MemberPath=%s ResolverError=%s"),
				*Resolution.PresetsPath, Resolution.PresetIndex, Resolution.MemberIndex,
				*Resolution.MemberId.ToString(), *Resolution.MemberPath.ToString(), *Resolution.Error);
		};
		SourceError = CheckOriginalPresets();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s after ResolveActivePresetRoster: %s; %s"),
				*LocalPlayerPath, *SourceError, *DescribeResolution()));
		}

		switch (Resolution.Status)
		{
		case EGGYGOSquadPresetRosterResolveStatus::Invalid:
			return Reject(FString::Printf(TEXT("Invalid preset resolution; %s"), *DescribeResolution()));
		case EGGYGOSquadPresetRosterResolveStatus::Unconfigured:
			if (!ResolvedRoster.IsEmpty())
			{
				return Reject(FString::Printf(TEXT("Unconfigured resolution returned a nonempty output; %s"),
					*DescribeResolution()));
			}
			return { ESavedRosterApplyStatus::Unconfigured, FString() };
		case EGGYGOSquadPresetRosterResolveStatus::Resolved:
			break;
		default:
			return Reject(FString::Printf(TEXT("Unexpected preset resolution status; %s"), *DescribeResolution()));
		}
		if (ResolvedRoster.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("Resolved preset returned no complete roster; %s"),
				*DescribeResolution()));
		}
		SourceError = CheckOriginalPresets();
		if (!SourceError.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("LocalPlayer=%s before SetRoster: %s; %s"),
				*LocalPlayerPath, *SourceError, *DescribeResolution()));
		}
		for (int32 OutputIndex = 0; OutputIndex < ResolvedRoster.Num(); ++OutputIndex)
		{
			UGGYGOPawnData* const PawnData = ResolvedRoster[OutputIndex];
			if (!IsValid(PawnData) || PawnData->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return Reject(FString::Printf(TEXT("Resolved output index %d became invalid before SetRoster; %s"),
					OutputIndex, *DescribeResolution()));
			}
		}
		const bool bAccepted = OriginalSquad->SetRoster(ResolvedRoster);
		SourceError = CheckOriginalPresets();
		if (!bAccepted)
		{
			return Reject(FString::Printf(TEXT("SetRoster rejected the complete saved roster. Qualification=%s; %s"),
				*SourceError, *DescribeResolution()));
		}
		if (!SourceError.IsEmpty())
		{
			// SetRoster's actual commit remains Squad-owned; this request cannot undo it or create Actors.
			return Reject(FString::Printf(TEXT("SetRoster accepted, then original qualification was lost: %s; %s"),
				*SourceError, *DescribeResolution()));
		}
		return { ESavedRosterApplyStatus::Applied, FString() };
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
	if (!IsInGameThread())
	{
		ErrorMessage = TEXT("[GameFeature] GameMode InitGame requires the game thread.");
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *ErrorMessage);
		return;
	}
	if (bGameFeatureStartupAttempted || bGameFeatureCallerClosed)
	{
		ErrorMessage = FString::Printf(
			TEXT("[GameFeature] GameMode [%s] World [%s] Experience [%s]: startup already attempted or caller closed; no replacement/retry."),
			*GetPathNameSafe(this), *GetPathNameSafe(GetWorld()), *GetPathNameSafe(Experience));
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *ErrorMessage);
		return;
	}
	bGameFeatureStartupAttempted = true;
	const TWeakObjectPtr<AGGYGOGameMode> WeakGameMode(this);
	Super::InitGame(MapName, Options, ErrorMessage);
	if (WeakGameMode.Get() != this)
	{
		ErrorMessage = TEXT("[GameFeature] Original GameMode became invalid during engine InitGame; startup refused.");
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *ErrorMessage);
		return;
	}
	InitializeGameFeatureSession(ErrorMessage);
}

void AGGYGOGameMode::InitializeGameFeatureSession(FString& ErrorMessage)
{
	const TWeakObjectPtr<AGGYGOGameMode> WeakGameMode(this);
	const TWeakObjectPtr<UWorld> WeakWorld(GetWorld());
	UWorld* World = WeakWorld.Get();
	const FString OwnerLabel = FString::Printf(TEXT("GameMode [%s] Experience [%s]"),
		*GetPathNameSafe(this), *GetPathNameSafe(Experience));
	// Synchronous local helper only. This reference never escapes into the Session receiver.
	const auto Reject = [this, &ErrorMessage, &OwnerLabel](const FString& Reason)
	{
		bGameFeatureCallerClosed = true;
		ErrorMessage = FString::Printf(TEXT("[GameFeature] %s World [%s]: %s"),
			*OwnerLabel, *GetPathNameSafe(GetWorld()), *Reason);
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), *ErrorMessage);
	};
	if (!ErrorMessage.IsEmpty())
	{
		Reject(FString::Printf(TEXT("Engine InitGame failed: %s"), *ErrorMessage));
		return;
	}
	if (!World || !IsGameFeatureCallerContextCurrent(*World, FGameFeatureSessionPtr()))
	{
		Reject(TEXT("Original caller/World admission is closed or no longer authoritative."));
		return;
	}
	if (!IsValid(Experience))
	{
		Reject(TEXT("Required Experience is missing or invalid."));
		return;
	}

	FGGYGOGameFeatureClosureResolver::FInput Input;
	TArray<FString> Errors;
	if (!Experience->TryBuildGameFeatureInput(Input, Errors))
	{
		Reject(FString::Printf(TEXT("Experience configuration rejected: %s"), *FString::Join(Errors, TEXT("; "))));
		return;
	}
	// Both empty is an explicit successful configuration mode, never a failed-input fallback.
	if (Input.RootPluginNames.IsEmpty() && Input.DeclaredSources.IsEmpty())
	{
		bConfiguredNoGameFeatures = true;
		SpawnSquadForPendingPlayers();
		return;
	}

	FString CreateError;
	const FGameFeatureSessionPtr OriginalSession =
		FGGYGOGameFeatureSession::TryCreate(*World, OwnerLabel, CreateError);
	if (!OriginalSession.IsValid())
	{
		Reject(FString::Printf(TEXT("Original Session creation failed: %s"), *CreateError));
		return;
	}
	// Publish the fully constructed resource before Start: completion may be synchronous.
	GameFeatureSession = OriginalSession;
	const TWeakPtr<FGGYGOGameFeatureSession, ESPMode::ThreadSafe> WeakSession(OriginalSession);
	// Start is void. A shared diagnostic relays an immediate failure without retaining a stack reference.
	const TSharedRef<FString, ESPMode::ThreadSafe> StartupError = MakeShared<FString, ESPMode::ThreadSafe>();
	OriginalSession->Start(MoveTemp(Input),
		[WeakGameMode, WeakWorld, WeakSession, StartupError](const FGGYGOGameFeatureSession::FStartResult& Result)
	{
		const FGameFeatureSessionPtr Session = WeakSession.Pin();
		AGGYGOGameMode* GameMode = WeakGameMode.Get();
		UWorld* OriginalWorld = WeakWorld.Get();
		if (!GameMode || !OriginalWorld || !Session.IsValid()
			|| !GameMode->IsGameFeatureCallerContextCurrent(*OriginalWorld, Session))
		{
			return;
		}
		if (Result.Status != FGGYGOGameFeatureSession::EStartStatus::Ready)
		{
			*StartupError = FString::Printf(
				TEXT("[GameFeature] GameMode [%s] World [%s] Experience [%s]: Session startup failed (status %u): %s"),
				*GetPathNameSafe(GameMode), *GetPathNameSafe(OriginalWorld), *GetPathNameSafe(GameMode->Experience),
				static_cast<uint32>(Result.Status), *FString::Join(Result.Errors, TEXT("; ")));
			GameMode->bGameFeatureCallerClosed = true;
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), **StartupError);
			// Session retains its own failed operation and drains its native resource automatically.
			return;
		}
		if (!GameMode->CanAssembleForGameFeatureContext(*OriginalWorld, Session))
		{
			*StartupError = FString::Printf(
				TEXT("[GameFeature] GameMode [%s] World [%s] Experience [%s]: Ready notification no longer grants current original Session qualification."),
				*GetPathNameSafe(GameMode), *GetPathNameSafe(OriginalWorld), *GetPathNameSafe(GameMode->Experience));
			GameMode->bGameFeatureCallerClosed = true;
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("%s"), **StartupError);
			Session->Close();
			return;
		}
		GameMode->SpawnSquadForPendingPlayers();
		GameMode = WeakGameMode.Get();
		OriginalWorld = WeakWorld.Get();
		if (!GameMode || !OriginalWorld
			|| !GameMode->CanAssembleForGameFeatureContext(*OriginalWorld, Session))
		{
			return;
		}
	});
	if (!StartupError->IsEmpty())
	{
		ErrorMessage = *StartupError;
	}
}

bool AGGYGOGameMode::IsGameFeatureCallerContextCurrent(
	const UWorld& ExpectedWorld, const FGameFeatureSessionPtr& ExpectedSession) const
{
	if (!IsInGameThread() || !IsValid(this) || IsActorBeingDestroyed()
		|| !bGameFeatureStartupAttempted || bGameFeatureCallerClosed
		|| !IsValid(&ExpectedWorld) || ExpectedWorld.bIsTearingDown || ExpectedWorld.IsBeingCleanedUp()
		|| (ExpectedWorld.WorldType != EWorldType::Game && ExpectedWorld.WorldType != EWorldType::PIE)
		|| GetWorld() != &ExpectedWorld || ExpectedWorld.GetAuthGameMode() != this
		|| GameFeatureSession != ExpectedSession)
	{
		return false;
	}
	const UGameInstance* GI = ExpectedWorld.GetGameInstance();
	return IsValid(GI) && GI->GetWorld() == &ExpectedWorld;
}

bool AGGYGOGameMode::AreGameFeaturesReady() const
{
	if (!IsInGameThread())
	{
		return false;
	}
	const UWorld* World = GetWorld();
	const FGameFeatureSessionPtr Session = GameFeatureSession;
	if (!World || !IsGameFeatureCallerContextCurrent(*World, Session))
	{
		return false;
	}
	return bConfiguredNoGameFeatures ? !Session.IsValid()
		: Session.IsValid() && Session->IsReadyFor(*World);
}

bool AGGYGOGameMode::CanAssembleForGameFeatureContext(
	const UWorld& ExpectedWorld, const FGameFeatureSessionPtr& ExpectedSession) const
{
	return IsGameFeatureCallerContextCurrent(ExpectedWorld, ExpectedSession) && AreGameFeaturesReady();
}

void AGGYGOGameMode::SpawnSquadForPendingPlayers()
{
	if (!IsInGameThread())
	{
		return;
	}
	const TWeakObjectPtr<AGGYGOGameMode> WeakGameMode(this);
	const TWeakObjectPtr<UWorld> WeakWorld(GetWorld());
	const FGameFeatureSessionPtr OriginalSession = GameFeatureSession;
	AGGYGOGameMode* GameMode = WeakGameMode.Get();
	UWorld* World = WeakWorld.Get();
	if (!GameMode || !World || !GameMode->CanAssembleForGameFeatureContext(*World, OriginalSession))
	{
		return;
	}

	// Current Controllers only. The snapshot is weak and local to this one dispatch, not a waiting roster.
	TArray<TWeakObjectPtr<APlayerController>> Controllers;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		Controllers.Add(*It);
	}
	for (const TWeakObjectPtr<APlayerController>& WeakController : Controllers)
	{
		GameMode = WeakGameMode.Get();
		World = WeakWorld.Get();
		if (!GameMode || !World || !GameMode->CanAssembleForGameFeatureContext(*World, OriginalSession))
		{
			return;
		}
		APlayerController* PlayerController = WeakController.Get();
		if (!PlayerController || PlayerController->IsActorBeingDestroyed() || PlayerController->GetWorld() != World)
		{
			continue;
		}
		const AGGYGOPlayerState* PlayerState = PlayerController->GetPlayerState<AGGYGOPlayerState>();
		const UGGYGOSquadComponent* SquadComponent = PlayerState ? PlayerState->GetSquadComponent() : nullptr;
		if (SquadComponent && SquadComponent->IsSquadAssembled())
		{
			continue;
		}
		GameMode->SpawnSquadForPlayer(PlayerController);
		GameMode = WeakGameMode.Get();
		World = WeakWorld.Get();
		if (!GameMode || !World || !GameMode->CanAssembleForGameFeatureContext(*World, OriginalSession))
		{
			return;
		}
	}
}

void AGGYGOGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// Deliberately no Super: it would run the engine's single-Pawn spawn path.
	if (!IsInGameThread())
	{
		return;
	}
	const TWeakObjectPtr<AGGYGOGameMode> WeakGameMode(this);
	const TWeakObjectPtr<UWorld> WeakWorld(GetWorld());
	const TWeakObjectPtr<APlayerController> WeakPlayer(NewPlayer);
	const FGameFeatureSessionPtr OriginalSession = GameFeatureSession;
	AGGYGOGameMode* GameMode = WeakGameMode.Get();
	UWorld* World = WeakWorld.Get();
	APlayerController* Player = WeakPlayer.Get();
	if (!GameMode || !World || !Player || Player->IsActorBeingDestroyed() || Player->GetWorld() != World
		|| !GameMode->CanAssembleForGameFeatureContext(*World, OriginalSession))
	{
		return;
	}
	GameMode->SpawnSquadForPlayer(Player);
	GameMode = WeakGameMode.Get();
	World = WeakWorld.Get();
	if (!GameMode || !World || !GameMode->CanAssembleForGameFeatureContext(*World, OriginalSession))
	{
		return;
	}
}

void AGGYGOGameMode::CloseGameFeatureSession()
{
	// Both engine lifecycle hooks guard GT. Retire admission before any Close reentry.
	bGameFeatureCallerClosed = true;
	bConfiguredNoGameFeatures = false;
	FGameFeatureSessionPtr OriginalSession = MoveTemp(GameFeatureSession);
	if (OriginalSession.IsValid())
	{
		OriginalSession->Close();
	}
	// No member reset after Close: a reentrant hook sees the original resource already moved.
}

void AGGYGOGameMode::Destroyed()
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[GameFeature] GameMode Destroyed requires GT; original caller/resource remains untouched."));
		return;
	}
	// Actor::Destroyed can run before BeginPlay, when RouteEndPlay does not invoke EndPlay.
	const TWeakObjectPtr<AGGYGOGameMode> OriginalCreator(this);
	CloseGameFeatureSession();
	if (AGGYGOGameMode* Creator = OriginalCreator.Get())
	{
		Creator->ConsumeUntransferredSquadActors(true);
	}
	Super::Destroyed();
}

void AGGYGOGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[GameFeature] GameMode EndPlay requires GT; original caller/resource remains untouched."));
		return;
	}
	const TWeakObjectPtr<AGGYGOGameMode> OriginalCreator(this);
	CloseGameFeatureSession();
	if (AGGYGOGameMode* Creator = OriginalCreator.Get())
	{
		Creator->ConsumeUntransferredSquadActors(true);
	}
	// Super::Destroyed may route here; the shared consumer cannot release the moved resource twice.
	Super::EndPlay(EndPlayReason);
}

bool AGGYGOGameMode::IsSquadCreationContextCurrent(const FSquadCreationContext& Context) const
{
	if (!IsInGameThread())
	{
		return false;
	}
	const UWorld* World = Context.World.Get();
	const APlayerController* Controller = Context.Controller.Get();
	const AGGYGOPlayerState* PlayerState = Context.PlayerState.Get();
	const UGGYGOSquadComponent* Squad = Context.Squad.Get();
	const UGGYGOExperienceDefinition* OriginalExperience = Context.Experience.Get();
	return Context.GameMode.Get() == this && World && Controller && PlayerState && Squad && OriginalExperience
		&& HasAuthority() && CanAssembleForGameFeatureContext(*World, Context.Session)
		&& !Controller->IsActorBeingDestroyed() && Controller->HasAuthority() && Controller->GetWorld() == World
		&& !PlayerState->IsActorBeingDestroyed() && PlayerState->HasAuthority() && PlayerState->GetWorld() == World
		&& Controller->GetPlayerState<AGGYGOPlayerState>() == PlayerState && PlayerState->GetOwner() == Controller
		&& !Squad->IsBeingDestroyed() && Squad->GetWorld() == World && Squad->GetOwner() == PlayerState
		&& PlayerState->GetSquadComponent() == Squad && Squad->GetRegistrationController() == Controller
		&& GetExperience() == OriginalExperience;
}

void AGGYGOGameMode::RequestUntransferredSquadActorDestruction(
	TArray<FUntransferredSquadActors>& OriginalActors, const FString& CreatorPath)
{
	check(IsInGameThread());
	// No creator/Controller/world admission is needed to discharge an original creation obligation.
	const auto RequestDestroy = [&CreatorPath](TWeakObjectPtr<AActor>& OriginalActor,
		const FUntransferredSquadActors& Origin, const TCHAR* Kind)
	{
		AActor* Actor = OriginalActor.Get();
		if (!Actor || Actor->IsActorBeingDestroyed())
		{
			OriginalActor.Reset();
			return;
		}
		const FString ActorPath = Actor->GetPathName();
		if (!Actor->HasAuthority() || !Actor->GetWorld())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Resource=%s Actor=%s: original destruction denied (Authority/World); obligation retained."),
				*CreatorPath, *Origin.ControllerPath, *Origin.PawnDataPath, Kind, *ActorPath);
			return;
		}
		const bool bNativeAccepted = Actor->Destroy();
		Actor = OriginalActor.Get();
		if (bNativeAccepted || !Actor || Actor->IsActorBeingDestroyed())
		{
			OriginalActor.Reset(); // Request obligation only; native/Host physical cleanup is not asserted.
			return;
		}
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Resource=%s Actor=%s: native Destroy rejected a live original; obligation retained."),
			*CreatorPath, *Origin.ControllerPath, *Origin.PawnDataPath, Kind, *ActorPath);
	};
	for (FUntransferredSquadActors& Origin : OriginalActors)
	{
		RequestDestroy(Origin.OriginalSlot, Origin, TEXT("OriginalSlot"));
		RequestDestroy(Origin.OriginalPawn, Origin, TEXT("OriginalPawn"));
	}
	// Another original's callback may already have started destruction. Never issue a second request for it.
	for (FUntransferredSquadActors& Origin : OriginalActors)
	{
		if (AActor* Slot = Origin.OriginalSlot.Get(); !Slot || Slot->IsActorBeingDestroyed())
		{
			Origin.OriginalSlot.Reset();
		}
		if (AActor* Pawn = Origin.OriginalPawn.Get(); !Pawn || Pawn->IsActorBeingDestroyed())
		{
			Origin.OriginalPawn.Reset();
		}
	}
	OriginalActors.RemoveAll([](const FUntransferredSquadActors& Origin)
	{
		return Origin.OriginalSlot.IsExplicitlyNull() && Origin.OriginalPawn.IsExplicitlyNull();
	});
}

bool AGGYGOGameMode::FinishUntransferredSquadActors(TArray<FUntransferredSquadActors>& OriginalActors,
	const TWeakObjectPtr<AGGYGOGameMode>& OriginalCreator, const FString& CreatorPath, bool bFinalDestruction)
{
	RequestUntransferredSquadActorDestruction(OriginalActors, CreatorPath);
	const bool bRequestsRetired = OriginalActors.IsEmpty();
	AGGYGOGameMode* Creator = OriginalCreator.Get();
	const bool bFinal = bFinalDestruction || !Creator || Creator->IsActorBeingDestroyed()
		|| Creator->bGameFeatureCallerClosed || Creator->bFinalSquadCreatorDestructionRequested;
	if (bFinal)
	{
		for (const FUntransferredSquadActors& Origin : OriginalActors)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams] Creator=%s Controller=%s PawnData=%s: final creator close still has untransferred originals Slot=%s Pawn=%s; physical cleanup incomplete%s."),
				*CreatorPath, *Origin.ControllerPath, *Origin.PawnDataPath,
				*GetPathNameSafe(Origin.OriginalSlot.Get()), *GetPathNameSafe(Origin.OriginalPawn.Get()),
				Creator ? TEXT(", original obligation retained") : TEXT(", creator no longer available to retain obligation"));
		}
	}
	if (Creator)
	{
		for (FUntransferredSquadActors& Origin : OriginalActors)
		{
			Creator->UntransferredSquadActors.Add(MoveTemp(Origin));
		}
	}
	OriginalActors.Reset();
	return bRequestsRetired; // Request obligations only, not Host/native physical cleanup success.
}

void AGGYGOGameMode::ConsumeUntransferredSquadActors(bool bFinalDestruction)
{
	check(IsInGameThread());
	bFinalSquadCreatorDestructionRequested |= bFinalDestruction;
	if (bConsumingUntransferredSquadActors)
	{
		return;
	}
	bConsumingUntransferredSquadActors = true;
	const TWeakObjectPtr<AGGYGOGameMode> OriginalCreator(this);
	const FString CreatorPath = GetPathName();
	TArray<FUntransferredSquadActors> OriginalActors = MoveTemp(UntransferredSquadActors);
	UntransferredSquadActors.Reset();
	FinishUntransferredSquadActors(OriginalActors, OriginalCreator, CreatorPath,
		bFinalSquadCreatorDestructionRequested);
	if (AGGYGOGameMode* Creator = OriginalCreator.Get())
	{
		Creator->bConsumingUntransferredSquadActors = false;
	}
}

void AGGYGOGameMode::SpawnSquadForPlayer(APlayerController* NewPlayer)
{
	if (!IsInGameThread() || !IsValid(this) || IsActorBeingDestroyed() || !IsValid(NewPlayer))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] SpawnSquadForPlayer Creator=%s Controller=%s: invalid synchronous creation caller."),
			*GetPathNameSafe(this), *GetPathNameSafe(NewPlayer));
		return;
	}
	FSquadCreationContext Context;
	Context.GameMode = this;
	Context.World = GetWorld();
	Context.Controller = NewPlayer;
	Context.PlayerState = NewPlayer->GetPlayerState<AGGYGOPlayerState>();
	Context.Squad = Context.PlayerState.IsValid() ? Context.PlayerState->GetSquadComponent() : nullptr;
	Context.Experience = Experience.Get();
	Context.Session = GameFeatureSession;
	Context.CreatorPath = GetPathName();
	const FString ControllerPath = NewPlayer->GetPathName();
	const auto ContextIsCurrent = [&Context]()
	{
		const AGGYGOGameMode* Creator = Context.GameMode.Get();
		return Creator && Creator->IsSquadCreationContextCurrent(Context);
	};
	const auto ReportClosedContext = [&Context, &ControllerPath](const TCHAR* Stage)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s Experience=%s Stage=%s: original World/GF session/PlayerState/Squad/Experience qualification lost; no new work."),
			*Context.CreatorPath, *ControllerPath, *GetPathNameSafe(Context.Experience.Get()), Stage);
	};
	if (!ContextIsCurrent())
	{
		ReportClosedContext(TEXT("Entry"));
		return;
	}
	if (ControllersCreatingSquads.ContainsByPredicate([&Context](const TWeakObjectPtr<APlayerController>& Controller)
		{ return Controller.HasSameIndexAndSerialNumber(Context.Controller); }))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s: synchronous squad creation already in flight; reentry rejected."),
			*Context.CreatorPath, *ControllerPath);
		return;
	}
	for (const FUntransferredSquadActors& Origin : UntransferredSquadActors)
	{
		const AActor* Slot = Origin.OriginalSlot.Get();
		const AActor* Pawn = Origin.OriginalPawn.Get();
		if (Origin.Controller.HasSameIndexAndSerialNumber(Context.Controller)
			&& ((Slot && !Slot->IsActorBeingDestroyed()) || (Pawn && !Pawn->IsActorBeingDestroyed())))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams] Creator=%s Controller=%s PawnData=%s: live rejected untransferred resources remain; new creation refused (no automatic retry)."),
				*Context.CreatorPath, *ControllerPath, *Origin.PawnDataPath);
			return;
		}
	}
	if (Context.Squad->IsSquadAssembled())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Teams] Creator=%s Controller=%s: existing Slots already assembled; no borrowed member upgraded or new resource created."),
			*Context.CreatorPath, *ControllerPath);
		return;
	}

	ControllersCreatingSquads.Add(Context.Controller);
	TArray<FUntransferredSquadActors> CreatorActors;
	ON_SCOPE_EXIT
	{
		// In-flight Spawn/handoff resources live only here, never in the lifecycle consumer's collection.
		FinishUntransferredSquadActors(CreatorActors, Context.GameMode, Context.CreatorPath, false);
		if (AGGYGOGameMode* Creator = Context.GameMode.Get())
		{
			Creator->ControllersCreatingSquads.RemoveAll([&Context](const TWeakObjectPtr<APlayerController>& Controller)
			{
				return Controller.HasSameIndexAndSerialNumber(Context.Controller);
			});
		}
	};
	const auto ReleaseUntransferredPair = [&Context](FUntransferredSquadActors& Origin)
	{
		TArray<FUntransferredSquadActors> OriginalActors;
		OriginalActors.Add(MoveTemp(Origin));
		Origin = FUntransferredSquadActors();
		return FinishUntransferredSquadActors(OriginalActors, Context.GameMode, Context.CreatorPath, false);
	};
	UGGYGOSquadComponent* SquadComponent = Context.Squad.Get();
	const UGGYGOExperienceDefinition* OriginalExperience = Context.Experience.Get();

	// Only an explicit normal Unconfigured result selects Experience; errors never select another source.
	bool bUseExperienceDefault = false;
	const TCHAR* RosterSource = TEXT("玩家编队（已设置）");
	if (!SquadComponent->GetRoster().IsEmpty())
	{
		RosterSource = TEXT("玩家编队（已设置）");
	}
	else
	{
		const FSavedRosterApplyResult SavedRoster =
			TryApplySavedRoster(Context.Controller, Context.Squad, ContextIsCurrent);
		switch (SavedRoster.Status)
		{
		case ESavedRosterApplyStatus::Applied:
			RosterSource = TEXT("玩家编队（本地存档）");
			break;
		case ESavedRosterApplyStatus::Unconfigured:
			bUseExperienceDefault = true;
			RosterSource = TEXT("Experience 默认编队");
			break;
		case ESavedRosterApplyStatus::Invalid:
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams.RosterSource] Creator=%s Controller=%s: %s; this request stopped before Actor generation; no default source selected."),
				*Context.CreatorPath, *ControllerPath, *SavedRoster.Error);
			return;
		default:
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams.RosterSource] Creator=%s Controller=%s: unexpected saved-roster apply status; no new work."),
				*Context.CreatorPath, *ControllerPath);
			return;
		}
	}
	if (!ContextIsCurrent())
	{
		ReportClosedContext(TEXT("RosterResolution"));
		return;
	}
	SquadComponent = Context.Squad.Get();
	OriginalExperience = Context.Experience.Get();
	// Copy weak input identities before any subsequent external call; this is not another member roster.
	TArray<TWeakObjectPtr<const UGGYGOPawnData>> SelectedPawnData;
	{
		const TArray<TObjectPtr<const UGGYGOPawnData>>& SquadRoster =
			bUseExperienceDefault ? OriginalExperience->SquadMembers : SquadComponent->GetRoster();
		const int32 OriginalRosterCount = SquadRoster.Num();
		if (!GGYGOSquad::IsMemberCountWithinCapacity(OriginalRosterCount))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams.Capacity] Creator=%s Controller=%s Experience=%s RosterSource=%s OriginalCount=%d Limit=%d: selected raw roster exceeds capacity; request stopped before Actor creation."),
				*Context.CreatorPath, *ControllerPath, *GetPathNameSafe(OriginalExperience),
				RosterSource, OriginalRosterCount, GGYGO_MAX_SQUAD_SIZE);
			return;
		}
		SelectedPawnData.Reserve(SquadRoster.Num());
		for (const TObjectPtr<const UGGYGOPawnData>& PawnData : SquadRoster)
		{
			SelectedPawnData.Add(PawnData.Get());
		}
	}

	if (SelectedPawnData.IsEmpty())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s Experience=%s: selected roster empty; no Actors created."),
			*Context.CreatorPath, *ControllerPath, *GetPathNameSafe(OriginalExperience));
		return;
	}

	const AActor* StartSpot = Context.GameMode->ChoosePlayerStart(Context.Controller.Get());
	if (!ContextIsCurrent())
	{
		ReportClosedContext(TEXT("ChoosePlayerStart"));
		return;
	}
	if (StartSpot && (!IsValid(StartSpot) || StartSpot->IsActorBeingDestroyed() || StartSpot->GetWorld() != Context.World.Get()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s: ChoosePlayerStart returned an invalid/different-World Actor; creation stopped."),
			*Context.CreatorPath, *ControllerPath);
		return;
	}
	// The existing null StartSpot -> Identity policy remains for its separately scheduled correction.
	const FTransform SpawnTransform = StartSpot
		? StartSpot->GetActorTransform()
		: FTransform::Identity;

	CreatorActors.Reserve(SelectedPawnData.Num());
	int32 FailedItems = 0;
	int32 AcceptedPairs = 0;
	// Phase one remains all Slots before any Pawn.
	for (const TWeakObjectPtr<const UGGYGOPawnData>& SelectedData : SelectedPawnData)
	{
		if (!ContextIsCurrent())
		{
			ReportClosedContext(TEXT("BeforeSlotSpawn"));
			return;
		}
		const UGGYGOPawnData* PawnData = SelectedData.Get();
		FUntransferredSquadActors Origin;
		Origin.Controller = Context.Controller;
		Origin.PawnData = SelectedData;
		Origin.ControllerPath = ControllerPath;
		Origin.PawnDataPath = GetPathNameSafe(PawnData);
		AGGYGOGameMode* Creator = Context.GameMode.Get();
		if (!Creator->SpawnSquadSlot(Context, PawnData, Origin))
		{
			++FailedItems;
			if (!ReleaseUntransferredPair(Origin))
			{
				return; // Stop this Controller's current request as well as future requests after native refusal.
			}
			if (!ContextIsCurrent())
			{
				ReportClosedContext(TEXT("SlotPreparation"));
				return;
			}
			continue;
		}
		CreatorActors.Add(MoveTemp(Origin));
	}
	// Phase two prepares each Pawn, requests Host binding, then makes one explicit creation handoff.
	for (int32 Index = 0; Index < CreatorActors.Num(); ++Index)
	{
		if (!ContextIsCurrent())
		{
			ReportClosedContext(TEXT("BeforePawnSpawn"));
			return;
		}
		FUntransferredSquadActors& Origin = CreatorActors[Index];
		const UGGYGOPawnData* PawnData = Origin.PawnData.Get();
		AGGYGOCharacterSlot* Slot = Cast<AGGYGOCharacterSlot>(Origin.OriginalSlot.Get());
		if (!Slot || Slot->IsActorBeingDestroyed() || !Slot->HasAuthority()
			|| Slot->GetWorld() != Context.World.Get() || Slot->GetOwner() != Context.Controller.Get()
			|| !PawnData || Slot->GetPawnData() != PawnData || !Slot->IsPawnDataInitializationComplete())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams] Creator=%s Controller=%s PawnData=%s: original prepared Slot no longer usable; original pair released."),
				*Context.CreatorPath, *ControllerPath, *Origin.PawnDataPath);
			++FailedItems;
			if (!ReleaseUntransferredPair(Origin))
			{
				return; // Stop this Controller's current request as well as future requests after native refusal.
			}
			continue;
		}
		AGGYGOGameMode* Creator = Context.GameMode.Get();
		if (!Creator->SpawnSquadMember(Context, PawnData, SpawnTransform, Origin))
		{
			++FailedItems;
			if (!ReleaseUntransferredPair(Origin))
			{
				return; // Stop this Controller's current request as well as future requests after native refusal.
			}
			if (!ContextIsCurrent())
			{
				ReportClosedContext(TEXT("PawnPreparation"));
				return;
			}
			continue;
		}
		if (!ContextIsCurrent())
		{
			ReportClosedContext(TEXT("BeforeHostAttach"));
			return;
		}
		Slot = Cast<AGGYGOCharacterSlot>(Origin.OriginalSlot.Get());
		AGGYGOCharacterBase* Member = Cast<AGGYGOCharacterBase>(Origin.OriginalPawn.Get());
		if (!Slot || Slot->IsActorBeingDestroyed() || !Slot->HasAuthority()
			|| Slot->GetWorld() != Context.World.Get() || Slot->GetOwner() != Context.Controller.Get()
			|| !Origin.PawnData.IsValid() || Slot->GetPawnData() != Origin.PawnData.Get()
			|| !Slot->IsPawnDataInitializationComplete() || !Member || Member->IsActorBeingDestroyed())
		{
			++FailedItems;
			if (!ReleaseUntransferredPair(Origin))
			{
				return; // Stop this Controller's current request as well as future requests after native refusal.
			}
			continue;
		}
		Slot->AttachAvatar(Member); // Host alone coordinates ActorInfo; Squad alone validates new binding admission.
		if (!ContextIsCurrent())
		{
			ReportClosedContext(TEXT("HostAttach"));
			return;
		}
		Slot = Cast<AGGYGOCharacterSlot>(Origin.OriginalSlot.Get());
		Member = Cast<AGGYGOCharacterBase>(Origin.OriginalPawn.Get());
		if (!Slot || Slot->IsActorBeingDestroyed() || !Member || Member->IsActorBeingDestroyed())
		{
			++FailedItems;
			if (!ReleaseUntransferredPair(Origin))
			{
				return; // Stop this Controller's current request as well as future requests after native refusal.
			}
			continue;
		}

		// Remove this pair from creator cleanup BEFORE the potentially reentrant acceptance call.
		FUntransferredSquadActors Handoff = MoveTemp(Origin);
		Origin = FUntransferredSquadActors();
		const bool bAccepted = Context.Squad->RegisterCreatedSlot(Slot, Member);
		// The bool is the historical handoff fact. Do not inspect current admission/Avatar/Slots first.
		if (bAccepted)
		{
			++AcceptedPairs;
			Handoff = FUntransferredSquadActors(); // Only Squad can now destroy this explicitly accepted original pair.
		}
		else
		{
			++FailedItems;
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Teams] Creator=%s Controller=%s PawnData=%s: RegisterCreatedSlot rejected original pair; creator retains cleanup."),
				*Context.CreatorPath, *ControllerPath, *Handoff.PawnDataPath);
			if (!ReleaseUntransferredPair(Handoff))
			{
				return;
			}
		}
		if (!ContextIsCurrent())
		{
			ReportClosedContext(TEXT("AfterHandoff"));
			return;
		}
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Teams] Creator=%s Controller=%s: creation dispatch ended, roster source=%s selected=%d accepted original pairs=%d failed items=%d; not a whole-squad/possession success assertion."),
		*Context.CreatorPath, *ControllerPath, RosterSource, SelectedPawnData.Num(), AcceptedPairs, FailedItems);
}

bool AGGYGOGameMode::SpawnSquadSlot(const FSquadCreationContext& Context, const UGGYGOPawnData* PawnData,
	FUntransferredSquadActors& OutActors)
{
	const TWeakObjectPtr<const UGGYGOPawnData> OriginalData(PawnData);
	if (!IsSquadCreationContextCurrent(Context) || !OriginalData.IsValid() || !OutActors.OriginalSlot.IsExplicitlyNull())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=SpawnSlot: invalid original context/configuration or occupied output; no spawn."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath);
		return false;
	}
	UWorld* World = Context.World.Get();
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Context.Controller.Get();
	SpawnParams.CustomPreSpawnInitialization = [&OutActors](AActor* OriginalActor)
	{
		OutActors.OriginalSlot = OriginalActor; // Before pre-spawn/global/project callbacks, no external work here.
	};
	AGGYGOCharacterSlot* SpawnedSlot = World->SpawnActor<AGGYGOCharacterSlot>(
		AGGYGOCharacterSlot::StaticClass(), FTransform::Identity, SpawnParams);
	AGGYGOGameMode* Creator = Context.GameMode.Get();
	AGGYGOCharacterSlot* Slot = Cast<AGGYGOCharacterSlot>(OutActors.OriginalSlot.Get());
	if (!Creator || !Creator->IsSquadCreationContextCurrent(Context) || !SpawnedSlot || SpawnedSlot != Slot
		|| !Slot || Slot->IsActorBeingDestroyed() || !Slot->HasAuthority()
		|| Slot->GetWorld() != Context.World.Get() || Slot->GetOwner() != Context.Controller.Get()
		|| !OriginalData.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=SpawnSlotReturn: original Spawn/context rejected; captured Slot=%s remains creator-owned."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath,
			*GetPathNameSafe(OutActors.OriginalSlot.Get()));
		return false;
	}
	Slot->InitializeForPawnData(OriginalData.Get());
	Creator = Context.GameMode.Get();
	Slot = Cast<AGGYGOCharacterSlot>(OutActors.OriginalSlot.Get());
	if (!Creator || !Creator->IsSquadCreationContextCurrent(Context) || !Slot || Slot->IsActorBeingDestroyed()
		|| !Slot->HasAuthority() || Slot->GetWorld() != Context.World.Get() || Slot->GetOwner() != Context.Controller.Get()
		|| !OriginalData.IsValid() || Slot->GetPawnData() != OriginalData.Get() || !Slot->IsPawnDataInitializationComplete())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=SlotInitialization: original Slot preparation failed; captured original retained."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath);
		return false;
	}
	return true; // Original initialization loop completed, not every GA/GE configuration or client Ready.
}

bool AGGYGOGameMode::SpawnSquadMember(const FSquadCreationContext& Context, const UGGYGOPawnData* PawnData,
	const FTransform& SpawnTransform, FUntransferredSquadActors& OutActors)
{
	const TWeakObjectPtr<const UGGYGOPawnData> OriginalData(PawnData);
	if (!IsSquadCreationContextCurrent(Context) || !OriginalData.IsValid() || !PawnData->PawnClass
		|| !PawnData->PawnClass->IsChildOf(AGGYGOCharacterBase::StaticClass())
		|| !OutActors.OriginalPawn.IsExplicitlyNull())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=SpawnPawn: invalid original context/PawnClass or occupied output; no alternate Pawn spawned."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath);
		return false;
	}
	UWorld* World = Context.World.Get();
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Context.Controller.Get();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	SpawnParams.CustomPreSpawnInitialization = [&OutActors](AActor* OriginalActor)
	{
		OutActors.OriginalPawn = OriginalActor;
	};
	AGGYGOCharacterBase* SpawnedMember = World->SpawnActor<AGGYGOCharacterBase>(
		PawnData->PawnClass.Get(), SpawnTransform, SpawnParams);
	AGGYGOGameMode* Creator = Context.GameMode.Get();
	AGGYGOCharacterBase* Member = Cast<AGGYGOCharacterBase>(OutActors.OriginalPawn.Get());
	if (!Creator || !Creator->IsSquadCreationContextCurrent(Context) || !SpawnedMember || SpawnedMember != Member
		|| !Member || Member->IsActorBeingDestroyed() || !Member->HasAuthority()
		|| Member->GetWorld() != Context.World.Get() || Member->GetOwner() != Context.Controller.Get()
		|| !OriginalData.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=SpawnPawnReturn: original Spawn/context rejected; captured Pawn=%s remains creator-owned."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath,
			*GetPathNameSafe(OutActors.OriginalPawn.Get()));
		return false;
	}
	UGGYGOPawnExtensionComponent* Extension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Member);
	if (!IsValid(Extension) || Extension->IsBeingDestroyed())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=PawnData: original Pawn=%s has no valid PawnExtension; preparation rejected."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath, *GetPathNameSafe(Member));
		return false;
	}
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(Extension);
	Extension->SetPawnData(OriginalData.Get());
	Creator = Context.GameMode.Get();
	Member = Cast<AGGYGOCharacterBase>(OutActors.OriginalPawn.Get());
	Extension = OriginalExtension.Get();
	if (!Creator || !Creator->IsSquadCreationContextCurrent(Context) || !Member || Member->IsActorBeingDestroyed()
		|| !Member->HasAuthority() || Member->GetWorld() != Context.World.Get() || Member->GetOwner() != Context.Controller.Get()
		|| (Member->GetController() && Member->GetController() != Context.Controller.Get())
		|| !Extension || Extension->IsBeingDestroyed() || Extension->GetOwner() != Member
		|| UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Member) != Extension
		|| !OriginalData.IsValid() || Extension->GetPawnData<UGGYGOPawnData>() != OriginalData.Get())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Teams] Creator=%s Controller=%s PawnData=%s Stage=PawnDataReturn: original Pawn/Extension injection readback failed; captured original retained."),
			*Context.CreatorPath, *OutActors.ControllerPath, *OutActors.PawnDataPath);
		return false;
	}
	return true;
}
