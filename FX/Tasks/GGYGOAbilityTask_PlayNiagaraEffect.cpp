/** @file GGYGOAbilityTask_PlayNiagaraEffect.cpp @brief 独立 Niagara 单元组合的 GAS 生命周期与原生尾部交接。 */
#include "FX/Tasks/GGYGOAbilityTask_PlayNiagaraEffect.h"

#include "FX/GGYGOMontageFXPublication.h"
#include "FX/Components/GGYGOFXNiagaraComponent.h"
#include "FX/Components/GGYGOMontageFXReplicationComponent.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/App.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOFXTask, Log, All);

namespace
{
bool HasPendingNiagaraCompilation([[maybe_unused]] const UNiagaraSystem* System)
{
#if WITH_EDITORONLY_DATA
	return System->HasOutstandingCompilationRequests(true);
#else
	return false;
#endif
}

bool DoSystemScriptAttributesMatch(const UNiagaraSystem* System)
{
	const UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
	const UNiagaraScript* UpdateScript = System->GetSystemUpdateScript();
	// 原生 System 数据集必须拥有相同顺序及类型；这项结构约束与 PSO 暂态无关。
	return SpawnScript && UpdateScript
		&& SpawnScript->GetVMExecutableData().Attributes == UpdateScript->GetVMExecutableData().Attributes;
}

FString DescribeNiagaraScripts(const UNiagaraSystem* System)
{
	const auto DescribeScript = [](const UNiagaraScript* Script, bool bGPUScript = false)
	{
		return Script ? FString::Printf(TEXT("%s[Pending=%d,Succeeded=%d]"), *Script->GetPathName(),
			Script->IsScriptCompilationPending(bGPUScript), Script->DidScriptCompilationSucceed(bGPUScript)) : FString(TEXT("<missing>"));
	};
	const UNiagaraScript* SpawnScript = System->GetSystemSpawnScript();
	const UNiagaraScript* UpdateScript = System->GetSystemUpdateScript();
	FString Description = FString::Printf(TEXT("Spawn=%s Update=%s Emitters=%d CompiledEmitters=%d CompiledCountMatches=%d"),
		*DescribeScript(SpawnScript), *DescribeScript(UpdateScript), System->GetEmitterHandles().Num(),
		System->GetEmitterCompiledData().Num(), System->GetEmitterHandles().Num() == System->GetEmitterCompiledData().Num());
	// 只记录原生数据，不据此另建播放准入；数组顺序及类型均保留在真实相等比较中。
	if (SpawnScript && UpdateScript)
	{
		const TArray<FNiagaraVariableBase>& SpawnAttributes = SpawnScript->GetVMExecutableData().Attributes;
		const TArray<FNiagaraVariableBase>& UpdateAttributes = UpdateScript->GetVMExecutableData().Attributes;
		const bool bAttributesEqual = SpawnAttributes == UpdateAttributes;
		Description += FString::Printf(TEXT(" VMAttributesEqual=%d SpawnAttributeCount=%d UpdateAttributeCount=%d"),
			bAttributesEqual, SpawnAttributes.Num(), UpdateAttributes.Num());
		if (!bAttributesEqual)
		{
			int32 DifferenceIndex = 0;
			while (SpawnAttributes.IsValidIndex(DifferenceIndex) && UpdateAttributes.IsValidIndex(DifferenceIndex)
				&& SpawnAttributes[DifferenceIndex] == UpdateAttributes[DifferenceIndex]) { ++DifferenceIndex; }
			const auto DescribeAttribute = [](const TArray<FNiagaraVariableBase>& Attributes, int32 Index)
			{
				return Attributes.IsValidIndex(Index) ? FString::Printf(TEXT("%s:%s"),
					*Attributes[Index].GetName().ToString(), *Attributes[Index].GetType().GetName()) : FString(TEXT("<missing>"));
			};
			Description += FString::Printf(TEXT(" FirstAttributeDifference=%d SpawnAttribute=%s UpdateAttribute=%s"),
				DifferenceIndex, *DescribeAttribute(SpawnAttributes, DifferenceIndex), *DescribeAttribute(UpdateAttributes, DifferenceIndex));
		}
	}
	else
	{
		Description += TEXT(" VMAttributes=<missing_system_script>");
	}
	// 原生 Ready 检查没有跳过 disabled handle；诊断也保留全部 handle。
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		Description += FString::Printf(TEXT(" Emitter=%s[Enabled=%d"), *Handle.GetName().ToString(), Handle.GetIsEnabled());
		if (!EmitterData)
		{
			Description += TEXT(",Data=<none>,Ready=<native_skips_absent_data>]");
			continue;
		}
		const bool bEmitterReady = EmitterData->IsReadyToRun();
		const bool bEmitterValid = EmitterData->IsValid();
		Description += FString::Printf(TEXT(",Ready=%d,Valid=%d,SimTarget=%d,Events=%d]"),
			bEmitterReady, bEmitterValid, static_cast<int32>(EmitterData->SimTarget), EmitterData->GetEventHandlers().Num());
		if (!bEmitterReady)
		{
			Description += FString::Printf(TEXT(" ParticleSpawn=%s ParticleUpdate=%s"),
				*DescribeScript(EmitterData->SpawnScriptProps.Script), *DescribeScript(EmitterData->UpdateScriptProps.Script));
			if (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim)
			{
				Description += FString::Printf(TEXT(" GPUCompute=%s"), *DescribeScript(EmitterData->GetGPUComputeScript(), true));
			}
			int32 EventIndex = 0;
			for (const FNiagaraEventScriptProperties& Event : EmitterData->GetEventHandlers())
			{
				Description += FString::Printf(TEXT(" Event[%d]=%s"), EventIndex++, *DescribeScript(Event.Script));
			}
		}
		if (Handle.GetIsEnabled() && !bEmitterValid && EmitterData->IsAllowedByScalability())
		{
			Description += FString::Printf(TEXT(" InvalidEmitter=%s"), *Handle.GetName().ToString());
		}
	}
	return Description;
}
}

