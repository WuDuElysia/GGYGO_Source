#include "Input/GGYGOPlayerInput.h"

#include "Components/InputComponent.h"
#include "EnhancedActionKeyMapping.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "Input/GGYGOMovementInputOriginResource.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Player/GGYGOLocalPlayer.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMovementInput, Log, All);

namespace
{
	uint8 RequiredAxes(const FKey& Key)
	{
		return Key.IsAxis3D() ? 0b111 : Key.IsAxis2D() ? 0b011 : 0b001;
	}
}

void UGGYGOPlayerInput::PostInitProperties()
{
	const bool bWasTemplate = IsTemplate();
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	const TWeakObjectPtr<APlayerController> OriginalController(Cast<APlayerController>(GetOuter()));
	TWeakObjectPtr<ULocalPlayer> OriginalLocalPlayer;
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource;
	if (!bWasTemplate && OriginalController.IsValid() && !OriginalController->IsTemplate()
		&& !OriginalController->IsActorBeingDestroyed())
	{
		OriginalLocalPlayer = OriginalController->GetLocalPlayer();
		if (UGGYGOLocalPlayer* Host = Cast<UGGYGOLocalPlayer>(OriginalLocalPlayer.Get()))
		{
			if (!Host->IsTemplate())
			{
				OriginalResource = Host->GetMovementInputOriginResource();
			}
		}
	}
	const FString OriginalContext = FString::Printf(
		TEXT("Construction=%s Class=%s Controller=%s LocalPlayer=%s Resource=%s"),
		*GetPathNameSafe(this), *GetPathNameSafe(GetClass()), *GetPathNameSafe(OriginalController.Get()),
		*GetPathNameSafe(OriginalLocalPlayer.Get()), *GetPathNameSafe(OriginalResource.Get()));

	Super::PostInitProperties();
	if (bWasTemplate)
	{
		return;
	}
	const auto Reject = [&](FName Reason, const FString& Detail)
	{
		const FString Error = FString::Printf(TEXT("%s: %s"), *OriginalContext, *Detail);
		if (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
		{
			Producer->ReportOnce(Reason, Error);
		}
		else
		{
			// The original object is no longer usable; diagnostics use only captured values.
			UE_LOG(LogGGYGOMovementInput, Warning, TEXT("[Input.OriginConstruction] %s Reason=%s"),
				*Error, *Reason.ToString());
		}
	};
	UGGYGOPlayerInput* const Producer = OriginalProducer.Get();
	if (!Producer)
	{
		Reject(TEXT("ProducerLostDuringPostInit"), TEXT("Native Super invalidated the original producer; construction cannot be registered."));
		return;
	}
	if (Producer->IsTemplate())
	{
		return;
	}
	APlayerController* const Controller = OriginalController.Get();
	if (!Controller || Controller->IsTemplate() || Controller->IsActorBeingDestroyed()
		|| Producer->GetOuter() != Controller
		|| !OriginalLocalPlayer.HasSameIndexAndSerialNumber(TWeakObjectPtr<ULocalPlayer>(Controller->GetLocalPlayer())))
	{
		Reject(TEXT("PostInitOriginalControllerChanged"), TEXT("The original Controller/LocalPlayer/Outer relationship is invalid or changed; no successor is adopted."));
		return;
	}
	if (OriginalLocalPlayer.IsExplicitlyNull())
	{
		// Native creation before SetPlayer, or a nonlocal Controller, does not request qualification.
		return;
	}
	UGGYGOLocalPlayer* const Host = Cast<UGGYGOLocalPlayer>(OriginalLocalPlayer.Get());
	if (!IsValid(Host) || Host->IsTemplate() || Host->PlayerController != Controller
		|| !OriginalResource.IsValid() || OriginalResource->IsTemplate() || OriginalResource->GetOuter() != Host
		|| !OriginalResource.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<UGGYGOMovementInputOriginResource>(Host->GetMovementInputOriginResource())))
	{
		Reject(TEXT("PostInitOriginalResourceUnavailable"), TEXT("Real PlayerAdded did not retain the original valid resource/host relationship; this producer cannot create or replace one."));
		return;
	}
	FString Error;
	if (!OriginalResource->RecordNativeCreatedProducer(Controller, Producer, Error))
	{
		Reject(TEXT("NativeConstructionRecordRejected"), Error);
		return;
	}
	// This is a witness, not qualification consumption. D9 alone completes/closes its original ticket.
}

void UGGYGOMovementInputRouteObserver::Initialize(UEnhancedInputLocalPlayerSubsystem* Subsystem, APawn* Pawn,
	const FGGYGOMovementInputSessionIdentity& Session)
{
	check(OriginalSession.SessionSerial == 0);
	OriginalSubsystem = Subsystem;
	OriginalPawn = Pawn;
	OriginalSession = Session;
	Subsystem->ControlMappingsRebuiltDelegate.AddDynamic(this, &ThisClass::OnMappingsRebuilt);
	Pawn->ReceiveControllerChangedDelegate.AddDynamic(this, &ThisClass::OnControllerChanged);
	Pawn->OnEndPlay.AddDynamic(this, &ThisClass::OnPawnEndPlay);
}

void UGGYGOMovementInputRouteObserver::Detach()
{
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = OriginalSubsystem.Get())
	{
		Subsystem->ControlMappingsRebuiltDelegate.RemoveDynamic(this, &ThisClass::OnMappingsRebuilt);
	}
	if (APawn* Pawn = OriginalPawn.Get())
	{
		Pawn->ReceiveControllerChangedDelegate.RemoveDynamic(this, &ThisClass::OnControllerChanged);
		Pawn->OnEndPlay.RemoveDynamic(this, &ThisClass::OnPawnEndPlay);
	}
	// Retain the original identity for callbacks already copied by native multicast dispatch.
	OriginalSubsystem.Reset();
	OriginalPawn.Reset();
}

void UGGYGOMovementInputRouteObserver::OnMappingsRebuilt()
{
	if (UGGYGOPlayerInput* Producer = Cast<UGGYGOPlayerInput>(OriginalSession.Producer.Get()))
	{
		Producer->EndMovementInputSession(OriginalSession, TEXT("NativeMappingsRebuilt"));
	}
}

void UGGYGOMovementInputRouteObserver::OnControllerChanged(APawn* Pawn, AController* OldController, AController* NewController)
{
	if (UGGYGOPlayerInput* Producer = Cast<UGGYGOPlayerInput>(OriginalSession.Producer.Get()))
	{
		Producer->EndMovementInputSession(OriginalSession, TEXT("ControllerChanged"));
	}
}

void UGGYGOMovementInputRouteObserver::OnPawnEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason)
{
	if (UGGYGOPlayerInput* Producer = Cast<UGGYGOPlayerInput>(OriginalSession.Producer.Get()))
	{
		Producer->EndMovementInputSession(OriginalSession, TEXT("PawnEndPlay"));
	}
}

bool UGGYGOPlayerInput::FRouteMapping::Matches(const FRouteMapping& Other) const
{
	if (Key != Other.Key || Objects.Num() != Other.Objects.Num()
		|| MappingModifierCount != Other.MappingModifierCount || MappingTriggerCount != Other.MappingTriggerCount
		|| ActionModifierCount != Other.ActionModifierCount || ActionTriggerCount != Other.ActionTriggerCount)
	{
		return false;
	}
	for (int32 Index = 0; Index < Objects.Num(); ++Index)
	{
		if (!Objects[Index].IsValid() || !Objects[Index].HasSameIndexAndSerialNumber(Other.Objects[Index]))
		{
			return false;
		}
	}
	return true;
}

