/** @file GGYGOLocomotionFXComponent.cpp @brief 消费原完成/退休事实并归还自身表现资源。 */
#include "FX/Components/GGYGOLocomotionFXComponent.h"

#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "Animation/AnimSequence.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOPawnExtensionComponent.h"
#include "Character/GGYGOCharacterBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "FX/Components/GGYGOMontageFXReplicationComponent.h"
#include "FX/Components/GGYGORemoteMontageFXComponent.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOLocomotionFX, Log, All);

struct UGGYGOLocomotionFXComponent::FCueResource
{
	FGGYGOLocomotionFXCue Config;
	FGGYGOMontageFXRecord Request;
	TStrongObjectPtr<UNiagaraSystem> System;
	TStrongObjectPtr<UGGYGORemoteMontageFXComponent> Native;
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Publisher;
	FGGYGOMontageFXLease Lease;
	FGGYGOFXSourceIdentity Source;
	TWeakObjectPtr<UWorld> BeginWorld;
	double BeginWorldTime = 0.;
	bool bConsumed = false;
	bool bClosed = false;
	bool bNaturallyCompleted = false;
	bool bResumeAfterRoleChange = false;
};

struct UGGYGOLocomotionFXComponent::FSource
{
	FGGYGOLocomotionSourceProof Proof;
	TArray<TSharedPtr<FCueResource>> Cues;
	uint64 LastIntervalSerial = 0;
	bool bClosed = false;
};

/** 只在本次组件注册中存活；配置取自该注册的属性，原订阅在任何外调前拆下。 */
struct UGGYGOLocomotionFXComponent::FConsumer
{
	TWeakObjectPtr<AGGYGOCharacterBase> Character;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
	/** 本 Pawn 的 ASC 就绪订阅；ASC 是已消费资源的派生缓存。 */
	FGGYGOLocalASCSubscription ASCSubscription;
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	FDelegateHandle Retirement;
	TArray<FGGYGOLocomotionFXCue> ConfiguredCues;
	TArray<TSharedPtr<FSource>> Sources;
	uint64 LastIntervalSerial = 0;
	bool bClosed = false;
};

UGGYGOLocomotionFXComponent::UGGYGOLocomotionFXComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UGGYGOLocomotionFXComponent::ValidateCues(FString& Error) const
{
	if (Cues.IsEmpty()) { Error = TEXT("缺已核来源的 Sequence FX 窗口配置。"); return false; }
	for (int32 Index = 0; Index < Cues.Num(); ++Index)
	{
		const auto& Cue = Cues[Index];
		if (Cue.CueName.IsNone() || Cue.MotionType == EGGYGOLocomotionMotionType::None || !IsValid(Cue.Sequence)
			|| !FMath::IsFinite(Cue.BeginSequenceSeconds) || !FMath::IsFinite(Cue.EndSequenceSeconds)
			|| Cue.BeginSequenceSeconds < 0. || Cue.EndSequenceSeconds <= Cue.BeginSequenceSeconds
			|| Cue.EndSequenceSeconds > Cue.Sequence->GetPlayLength())
		{ Error = FString::Printf(TEXT("cue[%d] 缺原来源、唯一名字或有效原片窗口。"), Index); return false; }
		FGGYGOMontageFXRecord Playback;
		Playback.System = Cue.System;
		Playback.FieldSettings = Cue.FieldSettings;
		Playback.Space = Cue.Space;
		Playback.Transform = Cue.LocalTransform;
		Playback.NormalStopMode = Cue.NormalStopMode;
		Playback.AbortStopMode = Cue.AbortStopMode;
		Playback.RemoteSeekDeltaSeconds = Cue.SeekDeltaSeconds;
		Playback.RemoteMaxSimTimeSeconds = Cue.MaxSimTimeSeconds;
		if (!Playback.Validate(Error)) { return false; }
		if (Cue.Space != EGGYGONiagaraEffectSpace::Attached
			&& (Cue.NormalStopMode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached
				|| Cue.AbortStopMode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached))
		{ Error = TEXT("World 空间不能选择 KeepAttached 尾粒子移交。"); return false; }
		for (int32 Previous = 0; Previous < Index; ++Previous)
		{
			if (Cues[Previous].MotionType == Cue.MotionType && Cues[Previous].Sequence == Cue.Sequence
				&& Cues[Previous].CueName == Cue.CueName)
			{ Error = TEXT("同一原片的 CueName 重复。"); return false; }
		}
	}
	return true;
}