UGGYGOAbilityTask_PlayNiagaraEffect* UGGYGOAbilityTask_PlayNiagaraEffect::PlayNiagaraEffect(
	UGameplayAbility* OwningAbility, FName TaskInstanceName, UNiagaraSystem* System,
	EGGYGONiagaraEffectSpace Space, USceneComponent* AttachComponent, FName SocketName,
	const FTransform& Transform, const FGGYGOFXFieldSettings& FieldSettings)
{
	FGGYGONiagaraEffectUnit Unit;
	Unit.System = System;
	Unit.FieldSettings = FieldSettings;
	return PlayNiagaraEffects(OwningAbility, TaskInstanceName, {Unit}, Space, AttachComponent, SocketName, Transform);
}

UGGYGOAbilityTask_PlayNiagaraEffect* UGGYGOAbilityTask_PlayNiagaraEffect::PlayNiagaraEffects(
	UGameplayAbility* OwningAbility, FName TaskInstanceName, const TArray<FGGYGONiagaraEffectUnit>& Units,
	EGGYGONiagaraEffectSpace Space, USceneComponent* AttachComponent, FName SocketName, const FTransform& Transform)
{
	if (!IsValid(OwningAbility) || !OwningAbility->IsActive())
	{
		UE_LOG(LogGGYGOFXTask, Error, TEXT("[FX.Task] Ability='%s' Units=%d: an active owning Ability is required."),
			*GetPathNameSafe(OwningAbility), Units.Num());
		return nullptr;
	}
	UAbilitySystemComponent* const Issuer = OwningAbility->GetAbilitySystemComponentFromActorInfo();
	AActor* const Avatar = OwningAbility->GetAvatarActorFromActorInfo();
	const UGGYGOMontageFXReplicationComponent* Host = UGGYGOMontageFXReplicationComponent::FindForASC(Issuer);
	UGGYGOMontageFXPublicationOwner* const ResourceScope = Host ? Host->GetPublicationOwner() : nullptr;
	if (!IsValid(ResourceScope) || !IsValid(Avatar) || !IsValid(Avatar->GetWorld()))
	{
		UE_LOG(LogGGYGOFXTask, Error, TEXT("FX factory requires its initialized original resource scope: ASC='%s' Host='%s'."),
			*GetPathNameSafe(Issuer), *GetPathNameSafe(Host));
		return nullptr;
	}
	UGGYGOAbilityTask_PlayNiagaraEffect* Task = NewAbilityTask<UGGYGOAbilityTask_PlayNiagaraEffect>(OwningAbility, TaskInstanceName);
	if (!IsValid(Task) || Task->IsFinished()) { return nullptr; }
	if (!IsValid(OwningAbility) || !OwningAbility->IsActive()
		|| OwningAbility->GetAbilitySystemComponentFromActorInfo() != Issuer
		|| OwningAbility->GetAvatarActorFromActorInfo() != Avatar || !IsValid(ResourceScope))
	{ Task->EndTask(); return nullptr; }
	Task->ResourceOwner = ResourceScope;
	Task->OriginalIssuer = Issuer;
	Task->OriginalAvatar = Avatar;
	Task->OriginalWorld = Avatar->GetWorld();
	Task->RequestedUnits = Units;
	Task->RequestedSpace = Space;
	Task->RequestedAttachComponent = AttachComponent;
	Task->RequestedSocketName = SocketName;
	Task->RequestedTransform = Transform;
	FString RegistrationError;
	Task->LocalCapability = ResourceScope->RegisterLocalTask(Task, Avatar, Avatar->GetWorld(), RegistrationError);
	if (!Task->LocalCapability.IsValid())
	{
		Task->FailAndEndTask(RegistrationError);
		return nullptr;
	}
	if (Space == EGGYGONiagaraEffectSpace::Attached)
	{ Avatar->OnEndPlay.AddUniqueDynamic(Task, &ThisClass::HandleOriginalAvatarEndPlay); }
	return Task;
}

