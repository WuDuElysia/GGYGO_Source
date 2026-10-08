/**
 * @file GGYGOActionMotionExecution.cpp
 * @brief Original action resource lifetime, native registration and exact observer retirement.
 */
#include "Character/Components/GGYGOActionMotionExecution.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Components/GGYGOActionCurveRootMotionSource.h"
#include "Character/Data/GGYGOActionMotionProfile.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/Character.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOActionMotion, Log, All);

const FName& FGGYGOActionMotionExecution::SourceName()
{
	static const FName Name(TEXT("GGYGO.ActionCurve"));
	return Name;
}

bool FGGYGOActionMotionExecution::IsRegisteredSource(const FRootMotionSource& Source)
{
	if (Source.GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()
		|| Source.InstanceName != SourceName() || Source.Priority != SourcePriority) return false;
	const auto& Typed = static_cast<const FRootMotionSource_GGYGOActionCurve&>(Source);
	return Source.AccumulateMode == ERootMotionAccumulateMode::Override
		|| (Typed.bExplicitlyCancelled && Source.AccumulateMode == ERootMotionAccumulateMode::Additive);
}

FGGYGOActionMotionExecution::FRetiredObservers FGGYGOActionMotionExecution::RetireCompletedResource()
{
	if (CompletedMontageActionResource.IsValid()) LastRetiredMontageActionHandle = CompletedMontageActionResource->Handle;
	NeutralizeActionSource(CompletedMontageActionResource);
	CompletedMontageActionResource.Reset();
	bCompletedMontageActionNativeContributionConsumed = false;
	FRetiredObservers Retired{MoveTemp(ActionMotionFailureCallback), MoveTemp(ActionMotionCompletionCallback)};
	ActionMotionFailureResource.Reset();
	ActionMotionCompletionResource.Reset();
	bMontageActionCompletionObserverInstalled = false;
	MontageActionNaturalBlendOutResource.Reset();
	return Retired;
}

FGGYGOActionMotionExecution::FRetiredObservers FGGYGOActionMotionExecution::Close()
{
	NeutralizeActionSource(ActiveMontageActionResource);
	NeutralizeActionSource(ActiveProfileActionResource);
	ActiveMontageActionResource.Reset();
	ActiveProfileActionResource.Reset();
	auto Retired = RetireCompletedResource();
	ActiveActionMotionHandle = INDEX_NONE;
	ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
	ActionSkippedMovementTickTime = 0.0f;
	return Retired;
}

bool FGGYGOActionMotionExecution::IsMontageActionMovementModeSupported() const
{
	return Movement.IsMovingOnGround() || Movement.IsFalling();
}

