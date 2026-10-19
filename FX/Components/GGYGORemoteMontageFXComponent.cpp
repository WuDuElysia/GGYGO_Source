/** @file GGYGORemoteMontageFXComponent.cpp @brief 真实原生就绪、追赶达到与尾部交接。 */
#include "FX/Components/GGYGORemoteMontageFXComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGORemoteFX, Log, All);

void UGGYGORemoteMontageFXComponent::InitializePresentation(UGGYGOMontageFXPublicationOwner* Owner,
	const FGGYGOMontageFXRecord& Record, uint64 Generation)
{
	ResourceOwner = Owner;
#if WITH_EDITOR
	ObservationPublisher = Owner;
	ObservationRegistration = Owner->GetRegistrationSerial();
#endif
	Playback = Record;
	ResourceGeneration = Generation;
	bAutoActivate = false;
	SetAutoDestroy(false);
	bAllowAnyoneToDestroyMe = true;
	SetVisibleInRayTracing(false);
	bOriginalForceSolo = GetForceSolo();
	bOriginalRendering = GetRenderingEnabled();
	OriginalAgeMode = GetAgeUpdateMode();
	SetRenderingEnabled(false);
	SetCanRenderWhileSeeking(false);
	SetForceSolo(true);
	SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
	SetSeekDelta(Record.RemoteSeekDeltaSeconds);
	SetMaxSimTime(Record.RemoteMaxSimTimeSeconds);
	SetLockDesiredAgeDeltaTimeToSeekDelta(false);
	SetAsset(Record.System);
	OnSystemFinished.AddUniqueDynamic(this, &ThisClass::HandlePresentationFinished);
}

#if WITH_EDITOR
void UGGYGORemoteMontageFXComponent::ReadObservation(FGGYGOMontageFXObservationFact& Fact) const
{
	check(IsInGameThread());
	Fact = {};
	Fact.Component = const_cast<ThisClass*>(this);
	Fact.ComponentObjectId = GetUniqueID();
	Fact.ComponentPath = GetPathName();
	Fact.Occurrence = Playback.Occurrence;
	Fact.Generation = ResourceGeneration;
	Fact.OwnerRegistration = ObservationRegistration;
	Fact.bHasRecord = true;
	Fact.Avatar = Playback.Avatar.Get();
	Fact.Mesh = Playback.Mesh.Get();
	Fact.System = Playback.System.Get();
	if (const auto* Avatar = Playback.Avatar.Get())
	{ Fact.AvatarObjectId = Avatar->GetUniqueID(); Fact.AvatarPath = Avatar->GetPathName(); }
	if (const auto* Mesh = Playback.Mesh.Get())
	{ Fact.MeshObjectId = Mesh->GetUniqueID(); Fact.MeshPath = Mesh->GetPathName(); }
	Fact.SystemPath = GetPathNameSafe(Playback.System.Get());
	Fact.BeginServerTime = Playback.BeginServerTime;
	Fact.RemoteSeekDeltaSeconds = Playback.RemoteSeekDeltaSeconds;
	Fact.RemoteMaxSimTimeSeconds = Playback.RemoteMaxSimTimeSeconds;
	Fact.DesiredAge = GetDesiredAge();
	Fact.bDesiredAgeValid = !IsBeingDestroyed() && FMath::IsFinite(Fact.DesiredAge);
	Fact.bReached = bReached;
	Fact.bRenderingEnabled = GetRenderingEnabled();
	Fact.bRegistered = IsRegistered();
	Fact.bTailTransferred = bObservationTailTransferred;
	Fact.bNaturalCompletion = bNaturalCompletion || bObservationNaturalCompletion;
	Fact.bFailed = bFailed;
	if (const auto* System = Playback.System.Get())
	{ Fact.EffectiveStep = System->HasFixedTickDelta() ? System->GetFixedTickDeltaTime() : Playback.RemoteSeekDeltaSeconds; }
	if (const UWorld* World = GetWorld())
	{
		if (const AGameStateBase* GameState = World->GetGameState())
		{
			Fact.TargetAge = GameState->GetServerWorldTimeSeconds() - Playback.BeginServerTime;
			Fact.bTargetAgeValid = FMath::IsFinite(Fact.TargetAge) && Fact.TargetAge >= 0.;
		}
	}
	const auto Controller = GetSystemInstanceController();
	Fact.bControllerValid = Controller.IsValid() && Controller->IsValid();
	if (Fact.bControllerValid)
	{
		// 只读公开原生 getter；观察不会 Wait/Finalize、Tick 或修改 DesiredAge。
		Fact.ControllerId = Controller->GetSystemInstanceID();
		Fact.ControllerAge = Controller->GetAge();
		Fact.bControllerAgeValid = FMath::IsFinite(Fact.ControllerAge);
		Fact.bControllerComplete = Controller->IsComplete();
		Fact.bControllerPendingSpawn = Controller->IsPendingSpawn();
		Fact.ActualExecutionState = static_cast<int32>(Controller->GetActualExecutionState());
		Fact.RequestedExecutionState = static_cast<int32>(Controller->GetRequestedExecutionState());
	}
}

