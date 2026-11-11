/** @file GGYGORemoteMontageFXComponent.cpp @brief 真实原生就绪、追赶达到与尾部交接。 */
#include "FX/Components/GGYGORemoteMontageFXComponent.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraSceneProxy.h"
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
	FString FieldError;
	if (!ConfigureField(Record.FieldSettings, FieldError)) { FailPresentation(FieldError); return; }
	if (!ConfigureSceneLightingSource(Record.Avatar, FieldError)) { FailPresentation(FieldError); return; }
	OnEffectFailed.AddUObject(this, &ThisClass::HandleEffectFailed);
	bAutoActivate = false;
	SetAutoDestroy(false);
	bAllowAnyoneToDestroyMe = true;
	SetVisibleInRayTracing(false);
	bOriginalForceSolo = GetForceSolo();
	bOriginalRendering = GetRenderingEnabled();
	OriginalAgeMode = GetAgeUpdateMode();
	SetPresentationRenderingEnabled(false);
	SetCanRenderWhileSeeking(false);
	SetForceSolo(true);
	SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
	SetSeekDelta(Record.RemoteSeekDeltaSeconds);
	SetMaxSimTime(Record.RemoteMaxSimTimeSeconds);
	SetLockDesiredAgeDeltaTimeToSeekDelta(false);
	SetAsset(Record.System);
	OnSystemFinished.AddUniqueDynamic(this, &ThisClass::HandlePresentationFinished);
}

bool UGGYGORemoteMontageFXComponent::InitializeLocalPresentation(UGGYGOMontageFXPublicationOwner* Owner,
	const FGGYGOFXSourceIdentity& Source, const FGGYGOMontageFXRecord& Request, double BeginWorldTime, FString& OutError)
{
	if (!IsValid(Owner) || ResourceOwner.IsValid() || LocalCapability.IsValid() || bTransferred || !IsValid(Request.Avatar)
		|| (Request.Avatar->GetLocalRole() != ROLE_Authority && Request.Avatar->GetLocalRole() != ROLE_AutonomousProxy)
		|| !Request.Validate(OutError)
		|| !FMath::IsFinite(BeginWorldTime) || BeginWorldTime < 0.)
	{ if (OutError.IsEmpty()) { OutError = TEXT("本地 FX 缺原 publisher、事件时间或本机运动来源。"); } return false; }
	const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> OriginalOwner(Owner);
	const TWeakObjectPtr<ThisClass> Self(this);
	bLocalProducer = true;
	LocalBeginWorldTime = BeginWorldTime;
	ResourceOwner = Owner;
	const auto Capability = Owner->RegisterLocalProducer(this, EGGYGOFXSourceKind::Locomotion,
		Request.Avatar, Request.Avatar->GetWorld(),
		FSimpleDelegate::CreateUObject(this, &ThisClass::ReleasePresentation, EGGYGONiagaraEffectStopMode::DestroyImmediately), OutError, Source);
	if (!Capability.IsValid()) { ResourceOwner.Reset(); return false; }
	LocalCapability = Capability;
	InitializePresentation(Owner, Request, 0);
	if (!Self.IsValid() || bTransferred || ResourceOwner.Get() != Owner)
	{ OriginalOwner->SealLocalProducer(Capability); OriginalOwner->FlushResourceChanges(); return false; }
#if WITH_EDITOR
	for (const auto& Weak : Owner->ObservationWindows)
	{ if (const auto Window = Weak.Pin()) { AddObservationWindow(Window); } }
#endif
	FGGYGOMontageFXLocalStartTicket Ticket;
	const auto Result = Owner->RequestLocalStart(Capability, Ticket, OutError);
	if (!Self.IsValid() || bTransferred || ResourceOwner.Get() != Owner)
	{
		OriginalOwner->CancelLocalStart(Ticket);
		OriginalOwner->SealLocalProducer(Capability);
		OriginalOwner->FlushResourceChanges();
		return false;
	}
	LocalStartTicket = Ticket;
	if (Result == EGGYGOMontageFXLocalStartResult::DedicatedServerNoPresentation)
	{ ReleasePresentation(EGGYGONiagaraEffectStopMode::DestroyImmediately); return true; }
	if ((Result != EGGYGOMontageFXLocalStartResult::ReadyToActivate
		&& Result != EGGYGOMontageFXLocalStartResult::WaitingForRemoteRelease) || !Ticket.IsSet())
	{ ReleasePresentation(EGGYGONiagaraEffectStopMode::DestroyImmediately); return false; }
	if (!Owner->ObserveLocalProducerStart(Ticket, FSimpleDelegate::CreateUObject(this, &ThisClass::HandleLocalStartReady)))
	{ OutError = TEXT("本地 FX 原 Ticket 就绪观察未安装。"); ReleasePresentation(EGGYGONiagaraEffectStopMode::DestroyImmediately); return false; }
	return true;
}

