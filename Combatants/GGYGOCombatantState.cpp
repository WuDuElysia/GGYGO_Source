/**
 * @file GGYGOCombatantState.cpp
 * @brief 可替换 Pawn 的持久战斗状态宿主实现
 */
#include "Combatants/GGYGOCombatantState.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Attributes/GGYGOCombatSet.h"
#include "AbilitySystem/Attributes/GGYGOHealthSet.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "System/GGYGOGameplayTags.h"
#include "GameFramework/Pawn.h"
#include "Misc/ScopeExit.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOCombatantState)

namespace
{
	using FHostRequest = FGGYGOAvatarBindingHostRequest;
	using FHostResult = FGGYGOAvatarBindingHostResult;
	using EHostOperation = EGGYGOAvatarBindingHostOperation;
	using EHostReason = EGGYGOAvatarBindingHostReason;
	using EHostStep = EGGYGOAvatarBindingHostStep;
	using EOutcome = EGGYGOAvatarBindingOutcome;

	FHostResult HostResult(EOutcome Outcome, EHostReason Reason)
	{
		FHostResult Result;
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		return Result;
	}

	void AppendASCStep(FHostResult& History, EHostStep Kind, const FGGYGOAvatarBindingResult& Value)
	{
		FGGYGOAvatarBindingHostStepResult Step;
		Step.Step = Kind;
		Step.ASCResult = Value;
		History.Steps.Add(MoveTemp(Step));
	}

	void AppendLocalStep(FHostResult& History, EHostStep Kind, const FGGYGOPawnASCLocalResult& Value)
	{
		FGGYGOAvatarBindingHostStepResult Step;
		Step.Step = Kind;
		Step.LocalResult = Value;
		History.Steps.Add(MoveTemp(Step));
	}

	void SetASCFailure(FHostResult& History, const FGGYGOAvatarBindingResult& Value,
		EHostReason FailureReason = EHostReason::NativeStepFailed)
	{
		History.Outcome = Value.Outcome == EOutcome::Succeeded ? EOutcome::Failed : Value.Outcome;
		History.Reason = Value.Outcome == EOutcome::Busy ? EHostReason::NativeWriteBusy
			: Value.Outcome == EOutcome::Stale ? EHostReason::CallerInvalidated : FailureReason;
	}

	void SetLocalFailure(FHostResult& History, const FGGYGOPawnASCLocalResult& Value)
	{
		History.Outcome = Value.Outcome == EGGYGOPawnASCLocalOutcome::Stale ? EOutcome::Stale
			: Value.Outcome == EGGYGOPawnASCLocalOutcome::Rejected ? EOutcome::Rejected : EOutcome::Failed;
		History.Reason = History.Outcome == EOutcome::Stale
			? EHostReason::CallerInvalidated : EHostReason::LocalStepFailed;
	}

	// Select one admission from public evidence. Never try another entry after a rejection.
	FGGYGOAvatarBindingResult ExecuteHostActorInfo(UGGYGOAbilitySystemComponent* ASC,
		const FGGYGOAvatarBindingRequest& Request, FGGYGOAvatarBindingPublicationReceipt& Publication)
	{
		if (!Request.ExpectedContext.HasIssuedContext())
		{
			return ASC->TryBootstrapAvatarActorInfoTransaction(Request, Publication);
		}
		EGGYGOAvatarBindingReason Reason;
		const EOutcome Checked = ASC->CheckAvatarBindingContext(Request.ExpectedContext, Reason);
		if (Checked == EOutcome::Stale && Reason == EGGYGOAvatarBindingReason::OperationInvalidated
			&& ASC->GetAvatarBindingContext().HasSameContext(Request.ExpectedContext)
			&& (Request.Kind == EGGYGOAvatarBindingKind::Init || Request.Kind == EGGYGOAvatarBindingKind::Clear))
		{
			return ASC->TryReplaceRevokedAvatarActorInfoTransaction(Request, Publication);
		}
		return ASC->TryExecuteAvatarActorInfoTransaction(Request, Publication);
	}

	bool FindClearCommit(const FHostResult& History, FGGYGOAvatarBindingContext& OutContext)
	{
		for (int32 Index = History.Steps.Num() - 1; Index >= 0; --Index)
		{
			const FGGYGOAvatarBindingHostStepResult& Step = History.Steps[Index];
			if (Step.Step == EHostStep::ActorInfoClear && Step.ASCResult.IsSet()
				&& Step.ASCResult.GetValue().bCommitted)
			{
				OutContext = Step.ASCResult.GetValue().CommittedContext;
				return true;
			}
		}
		return false;
	}
}

AGGYGOCombatantState::AGGYGOCombatantState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicatingMovement(false);

	AbilitySystemComponent = CreateDefaultSubobject<UGGYGOAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	HealthSet = CreateDefaultSubobject<UGGYGOHealthSet>(TEXT("HealthSet"));
	CombatSet = CreateDefaultSubobject<UGGYGOCombatSet>(TEXT("CombatSet"));
}

UAbilitySystemComponent* AGGYGOCombatantState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AGGYGOCombatantState::PostInitializeComponents()
{
	Super::PostInitializeComponents();
	check(AbilitySystemComponent);
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const TWeakObjectPtr<APawn> SelectedAvatar(AvatarPawn);
	if (!InitializeOwnerActorInfo())
	{
		return;
	}
	if (AGGYGOCombatantState* Host = OriginalHost.Get())
	{
		if (SelectedAvatar.IsValid()
			&& SelectedAvatar.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn)))
		{
			Host->SynchronizeAvatarBinding();
		}
	}
}

void AGGYGOCombatantState::BeginPlay()
{
	if (IsActorBeginningPlay() && IsValid(this) && !IsActorBeingDestroyed())
	{
		bAvatarBindingPermitted = true;
	}
	Super::BeginPlay();
}

void AGGYGOCombatantState::Destroyed()
{
	CloseAvatarBindingLifecycle(TEXT("Destroyed"));
	Super::Destroyed();
}

void AGGYGOCombatantState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CloseAvatarBindingLifecycle(TEXT("EndPlay"));
	Super::EndPlay(EndPlayReason);
}

