#pragma once

#include "EnhancedPlayerInput.h"
#include "Engine/EngineTypes.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/Optional.h"

#include "GGYGOPlayerInput.generated.h"

class APawn;
class AController;
class AActor;
class UInputAction;
class UInputComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UGGYGOMovementInputOriginResource;

/** One immutable session per native delegate subscription, including callbacks already in flight. */
UCLASS(Transient)
class UGGYGOMovementInputRouteObserver : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UEnhancedInputLocalPlayerSubsystem* Subsystem, APawn* Pawn,
		const FGGYGOMovementInputSessionIdentity& Session);
	void Detach();

private:
	UFUNCTION()
	void OnMappingsRebuilt();
	UFUNCTION()
	void OnControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);
	UFUNCTION()
	void OnPawnEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason);

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> OriginalSubsystem;
	TWeakObjectPtr<APawn> OriginalPawn;
	FGGYGOMovementInputSessionIdentity OriginalSession;
};

/** Native input provenance only; movement execution and failure admission remain in CMC. */
UCLASS(Config = Input, Transient)
class GGYGO_API UGGYGOPlayerInput : public UEnhancedPlayerInput
{
	GENERATED_BODY()

public:
	bool BeginMovementInputSession(APawn* Pawn, UInputComponent* Component, const UInputAction* Action,
		FGGYGOMovementInputSessionIdentity& OutSession, FString& OutError);
	bool AttachMovementInputReceiver(const FGGYGOMovementInputSessionIdentity& Session,
		const FGGYGOMovementInputConsumerBindingId& Binding,
		FGGYGOMovementInputFactDelegate Receiver, FString& OutError);
	void EndMovementInputSession(const FGGYGOMovementInputSessionIdentity& Session, FName Reason);
	bool GetMovementInputRequest(const FGGYGOMovementInputSessionIdentity& Session,
		FGGYGOMovementInputRequestIdentity& OutRequest, FString& OutError) const;

	/** Records this construction only inside the original resource's explicit native scope. */
	virtual void PostInitProperties() override;
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;
	virtual void FlushPressedKeys() override;
	virtual void BeginDestroy() override;

protected:
	virtual void EvaluateInputDelegates(const TArray<UInputComponent*>& InputComponentStack,
		float DeltaTime, bool bGamePaused, const TArray<TPair<FKey, FKeyState*>>& KeysWithEvents) override;
	virtual void EvaluateBlockedInputComponent(UInputComponent* Component) override;

private:
	enum class ESourceProof : uint8 { Unknown, Neutral, Held };
	/** Actual native observations only; movement teardown never changes this record. */
	struct FPhysicalSource
	{
		FKey Key;
		FInputDeviceId Device;
		FVector RawValue = FVector::ZeroVector;
		uint8 ObservedAxes = 0;
		bool bDigitalDown = false;
		bool bUnproven = false;
		FName UnprovenReason = NAME_None;
		uint64 NativeProofGapRevision = 0;
		uint64 LastRealReleaseRevision = 0;
		// All required axis samples must follow a movement barrier, not only the final sample.
		uint64 LastRealReleaseProofStartRevision = 0;
		uint64 AxisObservationRevisions[3] = {};

		bool HasNativeReleaseGap() const
		{
			return NativeProofGapRevision != 0 && LastRealReleaseRevision <= NativeProofGapRevision;
		}
	};
	/** Derived movement membership/recovery only; never copies raw values or held. */
	struct FMovementSourceObligation
	{
		FKey Key;
		FInputDeviceId Device;
		bool bParticipant = false;
		TOptional<uint64> ReleaseAfterObservationRevision;
		FName ReleaseReason = NAME_None;
	};
	struct FNativePhysicalSourceObservation
	{
		FKey Key;
		FInputDeviceId Device;
		bool bRealHeldEdge = false;
		bool bRealDigitalPress = false;
		bool bRealRelease = false;
	};
	struct FNativePhysicalObservation
	{
		uint64 Revision = 0;
		TArray<FNativePhysicalSourceObservation, TInlineAllocator<2>> Sources;
	};
	struct FPhysicalObservation
	{
		uint64 Revision = 0;
		ESourceProof Before = ESourceProof::Unknown;
		ESourceProof After = ESourceProof::Unknown;
		bool bRealHeldEdge = false;
		bool bHadParticipantsBefore = false;
		bool bRealDigitalPress = false;
	};
	struct FRouteMapping
	{
		FKey Key;
		TArray<TWeakObjectPtr<UObject>> Objects;
		int32 MappingModifierCount = 0;
		int32 MappingTriggerCount = 0;
		int32 ActionModifierCount = 0;
		int32 ActionTriggerCount = 0;
		bool Matches(const FRouteMapping& Other) const;
	};
	struct FPendingFact
	{
		FGGYGOMovementInputConsumerBindingId Binding;
		FGGYGOMovementInputFact Fact;
		FGGYGOMovementInputFactDelegate Receiver;
	};