void UGGYGORemoteMontageFXComponent::HandleLocalStartReady()
{
	const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Owner(ResourceOwner.Get());
	if (!Owner.IsValid() || bTransferred) { return; }
	const TWeakObjectPtr<ThisClass> Self(this);
	const auto Result = Owner->ConsumeLocalStart(LocalStartTicket);
	if (!Self.IsValid() || bTransferred || ResourceOwner.Get() != Owner.Get()) { return; }
	if (Result == EGGYGOMontageFXPermitResult::AlreadyConsumed || Result == EGGYGOMontageFXPermitResult::StillWaiting) { return; }
	if (Result != EGGYGOMontageFXPermitResult::Granted)
	{ FailPresentation(TEXT("本地 FX 原 Ticket 未获激活许可。")); return; }
	const auto Capability = LocalCapability;
	Owner->BeginLocalOperation(Capability);
	Owner->SetLocalOwnership(Capability, true);
	StartPresentation();
	Owner->EndLocalOperation(Capability);
	Owner->FlushResourceChanges();
}

bool UGGYGORemoteMontageFXComponent::CanPresent() const
{
	if (!ResourceOwner.IsValid() || !GetEffectFailure().IsEmpty()) { return false; }
	if (!bLocalProducer) { return ResourceOwner->CanRestore(Playback.Occurrence, ResourceGeneration); }
	return !bTransferred && IsValid(Playback.Avatar) && !Playback.Avatar->IsActorBeingDestroyed()
		&& (Playback.Avatar->GetLocalRole() == ROLE_Authority || Playback.Avatar->GetLocalRole() == ROLE_AutonomousProxy)
		&& ResourceOwner->HasLocalPermit(LocalStartTicket);
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
	Fact.bHasRecord = !bLocalProducer || Playback.Occurrence != 0;
	Fact.Avatar = Playback.Avatar.Get();
	Fact.Mesh = Playback.Mesh.Get();
	Fact.System = Playback.System.Get();
	if (const auto* Avatar = Playback.Avatar.Get())
	{ Fact.AvatarObjectId = Avatar->GetUniqueID(); Fact.AvatarPath = Avatar->GetPathName(); }
	if (const auto* Mesh = Playback.Mesh.Get())
	{ Fact.MeshObjectId = Mesh->GetUniqueID(); Fact.MeshPath = Mesh->GetPathName(); }
	Fact.SystemPath = GetPathNameSafe(Playback.System.Get());
	Fact.BeginServerTime = Fact.bHasRecord ? Playback.BeginServerTime : 0.;
	Fact.RemoteSeekDeltaSeconds = Playback.RemoteSeekDeltaSeconds;
	Fact.RemoteMaxSimTimeSeconds = Playback.RemoteMaxSimTimeSeconds;
	Fact.DesiredAge = GetDesiredAge();
	Fact.bDesiredAgeValid = !IsBeingDestroyed() && FMath::IsFinite(Fact.DesiredAge);
	Fact.bReached = bReached;
	Fact.bRenderingEnabled = GetRenderingEnabled();
	Fact.bRegistered = IsRegistered();
	Fact.bTailTransferred = bObservationTailTransferred;
	Fact.bNaturalCompletion = bNaturalCompletion || bObservationNaturalCompletion;
	Fact.bFailed = bFailed || !GetEffectFailure().IsEmpty();
	if (const auto* System = Playback.System.Get())
	{ Fact.EffectiveStep = System->HasFixedTickDelta() ? System->GetFixedTickDeltaTime() : Playback.RemoteSeekDeltaSeconds; }
	if (const UWorld* World = GetWorld())
	{
		if (bLocalProducer)
		{
			Fact.TargetAge = World->GetTimeSeconds() - LocalBeginWorldTime;
			Fact.bTargetAgeValid = FMath::IsFinite(Fact.TargetAge) && Fact.TargetAge >= 0.;
		}
		else if (const AGameStateBase* GameState = World->GetGameState())
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
	if (bFailed) { return; }
	const TWeakObjectPtr<ThisClass> Self(this);
	UWorld* World = GetWorld();
	if (!World || !CanPresent()) { return; }
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
	if (!CanPresent())
	{ DestroyComponent(); return; }
	// Mesh 注册可晚于引用映射；使用此组件原本的 Tick 等待，不新建轮询任务。
	SetComponentTickEnabled(true);
}

void UGGYGORemoteMontageFXComponent::RefreshSourceReferences(const FGGYGOMontageFXRecord& Record)
{
	if (Record.Occurrence != Playback.Occurrence || Playback.Space != EGGYGONiagaraEffectSpace::World) { return; }
	// 同一原复制记录的 GUID 重新映射；保留既有世界变换及模拟，不查当前 Pawn。
	// field 资产须全部映射才创建本实例，配置已一次提交；这里不改活动材质配置。
	Playback.Avatar = Record.Avatar;
	Playback.Mesh = Record.Mesh;
}

void UGGYGORemoteMontageFXComponent::TickComponent(float Delta, ELevelTick TickType, FActorComponentTickFunction* Tick)
{
	if (bTransferred)
	{
#if WITH_EDITOR
		const TWeakObjectPtr<ThisClass> Self(this);
#endif
		Super::TickComponent(Delta, TickType, Tick);
#if WITH_EDITOR
		if (Self.IsValid() && !IsBeingDestroyed() && bObservationTailTransferred)
		{ AppendObservation(EGGYGOMontageFXObservationKind::Snapshot); }
#endif
		return;
	}
	const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Owner = ResourceOwner;
	const uint64 Key = Playback.Occurrence;
	const uint64 Generation = ResourceGeneration;
	const auto Capability = LocalCapability;
	const bool bLocal = bLocalProducer;
	if (!Owner.IsValid()) { DestroyComponent(); return; }
	if (bLocal) { Owner->BeginLocalOperation(Capability); }
	else { Owner->BeginRemoteOperation(Key, Generation); }
	RestoreAge(Delta, TickType, Tick);
	if (Owner.IsValid())
	{
		if (bLocal) { Owner->EndLocalOperation(Capability); Owner->FlushResourceChanges(); }
		else { Owner->EndRemoteOperation(Key, Generation); }
	}
}

void UGGYGORemoteMontageFXComponent::SetPresentationRenderingEnabled(bool bEnabled)
{
	SetRenderingEnabled(bEnabled);
	// Niagara 的 setter 只改组件字段。原 Tick 已提交隐藏时，完成追赶必须同帧更新代理。
	if (SceneProxy) { static_cast<FNiagaraSceneProxy*>(SceneProxy)->SetRenderingEnabled_GT(bEnabled); }
}

void UGGYGORemoteMontageFXComponent::RestoreAge(float Delta, ELevelTick TickType, FActorComponentTickFunction* Tick)
{
	const FString RuntimeFailure = GetEffectFailure();
	if (!RuntimeFailure.IsEmpty()) { FailPresentation(RuntimeFailure); return; }
	if (bLocalProducer && !CanPresent()) { DestroyComponent(); return; }
	if (bReached && bLocalProducer) { Super::TickComponent(Delta, TickType, Tick); return; }
	if (!CanPresent())
	{ SetPresentationRenderingEnabled(false); return; } // 缺条目尚无 Terminal：保持隐藏等准确终结。
	if (!IsValid(Playback.Mesh) || !Playback.Mesh->IsRegistered()) { return; }
	FString CompilationError;
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::ValidateCompiledSystem(Playback.System, CompilationError))
	{ FailPresentation(CompilationError); return; }
	if (Playback.Space == EGGYGONiagaraEffectSpace::Attached && !Playback.SocketName.IsNone()
		&& !Playback.Mesh->DoesSocketExist(Playback.SocketName))
	{ FailPresentation(TEXT("Required original socket does not exist.")); return; }
	const UWorld* World = GetWorld();
	if (!World) { return; }
	const AGameStateBase* GameState = World->GetGameState();
	if (!bLocalProducer && !GameState) { return; }
	const double Age = bLocalProducer ? World->GetTimeSeconds() - LocalBeginWorldTime
		: GameState->GetServerWorldTimeSeconds() - Playback.BeginServerTime;
	if (!FMath::IsFinite(Age) || Age < 0.) { return; } // 时钟未到达原 Begin，不换成 Age0。
	const float Step = Playback.System->HasFixedTickDelta() ? Playback.System->GetFixedTickDeltaTime()
		: Playback.RemoteSeekDeltaSeconds;
	if (!FMath::IsFinite(Step) || Step <= 0.f || Age / Step >= MAX_int32)
	{ FailPresentation(TEXT("Remote native seek step or age range is invalid.")); return; }
	const TWeakObjectPtr<ThisClass> Self(this);
	SetPresentationRenderingEnabled(false);
	SetDesiredAge(static_cast<float>(Age));
	if (!bActivationRequested)
	{
		bActivationRequested = true;
		Activate(true);
		if (!Self.IsValid() || !CanPresent()) { return; }
	}
	Super::TickComponent(Delta, TickType, Tick);
	if (!Self.IsValid() || !CanPresent()) { return; }
	const auto OriginalController = GetSystemInstanceController();
	if (!OriginalController.IsValid()) { return; }
	OriginalController->WaitForConcurrentTickAndFinalize();
	if (!Self.IsValid() || GetSystemInstanceController() != OriginalController || !CanPresent()) { return; }
	if (OriginalController->IsComplete() || OriginalController->IsPendingSpawn()
		|| !AreSceneMaterialsApplied()
		|| !Playback.System->IsReadyToRun()
		|| OriginalController->GetActualExecutionState() != ENiagaraExecutionState::Active
		|| OriginalController->GetRequestedExecutionState() != ENiagaraExecutionState::Active
		|| !FMath::IsFinite(OriginalController->GetAge())
		|| FMath::Abs(static_cast<double>(OriginalController->GetAge()) - Age) >= Step) { return; }
	bReached = true;
	// 公共 setter 同时清除原生 seek 标记；不读取/改写私有 bIsSeeking。
	SetDesiredAge(static_cast<float>(Age));
	if (bLocalProducer) { SetAgeUpdateMode(OriginalAgeMode); }
	SetPresentationRenderingEnabled(bOriginalRendering);
	if (bLocalProducer)
	{
		if (Self.IsValid() && CanPresent()) { SetForceSolo(bOriginalForceSolo); }
		return;
	}
	// 此组件已经在本帧的 Solo Tick 中消费到 Age。立即转入 WorldManager 的批量
	// Simulation 会允许同帧再消费一次 Delta；组件及其活尾始终由原 Solo Tick 推进。
	// 活动期间每帧继续按原 Begin 求 DesiredAge，避免 TickDeltaTime 被原生 MaxDeltaTime
	// 截短后持续落后服务器时钟。超出本帧追赶预算仍保持隐藏，不把 reached 当成当前 Age。
}