void AGGYGOCombatantState::CloseAvatarBindingLifecycle(const TCHAR* EntryPoint)
{
	check(IsInGameThread());
	if (!bAvatarBindingPermitted)
	{
		// A repeated native entry neither proves cleanup nor captures a callback's successor.
		return;
	}
	bAvatarBindingPermitted = false;
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(AbilitySystemComponent);
	const TWeakObjectPtr<APawn> SelectedAvatar(AvatarPawn);
	const FHostRequest OriginalRelease = MakeAvatarResourceRequest(EHostOperation::Release);
	const FGGYGOPawnASCResourceHandle OriginalResource = OriginalRelease.ExpectedResource;
	const FGGYGOAvatarBindingContext OriginalContext = OriginalResource.HasResource()
		? OriginalRelease.ExpectedContext
		: AbilitySystemComponent ? AbilitySystemComponent->GetAvatarBindingContext() : FGGYGOAvatarBindingContext{};
	FGGYGOAvatarBindingContext RetirementContext = OriginalContext;
	FHostResult History;
	EGGYGOAvatarBindingReason NativeReason = EGGYGOAvatarBindingReason::None;
	if (OriginalResource.HasResource())
	{
		History = RequestAvatarBinding(OriginalRelease);
		FindClearCommit(History, RetirementContext);
	}
	else
	{
		// No Extension installation exists here. ASC alone authenticates the original ActorInfo.
		const auto OwnerOnlyCleanupScope = [&]()
		{
			AGGYGOCombatantState* Host = OriginalHost.Get();
			UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get();
			return Host && ASC && !Host->bAvatarBindingPermitted
				&& Host->AbilitySystemComponent == ASC && ASC->GetOwner() == Host
				&& !Host->AvatarResource.HasResource()
				&& SelectedAvatar.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn));
		};
		UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get();
		if (!ASC)
		{
			History = HostResult(EOutcome::Failed, EHostReason::InvalidASC);
		}
		else
		{
			const EOutcome Checked = ASC->CheckAvatarBindingCleanupContext(OriginalContext, NativeReason);
			if (Checked != EOutcome::Succeeded)
			{
				History = HostResult(Checked, EHostReason::ContextMismatch);
			}
			else if (!OwnerOnlyCleanupScope())
			{
				History = HostResult(EOutcome::Stale, EHostReason::CallerInvalidated);
			}
			else
			{
				FGGYGOAvatarBindingRequest Clear;
				Clear.Kind = EGGYGOAvatarBindingKind::Clear;
				Clear.ExpectedContext = OriginalContext;
				Clear.ClearMode = EGGYGOAvatarBindingClearMode::ClearActorInfo;
				Clear.IsRequestContextCurrent = OwnerOnlyCleanupScope;
				FGGYGOAvatarBindingPublicationReceipt Publication;
				const FGGYGOAvatarBindingResult Cleared = ASC->TryExecuteAvatarActorInfoTransaction(Clear, Publication);
				AppendASCStep(History, EHostStep::ActorInfoClear, Cleared);
				NativeReason = Cleared.Reason;
				if (Cleared.bCommitted)
				{
					RetirementContext = Cleared.CommittedContext;
				}
				if (Cleared.Outcome != EOutcome::Succeeded || !Cleared.bCommitted)
				{
					SetASCFailure(History, Cleared);
				}
				else if (ASC = OriginalASC.Get(); !ASC)
				{
					History.Outcome = EOutcome::Failed;
					History.Reason = EHostReason::InvalidASC;
				}
				else
				{
					const FGGYGOAvatarBindingResult Published = ASC->PublishAvatarBindingNotice(Publication, OwnerOnlyCleanupScope);
					AppendASCStep(History, EHostStep::PublishNotice, Published);
					NativeReason = Published.Reason;
					if (Published.Outcome == EOutcome::Succeeded)
					{
						History.Outcome = EOutcome::Succeeded;
						History.Reason = EHostReason::None;
					}
					else
					{
						SetASCFailure(History, Published, EHostReason::PublicationFailed);
					}
				}
			}
		}
	}
	EGGYGOAvatarBindingReason RetirementReason = EGGYGOAvatarBindingReason::InvalidASC;
	bool bRetiredOriginalContext = false;
	if (UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get())
	{
		// Metadata revocation is separate from physical Clear, even when cleanup failed or was Busy.
		bRetiredOriginalContext = ASC->InvalidateAvatarBinding(
			RetirementContext, EGGYGOAvatarBindingReason::LifecycleClosed, RetirementReason);
	}
	if (History.Outcome != EOutcome::Succeeded || !bRetiredOriginalContext)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] %s binding close: Host=%s ASC=%s SelectedPawn=%s HasResource=%d Binding=%llu Write=%llu Outcome=%u Reason=%u NativeReason=%u Steps=%d; metadata retirement Binding=%llu Write=%llu Accepted=%d Reason=%u (not physical Clear)."),
			EntryPoint, *GetPathNameSafe(OriginalHost.Get()), *GetPathNameSafe(OriginalASC.Get()),
			*GetPathNameSafe(SelectedAvatar.Get()), OriginalResource.HasResource() ? 1 : 0,
			static_cast<unsigned long long>(OriginalContext.Binding.Serial),
			static_cast<unsigned long long>(OriginalContext.LastActorInfoWrite.Serial),
			static_cast<uint32>(History.Outcome), static_cast<uint32>(History.Reason),
			static_cast<uint32>(NativeReason), History.Steps.Num(),
			static_cast<unsigned long long>(RetirementContext.Binding.Serial),
			static_cast<unsigned long long>(RetirementContext.LastActorInfoWrite.Serial),
			bRetiredOriginalContext ? 1 : 0, static_cast<uint32>(RetirementReason));
	}
}

void AGGYGOCombatantState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGGYGOCombatantState, AvatarPawn);
}

FHostResult AGGYGOCombatantState::RequestAvatarBinding(const FHostRequest& Request)
{
	check(IsInGameThread());
	const FHostRequest Original = Request;
	const TWeakObjectPtr<AActor> Self(this);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OwnedASC(AbilitySystemComponent);
	FHostResult Result;
	if (!Original.ExpectedHost.HasSameIndexAndSerialNumber(Self))
	{
		Result = HostResult(EOutcome::Rejected, EHostReason::InvalidHost);
	}
	else if (!Original.ExpectedASC.HasSameIndexAndSerialNumber(OwnedASC)
		|| !AbilitySystemComponent || AbilitySystemComponent->GetOwner() != this)
	{
		Result = HostResult(EOutcome::Rejected, EHostReason::EndpointMismatch);
	}
	else if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		Result = HostResult(EOutcome::Failed, EHostReason::LifecycleClosed);
	}
	else if (UGGYGOAbilitySystemComponent* ASC = Original.ExpectedASC.Get();
		ASC && ASC->IsAvatarBindingNativeWriteBusy())
	{
		Result = HostResult(EOutcome::Busy, EHostReason::NativeWriteBusy);
	}
	else
	{
		switch (Original.Operation)
		{
		case EHostOperation::Initialize:
			Result = InitializeAvatarBinding(Original);
			break;
		case EHostOperation::Release:
			Result = ReleaseAvatarBinding(Original);
			break;
		case EHostOperation::Refresh:
			Result = RefreshAvatarBinding(Original);
			break;
		default:
			Result = HostResult(EOutcome::Rejected, EHostReason::InvalidRequest);
			break;
		}
	}
	if (Result.Outcome != EOutcome::Succeeded)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] RequestAvatarBinding: Host=%s ASC=%s Pawn=%s Extension=%s Operation=%u Binding=%llu Write=%llu Outcome=%u Reason=%u Steps=%d"),
			*GetPathNameSafe(Original.ExpectedHost.Get()), *GetPathNameSafe(Original.ExpectedASC.Get()),
			*GetPathNameSafe(Original.ExpectedPawn.Get()), *GetPathNameSafe(Original.ExpectedExtension.Get()),
			static_cast<uint32>(Original.Operation), static_cast<unsigned long long>(Original.ExpectedContext.Binding.Serial),
			static_cast<unsigned long long>(Original.ExpectedContext.LastActorInfoWrite.Serial),
			static_cast<uint32>(Result.Outcome), static_cast<uint32>(Result.Reason), Result.Steps.Num());
	}
	return Result;
}

FHostRequest AGGYGOCombatantState::MakeAvatarResourceRequest(EHostOperation Operation) const
{
	FHostRequest Request;
	Request.Operation = Operation;
	Request.ExpectedHost = const_cast<AGGYGOCombatantState*>(this);
	Request.ExpectedASC = AbilitySystemComponent;
	Request.ExpectedResource = AvatarResource;
	Request.ExpectedPawn = AvatarResource.GetIdentity().Pawn;
	Request.ExpectedExtension = AvatarResourceExtension;
	Request.ExpectedContext = AvatarResourceContext;
	if (AbilitySystemComponent && AvatarResource.HasResource())
	{
		const FGGYGOAvatarBindingContext Current = AbilitySystemComponent->GetAvatarBindingContext();
		if (Current.Binding.HasSameIdentity(AvatarResource.GetIdentity().Binding))
		{
			Request.ExpectedContext = Current;
		}
	}
	return Request;
}