void UGGYGORemoteMontageFXComponent::AddObservationWindow(const TSharedPtr<FGGYGOMontageFXObservation>& Window)
{
	check(IsInGameThread());
	if (!Window.IsValid() || !Window->IsActive() || Window->Registration != ObservationRegistration
		|| Window->Publisher != ObservationPublisher) { return; }
	ObservationWindows.RemoveAll([](const auto& Weak)
	{ const auto Existing = Weak.Pin(); return !Existing.IsValid() || !Existing->IsActive(); });
	ObservationWindows.AddUnique(Window);
}

void UGGYGORemoteMontageFXComponent::AppendObservation(EGGYGOMontageFXObservationKind Kind, bool bDestroyed)
{
	ObservationWindows.RemoveAll([](const auto& Weak)
	{ const auto Window = Weak.Pin(); return !Window.IsValid() || !Window->IsActive(); });
	if (ObservationWindows.IsEmpty()) { return; }
	FGGYGOMontageFXObservationFact Fact;
	ReadObservation(Fact);
	Fact.Kind = Kind;
	Fact.bComponentDestroyed = bDestroyed;
	if (Kind == EGGYGOMontageFXObservationKind::NaturalCompleted) { Fact.bNaturalCompletion = true; }
	if (const auto* Publisher = ObservationPublisher.Get()) { Publisher->FillObservationContext(Fact); }
	for (const auto& Weak : ObservationWindows)
	{ if (const auto Window = Weak.Pin()) { Window->Add(Fact); } }
}
#endif

void UGGYGORemoteMontageFXComponent::StartPresentation()
{
	const TWeakObjectPtr<ThisClass> Self(this);
	UWorld* World = GetWorld();
	if (!World || !ResourceOwner.IsValid() || !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration)) { return; }
	FString Error;
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::ValidateCompiledSystem(Playback.System, Error))
	{ FailPresentation(Error); return; }
	if (Playback.Space == EGGYGONiagaraEffectSpace::Attached)
	{
		if (!IsValid(Playback.Mesh) || Playback.Mesh->GetWorld() != World)
		{ FailPresentation(TEXT("Original attachment is in another World.")); return; }
		SetupAttachment(Playback.Mesh, Playback.SocketName);
		SetRelativeTransform(Playback.Transform);
	}
	else
	{
		SetAbsolute(true, true, true);
		SetWorldTransform(Playback.Transform);
	}
	RegisterComponentWithWorld(World);
	if (!Self.IsValid()) { return; }
	if (!ResourceOwner.IsValid() || !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration))
	{ DestroyComponent(); return; }
	// Mesh 注册可晚于引用映射；使用此组件原本的 Tick 等待，不新建轮询任务。
	SetComponentTickEnabled(true);
}

