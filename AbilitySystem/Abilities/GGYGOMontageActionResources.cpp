/** @file GGYGOMontageActionResources.cpp @brief 原 Montage、动作及表现资源的唯一执行与清理接缝 */
#include "AbilitySystem/Abilities/GGYGOMontageActionResources.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "FX/Components/GGYGOMontageFXReplicationComponent.h"
#include "System/GGYGOGameplayTags.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"

namespace
{
	/** 仅归还捕获的原 lease；全部 holder 成员须在此可重入外调前摘除。 */
	void ClosePublication(const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner>& OriginalPublisher,
		const FGGYGOMontageFXLease& OriginalLease, EGGYGOMontageFXCloseReason Reason)
	{
		if (!OriginalLease.HasLease()) { return; }
		UGGYGOMontageFXPublicationOwner* Publisher = OriginalPublisher.Get();
		// 已销毁发行者由其 Shutdown 归还；历史 lease 不改向后继 ASC。
		if (!Publisher) { return; }
		FGGYGOMontageFXCloseObligation Obligation;
		const EGGYGOMontageFXCloseResult Result = Publisher->PrepareClose(OriginalLease, Reason, Obligation);
		if (Result == EGGYGOMontageFXCloseResult::Rejected)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("Montage FX 原 lease [%llu] 关闭被发行者拒绝。"),
				static_cast<unsigned long long>(OriginalLease.GetOccurrence()));
			return;
		}
		if (Obligation.IsSet() && !Publisher->DispatchClose(MoveTemp(Obligation)))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("Montage FX 原 lease [%llu] 已关闭，但终结派发未成功。"),
				static_cast<unsigned long long>(OriginalLease.GetOccurrence()));
		}
	}

	/** 旧资产显式使用原单项配置；新制作单元按作者顺序消费，不合并为新的 Niagara System。 */
	TArray<FGGYGONiagaraEffectUnit> GetFXUnits(const FGGYGOComboStepFX& FX)
	{
		if (!FX.SystemUnits.IsEmpty()) { return FX.SystemUnits; }
		FGGYGONiagaraEffectUnit Unit;
		Unit.System = FX.System;
		Unit.FieldSettings = FX.FieldSettings;
		TArray<FGGYGONiagaraEffectUnit> Units;
		Units.Add(MoveTemp(Unit));
		return Units;
	}
}

bool FGGYGOMontageActionResources::ValidateFXSocketsOnMesh(const FGGYGOComboStepFX& Config,
	const USkeletalMeshComponent& Mesh, FString& OutError)
{
	OutError.Reset();
	if (Config.Space == EGGYGONiagaraEffectSpace::World)
	{
		if (!Config.SocketName.IsNone() && !Mesh.DoesSocketExist(Config.SocketName))
		{
			OutError = FString::Printf(TEXT("FX World 起点 [%s:%s] 不存在。"),
				*Mesh.GetPathName(), *Config.SocketName.ToString());
			return false;
		}
		return true;
	}
	for (const FGGYGONiagaraEffectUnit& Unit : GetFXUnits(Config))
	{
		FName Socket;
		FTransform Transform;
		Unit.ResolveAttachment(Config.SocketName, Config.LocalTransform, Socket, Transform);
		if (!Socket.IsNone() && !Mesh.DoesSocketExist(Socket))
		{
			OutError = FString::Printf(TEXT("FX 单元 '%s' 的挂点 [%s:%s] 不存在。"),
				*GetPathNameSafe(Unit.System), *Mesh.GetPathName(), *Socket.ToString());
			return false;
		}
	}
	return true;
}

