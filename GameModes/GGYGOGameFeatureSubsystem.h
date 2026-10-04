#pragma once

#include "CoreMinimal.h"
#include "GameModes/GGYGOGameFeatureClosureResolver.h"
#include "GameModes/GGYGOGameFeatureRetention.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "GGYGOGameFeatureSubsystem.generated.h"

class UWorld;
struct FGGYGOGameFeatureGIOwner;

/**
 * Immutable root/activation request snapshots and memory-lifetime lease on one GI's Loaded resource.
 * Keep this original lease through a scene's Active/pending cleanup. It cannot release the
 * resource or expose its native handle. ThreadSafe sharing protects memory lifetime only.
 */
class GGYGO_API FGGYGOGameFeatureLoadedLease final
{
public:
	/** Immutable copied roots, not current plugin states. Keep the lease alive while using the view. */
	TConstArrayView<FString> GetRootPluginURLs() const;
	/**
	 * Roots plus GFPs reachable through bShouldActivate=true edges in this resolved managed closure.
	 * Copied request values, not actual Active states or native references. Keep the lease alive
	 * while using the view. False-only dependencies remain in the GI's complete Loaded closure.
	 */
	TConstArrayView<FString> GetActivationPluginURLs() const;
	/** Game thread only; tests original World/GI admission, never policy/plugin Ready or Active. */
	bool IsAdmissionOpenFor(const UWorld& RequestWorld) const;
	~FGGYGOGameFeatureLoadedLease();

	FGGYGOGameFeatureLoadedLease(const FGGYGOGameFeatureLoadedLease&) = delete;
	FGGYGOGameFeatureLoadedLease& operator=(const FGGYGOGameFeatureLoadedLease&) = delete;

private:
	friend class UGGYGOGameFeatureSubsystem;
	FGGYGOGameFeatureLoadedLease(
		TSharedRef<FGGYGOGameFeatureGIOwner, ESPMode::ThreadSafe> InOwner,
		const UWorld& InRequestWorld, TArray<FString> InRootPluginURLs,
		TArray<FString> InActivationPluginURLs);

	TSharedRef<FGGYGOGameFeatureGIOwner, ESPMode::ThreadSafe> Owner;
	TWeakObjectPtr<const UWorld> OriginalWorld;
	TArray<FString> RootPluginURLs;
	const TArray<FString> ActivationPluginURLs;
};

/**
 * Thin GI owner for explicit managed Loaded closures. No Experience/scene/Active caller yet.
 * Supports ordinary engine-created Game/PIE GI lifetimes with the exact reviewed project
 * AssetManager after its startup attempt completes, under the native startup/shutdown order.
 * Manual/RPC GI initialization, reentry, dynamic subsystem/policy reload and
 * changing installed metadata are outside the contract; this is not an arbitrary readiness probe.
 * Engine/subsystem/AssetManager pointers only reject incompatibilities, not establish readiness.
 * Policy is acquired only inside the synchronous preparation entry and is never retained.
 */
UCLASS()
class GGYGO_API UGGYGOGameFeatureSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	using FLoadedLease = TSharedPtr<const FGGYGOGameFeatureLoadedLease, ESPMode::ThreadSafe>;
	using FPrepareCompletion = TFunction<void(
		FLoadedLease, const FGGYGOGameFeatureRetention::FLoadResult&)>;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Copies explicit input; missing/conflicting sources or any Borrowed node reject before loading.
	 * Completion is required and may be synchronous. Loaded delivers a fully constructed original
	 * World lease with root/activation request snapshots; all other results deliver null.
	 * GI/World closure before delivery interrupts success.
	 * Future scene owners must exist before calling and retain the delivered lease until their own
	 * Active/pending cleanup finishes. Map exit does not release this GI's Loaded references.
	 * Native-only entry: no Blueprint load/release API or default managed-source declaration.
	 */
	void PrepareManagedLoadedClosure(
		const UWorld& RequestWorld, FGGYGOGameFeatureClosureResolver::FInput Input,
		FPrepareCompletion Completion);

private:
	TSharedPtr<FGGYGOGameFeatureGIOwner, ESPMode::ThreadSafe> Owner;
	/** Diagnostic for this host's failed/closed lifecycle, not a native readiness flag. */
	FString InitializationError;
};