void UGGYGORemoteMontageFXComponent::RefreshSourceReferences(const FGGYGOMontageFXRecord& Record)
{
	if (Record.Occurrence != Playback.Occurrence || Playback.Space != EGGYGONiagaraEffectSpace::World) { return; }
	// 同一原复制记录的 GUID 重新映射；保留既有世界变换及模拟，不查当前 Pawn。
	Playback.Avatar = Record.Avatar;
	Playback.Mesh = Record.Mesh;
}

void UGGYGORemoteMontageFXComponent::TickComponent(float Delta, ELevelTick TickType, FActorComponentTickFunction* Tick)
{
	if (bTransferred) { Super::TickComponent(Delta, TickType, Tick); return; }
	const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Owner = ResourceOwner;
	const uint64 Key = Playback.Occurrence;
	const uint64 Generation = ResourceGeneration;
	if (!Owner.IsValid()) { DestroyComponent(); return; }
	Owner->BeginRemoteOperation(Key, Generation);
	RestoreAge(Delta, TickType, Tick);
	if (Owner.IsValid()) { Owner->EndRemoteOperation(Key, Generation); }
}

void UGGYGORemoteMontageFXComponent::RestoreAge(float Delta, ELevelTick TickType, FActorComponentTickFunction* Tick)
{
	if (bReached) { Super::TickComponent(Delta, TickType, Tick); return; }
	if (!ResourceOwner.IsValid() || !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration))
	{ SetRenderingEnabled(false); return; } // 缺条目尚无 Terminal：保持隐藏等准确终结。
	if (!IsValid(Playback.Mesh) || !Playback.Mesh->IsRegistered()) { return; }
	FString CompilationError;
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::ValidateCompiledSystem(Playback.System, CompilationError))
	{ FailPresentation(CompilationError); return; }
	if (Playback.Space == EGGYGONiagaraEffectSpace::Attached && !Playback.SocketName.IsNone()
		&& !Playback.Mesh->DoesSocketExist(Playback.SocketName))
	{ FailPresentation(TEXT("Required original socket does not exist.")); return; }
	const AGameStateBase* GameState = GetWorld()->GetGameState();
	if (!GameState) { return; }
	const double Age = GameState->GetServerWorldTimeSeconds() - Playback.BeginServerTime;
	if (!FMath::IsFinite(Age) || Age < 0.) { return; } // 时钟未到达原 Begin，不换成 Age0。
	const float Step = Playback.System->HasFixedTickDelta() ? Playback.System->GetFixedTickDeltaTime()
		: Playback.RemoteSeekDeltaSeconds;
	if (!FMath::IsFinite(Step) || Step <= 0.f || Age / Step >= MAX_int32)
	{ FailPresentation(TEXT("Remote native seek step or age range is invalid.")); return; }
	const TWeakObjectPtr<ThisClass> Self(this);
	SetDesiredAge(static_cast<float>(Age));
	if (!bActivationRequested)
	{
		bActivationRequested = true;
		Activate(true);
		if (!Self.IsValid() || !ResourceOwner.IsValid()
			|| !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration)) { return; }
	}
	Super::TickComponent(Delta, TickType, Tick);
	if (!Self.IsValid() || !ResourceOwner.IsValid()
		|| !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration)) { return; }
	const auto OriginalController = GetSystemInstanceController();
	if (!OriginalController.IsValid()) { return; }
	OriginalController->WaitForConcurrentTickAndFinalize();
	if (!Self.IsValid() || GetSystemInstanceController() != OriginalController || !ResourceOwner.IsValid()
		|| !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration)) { return; }
	if (OriginalController->IsComplete() || OriginalController->IsPendingSpawn()
		|| !Playback.System->IsReadyToRun()
		|| OriginalController->GetActualExecutionState() != ENiagaraExecutionState::Active
		|| OriginalController->GetRequestedExecutionState() != ENiagaraExecutionState::Active
		|| !FMath::IsFinite(OriginalController->GetAge())
		|| FMath::Abs(static_cast<double>(OriginalController->GetAge()) - Age) >= Step) { return; }
	bReached = true;
	// 公共 setter 同时清除原生 seek 标记；不读取/改写私有 bIsSeeking。
	SetDesiredAge(static_cast<float>(Age));
	SetAgeUpdateMode(OriginalAgeMode);
	SetRenderingEnabled(bOriginalRendering);
	if (!Self.IsValid() || !ResourceOwner.IsValid()
		|| !ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration)) { return; }
	SetForceSolo(bOriginalForceSolo);
}