FString UGGYGOPlayerInput::MakeError(const FString& Reason) const
{
	return FString::Printf(TEXT("[Input.MovementSource] PlayerInput=%s Controller=%s Pawn=%s Component=%s Action=%s: %s"),
		*GetPathNameSafe(this), *GetPathNameSafe(GetOuter()), *GetPathNameSafe(SessionPawn.Get()),
		*GetPathNameSafe(SessionComponent.Get()), *GetPathNameSafe(SessionAction.Get()), *Reason);
}

void UGGYGOPlayerInput::ReportOnce(FName Reason, const FString& Detail)
{
	if (!ReportedReasons.Contains(Reason))
	{
		ReportedReasons.Add(Reason);
		UE_LOG(LogGGYGOMovementInput, Warning, TEXT("%s Reason=%s"), *MakeError(Detail), *Reason.ToString());
	}
}

bool UGGYGOPlayerInput::AllocateSerial(uint64& Counter, uint64& OutSerial)
{
	OutSerial = 0;
	if (bSerialExhausted || Counter == MAX_uint64)
	{
		bSerialExhausted = true;
		ReportOnce(TEXT("SerialExhausted"), TEXT("Source serial exhausted; this producer cannot issue further identities."));
		return false;
	}
	OutSerial = ++Counter;
	return true;
}

bool UGGYGOPlayerInput::ReadRoute(const UInputAction* Action, TArray<FRouteMapping>& OutMappings, FString& OutError) const
{
	OutMappings.Reset();
	if (!IsValid(Action) || Action->ValueType != EInputActionValueType::Axis2D)
	{
		OutError = MakeError(FString::Printf(TEXT("Movement Action=%s must be a valid Axis2D action."), *GetPathNameSafe(Action)));
		return false;
	}
	for (const FEnhancedActionKeyMapping& Mapping : GetEnhancedActionMappingsView())
	{
		if (Mapping.Action != Action)
		{
			continue;
		}
		FRouteMapping Route;
		Route.Key = Mapping.Key.GetVirtualKey();
		Route.MappingModifierCount = Mapping.Modifiers.Num();
		Route.MappingTriggerCount = Mapping.Triggers.Num();
		Route.ActionModifierCount = Action->Modifiers.Num();
		Route.ActionTriggerCount = Action->Triggers.Num();
		// AnyKey substitution, touch/gesture and relative mouse motion do not expose a
		// complete physical held/release sequence through this InputKey extension point.
		if (!Route.Key.IsValid() || Route.Key == EKeys::AnyKey || Route.Key.IsTouch()
			|| Route.Key.IsGesture() || (Route.Key.GetMenuCategory() == EKeys::NAME_MouseCategory && !Route.Key.IsMouseButton()))
		{
			OutMappings.Reset();
			OutError = MakeError(FString::Printf(TEXT("Action=%s has unsupported physical source Key=%s."),
				*GetPathNameSafe(Action), *Route.Key.ToString()));
			return false;
		}
		auto AppendObjects = [&Route](const auto& Objects)
		{
			for (const auto& Object : Objects)
			{
				if (!IsValid(Object.Get()))
				{
					return false;
				}
				Route.Objects.Add(Object.Get());
			}
			return true;
		};
		if (!AppendObjects(Mapping.Modifiers) || !AppendObjects(Mapping.Triggers)
			|| !AppendObjects(Action->Modifiers) || !AppendObjects(Action->Triggers))
		{
			OutMappings.Reset();
			OutError = MakeError(FString::Printf(TEXT("Action=%s Key=%s has an invalid modifier/trigger dependency."),
				*GetPathNameSafe(Action), *Route.Key.ToString()));
			return false;
		}
		OutMappings.Add(MoveTemp(Route));
	}
	if (OutMappings.IsEmpty())
	{
		OutError = MakeError(FString::Printf(TEXT("Action=%s has no actual enhanced mappings."), *GetPathNameSafe(Action)));
		return false;
	}
	OutError.Reset();
	return true;
}

bool UGGYGOPlayerInput::IsCurrentSession(const FGGYGOMovementInputSessionIdentity& Session) const
{
	// CurrentSession was issued by this object. Weak identity comparison still works
	// during destruction, when weak Get() may already be null but cleanup is required.
	return Session.SessionSerial != 0 && Session == CurrentSession;
}

bool UGGYGOPlayerInput::ValidateCurrentRoute(FString& OutError) const
{
	const APawn* Pawn = SessionPawn.Get();
	const APlayerController* Controller = GetOuterAPlayerController();
	const ULocalPlayer* LocalPlayer = Controller ? Controller->GetLocalPlayer() : nullptr;
	if (bEndingInput || bInsideFlush || bSerialExhausted || !IsValid(Controller) || Controller->PlayerInput != this
		|| !IsValid(Pawn) || Controller->IsActorBeingDestroyed() || Pawn->IsActorBeingDestroyed()
		|| Controller->GetPawn() != Pawn || Pawn->GetController() != Controller
		|| !Pawn->IsLocallyControlled() || !Pawn->InputEnabled() || Pawn->IsMoveInputIgnored()
		|| !SessionComponent.IsValid() || Pawn->InputComponent != SessionComponent.Get()
		|| !IsValid(LocalPlayer) || !SessionSubsystem.IsValid()
		|| LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() != SessionSubsystem.Get()
		|| SessionSubsystem->GetPlayerInput() != this
		|| (ReceiverBinding.ConsumerBindingSerial != 0 && (!FactReceiver.IsBound() || !ReceiverBinding.Consumer.IsValid())))
	{
		OutError = MakeError(TEXT("Original Pawn/Controller/PlayerInput/InputComponent route is no longer available for movement."));
		return false;
	}
	if (!ValidateOriginalOrigin(OutError))
	{
		return false;
	}
	TArray<FRouteMapping> ActualMappings;
	if (!ReadRoute(SessionAction.Get(), ActualMappings, OutError))
	{
		return false;
	}
	if (ActualMappings.Num() != RouteMappings.Num())
	{
		OutError = MakeError(TEXT("Actual movement mapping count changed."));
		return false;
	}
	for (int32 Index = 0; Index < RouteMappings.Num(); ++Index)
	{
		if (!RouteMappings[Index].Matches(ActualMappings[Index]))
		{
			OutError = MakeError(FString::Printf(TEXT("Actual movement mapping/dependency changed at Key=%s."),
				*RouteMappings[Index].Key.ToString()));
			return false;
		}
	}
	OutError.Reset();
	return true;
}

TWeakObjectPtr<UGGYGOMovementInputOriginResource> UGGYGOPlayerInput::SnapshotOriginForCleanup() const
{
	if (!ProducerOriginResource.IsExplicitlyNull())
	{
		return ProducerOriginResource;
	}
	// Before any session, capture only this lifecycle entry's host for exact retirement.
	// Retaining ownership still requires an authenticated Cold query; this grants no qualification.
	const APlayerController* Controller = Cast<APlayerController>(GetOuter());
	if (!Controller || Controller->PlayerInput != this)
	{
		// Native construction/key-map initialization precedes registration and slot assignment.
		return {};
	}
	const UGGYGOLocalPlayer* Host = Controller ? Cast<UGGYGOLocalPlayer>(Controller->GetLocalPlayer()) : nullptr;
	return Host && !Host->IsTemplate()
		? TWeakObjectPtr<UGGYGOMovementInputOriginResource>(Host->GetMovementInputOriginResource())
		: TWeakObjectPtr<UGGYGOMovementInputOriginResource>();
}