bool UGGYGOLocomotionFXComponent::OwnsConsumer(const TSharedPtr<FConsumer>& Original) const
{
	return Original.IsValid() && Consumer == Original && !Original->bClosed;
}

bool UGGYGOLocomotionFXComponent::IsOriginalConsumer(const TSharedPtr<FConsumer>& Original) const
{
	return OwnsConsumer(Original) && IsRegistered()
		&& Original->Character.IsValid() && GetOwner() == Original->Character.Get()
		&& Original->Movement.IsValid() && Original->Character->GetCharacterMovement() == Original->Movement.Get();
}

void UGGYGOLocomotionFXComponent::OnRegister()
{
	Super::OnRegister();
	if (Consumer.IsValid()) { return; }
	if (!GetWorld() || !GetWorld()->IsGameWorld()) { return; }
	FString Error;
	AGGYGOCharacterBase* Character = Cast<AGGYGOCharacterBase>(GetOwner());
	UGGYGOCharacterMovementComponent* Movement = Character ? Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement()) : nullptr;
	UGGYGOPawnExtensionComponent* Extension = Character ? UGGYGOPawnExtensionComponent::FindPawnExtensionComponent(Character) : nullptr;
	if (!Character || !Movement || !Extension)
	{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s] 需要原 GGYGO Character、CMC 和 PawnExtension。"), *GetPathName()); return; }
	if (!ValidateCues(Error))
	{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s] 无法注册原运动 FX：%s"), *GetPathName(), *Error); return; }
	const auto Original = MakeShared<FConsumer>();
	Original->Character = Character;
	Original->Movement = Movement;
	Original->ConfiguredCues = Cues;
	// 注册前已存在的区间不是本订阅收到的新事件；防同步重注册重放原回调。
	Original->LastIntervalSerial = Movement->GetMotionExecutionSnapshot().CompletedIntervalSerial;
	Consumer = Original;
	Character->OnCharacterMovementUpdated.AddUniqueDynamic(this, &ThisClass::HandleMovementUpdated);
	const TWeakObjectPtr<ThisClass> Self(this);
	const TWeakPtr<FConsumer> WeakOriginal(Original);
	Original->Retirement = Movement->OnLocomotionSourceRetired().AddWeakLambda(this,
		[Self, WeakOriginal](const FGGYGOLocomotionSourceRetirementFact& Fact)
		{ if (auto* Owner = Self.Get()) { Owner->ConsumeRetirement(WeakOriginal.Pin(), Fact); } });
	// 已就绪时立即回放 Ready。
	Original->ASCSubscription.Subscribe(Extension, [Self, WeakOriginal](const FGGYGOPawnASCNotice& Notice)
	{ if (auto* Owner = Self.Get()) { Owner->ConsumeNotice(WeakOriginal.Pin(), Notice); } });
}

void UGGYGOLocomotionFXComponent::ConsumeNotice(const TSharedPtr<FConsumer>& Original,
	const FGGYGOPawnASCNotice& Notice)
{
	if (!OwnsConsumer(Original)) { return; }
	// 订阅已过滤重复 Ready 与非本次资源的 Released；绑定变化时旧来源的窗口全部关闭。
	Original->ASC.Reset();
	CloseSources(Original);
	if (Notice.Kind != EGGYGOPawnASCNoticeKind::Ready || !IsOriginalConsumer(Original)
		|| !Original->ASCSubscription.GetConsumedResource().HasSameResource(Notice.Resource)) { return; }
	const auto Identity = Notice.Resource.GetIdentity();
	if (Identity.Pawn.Get() != Original->Character.Get() || !Identity.ASC.IsValid()) { return; }
	Original->ASC = Identity.ASC;
}