/** 读取作者配置，不建立窗口计时或运行期有效缓存。 */
bool FGGYGOMontageActionResources::ValidateFXConfiguration(const UAnimMontage* Montage, const FGGYGOComboStepFX& FX, float RangeStart, float RangeEnd,
	FString& OutError, FGGYGOMontageFXWindowDeclaration* OutWindow)
{
	OutError.Reset();
	if (OutWindow) { *OutWindow = {}; }
	if (FX.Mode == EGGYGOComboFXMode::Disabled) { return true; }
	if (FX.Mode != EGGYGOComboFXMode::MontageWindow)
	{
		OutError = TEXT("FX.Mode 不是显式 Disabled 或 MontageWindow。");
		return false;
	}
	if (!FMath::IsFinite(FX.RemoteSeekDeltaSeconds) || FX.RemoteSeekDeltaSeconds <= 0.0f
		|| !FMath::IsFinite(FX.RemoteMaxSimTimeSeconds) || FX.RemoteMaxSimTimeSeconds <= 0.0f)
	{
		OutError = TEXT("MontageWindow FX 必须显式配置有限正值的 RemoteSeekDeltaSeconds 与 RemoteMaxSimTimeSeconds。");
		return false;
	}
	if (!FMath::IsFinite(RangeStart) || !FMath::IsFinite(RangeEnd)
		|| RangeStart < 0.0f || RangeEnd <= RangeStart)
	{
		OutError = TEXT("FX 原动作绑定范围必须有限、非负且具有正时长。");
		return false;
	}
	if (FX.System && !FX.SystemUnits.IsEmpty())
	{
		OutError = TEXT("FX System 与 SystemUnits 是互斥的显式配置，不能同时启用。");
		return false;
	}
	const TArray<FGGYGONiagaraEffectUnit> Units = GetFXUnits(FX);
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfigurations(Units, FX.Space, FX.LocalTransform, OutError))
	{
		return false;
	}
	if (!FX.BeginEventTag.IsValid() || !FX.EndEventTag.IsValid()
		|| FX.BeginEventTag.MatchesTag(FX.EndEventTag) || FX.EndEventTag.MatchesTag(FX.BeginEventTag))
	{
		OutError = TEXT("FX Begin/End 必须是有效且互不包含的独立 GameplayTag。");
		return false;
	}
	const FGameplayTag ReservedTags[] = {
		GGYGOGameplayTags::Event_Montage_HitWindowBegin, GGYGOGameplayTags::Event_Montage_HitWindowEnd,
		GGYGOGameplayTags::Event_Montage_ComboWindowBegin, GGYGOGameplayTags::Event_Montage_ComboWindowEnd };
	for (FGameplayTag Reserved : ReservedTags)
	{
		if (FX.BeginEventTag.MatchesTag(Reserved) || Reserved.MatchesTag(FX.BeginEventTag)
			|| FX.EndEventTag.MatchesTag(Reserved) || Reserved.MatchesTag(FX.EndEventTag))
		{
			OutError = TEXT("FX 标签不能复用或包含 Hit/Combo 窗口标签。");
			return false;
		}
	}
	if (!UGGYGOAbilityTask_PlayNiagaraEffect::IsStopModeValid(FX.NormalStopMode)
		|| !UGGYGOAbilityTask_PlayNiagaraEffect::IsStopModeValid(FX.AbortStopMode)
		|| (FX.Space != EGGYGONiagaraEffectSpace::Attached
			&& (FX.NormalStopMode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached
				|| FX.AbortStopMode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached)))
	{
		OutError = TEXT("FX 必须显式选择正常/中止退出策略；KeepAttached 只用于 Attached 空间。");
		return false;
	}
	FGGYGOMontageFXWindowDeclaration Declaration;
	if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindowDeclaration(
		Montage, FX.BeginEventTag, FX.EndEventTag, RangeStart, RangeEnd, Declaration, OutError)) { return false; }
	if (!Declaration.bQueued || !Declaration.bNoMergeOnConcurrentPlay)
	{
		OutError = TEXT("FX 作者窗须显式使用 Queued 与 NoMergeOnConcurrentPlay。");
		return false;
	}
	if (OutWindow) { *OutWindow = MoveTemp(Declaration); }
	return true;
}

bool FGGYGOMontageActionResources::AdoptTask(UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask)
{
	if (!IsValid(OriginalTask) || TaskOwner.Get() || TaskRegistration.IsValid()) { return false; }
	TaskOwner.Reset(OriginalTask);
	Task = OriginalTask;
	return true;
}

UGGYGOAbilityTask_PlayMontageAndWaitForEvent* FGGYGOMontageActionResources::GetTask() const { return TaskOwner.Get(); }
UGGYGOAbilityTask_PlayNiagaraEffect* FGGYGOMontageActionResources::GetFXTask() const { return FXTaskOwner.Get(); }

bool FGGYGOMontageActionResources::RegisterCallbacks(UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks)
{
	if (!Task.IsValid() || TaskRegistration.IsValid()) { return false; }
	TaskRegistration = Task->RegisterNativeCallbacks(MoveTemp(Callbacks));
	return TaskRegistration.IsValid();
}

