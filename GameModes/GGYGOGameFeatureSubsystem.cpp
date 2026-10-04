#include "GameModes/GGYGOGameFeatureSubsystem.h"

#include "CoreGlobals.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFeaturesProjectPolicies.h"
#include "GameFeaturesSubsystem.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "System/GGYGOAssetManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOGameFeatureSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOGameFeatureSubsystem, Log, All);

namespace
{
	using FRetention = FGGYGOGameFeatureRetention;
	using FResolver = FGGYGOGameFeatureClosureResolver;

	// Pure selection from one validated closure; no policy, plugin state or native reference reads.
	bool TryBuildActivationPluginURLs(const FResolver::FResult& Closure,
		TArray<FString>& OutURLs, FString& OutError)
	{
		OutURLs.Reset();
		OutError.Reset();
		TMap<FString, const FResolver::FNode*> NodesByName;
		for (int32 Index = 0; Index < Closure.Nodes.Num(); ++Index)
		{
			const FResolver::FNode& Node = Closure.Nodes[Index];
			if (Node.PluginName.IsEmpty() || Node.PluginURL.IsEmpty() || NodesByName.Contains(Node.PluginName))
			{
				OutError = FString::Printf(
					TEXT("Activation request closure Nodes[%d] has an empty name/URL or duplicate name: plugin [%s], URL [%s]."),
					Index, *Node.PluginName, *Node.PluginURL);
				return false;
			}
			NodesByName.Add(Node.PluginName, &Node);
		}
		for (const FResolver::FDependencyEdge& Edge : Closure.DependencyEdges)
		{
			const FResolver::FNode* Parent = NodesByName.FindRef(Edge.ParentPluginName);
			const FResolver::FNode* Dependency = NodesByName.FindRef(Edge.DependencyPluginName);
			if (!Parent || !Dependency)
			{
				OutError = FString::Printf(TEXT("Activation request edge [%s -> %s] refers to a missing closure node."),
					*Edge.ParentPluginName, *Edge.DependencyPluginName);
				return false;
			}
			if (Edge.DependencyPluginURL.IsEmpty() || Edge.DependencyPluginURL != Dependency->PluginURL)
			{
				OutError = FString::Printf(
					TEXT("Activation request edge [%s -> %s] has an empty/mismatched URL [%s]; node URL is [%s]."),
					*Edge.ParentPluginName, *Edge.DependencyPluginName, *Edge.DependencyPluginURL, *Dependency->PluginURL);
				return false;
			}
		}

		TArray<FString> PendingPluginNames = Closure.RootPluginNames;
		TSet<FString> VisitedPluginNames;
		TArray<FString> CandidateURLs;
		for (int32 Index = 0; Index < PendingPluginNames.Num(); ++Index)
		{
			// Copy before adding dependencies, which may reallocate the pending array.
			const FString PluginName = PendingPluginNames[Index];
			if (VisitedPluginNames.Contains(PluginName))
			{
				continue;
			}
			const FResolver::FNode* Node = NodesByName.FindRef(PluginName);
			if (!Node)
			{
				OutError = FString::Printf(TEXT("Activation request plugin [%s] has no closure node."), *PluginName);
				return false;
			}
			VisitedPluginNames.Add(PluginName);
			CandidateURLs.AddUnique(Node->PluginURL);
			for (const FResolver::FDependencyEdge& Edge : Closure.DependencyEdges)
			{
				if (Edge.ParentPluginName == PluginName && Edge.bShouldActivate)
				{
					PendingPluginNames.Add(Edge.DependencyPluginName);
				}
			}
		}
		if (CandidateURLs.IsEmpty())
		{
			OutError = TEXT("Activation request roots/URLs must not be empty.");
			return false;
		}
		OutURLs = MoveTemp(CandidateURLs);
		return true;
	}

	bool IsSupportedWorldType(EWorldType::Type Type)
	{
		return Type == EWorldType::Game || Type == EWorldType::PIE;
	}