	bool ReadRoute(const UInputAction* Action, TArray<FRouteMapping>& OutMappings, FString& OutError) const;
	bool IsCurrentSession(const FGGYGOMovementInputSessionIdentity& Session) const;
	bool ValidateCurrentRoute(FString& OutError) const;
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> SnapshotOriginForCleanup() const;
	bool CaptureOriginalOrigin(FString& OutError);
	bool ValidateOriginalOrigin(FString& OutError) const;
	void RetireOriginalQualification(FName Reason, bool bEstablishBarrier = false);
	bool HasParticipatingMovementSources() const;
	bool IssuePhysicalRequest(EGGYGOMovementInputStartProof Proof,
		const FGGYGOMovementInputSessionIdentity& ExpectedSession);
	ESourceProof ReadSourceProof() const;
	FString DescribeSourceProof() const;
	bool IsMappedMovementSource(const FKey& Key) const;
	void AdoptKnownMovementSources();
	void EstablishMovementReleaseBarrier(FName Reason);
	const FPhysicalSource* FindPhysicalSource(const FKey& Key, FInputDeviceId Device) const;
	const FMovementSourceObligation* FindMovementSourceObligation(const FPhysicalSource& Source) const;
	FMovementSourceObligation& FindOrAddMovementSourceObligation(const FPhysicalSource& Source);
	bool IsMovementReleaseRequired(const FPhysicalSource& Source) const;
	void InvalidatePhysicalInputObservations(FName Reason);
	FNativePhysicalObservation ObservePhysicalInput(const FInputKeyEventArgs& Params);
	FPhysicalObservation ProjectMovementObservation(const FNativePhysicalObservation& Native,
		ESourceProof Before, bool bHadParticipantsBefore);
	void UpdateRequestFromSource(const FPhysicalObservation& Observation,
		const FGGYGOMovementInputSessionIdentity& ExpectedSession);
	bool Publish(EGGYGOMovementInputFactKind Kind, uint64 RequestSerial, FName Reason,
		const FGGYGOMovementInputSessionIdentity& ExpectedSession);
	void DeliverPendingFacts();
	bool AllocateSerial(uint64& Counter, uint64& OutSerial);
	FString MakeError(const FString& Reason) const;
	void ReportOnce(FName Reason, const FString& Detail);

	// Native device observations survive session teardown/flush; synthetic releases never clear them.
	TArray<FPhysicalSource> PhysicalSources;
	TArray<FMovementSourceObligation> MovementSourceObligations;
	TArray<FRouteMapping> RouteMappings;
	TWeakObjectPtr<APawn> SessionPawn;
	TWeakObjectPtr<UInputComponent> SessionComponent;
	TWeakObjectPtr<const UInputAction> SessionAction;
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> SessionSubsystem;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMovementInputRouteObserver> RouteObserver;
	FGGYGOMovementInputSessionIdentity CurrentSession;
	// Captured once from authenticated Cold; a new producer cannot infer transferred observations.
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> ProducerOriginResource;
	EGGYGOMovementInputSessionMode CurrentSessionMode = EGGYGOMovementInputSessionMode::Invalid;
	// This session's outstanding Claim obligation, always checked against the resource authority.
	bool bSessionInitialClaimPending = false;
	FGGYGOMovementInputConsumerBindingId ReceiverBinding;
	FGGYGOMovementInputFactDelegate FactReceiver;
	TSet<FName> ReportedReasons;
	// Original outgoing fact copies only; drained synchronously on the same call stack.
	TArray<FPendingFact> PendingFacts;
	uint64 LastSessionSerial = 0;
	uint64 LastRequestSerial = 0;
	uint64 LastEventSerial = 0;
	uint64 ActiveRequestSerial = 0;
	uint64 ObservationRevision = 0;
	uint64 PreparedObservationRevision = 0;
	bool bNeutralConfirmed = false;
	bool bRequestSourceUnresolved = false;
	bool bInsideFlush = false;
	bool bEndingInput = false;
	bool bSerialExhausted = false;
	bool bDeliveringFacts = false;
	bool bBatchingFacts = false;
};