bool FGGYGOMontageActionResources::MatchesFailure(const FGGYGOMontageTaskFailureFact& Fact) const
{
	const auto* OriginalTask = Task.Get(true);
	if (!OriginalTask || OriginalTask != TaskOwner.Get() || OriginalTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !Source.IsValid() || Fact.Montage != Source->Montage.Get() || Fact.SlotName != Source->SlotName) { return false; }
	return Fact.Stage == EGGYGOMontageTaskFailureStage::Startup
		? Fact.MontageInstanceId == INDEX_NONE && MontageInstanceId == INDEX_NONE
		: Fact.Stage == EGGYGOMontageTaskFailureStage::Playback && MontageInstanceId != INDEX_NONE
			&& Fact.MontageInstanceId == MontageInstanceId;
}

bool FGGYGOMontageActionResources::ReadOriginalSnapshot(FGGYGOMontageSectionSnapshot& OutSnapshot, FString& OutError) const
{
	OutSnapshot = {};
	OutError.Reset();
	if (!Source.IsValid() || !Task.IsValid() || !Task->TryGetOriginalSectionSnapshot(OutSnapshot)
		|| OutSnapshot.Montage != Source->Montage.Get() || OutSnapshot.MontageInstanceId == INDEX_NONE
		|| !FMath::IsFinite(OutSnapshot.PositionSeconds) || !FMath::IsFinite(OutSnapshot.InstancePlayRate)
		|| OutSnapshot.InstancePlayRate <= 0.0f || OutSnapshot.InstancePlayRate * Source->MontageRateScale != EffectiveRate)
	{
		OutError = TEXT("Original Task section/position/rate snapshot is unavailable or differs from the fixed playback rate.");
		OutSnapshot = {};
		return false;
	}
	return true;
}

bool FGGYGOMontageActionResources::BeginMotion(const FGGYGOMontageSectionSnapshot& Snapshot,
	const FGGYGOActionMotionStartFacing& StartFacing, FQuat& OutEntryActorRotation, FString& OutError)
{
	OutError.Reset();
	OutEntryActorRotation = FQuat::Identity;
	if (!Movement.IsValid() || !Source.IsValid() || MotionHandle != INDEX_NONE
		|| Snapshot.Montage != Source->Montage.Get() || Snapshot.MontageInstanceId == INDEX_NONE)
	{
		OutError = TEXT("Original CMC/source/playback is invalid or motion has already been issued.");
		return false;
	}
	MontageInstanceId = Snapshot.MontageInstanceId;
	FQuat AcceptedEntryActorRotation = FQuat::Identity;
	if (!Movement->BeginMontageActionMotion(Source, MontageInstanceId, Snapshot.PositionSeconds,
		EffectiveRate, TranslationScale, StartFacing, MotionHandle, AcceptedEntryActorRotation, OutError)
		|| MotionHandle == INDEX_NONE)
	{
		return false;
	}
	OutEntryActorRotation = AcceptedEntryActorRotation;
	return true;
}

bool FGGYGOMontageActionResources::BeginProfileMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate,
	FGGYGOActionMotionFailureDelegate Failure, FString& OutError)
{
	OutError.Reset();
	if (!Movement.IsValid() || !IsValid(Profile) || MotionHandle != INDEX_NONE || Source.IsValid())
	{
		OutError = TEXT("Original CMC/Profile is invalid, motion has already been issued, or a Montage motion source is configured.");
		return false;
	}
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> OriginalMovement = Movement;
	const int32 Handle = OriginalMovement->BeginActionMotion(Profile, PlayRate);
	if (Handle == INDEX_NONE)
	{
		OutError = TEXT("Original CMC rejected the Profile motion.");
		return false;
	}
	MotionHandle = Handle;
	bProfileMotion = true;
	if (!OriginalMovement.IsValid())
	{
		OutError = TEXT("Original CMC expired after starting the Profile motion.");
		return false;
	}
	return OriginalMovement->ObserveActionMotionFailure(Handle, MoveTemp(Failure), OutError);
}

bool FGGYGOMontageActionResources::ObserveMotion(FGGYGOActionMotionFailureDelegate Failure,
	FGGYGOActionMotionCompletionDelegate Completion, TFunctionRef<bool()> IsCurrent, FString& OutError)
{
	OutError.Reset();
	if (!Movement.IsValid() || MotionHandle == INDEX_NONE) { OutError = TEXT("Missing original CMC handle."); return false; }
	const bool bFailureObserved = Movement->ObserveMontageActionMotionFailure(MotionHandle, MoveTemp(Failure), OutError);
	if (!IsCurrent()) { return true; }
	if (!bFailureObserved) { return false; }
	if (!Movement.IsValid()) { OutError = TEXT("Original CMC expired during failure observation."); return false; }
	return Movement->ObserveMontageActionMotionCompletion(MotionHandle, MoveTemp(Completion), OutError);
}