void UGGYGOLocomotionFXComponent::HandleMovementUpdated(float DeltaSeconds, FVector OldLocation, FVector OldVelocity)
{
	const auto Original = Consumer;
	if (!OwnsConsumer(Original)) { return; }
	if (!IsOriginalConsumer(Original) || !Original->ASCSubscription.IsSubscribed()) { CloseSources(Original); return; }
	if (!Original->ASC.IsValid() || Original->ASCSubscription.GetReadyASC() != Original->ASC.Get()) { return; }
	const auto Role = Original->Character->GetLocalRole();
	if (Role != ROLE_Authority && Role != ROLE_AutonomousProxy) { return; }
	// 原 Character delegate 已晚于 CMC 的完整区间生产；本回调只查询一次。
	const FGGYGOMotionExecutionSnapshot Interval = Original->Movement->GetMotionExecutionSnapshot();
	ConsumeInterval(Original, Interval, true);
}

void UGGYGOLocomotionFXComponent::ConsumeInterval(const TSharedPtr<FConsumer>& Original,
	const FGGYGOMotionExecutionSnapshot& Interval, bool bAllowNewSource,
	const FGGYGOLocomotionSourceProof* RetiredSource)
{
	if (!IsOriginalConsumer(Original) || Interval.Status != EGGYGOMotionExecutionStatus::Valid
		|| Interval.SourceKind != EGGYGOMotionExecutionSource::Locomotion || !Interval.bHasSingleSequenceTime
		|| Interval.OriginalMovement.Get() != Original->Movement.Get()
		|| Interval.OriginalCharacter.Get() != Original->Character.Get()
		|| Interval.CompletedIntervalSerial == 0
		|| (bAllowNewSource && Interval.CompletedIntervalSerial <= Original->LastIntervalSerial)
		|| !FMath::IsFinite(Interval.SequenceStartSeconds) || !FMath::IsFinite(Interval.SequenceEndSeconds)
		|| Interval.SequenceEndSeconds < Interval.SequenceStartSeconds) { return; }
	FGGYGOLocomotionSourceProof Proof;
	FString Error;
	if (!bAllowNewSource && RetiredSource) { Proof = *RetiredSource; }
	else if (!Original->Movement->TryGetLocomotionSourceProof(Interval, Proof, Error)) { return; }
	if (bAllowNewSource) { Original->LastIntervalSerial = Interval.CompletedIntervalSerial; }
	TSharedPtr<FSource> Source;
	for (const auto& Existing : Original->Sources)
	{ if (Existing->Proof.IsSameSource(Proof)) { Source = Existing; break; } }
	if (!Source.IsValid())
	{
		if (!bAllowNewSource) { return; }
		bool bConfiguredMotion = false;
		Source = MakeShared<FSource>();
		Source->Proof = Proof;
		for (const auto& Config : Original->ConfiguredCues)
		{
			if (Config.MotionType != Interval.MotionType) { continue; }
			bConfiguredMotion = true;
			if (Config.Sequence.Get() != Interval.OriginalSequence.Get()) { continue; }
			const auto Cue = MakeShared<FCueResource>();
			Cue->Config = Config;
			if (Config.BeginSequenceSeconds < Interval.SequenceStartSeconds)
			{
				Cue->bConsumed = Cue->bClosed = true;
				if (Config.EndSequenceSeconds > Interval.SequenceStartSeconds)
				{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s:%s] 入场前已越过原 Begin，缺原事件采样，拒绝用当前时刻补播。"), *GetPathName(), *Config.CueName.ToString()); }
			}
			Source->Cues.Add(Cue);
		}
		if (!bConfiguredMotion) { return; }
		Original->Sources.Add(Source);
		if (Source->Cues.IsEmpty())
		{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s] 原 Sequence [%s] 没有对应已核 FX 配置。"), *GetPathName(), *GetPathNameSafe(Interval.OriginalSequence.Get())); }
	}
	if (Source->bClosed || Interval.CompletedIntervalSerial <= Source->LastIntervalSerial) { return; }
	Source->LastIntervalSerial = Interval.CompletedIntervalSerial;
	const auto SelectedCues = Source->Cues;
	for (const auto& Cue : SelectedCues)
	{
		if (!IsOriginalConsumer(Original) || Source->bClosed) { return; }
		if (Cue->bClosed) { continue; }
		if (bAllowNewSource && !Cue->bConsumed && Interval.SequenceEndSeconds > Interval.SequenceStartSeconds
			&& Cue->Config.BeginSequenceSeconds >= Interval.SequenceStartSeconds
			&& Cue->Config.BeginSequenceSeconds <= Interval.SequenceEndSeconds)
		{ BeginCue(Original, Source, Cue, Interval); }
		if (!IsOriginalConsumer(Original) || Source->bClosed) { return; }
		if (Cue->bConsumed && !Cue->bClosed && Interval.SequenceEndSeconds >= Cue->Config.EndSequenceSeconds)
		{ CloseCue(Cue, true); }
		else if (Cue->bConsumed && !Cue->bClosed && Cue->bResumeAfterRoleChange && !Cue->bNaturallyCompleted)
		{
			// 仅恢复同一原来源仍开的窗，不重新签发 Begin，也不补播已结束的窗口。
			StartLocalCue(Original, Source, Cue);
		}
	}
}