	// Rejection checks within the reviewed normal GI lifetime, not proof of policy readiness.
	bool CheckGameInstanceContext(const UGameInstance& GameInstance, FString& OutError)
	{
		if (!IsInGameThread() || IsEngineExitRequested())
		{
			OutError = TEXT("Requires the game thread before engine exit.");
			return false;
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("SkipAssetScan")))
		{
			OutError = TEXT("SkipAssetScan is outside the reviewed policy initialization path.");
			return false;
		}
		if (!IsValid(GEngine) || GameInstance.GetEngine() != GEngine)
		{
			OutError = TEXT("The GameInstance is not owned by the current engine.");
			return false;
		}
		const FWorldContext* Context = GameInstance.GetWorldContext();
		if (!Context || !IsSupportedWorldType(Context->WorldType)
			|| Context->OwningGameInstance != &GameInstance)
		{
			OutError = TEXT("Requires this GameInstance's normal Game/PIE WorldContext.");
			return false;
		}
		for (const FWorldContext& Registered : GEngine->GetWorldContexts())
		{
			if (&Registered == Context)
			{
				return true;
			}
		}
		OutError = TEXT("The GameInstance WorldContext is not registered with the current engine.");
		return false;
	}

	void RejectPreparation(const FString& Label, TArray<FString> Errors,
		UGGYGOGameFeatureSubsystem::FPrepareCompletion Completion)
	{
		FRetention::FLoadResult Result;
		for (FString& Error : Errors)
		{
			Error = FString::Printf(TEXT("%s: %s"), *Label, *Error);
			UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("%s"), *Error);
		}
		Result.Errors = MoveTemp(Errors);
		if (Completion)
		{
			Completion(nullptr, Result);
		}
	}
}

/** Only this GI resource's admission and ownership; no plugin/reference-count authority. */
struct FGGYGOGameFeatureGIOwner
{
	TWeakObjectPtr<UGameInstance> GameInstance;
	TSharedPtr<FGGYGOGameFeatureRetention, ESPMode::ThreadSafe> Retention;
	FString Label;
	bool bClosing = false;

	bool CheckWorldAdmission(const UWorld& RequestWorld, FString& OutError) const
	{
		if (!IsInGameThread())
		{
			OutError = TEXT("World admission requires the game thread.");
			return false;
		}
		if (bClosing)
		{
			OutError = TEXT("The GI owner has closed admission.");
			return false;
		}
		const UGameInstance* GI = GameInstance.Get();
		if (!IsValid(GI) || !CheckGameInstanceContext(*GI, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("The owning GameInstance is no longer valid.");
			}
			return false;
		}
		const FWorldContext* Context = GI->GetWorldContext();
		if (!IsValid(&RequestWorld) || RequestWorld.bIsTearingDown || RequestWorld.IsBeingCleanedUp()
			|| !IsSupportedWorldType(RequestWorld.WorldType)
			|| RequestWorld.GetGameInstance() != GI || Context->World() != &RequestWorld
			|| GEngine->GetWorldContextFromWorld(&RequestWorld) != Context)
		{
			OutError = TEXT("The request World is invalid, tearing down, or not this GI's current World.");
			return false;
		}
		return true;
	}

	~FGGYGOGameFeatureGIOwner()
	{
		if (Retention.IsValid())
		{
			if (IsInGameThread())
			{
				// All original leases are gone; the core also drains its own actual native calls.
				Retention->ReleaseOwnedReferences();
			}
			else
			{
				UE_LOG(LogGGYGOGameFeatureSubsystem, Error,
					TEXT("%s: Last GI resource lease returned off game thread; detached Retention cleanup is required."),
					*Label);
				// Retention's existing destructor owns the detached GT handoff; no new scheduler.
			}
		}
	}
};

FGGYGOGameFeatureLoadedLease::FGGYGOGameFeatureLoadedLease(
	TSharedRef<FGGYGOGameFeatureGIOwner, ESPMode::ThreadSafe> InOwner,
	const UWorld& InRequestWorld, TArray<FString> InRootPluginURLs,
	TArray<FString> InActivationPluginURLs)
	: Owner(MoveTemp(InOwner)), OriginalWorld(&InRequestWorld), RootPluginURLs(MoveTemp(InRootPluginURLs))
	, ActivationPluginURLs(MoveTemp(InActivationPluginURLs))
{
}

FGGYGOGameFeatureLoadedLease::~FGGYGOGameFeatureLoadedLease() = default;

TConstArrayView<FString> FGGYGOGameFeatureLoadedLease::GetRootPluginURLs() const
{
	return RootPluginURLs;
}

TConstArrayView<FString> FGGYGOGameFeatureLoadedLease::GetActivationPluginURLs() const
{
	return ActivationPluginURLs;
}