bool FGGYGOMontageActionResources::AcceptNaturalBlendOut(FString& OutError)
{
	OutError.Reset();
	if (bExpectFXBegin && !bFXBeginConsumed) { OutError = TEXT("Original natural blend-out began without the required authenticated FX begin event."); return false; }
	if (!Movement.IsValid() || MotionHandle == INDEX_NONE) { OutError = TEXT("Missing original CMC handle at natural blend-out."); return false; }
	return Movement->NotifyMontageActionNaturalBlendOut(MotionHandle, MontageInstanceId, OutError);
}

bool FGGYGOMontageActionResources::AcceptMontageCompletion(FString& OutError)
{
	OutError.Reset();
	if (bExpectFXBegin && !bFXBeginConsumed) { OutError = TEXT("Original Montage completed without the required authenticated FX begin event."); return false; }
	bMontageCompleted = true;
	if (!Movement.IsValid() || MotionHandle == INDEX_NONE) { OutError = TEXT("Missing original CMC handle at Montage completion."); return false; }
	return Movement->ReleaseMontageActionMotion(MotionHandle, EGGYGOActionMotionReleaseReason::Completed, OutError);
}

bool FGGYGOMontageActionResources::ArmWatchdog(USkeletalMeshComponent* Mesh, UWorld* World, float StartPosition,
	float GraceSeconds, FTimerDelegate Callback, FString& OutError)
{
	OutError.Reset();
	auto* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	const auto* Playback = Anim ? Anim->GetMontageInstanceForID(MontageInstanceId) : nullptr;
	if (!World || !Source.IsValid() || !Playback || Playback->Montage != Source->Montage.Get()
		|| !Playback->bEnableAutoBlendOut || !FMath::IsFinite(Playback->DefaultBlendTimeMultiplier)
		|| Playback->DefaultBlendTimeMultiplier < 0.0f || !FMath::IsFinite(StartPosition)
		|| !FMath::IsFinite(EffectiveRate) || EffectiveRate <= 0.0f || !FMath::IsFinite(GraceSeconds) || GraceSeconds < 0.0f)
	{ OutError = TEXT("Original world/playback or natural blend-out budget is invalid."); return false; }
	const float BlendSeconds = Source->Montage->GetDefaultBlendOutTime() * Playback->DefaultBlendTimeMultiplier;
	const float Timeout = FMath::Max(0.0f, Source->MontageEndSeconds - StartPosition) / EffectiveRate + BlendSeconds + GraceSeconds;
	if (!FMath::IsFinite(BlendSeconds) || BlendSeconds < 0.0f || !FMath::IsFinite(Timeout) || Timeout <= 0.0f)
	{ OutError = TEXT("Original completion deadline is invalid."); return false; }
	ClearWatchdog();
	WatchdogWorld = World;
	World->GetTimerManager().SetTimer(WatchdogHandle, MoveTemp(Callback), Timeout, false);
	return WatchdogHandle.IsValid();
}

bool FGGYGOMontageActionResources::ArmFixedWatchdog(UWorld* World, float TimeoutSeconds, FTimerDelegate Callback, FString& OutError)
{
	OutError.Reset();
	if (!World || !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0f)
	{
		OutError = TEXT("Original world or fixed completion deadline is invalid.");
		return false;
	}
	ClearWatchdog();
	WatchdogWorld = World;
	World->GetTimerManager().SetTimer(WatchdogHandle, MoveTemp(Callback), TimeoutSeconds, false);
	if (!WatchdogHandle.IsValid()) { OutError = TEXT("Original world could not register the completion deadline."); }
	return WatchdogHandle.IsValid();
}

void FGGYGOMontageActionResources::ClearWatchdog()
{
	const auto World = WatchdogWorld;
	FTimerHandle OriginalHandle = WatchdogHandle;
	WatchdogWorld.Reset();
	WatchdogHandle.Invalidate();
	if (World.IsValid()) { World->GetTimerManager().ClearTimer(OriginalHandle); }
}

