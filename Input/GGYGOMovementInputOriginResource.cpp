#include "Input/GGYGOMovementInputOriginResource.h"

#include "CoreGlobals.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/PlatformStackWalk.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMovementInputOrigin, Log, All);

FString UGGYGOMovementInputOriginResource::DescribeError(FName Reason, const FString& Detail) const
{
	return FString::Printf(TEXT("[Input.MovementOrigin] Resource=%s Player=%s Controller=%s Producer=%s Constructed=%s Scope=%llu Class=%s FirstRetirement=%s Reason=%s: %s"),
		*GetPathNameSafe(this), *GetPathNameSafe(GetOuter()),
		*GetPathNameSafe(CreationScope.Ticket.CreationSerial != 0 ? CreationScope.Controller.Get() : RegisteredController.Get()),
		*GetPathNameSafe(RegisteredProducer.Get()), *GetPathNameSafe(CreationScope.ConstructedProducer.Get()),
		static_cast<unsigned long long>(CreationScope.Ticket.CreationSerial),
		*GetPathNameSafe(CreationScope.ConfiguredClass.Get()), *FirstRetirementReason.ToString(), *Reason.ToString(), *Detail);
}

void UGGYGOMovementInputOriginResource::ReportOnce(FName Reason, const FString& Detail)
{
	ReportErrorOnce(Reason, DescribeError(Reason, Detail));
}

void UGGYGOMovementInputOriginResource::ReportErrorOnce(FName Reason, const FString& Error)
{
	if (!ReportedReasons.Contains(Reason))
	{
		ReportedReasons.Add(Reason);
		UE_LOG(LogGGYGOMovementInputOrigin, Warning, TEXT("%s"), *Error);
	}
}

bool UGGYGOMovementInputOriginResource::Reject(FName Reason, const FString& Detail, FString& OutError)
{
	OutError = DescribeError(Reason, Detail);
	ReportErrorOnce(Reason, OutError);
	return false;
}

bool UGGYGOMovementInputOriginResource::IsLifecycleCurrent() const
{
	return bLifecycleActive && !IsTemplate() && IsValid(this)
		&& OriginalPlayer.IsValid() && OriginalPlayer.Get() == GetOuter();
}

bool UGGYGOMovementInputOriginResource::IsControllerCurrent(const APlayerController* Controller) const
{
	return IsLifecycleCurrent() && IsValid(Controller) && !Controller->IsActorBeingDestroyed()
		&& Controller->GetLocalPlayer() == OriginalPlayer.Get()
		&& OriginalPlayer->PlayerController == Controller;
}

bool UGGYGOMovementInputOriginResource::IsRegisteredProducerCurrent(const UPlayerInput* Producer) const
{
	return IsValid(Producer) && RegisteredProducer.Get() == Producer
		&& IsControllerCurrent(RegisteredController.Get())
		&& Producer->GetOuter() == RegisteredController.Get()
		&& RegisteredController->PlayerInput == Producer;
}

bool UGGYGOMovementInputOriginResource::IsResourceTicket(const FCreationTicket& Ticket) const
{
	return Ticket.CreationSerial != 0
		&& Ticket.Resource.HasSameIndexAndSerialNumber(TWeakObjectPtr<UGGYGOMovementInputOriginResource>(
			const_cast<UGGYGOMovementInputOriginResource*>(this)));
}

bool UGGYGOMovementInputOriginResource::IsCurrentTicket(const FCreationTicket& Ticket) const
{
	return IsResourceTicket(Ticket) && Ticket.CreationSerial == CreationScope.Ticket.CreationSerial;
}

