#pragma once

#include "CoreMinimal.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "UObject/Object.h"

#include "GGYGOMovementInputOriginResource.generated.h"

class APlayerController;
class ULocalPlayer;
class UPlayerInput;
class UGGYGOMovementInputOriginResource;

/** Resource scope identity; unrelated to PlayerInput source/request/event serials. */
struct FGGYGOMovementInputCreationTicket
{
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> Resource;
	uint64 CreationSerial = 0;
};

/** Qualification only. Rearm neither proves physical release nor admits movement. */
enum class EGGYGOMovementInputOriginQualification : uint8
{
	Unavailable = 0,
	Cold,
	Rearm
};

/** Input-owned lifetime resource; the native LocalPlayer host retains it through removal. */
UCLASS(Transient)
class GGYGO_API UGGYGOMovementInputOriginResource : public UObject
{
	GENERATED_BODY()

public:
	using FCreationTicket = FGGYGOMovementInputCreationTicket;
	using FSession = FGGYGOMovementInputSessionIdentity;
	using FRequest = FGGYGOMovementInputRequestIdentity;

	/** Called after real PlayerAdded Super, using its captured original controller identity. */
	bool InitializeFromPlayerAdded(ULocalPlayer* Player,
		TWeakObjectPtr<APlayerController> BeforeSuper, FString& OutError);
	void NotifyPlayerRemoved(ULocalPlayer* Player);
	/** Host calls before native notification's external delegates and after SpawnPlayActor. */
	void NotifyControllerChanged(APlayerController* Controller);

	/** Existing exact registration succeeds with no ticket and OutNeedsCreation=false. */
	bool BeginNativeCreation(APlayerController* Controller, UClass* ConfiguredClass,
		FCreationTicket& OutTicket, bool& OutNeedsCreation, FString& OutError);
	/** Called by the producer's PostInitProperties only inside the explicit native scope. */
	bool RecordNativeCreatedProducer(APlayerController* Controller,
		UPlayerInput* ConstructedProducer, FString& OutError);
	bool CompleteNativeCreation(const FCreationTicket& Ticket,
		UPlayerInput* ActualProducer, FString& OutError);
	/** Stack cleanup/abort: exact ticket, independent of actor validity, idempotent once closed. */
	bool CloseNativeCreation(const FCreationTicket& Ticket, FName Reason, FString& OutError);

	bool ClaimInitialQualification(UPlayerInput* Producer, const FSession& Session, FString& OutError);
	/** Only the original producer calls this after actually issuing the nonzero request. */
	bool ConsumeInitialQualification(const FRequest& Request, FString& OutError);
	void RetireInitialQualification(UPlayerInput* ExpectedProducer, FName Reason);
	EGGYGOMovementInputOriginQualification GetQualification(const UPlayerInput* ExpectedProducer) const;

	virtual void BeginDestroy() override;

private:
	enum class EInitialQualificationStage : uint8 { Unavailable, Available, Claimed, Consumed, Retired };
	struct FNativeCreationScope
	{
		FCreationTicket Ticket;
		TWeakObjectPtr<APlayerController> Controller;
		TWeakObjectPtr<UClass> ConfiguredClass;
		TWeakObjectPtr<UPlayerInput> ConstructedProducer;
		bool bHasRecordedProducer = false;
		bool bFailed = false;
		FName FailureReason = NAME_None;
	};

	bool IsLifecycleCurrent() const;
	bool IsControllerCurrent(const APlayerController* Controller) const;
	bool IsRegisteredProducerCurrent(const UPlayerInput* Producer) const;
	bool IsResourceTicket(const FCreationTicket& Ticket) const;
	bool IsCurrentTicket(const FCreationTicket& Ticket) const;
	void SealInitialQualification(FName Reason);
	void SealCreationScope();
	bool Reject(FName Reason, const FString& Detail, FString& OutError);
	bool FailCurrentCreation(FName Reason, const FString& Detail, FString& OutError);
	bool RejectAndCloseCreation(FName Reason, const FString& Detail, FString& OutError);
	FString DescribeError(FName Reason, const FString& Detail) const;
	void ReportOnce(FName Reason, const FString& Detail);
	void ReportErrorOnce(FName Reason, const FString& Error);

	TWeakObjectPtr<ULocalPlayer> OriginalPlayer;
	TWeakObjectPtr<APlayerController> RegisteredController;
	TWeakObjectPtr<UPlayerInput> RegisteredProducer;
	FSession ClaimedSession;
	FNativeCreationScope CreationScope;
	TSet<FName> ReportedReasons;
	uint64 LastCreationSerial = 0;
	uint64 LastClosedCreationSerial = 0;
	EInitialQualificationStage QualificationStage = EInitialQualificationStage::Unavailable;
	FName FirstRetirementReason = NAME_None;
	bool bPlayerAddedHandled = false;
	bool bLifecycleActive = false;
};