bool FGGYGOActionMotionExecution::BeginMontageActionMotion(
	const FGGYGOActionMotionSourceBindingPtr& OriginalSource, int32 OriginalMontageInstanceId,
	float MontagePositionSeconds, float EffectiveMontagePlayRate, float TranslationScale,
	int32& OutHandle, FString& OutError)
{
	OutHandle = INDEX_NONE;
	OutError.Reset();
	const auto Reject = [this, &OriginalSource, &OutError](const FString& Reason)
	{
		OutError = FString::Printf(TEXT("[Movement.ActionMotion] CMC='%s' Montage='%s' Slot='%s' Section='%s' Reason='%s'"),
			*Movement.GetPathName(), OriginalSource.IsValid() ? *GetPathNameSafe(OriginalSource->Montage.Get()) : TEXT("None"),
			OriginalSource.IsValid() ? *OriginalSource->SlotName.ToString() : TEXT("None"),
			OriginalSource.IsValid() ? *OriginalSource->SectionName.ToString() : TEXT("None"), *Reason);
		return false;
	};
	if (!IsInGameThread() || !IsValid(Movement.CharacterOwner) || Movement.IsBeingDestroyed() || !IsMontageActionMovementModeSupported()
		|| (!Movement.CharacterOwner->HasAuthority() && (Movement.CharacterOwner->GetLocalRole() != ROLE_AutonomousProxy
			|| !Movement.CharacterOwner->IsLocallyControlled())) || Movement.CharacterOwner->bClientUpdating)
		return Reject(TEXT("a live authority or original autonomous owner in Walking, NavWalking or Falling is required"));
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(&Movement);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(Movement.CharacterOwner);
	const auto OriginalOwnerContext = Movement.MovementOwnerSyncContext;
	const int32 OriginalNextHandle = NextActionMotionHandle;
	CleanupFinishedActionMotion();
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || !OriginalCharacter.IsValid()
		|| Self->CharacterOwner != OriginalCharacter.Get() || Self->MovementOwnerSyncContext != OriginalOwnerContext
		|| Self->ActionMotionExecution.NextActionMotionHandle != OriginalNextHandle)
	{
		OutError = TEXT("[Movement.ActionMotion] original owner/action slot changed during prior action retirement");
		return false;
	}
	if (HasActiveActionMotion() || NextActionMotionHandle <= 0 || NextActionMotionHandle == MAX_int32)
		return Reject(TEXT("action execution slot is occupied or original token allocator is exhausted"));
	FString Error;
	if (!OriginalSource.IsValid() || !GGYGOActionMotionEvaluation::ValidateSource(*OriginalSource, Error))
		return Reject(Error.IsEmpty() ? TEXT("original source binding is missing") : Error);
	if (!FMath::IsFinite(MontagePositionSeconds) || MontagePositionSeconds < OriginalSource->MontageStartSeconds
		|| MontagePositionSeconds >= OriginalSource->MontageEndSeconds
		|| !FMath::IsFinite(EffectiveMontagePlayRate) || EffectiveMontagePlayRate <= 0.0f
		|| !FMath::IsFinite(TranslationScale) || TranslationScale < 0.0f)
		return Reject(TEXT("original position must be in the section; rate positive and translation scale non-negative, all finite"));
	USkeletalMeshComponent* Mesh = Movement.CharacterOwner->GetMesh();
	UAnimInstance* Anim = Mesh ? Mesh->GetAnimInstance() : nullptr;
	FAnimMontageInstance* Instance = Anim ? Anim->GetMontageInstanceForID(OriginalMontageInstanceId) : nullptr;
	const FGGYGOActionMotionSourceSection* InitialSection = Instance
		? OriginalSource->FindSection(Instance->GetCurrentSection()) : nullptr;
	if (!IsValid(Mesh) || !IsValid(Anim) || !Instance || Instance->Montage != OriginalSource->Montage.Get()
		|| !Instance->IsActive() || !Instance->IsPlaying() || !InitialSection
		|| MontagePositionSeconds < InitialSection->MontageStartSeconds
		|| MontagePositionSeconds >= InitialSection->MontageEndSeconds
		|| !FMath::IsNearlyEqual(Instance->GetPosition(), MontagePositionSeconds, UE_KINDA_SMALL_NUMBER)
		|| !FMath::IsFinite(Instance->GetPlayRate()) || Instance->GetPlayRate() <= 0.0f
		|| !FMath::IsNearlyEqual(Instance->GetPlayRate() * OriginalSource->MontageRateScale,
			EffectiveMontagePlayRate, UE_KINDA_SMALL_NUMBER) || Movement.CharacterOwner->IsPlayingRootMotion())
		return Reject(TEXT("original Montage/instance/section/position/rate is not current or native animation RootMotion is active"));
	const float Duration = (OriginalSource->MontageEndSeconds - MontagePositionSeconds) / EffectiveMontagePlayRate;
	const FQuat Rotation = Movement.CharacterOwner->GetActorQuat();
	const FVector Scale = Mesh->GetComponentScale() * TranslationScale; // Original avatar/Actor scale once; Actor basis, no Mesh rotation.
	if (!FMath::IsFinite(Duration) || Duration <= 0.0f || Rotation.ContainsNaN()
		|| !Rotation.IsNormalized() || Scale.ContainsNaN())
		return Reject(TEXT("native duration or original Actor transform cannot be represented"));
	const auto Resource = MakeShared<FGGYGOActionMotionResource>();
	Resource->Owner = &Movement;
	Resource->Character = Movement.CharacterOwner;
	Resource->Mesh = Mesh;
	Resource->AnimInstance = Anim;
	Resource->Source = OriginalSource;
	Resource->MontageInstanceId = OriginalMontageInstanceId;
	Resource->InstancePlayRate = Instance->GetPlayRate();
	Resource->Handle = NextActionMotionHandle;
	const auto Source = MakeShared<FRootMotionSource_GGYGOActionCurve>();
	Source->SourceMode = EGGYGOActionCurveSourceMode::OriginalMontage;
	// The original XYZ trajectory owns all velocity axes while its native interval is active.
	// Sensitive native liftoff lets authored upward motion leave Walking without a jump impulse.
	Source->Settings.UnSetFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
	Source->Settings.SetFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck);
	Source->OriginalBinding = OriginalSource;
	Source->OriginalResource = Resource;
	Source->MontageStartSeconds = MontagePositionSeconds;
	Source->EntryActorRotation = Rotation;
	Source->TranslationScale = Scale;
	Source->PlayRate = EffectiveMontagePlayRate;
	Source->Duration = Duration;
	Source->InstanceName = SourceName();
	Source->Priority = SourcePriority;
	// Retire only the previous completed native tail before its successor enters this one execution slot.
	const auto RetiredObservers = RetireCompletedResource();
	const uint16 Id = Movement.ApplyRootMotionSource(Source);
	if (Id == static_cast<uint16>(ERootMotionSourceID::Invalid))
		return Reject(TEXT("native ApplyRootMotionSource rejected the original source"));
	ActiveMontageActionResource = Resource;
	ActiveActionMotionHandle = Resource->Handle;
	ActionMotionSourceID = Id;
	++NextActionMotionHandle;
	Movement.SuspendLocomotionForAction();
	OutHandle = Resource->Handle;
	return true;
}