void UGGYGOMovementInputOriginResource::SealInitialQualification(FName Reason)
{
	FString FirstRetirementContext;
	if (FirstRetirementReason.IsNone())
	{
		if (UE_LOG_ACTIVE(LogGGYGOMovementInputOrigin, Verbose))
		{
			FirstRetirementContext = FString::Printf(TEXT("[Input.OriginFirstRetirement] Frame=%llu Resource=%s Reason=%s StageBefore=%u LifecycleActive=%d PlayerAddedHandled=%d\nOriginalLP=%s Valid=%d Stale=%d\nRegisteredPC=%s Valid=%d Stale=%d\nRegisteredProducer=%s Valid=%d Stale=%d\nCreationSerial=%llu LastClosedSerial=%llu ScopePC=%s ConfiguredClass=%s ConstructedProducer=%s HasRecordedProducer=%d CreationFailed=%d CreationFailureReason=%s ClaimedSession=%llu"),
				static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(this), *Reason.ToString(),
				static_cast<uint32>(QualificationStage), bLifecycleActive, bPlayerAddedHandled,
				*GetPathNameSafe(OriginalPlayer.Get()), OriginalPlayer.IsValid(), OriginalPlayer.IsStale(),
				*GetPathNameSafe(RegisteredController.Get()), RegisteredController.IsValid(), RegisteredController.IsStale(),
				*GetPathNameSafe(RegisteredProducer.Get()), RegisteredProducer.IsValid(), RegisteredProducer.IsStale(),
				static_cast<unsigned long long>(CreationScope.Ticket.CreationSerial),
				static_cast<unsigned long long>(LastClosedCreationSerial), *GetPathNameSafe(CreationScope.Controller.Get()),
				*GetPathNameSafe(CreationScope.ConfiguredClass.Get()), *GetPathNameSafe(CreationScope.ConstructedProducer.Get()),
				CreationScope.bHasRecordedProducer, CreationScope.bFailed, *CreationScope.FailureReason.ToString(),
				static_cast<unsigned long long>(ClaimedSession.SessionSerial));
		}
		FirstRetirementReason = Reason;
	}
	if (QualificationStage != EInitialQualificationStage::Consumed)
	{
		QualificationStage = EInitialQualificationStage::Retired;
	}
	if (!FirstRetirementContext.IsEmpty())
	{
		const uint32 StageAfter = static_cast<uint32>(QualificationStage);
		ANSICHAR Callstack[16384] = {};
		FPlatformStackWalk::StackWalkAndDump(Callstack, UE_ARRAY_COUNT(Callstack), 1);
		UE_LOG(LogGGYGOMovementInputOrigin, Verbose, TEXT("%s\nStageAfter=%u\nCallstack:\n%s"),
			*FirstRetirementContext, StageAfter, ANSI_TO_TCHAR(Callstack));
	}
}

void UGGYGOMovementInputOriginResource::SealCreationScope()
{
	if (CreationScope.Ticket.CreationSerial != 0)
	{
		LastClosedCreationSerial = CreationScope.Ticket.CreationSerial;
		CreationScope = {};
	}
}

bool UGGYGOMovementInputOriginResource::FailCurrentCreation(FName Reason, const FString& Detail, FString& OutError)
{
	if (!CreationScope.bFailed)
	{
		CreationScope.bFailed = true;
		CreationScope.FailureReason = Reason;
	}
	SealInitialQualification(Reason);
	return Reject(Reason, Detail, OutError);
}

bool UGGYGOMovementInputOriginResource::RejectAndCloseCreation(FName Reason, const FString& Detail, FString& OutError)
{
	// Clear the scope before logging, which is also an external call boundary.
	OutError = DescribeError(Reason, Detail);
	SealInitialQualification(Reason);
	SealCreationScope();
	ReportErrorOnce(Reason, OutError);
	return false;
}