bool UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfiguration(UNiagaraSystem* System,
	EGGYGONiagaraEffectSpace Space, const FTransform& Transform, FString& OutFailureReason)
{
	OutFailureReason.Reset();
	if (!IsValid(System))
	{
		OutFailureReason = TEXT("Required Niagara System is missing or invalid.");
		return false;
	}
	if (Space != EGGYGONiagaraEffectSpace::World && Space != EGGYGONiagaraEffectSpace::Attached)
	{
		OutFailureReason = TEXT("Effect space must explicitly select World or Attached.");
		return false;
	}
	const FVector Scale = Transform.GetScale3D();
	if (!Transform.IsValid() || FMath::IsNearlyZero(Scale.X) || FMath::IsNearlyZero(Scale.Y) || FMath::IsNearlyZero(Scale.Z))
	{
		OutFailureReason = TEXT("Effect transform requires finite values, normalized rotation and nonzero scale on every axis.");
		return false;
	}
	return true;
}

bool UGGYGOAbilityTask_PlayNiagaraEffect::IsStopModeValid(EGGYGONiagaraEffectStopMode Mode)
{
	return Mode == EGGYGONiagaraEffectStopMode::DestroyImmediately
		|| Mode == EGGYGONiagaraEffectStopMode::DeactivateKeepAttached
		|| Mode == EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld;
}

bool UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfigurations(const TArray<FGGYGONiagaraEffectUnit>& Units,
	EGGYGONiagaraEffectSpace Space, const FTransform& Transform, FString& OutFailureReason)
{
	OutFailureReason.Reset();
	if (Units.IsEmpty())
	{
		OutFailureReason = TEXT("Niagara composition requires at least one explicit System unit.");
		return false;
	}
	for (int32 Index = 0; Index < Units.Num(); ++Index)
	{
		const FGGYGONiagaraEffectUnit& Unit = Units[Index];
		if (!ValidateConfiguration(Unit.System, Space, Transform, OutFailureReason)
			|| !Unit.FieldSettings.ValidateConfiguration(OutFailureReason))
		{
			OutFailureReason = FString::Printf(TEXT("Unit[%d] '%s': %s"), Index,
				*GetPathNameSafe(Unit.System), *OutFailureReason);
			return false;
		}
	}
	return true;
}

bool UGGYGOAbilityTask_PlayNiagaraEffect::ValidateCompiledSystem(UNiagaraSystem* System, FString& OutError)
{
	if (!IsValid(System)) { OutError = TEXT("Required Niagara System is invalid."); return false; }
	if (!HasPendingNiagaraCompilation(System) && (!System->IsValid() || !DoSystemScriptAttributesMatch(System)))
	{
		OutError = FString::Printf(TEXT("Invalid Niagara compilation or System VM attributes: %s"), *DescribeNiagaraScripts(System));
		return false;
	}
	return true;
}

EGGYGOMontageFXLocalStartResult UGGYGOAbilityTask_PlayNiagaraEffect::RequestMontageFXLocalStart(
	UAbilitySystemComponent* Issuer, AActor* Avatar, USkeletalMeshComponent* Mesh, UWorld* World,
	FGGYGOMontageFXLocalStartTicket& OutTicket, FString& OutError)
{
	OutTicket = {};
	if (IsFinished() || !ResourceOwner || !LocalCapability.IsValid()) { return EGGYGOMontageFXLocalStartResult::Cancelled; }
	if (Issuer != OriginalIssuer.Get() || Avatar != OriginalAvatar.Get() || World != OriginalWorld.Get()
		|| !IsValid(Mesh) || Mesh->GetOwner() != Avatar || Mesh->GetWorld() != World
		|| StartResult != EGGYGONiagaraEffectStartResult::NotRequested || LocalStartTicket.IsValid())
	{
		OutError = TEXT("Local FX request must bind the original factory Task/source once, before activation.");
		return EGGYGOMontageFXLocalStartResult::Rejected;
	}
	LocalStartTicket = MakeShared<FGGYGOMontageFXLocalStartTicket>();
	const auto TicketStorage = LocalStartTicket;
	const auto Capability = LocalCapability;
	const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Scope(ResourceOwner);
	const TWeakObjectPtr<ThisClass> Self(this);
	const auto Result = Scope->RequestLocalStart(Capability, *TicketStorage, OutError);
	OutTicket = *TicketStorage;
	if (!Self.IsValid() || Self->IsFinished()) { return EGGYGOMontageFXLocalStartResult::Cancelled; }
	return Result;
}