bool AGGYGOCombatantState::IsOriginalAvatarResource(const FHostRequest& Request) const
{
	const FGGYGOPawnASCResourceIdentity Identity = Request.ExpectedResource.GetIdentity();
	return IsValid(this) && !HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		&& Request.ExpectedHost.HasSameIndexAndSerialNumber(
			TWeakObjectPtr<AActor>(const_cast<AGGYGOCombatantState*>(this)))
		&& AvatarResource.HasSameResource(Request.ExpectedResource)
		&& AvatarResourceExtension.HasSameIndexAndSerialNumber(Request.ExpectedExtension)
		&& Identity.ASC.HasSameIndexAndSerialNumber(Request.ExpectedASC)
		&& Identity.Pawn.HasSameIndexAndSerialNumber(Request.ExpectedPawn)
		&& AvatarResourceContext.HasSameContext(Request.ExpectedContext);
}

void AGGYGOCombatantState::RetireHostAvatarResource(const FGGYGOPawnASCResourceHandle& OriginalResource)
{
	if (!AvatarResource.HasSameResource(OriginalResource))
	{
		return;
	}
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	if (APawn* Pawn = Identity.Pawn.Get())
	{
		Pawn->OnDestroyed.RemoveDynamic(this, &ThisClass::HandleAvatarDestroyed);
	}
	AvatarResource = FGGYGOPawnASCResourceHandle{};
	AvatarResourceContext = {};
	AvatarResourceExtension.Reset();
	// On clients AvatarPawn is the server's received selection, possibly already a new Pawn.
	if (HasAuthority() && Identity.Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(AvatarPawn)))
	{
		AvatarPawn = nullptr;
	}
}

FGGYGOAvatarBindingResult AGGYGOCombatantState::PublishAvatarResources(
	const FGGYGOAvatarBindingPublicationReceipt& Publication,
	const FGGYGOAvatarBindingContext& CommittedContext,
	const FGGYGOPawnASCResourceHandle& OriginalResource,
	UGGYGOPawnExtensionComponent* OriginalExtension, bool bReleased, FHostResult& OutHistory)
{
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(AbilitySystemComponent);
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension(OriginalExtension);
	const TWeakObjectPtr<APawn> PublishedAvatar(AvatarPawn);
	const FGGYGOAvatarBindingPublicationReceipt OwnPublication = Publication;
	const FGGYGOPawnASCResourceHandle OwnResource = OriginalResource;
	bool bBridgeReturned = false;
	bool bBridgeSucceeded = false;

	const auto PublicationQuery = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		UGGYGOPawnExtensionComponent* LiveExtension = Extension.Get();
		if (!Host || !LiveExtension || !OriginalASC.IsValid()
			|| !PublishedAvatar.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn))
			|| (bBridgeReturned && !bBridgeSucceeded))
		{
			return false;
		}
		if (bReleased)
		{
			return !Host->AvatarResource.HasResource()
				&& !LiveExtension->GetCurrentLocalAbilitySystemResource().HasResource();
		}
		return Host->IsAvatarBindingPermitted()
			&& Host->AvatarResource.HasSameResource(OwnResource)
			&& Host->AvatarResourceContext.HasSameContext(CommittedContext)
			&& OwnResource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(PublishedAvatar)
			&& LiveExtension->IsLocalAbilitySystemResourceInstalled(OwnResource);
	};

	UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get();
	const FDelegateHandle NoticeHandle = ASC->OnAvatarBindingNotice().AddLambda(
		[&](const FGGYGOAvatarBindingPublicationReceipt& Incoming,
			const FGGYGOAvatarBindingNotice& Notice)
		{
			UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
			if (!LiveASC || !LiveASC->IsAvatarBindingNoticeDispatching(OwnPublication)
				|| !LiveASC->IsAvatarBindingNoticeDispatching(Incoming)
				|| !Notice.After.HasSameContext(CommittedContext)
				|| !Notice.Operation.HasSameIdentity(CommittedContext.LastActorInfoWrite)
				|| !PublicationQuery())
			{
				return;
			}
			UGGYGOPawnExtensionComponent* LiveExtension = Extension.Get();
			FGGYGOPawnASCLocalResult Local = bReleased
				? LiveExtension->NotifyLocalResourcesReleased(OwnResource)
				: LiveExtension->NotifyLocalResourcesReady(OwnResource, OwnPublication);
			AppendLocalStep(OutHistory, bReleased ? EHostStep::NotifyLocalReleased : EHostStep::NotifyLocalReady, Local);
			bBridgeReturned = true;
			bBridgeSucceeded = Local.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded;
		});
	ON_SCOPE_EXIT
	{
		if (UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.GetEvenIfUnreachable())
		{
			LiveASC->OnAvatarBindingNotice().Remove(NoticeHandle);
		}
	};
	FGGYGOAvatarBindingResult Published = ASC->PublishAvatarBindingNotice(OwnPublication, PublicationQuery);
	AppendASCStep(OutHistory, EHostStep::PublishNotice, Published);
	if (Published.Outcome == EOutcome::Succeeded && (!bBridgeReturned || !bBridgeSucceeded))
	{
		// A publication with no successful local bridge cannot complete this Host request.
		OutHistory.Outcome = EOutcome::Failed;
		OutHistory.Reason = EHostReason::ReadyNotEstablished;
	}
	return Published;
}