bool UGGYGOMovementInputOriginResource::InitializeFromPlayerAdded(ULocalPlayer* Player,
	TWeakObjectPtr<APlayerController> BeforeSuper, FString& OutError)
{
	if (bPlayerAddedHandled || QualificationStage != EInitialQualificationStage::Unavailable)
	{
		SealInitialQualification(TEXT("RepeatedPlayerAdded"));
		SealCreationScope();
		return Reject(TEXT("RepeatedPlayerAdded"), TEXT("This resource cannot reopen an initial qualification after another lifecycle attempt."), OutError);
	}
	bPlayerAddedHandled = true;
	if (IsTemplate() || !IsValid(Player) || GetOuter() != Player || Player->IsTemplate())
	{
		SealInitialQualification(TEXT("InvalidPlayerAddedOwner"));
		return Reject(TEXT("InvalidPlayerAddedOwner"), TEXT("Real PlayerAdded requires a live, matching LocalPlayer outer and a non-template resource."), OutError);
	}
	OriginalPlayer = Player;
	bLifecycleActive = true;
	if (!BeforeSuper.IsExplicitlyNull() || Player->PlayerController != nullptr)
	{
		SealInitialQualification(TEXT("LatePlayerAdded"));
		return Reject(TEXT("LatePlayerAdded"), TEXT("A controller existed before or during PlayerAdded; initial input birth cannot be reconstructed."), OutError);
	}
	QualificationStage = EInitialQualificationStage::Available;
	OutError.Reset();
	return true;
}

void UGGYGOMovementInputOriginResource::NotifyPlayerRemoved(ULocalPlayer* Player)
{
	if (!Player || GetOuter() != Player || (!OriginalPlayer.IsExplicitlyNull()
		&& !OriginalPlayer.HasSameIndexAndSerialNumber(TWeakObjectPtr<ULocalPlayer>(Player))))
	{
		ReportOnce(TEXT("ForeignPlayerRemoved"), TEXT("Removal belongs to another LocalPlayer; the current resource is retained."));
		return;
	}
	bLifecycleActive = false;
	bPlayerAddedHandled = true;
	SealInitialQualification(TEXT("PlayerRemoved"));
	SealCreationScope();
}

void UGGYGOMovementInputOriginResource::NotifyControllerChanged(APlayerController* Controller)
{
	if (!IsLifecycleCurrent())
	{
		bLifecycleActive = false;
		SealInitialQualification(TEXT("LocalPlayerUnavailable"));
		SealCreationScope();
		return;
	}
	if (OriginalPlayer->PlayerController != Controller)
	{
		ReportOnce(TEXT("StaleControllerNotification"), TEXT("Notification no longer matches the native LocalPlayer controller; successor is retained."));
		return;
	}
	if (CreationScope.Ticket.CreationSerial != 0 && CreationScope.Controller.Get() == Controller
		&& IsControllerCurrent(Controller))
	{
		if (CreationScope.bHasRecordedProducer && (!CreationScope.ConstructedProducer.IsValid()
			|| (Controller->PlayerInput != nullptr && Controller->PlayerInput != CreationScope.ConstructedProducer.Get())))
		{
			FString Error;
			FailCurrentCreation(TEXT("ProducerReplacedDuringConstruction"),
				TEXT("Native confirmation observed a lost original witness or replacement input slot; the ticket cannot recover."), Error);
		}
		// Native confirmation during the exact pending scope does not grant birth.
		return;
	}
	if (IsControllerCurrent(Controller) && RegisteredController.Get() == Controller
		&& IsRegisteredProducerCurrent(RegisteredProducer.Get()))
	{
		return;
	}
	SealInitialQualification(TEXT("ControllerChangedOrUnregistered"));
	SealCreationScope();
	ReportOnce(TEXT("ControllerChangedOrUnregistered"), TEXT("A replacement/nonregistered controller or input object cannot obtain a new initial qualification."));
}

