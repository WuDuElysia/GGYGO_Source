/** @file GGYGOMontageFXPublication.cpp @brief 唯一派生发布集及本地原资源准入。 */
#include "FX/GGYGOMontageFXPublication.h"

#include "Character/GGYGOCharacterBase.h"
#include "FX/Components/GGYGORemoteMontageFXComponent.h"
#include "AbilitySystemComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/WorldSettings.h"
#include "UObject/StrongObjectPtr.h"
#if WITH_EDITOR
#include "CoreGlobals.h"
#include "NiagaraSystem.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOMontageFX, Log, All);

#if WITH_EDITOR
namespace
{
void CopyObservationRecord(const FGGYGOMontageFXRecord& Record, FGGYGOMontageFXObservationFact& Fact)
{
	Fact.bHasRecord = true;
	Fact.Occurrence = Record.Occurrence;
	Fact.BeginServerTime = Record.BeginServerTime;
	Fact.Avatar = Record.Avatar.Get();
	Fact.Mesh = Record.Mesh.Get();
	Fact.System = Record.System.Get();
	if (const auto* Avatar = Record.Avatar.Get())
	{ Fact.AvatarObjectId = Avatar->GetUniqueID(); Fact.AvatarPath = Avatar->GetPathName(); }
	if (const auto* Mesh = Record.Mesh.Get())
	{ Fact.MeshObjectId = Mesh->GetUniqueID(); Fact.MeshPath = Mesh->GetPathName(); }
	Fact.SystemPath = GetPathNameSafe(Record.System.Get());
	Fact.RemoteSeekDeltaSeconds = Record.RemoteSeekDeltaSeconds;
	Fact.RemoteMaxSimTimeSeconds = Record.RemoteMaxSimTimeSeconds;
}
}

void FGGYGOMontageFXObservation::Add(const FGGYGOMontageFXObservationFact& Fact)
{
	check(IsInGameThread());
	if (!IsActive() || Fact.OwnerRegistration != Registration) { return; }
	if (Pending.Num() >= Capacity) { bOverflowed = true; return; }
	Pending.Add(Fact);
	auto& Copy = Pending.Last();
	Copy.NativeFrame = GFrameCounter;
	Copy.Publisher = Publisher;
	Copy.Issuer = Issuer;
	Copy.World = World;
	Copy.IssuerObjectId = IssuerObjectId;
	Copy.WorldObjectId = WorldObjectId;
}

bool FGGYGOMontageFXObservation::Drain(TArray<FGGYGOMontageFXObservationFact>& Out, FString& Error)
{
	check(IsInGameThread());
	Out = MoveTemp(Pending);
	Pending.Reset();
	Error.Reset();
	if (bOverflowed) { Error = TEXT("FX 观察窗容量溢出；保留已采事实，本窗不能作为完整证据。"); return false; }
	if (bStopped) { Error = TEXT("原 FX 观察窗已停止。"); return false; }
	return true;
}

void FGGYGOMontageFXObservation::Stop()
{
	check(IsInGameThread());
	bStopped = true;
	Pending.Reset();
	Publisher.Reset();
	Issuer.Reset();
	World.Reset();
}
#endif

namespace
{
/** 原引擎网络引用的本地身份。Actor 失相关重建 UObject 时，仍能认出既存 World 资源。 */
struct FNativeFXAvatarReference
{
	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<UNetDriver> Driver;
	FNetworkGUID Guid;
	FNativeFXAvatarReference() = default;
	explicit FNativeFXAvatarReference(AActor* InActor) { *this = InActor; }
	FNativeFXAvatarReference& operator=(AActor* InActor)
	{
		Actor = InActor;
		World = InActor ? InActor->GetWorld() : nullptr;
		Driver = World.IsValid() ? World->GetNetDriver() : nullptr;
		Guid = {};
		if (Driver.IsValid() && Driver->GetNetGuidCache().IsValid())
		{ Guid = Driver->GetNetGuidCache()->GetNetGUID(InActor); }
		return *this;
	}
	AActor* Get() const { return Actor.Get(); }
	bool IsValid() const { return Actor.IsValid() && !Actor->IsActorBeingDestroyed(); }
	bool operator==(const FNativeFXAvatarReference& Other) const
	{
		if (Actor.IsValid() && Actor == Other.Actor) { return true; }
		return Guid.IsValid() && Other.Guid.IsValid() && World == Other.World
			&& Driver.IsValid() && Driver == Other.Driver && Guid == Other.Guid;
	}
	bool Matches(AActor* Other) const { return *this == FNativeFXAvatarReference(Other); }
	bool ProvesDifferent(AActor* Other) const
	{
		const FNativeFXAvatarReference Reference(Other);
		if (*this == Reference) { return false; }
		if (World.IsValid() && Reference.World.IsValid() && World != Reference.World) { return true; }
		if (Guid.IsValid() && Reference.Guid.IsValid() && Driver == Reference.Driver) { return Guid != Reference.Guid; }
		if (IsValid() && Reference.IsValid()) { return true; }
		return World.IsValid() && World == Reference.World && World->GetNetMode() == NM_Standalone;
	}
};
}

struct FGGYGOMontageFXLeaseState
{
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Issuer;
	uint64 Occurrence = 0;
	uint64 Registration = 0;
	bool bClosed = false;
};

/** 工厂签发，Task 唯一写实际资源事实；未生产与最终归还不是同一个事实。 */
struct FGGYGOMontageFXLocalCapability
{
	TWeakObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> Task;
	FNativeFXAvatarReference Avatar;
	uint64 Nonce = 0;
	uint64 Registration = 0;
	int32 Operations = 0;
	bool bOwned = false;
	bool bClosed = false;
	bool bReleased = false;
	bool bStartupGranted = false;
	bool bLostOwnerReported = false;
};

