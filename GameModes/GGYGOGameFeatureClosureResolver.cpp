#include "GameModes/GGYGOGameFeatureClosureResolver.h"

#include "Containers/Map.h"
#include "Containers/Set.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "GameFeaturesProjectPolicies.h"
#include "GameFeaturesSubsystem.h"
#include "GameFeaturesSubsystemSettings.h"
#include "GameFeatureTypes.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"
#include "PluginReferenceDescriptor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using FResolver = FGGYGOGameFeatureClosureResolver;

	struct FDescriptorDependency
	{
		bool bEnabled = false;
		bool bShouldActivate = false;
	};

	struct FDependencyObservation
	{
		FString PluginName;
		bool bShouldActivate = false;
	};

	struct FTraversalFrame
	{
		FString PluginName;
		FString PluginURL;
		TArray<FDependencyObservation> Dependencies;
		int32 NextDependency = 0;
	};

	FResolver::FResult Reject(FString Error)
	{
		FResolver::FResult Result;
		Result.Errors.Add(FString(TEXT("GameFeature closure: ")) + Error);
		return Result;
	}

	bool IsExplicitText(const FString& Text)
	{
		return !Text.IsEmpty() && Text.TrimStartAndEnd() == Text;
	}

	// These are rejection checks, not evidence that private policy initialization is complete.
	bool CheckObservedCompatibility(
		const UGameFeaturesSubsystem& GameFeatures,
		const UGameFeaturesProjectPolicies& Policy,
		FString& OutError)
	{
		if (!IsInGameThread())
		{
			OutError = TEXT("requires the game thread");
			return false;
		}
		if (IsEngineExitRequested())
		{
			OutError = TEXT("engine exit has been requested");
			return false;
		}
		if (!IsValid(GEngine))
		{
			OutError = TEXT("the engine is unavailable");
			return false;
		}
		if (!IsValid(&GameFeatures)
			|| GEngine->GetEngineSubsystem<UGameFeaturesSubsystem>() != &GameFeatures)
		{
			OutError = TEXT("the supplied GameFeatures subsystem is not the current engine subsystem");
			return false;
		}
		if (!IsValid(UAssetManager::GetIfInitialized()))
		{
			OutError = TEXT("the engine's AssetManager object is unavailable");
			return false;
		}
		if (!IsValid(&Policy) || Policy.GetOuter() != &GameFeatures
			|| Policy.GetClass() != UDefaultGameFeaturesProjectPolicies::StaticClass())
		{
			OutError = TEXT("the supplied policy must have this subsystem as Outer and be exactly the default policy");
			return false;
		}
		const FSoftClassPath& ConfiguredPolicyClass =
			GetDefault<UGameFeaturesSubsystemSettings>()->GameFeaturesManagerClassName;
		if (!ConfiguredPolicyClass.IsNull()
			&& ConfiguredPolicyClass != FSoftClassPath(UDefaultGameFeaturesProjectPolicies::StaticClass()))
		{
			OutError = FString::Printf(TEXT("configured policy [%s] is outside the default-policy scope"),
				*ConfiguredPolicyClass.ToString());
			return false;
		}
		if (FPluginDescriptor::CustomPluginDescriptorReaderDelegate.IsBound())
		{
			OutError = TEXT("custom plugin descriptor readers are outside the supported scope");
			return false;
		}
		return true;
	}

	bool ValidateFileURL(const FString& URL, FString& OutFilename, FString& OutError)
	{
		if (!IsExplicitText(URL)
			|| !UGameFeaturesSubsystem::IsPluginURLProtocol(FStringView(URL), EGameFeaturePluginProtocol::File)
			|| URL.Contains(UE::GameFeatures::PluginURLStructureInfo::OptionSeperator))
		{
			OutError = FString::Printf(TEXT("unsupported or malformed file URL [%s]; options are unsupported"), *URL);
			return false;
		}

		// ParsePluginURL ensures on bad protocols/paths. Check its path precondition before entering it.
		const FString Prefix = UGameFeaturesSubsystem::GetPluginURL_FileProtocol(TEXT(""));
		const FString UnparsedPath = URL.RightChop(Prefix.Len());
		if (!IsExplicitText(UnparsedPath) || !UnparsedPath.EndsWith(TEXT(".uplugin"), ESearchCase::CaseSensitive))
		{
			OutError = FString::Printf(TEXT("URL [%s] does not contain a supported .uplugin path"), *URL);
			return false;
		}
		FStringView Path;
		FStringView Options;
		EGameFeaturePluginProtocol Protocol = EGameFeaturePluginProtocol::Unknown;
		if (!UGameFeaturesSubsystem::ParsePluginURL(FStringView(URL), &Protocol, &Path, &Options)
			|| Protocol != EGameFeaturePluginProtocol::File || !Options.IsEmpty())
		{
			OutError = FString::Printf(TEXT("native parsing rejected file URL [%s]"), *URL);
			return false;
		}
		OutFilename = FPaths::ConvertRelativePathToFull(FString(Path));
		if (!FPaths::FileExists(OutFilename))
		{
			OutError = FString::Printf(TEXT("installed descriptor is missing for URL [%s], file [%s]"), *URL, *OutFilename);
			return false;
		}
		return true;
	}

	bool FindInstalledPlugin(
		const FString& Name, TSharedPtr<IPlugin>& OutPlugin, FString& OutFilename, FString& OutError)
	{
		if (!IsExplicitText(Name))
		{
			OutError = FString::Printf(TEXT("invalid plugin name [%s]"), *Name);
			return false;
		}
		OutPlugin = IPluginManager::Get().FindPlugin(Name);
		if (!OutPlugin || OutPlugin->GetName() != Name)
		{
			OutError = FString::Printf(TEXT("installed plugin [%s] is missing or its canonical name differs"), *Name);
			return false;
		}
		const FString& DescriptorFilename = OutPlugin->GetDescriptorFileName();
		if (!IsExplicitText(DescriptorFilename) || FPaths::GetBaseFilename(DescriptorFilename) != Name)
		{
			OutError = FString::Printf(TEXT("plugin [%s] has an invalid or conflicting descriptor filename [%s]"),
				*Name, *DescriptorFilename);
			return false;
		}
		OutFilename = FPaths::ConvertRelativePathToFull(DescriptorFilename);
		if (!FPaths::FileExists(OutFilename))
		{
			OutError = FString::Printf(TEXT("plugin [%s] descriptor is not installed: [%s]"), *Name, *OutFilename);
			return false;
		}
		return true;
	}

	bool ReadDescriptorDependencies(
		const FString& Filename, TMap<FString, FDescriptorDependency>& OutDependencies, FString& OutError)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (!FFileHelper::LoadFileToString(Text, *Filename)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object)
		{
			OutError = FString::Printf(TEXT("cannot read a descriptor JSON object from [%s]"), *Filename);
			return false;
		}
		// Reject malformed native GF fields before GetDetails can apply its permissive defaults.
		if (Object->HasField(TEXT("BuiltInInitialFeatureState")))
		{
			FString InitialState;
			if (!Object->TryGetStringField(TEXT("BuiltInInitialFeatureState"), InitialState)
				|| (InitialState != TEXT("Installed") && InitialState != TEXT("Registered")
					&& InitialState != TEXT("Loaded") && InitialState != TEXT("Active")))
			{
				OutError = FString::Printf(TEXT("invalid BuiltInInitialFeatureState in [%s]"), *Filename);
				return false;
			}
		}
		const TCHAR* BooleanKeys[] = { TEXT("BuiltInAutoRegister"), TEXT("BuiltInAutoLoad"),
			TEXT("BuiltInAutoActivate"), TEXT("Hotfixable") };
		for (const TCHAR* Key : BooleanKeys)
		{
			bool Value = false;
			if (Object->HasField(Key) && !Object->TryGetBoolField(Key, Value))
			{
				OutError = FString::Printf(TEXT("field [%s] must be a boolean in [%s]"), Key, *Filename);
				return false;
			}
		}
		if (!Object->HasField(TEXT("Plugins")))
		{
			return true; // An absent optional Plugins field declares no dependencies.
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Object->TryGetArrayField(TEXT("Plugins"), Entries) || !Entries)
		{
			OutError = FString::Printf(TEXT("Plugins must be an array in [%s]"), *Filename);
			return false;
		}
		for (int32 Index = 0; Index < Entries->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			FString Name;
			FDescriptorDependency Dependency;
			if (!(*Entries)[Index] || !(*Entries)[Index]->TryGetObject(Entry) || !Entry || !*Entry
				|| !(*Entry)->TryGetStringField(TEXT("Name"), Name) || !IsExplicitText(Name)
				|| !(*Entry)->TryGetBoolField(TEXT("Enabled"), Dependency.bEnabled)
				|| ((*Entry)->HasField(TEXT("Activate"))
					&& !(*Entry)->TryGetBoolField(TEXT("Activate"), Dependency.bShouldActivate)))
			{
				OutError = FString::Printf(TEXT("malformed Plugins[%d] Name/Enabled/Activate in [%s]"), Index, *Filename);
				return false;
			}
			if (OutDependencies.Contains(Name))
			{
				OutError = FString::Printf(TEXT("duplicate dependency [%s] in [%s]"), *Name, *Filename);
				return false;
			}
			OutDependencies.Add(Name, Dependency);
		}
		return true;
	}

	bool CollectCrossCheckedDependencies(
		const IPlugin& Plugin, const FString& Filename, const TMap<FString, FDescriptorDependency>& FileDependencies,
		const FGameFeaturePluginDetails& NativeDetails,
		TArray<FDependencyObservation>& OutDependencies, FString& OutError)
	{
		const TArray<FPluginReferenceDescriptor>& ProjectDependencies = Plugin.GetDescriptor().Plugins;
		if (ProjectDependencies.Num() != FileDependencies.Num())
		{
			OutError = FString::Printf(TEXT("Projects/disk dependency counts disagree for [%s], file [%s]"),
				*Plugin.GetName(), *Filename);
			return false;
		}
		TSet<FString> ProjectNames;
		TMap<FString, bool> EnabledDependencies;
		for (const FPluginReferenceDescriptor& Dependency : ProjectDependencies)
		{
			const FDescriptorDependency* FileDependency = FileDependencies.Find(Dependency.Name);
			if (!IsExplicitText(Dependency.Name) || ProjectNames.Contains(Dependency.Name)
				|| !FileDependency || FileDependency->bEnabled != Dependency.bEnabled
				|| FileDependency->bShouldActivate != Dependency.bActivate)
			{
				OutError = FString::Printf(TEXT("Projects/disk metadata disagree for [%s] -> [%s], file [%s]"),
					*Plugin.GetName(), *Dependency.Name, *Filename);
				return false;
			}
			ProjectNames.Add(Dependency.Name);
			if (Dependency.bEnabled)
			{
				EnabledDependencies.Add(Dependency.Name, Dependency.bActivate);
			}
		}
		if (NativeDetails.PluginDependencies.Num() != EnabledDependencies.Num())
		{
			OutError = FString::Printf(TEXT("native/Projects enabled dependency counts disagree for [%s], file [%s]"),
				*Plugin.GetName(), *Filename);
			return false;
		}
		TSet<FString> NativeNames;
		for (const FGameFeaturePluginReferenceDetails& Dependency : NativeDetails.PluginDependencies)
		{
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			const FString Name(Dependency.PluginName);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS
			const bool* bExpectedActivate = EnabledDependencies.Find(Name);
			if (!IsExplicitText(Name) || NativeNames.Contains(Name) || !bExpectedActivate
				|| *bExpectedActivate != Dependency.bShouldActivate)
			{
				OutError = FString::Printf(TEXT("native/Projects metadata disagree for [%s] -> [%s], file [%s]"),
					*Plugin.GetName(), *Name, *Filename);
				return false;
			}
			NativeNames.Add(Name);
			FDependencyObservation& Observation = OutDependencies.AddDefaulted_GetRef();
			Observation.PluginName = Name;
			Observation.bShouldActivate = Dependency.bShouldActivate;
		}
		return true;
	}

	bool BuildFrame(
		const UGameFeaturesSubsystem& GameFeatures, const UGameFeaturesProjectPolicies& Policy,
		const FString& Name, const FString& URL, const FResolver::FSourceDeclaration& Source,
		FTraversalFrame& OutFrame, FResolver::FNode& OutNode, FString& OutError)
	{
		if (!CheckObservedCompatibility(GameFeatures, Policy, OutError))
		{
			return false;
		}
		FString URLFilename;
		FString InstalledFilename;
		TSharedPtr<IPlugin> Plugin;
		if (!ValidateFileURL(URL, URLFilename, OutError)
			|| !FindInstalledPlugin(Name, Plugin, InstalledFilename, OutError))
		{
			return false;
		}
		FString PolicyURL;
		if (!FPaths::IsSamePath(URLFilename, InstalledFilename)
			|| !Policy.GetGameFeaturePluginURL(Plugin.ToSharedRef(), PolicyURL)
			|| PolicyURL.IsEmpty() || PolicyURL != URL)
		{
			OutError = FString::Printf(TEXT("GFP identity, installed path or default policy URL disagree for [%s], URL [%s], file [%s]"),
				*Name, *URL, *InstalledFilename);
			return false;
		}
		TMap<FString, FDescriptorDependency> FileDependencies;
		if (!ReadDescriptorDependencies(InstalledFilename, FileDependencies, OutError))
		{
			return false;
		}
		FGameFeaturePluginDetails NativeDetails;
		if (!GameFeatures.GetGameFeaturePluginDetails(URL, NativeDetails))
		{
			OutError = FString::Printf(TEXT("native plugin details unavailable for [%s], URL [%s]"), *Name, *URL);
			return false;
		}
		if (!CollectCrossCheckedDependencies(*Plugin, InstalledFilename, FileDependencies, NativeDetails, OutFrame.Dependencies, OutError)
			|| !CheckObservedCompatibility(GameFeatures, Policy, OutError))
		{
			return false;
		}
		OutFrame.PluginName = Name;
		OutFrame.PluginURL = URL;
		OutNode.PluginName = Name;
		OutNode.PluginURL = URL;
		OutNode.DescriptorFilename = InstalledFilename;
		OutNode.DeclaredSource = Source;
		return true;
	}

	bool CheckNonGameFeatureDependency(
		const UGameFeaturesSubsystem& GameFeatures, const UGameFeaturesProjectPolicies& Policy,
		const FString& Name, FString& OutFilename, FString& OutError)
	{
		TSharedPtr<IPlugin> Plugin;
		if (!FindInstalledPlugin(Name, Plugin, OutFilename, OutError))
		{
			return false;
		}
		FString PolicyURL;
		FString MappedURL;
		if (!Policy.GetGameFeaturePluginURL(Plugin.ToSharedRef(), PolicyURL) || !PolicyURL.IsEmpty()
			|| GameFeatures.GetPluginURLByName(Name, MappedURL))
		{
			OutError = FString::Printf(TEXT("empty dependency URL conflicts with installed/default/native GFP identity for [%s]"), *Name);
			return false;
		}
		return true;
	}

	FString DescribeCycle(const TArray<FTraversalFrame>& Stack, const FString& DependencyName)
	{
		TArray<FString> Path;
		for (const FTraversalFrame& Frame : Stack)
		{
			Path.Add(Frame.PluginName);
		}
		Path.Add(DependencyName);
		return FString::Join(Path, TEXT(" -> "));
	}
}