bool UGGYGOMovementInputOriginResource::BeginNativeCreation(APlayerController* Controller, UClass* ConfiguredClass,
	FCreationTicket& OutTicket, bool& OutNeedsCreation, FString& OutError)
{
	OutTicket = {};
	OutNeedsCreation = false;
	if (IsLifecycleCurrent() && OriginalPlayer->PlayerController != Controller)
	{
		return Reject(TEXT("ForeignNativeCreationController"),
			FString::Printf(TEXT("Controller=%s is not the current native controller; this call cannot affect its successor."),
				*GetPathNameSafe(Controller)), OutError);
	}
	if (CreationScope.Ticket.CreationSerial != 0)
	{
		return FailCurrentCreation(TEXT("NativeCreationBusy"), TEXT("Nested creation cannot replace the current synchronous ticket."), OutError);
	}
	if (!IsControllerCurrent(Controller))
	{
		SealInitialQualification(TEXT("InvalidNativeCreationController"));
		return Reject(TEXT("InvalidNativeCreationController"), FString::Printf(TEXT("Controller=%s is not the live original LocalPlayer's native owner."),
			*GetPathNameSafe(Controller)), OutError);
	}
	if (!IsValid(ConfiguredClass) || !ConfiguredClass->IsChildOf(UPlayerInput::StaticClass())
		|| ConfiguredClass->HasAnyClassFlags(CLASS_Abstract))
	{
		SealInitialQualification(TEXT("InvalidNativeInputClass"));
		return Reject(TEXT("InvalidNativeInputClass"), FString::Printf(TEXT("ConfiguredClass=%s must be an explicit concrete PlayerInput class."),
			*GetPathNameSafe(ConfiguredClass)), OutError);
	}
	if (UPlayerInput* Existing = Controller->PlayerInput)
	{
		if (IsRegisteredProducerCurrent(Existing) && Existing->GetClass() == ConfiguredClass)
		{
			OutError.Reset();
			return true;
		}
		SealInitialQualification(TEXT("ExistingUnregisteredProducer"));
		return Reject(TEXT("ExistingUnregisteredProducer"), FString::Printf(TEXT("ExistingInput=%s is not the exact registered producer/configuration; no birth is inferred."),
			*GetPathNameSafe(Existing)), OutError);
	}
	if (!RegisteredProducer.IsExplicitlyNull() || !RegisteredController.IsExplicitlyNull())
	{
		SealInitialQualification(TEXT("ProducerRecreated"));
	}
	if (LastCreationSerial == MAX_uint64)
	{
		SealInitialQualification(TEXT("CreationSerialExhausted"));
		return Reject(TEXT("CreationSerialExhausted"), TEXT("Creation resource serial cannot wrap or be reset."), OutError);
	}
	CreationScope.Ticket.Resource = this;
	CreationScope.Ticket.CreationSerial = ++LastCreationSerial;
	CreationScope.Controller = Controller;
	CreationScope.ConfiguredClass = ConfiguredClass;
	OutTicket = CreationScope.Ticket;
	OutNeedsCreation = true;
	OutError.Reset();
	return true;
}

bool UGGYGOMovementInputOriginResource::RecordNativeCreatedProducer(APlayerController* Controller,
	UPlayerInput* ConstructedProducer, FString& OutError)
{
	if (CreationScope.Ticket.CreationSerial == 0)
	{
		if (!IsLifecycleCurrent() || OriginalPlayer->PlayerController == Controller)
		{
			SealInitialQualification(TEXT("ConstructionOutsideNativeScope"));
		}
		return Reject(TEXT("ConstructionOutsideNativeScope"), TEXT("PostInitProperties has no explicit native creation ticket; no permission is issued."), OutError);
	}
	if (!Controller || !CreationScope.Controller.HasSameIndexAndSerialNumber(TWeakObjectPtr<APlayerController>(Controller)))
	{
		return Reject(TEXT("ForeignConstructionWitness"),
			TEXT("Construction belongs to a different controller and cannot change the original creation ticket."), OutError);
	}
	if (CreationScope.bFailed)
	{
		return Reject(CreationScope.FailureReason, TEXT("The current creation ticket already failed and cannot be repaired."), OutError);
	}
	if (!IsControllerCurrent(Controller) || CreationScope.Controller.Get() != Controller
		|| !IsValid(ConstructedProducer) || ConstructedProducer->IsTemplate()
		|| ConstructedProducer->GetOuter() != Controller || !CreationScope.ConfiguredClass.IsValid()
		|| ConstructedProducer->GetClass() != CreationScope.ConfiguredClass.Get())
	{
		return FailCurrentCreation(TEXT("InvalidConstructionWitness"), FString::Printf(TEXT("Controller=%s Constructed=%s does not match the ticket's original owner/configured class."),
			*GetPathNameSafe(Controller), *GetPathNameSafe(ConstructedProducer)), OutError);
	}
	if (CreationScope.bHasRecordedProducer)
	{
		if (!CreationScope.ConstructedProducer.HasSameIndexAndSerialNumber(TWeakObjectPtr<UPlayerInput>(ConstructedProducer)))
		{
			return FailCurrentCreation(TEXT("SecondConstructedProducer"), TEXT("A second object within one native ticket cannot replace the original construction witness."), OutError);
		}
		if (Controller->PlayerInput != nullptr && Controller->PlayerInput != ConstructedProducer)
		{
			return FailCurrentCreation(TEXT("ProducerReplacedDuringConstruction"), TEXT("The native input slot changed after the original construction witness."), OutError);
		}
		OutError.Reset();
		return true;
	}
	if (Controller->PlayerInput != nullptr)
	{
		return FailCurrentCreation(TEXT("LateConstructionWitness"), TEXT("The native input slot was already populated before its construction was recorded."), OutError);
	}
	CreationScope.ConstructedProducer = ConstructedProducer;
	CreationScope.bHasRecordedProducer = true;
	OutError.Reset();
	return true;
}