EGGYGONiagaraEffectStopMode UGGYGORemoteMontageFXComponent::GetStopMode(EGGYGOMontageFXCloseReason Reason) const
{
	return Reason == EGGYGOMontageFXCloseReason::Normal ? Playback.NormalStopMode : Playback.AbortStopMode;
}

void UGGYGORemoteMontageFXComponent::ReleasePresentation(EGGYGONiagaraEffectStopMode Mode)
{
	if (bTransferred) { return; }
	bTransferred = true;
	ResourceOwner.Reset();
#if WITH_EDITOR
	// 仅活动观察窗保留同一自有回调，采尾部自然完成；不再通知资源 owner 或销毁尾粒子。
	ObservationWindows.RemoveAll([](const auto& Weak)
	{ const auto Window = Weak.Pin(); return !Window.IsValid() || !Window->IsActive(); });
	if (ObservationWindows.IsEmpty())
	{ OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished); }
#else
	OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished);
#endif
	const TWeakObjectPtr<ThisClass> Self(this);
	UGGYGOAbilityTask_PlayNiagaraEffect::StopComponent(this,
		bReached ? Mode : EGGYGONiagaraEffectStopMode::DestroyImmediately,
		[Self]() { return Self.IsValid() && !Self->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed); });
#if WITH_EDITOR
	if (Self.IsValid() && !Self->IsBeingDestroyed() && bReached
		&& (Mode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached
			|| Mode == EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld))
	{
		bObservationTailTransferred = true;
		AppendObservation(EGGYGOMontageFXObservationKind::TailTransferred);
	}
#endif
}

void UGGYGORemoteMontageFXComponent::HandlePresentationFinished(UNiagaraComponent* Component)
{
	if (Component != this) { return; }
#if WITH_EDITOR
	bObservationNaturalCompletion = true;
	AppendObservation(EGGYGOMontageFXObservationKind::NaturalCompleted);
#endif
	if (bTransferred) { return; }
	bNaturalCompletion = true;
	DestroyComponent();
}

void UGGYGORemoteMontageFXComponent::FailPresentation(const FString& Reason)
{
	bFailed = true;
	UE_LOG(LogGGYGORemoteFX, Error, TEXT("Remote Montage FX %llu: %s"), Playback.Occurrence, *Reason);
	DestroyComponent();
}

void UGGYGORemoteMontageFXComponent::OnUnregister()
{
	const auto Owner = ResourceOwner;
	const uint64 Key = Playback.Occurrence;
	const uint64 Generation = ResourceGeneration;
	if (Owner.IsValid()) { Owner->BeginRemoteOperation(Key, Generation); }
	ResourceOwner.Reset();
	OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished);
	Super::OnUnregister();
#if WITH_EDITOR
	AppendObservation(EGGYGOMontageFXObservationKind::ComponentUnregistered);
#endif
	if (Owner.IsValid())
	{
		Owner->RemoteDestroyed(Key, Generation, bNaturalCompletion, bFailed);
		Owner->EndRemoteOperation(Key, Generation);
	}
}

void UGGYGORemoteMontageFXComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	const auto Owner = ResourceOwner;
	ResourceOwner.Reset();
	OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished);
	Super::OnComponentDestroyed(bDestroyingHierarchy);
#if WITH_EDITOR
	AppendObservation(EGGYGOMontageFXObservationKind::ComponentDestroyed, true);
#endif
	if (Owner.IsValid())
	{
		// 未完成 Register 的失败也有实际销毁事实，不能只等 OnUnregister。
		Owner->RemoteDestroyed(Playback.Occurrence, ResourceGeneration, bNaturalCompletion, bFailed);
	}
}