EGGYGONiagaraEffectStopMode UGGYGORemoteMontageFXComponent::GetStopMode(EGGYGOMontageFXCloseReason Reason) const
{
	return Reason == EGGYGOMontageFXCloseReason::Normal ? Playback.NormalStopMode : Playback.AbortStopMode;
}

void UGGYGORemoteMontageFXComponent::ReleasePresentation(EGGYGONiagaraEffectStopMode Mode)
{
	if (bTransferred) { return; }
	// 归还后只有原生尾粒子继续，退出服务器 Age 追赶并恢复资产的时间模式。
	if (!bLocalProducer)
	{
		SetAgeUpdateMode(OriginalAgeMode);
		if (bReached && (Mode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached
			|| Mode == EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld))
		{
			// 上一帧可能因追赶预算不足而隐藏；已到达过的原活尾不能继承 seek 的隐藏状态。
			SetDesiredAge(GetDesiredAge());
			SetPresentationRenderingEnabled(bOriginalRendering);
		}
	}
	if (bLocalProducer)
	{
		const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Owner(ResourceOwner.Get());
		const auto Capability = LocalCapability;
		const auto Ticket = LocalStartTicket;
		const auto Callback = MoveTemp(LocalClosed);
		LocalClosed.Unbind();
		if (Owner.IsValid())
		{ Owner->BeginLocalOperation(Capability); Owner->CancelLocalStart(Ticket); Owner->SealLocalProducer(Capability); }
		bTransferred = true;
		ResourceOwner.Reset();
		OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished);
		const TWeakObjectPtr<ThisClass> Self(this);
		const bool bReturned = UGGYGOAbilityTask_PlayNiagaraEffect::StopComponent(this,
			bReached ? Mode : EGGYGONiagaraEffectStopMode::DestroyImmediately,
			[Self] { return Self.IsValid() && !Self->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed); });
		if (!bReturned)
		{ UE_LOG(LogGGYGORemoteFX, Error, TEXT("原本地 FX 停止策略无效，销毁其原组件。")); if (Self.IsValid()) { Self->DestroyComponent(); } }
		if (Owner.IsValid())
		{ Owner->SetLocalOwnership(Capability, false); Owner->EndLocalOperation(Capability); Owner->FlushResourceChanges(); }
		Callback.ExecuteIfBound(this, !bReturned);
		return;
	}
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
	const FString Failure = GetEffectFailure();
	if (!Failure.IsEmpty()) { FailPresentation(Failure); return; }
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
	if (bFailed) { return; }
	bFailed = true;
	UE_LOG(LogGGYGORemoteFX, Error, TEXT("Remote Montage FX %llu: %s"), Playback.Occurrence, *Reason);
	DestroyComponent();
}