FHostResult AGGYGOCombatantState::InitializeAvatarBinding(const FHostRequest& Request)
{
	FHostResult History;
	UGGYGOAbilitySystemComponent* ASC = Request.ExpectedASC.Get();
	APawn* Pawn = Request.ExpectedPawn.Get();
	UGGYGOPawnExtensionComponent* Extension = Request.ExpectedExtension.Get();
	if (!ASC)
	{
		return HostResult(EOutcome::Failed, EHostReason::InvalidASC);
	}
	if (Request.ExpectedResource.HasResource())
	{
		return HostResult(EOutcome::Rejected, EHostReason::InvalidRequest);
	}
	if (!ValidateAvatarBinding(Pawn, TEXT("Request.Initialize")))
	{
		return HostResult(EOutcome::Rejected, EHostReason::LifecycleClosed);
	}
	if (!Extension || Extension != UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Pawn))
	{
		return HostResult(EOutcome::Rejected, EHostReason::InvalidExtension);
	}
	if (!HasAuthority() && AvatarPawn != Pawn)
	{
		return HostResult(EOutcome::Rejected, EHostReason::AuthorityDenied);
	}
	const FGGYGOPawnASCResourceHandle TargetResource = Extension->GetCurrentLocalAbilitySystemResource();
	EGGYGOAvatarBindingReason CheckedReason;
	if (TargetResource.HasResource())
	{
		if (AvatarResource.HasSameResource(TargetResource) && AvatarPawn == Pawn
			&& TargetResource.GetIdentity().Binding.HasSameIdentity(Request.ExpectedContext.Binding)
			&& ASC->CheckAvatarBindingContext(Request.ExpectedContext, CheckedReason) == EOutcome::Succeeded
			&& Extension->IsLocalAbilitySystemResourceReady(TargetResource))
		{
			return HostResult(EOutcome::Succeeded, EHostReason::None);
		}
		return HostResult(EOutcome::Rejected, EHostReason::ResourceConflict);
	}
	if (AvatarResource.HasResource())
	{
		return HostResult(EOutcome::Rejected, EHostReason::ResourceConflict);
	}
	if (ASC->GetOwnerActor() && ASC->GetOwnerActor() != this)
	{
		return HostResult(EOutcome::Rejected, EHostReason::EndpointMismatch);
	}
	if (ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dying)
		|| ASC->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dead))
	{
		return HostResult(EOutcome::Rejected, EHostReason::LifecycleClosed);
	}
	if (!ASC->GetAvatarBindingContext().HasSameContext(Request.ExpectedContext)
		&& Request.ExpectedContext.HasIssuedContext())
	{
		return HostResult(EOutcome::Stale, EHostReason::ContextMismatch);
	}

	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const TWeakObjectPtr<APawn> OriginalSelection(AvatarPawn);
	const auto OriginalScope = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		UGGYGOPawnExtensionComponent* LiveExtension = Request.ExpectedExtension.Get();
		return Host && Host->IsAvatarBindingPermitted() && Request.ExpectedPawn.IsValid()
			&& !Request.ExpectedPawn->IsActorBeingDestroyed() && LiveExtension
			&& !Host->AvatarResource.HasResource()
			&& OriginalSelection.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn))
			&& !LiveExtension->GetCurrentLocalAbilitySystemResource().HasResource();
	};
	FGGYGOAvatarBindingRequest Native;
	Native.Kind = EGGYGOAvatarBindingKind::Init;
	Native.ExpectedContext = Request.ExpectedContext;
	Native.OwnerActor = this;
	Native.AvatarActor = Pawn;
	Native.IsRequestContextCurrent = OriginalScope;
	FGGYGOAvatarBindingPublicationReceipt Publication;
	const FGGYGOAvatarBindingResult Initialized = ExecuteHostActorInfo(ASC, Native, Publication);
	AppendASCStep(History, EHostStep::ActorInfoInit, Initialized);
	if (Initialized.Outcome != EOutcome::Succeeded || !Initialized.bCommitted)
	{
		SetASCFailure(History, Initialized);
		if (!Initialized.bCommitted && Initialized.Operation.HasIssuedIdentity())
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Combatants] Init failed before commit: Host=%s ASC=%s Pawn=%s Operation=%llu Outcome=%u Reason=%u."),
				*GetPathNameSafe(OriginalHost.Get()), *GetPathNameSafe(Request.ExpectedASC.Get()),
				*GetPathNameSafe(Request.ExpectedPawn.Get()), static_cast<unsigned long long>(Initialized.Operation.Serial),
				static_cast<uint32>(Initialized.Outcome),
				static_cast<uint32>(Initialized.Reason));
			const auto FailedInitCleanupScope = [&]()
			{
				AGGYGOCombatantState* Host = OriginalHost.Get();
				UGGYGOAbilitySystemComponent* OriginalASC = Request.ExpectedASC.Get();
				APawn* OriginalPawn = Request.ExpectedPawn.Get();
				UGGYGOPawnExtensionComponent* OriginalExtension = Request.ExpectedExtension.Get();
				// Original empty-resource scope may close; it never grants new-work/Ready admission.
				return Host && OriginalASC && OriginalPawn && OriginalExtension
					&& Request.ExpectedHost.HasSameIndexAndSerialNumber(TWeakObjectPtr<AActor>(Host))
					&& Host->AbilitySystemComponent == OriginalASC && OriginalASC->GetOwner() == Host
					&& OriginalExtension->GetOwner() == OriginalPawn
					&& !Request.ExpectedResource.HasResource() && !Host->AvatarResource.HasResource()
					&& OriginalSelection.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn))
					&& !TargetResource.HasResource()
					&& !OriginalExtension->GetCurrentLocalAbilitySystemResource().HasResource();
			};
			if (UGGYGOAbilitySystemComponent* OriginalASC = Request.ExpectedASC.Get())
			{
				const FGGYGOAvatarBindingResult Cleanup = OriginalASC->TryCleanupFailedAvatarActorInfoInit(
					Initialized.Operation, FailedInitCleanupScope);
				AppendASCStep(History, EHostStep::ActorInfoClear, Cleanup);
				UE_LOG(LogGGYGOAbilitySystem, Warning,
					TEXT("[Combatants] Failed Init cleanup returned: Host=%s ASC=%s Pawn=%s Operation=%llu Outcome=%u Reason=%u Committed=%d; original Init remains failed."),
					*GetPathNameSafe(OriginalHost.Get()), *GetPathNameSafe(Request.ExpectedASC.Get()),
					*GetPathNameSafe(Request.ExpectedPawn.Get()), static_cast<unsigned long long>(Initialized.Operation.Serial),
					static_cast<uint32>(Cleanup.Outcome), static_cast<uint32>(Cleanup.Reason), Cleanup.bCommitted ? 1 : 0);
			}
			else
			{
				UE_LOG(LogGGYGOAbilitySystem, Warning,
					TEXT("[Combatants] Failed Init cleanup not called: Host=%s Pawn=%s Operation=%llu; original ASC expired."),
					*GetPathNameSafe(OriginalHost.Get()), *GetPathNameSafe(Request.ExpectedPawn.Get()),
					static_cast<unsigned long long>(Initialized.Operation.Serial));
			}
		}
		return History;
	}

	FGGYGOPawnASCResourceHandle InstalledResource;
	// Cleanup can only consume this exact commit and local record, never a successor.
	const auto CleanupOwnCommit = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		UGGYGOAbilitySystemComponent* LiveASC = Request.ExpectedASC.Get();
		UGGYGOPawnExtensionComponent* LiveExtension = Request.ExpectedExtension.Get();
		EGGYGOAvatarBindingReason Reason;
		if (!Host || !LiveASC || !LiveExtension
			|| Host->AbilitySystemComponent != LiveASC || LiveASC->GetOwner() != Host
			|| LiveASC->CheckAvatarBindingCleanupContext(Initialized.CommittedContext, Reason) != EOutcome::Succeeded)
		{
			return;
		}
		if (Host->AvatarResource.HasResource())
		{
			if (Host->AvatarResource.HasSameResource(InstalledResource)
				&& Host->AvatarResourceContext.HasSameContext(Initialized.CommittedContext))
			{
				FHostRequest Release = Host->MakeAvatarResourceRequest(EHostOperation::Release);
				Release.ExpectedContext = Initialized.CommittedContext;
				const FHostResult Cleanup = Host->RequestAvatarBinding(Release);
				History.Steps.Append(Cleanup.Steps);
			}
			return;
		}
		if (!OriginalSelection.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn)))
		{
			return;
		}
		const FGGYGOPawnASCResourceHandle Local = LiveExtension->GetCurrentLocalAbilitySystemResource();
		if (Local.HasResource())
		{
			if (!Local.HasSameResource(InstalledResource))
			{
				return;
			}
			const FGGYGOPawnASCLocalResult Withdrawn = LiveExtension->WithdrawLocalAbilitySystemResources(InstalledResource);
			AppendLocalStep(History, EHostStep::WithdrawLocalResources, Withdrawn);
			if (Withdrawn.Outcome != EGGYGOPawnASCLocalOutcome::Succeeded)
			{
				return;
			}
		}
		FGGYGOAvatarBindingRequest Clear;
		Clear.Kind = EGGYGOAvatarBindingKind::Clear;
		Clear.ExpectedContext = Initialized.CommittedContext;
		const AActor* OriginalOwner = LiveASC->GetOwnerActor();
		const bool bClosingOriginalHost = !Host->bAvatarBindingPermitted || Host->IsActorBeingDestroyed();
		Clear.ClearMode = bClosingOriginalHost || (OriginalOwner && OriginalOwner->IsActorBeingDestroyed())
			? EGGYGOAvatarBindingClearMode::ClearActorInfo : EGGYGOAvatarBindingClearMode::PreserveOwner;
		Clear.IsRequestContextCurrent = [&]()
		{
			AGGYGOCombatantState* CurrentHost = OriginalHost.Get();
			UGGYGOPawnExtensionComponent* CurrentExtension = Request.ExpectedExtension.Get();
			UGGYGOAbilitySystemComponent* CurrentASC = Request.ExpectedASC.Get();
			// Original-resource cleanup scope, including Closing; never new-work or Ready admission.
			return CurrentHost && CurrentExtension && CurrentASC
				&& CurrentHost->AbilitySystemComponent == CurrentASC && CurrentASC->GetOwner() == CurrentHost
				&& !CurrentHost->AvatarResource.HasResource()
				&& OriginalSelection.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(CurrentHost->AvatarPawn))
				&& !CurrentExtension->GetCurrentLocalAbilitySystemResource().HasResource();
		};
		FGGYGOAvatarBindingPublicationReceipt ClearPublication;
		const FGGYGOAvatarBindingResult Cleared = LiveASC->TryExecuteAvatarActorInfoTransaction(Clear, ClearPublication);
		AppendASCStep(History, EHostStep::ActorInfoClear, Cleared);
		if (Cleared.Outcome == EOutcome::Succeeded && Cleared.bCommitted)
		{
			if (InstalledResource.HasResource())
			{
				Host->PublishAvatarResources(ClearPublication, Cleared.CommittedContext,
					InstalledResource, LiveExtension, true, History);
			}
			else
			{
				const auto CleanupQuery = Clear.IsRequestContextCurrent;
				const FGGYGOAvatarBindingResult Published = LiveASC->PublishAvatarBindingNotice(ClearPublication, CleanupQuery);
				AppendASCStep(History, EHostStep::PublishNotice, Published);
			}
		}
		else if (InstalledResource.HasResource() && Clear.IsRequestContextCurrent())
		{
			// Local retirement is still required; it cannot turn the failed Init into success.
			const FGGYGOPawnASCLocalResult Released = LiveExtension->NotifyLocalResourcesReleased(InstalledResource);
			AppendLocalStep(History, EHostStep::NotifyLocalReleased, Released);
		}
	};

	const FGGYGOPawnASCLocalResult Installed = Extension->InstallLocalAbilitySystemResources(
		ASC, Pawn, Initialized.CommittedContext);
	AppendLocalStep(History, EHostStep::InstallLocalResources, Installed);
	InstalledResource = Installed.Resource;
	if (Installed.Outcome != EGGYGOPawnASCLocalOutcome::Succeeded || !Installed.Resource.HasResource())
	{
		SetLocalFailure(History, Installed);
		CleanupOwnCommit();
		return History;
	}
	AGGYGOCombatantState* Host = OriginalHost.Get();
	if (!Host || !Host->IsAvatarBindingPermitted() || Host->AvatarResource.HasResource()
		|| !OriginalSelection.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn))
		|| !Extension->IsLocalAbilitySystemResourceInstalled(Installed.Resource))
	{
		History.Outcome = EOutcome::Stale;
		History.Reason = EHostReason::CallerInvalidated;
		CleanupOwnCommit();
		return History;
	}
	Host->AvatarResource = Installed.Resource;
	Host->AvatarResourceContext = Initialized.CommittedContext;
	Host->AvatarResourceExtension = Request.ExpectedExtension;
	Host->AvatarPawn = Pawn;
	Pawn->OnDestroyed.AddUniqueDynamic(Host, &ThisClass::HandleAvatarDestroyed);

	History.Outcome = EOutcome::Succeeded;
	History.Reason = EHostReason::None;
	const FGGYGOAvatarBindingResult Published = Host->PublishAvatarResources(Publication,
		Initialized.CommittedContext, Installed.Resource, Extension, false, History);
	Host = OriginalHost.Get();
	if (Published.Outcome != EOutcome::Succeeded)
	{
		SetASCFailure(History, Published, EHostReason::PublicationFailed);
		CleanupOwnCommit();
		return History;
	}
	if (History.Outcome != EOutcome::Succeeded || !Host
		|| !Host->AvatarResource.HasSameResource(Installed.Resource)
		|| !Extension->IsLocalAbilitySystemResourceReady(Installed.Resource))
	{
		if (History.Outcome == EOutcome::Succeeded)
		{
			History.Outcome = EOutcome::Stale;
			History.Reason = EHostReason::CallerInvalidated;
		}
		CleanupOwnCommit();
		return History;
	}
	if (Host->HasAuthority() && Host->IsAvatarBindingPermitted())
	{
		Host->ForceNetUpdate();
	}
	return History;
}