void FGGYGOMontageActionResources::ReleaseFX(bool bNormalExit)
{
	TStrongObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> OriginalOwner = MoveTemp(FXTaskOwner);
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> OriginalFX(OriginalOwner.Get());
	const FScriptDelegate OriginalFailureCallback = FXFailureCallback;
	const FGGYGOMontageFXLocalStartTicket OriginalTicket = FXLocalStartTicket;
	const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> OriginalPublisher = FXPublicationOwner;
	const TArray<FGGYGOMontageFXLease> OriginalLeases = MoveTemp(FXPublicationLeases);
	const EGGYGONiagaraEffectStopMode StopMode = bNormalExit ? FX.NormalStopMode : FX.AbortStopMode;
	const EGGYGOMontageFXCloseReason CloseReason = bNormalExit
		? EGGYGOMontageFXCloseReason::Normal : EGGYGOMontageFXCloseReason::Abort;
	FXFailureCallback.Unbind();
	FXLocalStartTicket = {};
	FXPublicationOwner.Reset();
	bFXWindowClosed = true;
	if (!FXTerminalReason.IsSet()) { FXTerminalReason = CloseReason; }
	UGGYGOAbilityTask_PlayNiagaraEffect* TaskToRelease = OriginalFX.Get();
	if (TaskToRelease)
	{
		TaskToRelease->OnFailed.Remove(OriginalFailureCallback);
		if (OriginalTicket.IsSet()) { TaskToRelease->CancelMontageFXLocalStartRequest(OriginalTicket); }
	}
	// 专服无本地组件、Task 自然 Finished 等路径仍须关闭原发布。
	for (const FGGYGOMontageFXLease& OriginalLease : OriginalLeases)
	{
		ClosePublication(OriginalPublisher, OriginalLease, CloseReason);
	}
	TaskToRelease = OriginalFX.Get();
	if (TaskToRelease) { TaskToRelease->StopEffect(StopMode); }
	// 不在外调后访问 holder；本地 Released 由 Task 的真实原生资源收尾事实发布。
}

void FGGYGOMontageActionResources::ReleaseMotion(EGGYGOActionMotionReleaseReason Reason)
{
	const int32 OriginalHandle = MotionHandle;
	const bool bOriginalProfileMotion = bProfileMotion;
	MotionHandle = INDEX_NONE;
	bProfileMotion = false;
	if (!Movement.IsValid() || OriginalHandle == INDEX_NONE) { return; }
	if (bOriginalProfileMotion)
	{
		Movement->EndActionMotion(OriginalHandle);
		return;
	}
	FString Error;
	if (!Movement->ReleaseMontageActionMotion(OriginalHandle, Reason, Error))
	{ UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("MontageAction [%s] original handle %d cleanup failed: %s"), *AbilityPath, OriginalHandle, *Error); }
}

void FGGYGOMontageActionResources::ReleaseTraceWindow()
{
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace = Trace;
	const FGGYGOMeleeTraceWindowHandle OriginalWindow = TraceWindow;
	const FDelegateHandle OriginalHits = TraceHits;
	TraceWindow = {};
	TraceWindowSource = {};
	TraceHits.Reset();
	if (UGGYGOMeleeTraceComponent* Receiver = OriginalTrace.Get(); Receiver && OriginalHits.IsValid())
	{
		Receiver->UnsubscribeWindowHit(OriginalHits);
	}
	if (UGGYGOMeleeTraceComponent* Receiver = OriginalTrace.Get(); Receiver && OriginalWindow.HasWindow())
	{
		Receiver->CloseOwnedTraceWindow(OriginalWindow);
	}
}

void FGGYGOMontageActionResources::UnregisterTaskCallbacks()
{
	const FDelegateHandle Registration = TaskRegistration;
	TaskRegistration.Reset();
	if (UGGYGOAbilityTask_PlayMontageAndWaitForEvent* OriginalTask = TaskOwner.Get(); OriginalTask && Registration.IsValid())
	{
		OriginalTask->UnregisterNativeCallbacks(Registration);
	}
}

void FGGYGOMontageActionResources::ReleaseTask(bool bOwnerEnded)
{
	TStrongObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalOwner = MoveTemp(TaskOwner);
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> OriginalTask(OriginalOwner.Get());
	const auto Registration = TaskRegistration;
	TaskRegistration.Reset();
	Task.Reset();
	if (!OriginalTask.IsValid()) { return; }
	OriginalTask->UnregisterNativeCallbacks(Registration);
	if (!OriginalTask.IsValid()) { return; }
	if (bOwnerEnded) { OriginalTask->TaskOwnerEnded(); }
	else { OriginalTask->EndTask(); } // Next original ASC playback owns replacement blending.
}