bool UGGYGOAbilityTask_PlayNiagaraEffect::ObserveMontageFXLocalStartReady(
	const FGGYGOMontageFXLocalStartTicket& Ticket, FGGYGOMontageFXLocalStartReadyDelegate Callback)
{
	return ResourceOwner && LocalStartTicket.IsValid() && *LocalStartTicket == Ticket
		&& ResourceOwner->ObserveLocalStart(Ticket, MoveTemp(Callback));
}

EGGYGOMontageFXPermitResult UGGYGOAbilityTask_PlayNiagaraEffect::TryConsumeMontageFXLocalStartPermit(
	const FGGYGOMontageFXLocalStartTicket& Ticket)
{
	return ResourceOwner && LocalStartTicket.IsValid() && *LocalStartTicket == Ticket
		? ResourceOwner->ConsumeLocalStart(Ticket) : EGGYGOMontageFXPermitResult::Rejected;
}

void UGGYGOAbilityTask_PlayNiagaraEffect::CancelMontageFXLocalStartRequest(const FGGYGOMontageFXLocalStartTicket& Ticket)
{
	if (ResourceOwner && LocalStartTicket.IsValid() && *LocalStartTicket == Ticket)
	{ ResourceOwner->CancelLocalStart(Ticket); }
}

void UGGYGOAbilityTask_PlayNiagaraEffect::ResumeLegacyStart(const FGGYGOMontageFXLocalStartReadyFact& Fact)
{
	if (Fact.Task.Get() != this || IsFinished()) { return; }
	if (TryConsumeMontageFXLocalStartPermit(Fact.Ticket) == EGGYGOMontageFXPermitResult::Granted) { Activate(); }
}

void UGGYGOAbilityTask_PlayNiagaraEffect::Activate()
{
	if (IsFinished() || StartResult != EGGYGONiagaraEffectStartResult::NotRequested) { return; }
	if (!ResourceOwner || !LocalCapability.IsValid())
	{ FailAndEndTask(TEXT("Native FX creation requires its factory-issued resource capability.")); return; }
	// 保留原公开 BP 工厂调用：未显式请求的调用也经过同一真实准入门。
	if (!LocalStartTicket.IsValid())
	{
		LocalStartTicket = MakeShared<FGGYGOMontageFXLocalStartTicket>();
		const auto TicketStorage = LocalStartTicket;
		const auto Capability = LocalCapability;
		const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Scope(ResourceOwner);
		const TWeakObjectPtr<ThisClass> Self(this);
		FString Error;
		const auto Result = Scope->RequestLocalStart(Capability, *TicketStorage, Error);
		if (!Self.IsValid() || Self->IsFinished()) { return; }
		if (Result == EGGYGOMontageFXLocalStartResult::Rejected || Result == EGGYGOMontageFXLocalStartResult::Cancelled)
		{ FailAndEndTask(Error); return; }
		if (Result == EGGYGOMontageFXLocalStartResult::WaitingForRemoteRelease)
		{
			ResourceOwner->ObserveLocalStart(*LocalStartTicket,
				FGGYGOMontageFXLocalStartReadyDelegate::CreateUObject(this, &ThisClass::ResumeLegacyStart));
			return;
		}
		if (ResourceOwner->ConsumeLocalStart(*LocalStartTicket) != EGGYGOMontageFXPermitResult::Granted)
		{ FailAndEndTask(TEXT("Original local FX start permission could not be consumed.")); return; }
	}
	if (!ResourceOwner->HasLocalPermit(*LocalStartTicket))
	{ FailAndEndTask(TEXT("Local FX native activation has no current consumed permission.")); return; }
	const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Scope(ResourceOwner);
	const auto Capability = LocalCapability;
	Scope->BeginLocalOperation(Capability);
	ActivateNative();
	Scope->EndLocalOperation(Capability);
	Scope->FlushResourceChanges();
}