FHostResult AGGYGOCombatantState::ReleaseAvatarBinding(const FHostRequest& Request)
{
	FHostResult History;
	const FGGYGOPawnASCResourceIdentity Identity = Request.ExpectedResource.GetIdentity();
	if (!Request.ExpectedResource.HasResource()
		|| !Identity.ASC.HasSameIndexAndSerialNumber(Request.ExpectedASC)
		|| !Identity.Pawn.HasSameIndexAndSerialNumber(Request.ExpectedPawn)
		|| !Identity.Binding.HasSameIdentity(Request.ExpectedContext.Binding))
	{
		return HostResult(EOutcome::Rejected, EHostReason::ResourceMismatch);
	}
	if (!IsOriginalAvatarResource(Request))
	{
		return HostResult(EOutcome::Stale, EHostReason::CallerInvalidated);
	}
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	UGGYGOPawnExtensionComponent* Extension = Request.ExpectedExtension.Get();
	if (!Extension)
	{
		RetireHostAvatarResource(Request.ExpectedResource);
		return HostResult(EOutcome::Failed, EHostReason::InvalidExtension);
	}
	const FGGYGOPawnASCLocalResult Withdrawn = Extension->WithdrawLocalAbilitySystemResources(Request.ExpectedResource);
	AppendLocalStep(History, EHostStep::WithdrawLocalResources, Withdrawn);
	if (Withdrawn.Outcome != EGGYGOPawnASCLocalOutcome::Succeeded)
	{
		SetLocalFailure(History, Withdrawn);
		return History;
	}
	const auto OriginalScope = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		UGGYGOPawnExtensionComponent* LiveExtension = Request.ExpectedExtension.Get();
		UGGYGOAbilitySystemComponent* LiveASC = Request.ExpectedASC.Get();
		// Actor Destroy/EndPlay may clean its original resource; no new-work lifecycle check here.
		if (!Host || !Host->IsOriginalAvatarResource(Request) || !LiveExtension
			|| !TWeakObjectPtr<UGGYGOAbilitySystemComponent>(Host->AbilitySystemComponent)
				.HasSameIndexAndSerialNumber(Request.ExpectedASC)
			|| (LiveASC && LiveASC->GetOwner() != Host))
		{
			return false;
		}
		const FGGYGOPawnASCResourceHandle Current = LiveExtension->GetCurrentLocalAbilitySystemResource();
		// Withdrawal is expected. A new opaque slot, even in the same Context, is a successor.
		return !Current.HasResource() || Current.HasSameResource(Request.ExpectedResource);
	};
	const auto FinishLocalFailure = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		if (Host && Host->IsOriginalAvatarResource(Request) && !Request.ExpectedExtension.IsValid())
		{
			Host->RetireHostAvatarResource(Request.ExpectedResource);
			History.Outcome = EOutcome::Failed;
			History.Reason = EHostReason::InvalidExtension;
			return;
		}
		if (!Host || !OriginalScope())
		{
			History.Outcome = EOutcome::Stale;
			History.Reason = EHostReason::CallerInvalidated;
			return;
		}
		if (UGGYGOAbilitySystemComponent* ASC = Request.ExpectedASC.Get())
		{
			EGGYGOAvatarBindingReason Reason;
			ASC->InvalidateAvatarBinding(Request.ExpectedContext, EGGYGOAvatarBindingReason::OperationInvalidated, Reason);
		}
		Host->RetireHostAvatarResource(Request.ExpectedResource);
		if (UGGYGOPawnExtensionComponent* LiveExtension = Request.ExpectedExtension.Get())
		{
			const FGGYGOPawnASCLocalResult Local = LiveExtension->NotifyLocalResourcesReleased(Request.ExpectedResource);
			AppendLocalStep(History, EHostStep::NotifyLocalReleased, Local);
		}
		if (AGGYGOCombatantState* CurrentHost = OriginalHost.Get();
			!CurrentHost || CurrentHost->AvatarResource.HasResource())
		{
			History.Outcome = EOutcome::Stale;
			History.Reason = EHostReason::CallerInvalidated;
		}
		// Native failure remains failure even when original local cleanup completed.
	};

	UGGYGOAbilitySystemComponent* ASC = Request.ExpectedASC.Get();
	if (!ASC)
	{
		History.Outcome = EOutcome::Failed;
		History.Reason = EHostReason::InvalidASC;
		FinishLocalFailure();
		return History;
	}
	FGameplayTagContainer IgnoredAbilities;
	IgnoredAbilities.AddTag(GGYGOGameplayTags::Ability_Behavior_SurvivesDeath);
	const FGGYGOAvatarBindingResult Canceled = ASC->TryCancelAvatarBindingAbilities(
		Request.ExpectedContext, nullptr, &IgnoredAbilities, OriginalScope);
	AppendASCStep(History, EHostStep::CancelAbilities, Canceled);
	if (Canceled.Outcome != EOutcome::Succeeded)
	{
		SetASCFailure(History, Canceled);
		FinishLocalFailure();
		return History;
	}
	EGGYGOAvatarBindingReason Reason;
	ASC = Request.ExpectedASC.Get();
	if (!ASC || !OriginalScope()
		|| ASC->CheckAvatarBindingCleanupContext(Request.ExpectedContext, Reason) != EOutcome::Succeeded)
	{
		History.Outcome = EOutcome::Stale;
		History.Reason = EHostReason::CallerInvalidated;
		FinishLocalFailure();
		return History;
	}
	ASC->ClearAbilityInput();
	FGGYGOAvatarBindingHostStepResult InputStep;
	InputStep.Step = EHostStep::ClearAbilityInput;
	FGGYGOAvatarBindingHostInputClearHistory InputHistory;
	InputHistory.OriginalContext = Request.ExpectedContext;
	InputHistory.OriginalResource = Request.ExpectedResource;
	InputStep.InputClear = InputHistory;
	History.Steps.Add(MoveTemp(InputStep));
	if (!OriginalScope())
	{
		History.Outcome = EOutcome::Stale;
		History.Reason = EHostReason::CallerInvalidated;
		FinishLocalFailure();
		return History;
	}
	const FGGYGOAvatarBindingResult Cues = ASC->TryRemoveAvatarBindingGameplayCues(
		Request.ExpectedContext, OriginalScope);
	AppendASCStep(History, EHostStep::RemoveGameplayCues, Cues);
	if (Cues.Outcome != EOutcome::Succeeded)
	{
		SetASCFailure(History, Cues);
		FinishLocalFailure();
		return History;
	}
	FGGYGOAvatarBindingRequest Clear;
	Clear.Kind = EGGYGOAvatarBindingKind::Clear;
	Clear.ExpectedContext = Request.ExpectedContext;
	const AActor* OriginalOwner = ASC->GetOwnerActor();
	const bool bClosingOriginalHost = !bAvatarBindingPermitted || IsActorBeingDestroyed();
	Clear.ClearMode = bClosingOriginalHost || (OriginalOwner && OriginalOwner->IsActorBeingDestroyed())
		? EGGYGOAvatarBindingClearMode::ClearActorInfo : EGGYGOAvatarBindingClearMode::PreserveOwner;
	Clear.IsRequestContextCurrent = OriginalScope;
	FGGYGOAvatarBindingPublicationReceipt Publication;
	const FGGYGOAvatarBindingResult Cleared = ASC->TryExecuteAvatarActorInfoTransaction(Clear, Publication);
	AppendASCStep(History, EHostStep::ActorInfoClear, Cleared);
	if (Cleared.Outcome != EOutcome::Succeeded || !Cleared.bCommitted)
	{
		SetASCFailure(History, Cleared);
		FinishLocalFailure();
		return History;
	}
	AGGYGOCombatantState* Host = OriginalHost.Get();
	if (!Host || !OriginalScope())
	{
		History.Outcome = EOutcome::Stale;
		History.Reason = EHostReason::CallerInvalidated;
		return History;
	}
	Host->RetireHostAvatarResource(Request.ExpectedResource);
	History.Outcome = EOutcome::Succeeded;
	History.Reason = EHostReason::None;
	const FGGYGOAvatarBindingResult Published = Host->PublishAvatarResources(Publication,
		Cleared.CommittedContext, Request.ExpectedResource, Extension, true, History);
	if (Published.Outcome != EOutcome::Succeeded)
	{
		SetASCFailure(History, Published, EHostReason::PublicationFailed);
		return History;
	}
	Host = OriginalHost.Get();
	if (History.Outcome == EOutcome::Succeeded && Host && Host->HasAuthority() && Host->IsAvatarBindingPermitted())
	{
		Host->ForceNetUpdate();
	}
	return History;
}