bool UGGYGOPlayerInput::CaptureOriginalOrigin(FString& OutError)
{
	if (ProducerOriginResource.IsExplicitlyNull())
	{
		const TWeakObjectPtr<UGGYGOMovementInputOriginResource> Candidate = SnapshotOriginForCleanup();
		UGGYGOMovementInputOriginResource* Resource = Candidate.Get();
		if (!Resource || Resource->IsTemplate())
		{
			OutError = MakeError(TEXT("Original registered OriginResource is missing or invalid; no qualification is adopted."));
			return false;
		}
		const EGGYGOMovementInputOriginQualification Qualification = Resource->GetQualification(this);
		if (Qualification != EGGYGOMovementInputOriginQualification::Cold)
		{
			OutError = MakeError(FString::Printf(TEXT("OriginResource=%s Qualification=%u: no original Cold observation ownership or cross-producer handoff proof."),
				*GetPathNameSafe(Resource), static_cast<uint32>(Qualification)));
			return false;
		}
		ProducerOriginResource = Candidate;
	}
	return ValidateOriginalOrigin(OutError);
}

bool UGGYGOPlayerInput::ValidateOriginalOrigin(FString& OutError) const
{
	const APlayerController* Controller = Cast<APlayerController>(GetOuter());
	const UGGYGOLocalPlayer* Host = Controller ? Cast<UGGYGOLocalPlayer>(Controller->GetLocalPlayer()) : nullptr;
	const UGGYGOMovementInputOriginResource* Resource = ProducerOriginResource.Get();
	if (!IsValid(Controller) || Controller->IsActorBeingDestroyed() || Controller->PlayerInput != this
		|| !IsValid(Host) || Host->IsTemplate() || Host->PlayerController != Controller
		|| !IsValid(Resource) || Resource->IsTemplate() || Resource->GetOuter() != Host
		|| !ProducerOriginResource.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<UGGYGOMovementInputOriginResource>(Host->GetMovementInputOriginResource())))
	{
		OutError = MakeError(FString::Printf(TEXT("Original OriginResource=%s Controller/LocalPlayer relationship changed; no successor is adopted."),
			*GetPathNameSafe(Resource)));
		return false;
	}
	const EGGYGOMovementInputOriginQualification Qualification = Resource->GetQualification(this);
	if (Qualification == EGGYGOMovementInputOriginQualification::Unavailable
		|| (bSessionInitialClaimPending && Qualification != EGGYGOMovementInputOriginQualification::Cold))
	{
		OutError = MakeError(FString::Printf(TEXT("Original OriginResource=%s Qualification=%u no longer supports the original registration/claim."),
			*GetPathNameSafe(Resource), static_cast<uint32>(Qualification)));
		return false;
	}
	OutError.Reset();
	return true;
}

void UGGYGOPlayerInput::RetireOriginalQualification(FName Reason, bool bEstablishBarrier)
{
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource = SnapshotOriginForCleanup();
	bSessionInitialClaimPending = false;
	if (bEstablishBarrier)
	{
		EstablishMovementReleaseBarrier(Reason);
	}
	if (!IsTemplate())
	{
		if (UGGYGOMovementInputOriginResource* Resource = OriginalResource.Get())
		{
			if (ProducerOriginResource.IsExplicitlyNull()
				&& Resource->GetQualification(this) == EGGYGOMovementInputOriginQualification::Cold)
			{
				// A registered producer can lose Cold before its first Begin (for example, flush).
				// Retain only that original ownership, so its real release/press can later Rearm.
				ProducerOriginResource = OriginalResource;
			}
			// The resource authenticates this exact producer, even during producer destruction.
			Resource->RetireInitialQualification(this, Reason);
		}
		else if (!OriginalResource.IsExplicitlyNull())
		{
			ReportOnce(TEXT("OriginalOriginLostDuringRetirement"), TEXT("The captured resource expired; its own lifecycle cleanup owns retirement, no replacement is queried."));
		}
	}
}

const UGGYGOPlayerInput::FPhysicalSource* UGGYGOPlayerInput::FindPhysicalSource(const FKey& Key, FInputDeviceId Device) const
{
	return PhysicalSources.FindByPredicate([&](const FPhysicalSource& Source)
	{
		return Source.Key == Key && Source.Device == Device;
	});
}

const UGGYGOPlayerInput::FMovementSourceObligation* UGGYGOPlayerInput::FindMovementSourceObligation(const FPhysicalSource& Source) const
{
	return MovementSourceObligations.FindByPredicate([&](const FMovementSourceObligation& Obligation)
	{
		return Obligation.Key == Source.Key && Obligation.Device == Source.Device;
	});
}

UGGYGOPlayerInput::FMovementSourceObligation& UGGYGOPlayerInput::FindOrAddMovementSourceObligation(const FPhysicalSource& Source)
{
	if (FMovementSourceObligation* Existing = MovementSourceObligations.FindByPredicate([&](const FMovementSourceObligation& Obligation)
	{
		return Obligation.Key == Source.Key && Obligation.Device == Source.Device;
	}))
	{
		return *Existing;
	}
	FMovementSourceObligation& Added = MovementSourceObligations.AddDefaulted_GetRef();
	Added.Key = Source.Key;
	Added.Device = Source.Device;
	return Added;
}

bool UGGYGOPlayerInput::IsMovementReleaseRequired(const FPhysicalSource& Source) const
{
	const FMovementSourceObligation* Obligation = FindMovementSourceObligation(Source);
	return Obligation && Obligation->ReleaseAfterObservationRevision.IsSet()
		&& Source.LastRealReleaseProofStartRevision <= Obligation->ReleaseAfterObservationRevision.GetValue();
}

bool UGGYGOPlayerInput::HasParticipatingMovementSources() const
{
	return MovementSourceObligations.ContainsByPredicate([](const FMovementSourceObligation& Obligation) { return Obligation.bParticipant; });
}

UGGYGOPlayerInput::ESourceProof UGGYGOPlayerInput::ReadSourceProof() const
{
	bool bAnyParticipant = false;
	bool bAnyHeld = false;
	for (const FMovementSourceObligation& Obligation : MovementSourceObligations)
	{
		if (!Obligation.bParticipant)
		{
			continue;
		}
		bAnyParticipant = true;
		const FPhysicalSource* Source = FindPhysicalSource(Obligation.Key, Obligation.Device);
		if (!Source || !Source->Device.IsValid() || Source->bUnproven || Source->HasNativeReleaseGap()
			|| IsMovementReleaseRequired(*Source)
			|| (Source->Key.IsAnalog() && Source->ObservedAxes != RequiredAxes(Source->Key)))
		{
			return ESourceProof::Unknown;
		}
		// Native per-key level is read from its sole owner, never copied into the obligation.
		bAnyHeld |= Source->Key.IsAnalog() ? Source->RawValue.SizeSquared() > 0 : Source->bDigitalDown;
	}
	return !bAnyParticipant ? ESourceProof::Unknown : bAnyHeld ? ESourceProof::Held : ESourceProof::Neutral;
}

FString UGGYGOPlayerInput::DescribeSourceProof() const
{
	FString Detail;
	for (const FMovementSourceObligation& Obligation : MovementSourceObligations)
	{
		if (!Obligation.bParticipant)
		{
			continue;
		}
		const FPhysicalSource* Source = FindPhysicalSource(Obligation.Key, Obligation.Device);
		if (!Source)
		{
			Detail += FString::Printf(TEXT(" Key=%s Device=%d NativeSourceMissing;"), *Obligation.Key.ToString(), Obligation.Device.GetId());
			continue;
		}
		const bool bMovementReleaseRequired = IsMovementReleaseRequired(*Source);
		const FName Reason = bMovementReleaseRequired && !Obligation.ReleaseReason.IsNone()
			? Obligation.ReleaseReason : Source->UnprovenReason;
		Detail += FString::Printf(TEXT(" Key=%s Device=%d Mapped=%d Held=%d Axes=%u/%u RequiresRelease=%d Unproven=%s NativeGap=%s;"),
			*Source->Key.ToString(), Source->Device.GetId(), IsMappedMovementSource(Source->Key),
			Source->Key.IsAnalog() ? Source->RawValue.SizeSquared() > 0 : Source->bDigitalDown,
			Source->ObservedAxes, RequiredAxes(Source->Key), bMovementReleaseRequired || Source->HasNativeReleaseGap(),
			*Reason.ToString(), *Source->UnprovenReason.ToString());
	}
	return Detail.IsEmpty() ? TEXT(" No actual participating source has a physical proof.") : Detail;
}