bool FGGYGOMontageActionResources::ConsumeFXWindow(UGGYGOGameplayAbility* Owner, FName FailureFunction,
	const FGGYGOMontageWindowFact& Fact,
	FGGYGOMontageFXLocalStartReadyDelegate ReadyCallback,
	TFunctionRef<bool()> IsCurrent, FString& OutError)
{
	OutError.Reset();
	if (!IsCurrent()) { return true; }
	if (!Source.IsValid()) { OutError = TEXT("Original FX animation source is unavailable."); return false; }
	if (!Task.IsValid())
	{
		if (bMontageCompleted) { return true; } // 完成后的历史事件不再授予表现播放。
		OutError = TEXT("Original FX playback Task is unavailable before completion.");
		return false;
	}
	if (FX.Mode != EGGYGOComboFXMode::MontageWindow
		|| Fact.Declaration.BeginEventTag != FX.BeginEventTag
		|| Fact.Declaration.EndEventTag != FX.EndEventTag) { return true; }
	USkeletalMeshComponent* const Mesh = Fact.Mesh;
	if (!Fact.Declaration.HasSameWindow(FXWindow))
	{
		OutError = TEXT("已认证窗口与本段保存的 FX 原作者条目不一致。");
		return false;
	}
	if (Fact.Phase == EGGYGOMontageWindowPhase::End)
	{
		if (bExpectFXBegin && !bFXBeginConsumed)
		{
			OutError = TEXT("Original FX window ended without its required authenticated begin event.");
			return false;
		}
		ReleaseFX(true);
		return true;
	}
	if (Fact.Phase != EGGYGOMontageWindowPhase::Begin)
	{
		OutError = TEXT("FX 窗口阶段不是 Begin 或 End。");
		return false;
	}
	if (bFXBeginConsumed || bFXWindowClosed) { return true; }
	bFXBeginConsumed = true;
	if (GetFXTask() || !IsValid(Mesh))
	{
		OutError = FString::Printf(TEXT("Required Mesh [%s] is invalid or an earlier FX Task was not released."),
			*GetPathNameSafe(Mesh));
		return false;
	}
	if (!ValidateFXSocketsOnMesh(FX, *Mesh, OutError)) { return false; }
	UGGYGOAbilitySystemComponent* const Issuer = Owner ? Owner->GetGGYGOAbilitySystemComponentFromActorInfo() : nullptr;
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalIssuer(Issuer);
	const TWeakObjectPtr<AActor> OriginalAvatar(Mesh->GetOwner());
	const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh(Mesh);
	const TWeakObjectPtr<UWorld> OriginalWorld(Mesh->GetWorld());
	if (!Issuer || !OriginalAvatar.IsValid() || !OriginalWorld.IsValid()
		|| Owner->GetAvatarActorFromActorInfo() != OriginalAvatar.Get() || Issuer->GetWorld() != OriginalWorld.Get())
	{
		OutError = TEXT("FX 原 Issuer/Avatar/Mesh/World 来源不一致或已失效。");
		return false;
	}
	FTransform PlaybackTransform = FX.LocalTransform;
	if (FX.Space == EGGYGONiagaraEffectSpace::World)
	{
		const FTransform Origin = FX.SocketName.IsNone() ? Mesh->GetComponentTransform()
			: Mesh->GetSocketTransform(FX.SocketName, RTS_World);
		PlaybackTransform = FX.LocalTransform * Origin;
	}
	if (!IsCurrent()) { return true; }
	const TArray<FGGYGONiagaraEffectUnit> Units = GetFXUnits(FX);
	if (!PublishFXWindow(Owner, Issuer, OriginalAvatar.Get(), Fact, Units, PlaybackTransform, IsCurrent, OutError)) { return false; }
	if (!IsCurrent() || bFXWindowClosed) { return true; }
	UGGYGOAbilityTask_PlayNiagaraEffect* const StartedTask = UGGYGOAbilityTask_PlayNiagaraEffect::PlayNiagaraEffects(
		Owner, TEXT("PlayerComboFX"), Units, FX.Space,
		FX.Space == EGGYGONiagaraEffectSpace::Attached ? Mesh : nullptr,
		FX.Space == EGGYGONiagaraEffectSpace::Attached ? FX.SocketName : NAME_None, PlaybackTransform);
	if (!IsCurrent() || bFXWindowClosed)
	{
		if (IsValid(StartedTask)) { StartedTask->EndTask(); }
		ReleaseFX(FXTerminalReason.IsSet() && FXTerminalReason.GetValue() == EGGYGOMontageFXCloseReason::Normal);
		return true;
	}
	if (!IsValid(StartedTask))
	{
		OutError = FString::Printf(TEXT("FX 组合 Task 创建失败，独立单元数=%d。"), Units.Num());
		ReleaseFX(false);
		return false;
	}
	FXTaskOwner.Reset(StartedTask);
	FXFailureCallback.BindUFunction(Owner, FailureFunction);
	StartedTask->OnFailed.AddUnique(FXFailureCallback);
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> OriginalTask(StartedTask);
	FGGYGOMontageFXLocalStartTicket Ticket;
	const EGGYGOMontageFXLocalStartResult Request = StartedTask->RequestMontageFXLocalStart(
		OriginalIssuer.Get(), OriginalAvatar.Get(), OriginalMesh.Get(), OriginalWorld.Get(), Ticket, OutError);
	if (!IsCurrent() || bFXWindowClosed || GetFXTask() != StartedTask)
	{
		if (UGGYGOAbilityTask_PlayNiagaraEffect* HistoricalTask = OriginalTask.Get())
		{
			if (Ticket.IsSet()) { HistoricalTask->CancelMontageFXLocalStartRequest(Ticket); }
		}
		ReleaseFX(FXTerminalReason.IsSet() && FXTerminalReason.GetValue() == EGGYGOMontageFXCloseReason::Normal);
		return true;
	}
	if (!Ticket.IsSet() || (Request != EGGYGOMontageFXLocalStartResult::ReadyToActivate
		&& Request != EGGYGOMontageFXLocalStartResult::WaitingForRemoteRelease
		&& Request != EGGYGOMontageFXLocalStartResult::DedicatedServerNoPresentation))
	{
		if (OutError.IsEmpty()) { OutError = FString::Printf(TEXT("本地 FX 启动请求未取得有效 Ticket，结果=%d。"), int32(Request)); }
		if (Ticket.IsSet()) { StartedTask->CancelMontageFXLocalStartRequest(Ticket); }
		ReleaseFX(false);
		return false;
	}
	FXLocalStartTicket = Ticket;
	if (!ReadyCallback.IsBound())
	{
		OutError = TEXT("FX 原 holder 未提供启动就绪观察者。");
		ReleaseFX(false);
		return false;
	}
	// Ready 不是永久许可，当前资源版本可在消费前改变；所有请求都先保存观察者。
	// register-and-call 覆盖同步 Ready，StillWaiting 则保留原票，等待下一次真实归还。
	const bool bObserved = StartedTask->ObserveMontageFXLocalStartReady(Ticket, MoveTemp(ReadyCallback));
	if (!IsCurrent() || bFXWindowClosed || GetFXTask() != StartedTask) { return true; }
	if (!bObserved)
	{
		OutError = TEXT("FX 原 Ticket 就绪观察未注册成功。");
		ReleaseFX(false);
		return false;
	}
	return true; // 实际启动结果仅在 Granted→Ready 的回调返栈核对，等待保持 NotRequested。
}

