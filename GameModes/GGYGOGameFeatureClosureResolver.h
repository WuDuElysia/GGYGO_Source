#pragma once

#include "CoreMinimal.h"

class UGameFeaturesSubsystem;
class UGameFeaturesProjectPolicies;

/**
 * Produces an owned value candidate for installed, mapped file GFPs under the exact default policy.
 * "Installed" means the descriptor exists on disk, not a native plugin state.
 * No instance, native reference, plugin state, policy reference or readiness cache is retained.
 */
class GGYGO_API FGGYGOGameFeatureClosureResolver final
{
public:
	enum class ESource : uint8
	{
		Unspecified,
		ProjectNativeManaged,
		ExplicitExternalBorrow
	};

	struct FSourceDeclaration
	{
		FString PluginName;
		ESource Source = ESource::Unspecified;
		/** Required only for a borrow; a label does not prove protection or its lifetime. */
		FString ExternalOwnerLabel;
	};

	struct FInput
	{
		TArray<FString> RootPluginNames;
		/** Every reached GFP needs one explicit declaration; conflicting or unused entries reject. */
		TArray<FSourceDeclaration> DeclaredSources;
	};

	enum class EStatus : uint8
	{
		Rejected,
		ResolvedCandidate
	};

	struct FNode
	{
		FString PluginName;
		FString PluginURL;
		FString DescriptorFilename;
		FSourceDeclaration DeclaredSource;
	};

	struct FDependencyEdge
	{
		FString ParentPluginName;
		FString DependencyPluginName;
		FString DependencyPluginURL;
		/** Recorded native metadata. Both true and false dependencies belong to this closure. */
		bool bShouldActivate = false;
	};

	struct FNonGameFeatureDependency
	{
		FString ParentPluginName;
		FString DependencyPluginName;
		FString DescriptorFilename;
		bool bShouldActivate = false;
		/** Actual default policy returned a value containing an empty URL. */
		FString PolicyEvidence;
	};

	struct FResult
	{
		EStatus Status = EStatus::Rejected;
		TArray<FString> RootPluginNames;
		TArray<FNode> Nodes;
		TArray<FDependencyEdge> DependencyEdges;
		TArray<FNonGameFeatureDependency> NonGameFeatureDependencies;
		TArray<FString> BorrowedPluginNamesRequiringProtection;
		/** Rejected results contain errors and no partial candidate arrays. */
		TArray<FString> Errors;
	};

	/**
	 * The future sole caller must acquire the actual current policy and review a legal synchronous
	 * native policy lifetime covering this entire call, including GetGameFeaturePluginDetails.
	 * The actual native PolicyPostInit stack provides initialization-order evidence; ordinary later
	 * call sites need their own lifecycle review. No production caller exists in this step.
	 * Initialization/shutdown reentry, deferred references, dynamic subsystem reload and unstable
	 * installed metadata are outside the contract. This is not a safe arbitrary-stage readiness probe.
	 *
	 * Public thread/exit/identity/Outer/class checks only reject observed incompatibilities.
	 * IsValid, Outer, Engine.IsInitialized or AssetManager existence cannot establish policy readiness.
	 * The resolver never acquires the policy, stores these references or accepts a readiness flag.
	 * Custom descriptor readers and file URL options are unsupported.
	 *
	 * Input is copied. Output only describes the observed metadata graph and explicit source claims.
	 * Borrow protection, native ownership, Loaded retention, Active cleanup and release remain unproved.
	 * A result must not authorize native reference or lifecycle operations.
	 */
	static FResult ResolveClosureAndDeclaredSources(
		const UGameFeaturesSubsystem& GameFeatures,
		const UGameFeaturesProjectPolicies& CurrentPolicy,
		FInput Input);

private:
	FGGYGOGameFeatureClosureResolver() = delete;
};