void UGGYGOAbilityTask_PlayNiagaraEffect::ActivateNative()
{
	if (IsFinished() || StartResult != EGGYGONiagaraEffectStartResult::NotRequested) { return; }
	FString FailureReason;
	if (!ValidateConfigurations(RequestedUnits, RequestedSpace, RequestedTransform, FailureReason))
	{
		FailAndEndTask(FailureReason);
		return;
	}
	AActor* const Avatar = OriginalAvatar.Get();
	UWorld* const World = OriginalWorld.Get();
	if (!IsValid(Ability) || !Ability->IsActive() || !IsValid(World) || World->bIsTearingDown)
	{
		FailAndEndTask(TEXT("Original active Ability, Avatar and live World are required."));
		return;
	}
	USceneComponent* const AttachComponent = RequestedAttachComponent.Get();
	if (RequestedSpace == EGGYGONiagaraEffectSpace::Attached)
	{
		if (!IsValid(AttachComponent) || !AttachComponent->IsRegistered() || AttachComponent->GetWorld() != World
			|| (!RequestedSocketName.IsNone() && !AttachComponent->DoesSocketExist(RequestedSocketName)))
		{
			FailAndEndTask(FString::Printf(TEXT("Required attachment [%s:%s] must be registered in the original Avatar World with its configured Socket."),
				*GetPathNameSafe(AttachComponent), *RequestedSocketName.ToString()));
			return;
		}
	}
	else if (!RequestedAttachComponent.IsExplicitlyNull() || !RequestedSocketName.IsNone())
	{
		FailAndEndTask(TEXT("World space requires no attachment component or Socket; supply the resolved world transform."));
		return;
	}
	if (World->GetNetMode() == NM_DedicatedServer)
	{
		StartResult = EGGYGONiagaraEffectStartResult::DedicatedServerNoPresentation;
		EndTask();
		return;
	}
	if (!FApp::CanEverRender())
	{
		FailAndEndTask(TEXT("Rendering is unavailable in this non-dedicated World."));
		return;
	}
	const TWeakObjectPtr<ThisClass> OriginalTask(this);
	const TWeakObjectPtr<AActor> ActivationAvatar(Avatar);
	const TWeakObjectPtr<UWorld> ActivationWorld(World);
	// 全部单元先准备并注册，再按原顺序激活；同一资源门只登记一个 cap/ticket。
	TArray<TStrongObjectPtr<UGGYGOFXNiagaraComponent>> CreatedComponents;
	CreatedComponents.Reserve(RequestedUnits.Num());
	AActor* const NativeOwner = RequestedSpace == EGGYGONiagaraEffectSpace::World
		? static_cast<AActor*>(World->GetWorldSettings()) : AttachComponent->GetOwner();
	UObject* const Outer = NativeOwner ? static_cast<UObject*>(NativeOwner) : static_cast<UObject*>(World);
	for (const FGGYGONiagaraEffectUnit& Unit : RequestedUnits)
	{
		UNiagaraSystem* const System = Unit.System;
		// Ready 会完成原生加载，并包含编译与 PSO 的合法暂态；无效编译结果须独立拒绝。
		const bool bSystemReady = System->IsReadyToRun();
		const bool bSystemValid = System->IsValid();
		const bool bCompilationPending = HasPendingNiagaraCompilation(System);
		ThisClass* LiveTask = OriginalTask.Get(true);
		if (!LiveTask || LiveTask->IsFinished() || LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return; }
		if (!bSystemReady || !bSystemValid)
		{
			UE_LOG(LogGGYGOFXTask, Display, TEXT("[FX.Task.Readiness] Task='%s' System='%s' Ready=%d Valid=%d OutstandingCompilation=%d %s"),
				*LiveTask->GetPathName(), *System->GetPathName(), bSystemReady, bSystemValid, bCompilationPending,
				*DescribeNiagaraScripts(System));
		}
		if (!bCompilationPending && (!bSystemValid || !DoSystemScriptAttributesMatch(System)))
		{
			LiveTask->FailAndEndTask(FString::Printf(TEXT("Required Niagara System has invalid scripts, enabled emitters or mismatched System VM attributes: %s"),
				*DescribeNiagaraScripts(System)));
			return;
		}
#if WITH_EDITOR
		// 原生组件负责等待与激活；这里只观察真实编译结果，避免最终失败变成无限等待。
		if (!LiveTask->SystemCompiledHandles.Contains(System))
		{
			LiveTask->SystemCompiledHandles.Add(System, System->OnSystemCompiled().AddUObject(LiveTask, &ThisClass::HandleSystemCompiled));
		}
#endif
		CreatedComponents.Emplace(NewObject<UGGYGOFXNiagaraComponent>(Outer, NAME_None, RF_Transient));
		UGGYGOFXNiagaraComponent* const CreatedComponent = CreatedComponents.Last().Get();
		LiveTask = OriginalTask.Get(true);
		if (!LiveTask || LiveTask->IsFinished() || LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			if (IsValid(CreatedComponent)) { CreatedComponent->DestroyComponent(); }
			return;
		}
		if (!IsValid(CreatedComponent))
		{
			LiveTask->FailAndEndTask(TEXT("Native Niagara spawn returned no component."));
			return;
		}
		LiveTask->OwnedComponents.Add(CreatedComponent);
		if (LiveTask->ResourceOwner) { LiveTask->ResourceOwner->SetLocalOwnership(LiveTask->LocalCapability, true); }
		CreatedComponent->OnSystemFinished.AddDynamic(LiveTask, &ThisClass::HandleSystemFinished);
		CreatedComponent->OnEffectFailed.AddUObject(LiveTask, &ThisClass::HandleEffectFailed);
		if (!CreatedComponent->ConfigureField(Unit.FieldSettings, FailureReason))
		{ LiveTask->FailAndEndTask(FailureReason); return; }
		if (!CreatedComponent->ConfigureSceneLightingSource(ActivationAvatar.Get(), FailureReason))
		{ LiveTask->FailAndEndTask(FailureReason); return; }
		CreatedComponent->SetAutoDestroy(false);
		CreatedComponent->bAllowAnyoneToDestroyMe = true;
		CreatedComponent->SetVisibleInRayTracing(false);
#if WITH_EDITORONLY_DATA
		CreatedComponent->bWaitForCompilationOnActivate = GIsAutomationTesting;
#endif
		CreatedComponent->SetAsset(System);
		LiveTask = OriginalTask.Get(true);
		if (!LiveTask || LiveTask->IsFinished() || !LiveTask->OwnedComponents.Contains(CreatedComponent)) { return; }
		if (!IsValid(CreatedComponent) || !ActivationWorld.IsValid() || World->bIsTearingDown
			|| !ActivationAvatar.IsValid() || !IsValid(LiveTask->Ability) || !LiveTask->Ability->IsActive()
			|| LiveTask->Ability->GetAvatarActorFromActorInfo() != ActivationAvatar.Get())
		{ LiveTask->FailAndEndTask(TEXT("设置 Niagara 资产时原 FX 作用域已变化。")); return; }
		if (LiveTask->RequestedSpace == EGGYGONiagaraEffectSpace::World)
		{
			CreatedComponent->SetAbsolute(true, true, true);
			CreatedComponent->SetWorldTransform(LiveTask->RequestedTransform);
		}
		else
		{
			CreatedComponent->SetupAttachment(AttachComponent, LiveTask->RequestedSocketName);
			CreatedComponent->SetRelativeTransform(LiveTask->RequestedTransform);
		}
		CreatedComponent->RegisterComponentWithWorld(World);
		LiveTask = OriginalTask.Get(true);
		if (!LiveTask || LiveTask->IsFinished() || !LiveTask->OwnedComponents.Contains(CreatedComponent)) { return; }
		if (!IsValid(LiveTask->Ability) || !LiveTask->Ability->IsActive()
			|| !ActivationAvatar.IsValid() || LiveTask->Ability->GetAvatarActorFromActorInfo() != ActivationAvatar.Get()
			|| !ActivationWorld.IsValid() || ActivationWorld->bIsTearingDown
			|| !CreatedComponent->IsRegistered() || CreatedComponent->GetWorld() != ActivationWorld.Get())
		{
			LiveTask->FailAndEndTask(TEXT("Original Ability/Avatar or component registration changed during native spawn."));
			return;
		}
	}

	// 强持原组件快照；同步完成可摘除 OwnedComponents，不会使后续单元迭代失效。
	for (const TStrongObjectPtr<UGGYGOFXNiagaraComponent>& OriginalComponent : CreatedComponents)
	{
		ThisClass* LiveTask = OriginalTask.Get(true);
		if (!LiveTask || LiveTask->IsFinished() || LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return; }
		UGGYGOFXNiagaraComponent* const CreatedComponent = OriginalComponent.Get();
		if (!LiveTask->OwnedComponents.Contains(CreatedComponent)) { continue; }
		if (!IsValid(CreatedComponent) || !ActivationWorld.IsValid() || ActivationWorld->bIsTearingDown
			|| !ActivationAvatar.IsValid() || !IsValid(LiveTask->Ability) || !LiveTask->Ability->IsActive()
			|| LiveTask->Ability->GetAvatarActorFromActorInfo() != ActivationAvatar.Get())
		{ LiveTask->FailAndEndTask(TEXT("激活组合单元前原 FX 作用域或组件已失效。")); return; }
		// 合法编译／PSO 等待仍是请求已发出；不以 IsActive 或首粒子反推启动事实。
		LiveTask->StartResult = EGGYGONiagaraEffectStartResult::SpawnAndActivationRequested;
		CreatedComponent->Activate(true);
		LiveTask = OriginalTask.Get(true);
		if (!LiveTask || LiveTask->IsFinished() || LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { return; }
		if (!LiveTask->OwnedComponents.Contains(CreatedComponent)) { continue; }
		const FString RuntimeFailure = CreatedComponent->GetEffectFailure();
		if (!RuntimeFailure.IsEmpty()) { LiveTask->FailAndEndTask(RuntimeFailure); return; }
		UNiagaraSystem* const System = CreatedComponent->GetAsset();
		// Activate 可同步收束编译；失败必须沿原 Task 传播，不能发布 OnSpawned。
		if (!ValidateCompiledSystem(System, FailureReason))
		{ LiveTask->FailAndEndTask(FailureReason); return; }
		if (LiveTask->ShouldBroadcastAbilityTaskDelegates())
		{ LiveTask->OnSpawned.Broadcast(LiveTask, CreatedComponent); }
	}
}

void UGGYGOAbilityTask_PlayNiagaraEffect::FailAndEndTask(const FString& Reason)
{
	if (IsFinished()) { return; }
	if (StartResult == EGGYGONiagaraEffectStartResult::NotRequested)
	{
		StartResult = EGGYGONiagaraEffectStartResult::Failed;
	}
	UE_LOG(LogGGYGOFXTask, Error, TEXT("[FX.Task] Task='%s' Ability='%s' Avatar='%s' Units=%d: %s"),
		*GetPathName(), *GetPathNameSafe(Ability),
		*GetPathNameSafe(IsValid(Ability) ? Ability->GetAvatarActorFromActorInfo() : nullptr),
		RequestedUnits.Num(), *Reason);
	const TWeakObjectPtr<ThisClass> OriginalTask(this);
	if (ShouldBroadcastAbilityTaskDelegates()) { OnFailed.Broadcast(this, Reason); }
	if (ThisClass* LiveTask = OriginalTask.Get(true))
	{
		if (!LiveTask->IsFinished() && !LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { LiveTask->EndTask(); }
	}
}

TArray<TStrongObjectPtr<UNiagaraComponent>> UGGYGOAbilityTask_PlayNiagaraEffect::ReleaseOwnedComponents()
{
	TArray<TStrongObjectPtr<UNiagaraComponent>> Originals;
	Originals.Reserve(OwnedComponents.Num());
	for (UNiagaraComponent* Component : OwnedComponents) { Originals.Emplace(Component); }
	OwnedComponents.Reset();
	for (const TStrongObjectPtr<UNiagaraComponent>& Component : Originals) { UnbindOwnedComponent(Component.Get()); }
	if (ResourceOwner) { ResourceOwner->SetLocalOwnership(LocalCapability, false); }
	return Originals;
}

void UGGYGOAbilityTask_PlayNiagaraEffect::UnbindOwnedComponent(UNiagaraComponent* Component)
{
	if (IsValid(Component)) { Component->OnSystemFinished.RemoveDynamic(this, &ThisClass::HandleSystemFinished); }
	if (UGGYGOFXNiagaraComponent* EffectComponent = Cast<UGGYGOFXNiagaraComponent>(Component))
	{ EffectComponent->OnEffectFailed.RemoveAll(this); }
}

void UGGYGOAbilityTask_PlayNiagaraEffect::HandleEffectFailed(
	UGGYGOFXNiagaraComponent* Component, const FString& Reason)
{
	if (OwnedComponents.Contains(Component) && !IsFinished()) { FailAndEndTask(Reason); }
}

void UGGYGOAbilityTask_PlayNiagaraEffect::StopEffect(EGGYGONiagaraEffectStopMode Mode)
{
	const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Scope(ResourceOwner);
	const auto Capability = LocalCapability;
	if (Scope.IsValid()) { Scope->BeginLocalOperation(Capability); }
	if (Scope.IsValid() && LocalStartTicket.IsValid()) { Scope->CancelLocalStart(*LocalStartTicket); }
	StopNative(Mode);
	if (Scope.IsValid()) { Scope->EndLocalOperation(Capability); Scope->FlushResourceChanges(); }
}

bool UGGYGOAbilityTask_PlayNiagaraEffect::StopComponent(UNiagaraComponent* Component,
	EGGYGONiagaraEffectStopMode Mode, TFunctionRef<bool()> IsOriginalOwnerAlive)
{
	if (!IsStopModeValid(Mode)) { return false; }
	TStrongObjectPtr<UNiagaraComponent> Original(Component);
	if (!Original.IsValid()) { return true; }
	if (Mode == EGGYGONiagaraEffectStopMode::DestroyImmediately) { Original->DestroyComponent(); return true; }
	Original->SetAutoDestroy(true);
	if (Mode == EGGYGONiagaraEffectStopMode::DeactivateDetachKeepWorld)
	{ Original->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform); }
	if (!IsOriginalOwnerAlive())
	{ if (IsValid(Original.Get())) { Original->DestroyComponent(); } return true; }
	if (IsValid(Original.Get())) { Original->Deactivate(); }
	if (!IsOriginalOwnerAlive())
	{ if (IsValid(Original.Get())) { Original->DestroyComponent(); } return true; }
	if (IsValid(Original.Get()) && Original->IsComplete()) { Original->DestroyComponent(); }
	return true;
}

void UGGYGOAbilityTask_PlayNiagaraEffect::StopNative(EGGYGONiagaraEffectStopMode Mode)
{
	if (IsFinished()) { return; }
	if (!IsStopModeValid(Mode))
	{
		FailAndEndTask(TEXT("A valid explicit stop mode is required."));
		return;
	}
	const TWeakObjectPtr<ThisClass> OriginalTask(this);
	const auto IsOriginalOwnerAlive = [OriginalTask]()
	{
		const ThisClass* Task = OriginalTask.Get();
		return Task && !Task->IsFinished() && !Task->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed);
	};
	// 整组先退所有权及委托，再逐项转交尾部；外调关闭原 Task 后，其余未交出的资源仍须销毁。
	const TArray<TStrongObjectPtr<UNiagaraComponent>> OriginalComponents = ReleaseOwnedComponents();
	for (const TStrongObjectPtr<UNiagaraComponent>& Component : OriginalComponents)
	{
		if (!IsOriginalOwnerAlive())
		{ if (IsValid(Component.Get())) { Component->DestroyComponent(); } continue; }
		StopComponent(Component.Get(), Mode, IsOriginalOwnerAlive);
	}
	if (ThisClass* LiveTask = OriginalTask.Get(true))
	{
		if (!LiveTask->IsFinished() && !LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) { LiveTask->EndTask(); }
	}
}