bool FGGYGOMontageActionResources::PublishFXWindow(UGGYGOGameplayAbility* Owner,
	UGGYGOAbilitySystemComponent* Issuer, AActor* Avatar, const FGGYGOMontageWindowFact& Fact,
	const TArray<FGGYGONiagaraEffectUnit>& Units, const FTransform& PlaybackTransform,
	TFunctionRef<bool()> IsCurrent, FString& OutError)
{
	if (!Issuer->IsOwnerActorAuthoritative()) { return true; }
	UGGYGOMontageFXReplicationComponent* Host = UGGYGOMontageFXReplicationComponent::FindForASC(Issuer);
	if (!Host)
	{
		OutError = FString::Printf(TEXT("原 ASC [%s] 的 Owner [%s] 没有 Montage FX 发布宿主。"),
			*GetPathNameSafe(Issuer), *GetPathNameSafe(Issuer->GetOwner()));
		return false;
	}
	const TWeakObjectPtr<UGGYGOMontageFXReplicationComponent> OriginalHost(Host);
	const TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Publisher(Host->GetPublicationOwner());
	if (!IsCurrent() || bFXWindowClosed) { return true; }
	if (!Publisher.IsValid()) { OutError = TEXT("原宿主的 Montage FX 发行者尚未建立。"); return false; }
	FGGYGOMontageFXRecord Record;
	Record.Avatar = Avatar;
	Record.Mesh = Fact.Mesh;
	Record.Space = FX.Space;
	Record.NormalStopMode = FX.NormalStopMode;
	Record.AbortStopMode = FX.AbortStopMode;
	Record.RemoteSeekDeltaSeconds = FX.RemoteSeekDeltaSeconds;
	Record.RemoteMaxSimTimeSeconds = FX.RemoteMaxSimTimeSeconds;
	for (const FGGYGONiagaraEffectUnit& Unit : Units)
	{
		Unit.ResolveAttachment(FX.Space == EGGYGONiagaraEffectSpace::Attached ? FX.SocketName : NAME_None,
			PlaybackTransform, Record.SocketName, Record.Transform);
		Record.System = Unit.System;
		Record.FieldSettings = Unit.FieldSettings;
		FGGYGOMontageFXLease NewLease;
		UGGYGOMontageFXReplicationComponent* LiveHost = OriginalHost.Get();
		const bool bPublished = LiveHost
			&& LiveHost->PublishMontageFX(Owner, Activation, Task.Get(), Fact, Record, NewLease, OutError);
		if (!IsCurrent() || bFXWindowClosed)
		{
			const EGGYGOMontageFXCloseReason Reason = FXTerminalReason.IsSet()
				? FXTerminalReason.GetValue() : EGGYGOMontageFXCloseReason::Abort;
			ClosePublication(Publisher, NewLease, Reason);
			return true;
		}
		if (!bPublished || !NewLease.HasLease())
		{
			ClosePublication(Publisher, NewLease, EGGYGOMontageFXCloseReason::Abort);
			if (OutError.IsEmpty()) { OutError = TEXT("FX 发行者未返回明确成功及原 lease。"); }
			return false;
		}
		FXPublicationOwner = Publisher;
		FXPublicationLeases.Add(MoveTemp(NewLease));
	}
	return true;
}