bool FGGYGOActionMotionExecution::ValidateMontageActionRuntime(
	const FRootMotionSource_GGYGOActionCurve& Source, float SimulationTime, FString& OutError) const
{
	OutError.Reset();
	if (!IsValid(Movement.CharacterOwner) || !IsMontageActionMovementModeSupported() || !Source.OriginalBinding.IsValid()
		|| Movement.CharacterOwner->IsPlayingRootMotion())
	{
		OutError = TEXT("original XYZ action requires Walking, NavWalking or Falling, its source and no native animation RootMotion conflict");
		return false;
	}
	if (Source.Settings.HasFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate)
		|| !Source.Settings.HasFlag(ERootMotionSourceSettingsFlags::UseSensitiveLiftoffCheck))
	{
		OutError = TEXT("original XYZ action source must override Z and use native sensitive liftoff");
		return false;
	}
	const auto Resource = Source.OriginalResource;
	if (!Resource.IsValid())
	{
		// Native authoritative RMS replication may drive clients, never grant authority-side actions.
		if (Source.bNativeImported && !Movement.CharacterOwner->HasAuthority()) return true;
		OutError = TEXT("original local execution resource is missing; imported data cannot grant authority execution");
		return false;
	}
	if (Resource->Owner.Get() != &Movement || Resource->Character.Get() != Movement.CharacterOwner
		|| Resource->Source != Source.OriginalBinding)
	{
		OutError = TEXT("source does not belong to its original CMC, Character and immutable binding");
		return false;
	}
	// The saved native source already captures the original validated mapping and release state.
	// Replay never borrows today's AnimInstance/Montage position or a successor's resource.
	if (Movement.CharacterOwner->bClientUpdating) return true;
	UAnimInstance* Anim = Resource->AnimInstance.Get();
	USkeletalMeshComponent* Mesh = Resource->Mesh.Get();
	if (!IsValid(Mesh) || Mesh != Movement.CharacterOwner->GetMesh() || !IsValid(Anim) || Mesh->GetAnimInstance() != Anim)
	{
		OutError = TEXT("original Mesh/AnimInstance membership was retired or replaced");
		return false;
	}
	// The original Task's authenticated Completed request allows only this finite native tail.
	// It is not the consumed-final-interval fact, and never looks up a successor instance.
	if (Source.bCompletionRequested && CompletedMontageActionResource == Resource) return true;
	FAnimMontageInstance* Instance = Anim ? Anim->GetMontageInstanceForID(Resource->MontageInstanceId) : nullptr;
	const FGGYGOActionMotionSourceSection* CurrentSection = Instance
		? Source.OriginalBinding->FindSection(Instance->GetCurrentSection()) : nullptr;
	// Only the original Task's non-interrupted native BlendOut fact permits stopped playback.
	// UE still advances during that configured blend window, then holds End - KINDA_SMALL_NUMBER/2.
	// A stopped hold can consume only the final native interval; a pause/foreign Stop has no grant.
	const bool bNaturalBlendOut = Instance && CurrentSection && Instance->IsStopped()
		&& MontageActionNaturalBlendOutResource == Resource && Source.OriginalBinding->Sections.Num() > 0
		&& CurrentSection == &Source.OriginalBinding->Sections.Last()
		&& FMath::IsFinite(Instance->GetPosition())
		&& (Instance->IsPlaying()
			|| (FMath::IsNearlyEqual(Instance->GetPosition(), Source.OriginalBinding->MontageEndSeconds, UE_KINDA_SMALL_NUMBER)
				&& Source.GetTime() + SimulationTime >= Source.Duration));
	if (!Instance || Instance->Montage != Source.OriginalBinding->Montage.Get()
		|| !FMath::IsFinite(Instance->GetPosition()) || !CurrentSection
		|| Instance->GetPosition() < CurrentSection->MontageStartSeconds
		|| Instance->GetPosition() > CurrentSection->MontageEndSeconds
		|| ((!Instance->IsActive() || !Instance->IsPlaying()) && !bNaturalBlendOut)
		|| !FMath::IsFinite(Instance->GetPlayRate()) || Instance->GetPlayRate() != Resource->InstancePlayRate
		|| (!Source.bCompletionRequested && ActiveMontageActionResource != Resource))
	{
		OutError = TEXT("original runtime identity/playback/rate was retired, replaced, paused or dynamically changed");
		return false;
	}
	return true;
}

bool FGGYGOActionMotionExecution::NotifyMontageActionNaturalBlendOut(
	int32 OriginalHandle, int32 OriginalMontageInstanceId, FString& OutError)
{
	OutError.Reset();
	const auto Resource = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle
		? ActiveMontageActionResource : (CompletedMontageActionResource.IsValid()
			&& CompletedMontageActionResource->Handle == OriginalHandle ? CompletedMontageActionResource : nullptr);
	if (!IsInGameThread() || Movement.IsBeingDestroyed() || !Resource.IsValid()
		|| Resource->Owner.Get() != &Movement || Resource->Character.Get() != Movement.CharacterOwner
		|| Resource->MontageInstanceId != OriginalMontageInstanceId || !Resource->Source.IsValid()
		|| Resource->Source->Sections.Num() == 0)
	{
		OutError = TEXT("[Movement.ActionMotion] natural BlendOut fact requires the original live handle and Montage instance identity");
		return false;
	}
	if (MontageActionNaturalBlendOutResource == Resource) return true;
	// A late original blend fact grants no execution after the genuine final native contribution.
	if (CompletedMontageActionResource == Resource && bCompletedMontageActionNativeContributionConsumed) return true;
	UAnimInstance* Anim = Resource->AnimInstance.Get();
	FAnimMontageInstance* Instance = Anim ? Anim->GetMontageInstanceForID(OriginalMontageInstanceId) : nullptr;
	const UAnimMontage* Montage = Resource->Source->Montage.Get();
	const auto& FinalSection = Resource->Source->Sections.Last();
	const float Rate = Instance ? Instance->GetPlayRate() * Resource->Source->MontageRateScale : 0.0f;
	const float Trigger = Montage && Instance ? (Montage->BlendOutTriggerTime >= 0.0f
		? Montage->BlendOutTriggerTime : Montage->GetDefaultBlendOutTime() * Instance->DefaultBlendTimeMultiplier) : 0.0f;
	if (!Instance || Instance->Montage != Montage || !Instance->IsStopped() || !Instance->bEnableAutoBlendOut
		|| Instance->GetCurrentSection() != FinalSection.SectionName || Instance->GetNextSection() != NAME_None
		|| !FMath::IsFinite(Instance->GetPosition()) || Instance->GetPosition() < FinalSection.MontageStartSeconds
		|| Instance->GetPosition() > FinalSection.MontageEndSeconds || !FMath::IsFinite(Rate) || Rate <= 0.0f
		|| !FMath::IsFinite(Trigger) || Trigger < 0.0f
		|| (FinalSection.MontageEndSeconds - Instance->GetPosition()) / Rate > FMath::Max(Trigger, UE_KINDA_SMALL_NUMBER))
	{
		OutError = TEXT("[Movement.ActionMotion] original non-interrupted BlendOut is outside its configured final-section auto-blend window");
		return false;
	}
	// The original native Task callback authenticates non-interruption; no private engine state is read.
	MontageActionNaturalBlendOutResource = Resource;
	return true;
}