void UGGYGOAbilityTask_PlayNiagaraEffect::HandleSystemFinished(UNiagaraComponent* Component)
{
	if (!OwnedComponents.Contains(Component) || IsFinished()) { return; }
	if (const auto* Original = Cast<UGGYGOFXNiagaraComponent>(Component))
	{
		const FString Failure = Original->GetEffectFailure();
		if (!Failure.IsEmpty()) { FailAndEndTask(Failure); return; }
	}
	const TWeakObjectPtr<ThisClass> OriginalTask(this);
	const TStrongObjectPtr<UNiagaraComponent> OriginalComponent(Component);
	OwnedComponents.RemoveSingle(Component);
	UnbindOwnedComponent(Component);
	if (ResourceOwner) { ResourceOwner->SetLocalOwnership(LocalCapability, !OwnedComponents.IsEmpty()); }
	// 此事实只来自仍 owned 的原生完成回调；Stop/取消已先退订，不会冒签自然完成。
	OnUnitNaturallyCompleted.Broadcast(this, Component);
	if (IsValid(OriginalComponent.Get())) { OriginalComponent->DestroyComponent(); }
	if (ThisClass* LiveTask = OriginalTask.Get(true))
	{
		if (!LiveTask->IsFinished() && !LiveTask->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& LiveTask->OwnedComponents.IsEmpty()) { LiveTask->EndTask(); }
	}
}