bool UGGYGOMovementInputOriginResource::CompleteNativeCreation(const FCreationTicket& Ticket,
	UPlayerInput* ActualProducer, FString& OutError)
{
	if (!IsCurrentTicket(Ticket))
	{
		return Reject(TEXT("StaleCreationCompletion"), TEXT("Completion does not name the current original resource ticket."), OutError);
	}
	if (CreationScope.bFailed)
	{
		return RejectAndCloseCreation(CreationScope.FailureReason, TEXT("The native scope failed; completion cannot repair it."), OutError);
	}
	if (!CreationScope.bHasRecordedProducer)
	{
		return RejectAndCloseCreation(TEXT("MissingConstructionWitness"), TEXT("No PostInitProperties registration occurred within the native ticket."), OutError);
	}
	const APlayerController* Controller = CreationScope.Controller.Get();
	if (!IsControllerCurrent(Controller) || !IsValid(ActualProducer) || ActualProducer->IsTemplate()
		|| !CreationScope.ConstructedProducer.IsValid()
		|| !CreationScope.ConstructedProducer.HasSameIndexAndSerialNumber(TWeakObjectPtr<UPlayerInput>(ActualProducer))
		|| ActualProducer->GetOuter() != Controller || Controller->PlayerInput != ActualProducer
		|| !CreationScope.ConfiguredClass.IsValid() || ActualProducer->GetClass() != CreationScope.ConfiguredClass.Get())
	{
		return RejectAndCloseCreation(TEXT("NativeCreationReturnMismatch"), FString::Printf(TEXT("ActualInput=%s is not the original recorded construction/owner/assigned slot."),
			*GetPathNameSafe(ActualProducer)), OutError);
	}
	RegisteredController = CreationScope.Controller;
	RegisteredProducer = ActualProducer;
	SealCreationScope();
	OutError.Reset();
	return true;
}

bool UGGYGOMovementInputOriginResource::CloseNativeCreation(const FCreationTicket& Ticket, FName Reason, FString& OutError)
{
	if (!IsResourceTicket(Ticket))
	{
		return Reject(TEXT("ForeignCreationClose"), TEXT("Cleanup requires an assigned ticket from this original resource."), OutError);
	}
	if (Ticket.CreationSerial <= LastClosedCreationSerial)
	{
		// This may be an old closed ticket while a successor is open: do not touch it.
		OutError.Reset();
		return true;
	}
	if (!IsCurrentTicket(Ticket))
	{
		return Reject(TEXT("StaleCreationClose"), TEXT("Cleanup cannot close a different current creation scope."), OutError);
	}
	SealInitialQualification(Reason.IsNone() ? FName(TEXT("MissingCreationCloseReason")) : Reason);
	SealCreationScope();
	if (Reason.IsNone())
	{
		return Reject(TEXT("MissingCreationCloseReason"), TEXT("The matching scope was safely aborted, but its required diagnostic reason was missing."), OutError);
	}
	OutError.Reset();
	return true;
}