void UGGYGORemoteMontageFXComponent::HandleEffectFailed(
	UGGYGOFXNiagaraComponent* Component, const FString& Reason)
{
	if (Component == this && !bTransferred) { FailPresentation(Reason); }
}

void UGGYGORemoteMontageFXComponent::OnUnregister()
{
	const auto Owner = ResourceOwner;
	const uint64 Key = Playback.Occurrence;
	const uint64 Generation = ResourceGeneration;
	const auto Capability = LocalCapability;
	const auto Ticket = LocalStartTicket;
	const bool bLocal = bLocalProducer;
	const bool bCompleted = bNaturalCompletion;
	const bool bWasFailed = bFailed;
	const auto Callback = MoveTemp(LocalClosed);
	LocalClosed.Unbind();
	if (Owner.IsValid())
	{
		if (bLocal) { Owner->BeginLocalOperation(Capability); Owner->CancelLocalStart(Ticket); Owner->SealLocalProducer(Capability); }
		else { Owner->BeginRemoteOperation(Key, Generation); }
	}
	ResourceOwner.Reset();
	OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished);
	OnEffectFailed.RemoveAll(this);
	Super::OnUnregister();
#if WITH_EDITOR
	AppendObservation(EGGYGOMontageFXObservationKind::ComponentUnregistered);