FHostResult AGGYGOCombatantState::RefreshAvatarBinding(const FHostRequest& Request)
{
	FHostResult History;
	UGGYGOAbilitySystemComponent* ASC = Request.ExpectedASC.Get();
	UGGYGOPawnExtensionComponent* Extension = Request.ExpectedExtension.Get();
	if (!ASC || !Extension)
	{
		return HostResult(EOutcome::Failed, !ASC ? EHostReason::InvalidASC : EHostReason::InvalidExtension);
	}
	if (!IsOriginalAvatarResource(Request) || !IsAvatarBindingPermitted()
		|| !Extension->IsLocalAbilitySystemResourceReady(Request.ExpectedResource))
	{
		return HostResult(EOutcome::Stale, EHostReason::ReadyNotEstablished);
	}
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const auto OriginalScope = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		UGGYGOAbilitySystemComponent* LiveASC = Request.ExpectedASC.Get();
		APawn* LivePawn = Request.ExpectedPawn.Get();
		UGGYGOPawnExtensionComponent* LiveExtension = Request.ExpectedExtension.Get();
		// ASC retires its committed snapshot during the native write; this query owns only caller scope.
		return Host && Host->IsAvatarBindingPermitted() && Host->IsOriginalAvatarResource(Request)
			&& LiveASC && Host->AbilitySystemComponent == LiveASC && LiveASC->GetOwner() == Host
			&& LivePawn && LiveExtension && !LiveExtension->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& LiveExtension->GetOwner() == LivePawn
			&& LiveExtension->GetCurrentLocalAbilitySystemResource().HasSameResource(Request.ExpectedResource);
	};
	FGGYGOAvatarBindingRequest Native;
	Native.Kind = EGGYGOAvatarBindingKind::Refresh;
	Native.ExpectedContext = Request.ExpectedContext;
	Native.IsRequestContextCurrent = OriginalScope;
	FGGYGOAvatarBindingPublicationReceipt Publication;
	const FGGYGOAvatarBindingResult Refreshed = ASC->TryExecuteAvatarActorInfoTransaction(Native, Publication);
	AppendASCStep(History, EHostStep::ActorInfoRefresh, Refreshed);
	if (Refreshed.Outcome != EOutcome::Succeeded || !Refreshed.bCommitted)
	{
		SetASCFailure(History, Refreshed);
		return History;
	}
	AGGYGOCombatantState* Host = OriginalHost.Get();
	if (!Host || !OriginalScope())
	{
		History.Outcome = EOutcome::Stale;
		History.Reason = EHostReason::CallerInvalidated;
		return History;
	}
	Host->AvatarResourceContext = Refreshed.CommittedContext;
	History.Outcome = EOutcome::Succeeded;
	History.Reason = EHostReason::None;
	const FGGYGOAvatarBindingResult Published = Host->PublishAvatarResources(Publication,
		Refreshed.CommittedContext, Request.ExpectedResource, Extension, false, History);
	if (Published.Outcome != EOutcome::Succeeded)
	{
		SetASCFailure(History, Published, EHostReason::PublicationFailed);
	}
	return History;
}