EGGYGOMovementInputOriginQualification UGGYGOMovementInputOriginResource::GetQualification(const UPlayerInput* ExpectedProducer) const
{
	if (CreationScope.Ticket.CreationSerial != 0 || !IsRegisteredProducerCurrent(ExpectedProducer))
	{
		return EGGYGOMovementInputOriginQualification::Unavailable;
	}
	switch (QualificationStage)
	{
	case EInitialQualificationStage::Available:
	case EInitialQualificationStage::Claimed:
		return EGGYGOMovementInputOriginQualification::Cold;
	case EInitialQualificationStage::Consumed:
	case EInitialQualificationStage::Retired:
		return EGGYGOMovementInputOriginQualification::Rearm;
	default:
		return EGGYGOMovementInputOriginQualification::Unavailable;
	}
}

bool UGGYGOMovementInputOriginResource::ClaimInitialQualification(UPlayerInput* Producer,
	const FSession& Session, FString& OutError)
{
	if (GetQualification(Producer) != EGGYGOMovementInputOriginQualification::Cold
		|| Session.SessionSerial == 0 || !Session.Producer.IsValid() || Session.Producer.Get() != Producer)
	{
		return Reject(TEXT("InitialQualificationUnavailable"), TEXT("Claim requires the exact live registered producer, allocated source session and available initial qualification."), OutError);
	}
	if (QualificationStage == EInitialQualificationStage::Claimed)
	{
		if (ClaimedSession != Session)
		{
			SealInitialQualification(TEXT("InitialQualificationClaimConflict"));
			return Reject(TEXT("InitialQualificationClaimConflict"), TEXT("An initial qualification cannot be moved to a different source session."), OutError);
		}
		OutError.Reset();
		return true;
	}
	ClaimedSession = Session;
	QualificationStage = EInitialQualificationStage::Claimed;
	OutError.Reset();
	return true;
}

bool UGGYGOMovementInputOriginResource::ConsumeInitialQualification(const FRequest& Request, FString& OutError)
{
	if (QualificationStage != EInitialQualificationStage::Claimed || Request.RequestSerial == 0
		|| Request.Session != ClaimedSession || GetQualification(RegisteredProducer.Get()) != EGGYGOMovementInputOriginQualification::Cold)
	{
		return Reject(TEXT("InitialQualificationConsumeRejected"), TEXT("Consumption requires the original claimed session and its actually issued nonzero source request; it is one-shot."), OutError);
	}
	// This does not inspect keys or issue any request. The producer owns that assertion.
	QualificationStage = EInitialQualificationStage::Consumed;
	OutError.Reset();
	return true;
}

void UGGYGOMovementInputOriginResource::RetireInitialQualification(UPlayerInput* ExpectedProducer, FName Reason)
{
	if (!ExpectedProducer || !RegisteredProducer.HasSameIndexAndSerialNumber(TWeakObjectPtr<UPlayerInput>(ExpectedProducer)))
	{
		ReportOnce(TEXT("StaleQualificationRetirement"), TEXT("Old/foreign producer cannot retire a successor's resource registration."));
		return;
	}
	SealInitialQualification(Reason.IsNone() ? FName(TEXT("MissingQualificationRetirementReason")) : Reason);
	if (Reason.IsNone())
	{
		ReportOnce(TEXT("MissingQualificationRetirementReason"), TEXT("Initial permission was safely retired; caller omitted the required reason."));
	}
}

void UGGYGOMovementInputOriginResource::BeginDestroy()
{
	bLifecycleActive = false;
	SealInitialQualification(TEXT("OriginResourceDestroyed"));
	SealCreationScope();
	Super::BeginDestroy();
}