struct UGGYGOMontageFXPublicationOwner::FState
{
	TWeakObjectPtr<UAbilitySystemComponent> Issuer;
	TWeakObjectPtr<UWorld> World;
	FGGYGOMontageFXState* Snapshot = nullptr;
	FGGYGOMontageFXSendTerminal Send;
	/** Epoch 随实际 ASC owner 产生；Registration 只随同 owner 重注册推进。 */
	uint64 Epoch = 0;
	uint64 NextId = 0;
	uint64 NextOccurrence = 0;
	/** 只由本地资源写端变更，用于失效 NoProduction/Ready 观察，不上网络。 */
	uint64 ResourceRevision = 1;
	uint64 Registration = 0;
	uint64 ReceivedRevision = 0;
	bool bClosed = false;
	bool bProcessing = false;
	bool bChanged = false;
	struct FBinding
	{
		TSharedPtr<const void> Source;
		TSharedPtr<FGGYGOMontageFXLeaseState> Lease;
	};
	struct FRemote
	{
		TWeakObjectPtr<UGGYGORemoteMontageFXComponent> Component;
		FNativeFXAvatarReference Avatar;
		uint64 Generation = 0;
		uint64 Registration = 0;
		int32 Operations = 0;
		bool bReleased = false;
		bool bNaturalCompletion = false;
		bool bFailed = false;
		bool bReleaseRequested = false;
		bool bLostResourceReported = false;
		EGGYGONiagaraEffectStopMode ReleaseMode = EGGYGONiagaraEffectStopMode::DestroyImmediately;
	};
	struct FRequest
	{
		TSharedPtr<FGGYGOMontageFXLocalCapability> Capability;
		FGGYGOMontageFXLocalStartReadyDelegate Observer;
		uint64 LastNotifiedRevision = 0;
		bool bConsumed = false;
		bool bCancelled = false;
		bool bEvaluating = false;
	};
	struct FAvatarSubscription
	{
		TWeakObjectPtr<APawn> Pawn;
		FDelegateHandle Role;
	};
	/** 服务端仅保存原 source 与 lease 关联，payload 唯一位于 Snapshot。 */
	TMap<uint64, FBinding> Bindings;
	/** 接收过准确 Terminal 后保留，直到 revision/实际成员/资源三项均覆盖。 */
	TMap<uint64, FGGYGOMontageFXTerminal> ClosedKeys;
	TMap<uint64, TSharedPtr<FGGYGOMontageFXLocalCapability>> Local;
	TMap<uint64, FRequest> Requests;
	TMap<uint64, FRemote> Remote;
	TArray<FAvatarSubscription> Avatars;

	uint64 IssueId()
	{
		checkf(NextId != MAX_uint64, TEXT("Montage FX local identity exhausted"));
		return ++NextId;
	}
	bool HasRemoteResource(AActor* Avatar) const
	{
		for (const auto& Pair : Remote)
		{
			if (Pair.Value.Avatar.Matches(Avatar) && (!Pair.Value.bReleased || Pair.Value.Operations != 0)) { return true; }
		}
		return false;
	}
	bool RequestReady(const FRequest& Request) const
	{
		const auto& Cap = Request.Capability;
		if (bClosed || Request.bCancelled || Request.bEvaluating || !Cap.IsValid() || Cap->bClosed
			|| Cap->Registration != Registration || !Cap->Avatar.IsValid()
			|| !Cap->Task.IsValid() || HasRemoteResource(Cap->Avatar.Get())) { return false; }
		for (const auto& Pair : Local)
		{
			const auto& Other = Pair.Value;
			if (Other != Cap && Other->Avatar == Cap->Avatar
				&& (Other->bOwned || Other->Operations > 0 || Other->bStartupGranted)) { return false; }
		}
		return true;
	}
};

bool FGGYGOMontageFXRecord::Validate(FString& OutError) const
{
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfiguration(System, Space, Transform, OutError)) { return false; }
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::IsStopModeValid(NormalStopMode)
		|| !UGGYGOAbilityTask_PlayNiagaraEffect::IsStopModeValid(AbortStopMode)
		|| !FMath::IsFinite(RemoteSeekDeltaSeconds) || RemoteSeekDeltaSeconds <= 0.f
		|| !FMath::IsFinite(RemoteMaxSimTimeSeconds) || RemoteMaxSimTimeSeconds <= 0.f
		|| !FMath::IsFinite(BeginServerTime) || BeginServerTime < 0.)
	{
		OutError = TEXT("Montage FX requires explicit stop modes and positive finite remote seek step/budget.");
		return false;
	}
	return true;
}

const FGGYGOMontageFXRecord* FGGYGOMontageFXState::Find(uint64 Occurrence) const
{
	return ActiveRecords.FindByPredicate([Occurrence](const FGGYGOMontageFXRecord& Record)
	{ return Record.Occurrence == Occurrence; });
}

uint64 FGGYGOMontageFXLease::GetOccurrence() const { return State.IsValid() ? State->Occurrence : 0; }

FGGYGOMontageFXCloseObligation::FGGYGOMontageFXCloseObligation(FGGYGOMontageFXCloseObligation&& Other)
{ *this = MoveTemp(Other); }
FGGYGOMontageFXCloseObligation& FGGYGOMontageFXCloseObligation::operator=(FGGYGOMontageFXCloseObligation&& Other)
{
	if (this != &Other)
	{
		Issuer = Other.Issuer;
		Terminal = Other.Terminal;
#if WITH_EDITOR
		Observation = Other.Observation;
		Other.Observation = {};
#endif
		Other.Issuer.Reset();
		Other.Terminal = {};
	}
	return *this;
}

UGGYGOMontageFXPublicationOwner::UGGYGOMontageFXPublicationOwner() : Data(MakeUnique<FState>()) {}
UGGYGOMontageFXPublicationOwner::UGGYGOMontageFXPublicationOwner(FVTableHelper& Helper) : Super(Helper) {}
UGGYGOMontageFXPublicationOwner::~UGGYGOMontageFXPublicationOwner() = default;

UGGYGOMontageFXPublicationOwner* UGGYGOMontageFXPublicationOwner::RegisterHost(UAbilitySystemComponent* Issuer,
	FGGYGOMontageFXState& State, FGGYGOMontageFXSendTerminal Send)
{
	if (!IsValid(State.LocalOwner)) { State.LocalOwner = NewObject<ThisClass>(Issuer); }
	State.LocalOwner->Initialize(Issuer, State, MoveTemp(Send));
	return State.LocalOwner;
}

uint64 UGGYGOMontageFXPublicationOwner::GetRegistrationSerial() const { return Data->Registration; }

#if WITH_EDITOR
void UGGYGOMontageFXPublicationOwner::FillObservationContext(FGGYGOMontageFXObservationFact& Fact) const
{
	Fact.Publisher = const_cast<ThisClass*>(this);
	Fact.Issuer = Data->Issuer;
	Fact.World = Data->World;
	if (const auto* Issuer = Data->Issuer.Get()) { Fact.IssuerObjectId = Issuer->GetUniqueID(); }
	if (const auto* World = Data->World.Get()) { Fact.WorldObjectId = World->GetUniqueID(); }
	if (Fact.OwnerRegistration == 0) { Fact.OwnerRegistration = Data->Registration; }
	if (Data->Snapshot && Fact.OwnerRegistration == Data->Registration)
	{ Fact.StateRevision = Data->Snapshot->StateRevision; }
}