void UGGYGOLocomotionFXComponent::BeginCue(const TSharedPtr<FConsumer>& Original,
	const TSharedPtr<FSource>& Source, const TSharedPtr<FCueResource>& Cue,
	const FGGYGOMotionExecutionSnapshot& Interval)
{
	Cue->bConsumed = true;
	UGGYGOAbilitySystemComponent* ASC = Original->ASC.Get();
	AGGYGOCharacterBase* Character = Original->Character.Get();
	USkeletalMeshComponent* Mesh = Character ? Character->GetMesh() : nullptr;
	UWorld* World = nullptr;
	double BeginTime = 0.;
	FString Error;
	if (!ASC || !Mesh || !Original->Movement->TryGetLocomotionCompletionWorldTime(Source->Proof,
		Interval.CompletedIntervalSerial, Interval.CompletedFrame, World, BeginTime, Error)
		|| World != GetWorld() || !FMath::IsFinite(BeginTime) || BeginTime < 0.
		|| (!Cue->Config.SocketName.IsNone() && !Mesh->DoesSocketExist(Cue->Config.SocketName)))
	{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s:%s] 原源事件时间或挂点无效：%s"), *GetPathName(), *Cue->Config.CueName.ToString(), *Error); Cue->bClosed = true; return; }
	Cue->Source = FGGYGOFXSourceIdentity::FromLocomotion(Source->Proof, ASC);
	Cue->BeginWorld = World;
	Cue->BeginWorldTime = BeginTime;
	Cue->System.Reset(Cue->Config.System);
	FGGYGOMontageFXRecord Request;
	Request.Avatar = Character;
	Request.Mesh = Mesh;
	Request.System = Cue->Config.System;
	Request.FieldSettings = Cue->Config.FieldSettings;
	Request.Space = Cue->Config.Space;
	Request.SocketName = Request.Space == EGGYGONiagaraEffectSpace::Attached ? Cue->Config.SocketName : NAME_None;
	Request.Transform = Cue->Config.LocalTransform;
	if (Request.Space == EGGYGONiagaraEffectSpace::World)
	{ Request.Transform *= Cue->Config.SocketName.IsNone() ? Mesh->GetComponentTransform() : Mesh->GetSocketTransform(Cue->Config.SocketName, RTS_World); }
	Request.NormalStopMode = Cue->Config.NormalStopMode;
	Request.AbortStopMode = Cue->Config.AbortStopMode;
	Request.RemoteSeekDeltaSeconds = Cue->Config.SeekDeltaSeconds;
	Request.RemoteMaxSimTimeSeconds = Cue->Config.MaxSimTimeSeconds;
	Request.BeginServerTime = BeginTime;
	const TWeakObjectPtr<UGGYGOMontageFXReplicationComponent> Host(UGGYGOMontageFXReplicationComponent::FindForASC(ASC));
	const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Publisher(Host.IsValid() ? Host->GetPublicationOwner() : nullptr);
	if (!Publisher.IsValid())
	{
		UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s] 原 ASC [%s] 的 FX 发布宿主或发行者尚未建立。"),
			*GetPathName(), *GetPathNameSafe(ASC));
		Cue->bClosed = true;
		return;
	}
	Cue->Publisher = Publisher;
	if (Character->HasAuthority())
	{
		FGGYGOMontageFXLease Lease;
		const bool bPublished = Host.IsValid()
			&& Host->PublishLocomotionFX(Original->Movement.Get(), Interval, Request, Lease, Error);
		if (!IsOriginalConsumer(Original) || Source->bClosed || Cue->bClosed)
		{
			FGGYGOMontageFXCloseObligation Obligation;
			if (Publisher.IsValid() && Publisher->PrepareClose(Lease, EGGYGOMontageFXCloseReason::Abort, Obligation) == EGGYGOMontageFXCloseResult::Closed)
			{ Publisher->DispatchClose(MoveTemp(Obligation)); }
			return;
		}
		if (!bPublished || !Lease.HasLease())
		{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("[%s:%s] 原源发行失败：%s"), *GetPathName(), *Cue->Config.CueName.ToString(), *Error); Cue->bClosed = true; return; }
		Cue->Lease = MoveTemp(Lease);
		Request.Occurrence = Cue->Lease.GetOccurrence();
	}
	Cue->Request = Request;
	StartLocalCue(Original, Source, Cue);
}