bool FGGYGOGameFeatureLoadedLease::IsAdmissionOpenFor(const UWorld& RequestWorld) const
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("Lease admission requires the game thread."));
		return false;
	}
	FString Error;
	return OriginalWorld.Get() == &RequestWorld && Owner->CheckWorldAdmission(RequestWorld, Error);
}

bool UGGYGOGameFeatureSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!IsInGameThread())
	{
		return false;
	}
	const UGameInstance* GI = Cast<UGameInstance>(Outer);
	const FWorldContext* Context = GI ? GI->GetWorldContext() : nullptr;
	return Context && IsSupportedWorldType(Context->WorldType);
}

void UGGYGOGameFeatureSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("GI owner Initialize requires the game thread."));
		return;
	}
	if (Owner.IsValid() || !InitializationError.IsEmpty())
	{
		UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("[%s] Repeated GI owner initialization rejected."), *GetPathName());
		return;
	}
	Super::Initialize(Collection);
	UGameInstance* GI = GetGameInstance();
	if (!IsValid(GI) || !CheckGameInstanceContext(*GI, InitializationError))
	{
		if (InitializationError.IsEmpty())
		{
			InitializationError = TEXT("Owning GameInstance is unavailable.");
		}
		UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("[%s] %s"), *GetPathName(), *InitializationError);
		return;
	}
	const FString Label = FString::Printf(TEXT("GameFeature GI [%s] Loaded owner"), *GI->GetPathName());
	TSharedPtr<FRetention, ESPMode::ThreadSafe> Retention = FRetention::TryCreate(Label, InitializationError);
	if (!Retention.IsValid())
	{
		UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("%s: %s"), *Label, *InitializationError);
		return;
	}
	Owner = MakeShared<FGGYGOGameFeatureGIOwner, ESPMode::ThreadSafe>();
	Owner->GameInstance = GI;
	Owner->Retention = MoveTemp(Retention);
	Owner->Label = Label;
}

void UGGYGOGameFeatureSubsystem::Deinitialize()
{
	if (!IsInGameThread())
	{
		UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("GI owner Deinitialize requires the game thread; owner remains held."));
		return;
	}
	InitializationError = TEXT("GI subsystem deinitialized; admission is closed.");
	if (Owner.IsValid())
	{
		Owner->bClosing = true;
		Owner.Reset();
	}
	Super::Deinitialize();
}