void UGGYGOMontageFXPublicationOwner::CaptureObservationFact(uint64 Key, FGGYGOMontageFXObservationFact& Fact) const
{
	Fact.Occurrence = Key;
	if (const auto* Remote = Data->Remote.Find(Key))
	{
		if (const auto* Component = Remote->Component.Get()) { Component->ReadObservation(Fact); }
		Fact.bRemoteEntry = true;
		Fact.Generation = Remote->Generation;
		Fact.OwnerRegistration = Remote->Registration;
		Fact.Operations = Remote->Operations;
		Fact.bReleaseRequested = Remote->bReleaseRequested;
		Fact.bReleased = Remote->bReleased;
		Fact.bNaturalCompletion |= Remote->bNaturalCompletion;
		Fact.bFailed |= Remote->bFailed;
	}
	if (Data->Snapshot)
	{
		if (const auto* Record = Data->Snapshot->Find(Key))
		{ CopyObservationRecord(*Record, Fact); Fact.bActiveRecord = true; }
	}
	if (const auto* Terminal = Data->ClosedKeys.Find(Key))
	{ Fact.bHasTerminal = true; Fact.Terminal = *Terminal; }
	FillObservationContext(Fact);
}

void UGGYGOMontageFXPublicationOwner::AppendObservation(FGGYGOMontageFXObservationFact Fact)
{
	FillObservationContext(Fact);
	ObservationWindows.RemoveAll([](const auto& Weak)
	{ const auto Window = Weak.Pin(); return !Window.IsValid() || !Window->IsActive(); });
	for (const auto& Weak : ObservationWindows)
	{ if (const auto Window = Weak.Pin()) { Window->Add(Fact); } }
}

TSharedPtr<FGGYGOMontageFXObservation> UGGYGOMontageFXPublicationOwner::BeginObservation(int32 MaxRecords, FString& Error)
{
	check(IsInGameThread());
	Error.Reset();
	if (MaxRecords < 1 || MaxRecords > 4096 || Data->bClosed || !Data->Issuer.IsValid()
		|| !Data->Snapshot || !Data->World.IsValid() || Data->World->bIsTearingDown)
	{ Error = TEXT("FX 观察需要实际原 owner／World、活动注册及 1..4096 容量。"); return nullptr; }
	const auto Window = MakeShared<FGGYGOMontageFXObservation>();
	Window->Publisher = this;
	Window->Issuer = Data->Issuer;
	Window->World = Data->World;
	Window->IssuerObjectId = Data->Issuer->GetUniqueID();
	Window->WorldObjectId = Data->World->GetUniqueID();
	Window->Registration = Data->Registration;
	Window->Capacity = MaxRecords;
	ObservationWindows.RemoveAll([](const auto& Weak)
	{ const auto Existing = Weak.Pin(); return !Existing.IsValid() || !Existing->IsActive(); });
	ObservationWindows.Add(Window);
	for (const auto& Pair : Data->Remote)
	{ if (auto* Component = Pair.Value.Component.Get()) { Component->AddObservationWindow(Window); } }
	if (!CaptureObservation(Window, Error)) { Window->Stop(); return nullptr; }
	return Window;
}

bool UGGYGOMontageFXPublicationOwner::CaptureObservation(
	const TSharedPtr<FGGYGOMontageFXObservation>& Window, FString& Error) const
{
	check(IsInGameThread());
	Error.Reset();
	if (!Window.IsValid() || !Window->IsActive() || Window->Publisher.Get() != this
		|| Window->Registration != Data->Registration || Window->World != Data->World
		|| Window->Issuer != Data->Issuer || !Data->Issuer.IsValid()
		|| !Data->World.IsValid() || Data->World->bIsTearingDown)
	{ Error = TEXT("FX 原观察窗已失效、溢出或 owner 注册已被后继替换；须先 Drain 原事实再 Stop。"); return false; }
	TSet<uint64> Keys;
	if (Data->Snapshot) { for (const auto& Record : Data->Snapshot->ActiveRecords) { Keys.Add(Record.Occurrence); } }
	for (const auto& Pair : Data->Remote) { Keys.Add(Pair.Key); }
	for (const auto& Pair : Data->ClosedKeys) { Keys.Add(Pair.Key); }
	TArray<uint64> OrderedKeys = Keys.Array();
	OrderedKeys.Sort();
	if (OrderedKeys.IsEmpty())
	{
		FGGYGOMontageFXObservationFact Fact;
		Fact.Kind = Data->bClosed ? EGGYGOMontageFXObservationKind::PublisherClosed : EGGYGOMontageFXObservationKind::Snapshot;
		FillObservationContext(Fact);
		Window->Add(Fact);
	}
	for (uint64 Key : OrderedKeys)
	{
		FGGYGOMontageFXObservationFact Fact;
		CaptureObservationFact(Key, Fact);
		Window->Add(Fact);
	}
	if (!Window->IsActive()) { Error = TEXT("FX 当前快照超过原观察窗容量。"); return false; }
	return true;
}
#endif

void UGGYGOMontageFXPublicationOwner::Initialize(UAbilitySystemComponent* Issuer, FGGYGOMontageFXState& State,
	FGGYGOMontageFXSendTerminal Send)
{
	check(IsInGameThread());
	if (Data->Epoch != 0)
	{
		check(Data->Issuer.Get() == Issuer && (!Data->Snapshot || Data->Snapshot == &State));
		Data->Snapshot = &State;
		Data->World = Issuer->GetWorld();
		Data->Send = MoveTemp(Send);
		Data->bClosed = false;
		++Data->Registration;
		++Data->ResourceRevision;
		Data->NextOccurrence = FMath::Max(Data->NextOccurrence, State.StateRevision);
		return;
	}
	check(IsValid(Issuer) && IsValid(Issuer->GetWorld()));
	static uint64 NextScopeEpoch = 0;
	check(NextScopeEpoch != MAX_uint64);
	Data->Epoch = ++NextScopeEpoch;
	Data->Registration = 1;
	Data->Issuer = Issuer;
	Data->World = Issuer->GetWorld();
	Data->Snapshot = &State;
	Data->NextOccurrence = State.StateRevision;
	Data->Send = MoveTemp(Send);
}