void UGGYGOLocomotionFXComponent::StartLocalCue(const TSharedPtr<FConsumer>& Original,
	const TSharedPtr<FSource>& Source, const TSharedPtr<FCueResource>& Cue)
{
	if (!IsOriginalConsumer(Original) || Source->bClosed || Cue->bClosed || Cue->Native.IsValid()) { return; }
	UWorld* World = Cue->BeginWorld.Get();
	if (!World || World->bIsTearingDown || World != GetWorld() || !Original->ASC.IsValid()
		|| Original->ASCSubscription.GetReadyASC() != Original->ASC.Get()
		|| Cue->Source.GetSourceAbilitySystem() != Original->ASC.Get())
	{ CloseCue(Cue, false); return; }
	if (World->GetNetMode() == NM_DedicatedServer) { return; }
	const auto Role = Original->Character->GetLocalRole();
	if (Role != ROLE_Authority && Role != ROLE_AutonomousProxy) { return; }
	FString Error;
	if (!Original->Movement->IsLocomotionSourceProofCurrent(Source->Proof, Error))
	{ CloseCue(Cue, false); return; }
	const auto Publisher = Cue->Publisher;
	const auto Request = Cue->Request;
	if (!Publisher.IsValid()) { CloseCue(Cue, false); return; }
	Cue->bResumeAfterRoleChange = false;
	UObject* Outer = Request.Space == EGGYGONiagaraEffectSpace::World ? static_cast<UObject*>(World->GetWorldSettings()) : static_cast<UObject*>(Original->Character.Get());
	TStrongObjectPtr<UGGYGORemoteMontageFXComponent> Native(NewObject<UGGYGORemoteMontageFXComponent>(Outer));
	if (!IsOriginalConsumer(Original) || Source->bClosed || Cue->bClosed)
	{ if (Native.IsValid()) { Native->DestroyComponent(); } return; }
	if (!Native.IsValid())
	{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("原 Niagara 组件分配失败。")); CloseCue(Cue, false); return; }
	Cue->Native = MoveTemp(Native);
	const TWeakObjectPtr<ThisClass> Self(this);
	const TWeakPtr<FCueResource> WeakCue(Cue);
	const TWeakPtr<FConsumer> WeakOriginal(Original);
	Cue->Native->SetLocalClosedCallback(FGGYGOLocalNiagaraClosed::CreateWeakLambda(this,
		[Self, WeakCue, WeakOriginal](UNiagaraComponent* Component, bool bFailed)
		{
			const auto OriginalCue = WeakCue.Pin();
			if (!OriginalCue.IsValid() || OriginalCue->Native.Get() != Component) { return; }
			OriginalCue->bNaturallyCompleted = OriginalCue->Native->HasNaturallyCompleted();
			OriginalCue->Native.Reset();
			if (OriginalCue->bClosed || OriginalCue->bNaturallyCompleted) { return; }
			auto* Owner = Self.Get();
			const auto Scope = WeakOriginal.Pin();
			if (!Owner || !Owner->IsOriginalConsumer(Scope)) { return; }
			if (!bFailed && Scope->Character->GetLocalRole() == ROLE_SimulatedProxy)
			{ OriginalCue->bResumeAfterRoleChange = true; return; }
			Owner->CloseCue(OriginalCue, false);
			if (bFailed) { UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("原本地 FX 实例失败；不改变原运动执行状态。")); }
		}));
	if (!Cue->Native->InitializeLocalPresentation(Publisher.Get(), Cue->Source, Request, Cue->BeginWorldTime, Error))
	{
		if (!Self.IsValid() || !Self->IsOriginalConsumer(Original) || Source->bClosed || Cue->bClosed
			|| Cue->bResumeAfterRoleChange || Cue->bNaturallyCompleted) { return; }
		if (Self.IsValid()) { Self->CloseCue(Cue, false); }
		UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("原本地 FX 激活请求失败：%s"), *Error);
	}
}