void UGGYGOGameFeatureSubsystem::PrepareManagedLoadedClosure(
	const UWorld& RequestWorld, FResolver::FInput Input, FPrepareCompletion Completion)
{
	if (!IsInGameThread())
	{
		RejectPreparation(TEXT("GameFeature GI preparation"),
			{TEXT("Requires the game thread; no resource was touched.")}, MoveTemp(Completion));
		return;
	}
	const FString Label = FString::Printf(TEXT("GameFeature GI [%s] World [%s]"),
		*GetPathNameSafe(GetGameInstance()), *RequestWorld.GetPathName());
	if (!Completion)
	{
		RejectPreparation(Label, {TEXT("A completion receiver is required; no request was issued.")}, {});
		return;
	}
	const TSharedPtr<FGGYGOGameFeatureGIOwner, ESPMode::ThreadSafe> CurrentOwner = Owner;
	if (!CurrentOwner.IsValid())
	{
		RejectPreparation(Label, {InitializationError.IsEmpty()
			? FString(TEXT("GI Loaded owner is unavailable; initialization is not retried.")) : InitializationError},
			MoveTemp(Completion));
		return;
	}
	FString Error;
	if (!CurrentOwner->CheckWorldAdmission(RequestWorld, Error))
	{
		RejectPreparation(Label, {MoveTemp(Error)}, MoveTemp(Completion));
		return;
	}
	UGameFeaturesSubsystem* GameFeatures = GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>();
	const UAssetManager* AssetManager = UAssetManager::GetIfInitialized();
	if (!IsValid(GameFeatures) || !IsValid(AssetManager))
	{
		RejectPreparation(Label, {TEXT("Current GameFeatures subsystem or AssetManager is unavailable.")}, MoveTemp(Completion));
		return;
	}
	const UGGYGOAssetManager* ReviewedManager = Cast<UGGYGOAssetManager>(AssetManager);
	if (!ReviewedManager || ReviewedManager->GetClass() != UGGYGOAssetManager::StaticClass()
		|| !ReviewedManager->HasCompletedSharedAssetPreload())
	{
		RejectPreparation(Label, {FString::Printf(
			TEXT("AssetManager [%s] must be the exact reviewed GGYGOAssetManager with its startup attempt completed."),
			*AssetManager->GetPathName())}, MoveTemp(Completion));
		return;
	}

	// Legal only in the reviewed normal GI lifetime. Pointer checks above are not readiness proof.
	// The existing project startup snapshot only rejects incomplete/unsupported providers; it is
	// not a cached GameFeature readiness flag. This exact provider calls Super::StartInitialLoading.
	// Game/Editor Engine initialization finishes native policy Init/PostInit before creating this GI;
	// normal shutdown closes its Worlds/GI before Engine subsystems. Never retain this policy reference.
	FResolver::FResult Closure = FResolver::ResolveClosureAndDeclaredSources(
		*GameFeatures, GameFeatures->GetPolicy<UGameFeaturesProjectPolicies>(), MoveTemp(Input));
	if (Closure.Status != FResolver::EStatus::ResolvedCandidate)
	{
		RejectPreparation(Label, MoveTemp(Closure.Errors), MoveTemp(Completion));
		return;
	}
	for (const FResolver::FNode& Node : Closure.Nodes)
	{
		if (Node.DeclaredSource.Source != FResolver::ESource::ProjectNativeManaged)
		{
			RejectPreparation(Label, {FString::Printf(
				TEXT("Plugin [%s] is not explicitly project-managed; borrowed protection is not implemented."),
				*Node.PluginName)}, MoveTemp(Completion));
			return;
		}
	}
	if (!Closure.BorrowedPluginNamesRequiringProtection.IsEmpty())
	{
		RejectPreparation(Label, {TEXT("Borrowed plugins require protection that is not implemented.")}, MoveTemp(Completion));
		return;
	}
	TArray<FString> URLs;
	TArray<FString> RootURLs;
	for (const FResolver::FNode& Node : Closure.Nodes)
	{
		URLs.Add(Node.PluginURL);
	}
	for (const FString& Root : Closure.RootPluginNames)
	{
		const FResolver::FNode* Node = Closure.Nodes.FindByPredicate(
			[&Root](const FResolver::FNode& Candidate) { return Candidate.PluginName == Root; });
		if (!Node)
		{
			RejectPreparation(Label, {FString::Printf(TEXT("Resolved root [%s] has no closure node."), *Root)}, MoveTemp(Completion));
			return;
		}
		RootURLs.Add(Node->PluginURL);
	}
	TArray<FString> ActivationURLs;
	if (!TryBuildActivationPluginURLs(Closure, ActivationURLs, Error))
	{
		RejectPreparation(Label, {MoveTemp(Error)}, MoveTemp(Completion));
		return;
	}
	if (URLs.IsEmpty() || RootURLs.IsEmpty() || !CurrentOwner->CheckWorldAdmission(RequestWorld, Error))
	{
		RejectPreparation(Label, {Error.IsEmpty() ? FString(TEXT("Resolved managed closure/roots must not be empty.")) : Error},
			MoveTemp(Completion));
		return;
	}
	FLoadedLease Lease = MakeShareable(new FGGYGOGameFeatureLoadedLease(
		CurrentOwner.ToSharedRef(), RequestWorld, MoveTemp(RootURLs), MoveTemp(ActivationURLs)));
	CurrentOwner->Retention->LoadAndRetainValidatedManagedClosure(URLs,
		[Lease, Label, Completion = MoveTemp(Completion)](const FRetention::FLoadResult& NativeResult)
	{
		FRetention::FLoadResult Result = NativeResult;
		if (Result.Status == FRetention::ELoadStatus::Loaded)
		{
			const UWorld* World = Lease->OriginalWorld.Get();
			if (!World || !Lease->IsAdmissionOpenFor(*World))
			{
				Result.Status = FRetention::ELoadStatus::Interrupted;
				Result.Errors.Add(FString::Printf(
					TEXT("%s: Original World/GI admission closed before Loaded delivery; no Active qualification."), *Label));
				UE_LOG(LogGGYGOGameFeatureSubsystem, Error, TEXT("%s"), *Result.Errors.Last());
			}
		}
		Completion(Result.Status == FRetention::ELoadStatus::Loaded ? Lease : FLoadedLease(), Result);
	});
}