bool FGGYGOActionMotionExecution::ObserveMontageActionMotionCompletion(
	int32 OriginalHandle, FGGYGOActionMotionCompletionDelegate Callback, FString& OutError)
{
	OutError.Reset();
	const auto Resource = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle
		? ActiveMontageActionResource : (CompletedMontageActionResource.IsValid()
			&& CompletedMontageActionResource->Handle == OriginalHandle ? CompletedMontageActionResource : nullptr);
	if (!IsInGameThread() || Movement.IsBeingDestroyed() || !Resource.IsValid() || !Callback.IsBound()
		|| Resource->Owner.Get() != &Movement || Resource->Character.Get() != Movement.CharacterOwner
		|| bMontageActionCompletionObserverInstalled || ActionMotionCompletionCallback.IsBound() || ActionMotionCompletionResource.IsValid())
	{
		OutError = TEXT("[Movement.ActionMotion] completion observer requires the original resource and its sole callback");
		return false;
	}
	bMontageActionCompletionObserverInstalled = true;
	if (CompletedMontageActionResource == Resource && bCompletedMontageActionNativeContributionConsumed)
	{
		Callback.Execute(OriginalHandle);
		return true;
	}
	ActionMotionCompletionResource = Resource;
	ActionMotionCompletionCallback = MoveTemp(Callback);
	return true;
}

void FGGYGOActionMotionExecution::PublishMontageActionMotionCompletion()
{
	if (!Movement.CharacterOwner || Movement.CharacterOwner->bClientUpdating || Movement.PreparingLocomotionCurveReplayGroup) return;
	for (const auto& Base : Movement.CurrentRootMotion.RootMotionSources)
	{
		if (!Base.IsValid() || Base->GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()) continue;
		const auto& Source = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*Base);
		const auto Resource = Source.OriginalResource;
		if (Source.SourceMode != EGGYGOActionCurveSourceMode::OriginalMontage || Source.bExplicitlyCancelled
			|| !Source.bPreparedNaturalEnd || !Source.bPreparedContributionConsumed || !Resource.IsValid()
			|| Resource->Owner.Get() != &Movement || Resource->Character.Get() != Movement.CharacterOwner
			|| (ActiveMontageActionResource != Resource && CompletedMontageActionResource != Resource)
			|| (CompletedMontageActionResource == Resource && bCompletedMontageActionNativeContributionConsumed)) continue;
		CompletedMontageActionResource = Resource;
		bCompletedMontageActionNativeContributionConsumed = true;
		if (ActiveMontageActionResource == Resource)
		{
			ActiveMontageActionResource.Reset();
			ActiveActionMotionHandle = INDEX_NONE;
			ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
		}
		// All native physics for this move has completed. Detach before external cleanup/reentry.
		if (ActionMotionCompletionResource == Resource)
		{
			FGGYGOActionMotionCompletionDelegate Callback = MoveTemp(ActionMotionCompletionCallback);
			ActionMotionCompletionResource.Reset();
			Callback.ExecuteIfBound(Resource->Handle);
		}
		return;
	}
}

bool FGGYGOActionMotionExecution::ObserveActionMotionFailure(
	int32 OriginalHandle, FGGYGOActionMotionFailureDelegate Callback, FString& OutError)
{
	OutError.Reset();
	const auto Resource = ActiveMontageActionResource.IsValid() ? ActiveMontageActionResource : ActiveProfileActionResource;
	if (!IsInGameThread() || Movement.IsBeingDestroyed() || !Resource.IsValid()
		|| Resource->Handle != OriginalHandle || Resource->Owner.Get() != &Movement
		|| Resource->Character.Get() != Movement.CharacterOwner || !HasActiveActionMotion()
		|| !Callback.IsBound() || ActionMotionFailureCallback.IsBound() || ActionMotionFailureResource.IsValid())
	{
		OutError = TEXT("[Movement.ActionMotion] failure observer requires the original live action and its sole callback; no replacement");
		return false;
	}
	ActionMotionFailureResource = Resource;
	ActionMotionFailureCallback = MoveTemp(Callback);
	return true;
}

bool FGGYGOActionMotionExecution::ObserveMontageActionMotionFailure(
	int32 OriginalHandle, FGGYGOActionMotionFailureDelegate Callback, FString& OutError)
{
	if (!ActiveMontageActionResource.IsValid() || ActiveMontageActionResource->Handle != OriginalHandle)
	{
		OutError = TEXT("[Movement.ActionMotion] Montage failure observer requires the original live Montage action");
		return false;
	}
	return ObserveActionMotionFailure(OriginalHandle, MoveTemp(Callback), OutError);
}