void UGGYGOLocomotionFXComponent::CloseCue(const TSharedPtr<FCueResource>& Cue, bool bNormal)
{
	if (!Cue.IsValid() || Cue->bClosed) { return; }
	Cue->bClosed = true;
	const auto Publisher = Cue->Publisher;
	FGGYGOMontageFXLease Lease = MoveTemp(Cue->Lease);
	TStrongObjectPtr<UGGYGORemoteMontageFXComponent> Native = MoveTemp(Cue->Native);
	const auto Mode = bNormal ? Cue->Config.NormalStopMode : Cue->Config.AbortStopMode;
	FGGYGOMontageFXCloseObligation Obligation;
	if (Publisher.IsValid() && Lease.HasLease())
	{
		const auto Result = Publisher->PrepareClose(Lease,
			bNormal ? EGGYGOMontageFXCloseReason::Normal : EGGYGOMontageFXCloseReason::Abort, Obligation);
		if (Result == EGGYGOMontageFXCloseResult::Rejected)
		{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("原 FX lease [%llu] 关闭被拒绝。"), Lease.GetOccurrence()); }
	}
	if (Native.IsValid()) { Native->ReleasePresentation(Mode); }
	if (Obligation.IsSet() && Publisher.IsValid() && !Publisher->DispatchClose(MoveTemp(Obligation)))
	{ UE_LOG(LogGGYGOLocomotionFX, Error, TEXT("原 FX 关闭义务未派发。")); }
	Cue->System.Reset();
}

void UGGYGOLocomotionFXComponent::ConsumeRetirement(const TSharedPtr<FConsumer>& Original,
	const FGGYGOLocomotionSourceRetirementFact& Fact)
{
	if (!OwnsConsumer(Original) || !Fact.SourceProof.IsSet()) { return; }
	if (Fact.EndReason != EGGYGOMotionExecutionEndReason::Completed
		&& Fact.EndReason != EGGYGOMotionExecutionEndReason::Cancelled
		&& Fact.EndReason != EGGYGOMotionExecutionEndReason::Failed) { return; }
	if (Fact.bHasFinalInterval && Fact.EndReason == EGGYGOMotionExecutionEndReason::Completed)
	{ ConsumeInterval(Original, Fact.FinalInterval, false, &Fact.SourceProof); }
	if (!OwnsConsumer(Original)) { return; }
	TArray<TSharedPtr<FSource>> Originals;
	for (const auto& Source : Original->Sources)
	{ if (Source->Proof.IsSameSource(Fact.SourceProof)) { Originals.Add(Source); Source->bClosed = true; } }
	Original->Sources.RemoveAll([&Fact](const auto& Source) { return Source->Proof.IsSameSource(Fact.SourceProof); });
	for (const auto& Source : Originals)
	{ for (const auto& Cue : Source->Cues) { CloseCue(Cue, Fact.EndReason == EGGYGOMotionExecutionEndReason::Completed); } }
}

void UGGYGOLocomotionFXComponent::CloseSources(const TSharedPtr<FConsumer>& Original)
{
	TArray<TSharedPtr<FSource>> Sources = MoveTemp(Original->Sources);
	for (const auto& Source : Sources) { Source->bClosed = true; }
	for (const auto& Source : Sources)
	{ for (const auto& Cue : Source->Cues) { CloseCue(Cue, false); } }
}

void UGGYGOLocomotionFXComponent::OnUnregister()
{
	const auto Original = MoveTemp(Consumer);
	Consumer.Reset();
	if (Original.IsValid())
	{
		Original->bClosed = true;
		if (Original->Character.IsValid()) { Original->Character->OnCharacterMovementUpdated.RemoveDynamic(this, &ThisClass::HandleMovementUpdated); }
		if (Original->Movement.IsValid()) { Original->Movement->OnLocomotionSourceRetired().Remove(Original->Retirement); }
		Original->ASCSubscription.Unsubscribe();
		CloseSources(Original);
	}
	Super::OnUnregister();
}