bool AGGYGOCombatantState::InitializeOwnerActorInfo()
{
	if (!AbilitySystemComponent || AvatarResource.HasResource() || !IsAvatarBindingPermitted())
	{
		return false;
	}
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const TWeakObjectPtr<UGGYGOAbilitySystemComponent> OriginalASC(AbilitySystemComponent);
	const TWeakObjectPtr<APawn> SelectedAvatar(AvatarPawn);
	const FGGYGOAvatarBindingContext Context = AbilitySystemComponent->GetAvatarBindingContext();
	EGGYGOAvatarBindingReason Reason;
	if (AbilitySystemComponent->CheckAvatarBindingContext(Context, Reason) == EOutcome::Succeeded
		&& AbilitySystemComponent->GetOwnerActor() == this && !AbilitySystemComponent->GetAvatarActor())
	{
		return true;
	}
	const auto Query = [&]()
	{
		AGGYGOCombatantState* Host = OriginalHost.Get();
		return Host && Host->IsAvatarBindingPermitted() && !Host->AvatarResource.HasResource()
			&& SelectedAvatar.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn));
	};
	FGGYGOAvatarBindingRequest Native;
	Native.Kind = EGGYGOAvatarBindingKind::Init;
	Native.ExpectedContext = Context;
	Native.OwnerActor = this;
	Native.IsRequestContextCurrent = Query;
	FGGYGOAvatarBindingPublicationReceipt Publication;
	const FGGYGOAvatarBindingResult Initialized = ExecuteHostActorInfo(AbilitySystemComponent, Native, Publication);
	if (Initialized.Outcome != EOutcome::Succeeded || !Initialized.bCommitted)
	{
		FHostResult FailureHistory;
		AppendASCStep(FailureHistory, EHostStep::ActorInfoInit, Initialized);
		SetASCFailure(FailureHistory, Initialized);
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("[Combatants] Owner-only initialization failed: Host=%s ASC=%s Operation=%llu Outcome=%u Reason=%u."),
			*GetPathNameSafe(OriginalHost.Get()), *GetPathNameSafe(OriginalASC.Get()),
			static_cast<unsigned long long>(Initialized.Operation.Serial),
			static_cast<uint32>(Initialized.Outcome), static_cast<uint32>(Initialized.Reason));
		if (!Initialized.bCommitted && Initialized.Operation.HasIssuedIdentity())
		{
			const auto FailedInitCleanupScope = [&]()
			{
				AGGYGOCombatantState* Host = OriginalHost.Get();
				UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get();
				return Host && LiveASC && Host->AbilitySystemComponent == LiveASC && LiveASC->GetOwner() == Host
					&& !Host->AvatarResource.HasResource()
					&& SelectedAvatar.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Host->AvatarPawn));
			};
			if (UGGYGOAbilitySystemComponent* LiveASC = OriginalASC.Get())
			{
				const FGGYGOAvatarBindingResult Cleanup = LiveASC->TryCleanupFailedAvatarActorInfoInit(
					Initialized.Operation, FailedInitCleanupScope);
				AppendASCStep(FailureHistory, EHostStep::ActorInfoClear, Cleanup);
				UE_LOG(LogGGYGOAbilitySystem, Warning,
					TEXT("[Combatants] Owner-only failed Init cleanup returned: Host=%s ASC=%s Operation=%llu Outcome=%u Reason=%u Committed=%d Steps=%d; original Outcome=%u Reason=%u remains failed."),
					*GetPathNameSafe(OriginalHost.Get()), *GetPathNameSafe(OriginalASC.Get()),
					static_cast<unsigned long long>(Initialized.Operation.Serial), static_cast<uint32>(Cleanup.Outcome),
					static_cast<uint32>(Cleanup.Reason), Cleanup.bCommitted ? 1 : 0, FailureHistory.Steps.Num(),
					static_cast<uint32>(FailureHistory.Outcome), static_cast<uint32>(FailureHistory.Reason));
			}
			else
			{
				UE_LOG(LogGGYGOAbilitySystem, Warning,
					TEXT("[Combatants] Owner-only failed Init cleanup not called: Host=%s Operation=%llu; original ASC expired."),
					*GetPathNameSafe(OriginalHost.Get()), static_cast<unsigned long long>(Initialized.Operation.Serial));
			}
		}
		return false;
	}
	UGGYGOAbilitySystemComponent* ASC = OriginalASC.Get();
	if (!ASC)
	{
		return false;
	}
	const FGGYGOAvatarBindingResult Published = ASC->PublishAvatarBindingNotice(Publication, Query);
	if (Published.Outcome != EOutcome::Succeeded)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] Owner-only publication stopped: Host=%s Outcome=%u Reason=%u."),
			*GetPathNameSafe(OriginalHost.Get()), static_cast<uint32>(Published.Outcome), static_cast<uint32>(Published.Reason));
		return false;
	}
	return Query();
}