bool FGGYGOActionMotionExecution::ValidateProfileActionRuntime(
	const FRootMotionSource_GGYGOActionCurve& Source, FString& OutError) const
{
	OutError.Reset();
	const auto Resource = Source.OriginalResource;
	// A standalone/native-imported Profile source has no local observer or issued action token.
	if (!Resource.IsValid()) return true;
	const UGGYGOActionMotionProfile* Profile = Resource->Profile.Get();
	USkeletalMeshComponent* Mesh = Resource->Mesh.Get();
	if (!IsValid(Movement.CharacterOwner) || !IsValid(Mesh) || Mesh != Movement.CharacterOwner->GetMesh()
		|| Resource->Owner.Get() != &Movement || Resource->Character.Get() != Movement.CharacterOwner
		|| !IsValid(Profile) || Source.SourceMode != EGGYGOActionCurveSourceMode::Profile
		|| Source.TranslationCurve != Profile->TranslationCurve || Source.Duration != Profile->Duration / Source.PlayRate)
	{
		OutError = TEXT("original Profile configuration, owner or playback mapping was retired or changed");
		return false;
	}
	if (!Profile->ValidateMotion(OutError)) return false;
	// SavedMove clones retain the original identity, but never dispatch today's observer during replay.
	if (Movement.CharacterOwner && Movement.CharacterOwner->bClientUpdating) return true;
	if (ActiveProfileActionResource != Resource || ActiveActionMotionHandle != Resource->Handle)
	{
		OutError = TEXT("Profile source no longer belongs to the original active action slot");
		return false;
	}
	return true;
}

void FGGYGOActionMotionExecution::FailProfileActionMotion(
	const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error)
{
	if (!OriginalResource.IsValid() || ActiveProfileActionResource != OriginalResource
		|| (Movement.CharacterOwner && Movement.CharacterOwner->bClientUpdating)) return;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(&Movement);
	++ActionFailurePreparationDepth;
	{
		FGGYGOActionMotionFailureDelegate Callback;
		if (ActionMotionFailureResource == OriginalResource)
		{
			Callback = MoveTemp(ActionMotionFailureCallback);
			ActionMotionFailureResource.Reset();
		}
		NeutralizeActionSource(OriginalResource);
		EndActionMotion(OriginalResource->Handle);
		Callback.ExecuteIfBound(OriginalResource->Handle, Error);
	}
	if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get()) --Self->ActionMotionExecution.ActionFailurePreparationDepth;
}

void FGGYGOActionMotionExecution::FailMontageActionMotion(
	const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource, const FString& Error)
{
	if (!OriginalResource.IsValid() || (Movement.CharacterOwner && Movement.CharacterOwner->bClientUpdating)
		|| (ActiveMontageActionResource != OriginalResource && CompletedMontageActionResource != OriginalResource)) return;
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(&Movement);
	++ActionFailurePreparationDepth;
	{
		FGGYGOActionMotionFailureDelegate Callback;
		if (ActionMotionFailureResource == OriginalResource)
		{
			Callback = MoveTemp(ActionMotionFailureCallback);
			ActionMotionFailureResource.Reset();
		}
		FString ReleaseError;
		ReleaseMontageActionMotion(OriginalResource->Handle, EGGYGOActionMotionReleaseReason::OwnerInvalidated, ReleaseError);
		if (Callback.IsBound()) Callback.Execute(OriginalResource->Handle, Error);
	} // Destroy captures while native Prepare remains protected.
	if (UGGYGOCharacterMovementComponent* Self = WeakSelf.Get()) --Self->ActionMotionExecution.ActionFailurePreparationDepth;
}

void FGGYGOActionMotionExecution::NeutralizeActionSource(
	const TSharedPtr<const FGGYGOActionMotionResource>& OriginalResource)
{
	if (!OriginalResource.IsValid()) return;
	const auto Retire = [&OriginalResource](const auto& Sources)
	{
		for (const auto& Base : Sources)
		{
			if (!Base.IsValid() || Base->GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()) continue;
			auto* Source = static_cast<FRootMotionSource_GGYGOActionCurve*>(Base.Get());
			if (Source->OriginalResource != OriginalResource) continue;
			Source->bExplicitlyCancelled = true;
			Source->RootMotionParams.Set(FTransform::Identity);
			Source->AccumulateMode = ERootMotionAccumulateMode::Additive;
			Source->Status.SetFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
		}
	};
	Retire(Movement.CurrentRootMotion.RootMotionSources);
	Retire(Movement.CurrentRootMotion.PendingAddRootMotionSources);
	if (ActionFailurePreparationDepth != 0) return; // Native Prepare is still deriving group flags.
	// Native Prepare recalculates these flags. Also recompute them for a contribution cancelled after Prepare.
	Movement.CurrentRootMotion.bHasOverrideSources = false;
	Movement.CurrentRootMotion.bHasOverrideSourcesWithIgnoreZAccumulate = false;
	Movement.CurrentRootMotion.bHasAdditiveSources = false;
	for (const auto& Base : Movement.CurrentRootMotion.RootMotionSources)
	{
		if (!Base.IsValid() || !Base->Status.HasFlag(ERootMotionSourceStatusFlags::Prepared)) continue;
		if (Base->AccumulateMode == ERootMotionAccumulateMode::Override)
		{
			Movement.CurrentRootMotion.bHasOverrideSources = true;
			Movement.CurrentRootMotion.bHasOverrideSourcesWithIgnoreZAccumulate |=
				Base->Settings.HasFlag(ERootMotionSourceSettingsFlags::IgnoreZAccumulate);
		}
		else Movement.CurrentRootMotion.bHasAdditiveSources = true;
	}
}