bool UGGYGOMontageFXPublicationOwner::Begin(const TSharedPtr<const void>& Source, const FGGYGOMontageFXRecord& Request,
	FGGYGOMontageFXLease& OutLease, FString& OutError)
{
	OutLease = {};
	UAbilitySystemComponent* Issuer = Data->Issuer.Get();
	UWorld* World = Data->World.Get();
	if (Data->bClosed || !Issuer || !IsValid(Issuer->GetOwner()) || !Issuer->GetOwner()->HasAuthority()
		|| !World || World->bIsTearingDown || !Data->Snapshot || !Source.IsValid()
		|| !IsValid(Request.Avatar) || Request.Avatar->IsActorBeingDestroyed()
		|| !IsValid(Request.Mesh) || Request.Avatar->GetWorld() != World
		|| Request.Mesh->GetOwner() != Request.Avatar || !Request.Validate(OutError))
	{
		if (OutError.IsEmpty()) { OutError = TEXT("Montage FX Begin requires original authority/source, Avatar, Mesh and live World."); }
		return false;
	}
	if (Data->NextOccurrence == MAX_uint64 || Data->Snapshot->StateRevision == MAX_uint64)
	{ OutError = TEXT("Montage FX publication identity exhausted."); return false; }
	FGGYGOMontageFXRecord Record = Request;
	Record.Occurrence = ++Data->NextOccurrence;
	Record.BeginServerTime = World->GetTimeSeconds();
	OutLease.State = MakeShared<FGGYGOMontageFXLeaseState>();
	OutLease.State->Issuer = this;
	OutLease.State->Occurrence = Record.Occurrence;
	OutLease.State->Registration = Data->Registration;
	FState::FBinding Binding;
	Binding.Source = Source;
	Binding.Lease = OutLease.State;
	Data->Bindings.Add(Record.Occurrence, MoveTemp(Binding));
	Data->Snapshot->ActiveRecords.Add(Record);
	++Data->Snapshot->StateRevision;
#if WITH_EDITOR
	FGGYGOMontageFXObservationFact Observation;
	Observation.Kind = EGGYGOMontageFXObservationKind::Published;
	Observation.bActiveRecord = true;
	CopyObservationRecord(Record, Observation);
	AppendObservation(Observation);
#endif
	return true;
}

EGGYGOMontageFXCloseResult UGGYGOMontageFXPublicationOwner::PrepareClose(const FGGYGOMontageFXLease& Lease,
	EGGYGOMontageFXCloseReason Reason, FGGYGOMontageFXCloseObligation& Out)
{
	Out = {};
	if (!Lease.State.IsValid() || Lease.State->Issuer.Get() != this
		|| (Reason != EGGYGOMontageFXCloseReason::Normal && Reason != EGGYGOMontageFXCloseReason::Abort))
	{ return EGGYGOMontageFXCloseResult::Rejected; }
	if (Lease.State->bClosed) { return EGGYGOMontageFXCloseResult::AlreadyClosed; }
	if (!Data->Snapshot || !Data->Bindings.Contains(Lease.GetOccurrence()) || Data->Snapshot->StateRevision == MAX_uint64)
	{ return EGGYGOMontageFXCloseResult::Rejected; }
	Lease.State->bClosed = true;
	Out.Issuer = this;
	Out.Terminal.Occurrence = Lease.GetOccurrence();
	Out.Terminal.CloseRevision = ++Data->Snapshot->StateRevision;
	Out.Terminal.Reason = Reason;
#if WITH_EDITOR
	// 仅复制值到原义务；此纯写入口不派发观察，也不读取组件或执行外调。
	Out.Observation.OwnerRegistration = Lease.State->Registration;
	if (const auto* Record = Data->Snapshot->Find(Lease.GetOccurrence()))
	{ CopyObservationRecord(*Record, Out.Observation); }
#endif
	Data->Snapshot->ActiveRecords.RemoveAll([Key = Lease.GetOccurrence()](const FGGYGOMontageFXRecord& Record)
	{ return Record.Occurrence == Key; });
	Data->Bindings.Remove(Lease.GetOccurrence());
	return EGGYGOMontageFXCloseResult::Closed;
}

void UGGYGOMontageFXPublicationOwner::PrepareCloseSource(const TSharedPtr<const void>& Source,
	EGGYGOMontageFXCloseReason Reason, TArray<FGGYGOMontageFXCloseObligation>& Out)
{
	if (!Source.IsValid()) { return; }
	TArray<FGGYGOMontageFXLease> Leases;
	for (const auto& Pair : Data->Bindings)
	{
		if (Pair.Value.Source == Source)
		{
			FGGYGOMontageFXLease Lease;
			Lease.State = Pair.Value.Lease;
			Leases.Add(Lease);
		}
	}
	for (const auto& Lease : Leases)
	{
		FGGYGOMontageFXCloseObligation Obligation;
		if (PrepareClose(Lease, Reason, Obligation) == EGGYGOMontageFXCloseResult::Closed)
		{ Out.Add(MoveTemp(Obligation)); }
	}
}

bool UGGYGOMontageFXPublicationOwner::DispatchClose(FGGYGOMontageFXCloseObligation&& Obligation)
{
	if (!Obligation.IsSet() || Obligation.Issuer.Get() != this || !Data->Send.IsBound()) { return false; }
	const FGGYGOMontageFXTerminal Terminal = Obligation.Terminal;
#if WITH_EDITOR
	FGGYGOMontageFXObservationFact Observation = Obligation.Observation;
	Observation.Kind = EGGYGOMontageFXObservationKind::TerminalDispatched;
	Observation.Occurrence = Terminal.Occurrence;
	Observation.bHasTerminal = true;
	Observation.Terminal = Terminal;
	Observation.StateRevision = Terminal.CloseRevision;
	// 原义务已在安全出口交给发送者；先写有限值窗，没有观察回调能重入 Send。
	AppendObservation(Observation);
#endif
	Obligation = {};
	Data->Send.Execute(Terminal);
	return true;
}

void UGGYGOMontageFXPublicationOwner::GetSourceIdentities(TArray<TSharedPtr<const void>>& Out) const
{
	Out.Reset();
	for (const auto& Pair : Data->Bindings) { Out.AddUnique(Pair.Value.Source); }
}

bool UGGYGOMontageFXPublicationOwner::CanCollectClosedKey(uint64 Received, uint64 Close,
	bool bActive, bool bUnreturned)
{
	return Received >= Close && !bActive && !bUnreturned;
}

void UGGYGOMontageFXPublicationOwner::ReceiveState()
{
	if (Data->bClosed || !Data->Snapshot || !Data->Issuer.IsValid()) { return; }
	if (Data->Snapshot->StateRevision < Data->ReceivedRevision) { return; }
	Data->ReceivedRevision = Data->Snapshot->StateRevision;
	FlushResourceChanges();
}