FGGYGOGameFeatureClosureResolver::FResult FGGYGOGameFeatureClosureResolver::ResolveClosureAndDeclaredSources(
	const UGameFeaturesSubsystem& GameFeatures,
	const UGameFeaturesProjectPolicies& CurrentPolicy,
	FInput Input)
{
	FString Error;
	if (!CheckObservedCompatibility(GameFeatures, CurrentPolicy, Error))
	{
		return Reject(MoveTemp(Error));
	}
	if (Input.RootPluginNames.IsEmpty())
	{
		return Reject(TEXT("at least one explicit root plugin name is required"));
	}

	TMap<FString, FSourceDeclaration> Sources;
	for (const FSourceDeclaration& Declaration : Input.DeclaredSources)
	{
		if (!IsExplicitText(Declaration.PluginName)
			|| (Declaration.Source != ESource::ProjectNativeManaged && Declaration.Source != ESource::ExplicitExternalBorrow)
			|| (Declaration.Source == ESource::ProjectNativeManaged && !Declaration.ExternalOwnerLabel.IsEmpty())
			|| (Declaration.Source == ESource::ExplicitExternalBorrow && !IsExplicitText(Declaration.ExternalOwnerLabel)))
		{
			return Reject(FString::Printf(TEXT("invalid source declaration for [%s]"), *Declaration.PluginName));
		}
		if (const FSourceDeclaration* Existing = Sources.Find(Declaration.PluginName))
		{
			if (Existing->Source != Declaration.Source || Existing->ExternalOwnerLabel != Declaration.ExternalOwnerLabel)
			{
				return Reject(FString::Printf(TEXT("conflicting source declarations for [%s]"), *Declaration.PluginName));
			}
		}
		else
		{
			Sources.Add(Declaration.PluginName, Declaration);
		}
	}

	FResult Candidate;
	TSet<FString> RootNames;
	for (const FString& Root : Input.RootPluginNames)
	{
		if (!IsExplicitText(Root))
		{
			return Reject(FString::Printf(TEXT("invalid root plugin name [%s]"), *Root));
		}
		if (!RootNames.Contains(Root))
		{
			RootNames.Add(Root);
			Candidate.RootPluginNames.Add(Root);
		}
	}

	TMap<FString, FString> URLsByName;
	TMap<FString, FString> NamesByURL;
	TSet<FString> Visiting;
	TArray<FTraversalFrame> Stack;
	// Traversal borrows inputs only during this synchronous invocation; the lambda never escapes.
	auto PushNode = [&](const FString& Name, const FString& URL) -> bool
	{
		if (const FString* ExistingName = NamesByURL.Find(URL))
		{
			Error = FString::Printf(TEXT("URL [%s] has conflicting plugin identities [%s] and [%s]"), *URL, **ExistingName, *Name);
			return false;
		}
		const FSourceDeclaration* Source = Sources.Find(Name);
		if (!Source)
		{
			Error = FString::Printf(TEXT("GFP [%s], URL [%s] has no explicit Managed/Borrowed source declaration"), *Name, *URL);
			return false;
		}
		FTraversalFrame Frame;
		FNode Node;
		if (!BuildFrame(GameFeatures, CurrentPolicy, Name, URL, *Source, Frame, Node, Error))
		{
			return false;
		}
		URLsByName.Add(Name, URL);
		NamesByURL.Add(URL, Name);
		Visiting.Add(Name);
		if (Source->Source == ESource::ExplicitExternalBorrow)
		{
			Candidate.BorrowedPluginNamesRequiringProtection.Add(Name);
		}
		Candidate.Nodes.Add(MoveTemp(Node));
		Stack.Add(MoveTemp(Frame));
		return true;
	};

	for (const FString& Root : Candidate.RootPluginNames)
	{
		if (!CheckObservedCompatibility(GameFeatures, CurrentPolicy, Error))
		{
			return Reject(MoveTemp(Error));
		}
		FString RootURL;
		if (!GameFeatures.GetPluginURLByName(Root, RootURL) || RootURL.IsEmpty())
		{
			return Reject(FString::Printf(TEXT("root [%s] has no native plugin URL mapping"), *Root));
		}
		if (const FString* ExistingURL = URLsByName.Find(Root))
		{
			if (*ExistingURL != RootURL)
			{
				return Reject(FString::Printf(TEXT("conflicting URL mappings for root [%s]"), *Root));
			}
			continue; // This root was already fully visited as a dependency of an earlier root.
		}
		if (!PushNode(Root, RootURL))
		{
			return Reject(MoveTemp(Error));
		}
		while (!Stack.IsEmpty())
		{
			FTraversalFrame& Frame = Stack.Last();
			if (Frame.NextDependency == Frame.Dependencies.Num())
			{
				Visiting.Remove(Frame.PluginName);
				Stack.Pop();
				continue;
			}
			// Copy before a push can reallocate Stack; every enabled edge is processed regardless of Activate.
			const FString ParentName = Frame.PluginName;
			const FString ParentURL = Frame.PluginURL;
			const FDependencyObservation Dependency = Frame.Dependencies[Frame.NextDependency++];
			if (!CheckObservedCompatibility(GameFeatures, CurrentPolicy, Error))
			{
				return Reject(MoveTemp(Error));
			}
			FPluginDependencyDetails DependencyDetails;
			TValueOrError<FString, FString> Resolution =
				CurrentPolicy.ResolvePluginDependency(ParentURL, Dependency.PluginName, DependencyDetails);
			if (Resolution.HasError())
			{
				return Reject(FString::Printf(TEXT("policy dependency resolution failed [%s] -> [%s]: [%s]; optional omissions are unsupported"),
					*ParentName, *Dependency.PluginName, *Resolution.GetError()));
			}
			if (!Resolution.HasValue())
			{
				return Reject(FString::Printf(TEXT("policy returned neither a value nor an error for [%s] -> [%s]"),
					*ParentName, *Dependency.PluginName));
			}
			const FString DependencyURL = Resolution.GetValue();
			if (!CheckObservedCompatibility(GameFeatures, CurrentPolicy, Error))
			{
				return Reject(MoveTemp(Error));
			}
			if (DependencyURL.IsEmpty())
			{
				FString Filename;
				if (!CheckNonGameFeatureDependency(GameFeatures, CurrentPolicy, Dependency.PluginName, Filename, Error))
				{
					return Reject(MoveTemp(Error));
				}
				FNonGameFeatureDependency& Excluded = Candidate.NonGameFeatureDependencies.AddDefaulted_GetRef();
				Excluded.ParentPluginName = ParentName;
				Excluded.DependencyPluginName = Dependency.PluginName;
				Excluded.DescriptorFilename = MoveTemp(Filename);
				Excluded.bShouldActivate = Dependency.bShouldActivate;
				Excluded.PolicyEvidence = TEXT("Default ResolvePluginDependency returned success with an empty URL; installed identity cross-checked");
				continue;
			}
			FString MappedURL;
			if (!GameFeatures.GetPluginURLByName(Dependency.PluginName, MappedURL) || MappedURL != DependencyURL)
			{
				return Reject(FString::Printf(TEXT("policy/native URL mapping mismatch [%s] -> [%s]: policy [%s], native [%s]"),
					*ParentName, *Dependency.PluginName, *DependencyURL, *MappedURL));
			}
			if (Visiting.Contains(Dependency.PluginName))
			{
				return Reject(FString(TEXT("dependency cycle: ")) + DescribeCycle(Stack, Dependency.PluginName));
			}
			FDependencyEdge& Edge = Candidate.DependencyEdges.AddDefaulted_GetRef();
			Edge.ParentPluginName = ParentName;
			Edge.DependencyPluginName = Dependency.PluginName;
			Edge.DependencyPluginURL = DependencyURL;
			Edge.bShouldActivate = Dependency.bShouldActivate;
			if (const FString* ExistingURL = URLsByName.Find(Dependency.PluginName))
			{
				if (*ExistingURL != DependencyURL)
				{
					return Reject(FString::Printf(TEXT("conflicting URLs for dependency [%s]"), *Dependency.PluginName));
				}
			}
			else if (!PushNode(Dependency.PluginName, DependencyURL))
			{
				return Reject(MoveTemp(Error));
			}
		}
	}

	for (const TPair<FString, FSourceDeclaration>& Source : Sources)
	{
		if (!URLsByName.Contains(Source.Key))
		{
			return Reject(FString::Printf(TEXT("unused or non-GFP source declaration [%s] is outside the resolved closure"), *Source.Key));
		}
	}
	if (!CheckObservedCompatibility(GameFeatures, CurrentPolicy, Error))
	{
		return Reject(MoveTemp(Error));
	}
	for (const FNode& Node : Candidate.Nodes)
	{
		FString CurrentURL;
		if (!GameFeatures.GetPluginURLByName(Node.PluginName, CurrentURL) || CurrentURL != Node.PluginURL)
		{
			return Reject(FString::Printf(TEXT("native mapping changed during resolution for [%s]"), *Node.PluginName));
		}
	}
	Candidate.Status = EStatus::ResolvedCandidate;
	return Candidate;
}