bool FGGYGOActionMotionExecution::ReleaseMontageActionMotion(
	int32 OriginalHandle, EGGYGOActionMotionReleaseReason Reason, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || OriginalHandle == INDEX_NONE
		|| static_cast<uint8>(Reason) > static_cast<uint8>(EGGYGOActionMotionReleaseReason::OwnerInvalidated))
	{
		OutError = TEXT("[Movement.ActionMotion] release requires the original issued token, valid reason and game thread");
		return false;
	}
	const auto Resource = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle
		? ActiveMontageActionResource : (CompletedMontageActionResource.IsValid()
			&& CompletedMontageActionResource->Handle == OriginalHandle ? CompletedMontageActionResource : nullptr);
	if (!Resource.IsValid())
	{
		if (OriginalHandle == LastRetiredMontageActionHandle) return true;
		OutError = FString::Printf(TEXT("[Movement.ActionMotion] token=%d is not the original active/completed resource"), OriginalHandle);
		return false;
	}
	if (Reason == EGGYGOActionMotionReleaseReason::Completed
		&& (Resource->Owner.Get() != &Movement || Resource->Character.Get() != Movement.CharacterOwner || Movement.IsBeingDestroyed()))
	{
		OutError = TEXT("[Movement.ActionMotion] natural completion request no longer belongs to its original owner");
		return false;
	}
	FGGYGOActionMotionFailureDelegate RetiredCallback;
	FGGYGOActionMotionCompletionDelegate RetiredCompletionCallback;
	const bool bNativeComplete = CompletedMontageActionResource == Resource
		&& bCompletedMontageActionNativeContributionConsumed;
	if ((Reason != EGGYGOActionMotionReleaseReason::Completed || bNativeComplete)
		&& MontageActionNaturalBlendOutResource == Resource) MontageActionNaturalBlendOutResource.Reset();
	if ((Reason != EGGYGOActionMotionReleaseReason::Completed || bNativeComplete) && ActionMotionFailureResource == Resource)
	{
		RetiredCallback = MoveTemp(ActionMotionFailureCallback);
		ActionMotionFailureResource.Reset();
	}
	if (Reason != EGGYGOActionMotionReleaseReason::Completed && ActionMotionCompletionResource == Resource)
	{
		RetiredCompletionCallback = MoveTemp(ActionMotionCompletionCallback);
		ActionMotionCompletionResource.Reset();
	}
	if (Reason == EGGYGOActionMotionReleaseReason::Completed)
	{
		if (CompletedMontageActionResource == Resource) return true;
		const auto Complete = [&Resource](const auto& Sources)
		{
			for (const auto& Base : Sources)
				if (Base.IsValid() && Base->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
				{
					auto* Source = static_cast<FRootMotionSource_GGYGOActionCurve*>(Base.Get());
					if (Source->OriginalResource == Resource) Source->bCompletionRequested = true;
				}
		};
		Complete(Movement.CurrentRootMotion.RootMotionSources);
		Complete(Movement.CurrentRootMotion.PendingAddRootMotionSources);
		// The caller reports its authenticated original Task Completed, not a new motion grant.
		// Continue only the original bounded RMS clock until its final contribution is consumed.
		CompletedMontageActionResource = Resource;
		bCompletedMontageActionNativeContributionConsumed = false;
	}
	else
	{
		NeutralizeActionSource(Resource);
		LastRetiredMontageActionHandle = OriginalHandle;
		if (CompletedMontageActionResource == Resource)
		{
			CompletedMontageActionResource.Reset();
			bCompletedMontageActionNativeContributionConsumed = false;
		}
	}
	if (ActiveMontageActionResource == Resource)
	{
		ActiveMontageActionResource.Reset();
		ActiveActionMotionHandle = INDEX_NONE;
		ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
		Movement.CurveMotion.Reset();
	}
	// Completed identity remains bounded until the next original action or owner teardown.
	return true;
}