void UGGYGOMontageFXPublicationOwner::ReceiveTerminal(const FGGYGOMontageFXTerminal& Terminal)
{
	if (Data->bClosed || Terminal.Occurrence == 0 || Terminal.CloseRevision == 0) { return; }
	const auto* Issuer = Data->Issuer.Get();
	if (!Issuer || !IsValid(Issuer->GetOwner()) || Issuer->GetOwner()->HasAuthority()) { return; }
	if (Terminal.Reason != EGGYGOMontageFXCloseReason::Normal && Terminal.Reason != EGGYGOMontageFXCloseReason::Abort)
	{ UE_LOG(LogGGYGOMontageFX, Error, TEXT("Remote FX %llu received an invalid terminal reason."), Terminal.Occurrence); return; }
	// 先按原 key 收仍在持有/返栈的资源，不能先按水位丢弃迟到 Terminal。
	const FGGYGOMontageFXTerminal* Existing = Data->ClosedKeys.Find(Terminal.Occurrence);
	if (!Existing) { Data->ClosedKeys.Add(Terminal.Occurrence, Terminal); }
	const EGGYGOMontageFXCloseReason Reason = Existing ? Existing->Reason : Terminal.Reason;
#if WITH_EDITOR
	FGGYGOMontageFXObservationFact Observation;
	CaptureObservationFact(Terminal.Occurrence, Observation);
	Observation.Kind = EGGYGOMontageFXObservationKind::TerminalReceived;
	Observation.bHasTerminal = true;
	Observation.Terminal = Existing ? *Existing : Terminal;
	AppendObservation(Observation);
#endif
	if (auto* Remote = Data->Remote.Find(Terminal.Occurrence))
	{
		const auto* Component = Remote->Component.Get();
		const auto Mode = Component ? Component->GetStopMode(Reason) : EGGYGONiagaraEffectStopMode::DestroyImmediately;
		ReleaseRemote(Terminal.Occurrence, Mode, true);
	}
	FlushResourceChanges();
}