bool UGGYGOPlayerInput::IsMappedMovementSource(const FKey& Key) const
{
	return RouteMappings.ContainsByPredicate([&Key](const FRouteMapping& Mapping) { return Mapping.Key == Key; });
}

void UGGYGOPlayerInput::AdoptKnownMovementSources()
{
	for (const FPhysicalSource& Source : PhysicalSources)
	{
		if (!IsMappedMovementSource(Source.Key))
		{
			// Retain an old mapping's participant until its exact real release is proved.
			continue;
		}
		FMovementSourceObligation& Obligation = FindOrAddMovementSourceObligation(Source);
		if (!Obligation.bParticipant)
		{
			const bool bHeld = Source.Key.IsAnalog() ? Source.RawValue.SizeSquared() > 0 : Source.bDigitalDown;
			if (bHeld || Source.bUnproven || Source.HasNativeReleaseGap() || IsMovementReleaseRequired(Source) || !Source.Device.IsValid())
			{
				Obligation.ReleaseAfterObservationRevision = ObservationRevision;
				if (Obligation.ReleaseReason.IsNone())
				{
					Obligation.ReleaseReason = !Source.UnprovenReason.IsNone() ? Source.UnprovenReason
						: bHeld ? FName(TEXT("HeldBeforeSession")) : FName(TEXT("ExistingSourceProofGap"));
				}
			}
			Obligation.bParticipant = true;
		}
	}
}

void UGGYGOPlayerInput::EstablishMovementReleaseBarrier(FName Reason)
{
	for (const FPhysicalSource& Source : PhysicalSources)
	{
		// Even a later mapped source must prove a complete real release after this boundary.
		FMovementSourceObligation& Obligation = FindOrAddMovementSourceObligation(Source);
		Obligation.ReleaseAfterObservationRevision = ObservationRevision;
		Obligation.ReleaseReason = Reason;
	}
}

void UGGYGOPlayerInput::InvalidatePhysicalInputObservations(FName Reason)
{
	if (PhysicalSources.IsEmpty())
	{
		return;
	}
	uint64 GapRevision = 0;
	const bool bHasRevision = AllocateSerial(ObservationRevision, GapRevision);
	for (FPhysicalSource& Source : PhysicalSources)
	{
		// A real native flush loses continuity, not the recorded historical raw level.
		Source.NativeProofGapRevision = bHasRevision ? GapRevision : ObservationRevision;
		Source.bUnproven = true;
		Source.ObservedAxes = 0;
		for (uint64& AxisRevision : Source.AxisObservationRevisions)
		{
			AxisRevision = 0;
		}
		Source.UnprovenReason = Reason;
	}
}

bool UGGYGOPlayerInput::BeginMovementInputSession(APawn* Pawn, UInputComponent* Component, const UInputAction* Action,
	FGGYGOMovementInputSessionIdentity& OutSession, FString& OutError)
{
	OutSession = {};
	if (CurrentSession.SessionSerial != 0 || bEndingInput || bInsideFlush || bSerialExhausted)
	{
		OutError = MakeError(TEXT("An existing session must be ended, or the producer is unavailable."));
		if (CurrentSession.SessionSerial == 0 || bSessionInitialClaimPending)
		{
			RetireOriginalQualification(TEXT("SessionBeginRejected"), true);
		}
		return false;
	}
	if (!CaptureOriginalOrigin(OutError))
	{
		ReportOnce(TEXT("SessionOriginRejected"), OutError);
		RetireOriginalQualification(TEXT("SessionOriginRejected"), true);
		return false;
	}
	const APlayerController* Controller = GetOuterAPlayerController();
	ULocalPlayer* LocalPlayer = Controller ? Controller->GetLocalPlayer() : nullptr;
	UEnhancedInputLocalPlayerSubsystem* Subsystem = LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	if (!IsValid(Pawn) || !IsValid(Component) || !IsValid(Controller) || Controller->PlayerInput != this
		|| Controller->IsActorBeingDestroyed() || Pawn->IsActorBeingDestroyed()
		|| Controller->GetPawn() != Pawn || Pawn->GetController() != Controller || !Pawn->IsLocallyControlled()
		|| Pawn->InputComponent != Component || !Pawn->InputEnabled() || Pawn->IsMoveInputIgnored()
		|| !IsValid(Subsystem) || Subsystem->GetPlayerInput() != this)
	{
		OutError = MakeError(FString::Printf(TEXT("Cannot prepare original route Pawn=%s Component=%s Action=%s."),
			*GetPathNameSafe(Pawn), *GetPathNameSafe(Component), *GetPathNameSafe(Action)));
		RetireOriginalQualification(TEXT("SessionRouteRejected"), true);
		return false;
	}
	TArray<FRouteMapping> Mappings;
	if (!ReadRoute(Action, Mappings, OutError))
	{
		RetireOriginalQualification(TEXT("SessionMappingsRejected"), true);
		return false;
	}
	uint64 Serial = 0;
	if (!AllocateSerial(LastSessionSerial, Serial))
	{
		OutError = MakeError(TEXT("Session serial cannot be issued."));
		RetireOriginalQualification(TEXT("SessionSerialExhausted"), true);
		return false;
	}
	SessionPawn = Pawn;
	SessionComponent = Component;
	SessionAction = Action;
	SessionSubsystem = Subsystem;
	RouteMappings = MoveTemp(Mappings);
	AdoptKnownMovementSources();
	CurrentSession.Producer = this;
	CurrentSession.SessionSerial = Serial;
	const EGGYGOMovementInputOriginQualification Qualification = ProducerOriginResource->GetQualification(this);
	if (Qualification == EGGYGOMovementInputOriginQualification::Cold)
	{
		CurrentSessionMode = EGGYGOMovementInputSessionMode::Cold;
		if (!ProducerOriginResource->ClaimInitialQualification(this, CurrentSession, OutError))
		{
			EndMovementInputSession(CurrentSession, TEXT("SessionClaimRejected"));
			return false;
		}
		bSessionInitialClaimPending = true;
	}
	else if (Qualification == EGGYGOMovementInputOriginQualification::Rearm)
	{
		CurrentSessionMode = EGGYGOMovementInputSessionMode::Rearm;
	}
	else
	{
		OutError = MakeError(TEXT("Original qualification became unavailable before session claim; no mode is selected."));
		EndMovementInputSession(CurrentSession, TEXT("SessionQualificationLost"));
		return false;
	}
	RouteObserver = NewObject<UGGYGOMovementInputRouteObserver>(this);
	RouteObserver->Initialize(Subsystem, Pawn, CurrentSession);
	PreparedObservationRevision = ObservationRevision;
	ReportedReasons.Reset();
	OutSession = CurrentSession;
	OutError.Reset();
	return true;
}