bool FGGYGOActionMotionExecution::CancelMontageActionMotionForMovement(
	int32 OriginalHandle, const FGGYGOQualifiedMovementIntent& OriginalIntent, FString& OutError)
{
	FGGYGOQualifiedMovementIntent CurrentIntent;
	if (Movement.QueryQualifiedMovementIntent(OriginalIntent.Scope, CurrentIntent, OutError)
		!= EGGYGOQualifiedMovementIntentQueryResult::Qualified || !(CurrentIntent == OriginalIntent))
	{
		if (OutError.IsEmpty()) OutError = TEXT("[Movement.ActionMotion] original movement intent is no longer qualified/current");
		return false;
	}
	const bool bWasActive = ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == OriginalHandle;
	const auto OriginalResource = bWasActive ? ActiveMontageActionResource
		: (CompletedMontageActionResource.IsValid() && CompletedMontageActionResource->Handle == OriginalHandle
			? CompletedMontageActionResource : nullptr);
	const auto HasUnconsumedTail = [&OriginalResource](const auto& Sources)
	{
		return Sources.ContainsByPredicate([&OriginalResource](const auto& Base)
		{
			if (!Base.IsValid() || Base->GetScriptStruct() != FRootMotionSource_GGYGOActionCurve::StaticStruct()) return false;
			const auto& Source = static_cast<const FRootMotionSource_GGYGOActionCurve&>(*Base);
			return OriginalResource.IsValid() && Source.OriginalResource == OriginalResource
				&& !Source.bExplicitlyCancelled && !Source.bPreparedContributionConsumed;
		});
	};
	const bool bHadUnconsumedTail = HasUnconsumedTail(Movement.CurrentRootMotion.RootMotionSources)
		|| HasUnconsumedTail(Movement.CurrentRootMotion.PendingAddRootMotionSources);
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(&Movement);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(Movement.CharacterOwner);
	const int32 OriginalNextHandle = NextActionMotionHandle;
	if (!ReleaseMontageActionMotion(OriginalHandle, EGGYGOActionMotionReleaseReason::Cancelled, OutError)) return false;
	// Delegate capture destruction during release can replace the owner or install and end
	// a successor. Only clear this original cancellation's residual on the unchanged owner.
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	FGGYGOQualifiedMovementIntent AfterReleaseIntent;
	FString AfterReleaseError;
	if (Self && !Self->IsBeingDestroyed() && OriginalCharacter.IsValid()
		&& Self->CharacterOwner == OriginalCharacter.Get() && Self->ActionMotionExecution.NextActionMotionHandle == OriginalNextHandle
		&& Self->ActionMotionExecution.ActiveActionMotionHandle == INDEX_NONE && !Self->ActionMotionExecution.ActiveMontageActionResource.IsValid()
		&& (bWasActive || bHadUnconsumedTail)
		&& Self->QueryQualifiedMovementIntent(OriginalIntent.Scope, AfterReleaseIntent, AfterReleaseError)
			== EGGYGOQualifiedMovementIntentQueryResult::Qualified && AfterReleaseIntent == OriginalIntent)
	{
		Self->Velocity.X = 0.0; Self->Velocity.Y = 0.0;
		Self->NativeMovementVelocityResult.Reset();
		Self->NativeVelocityBeforeRootMotion.Reset();
		Self->bNativeVelocityIntervalCanRetain = false;
		// Airborne cancellation retains actual vertical momentum; native Falling resumes gravity.
		// Never restore the entry mode or manufacture a landing to admit this original input.
		Self->CurveMotion.Reset();
	}
	return true;
}

void FGGYGOActionMotionExecution::ResumeLocomotionAfterAction()
{
	if (ActionSkippedMovementTickTime <= 0.0f || Movement.HasRegisteredActionCurveSource()) return;
	const float OriginalTickTime = ActionSkippedMovementTickTime;
	ActionSkippedMovementTickTime = 0.0f; // The skipped original Before interval can be consumed only once.
	if (Movement.CharacterOwner && Movement.CharacterOwner->GetLocalRole() != ROLE_SimulatedProxy
		&& Movement.HasAcceptedMovementSet() && !Movement.ShouldRejectMovementInputGroundLocomotion()
		&& !Movement.ShouldRejectUnownedCurveGroundLocomotion())
		Movement.TryUpdateLocomotion(OriginalTickTime, true, Movement.bPreviousHasMoveInput, Movement.ResolvedGait);
}

int32 FGGYGOActionMotionExecution::BeginActionMotion(const UGGYGOActionMotionProfile* Profile, float PlayRate)
{
	const FString ErrorContext = FString::Printf(TEXT("[Movement.ActionMotion] CMC='%s' Profile='%s'"),
		*Movement.GetPathName(), *GetPathNameSafe(Profile));
	const auto Reject = [&ErrorContext](const FString& Reason)
	{
		UE_LOG(LogGGYGOActionMotion, Error, TEXT("%s Reason='%s'"), *ErrorContext, *Reason);
		return INDEX_NONE;
	};
	if (!IsInGameThread() || !IsValid(Movement.CharacterOwner) || Movement.IsBeingDestroyed())
		return Reject(TEXT("a live original authority owner and game thread are required"));
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> WeakSelf(&Movement);
	const TWeakObjectPtr<ACharacter> OriginalCharacter(Movement.CharacterOwner);
	const auto OriginalOwnerContext = Movement.MovementOwnerSyncContext;
	const int32 OriginalNextHandle = NextActionMotionHandle;
	CleanupFinishedActionMotion();
	UGGYGOCharacterMovementComponent* Self = WeakSelf.Get();
	if (!Self || Self->IsBeingDestroyed() || !OriginalCharacter.IsValid()
		|| Self->CharacterOwner != OriginalCharacter.Get() || Self->MovementOwnerSyncContext != OriginalOwnerContext
		|| Self->ActionMotionExecution.NextActionMotionHandle != OriginalNextHandle)
		return Reject(TEXT("original owner/action slot changed during prior action retirement"));
	USkeletalMeshComponent* Mesh = Movement.CharacterOwner->GetMesh();
	if (!Movement.CharacterOwner->HasAuthority() || !IsValid(Mesh) || !Movement.IsMovingOnGround()
		|| Movement.CharacterOwner->bClientUpdating || Movement.CharacterOwner->IsPlayingRootMotion())
		return Reject(TEXT("original authority ground owner and Mesh are required, without native animation root motion or replay"));
	if (HasActiveActionMotion() || NextActionMotionHandle <= 0 || NextActionMotionHandle == MAX_int32)
		return Reject(TEXT("action execution slot is occupied or original token allocator is exhausted"));
	FString Error;
	if (!IsValid(Profile) || !Profile->ValidateMotion(Error))
		return Reject(Error.IsEmpty() ? TEXT("original Profile is missing") : Error);
	if (!FMath::IsFinite(PlayRate) || PlayRate <= UE_SMALL_NUMBER
		|| !FMath::IsFinite(Profile->Duration / PlayRate) || Profile->Duration / PlayRate <= 0.0f)
		return Reject(TEXT("playback rate and native duration must be finite and positive"));
	const FQuat Rotation = Mesh->GetComponentQuat();
	const FVector Scale = Profile->TranslationScale * Mesh->GetComponentScale();
	if (Rotation.ContainsNaN() || !Rotation.IsNormalized() || Scale.ContainsNaN())
		return Reject(TEXT("original Mesh basis or scaled translation is not finite and normalized"));

	const auto Resource = MakeShared<FGGYGOActionMotionResource>();
	Resource->Owner = &Movement;
	Resource->Character = Movement.CharacterOwner;
	Resource->Mesh = Mesh;
	Resource->Profile = TStrongObjectPtr<const UGGYGOActionMotionProfile>(Profile);
	Resource->Handle = NextActionMotionHandle;
	const auto Source = MakeShared<FRootMotionSource_GGYGOActionCurve>();
	Source->SourceMode = EGGYGOActionCurveSourceMode::Profile;
	Source->OriginalResource = Resource;
	Source->InstanceName = SourceName();
	Source->Priority = SourcePriority;
	Source->TranslationCurve = Profile->TranslationCurve;
	Source->Duration = Profile->Duration / PlayRate;
	Source->PlayRate = PlayRate;
	Source->EntryMeshRotation = Rotation;
	Source->TranslationScale = Scale;
	const auto RetiredObservers = RetireCompletedResource();
	const uint16 Id = Movement.ApplyRootMotionSource(Source);
	if (Id == static_cast<uint16>(ERootMotionSourceID::Invalid))
		return Reject(TEXT("native ApplyRootMotionSource rejected the original Profile source"));
	ActiveProfileActionResource = Resource;
	ActiveActionMotionHandle = Resource->Handle;
	ActionMotionSourceID = Id;
	++NextActionMotionHandle;
	Movement.SuspendLocomotionForAction();
	return Resource->Handle;
}