FHostResult AGGYGOCombatantState::CoordinateAvatarSelection(APawn* DesiredAvatar)
{
	const TWeakObjectPtr<AGGYGOCombatantState> OriginalHost(this);
	const TWeakObjectPtr<APawn> SelectedAvatar(DesiredAvatar);
	FHostResult History = HostResult(EOutcome::Succeeded, EHostReason::None);
	if (!AbilitySystemComponent)
	{
		return HostResult(EOutcome::Failed, EHostReason::InvalidASC);
	}
	if (AbilitySystemComponent->IsAvatarBindingNativeWriteBusy())
	{
		return HostResult(EOutcome::Busy, EHostReason::NativeWriteBusy);
	}
	FGGYGOAvatarBindingContext NextContext = AbilitySystemComponent->GetAvatarBindingContext();
	if (DesiredAvatar)
	{
		if (!ValidateAvatarBinding(DesiredAvatar, TEXT("CoordinateAvatarSelection")))
		{
			return HostResult(EOutcome::Rejected, EHostReason::LifecycleClosed);
		}
		UGGYGOPawnExtensionComponent* TargetExtension =
			UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(DesiredAvatar);
		if (!TargetExtension)
		{
			return HostResult(EOutcome::Rejected, EHostReason::InvalidExtension);
		}
		const FGGYGOPawnASCResourceHandle Target = TargetExtension->GetCurrentLocalAbilitySystemResource();
		if (Target.HasResource() && !AvatarResource.HasSameResource(Target))
		{
			return HostResult(EOutcome::Rejected, EHostReason::ResourceConflict);
		}
		if (AvatarResource.HasResource() && !AvatarResource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(SelectedAvatar))
		{
			if (AbilitySystemComponent->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dying)
				|| AbilitySystemComponent->HasMatchingGameplayTag(GGYGOGameplayTags::State_Dead))
			{
				return HostResult(EOutcome::Rejected, EHostReason::LifecycleClosed);
			}
		}
	}
	if (AvatarResource.HasResource()
		&& (!DesiredAvatar || !AvatarResource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(SelectedAvatar)))
	{
		const FHostResult Released = RequestAvatarBinding(MakeAvatarResourceRequest(EHostOperation::Release));
		History.Steps.Append(Released.Steps);
		if (Released.Outcome != EOutcome::Succeeded)
		{
			History.Outcome = Released.Outcome;
			History.Reason = Released.Reason;
			return History;
		}
		if (!FindClearCommit(Released, NextContext))
		{
			History.Outcome = EOutcome::Failed;
			History.Reason = EHostReason::NativeStepFailed;
			return History;
		}
		AGGYGOCombatantState* Host = OriginalHost.Get();
		EGGYGOAvatarBindingReason Reason;
		if (!Host || Host->AvatarResource.HasResource()
			|| Host->AbilitySystemComponent->CheckAvatarBindingContext(NextContext, Reason) != EOutcome::Succeeded
			|| (DesiredAvatar && (!SelectedAvatar.IsValid() || !Host->IsAvatarBindingPermitted())))
		{
			History.Outcome = EOutcome::Stale;
			History.Reason = EHostReason::CallerInvalidated;
			return History;
		}
	}
	if (!DesiredAvatar)
	{
		return History;
	}
	AGGYGOCombatantState* Host = OriginalHost.Get();
	if (!Host || !SelectedAvatar.IsValid())
	{
		History.Outcome = EOutcome::Stale;
		History.Reason = EHostReason::CallerInvalidated;
		return History;
	}
	FHostRequest Initialize;
	Initialize.Operation = EHostOperation::Initialize;
	Initialize.ExpectedHost = Host;
	Initialize.ExpectedASC = Host->AbilitySystemComponent;
	Initialize.ExpectedPawn = SelectedAvatar;
	Initialize.ExpectedExtension = UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(SelectedAvatar.Get());
	Initialize.ExpectedContext = NextContext;
	const FHostResult Initialized = Host->RequestAvatarBinding(Initialize);
	History.Steps.Append(Initialized.Steps);
	History.Outcome = Initialized.Outcome;
	History.Reason = Initialized.Reason;
	return History;
}

void AGGYGOCombatantState::AttachAvatar(APawn* NewAvatar)
{
	if (!HasAuthority())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("[Combatants] AttachAvatar authority denied: Host=%s Pawn=%s."),
			*GetPathNameSafe(this), *GetPathNameSafe(NewAvatar));
		return;
	}
	const FHostResult Result = CoordinateAvatarSelection(NewAvatar);
	if (Result.Outcome != EOutcome::Succeeded)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] AttachAvatar stopped: Host=%s Pawn=%s Outcome=%u Reason=%u."),
			*GetPathNameSafe(this), *GetPathNameSafe(NewAvatar),
			static_cast<uint32>(Result.Outcome), static_cast<uint32>(Result.Reason));
	}
}

void AGGYGOCombatantState::DetachAvatar(APawn* ExpectedAvatar)
{
	if (!HasAuthority() || !AvatarResource.HasResource())
	{
		return;
	}
	if (ExpectedAvatar && !AvatarResource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(ExpectedAvatar)))
	{
		return;
	}
	RequestAvatarBinding(MakeAvatarResourceRequest(EHostOperation::Release));
}

void AGGYGOCombatantState::OnRep_AvatarPawn()
{
	SynchronizeAvatarBinding();
}

void AGGYGOCombatantState::HandleAvatarDestroyed(AActor* DestroyedActor)
{
	APawn* Pawn = Cast<APawn>(DestroyedActor);
	if (Pawn && AvatarResource.HasResource()
		&& AvatarResource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(Pawn)))
	{
		RequestAvatarBinding(MakeAvatarResourceRequest(EHostOperation::Release));
	}
}

void AGGYGOCombatantState::SynchronizeAvatarBinding()
{
	const FHostResult Result = CoordinateAvatarSelection(AvatarPawn);
	if (Result.Outcome != EOutcome::Succeeded)
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Combatants] SynchronizeAvatarBinding stopped: Host=%s ASC=%s Outcome=%u Reason=%u."),
			*GetPathNameSafe(this), *GetPathNameSafe(AbilitySystemComponent),
			static_cast<uint32>(Result.Outcome), static_cast<uint32>(Result.Reason));
	}
}

void AGGYGOCombatantState::ClearLocalAvatarBinding(APawn* AvatarToClean)
{
	if (AvatarResource.HasResource()
		&& AvatarResource.GetIdentity().Pawn.HasSameIndexAndSerialNumber(TWeakObjectPtr<APawn>(AvatarToClean)))
	{
		RequestAvatarBinding(MakeAvatarResourceRequest(EHostOperation::Release));
	}
}

bool AGGYGOCombatantState::IsAvatarBindingPermitted() const
{
	return bAvatarBindingPermitted && IsValid(this) && !IsActorBeingDestroyed();
}

bool AGGYGOCombatantState::ValidateAvatarBinding(APawn* AvatarToBind, const TCHAR* EntryPoint) const
{
	const TCHAR* RejectionReason = nullptr;
	if (!IsAvatarBindingPermitted())
	{
		RejectionReason = !IsValid(this) ? TEXT("宿主无效或已进入垃圾回收状态")
			: IsActorBeingDestroyed() ? TEXT("宿主正在原生销毁流程中")
			: TEXT("本次生命周期的绑定准入已关闭");
	}
	else if (!IsValid(AvatarToBind))
	{
		RejectionReason = TEXT("目标 Pawn 无效或已进入垃圾回收状态");
	}
	else if (AvatarToBind->IsActorBeingDestroyed())
	{
		RejectionReason = TEXT("目标 Pawn 正在原生销毁流程中");
	}
	if (!RejectionReason)
	{
		return true;
	}
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("[Combatants] %s: 宿主 [%s] ASC [%s] 拒绝绑定 Avatar [%s]，原因：%s。"),
		EntryPoint, *GetPathNameSafe(this), *GetPathNameSafe(AbilitySystemComponent),
		*GetPathNameSafe(AvatarToBind), RejectionReason);
	return false;
}