bool FGGYGOMontageActionResources::ContinueFXLocalStart(const FGGYGOMontageFXLocalStartReadyFact& Fact,
	TFunctionRef<bool()> IsCurrent, FString& OutError)
{
	OutError.Reset();
	if (!FXLocalStartTicket.IsSet()
		|| !(Fact.Ticket == FXLocalStartTicket) || Fact.Task.Get() != GetFXTask()) { return true; }
	if (!IsCurrent() || bFXWindowClosed)
	{
		if (UGGYGOAbilityTask_PlayNiagaraEffect* OriginalTask = Fact.Task.Get())
		{
			OriginalTask->CancelMontageFXLocalStartRequest(Fact.Ticket);
		}
		return true;
	}
	return TryActivateFXLocalTask(IsCurrent, OutError);
}

bool FGGYGOMontageActionResources::TryActivateFXLocalTask(TFunctionRef<bool()> IsCurrent, FString& OutError)
{
	OutError.Reset();
	if (!IsCurrent() || bFXWindowClosed) { return true; }
	UGGYGOAbilityTask_PlayNiagaraEffect* const StartedTask = GetFXTask();
	if (!IsValid(StartedTask) || !FXLocalStartTicket.IsSet())
	{
		OutError = TEXT("原 FX 启动 Task 或 Ticket 已失效。");
		ReleaseFX(false);
		return false;
	}
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> OriginalTask(StartedTask);
	const EGGYGOMontageFXPermitResult Permit = StartedTask->TryConsumeMontageFXLocalStartPermit(FXLocalStartTicket);
	if (!IsCurrent() || bFXWindowClosed || GetFXTask() != StartedTask) { return true; }
	if (Permit == EGGYGOMontageFXPermitResult::StillWaiting || Permit == EGGYGOMontageFXPermitResult::AlreadyConsumed) { return true; }
	if (Permit != EGGYGOMontageFXPermitResult::Granted)
	{
		OutError = FString::Printf(TEXT("原 FX Ticket 不能取得启动许可，结果=%d。"), int32(Permit));
		ReleaseFX(false);
		return false;
	}
	StartedTask->ReadyForActivation();
	if (!IsCurrent() || bFXWindowClosed || GetFXTask() != StartedTask) { return true; }
	UGGYGOAbilityTask_PlayNiagaraEffect* const HistoricalTask = OriginalTask.Get(true);
	if (!HistoricalTask || HistoricalTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutError = TEXT("Original FX Task disappeared before its startup request result could be read.");
		ReleaseFX(false);
		return false;
	}
	const EGGYGONiagaraEffectStartResult StartResult = HistoricalTask->GetStartResult();
	if (StartResult != EGGYGONiagaraEffectStartResult::SpawnAndActivationRequested
		&& StartResult != EGGYGONiagaraEffectStartResult::DedicatedServerNoPresentation)
	{
		OutError = FString::Printf(TEXT("FX 组合 Task [%s] 未返回明确的成功启动结果 (%d)。"),
			*GetPathNameSafe(HistoricalTask), static_cast<int32>(StartResult));
		ReleaseFX(false);
		return false;
	}
	return true;
}