bool FGGYGOActionMotionExecution::HasActiveActionMotion() const
{
	if (ActiveActionMotionHandle == INDEX_NONE) return false;
	const auto MatchesActive = [this](const TSharedPtr<FRootMotionSource>& Source)
	{
		return Source.IsValid() && Source->LocalID == ActionMotionSourceID
			&& Source->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct()
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::Finished)
			&& !Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval);
	};
	return Movement.CurrentRootMotion.RootMotionSources.ContainsByPredicate(MatchesActive)
		|| Movement.CurrentRootMotion.PendingAddRootMotionSources.ContainsByPredicate(MatchesActive);
}

void FGGYGOActionMotionExecution::EndActionMotion(int32 Handle)
{
	if (Handle == INDEX_NONE || Handle != ActiveActionMotionHandle) return;
	if (ActiveMontageActionResource.IsValid() && ActiveMontageActionResource->Handle == Handle)
	{
		FString Error;
		ReleaseMontageActionMotion(Handle, EGGYGOActionMotionReleaseReason::Cancelled, Error);
		return;
	}
	const auto OriginalResource = ActiveProfileActionResource;
	ActiveProfileActionResource.Reset();
	FGGYGOActionMotionFailureDelegate RetiredCallback;
	if (ActionMotionFailureResource == OriginalResource)
	{
		RetiredCallback = MoveTemp(ActionMotionFailureCallback);
		ActionMotionFailureResource.Reset();
	}
	const TSharedPtr<FRootMotionSource> Source = Movement.GetRootMotionSourceByID(ActionMotionSourceID);
	if (Source.IsValid() && Source->GetScriptStruct() == FRootMotionSource_GGYGOActionCurve::StaticStruct())
	{
		Movement.RemoveRootMotionSourceByID(ActionMotionSourceID);
	}
	ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
	ActiveActionMotionHandle = INDEX_NONE;
	Movement.Velocity.X = Movement.Velocity.Y = 0.;
	Movement.NativeMovementVelocityResult.Reset();
	Movement.NativeVelocityBeforeRootMotion.Reset();
	Movement.bNativeVelocityIntervalCanRetain = false;
	Movement.CurveMotion.Reset();
	Movement.WalkHoldTimer = 0.f;
	Movement.bPreviousHasMoveInput = false;
}

void FGGYGOActionMotionExecution::CleanupFinishedActionMotion()
{
	if (ActiveMontageActionResource.IsValid())
	{
		const auto Resource = ActiveMontageActionResource;
		if (!IsMontageActionMovementModeSupported())
		{
			const FString Error = FString::Printf(
				TEXT("[Movement.ActionMotion] CMC='%s' Montage='%s' token=%d: original XYZ action entered unsupported MovementMode=%d CustomMode=%d"),
				*Movement.GetPathName(), *GetPathNameSafe(Resource->Source->Montage.Get()), Resource->Handle,
				static_cast<int32>(Movement.MovementMode), static_cast<int32>(Movement.CustomMovementMode));
			UE_LOG(LogGGYGOActionMotion, Error, TEXT("%s"), *Error);
			FailMontageActionMotion(Resource, Error);
		}
		else if (!HasActiveActionMotion())
		{
			// Natural expiry does not zero/retire an original Prepared final contribution.
			CompletedMontageActionResource = Resource;
			ActiveMontageActionResource.Reset();
			ActiveActionMotionHandle = INDEX_NONE;
			ActionMotionSourceID = static_cast<uint16>(ERootMotionSourceID::Invalid);
		}
		return;
	}
	if (ActiveActionMotionHandle != INDEX_NONE && (!HasActiveActionMotion() || !Movement.IsMovingOnGround()))
	{
		EndActionMotion(ActiveActionMotionHandle);
	}
}