void UGGYGOAbilityTask_PlayNiagaraEffect::HandleOriginalAvatarEndPlay(AActor* Actor, EEndPlayReason::Type)
{
	// Attached 的原挂接实际退休；World 资源不使用此通知，也不随 Avatar 失相关回收。
	if (Actor == OriginalAvatar.Get(true) && RequestedSpace == EGGYGONiagaraEffectSpace::Attached) { EndTask(); }
}

#if WITH_EDITOR
void UGGYGOAbilityTask_PlayNiagaraEffect::HandleSystemCompiled(UNiagaraSystem* System)
{
	if (IsFinished() || !SystemCompiledHandles.Contains(System) || HasPendingNiagaraCompilation(System)) { return; }
	if (!System->IsValid() || !DoSystemScriptAttributesMatch(System))
	{
		FailAndEndTask(FString::Printf(TEXT("Niagara compilation completed with an invalid System or mismatched System VM attributes: %s"),
			*DescribeNiagaraScripts(System)));
	}
}
#endif

void UGGYGOAbilityTask_PlayNiagaraEffect::OnDestroy(bool AbilityEnded)
{
	const TStrongObjectPtr<UGGYGOMontageFXPublicationOwner> Scope(ResourceOwner);
	const auto Capability = LocalCapability;
	if (Scope.IsValid())
	{
		Scope->BeginLocalOperation(Capability);
		Scope->SealLocalProducer(Capability);
		if (LocalStartTicket.IsValid()) { Scope->CancelLocalStart(*LocalStartTicket); }
	}
	DestroyNative(AbilityEnded);
	if (Scope.IsValid()) { Scope->EndLocalOperation(Capability); Scope->FlushResourceChanges(); }
}

void UGGYGOAbilityTask_PlayNiagaraEffect::DestroyNative(bool AbilityEnded)
{
	if (AActor* Avatar = OriginalAvatar.Get(true))
	{ Avatar->OnEndPlay.RemoveDynamic(this, &ThisClass::HandleOriginalAvatarEndPlay); }
#if WITH_EDITOR
	for (const auto& Entry : SystemCompiledHandles)
	{
		if (IsValid(Entry.Key)) { Entry.Key->OnSystemCompiled().Remove(Entry.Value); }
	}
	SystemCompiledHandles.Reset();
#endif
	// 外调销毁组件前先摘除持有和委托。递归 EndTask 使用 UAbilityTask 原生销毁保护，
	// 旧清理栈不能触及后继资源。
	const TArray<TStrongObjectPtr<UNiagaraComponent>> OriginalComponents = ReleaseOwnedComponents();
	for (const TStrongObjectPtr<UNiagaraComponent>& Component : OriginalComponents)
	{ if (IsValid(Component.Get())) { Component->DestroyComponent(); } }
	Super::OnDestroy(AbilityEnded);
}