void UGGYGOMontageFXPublicationOwner::CollectClosedKeys()
{
	if (Data->bClosed || !Data->Snapshot || !Data->Issuer.IsValid()) { return; }
	for (auto It = Data->ClosedKeys.CreateIterator(); It; ++It)
	{
		const auto* Remote = Data->Remote.Find(It.Key());
		const bool bUnreturned = Remote && (!Remote->bReleased || Remote->Operations != 0);
		if (CanCollectClosedKey(Data->ReceivedRevision, It.Value().CloseRevision,
			Data->Snapshot->Find(It.Key()) != nullptr, bUnreturned))
		{
			Data->Remote.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
}

TSharedPtr<FGGYGOMontageFXLocalCapability> UGGYGOMontageFXPublicationOwner::RegisterLocalTask(
	UGGYGOAbilityTask_PlayNiagaraEffect* Task, AActor* Avatar, UWorld* World, FString& OutError)
{
	if (Data->bClosed || !Data->Snapshot || !Data->Issuer.IsValid() || !IsValid(Task)
		|| !IsValid(Avatar) || !IsValid(World) || World != Data->World.Get() || Avatar->GetWorld() != World)
	{ OutError = TEXT("Local FX capability requires the initialized original issuer/Avatar/World scope."); return {}; }
	auto Capability = MakeShared<FGGYGOMontageFXLocalCapability>();
	Capability->Task = Task;
	Capability->Avatar = Avatar;
	Capability->Nonce = Data->IssueId();
	Capability->Registration = Data->Registration;
	Data->Local.Add(Capability->Nonce, Capability);
	++Data->ResourceRevision;
	return Capability;
}

void UGGYGOMontageFXPublicationOwner::BeginLocalOperation(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Cap)
{
	if (Cap.IsValid()) { ++Cap->Operations; ++Data->ResourceRevision; }
}
void UGGYGOMontageFXPublicationOwner::EndLocalOperation(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Cap)
{
	if (Cap.IsValid()) { check(Cap->Operations > 0); --Cap->Operations; ++Data->ResourceRevision; }
}
void UGGYGOMontageFXPublicationOwner::SetLocalOwnership(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Cap, bool bOwned)
{
	if (Cap.IsValid() && Cap->bOwned != bOwned) { Cap->bOwned = bOwned; ++Data->ResourceRevision; }
}
void UGGYGOMontageFXPublicationOwner::SealLocalProducer(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Cap)
{
	if (Cap.IsValid()) { Cap->bClosed = true; Cap->bStartupGranted = false; ++Data->ResourceRevision; }
}

EGGYGOMontageFXLocalResourceFact UGGYGOMontageFXPublicationOwner::ReadLocalResourceFact(AActor* Avatar) const
{
	if (Data->bClosed || Data->Epoch == 0 || !Data->Issuer.IsValid() || !IsValid(Avatar))
	{ return EGGYGOMontageFXLocalResourceFact::Unknown; }
	bool bAny = false;
	bool bAllReleased = true;
	for (const auto& Pair : Data->Local)
	{
		const auto& Cap = Pair.Value;
		if (!Cap->Avatar.Matches(Avatar))
		{
			if (!Cap->Avatar.IsValid() && !Cap->Avatar.ProvesDifferent(Avatar)
				&& (Cap->bOwned || Cap->Operations || Cap->bStartupGranted))
			{ return EGGYGOMontageFXLocalResourceFact::Unknown; }
			continue;
		}
		if (!Cap->Task.IsValid() && !Cap->bClosed && !Cap->bReleased)
		{ return EGGYGOMontageFXLocalResourceFact::Unknown; }
		if (Cap->bOwned || Cap->Operations || Cap->bStartupGranted)
		{ return EGGYGOMontageFXLocalResourceFact::Pending; }
		bAny = true;
		bAllReleased &= Cap->bReleased;
	}
	return bAny && bAllReleased ? EGGYGOMontageFXLocalResourceFact::Released : EGGYGOMontageFXLocalResourceFact::NoProductionNow;
}

EGGYGOMontageFXLocalStartResult UGGYGOMontageFXPublicationOwner::RequestLocalStart(
	const TSharedPtr<FGGYGOMontageFXLocalCapability>& Cap, FGGYGOMontageFXLocalStartTicket& Out, FString& Error)
{
	Out = {};
	if (Data->bClosed || !Cap.IsValid() || Cap->bClosed || !Cap->Task.IsValid() || !Cap->Avatar.IsValid()
		|| Cap->Registration != Data->Registration || !Data->Local.Contains(Cap->Nonce))
	{ Error = TEXT("Original local FX capability has retired."); return EGGYGOMontageFXLocalStartResult::Cancelled; }
	for (const auto& Pair : Data->Requests)
	{
		if (Pair.Value.Capability == Cap)
		{ Error = TEXT("Local FX Task already has a start request."); return EGGYGOMontageFXLocalStartResult::Rejected; }
	}
	Out.ScopeEpoch = Data->Epoch;
	Out.Nonce = Data->IssueId();
	FState::FRequest Request;
	Request.Capability = Cap;
	Request.bEvaluating = true;
	Data->Requests.Add(Out.Nonce, MoveTemp(Request));
	// 记录集先固定；释放中的 re-entry 只能改自己的 generation，不能清后继。
	TArray<uint64> Old;
	for (const auto& Pair : Data->Remote)
	{ if (Pair.Value.Avatar == Cap->Avatar && !Pair.Value.bReleased) { Old.Add(Pair.Key); } }
	for (uint64 Key : Old)
	{
		const auto* Remote = Data->Remote.Find(Key);
		const auto* Component = Remote ? Remote->Component.Get() : nullptr;
		ReleaseRemote(Key, Component ? Component->GetStopMode(EGGYGOMontageFXCloseReason::Abort)
			: EGGYGONiagaraEffectStopMode::DestroyImmediately, false);
	}
	auto* Current = Data->Requests.Find(Out.Nonce);
	if (Current) { Current->bEvaluating = false; }
	if (!Current || Current->bCancelled || Cap->bClosed) { return EGGYGOMontageFXLocalStartResult::Cancelled; }
	if (Data->World.IsValid() && Data->World->GetNetMode() == NM_DedicatedServer)
	{ return EGGYGOMontageFXLocalStartResult::DedicatedServerNoPresentation; }
	return Data->RequestReady(*Current) ? EGGYGOMontageFXLocalStartResult::ReadyToActivate
		: EGGYGOMontageFXLocalStartResult::WaitingForRemoteRelease;
}

bool UGGYGOMontageFXPublicationOwner::ObserveLocalStart(const FGGYGOMontageFXLocalStartTicket& Ticket,
	FGGYGOMontageFXLocalStartReadyDelegate Callback)
{
	if (Ticket.ScopeEpoch != Data->Epoch) { return false; }
	auto* Request = Data->Requests.Find(Ticket.Nonce);
	if (!Request || Request->bCancelled || Request->bConsumed
		|| Request->Capability->Registration != Data->Registration || !Callback.IsBound()) { return false; }
	Request->Observer = MoveTemp(Callback);
	Request->LastNotifiedRevision = 0;
	FlushResourceChanges();
	return true;
}

EGGYGOMontageFXPermitResult UGGYGOMontageFXPublicationOwner::ConsumeLocalStart(const FGGYGOMontageFXLocalStartTicket& Ticket)
{
	if (Ticket.ScopeEpoch != Data->Epoch || Data->bClosed) { return EGGYGOMontageFXPermitResult::Rejected; }
	auto* Request = Data->Requests.Find(Ticket.Nonce);
	if (!Request || Request->bCancelled || Request->Capability->bClosed
		|| Request->Capability->Registration != Data->Registration) { return EGGYGOMontageFXPermitResult::Closed; }
	if (Request->bConsumed) { return EGGYGOMontageFXPermitResult::AlreadyConsumed; }
	if (!Data->RequestReady(*Request))
	{
		Request->bEvaluating = true;
		const auto Avatar = Request->Capability->Avatar;
		TArray<uint64> Old;
		for (const auto& Pair : Data->Remote)
		{ if (Pair.Value.Avatar == Avatar && !Pair.Value.bReleased) { Old.Add(Pair.Key); } }
		for (uint64 Key : Old)
		{
			const auto* Entry = Data->Remote.Find(Key);
			const auto* Component = Entry ? Entry->Component.Get() : nullptr;
			ReleaseRemote(Key, Component ? Component->GetStopMode(EGGYGOMontageFXCloseReason::Abort)
				: EGGYGONiagaraEffectStopMode::DestroyImmediately, false);
		}
		Request = Data->Requests.Find(Ticket.Nonce);
		if (!Request || Request->bCancelled || Request->Capability->bClosed) { return EGGYGOMontageFXPermitResult::Closed; }
		Request->bEvaluating = false;
		if (!Data->RequestReady(*Request)) { return EGGYGOMontageFXPermitResult::StillWaiting; }
	}
	Request->bConsumed = true;
	Request->Observer.Unbind();
	Request->Capability->bStartupGranted = true;
	++Data->ResourceRevision;
	return EGGYGOMontageFXPermitResult::Granted;
}

void UGGYGOMontageFXPublicationOwner::CancelLocalStart(const FGGYGOMontageFXLocalStartTicket& Ticket)
{
	if (Ticket.ScopeEpoch != Data->Epoch) { return; }
	if (auto* Request = Data->Requests.Find(Ticket.Nonce))
	{
		Request->bCancelled = true;
		Request->Observer.Unbind();
		Request->Capability->bStartupGranted = false;
		++Data->ResourceRevision;
	}
}

bool UGGYGOMontageFXPublicationOwner::HasLocalPermit(const FGGYGOMontageFXLocalStartTicket& Ticket) const
{
	const auto* Request = Ticket.ScopeEpoch == Data->Epoch ? Data->Requests.Find(Ticket.Nonce) : nullptr;
	return !Data->bClosed && Request && Request->bConsumed && !Request->bCancelled
		&& !Request->Capability->bClosed && Request->Capability->Registration == Data->Registration;
}

void UGGYGOMontageFXPublicationOwner::ObserveAvatar(AActor* Avatar)
{
	APawn* Pawn = Cast<APawn>(Avatar);
	if (!Pawn || Data->Avatars.ContainsByPredicate([Pawn](const FState::FAvatarSubscription& Entry)
	{ return Entry.Pawn.Get() == Pawn; })) { return; }
	FState::FAvatarSubscription Subscription;
	Subscription.Pawn = Pawn;
	Pawn->ReceiveControllerChangedDelegate.AddUniqueDynamic(this, &ThisClass::HandleControllerChanged);
	if (auto* Character = Cast<AGGYGOCharacterBase>(Pawn))
	{ Subscription.Role = Character->OnReplicatedLocalRoleChanged().AddUObject(this, &ThisClass::HandleRoleChanged); }
	Data->Avatars.Add(Subscription);
}

void UGGYGOMontageFXPublicationOwner::HandleRoleChanged(AGGYGOCharacterBase*) { FlushResourceChanges(); }
void UGGYGOMontageFXPublicationOwner::HandleControllerChanged(APawn*, AController*, AController*) { FlushResourceChanges(); }

void UGGYGOMontageFXPublicationOwner::Reconcile()
{
	if (Data->bClosed || !Data->Snapshot || !Data->World.IsValid() || Data->World->bIsTearingDown) { return; }
	// 目标销毁后弱订阅随之失效；清掉本地索引，存活 Pawn 的准确 handle 保持原样。
	Data->Avatars.RemoveAll([](const FState::FAvatarSubscription& Entry)
	{ return !Entry.Pawn.IsValid(); });
	const TArray<FGGYGOMontageFXRecord> Records = Data->Snapshot->ActiveRecords;
	for (const auto& Record : Records)
	{
		ObserveAvatar(Record.Avatar);
		if (Data->World->GetNetMode() == NM_DedicatedServer || !IsValid(Record.Avatar)
			|| Record.Avatar->IsActorBeingDestroyed() || !IsValid(Record.Mesh)
			|| !IsValid(Record.System) || Data->ClosedKeys.Contains(Record.Occurrence)) { continue; }
		const APawn* Pawn = Cast<APawn>(Record.Avatar);
		if (Record.Avatar->GetLocalRole() != ROLE_SimulatedProxy || (Pawn && Pawn->IsLocallyControlled())) { continue; }
		const auto LocalFact = ReadLocalResourceFact(Record.Avatar);
		if (LocalFact == EGGYGOMontageFXLocalResourceFact::Unknown || LocalFact == EGGYGOMontageFXLocalResourceFact::Pending) { continue; }
		bool bLocalEvaluation = false;
		for (const auto& Pair : Data->Requests)
		{ bLocalEvaluation |= Pair.Value.bEvaluating && Pair.Value.Capability->Avatar.Matches(Record.Avatar); }
		if (bLocalEvaluation) { continue; }
		auto* Existing = Data->Remote.Find(Record.Occurrence);
		if (Existing && !Existing->bReleased && Existing->Component.IsValid())
		{
			Existing->Component->RefreshSourceReferences(Record);
			Existing->Avatar = Record.Avatar;
		}
		if (Existing && (!Existing->bReleased || Existing->Operations || Existing->bNaturalCompletion || Existing->bFailed)) { continue; }
		// 同 scope 旧 native 归还尚未完成时不从另一个 key 绕开。
		if (Data->HasRemoteResource(Record.Avatar))
		{
			bool bReturning = false;
			for (const auto& Pair : Data->Remote)
			{ bReturning |= Pair.Value.Avatar.Matches(Record.Avatar) && Pair.Value.bReleaseRequested && !Pair.Value.bReleased; }
			if (bReturning) { continue; }
		}
		FString Error;
		if (!Record.Validate(Error))
		{
			UE_LOG(LogGGYGOMontageFX, Error, TEXT("Remote FX %llu: %s"), Record.Occurrence, *Error);
			FState::FRemote Failed;
			Failed.Avatar = Record.Avatar;
			Failed.bReleased = true;
			Failed.bFailed = true;
			Data->Remote.Add(Record.Occurrence, Failed);
			continue;
		}
		const uint64 Generation = Data->IssueId();
		FState::FRemote Entry;
		Entry.Avatar = Record.Avatar;
		Entry.Generation = Generation;
		Entry.Registration = Data->Registration;
		Entry.Operations = 1; // 覆盖 NewObject/注册尚未返回时的临时组件。
		Data->Remote.Add(Record.Occurrence, Entry);
		++Data->ResourceRevision;
		UObject* Outer = Record.Space == EGGYGONiagaraEffectSpace::World
			? static_cast<UObject*>(Data->World->GetWorldSettings()) : static_cast<UObject*>(Record.Mesh->GetOwner());
		TStrongObjectPtr<UGGYGORemoteMontageFXComponent> Component(NewObject<UGGYGORemoteMontageFXComponent>(Outer));
		if (auto* Current = Data->Remote.Find(Record.Occurrence); Current && Current->Generation == Generation)
		{
			Current->Component = Component.Get();
			OwnedRemoteComponents.Add(Component.Get());
		}
		Component->InitializePresentation(this, Record, Generation);
#if WITH_EDITOR
		for (const auto& Weak : ObservationWindows)
		{ if (const auto Window = Weak.Pin()) { Component->AddObservationWindow(Window); } }
		FGGYGOMontageFXObservationFact Observation;
		CaptureObservationFact(Record.Occurrence, Observation);
		Observation.Kind = EGGYGOMontageFXObservationKind::RemoteCreated;
		AppendObservation(Observation);
#endif
		if (CanRestore(Record.Occurrence, Generation)) { Component->StartPresentation(); }
		else { Component->DestroyComponent(); }
		EndRemoteOperation(Record.Occurrence, Generation);
	}
}

bool UGGYGOMontageFXPublicationOwner::CanRestore(uint64 Key, uint64 Generation) const
{
	const auto* Entry = Data->Remote.Find(Key);
	if (Data->bClosed || !Data->Snapshot || !Entry || Entry->Generation != Generation || Entry->bReleaseRequested
		|| Data->ClosedKeys.Contains(Key) || !Data->Snapshot->Find(Key)) { return false; }
	const AActor* Avatar = Entry->Avatar.Get();
	const APawn* Pawn = Cast<APawn>(Avatar);
	const auto Local = ReadLocalResourceFact(Entry->Avatar.Get());
	return Avatar && !Avatar->IsActorBeingDestroyed() && Avatar->GetLocalRole() == ROLE_SimulatedProxy && (!Pawn || !Pawn->IsLocallyControlled())
		&& Local != EGGYGOMontageFXLocalResourceFact::Pending && Local != EGGYGOMontageFXLocalResourceFact::Unknown;
}

void UGGYGOMontageFXPublicationOwner::BeginRemoteOperation(uint64 Key, uint64 Generation)
{
	if (auto* Entry = Data->Remote.Find(Key); Entry && Entry->Generation == Generation)
	{ ++Entry->Operations; ++Data->ResourceRevision; }
}
void UGGYGOMontageFXPublicationOwner::EndRemoteOperation(uint64 Key, uint64 Generation)
{
	if (auto* Entry = Data->Remote.Find(Key); Entry && Entry->Generation == Generation)
	{ check(Entry->Operations > 0); --Entry->Operations; ++Data->ResourceRevision; }
#if WITH_EDITOR
	if (const auto* Entry = Data->Remote.Find(Key); !ObservationWindows.IsEmpty() && Entry && Entry->Generation == Generation)
	{
		FGGYGOMontageFXObservationFact Observation;
		CaptureObservationFact(Key, Observation);
		Observation.Kind = EGGYGOMontageFXObservationKind::ResourceChanged;
		AppendObservation(Observation);
	}
#endif
	FlushResourceChanges();
}
void UGGYGOMontageFXPublicationOwner::RemoteDestroyed(uint64 Key, uint64 Generation, bool bNatural, bool bFailed)
{
	if (auto* Entry = Data->Remote.Find(Key); Entry && Entry->Generation == Generation)
	{
		Entry->bReleased = true;
		Entry->bNaturalCompletion |= bNatural;
		Entry->bFailed |= bFailed;
#if WITH_EDITOR
		FGGYGOMontageFXObservationFact Observation;
		CaptureObservationFact(Key, Observation);
		Observation.Kind = EGGYGOMontageFXObservationKind::ResourceChanged;
		AppendObservation(Observation);
#endif
		if (auto* Component = Entry->Component.Get(true)) { OwnedRemoteComponents.Remove(Component); }
		Entry->Component.Reset();
		++Data->ResourceRevision;
	}
}
void UGGYGOMontageFXPublicationOwner::ReleaseRemote(uint64 Key, EGGYGONiagaraEffectStopMode Mode, bool)
{
	auto* Entry = Data->Remote.Find(Key);
	if (!Entry || Entry->bReleased) { return; }
	Entry->bReleaseRequested = true;
	Entry->ReleaseMode = Mode;
	if (Entry->Operations) { return; }
	const uint64 Generation = Entry->Generation;
	TStrongObjectPtr<UGGYGORemoteMontageFXComponent> Component(Entry->Component.Get());
	if (!Component.IsValid())
	{
		// 实际组件的 OnUnregister 应写 Released；弱空本身不是归还证明。
		if (!Entry->bLostResourceReported)
		{
			Entry->bLostResourceReported = true;
			UE_LOG(LogGGYGOMontageFX, Error, TEXT("Remote FX %llu lost its resource without release evidence."), Key);
		}
		return;
	}
	BeginRemoteOperation(Key, Generation);
	Component->ReleasePresentation(Mode);
	RemoteDestroyed(Key, Generation, false);
	EndRemoteOperation(Key, Generation);
}

void UGGYGOMontageFXPublicationOwner::FlushResourceChanges()
{
	const TStrongObjectPtr<ThisClass> KeepAlive(this);
	if (Data->bProcessing) { Data->bChanged = true; return; }
	TGuardValue<bool> Processing(Data->bProcessing, true);
	do
	{
		Data->bChanged = false;
		for (const auto& Pair : Data->Local)
		{
			const auto& Cap = Pair.Value;
			if (!Cap->Task.IsValid() && !Cap->bClosed && !Cap->bReleased && !Cap->bLostOwnerReported)
			{
				Cap->bLostOwnerReported = true;
				UE_LOG(LogGGYGOMontageFX, Error, TEXT("FX witness %llu on '%s' lost its Task before owner closure; admission remains Unknown."),
					Cap->Nonce, *GetNameSafe(Data->Issuer.Get()));
			}
			if (Cap->bClosed && !Cap->bOwned && !Cap->Operations && !Cap->bReleased)
			{ Cap->bReleased = true; ++Data->ResourceRevision; }
		}
		TArray<uint64> Releasing;
		for (const auto& Pair : Data->Remote)
		{ if (Pair.Value.bReleaseRequested && !Pair.Value.bReleased && !Pair.Value.Operations) { Releasing.Add(Pair.Key); } }
		for (uint64 Key : Releasing)
		{ if (const auto* Entry = Data->Remote.Find(Key)) { ReleaseRemote(Key, Entry->ReleaseMode, false); } }
		TArray<uint64> Requests;
		Data->Requests.GetKeys(Requests);
		for (uint64 Id : Requests)
		{
			auto* Request = Data->Requests.Find(Id);
			if (!Request || Request->bConsumed || !Data->RequestReady(*Request) || !Request->Observer.IsBound()
				|| Request->LastNotifiedRevision == Data->ResourceRevision) { continue; }
			Request->LastNotifiedRevision = Data->ResourceRevision;
			const FGGYGOMontageFXLocalStartReadyFact Fact{Request->Capability->Task, {Data->Epoch, Id}, Data->ResourceRevision};
			const auto Callback = Request->Observer;
			Callback.ExecuteIfBound(Fact);
		}
		Reconcile();
		CollectClosedKeys();
		for (auto It = Data->Requests.CreateIterator(); It; ++It)
		{ if (It.Value().Capability->bReleased && !It.Value().Capability->Operations) { It.RemoveCurrent(); } }
		for (auto It = Data->Local.CreateIterator(); It; ++It)
		{ if (It.Value()->bReleased && !It.Value()->Task.IsValid()) { It.RemoveCurrent(); } }
	} while (Data->bChanged && !Data->bClosed);
}

void UGGYGOMontageFXPublicationOwner::Shutdown(uint64 ExpectedRegistration)
{
	const uint64 Closing = ExpectedRegistration ? ExpectedRegistration : Data->Registration;
	const bool bClosingCurrent = Closing == Data->Registration;
	if (Data->bClosed && bClosingCurrent) { return; }
	TArray<FGGYGOMontageFXLease> Leases;
	for (const auto& Pair : Data->Bindings)
	{
		if (Pair.Value.Lease->Registration != Closing) { continue; }
		FGGYGOMontageFXLease Lease; Lease.State = Pair.Value.Lease; Leases.Add(Lease);
	}
	TArray<FGGYGOMontageFXCloseObligation> Obligations;
	for (const auto& Lease : Leases)
	{
		FGGYGOMontageFXCloseObligation Obligation;
		if (PrepareClose(Lease, EGGYGOMontageFXCloseReason::Abort, Obligation) == EGGYGOMontageFXCloseResult::Closed)
		{ Obligations.Add(MoveTemp(Obligation)); }
	}
	if (bClosingCurrent) { Data->bClosed = true; }
	for (auto& Pair : Data->Requests)
	{
		if (Pair.Value.Capability->Registration == Closing)
		{ Pair.Value.bCancelled = true; Pair.Value.Observer.Unbind(); }
	}
	TArray<TWeakObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect>> Tasks;
	for (const auto& Pair : Data->Local)
	{ if (Pair.Value->Registration == Closing && !Pair.Value->bReleased) { Tasks.Add(Pair.Value->Task); } }
	TArray<TPair<uint64, uint64>> Resources;
	for (const auto& Pair : Data->Remote)
	{ if (Pair.Value.Registration == Closing) { Resources.Emplace(Pair.Key, Pair.Value.Generation); } }
	// 先移除旧订阅；外调里重注册可重新安装同对象/函数，不会被旧尾清理移除。
	if (bClosingCurrent)
	{
		for (const auto& Subscription : Data->Avatars)
		{
			if (APawn* Pawn = Subscription.Pawn.Get())
			{
				Pawn->ReceiveControllerChangedDelegate.RemoveDynamic(this, &ThisClass::HandleControllerChanged);
				if (auto* Character = Cast<AGGYGOCharacterBase>(Pawn))
				{ Character->OnReplicatedLocalRoleChanged().Remove(Subscription.Role); }
			}
		}
		Data->Avatars.Reset();
	}
	for (const auto& Task : Tasks) { if (Task.IsValid()) { Task->EndTask(); } }
	for (const auto& Resource : Resources)
	{
		const auto* Entry = Data->Remote.Find(Resource.Key);
		if (Entry && Entry->Generation == Resource.Value)
		{ ReleaseRemote(Resource.Key, EGGYGONiagaraEffectStopMode::DestroyImmediately, true); }
	}
	for (auto& Obligation : Obligations) { DispatchClose(MoveTemp(Obligation)); }
	if (Data->Registration == Closing) { Data->Snapshot = nullptr; }
#if WITH_EDITOR
	FGGYGOMontageFXObservationFact Observation;
	Observation.OwnerRegistration = Closing;
	Observation.Kind = EGGYGOMontageFXObservationKind::PublisherClosed;
	AppendObservation(Observation);
#endif
}