bool UGGYGOPlayerInput::AttachMovementInputReceiver(const FGGYGOMovementInputSessionIdentity& Session,
	const FGGYGOMovementInputConsumerBindingId& Binding, FGGYGOMovementInputFactDelegate Receiver, FString& OutError)
{
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	const FString OriginalProducerPath = GetPathName();
	if (!IsCurrentSession(Session))
	{
		OutError = MakeError(TEXT("Attach refers to a stale or foreign source session."));
		return false;
	}
	OutError.Reset();
	// A repeated attach cannot replace an installed receiver or tear down its binding.
	if (ReceiverBinding.ConsumerBindingSerial != 0)
	{
		OutError = MakeError(TEXT("The source session already has a receiver."));
		if (bSessionInitialClaimPending)
		{
			RetireOriginalQualification(TEXT("ReceiverAttachRejected"), true);
		}
		return false;
	}
	if (!Binding.Consumer.IsValid() || Binding.ConsumerBindingSerial == 0 || Binding.SourceSession != Session
		|| !Receiver.IsBound() || ObservationRevision != PreparedObservationRevision || !ValidateCurrentRoute(OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = MakeError(TEXT("Attach binding/delegate invalid or physical observation changed after Begin."));
		}
		EndMovementInputSession(Session, TEXT("AttachRejected"));
		return false;
	}
	ReceiverBinding = Binding;
	FactReceiver = MoveTemp(Receiver);
	if (!Publish(EGGYGOMovementInputFactKind::SessionOpened, 0, NAME_None, Session))
	{
		OutError = FString::Printf(TEXT("[Input.Source] Producer=%s: original session ended or event issuance failed during SessionOpened."), *OriginalProducerPath);
		if (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
		{
			Producer->EndMovementInputSession(Session, TEXT("AttachInterrupted"));
		}
		return false;
	}
	if (ObservationRevision != PreparedObservationRevision || !ValidateCurrentRoute(OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = MakeError(TEXT("Physical observation changed during SessionOpened callback."));
		}
		EndMovementInputSession(Session, TEXT("AttachInterrupted"));
		return false;
	}
	const ESourceProof Proof = ReadSourceProof();
	bNeutralConfirmed = Proof == ESourceProof::Neutral;
	if (Proof == ESourceProof::Unknown && !HasParticipatingMovementSources())
	{
		// Waiting for the first actual source is neither neutral nor an observation gap.
		OutError.Reset();
		return true;
	}
	const bool bPublished = bNeutralConfirmed
		? Publish(EGGYGOMovementInputFactKind::NeutralConfirmed, 0, NAME_None, Session)
		: Publish(EGGYGOMovementInputFactKind::SourceUnresolved, 0,
			Proof == ESourceProof::Held ? FName(TEXT("HeldAtSessionStart")) : FName(TEXT("SourceProofUnavailable")), Session);
	if (!bPublished)
	{
		OutError = FString::Printf(TEXT("[Input.Source] Producer=%s: original session ended or event issuance failed during initial proof."), *OriginalProducerPath);
		if (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
		{
			Producer->EndMovementInputSession(Session, TEXT("AttachInterrupted"));
		}
		return false;
	}
	OutError.Reset();
	return true;
}

bool UGGYGOPlayerInput::IssuePhysicalRequest(EGGYGOMovementInputStartProof Proof,
	const FGGYGOMovementInputSessionIdentity& ExpectedSession)
{
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	const TWeakObjectPtr<UGGYGOMovementInputOriginResource> OriginalResource = ProducerOriginResource;
	FString Error;
	if (!IsCurrentSession(ExpectedSession) || ActiveRequestSerial != 0)
	{
		return false;
	}
	if ((Proof != EGGYGOMovementInputStartProof::ColdPhysicalPress
		&& Proof != EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress)
		|| !FactReceiver.IsBound() || !ReceiverBinding.Consumer.IsValid() || !ValidateCurrentRoute(Error))
	{
		ReportOnce(TEXT("PhysicalRequestPreparationRejected"), Error.IsEmpty() ? TEXT("Physical proof/receiver is invalid.") : Error);
		EndMovementInputSession(ExpectedSession, TEXT("PhysicalRequestPreparationRejected"));
		return false;
	}
	if (Proof == EGGYGOMovementInputStartProof::ColdPhysicalPress
		&& (!bSessionInitialClaimPending || CurrentSessionMode != EGGYGOMovementInputSessionMode::Cold))
	{
		EndMovementInputSession(ExpectedSession, TEXT("ColdRequestWithoutOriginalClaim"));
		return false;
	}
	FPendingFact Prepared;
	Prepared.Binding = ReceiverBinding;
	Prepared.Receiver = FactReceiver;
	Prepared.Fact.Request.Session = ExpectedSession;
	Prepared.Fact.Kind = EGGYGOMovementInputFactKind::RequestStarted;
	Prepared.Fact.StartProof = Proof;
	Prepared.Fact.SessionMode = EGGYGOMovementInputSessionMode::Invalid;
	if (!AllocateSerial(LastRequestSerial, Prepared.Fact.Request.RequestSerial)
		|| !AllocateSerial(LastEventSerial, Prepared.Fact.EventSerial))
	{
		EndMovementInputSession(ExpectedSession, TEXT("PhysicalRequestSerialExhausted"));
		return false;
	}
	const bool bConsumeOriginalClaim = bSessionInitialClaimPending;
	// Commit the real request before resource consumption. Do not expose it through Get yet.
	ActiveRequestSerial = Prepared.Fact.Request.RequestSerial;
	bRequestSourceUnresolved = true;
	bNeutralConfirmed = false;
	bSessionInitialClaimPending = false;
	if (bConsumeOriginalClaim)
	{
		UGGYGOMovementInputOriginResource* Resource = OriginalResource.Get();
		if (!Resource || !Resource->ConsumeInitialQualification(Prepared.Fact.Request, Error))
		{
			if (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
			{
				Producer->ReportOnce(TEXT("InitialQualificationConsumptionRejected"), Error.IsEmpty()
					? TEXT("Original resource expired after actual request issuance; Started is not published.") : Error);
				Producer->EndMovementInputSession(ExpectedSession, TEXT("InitialQualificationConsumptionRejected"));
			}
			return false;
		}
	}
	UGGYGOPlayerInput* Producer = OriginalProducer.Get();
	if (!Producer || !Producer->IsCurrentSession(ExpectedSession))
	{
		return false;
	}
	if (Producer->ReceiverBinding != Prepared.Binding || Producer->ActiveRequestSerial != Prepared.Fact.Request.RequestSerial
		|| !OriginalResource.HasSameIndexAndSerialNumber(Producer->ProducerOriginResource)
		|| !Producer->ValidateCurrentRoute(Error))
	{
		Producer->EndMovementInputSession(ExpectedSession, TEXT("PhysicalRequestCommitChanged"));
		return false;
	}
	Producer->bRequestSourceUnresolved = false;
	// Queue only the prepared original fact after successful consumption, before any callout.
	Producer->PendingFacts.Add(MoveTemp(Prepared));
	Producer->DeliverPendingFacts();
	Producer = OriginalProducer.Get();
	return Producer && !Producer->bEndingInput && Producer->IsCurrentSession(ExpectedSession);
}

bool UGGYGOPlayerInput::Publish(EGGYGOMovementInputFactKind Kind, uint64 RequestSerial, FName Reason,
	const FGGYGOMovementInputSessionIdentity& ExpectedSession)
{
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	if (!IsCurrentSession(ExpectedSession) || !FactReceiver.IsBound() || !ReceiverBinding.Consumer.IsValid())
	{
		EndMovementInputSession(ExpectedSession, TEXT("ReceiverUnavailable"));
		return false;
	}
	if (Kind == EGGYGOMovementInputFactKind::RequestStarted
		|| (Kind == EGGYGOMovementInputFactKind::SessionOpened
			&& CurrentSessionMode != EGGYGOMovementInputSessionMode::Cold
			&& CurrentSessionMode != EGGYGOMovementInputSessionMode::Rearm))
	{
		EndMovementInputSession(ExpectedSession, TEXT("ExplicitFactPreparationRequired"));
		return false;
	}
	if (Kind == EGGYGOMovementInputFactKind::SourceUnresolved)
	{
		RetireOriginalQualification(Reason);
		UGGYGOPlayerInput* Producer = OriginalProducer.Get();
		if (!Producer || !Producer->IsCurrentSession(ExpectedSession))
		{
			return false;
		}
	}
	FGGYGOMovementInputFact Fact;
	Fact.Request.Session = ExpectedSession;
	Fact.Request.RequestSerial = RequestSerial;
	Fact.Kind = Kind;
	Fact.Reason = Reason;
	Fact.SessionMode = Kind == EGGYGOMovementInputFactKind::SessionOpened
		? CurrentSessionMode : EGGYGOMovementInputSessionMode::Invalid;
	Fact.StartProof = EGGYGOMovementInputStartProof::Invalid;
	if (!AllocateSerial(LastEventSerial, Fact.EventSerial))
	{
		EndMovementInputSession(ExpectedSession, TEXT("SerialExhausted"));
		return false;
	}
	if (Kind == EGGYGOMovementInputFactKind::SourceUnresolved)
	{
		ReportOnce(Reason, TEXT("Physical source cannot identify a movement request; requires an observed neutral then a real press.")
			+ DescribeSourceProof());
	}
	FPendingFact& Pending = PendingFacts.AddDefaulted_GetRef();
	Pending.Binding = ReceiverBinding;
	Pending.Receiver = FactReceiver;
	Pending.Fact = Fact;
	DeliverPendingFacts();
	UGGYGOPlayerInput* Producer = OriginalProducer.Get();
	return Producer && !Producer->bEndingInput && Producer->IsCurrentSession(ExpectedSession);
}

void UGGYGOPlayerInput::DeliverPendingFacts()
{
	if (bEndingInput)
	{
		// Destruction can invalidate weak self before End publishes its terminal fact.
		// Move all issued copies out before callouts; never touch producer fields afterward.
		TArray<FPendingFact> TerminalFacts = MoveTemp(PendingFacts);
		for (const FPendingFact& Pending : TerminalFacts)
		{
			if (Pending.Receiver.IsBound() && Pending.Binding.Consumer.IsValid())
			{
				Pending.Receiver.Execute(Pending.Binding, Pending.Fact);
			}
		}
		return;
	}
	if (bDeliveringFacts || bBatchingFacts)
	{
		return;
	}
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	bDeliveringFacts = true;
	while (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
	{
		if (Producer->bEndingInput)
		{
			return;
		}
		if (Producer->PendingFacts.IsEmpty())
		{
			Producer->bDeliveringFacts = false;
			return;
		}
		// Remove before calling out; reentry appends after all previously issued facts.
		FPendingFact Pending = MoveTemp(Producer->PendingFacts[0]);
		Producer->PendingFacts.RemoveAt(0);
		if (Pending.Receiver.IsBound() && Pending.Binding.Consumer.IsValid())
		{
			Pending.Receiver.Execute(Pending.Binding, Pending.Fact);
		}
		else if (Producer->IsCurrentSession(Pending.Fact.Request.Session))
		{
			Producer->ReportOnce(TEXT("ReceiverUnavailable"), TEXT("Original receiver/consumer expired before synchronous fact delivery."));
			if (UGGYGOPlayerInput* CurrentProducer = OriginalProducer.Get())
			{
				CurrentProducer->EndMovementInputSession(Pending.Fact.Request.Session, TEXT("ReceiverUnavailable"));
			}
		}
	}
}

void UGGYGOPlayerInput::EndMovementInputSession(const FGGYGOMovementInputSessionIdentity& Session, FName Reason)
{
	if (!IsCurrentSession(Session))
	{
		return;
	}
	const FGGYGOMovementInputConsumerBindingId OriginalBinding = ReceiverBinding;
	const FGGYGOMovementInputFactDelegate OriginalReceiver = FactReceiver;
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	const bool bEndingAtEntry = bEndingInput;
	RetireOriginalQualification(Reason.IsNone() ? FName(TEXT("ExplicitSessionEnd")) : Reason);
	if (!bEndingAtEntry)
	{
		UGGYGOPlayerInput* Producer = OriginalProducer.Get();
		if (!Producer || !Producer->IsCurrentSession(Session))
		{
			return;
		}
	}
	else if (!IsCurrentSession(Session))
	{
		return;
	}
	if (!bInsideFlush)
	{
		EstablishMovementReleaseBarrier(Reason.IsNone() ? FName(TEXT("ExplicitSessionEnd")) : Reason);
	}
	FGGYGOMovementInputFact Fact;
	Fact.Request.Session = Session;
	Fact.Request.RequestSerial = ActiveRequestSerial;
	Fact.Kind = EGGYGOMovementInputFactKind::SessionInvalidated;
	Fact.Reason = Reason.IsNone() ? FName(TEXT("ExplicitSessionEnd")) : Reason;
	const bool bHasEvent = AllocateSerial(LastEventSerial, Fact.EventSerial);
	// Detach before calling out. Reentrant End(old) cannot remove a successor.
	FactReceiver.Unbind();
	ReceiverBinding = {};
	CurrentSession = {};
	CurrentSessionMode = EGGYGOMovementInputSessionMode::Invalid;
	if (RouteObserver)
	{
		RouteObserver->Detach();
		RouteObserver = nullptr;
	}
	SessionPawn.Reset();
	SessionComponent.Reset();
	SessionAction.Reset();
	SessionSubsystem.Reset();
	RouteMappings.Reset();
	ActiveRequestSerial = 0;
	bNeutralConfirmed = false;
	bRequestSourceUnresolved = false;
	if (bHasEvent && OriginalReceiver.IsBound())
	{
		FPendingFact& Pending = PendingFacts.AddDefaulted_GetRef();
		Pending.Binding = OriginalBinding;
		Pending.Receiver = OriginalReceiver;
		Pending.Fact = Fact;
		DeliverPendingFacts();
	}
}

bool UGGYGOPlayerInput::GetMovementInputRequest(const FGGYGOMovementInputSessionIdentity& Session,
	FGGYGOMovementInputRequestIdentity& OutRequest, FString& OutError) const
{
	OutRequest = {};
	if (!IsCurrentSession(Session))
	{
		OutError = MakeError(TEXT("Request lookup refers to a stale or foreign source session."));
		return false;
	}
	if (!ValidateCurrentRoute(OutError))
	{
		return false;
	}
	if (!FactReceiver.IsBound() || !ReceiverBinding.Consumer.IsValid() || ActiveRequestSerial == 0
		|| bRequestSourceUnresolved || ReadSourceProof() != ESourceProof::Held)
	{
		OutError = MakeError(TEXT("No attached, proven physical movement request is held in this session.") + DescribeSourceProof());
		return false;
	}
	OutRequest.Session = Session;
	OutRequest.RequestSerial = ActiveRequestSerial;
	OutError.Reset();
	return true;
}

UGGYGOPlayerInput::FNativePhysicalObservation UGGYGOPlayerInput::ObservePhysicalInput(const FInputKeyEventArgs& Params)
{
	FNativePhysicalObservation Observation;
	if (Params.IsSimulatedInput() || bInsideFlush || !Params.Key.IsValid())
	{
		return Observation;
	}
	uint64 Revision = 0;
	if (!AllocateSerial(ObservationRevision, Revision))
	{
		return Observation;
	}
	Observation.Revision = Revision;
	auto FindSource = [this, &Params](const FKey& Key) -> FPhysicalSource&
	{
		FPhysicalSource* Existing = PhysicalSources.FindByPredicate([&](const FPhysicalSource& Source)
		{
			return Source.Key == Key && Source.Device == Params.InputDevice;
		});
		if (Existing)
		{
			return *Existing;
		}
		FPhysicalSource& Added = PhysicalSources.AddDefaulted_GetRef();
		Added.Key = Key;
		Added.Device = Params.InputDevice;
		return Added;
	};
	auto AddSourceObservation = [&Observation, &Params](const FKey& Key) -> FNativePhysicalSourceObservation&
	{
		FNativePhysicalSourceObservation& Added = Observation.Sources.AddDefaulted_GetRef();
		Added.Key = Key;
		Added.Device = Params.InputDevice;
		return Added;
	};
	const FKey Key = Params.Key.GetVirtualKey();
	if (Key.IsDigital())
	{
		FPhysicalSource& Source = FindSource(Key);
		FNativePhysicalSourceObservation& SourceObservation = AddSourceObservation(Key);
		if (!Params.InputDevice.IsValid())
		{
			Source.bUnproven = true;
			Source.NativeProofGapRevision = Revision;
			Source.UnprovenReason = TEXT("InputDeviceUnresolved");
			return Observation;
		}
		// Native edge proof does not depend on a movement route or recovery obligation.
		const bool bCanRecordPressEdge = !Source.bDigitalDown && !Source.bUnproven && !Source.HasNativeReleaseGap();
		switch (Params.Event)
		{
		case IE_Pressed:
			SourceObservation.bRealHeldEdge = bCanRecordPressEdge;
			SourceObservation.bRealDigitalPress = bCanRecordPressEdge;
			Source.bDigitalDown = true;
			break;
		case IE_DoubleClick:
			Source.bDigitalDown = true;
			break;
		case IE_Released:
			Source.bDigitalDown = false;
			Source.bUnproven = false;
			Source.LastRealReleaseRevision = Revision;
			Source.LastRealReleaseProofStartRevision = Revision;
			Source.UnprovenReason = NAME_None;
			SourceObservation.bRealRelease = true;
			break;
		case IE_Repeat:
			if (!Source.bDigitalDown)
			{
				Source.UnprovenReason = TEXT("MissingPhysicalPress");
				Source.NativeProofGapRevision = Revision;
			}
			Source.bUnproven |= !Source.bDigitalDown;
			Source.bDigitalDown = true;
			break;
		default:
			Source.bUnproven = true;
			Source.NativeProofGapRevision = Revision;
			Source.UnprovenReason = TEXT("UnsupportedKeyEvent");
			break;
		}
		return Observation;
	}
	auto ObserveAxis = [&FindSource, &AddSourceObservation, &Params, Revision](const FKey& AxisKey, uint8 Mask, int32 Component)
	{
		FPhysicalSource& Source = FindSource(AxisKey);
		FNativePhysicalSourceObservation& SourceObservation = AddSourceObservation(AxisKey);
		if (!Params.InputDevice.IsValid() || Params.NumSamples != 1 || !FMath::IsFinite(Params.AmountDepressed)
			|| Mask == 0 || Component == INDEX_NONE)
		{
			Source.bUnproven = true;
			Source.NativeProofGapRevision = Revision;
			Source.ObservedAxes = 0;
			for (uint64& AxisRevision : Source.AxisObservationRevisions)
			{
				AxisRevision = 0;
			}
			Source.UnprovenReason = !Params.InputDevice.IsValid() ? FName(TEXT("InputDeviceUnresolved"))
				: Params.NumSamples != 1 ? FName(TEXT("MergedOrMissingAxisSamples"))
				: !FMath::IsFinite(Params.AmountDepressed) ? FName(TEXT("NonFiniteAxisSample"))
				: FName(TEXT("IncompleteVectorEvent"));
			return;
		}
		const bool bWasProvenNeutral = !Source.bUnproven && !Source.HasNativeReleaseGap()
			&& Source.ObservedAxes == RequiredAxes(AxisKey) && Source.RawValue.SizeSquared() == 0;
		Source.RawValue[Component] = Params.AmountDepressed;
		Source.AxisObservationRevisions[Component] = Revision;
		Source.ObservedAxes |= Mask;
		Source.bUnproven = Source.ObservedAxes != RequiredAxes(AxisKey);
		if (Source.bUnproven)
		{
			Source.NativeProofGapRevision = Revision;
			Source.UnprovenReason = TEXT("MissingAxisComponents");
		}
		else if (Source.RawValue.SizeSquared() == 0)
		{
			Source.LastRealReleaseRevision = Revision;
			Source.LastRealReleaseProofStartRevision = Revision;
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if ((RequiredAxes(AxisKey) & (1 << Axis)) != 0)
				{
					Source.LastRealReleaseProofStartRevision = FMath::Min(Source.LastRealReleaseProofStartRevision, Source.AxisObservationRevisions[Axis]);
				}
			}
			Source.UnprovenReason = NAME_None;
			SourceObservation.bRealRelease = true;
		}
		else if (!Source.HasNativeReleaseGap())
		{
			Source.UnprovenReason = NAME_None;
			SourceObservation.bRealHeldEdge = bWasProvenNeutral;
		}
	};
	// Paired scalar events remain the only supported complete vector observation.
	ObserveAxis(Key, Key.IsAxis1D() || Key.IsButtonAxis() ? 0b001 : 0, Key.IsAxis1D() || Key.IsButtonAxis() ? 0 : INDEX_NONE);
	const FKey Paired = Key.GetPairedAxisKey();
	if (Paired.IsValid())
	{
		const EPairedAxis Axis = Key.GetPairedAxis();
		const int32 Component = Axis == EPairedAxis::X ? 0 : Axis == EPairedAxis::Y ? 1 : Axis == EPairedAxis::Z ? 2 : INDEX_NONE;
		ObserveAxis(Paired, Component != INDEX_NONE ? uint8(1 << Component) : 0, Component);
	}
	return Observation;
}

UGGYGOPlayerInput::FPhysicalObservation UGGYGOPlayerInput::ProjectMovementObservation(
	const FNativePhysicalObservation& Native, ESourceProof Before, bool bHadParticipantsBefore)
{
	FPhysicalObservation Observation;
	Observation.Revision = Native.Revision;
	Observation.Before = Before;
	Observation.bHadParticipantsBefore = bHadParticipantsBefore;
	if (Native.Revision == 0)
	{
		return Observation;
	}
	for (const FNativePhysicalSourceObservation& SourceObservation : Native.Sources)
	{
		const FPhysicalSource* Source = FindPhysicalSource(SourceObservation.Key, SourceObservation.Device);
		check(Source); // The synchronous native observation just installed this exact record.
		const bool bMapped = IsMappedMovementSource(Source->Key);
		FMovementSourceObligation* Obligation = nullptr;
		if (bMapped || FindMovementSourceObligation(*Source))
		{
			Obligation = &FindOrAddMovementSourceObligation(*Source);
			Obligation->bParticipant |= bMapped;
		}
		const bool bRequiresRelease = IsMovementReleaseRequired(*Source);
		if (Obligation && SourceObservation.bRealRelease && !bMapped && !bRequiresRelease)
		{
			Obligation->bParticipant = false;
		}
		Observation.bRealHeldEdge |= bMapped && !bRequiresRelease && SourceObservation.bRealHeldEdge;
		Observation.bRealDigitalPress |= bMapped && !bRequiresRelease && SourceObservation.bRealDigitalPress;
	}
	Observation.After = ReadSourceProof();
	return Observation;
}

void UGGYGOPlayerInput::UpdateRequestFromSource(const FPhysicalObservation& Observation,
	const FGGYGOMovementInputSessionIdentity& ExpectedSession)
{
	if (!IsCurrentSession(ExpectedSession) || ReceiverBinding.ConsumerBindingSerial == 0)
	{
		return;
	}
	if (Observation.Revision == 0)
	{
		if (bSerialExhausted)
		{
			EndMovementInputSession(ExpectedSession, TEXT("SerialExhausted"));
		}
		return;
	}
	if (Observation.Revision != ObservationRevision)
	{
		ReportOnce(TEXT("NativeInputObservationChanged"), TEXT("Native input reentry changed the original physical observation; no later observation is adopted."));
		EndMovementInputSession(ExpectedSession, TEXT("NativeInputObservationChanged"));
		return;
	}
	FString RouteError;
	if (!ValidateCurrentRoute(RouteError))
	{
		ReportOnce(TEXT("RouteInvalidatedAfterNativeInput"), RouteError);
		EndMovementInputSession(ExpectedSession, TEXT("RouteInvalidatedAfterNativeInput"));
		return;
	}
	const ESourceProof Before = Observation.Before;
	const ESourceProof After = Observation.After;
	if (After == ESourceProof::Unknown)
	{
		const uint64 UnresolvedRequest = ActiveRequestSerial;
		const bool bActualGap = HasParticipatingMovementSources();
		const bool bPublish = UnresolvedRequest != 0 ? !bRequestSourceUnresolved
			: Before != After || bNeutralConfirmed || (bSessionInitialClaimPending && bActualGap);
		// Unknown revokes association, but the original request remains open so its
		// last real release can close the same identity at the consumer.
		bRequestSourceUnresolved |= UnresolvedRequest != 0;
		bNeutralConfirmed = false;
		if (bPublish && (bActualGap || UnresolvedRequest != 0))
		{
			Publish(EGGYGOMovementInputFactKind::SourceUnresolved, UnresolvedRequest, TEXT("SourceProofUnavailable"), ExpectedSession);
		}
		return;
	}
	if (After == ESourceProof::Neutral)
	{
		const uint64 ReleasedRequest = ActiveRequestSerial;
		ActiveRequestSerial = 0;
		bRequestSourceUnresolved = false;
		const bool bPublishNeutral = !bNeutralConfirmed;
		bNeutralConfirmed = true;
		{
			// Reserve both physical-release facts before any receiver can reenter with
			// a new press. This buffer has no Tick, timer or deferred frame execution.
			TGuardValue<bool> BatchGuard(bBatchingFacts, true);
			if (ReleasedRequest != 0)
			{
				Publish(EGGYGOMovementInputFactKind::RequestReleased, ReleasedRequest, NAME_None, ExpectedSession);
			}
			if (bPublishNeutral && IsCurrentSession(ExpectedSession))
			{
				Publish(EGGYGOMovementInputFactKind::NeutralConfirmed, 0, NAME_None, ExpectedSession);
			}
		}
		DeliverPendingFacts();
		return;
	}
	const bool bColdPhysicalPress = bSessionInitialClaimPending
		&& CurrentSessionMode == EGGYGOMovementInputSessionMode::Cold && Observation.bRealDigitalPress
		&& (Before == ESourceProof::Neutral || !Observation.bHadParticipantsBefore);
	const bool bReleasedThenPhysicalPress = bNeutralConfirmed && Before == ESourceProof::Neutral && Observation.bRealHeldEdge;
	if (ActiveRequestSerial == 0 && (bColdPhysicalPress || bReleasedThenPhysicalPress))
	{
		IssuePhysicalRequest(bColdPhysicalPress ? EGGYGOMovementInputStartProof::ColdPhysicalPress
			: EGGYGOMovementInputStartProof::ReleasedThenPhysicalPress, ExpectedSession);
	}
	else if (ActiveRequestSerial == 0)
	{
		const bool bPublish = bNeutralConfirmed || Before != After;
		bNeutralConfirmed = false;
		if (bPublish)
		{
			Publish(EGGYGOMovementInputFactKind::SourceUnresolved, 0, TEXT("HeldWithoutProvenPress"), ExpectedSession);
		}
	}
}

bool UGGYGOPlayerInput::InputKey(const FInputKeyEventArgs& Params)
{
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	const FString OriginalProducerPath = GetPathName();
	const FGGYGOMovementInputSessionIdentity OriginalSession = CurrentSession;
	FString Error;
	if (IsCurrentSession(OriginalSession) && !ValidateCurrentRoute(Error))
	{
		ReportOnce(TEXT("RouteInvalidated"), Error);
		EndMovementInputSession(OriginalSession, TEXT("RouteInvalidated"));
	}
	const ESourceProof Before = ReadSourceProof();
	const bool bHadParticipantsBefore = HasParticipatingMovementSources();
	const FNativePhysicalObservation NativeObservation = ObservePhysicalInput(Params);
	const FPhysicalObservation Observation = ProjectMovementObservation(NativeObservation, Before, bHadParticipantsBefore);
	// Native key processing completes before the source receiver can reenter InputKey.
	const bool bResult = Super::InputKey(Params);
	if (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
	{
		if (!Params.IsSimulatedInput() && !Producer->bInsideFlush && !Producer->bEndingInput)
		{
			Producer->UpdateRequestFromSource(Observation, OriginalSession);
		}
	}
	else
	{
		UE_LOG(LogGGYGOMovementInput, Warning, TEXT("[Input.PhysicalObservation] Producer=%s Reason=ProducerLostDuringNativeInput; no source fact is issued."),
			*OriginalProducerPath);
	}
	// Super's return value describes native handling, not source authenticity.
	return bResult;
}

void UGGYGOPlayerInput::FlushPressedKeys()
{
	const TWeakObjectPtr<UGGYGOPlayerInput> OriginalProducer(this);
	const bool bWasInsideFlush = bInsideFlush;
	bInsideFlush = true;
	RetireOriginalQualification(TEXT("InputFlushed"));
	InvalidatePhysicalInputObservations(TEXT("InputFlushedObservationGap"));
	EstablishMovementReleaseBarrier(TEXT("InputFlushedObservationGap"));
	EndMovementInputSession(CurrentSession, TEXT("InputFlushed"));
	if (!OriginalProducer.IsValid())
	{
		return;
	}
	Super::FlushPressedKeys();
	if (UGGYGOPlayerInput* Producer = OriginalProducer.Get())
	{
		if (!Producer->bEndingInput)
		{
			Producer->bInsideFlush = bWasInsideFlush;
		}
	}
}

void UGGYGOPlayerInput::EvaluateInputDelegates(const TArray<UInputComponent*>& InputComponentStack,
	float DeltaTime, bool bGamePaused, const TArray<TPair<FKey, FKeyState*>>& KeysWithEvents)
{
	const FGGYGOMovementInputSessionIdentity OriginalSession = CurrentSession;
	FString Error;
	if (IsCurrentSession(OriginalSession))
	{
		if (bGamePaused || !InputComponentStack.Contains(SessionComponent.Get()))
		{
			EndMovementInputSession(OriginalSession, bGamePaused ? FName(TEXT("InputPaused")) : FName(TEXT("InputComponentRemoved")));
		}
		else if (!ValidateCurrentRoute(Error))
		{
			ReportOnce(TEXT("RouteInvalidated"), Error);
			EndMovementInputSession(OriginalSession, TEXT("RouteInvalidated"));
		}
	}
	Super::EvaluateInputDelegates(InputComponentStack, DeltaTime, bGamePaused, KeysWithEvents);
}

void UGGYGOPlayerInput::EvaluateBlockedInputComponent(UInputComponent* Component)
{
	if (Component == SessionComponent.Get())
	{
		EndMovementInputSession(CurrentSession, TEXT("InputStackBlocked"));
	}
	Super::EvaluateBlockedInputComponent(Component);
}

void UGGYGOPlayerInput::BeginDestroy()
{
	bEndingInput = true;
	if (CurrentSession.SessionSerial == 0)
	{
		RetireOriginalQualification(TEXT("ProducerDestroyed"));
	}
	EndMovementInputSession(CurrentSession, TEXT("ProducerDestroyed"));
	ProducerOriginResource.Reset();
	MovementSourceObligations.Reset();
	PhysicalSources.Reset();
	Super::BeginDestroy();
}