#endif
	if (Owner.IsValid())
	{
		if (bLocal)
		{ Owner->SetLocalOwnership(Capability, false); Owner->EndLocalOperation(Capability); Owner->FlushResourceChanges(); }
		else { Owner->RemoteDestroyed(Key, Generation, bCompleted, bWasFailed); Owner->EndRemoteOperation(Key, Generation); }
	}
	Callback.ExecuteIfBound(this, bWasFailed);
}

void UGGYGORemoteMontageFXComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	const auto Owner = ResourceOwner;
	const auto Capability = LocalCapability;
	const auto Ticket = LocalStartTicket;
	const bool bLocal = bLocalProducer;
	const uint64 Key = Playback.Occurrence;
	const uint64 Generation = ResourceGeneration;
	const bool bCompleted = bNaturalCompletion;
	const bool bWasFailed = bFailed;
	const auto Callback = MoveTemp(LocalClosed);
	LocalClosed.Unbind();
	ResourceOwner.Reset();
	OnSystemFinished.RemoveDynamic(this, &ThisClass::HandlePresentationFinished);
	OnEffectFailed.RemoveAll(this);
	Super::OnComponentDestroyed(bDestroyingHierarchy);
#if WITH_EDITOR
	AppendObservation(EGGYGOMontageFXObservationKind::ComponentDestroyed, true);
#endif
	if (Owner.IsValid())
	{
		// 未完成 Register 的失败也有实际销毁事实，不能只等 OnUnregister。
		if (bLocal)
		{ Owner->CancelLocalStart(Ticket); Owner->SealLocalProducer(Capability); Owner->SetLocalOwnership(Capability, false); Owner->FlushResourceChanges(); }
		else { Owner->RemoteDestroyed(Key, Generation, bCompleted, bWasFailed); }
	}
	Callback.ExecuteIfBound(this, bWasFailed);
}
