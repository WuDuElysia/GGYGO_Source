/**
 * @file GGYGOPawnExtensionComponent.cpp
 * @brief Pawn 初始化协调者实现
 */
#include "Character/Components/GGYGOPawnExtensionComponent.h"

#include "AbilitySystem/Cues/GGYGOGameplayCueManager.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOPawnData.h"
#include "Character/Interfaces/GGYGOAvatarBindingHostInterface.h"
#include "Components/GameFrameworkComponentManager.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "System/GGYGOGameplayTags.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPawnExtensionComponent)

class FLifetimeProperty;
class UActorComponent;

struct FGGYGOPawnASCResourceHandle::FLocalResource
{
	FLocalResource(UGGYGOPawnExtensionComponent* InExtension,
		const FGGYGOPawnASCResourceIdentity& InIdentity,
		const FGGYGOAvatarBindingContext& InInstallationContext)
		: Extension(InExtension), Identity(InIdentity), InstallationContext(InInstallationContext)
	{
	}

	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Extension;
	const FGGYGOPawnASCResourceIdentity Identity;
	const FGGYGOAvatarBindingContext InstallationContext;
	FGGYGOAvatarBindingContext PublishedContext{};
	bool bInstalled = true;
	bool bEverReady = false;
	bool bReleasedNotified = false;
};


namespace
{
	FGGYGOAvatarBindingHostResult MakeAvatarHostRequestFailure(
		EGGYGOAvatarBindingOutcome Outcome, EGGYGOAvatarBindingHostReason Reason)
	{
		FGGYGOAvatarBindingHostResult Result;
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		return Result;
	}

	FGGYGOAvatarBindingHostResult DispatchOriginalAvatarHostRequest(
		const FGGYGOAvatarBindingHostRequest& Request, bool& bOutHostInvoked)
	{
		bOutHostInvoked = false;
		if (!Request.ExpectedASC.IsValid())
		{
			return MakeAvatarHostRequestFailure(EGGYGOAvatarBindingOutcome::Failed,
				EGGYGOAvatarBindingHostReason::InvalidASC);
		}
		AActor* OriginalHost = Request.ExpectedHost.Get();
		if (!OriginalHost || OriginalHost->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
		{
			return MakeAvatarHostRequestFailure(EGGYGOAvatarBindingOutcome::Failed,
				EGGYGOAvatarBindingHostReason::InvalidHost);
		}
		IGGYGOAvatarBindingHostInterface* HostInterface = Cast<IGGYGOAvatarBindingHostInterface>(OriginalHost);
		if (!HostInterface)
		{
			return MakeAvatarHostRequestFailure(EGGYGOAvatarBindingOutcome::Rejected,
				EGGYGOAvatarBindingHostReason::UnsupportedHost);
		}
		// The Host owns admission and native execution. Ordinary notice callbacks may reenter it.
		bOutHostInvoked = true;
		return HostInterface->RequestAvatarBinding(Request);
	}

	void LogAvatarHostRequestResult(const TCHAR* EntryPoint,
		const FGGYGOAvatarBindingHostRequest& Request, const FGGYGOAvatarBindingHostResult& Result)
	{
		if (Result.Outcome == EGGYGOAvatarBindingOutcome::Succeeded) { return; }
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Character/PawnExtension] %s: Host=%s ASC=%s Pawn=%s Extension=%s Operation=%u Binding=%llu Write=%llu Outcome=%u Reason=%u Steps=%d"),
			EntryPoint, *GetPathNameSafe(Request.ExpectedHost.Get()), *GetPathNameSafe(Request.ExpectedASC.Get()),
			*GetPathNameSafe(Request.ExpectedPawn.Get()), *GetPathNameSafe(Request.ExpectedExtension.Get()),
			static_cast<uint32>(Request.Operation), static_cast<unsigned long long>(Request.ExpectedContext.Binding.Serial),
			static_cast<unsigned long long>(Request.ExpectedContext.LastActorInfoWrite.Serial),
			static_cast<uint32>(Result.Outcome), static_cast<uint32>(Result.Reason), Result.Steps.Num());
	}

	void AppendAvatarHostLocalHistory(FGGYGOAvatarBindingHostResult& History,
		EGGYGOAvatarBindingHostStep Step, const FGGYGOPawnASCLocalResult& Local)
	{
		FGGYGOAvatarBindingHostStepResult ReturnedStep;
		ReturnedStep.Step = Step;
		ReturnedStep.LocalResult = Local;
		History.Steps.Add(MoveTemp(ReturnedStep));
	}
}

const FName UGGYGOPawnExtensionComponent::NAME_ActorFeatureName("PawnExtension");

UGGYGOPawnExtensionComponent::UGGYGOPawnExtensionComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 本组件是纯协调者，没有任何需要每帧推进的状态。
	PrimaryComponentTick.bStartWithTickEnabled = false;
	PrimaryComponentTick.bCanEverTick = false;

	// PawnData 需要复制到客户端，客户端才能推进 DataAvailable。
	SetIsReplicatedByDefault(true);

	PawnData = nullptr;
}

void UGGYGOPawnExtensionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UGGYGOPawnExtensionComponent, PawnData);
}

void UGGYGOPawnExtensionComponent::OnRegister()
{
	Super::OnRegister();

	const APawn* Pawn = GetPawn<APawn>();
	ensureAlwaysMsgf((Pawn != nullptr),
		TEXT("UGGYGOPawnExtensionComponent 只能挂在 Pawn 上，当前挂在 [%s]。"), *GetNameSafe(GetOwner()));

	// 一个 Pawn 上出现两个协调者会导致 feature 名冲突，
	// Manager 无法区分两者的状态，HaveAllFeaturesReachedInitState 的结果就不可信了。
	TArray<UActorComponent*> PawnExtensionComponents;
	Pawn->GetComponents(UGGYGOPawnExtensionComponent::StaticClass(), PawnExtensionComponents);
	ensureAlwaysMsgf((PawnExtensionComponents.Num() == 1),
		TEXT("[%s] 上只能有一个 UGGYGOPawnExtensionComponent，当前有 %d 个。"),
		*GetNameSafe(GetOwner()), PawnExtensionComponents.Num());

	// 尽早注册。只在 game world 里生效，编辑器预览世界里是空操作。
	RegisterInitStateFeature();
}

void UGGYGOPawnExtensionComponent::BeginPlay()
{
	// 首次 PreBegin 默认开放；只在真实组件 BeginPlay 入口重开上一生命周期。
	APawn* Pawn = GetPawn<APawn>();
	if (!HasBegunPlay() && IsRegistered() && IsValid(this) && !IsBeingDestroyed()
		&& Pawn && !Pawn->IsActorBeingDestroyed()
		&& (Pawn->IsActorBeginningPlay() || Pawn->HasActorBegunPlay()))
	{
		bLocalAbilitySystemAdmissionClosed = false;
	}
	Super::BeginPlay();

	// 监听**所有** feature 的状态变化（第一个参数 NAME_None 表示不筛选 feature，
	// 第二个参数空 Tag 表示不筛选状态）。因为 DataInitialized 的条件是
	// "所有 feature 都到 DataAvailable"，任何一个 feature 的推进都可能让条件成立。
	BindOnActorInitStateChanged(NAME_None, FGameplayTag(), false);

	// Spawned 只在这里设置一次，之后的推进全靠条件驱动。
	ensure(TryToChangeInitState(GGYGOGameplayTags::InitState_Spawned));
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 先关闭新 Install/Ready/回放，再处理原 H；关闭的 Released 只退休义务并返回真实失败。
	bLocalAbilitySystemAdmissionClosed = true;
	UninitializeAbilitySystem();
	UnregisterInitStateFeature();

	Super::EndPlay(EndPlayReason);
}

void UGGYGOPawnExtensionComponent::SetPawnData(const UGGYGOPawnData* InPawnData)
{
	check(InPawnData);

	APawn* Pawn = GetPawnChecked<APawn>();

	// 客户端不能自己设，否则会和复制过来的值打架。
	if (Pawn->GetLocalRole() != ROLE_Authority)
	{
		return;
	}

	if (PawnData)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("SetPawnData: Pawn [%s] 已有 PawnData [%s]，拒绝改为 [%s]。换角色请换 Pawn。"),
			*GetNameSafe(Pawn), *GetNameSafe(PawnData), *GetNameSafe(InPawnData));
		return;
	}

	PawnData = InPawnData;

	// 强制立即同步。默认的复制节流可能让客户端晚几帧才拿到 PawnData，
	// 那期间客户端卡在 Spawned，本地控制的角色会有可感知的输入延迟。
	Pawn->ForceNetUpdate();

	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::OnRep_PawnData()
{
	CheckDefaultInitialization();
}

UGGYGOAbilitySystemComponent* UGGYGOPawnExtensionComponent::GetGGYGOAbilitySystemComponent() const
{
	check(IsInGameThread());
	const FGGYGOPawnASCResourceHandle OriginalResource = LocalAbilitySystemResource;
	return IsLocalAbilitySystemResourceReady(OriginalResource)
		? OriginalResource.Resource->Identity.ASC.Get() : nullptr;
}

void UGGYGOPawnExtensionComponent::InitializeAbilitySystem(UGGYGOAbilitySystemComponent* InASC, AActor* InOwnerActor)
{
	check(IsInGameThread());
	FGGYGOAvatarBindingHostRequest Request;
	Request.Operation = EGGYGOAvatarBindingHostOperation::Initialize;
	Request.ExpectedHost = InOwnerActor;
	Request.ExpectedASC = InASC;
	Request.ExpectedPawn = GetPawn<APawn>();
	Request.ExpectedExtension = this;
	// 空 H 请求；Context 只取本次原 ASC 值，是否允许 Bootstrap 由 Host/ASC 判定。
	if (IsValid(InASC))
	{
		Request.ExpectedContext = InASC->GetAvatarBindingContext();
	}
	FGGYGOAvatarBindingHostResult Result;
	if (bLocalAbilitySystemAdmissionClosed || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		Result = MakeAvatarHostRequestFailure(EGGYGOAvatarBindingOutcome::Rejected,
			EGGYGOAvatarBindingHostReason::LifecycleClosed);
	}
	else if (!Request.ExpectedPawn.IsValid() || Request.ExpectedPawn->IsActorBeingDestroyed())
	{
		Result = MakeAvatarHostRequestFailure(EGGYGOAvatarBindingOutcome::Rejected,
			EGGYGOAvatarBindingHostReason::InvalidPawn);
	}
	else
	{
		bool bHostInvoked;
		Result = DispatchOriginalAvatarHostRequest(Request, bHostInvoked);
	}
	// 只有 Host -> ASC -> 本地 NotifyReady 可以完成绑定。旧栈不补广播或分发退化配置。
	LogAvatarHostRequestResult(TEXT("InitializeAbilitySystem"), Request, Result);
}

void UGGYGOPawnExtensionComponent::UninitializeAbilitySystem(UGGYGOAbilitySystemComponent* ExpectedASC)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = LocalAbilitySystemResource;
	if (!OriginalResource.HasResource()) { return; }
	const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
	if (ExpectedASC && !Identity.ASC.HasSameIndexAndSerialNumber(
		TWeakObjectPtr<UGGYGOAbilitySystemComponent>(ExpectedASC)))
	{
		return;
	}
	FGGYGOAvatarBindingHostRequest Request;
	Request.Operation = EGGYGOAvatarBindingHostOperation::Release;
	Request.ExpectedASC = Identity.ASC;
	Request.ExpectedPawn = Identity.Pawn;
	Request.ExpectedExtension = OriginalExtension;
	Request.ExpectedResource = OriginalResource;
	Request.ExpectedContext = OriginalResource.Resource->PublishedContext.HasIssuedContext()
		? OriginalResource.Resource->PublishedContext : OriginalResource.Resource->InstallationContext;
	if (UGGYGOAbilitySystemComponent* OriginalASC = Identity.ASC.Get())
	{
		// 固定的组件 Owner 是原 Host；不能用可变 ActorInfo Owner 或当前 Context 猜权限。
		Request.ExpectedHost = OriginalASC->GetOwner();
	}
	bool bHostInvoked;
	FGGYGOAvatarBindingHostResult Result = DispatchOriginalAvatarHostRequest(Request, bHostInvoked);
	if (!bHostInvoked)
	{
		// 端口未受理时只归还捕获的本地 H，保留端口失败；不执行原生替代或处理后继。
		if (UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get())
		{
			const FGGYGOPawnASCLocalResult Withdrawn =
				LiveExtension->WithdrawLocalAbilitySystemResources(OriginalResource);
			AppendAvatarHostLocalHistory(Result, EGGYGOAvatarBindingHostStep::WithdrawLocalResources, Withdrawn);
			if (Withdrawn.Outcome == EGGYGOPawnASCLocalOutcome::Succeeded)
			{
				if (UGGYGOPawnExtensionComponent* OriginalLiveExtension = OriginalExtension.Get())
				{
					const FGGYGOPawnASCLocalResult Released =
						OriginalLiveExtension->NotifyLocalResourcesReleased(OriginalResource);
					AppendAvatarHostLocalHistory(Result, EGGYGOAvatarBindingHostStep::NotifyLocalReleased, Released);
				}
			}
		}
	}
	// 已受理的原生失败归 Host 真实历史；回调接续后此处只诊断，不再写本地槽。
	LogAvatarHostRequestResult(TEXT("UninitializeAbilitySystem"), Request, Result);
}

void UGGYGOPawnExtensionComponent::HandleControllerChanged()
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = LocalAbilitySystemResource;
	FGGYGOAvatarBindingContext ConfigurationContext;
	if (OriginalResource.HasResource())
	{
		const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
		FGGYGOAvatarBindingHostRequest Request;
		Request.Operation = EGGYGOAvatarBindingHostOperation::Refresh;
		Request.ExpectedASC = Identity.ASC;
		Request.ExpectedPawn = Identity.Pawn;
		Request.ExpectedExtension = OriginalExtension;
		Request.ExpectedResource = OriginalResource;
		Request.ExpectedContext = OriginalResource.Resource->PublishedContext;
		if (UGGYGOAbilitySystemComponent* OriginalASC = Identity.ASC.Get())
		{
			Request.ExpectedHost = OriginalASC->GetOwner();
		}
		FGGYGOAvatarBindingHostResult Result;
		if (!IsLocalAbilitySystemResourceReady(OriginalResource))
		{
			Result = MakeAvatarHostRequestFailure(EGGYGOAvatarBindingOutcome::Rejected,
				EGGYGOAvatarBindingHostReason::ReadyNotEstablished);
		}
		else
		{
			bool bHostInvoked;
			Result = DispatchOriginalAvatarHostRequest(Request, bHostInvoked);
		}
		LogAvatarHostRequestResult(TEXT("HandleControllerChanged"), Request, Result);
		if (Result.Outcome != EGGYGOAvatarBindingOutcome::Succeeded) { return; }
		for (const FGGYGOAvatarBindingHostStepResult& Step : Result.Steps)
		{
			if (Step.Step == EGGYGOAvatarBindingHostStep::ActorInfoRefresh && Step.ASCResult.IsSet()
				&& Step.ASCResult.GetValue().bCommitted)
			{
				ConfigurationContext = Step.ASCResult.GetValue().CommittedContext;
			}
		}
		if (!ConfigurationContext.HasIssuedContext())
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("[Character/PawnExtension] Refresh returned no committed Context: Host=%s ASC=%s Extension=%s."),
				*GetPathNameSafe(Request.ExpectedHost.Get()), *GetPathNameSafe(Request.ExpectedASC.Get()),
				*GetPathNameSafe(OriginalExtension.Get()));
			return;
		}
	}
	UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get();
	if (!LiveExtension || LiveExtension->bLocalAbilitySystemAdmissionClosed
		|| LiveExtension->IsBeingDestroyed() || LiveExtension->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| (OriginalResource.HasResource()
			&& (!LiveExtension->IsLocalAbilitySystemResourceReady(OriginalResource)
				|| !OriginalResource.Resource->PublishedContext.HasSameContext(ConfigurationContext))))
	{
		return;
	}
	// 只推进本次真实 Refresh 后仍匹配的原 H/Context；后续写入或装配接续会停止旧尾部。
	LiveExtension->CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::HandlePlayerStateReplicated()
{
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::SetupPlayerInputComponent()
{
	CheckDefaultInitialization();
}

void UGGYGOPawnExtensionComponent::ApplyPawnDataToConsumers()
{
	if (!PawnData)
	{
		return;
	}

	// ASC 侧的配置（组规则表、Tag 关系表）不在这里做：它们属于 ASC，
	// 而 ASC 归队伍位置持有，由 `AGGYGOCharacterSlot::InitializeForPawnData` 注入。
	// 在两处都写会让"当前生效的是哪一份配置"取决于两个初始化流程的先后。

	// 移动层。用 FindComponentByClass 而不是要求 Owner 是 ACharacter ——
	// 载具、飞行单位将来可能不是 Character，那时它们没有这个组件，跳过即可。
	if (AActor* Owner = GetOwner())
	{
		if (UGGYGOCharacterMovementComponent* MoveComp = Owner->FindComponentByClass<UGGYGOCharacterMovementComponent>())
		{
			MoveComp->SetMovementSet(PawnData->MovementSet);
		}
	}

	// 预热该角色的特效。Cue 是按需异步加载的，不预热则第一次触发时
	// 资产还没就位，表现为"第一刀没有火花"。
	if (UGGYGOGameplayCueManager* CueManager = UGGYGOGameplayCueManager::Get())
	{
		CueManager->PreloadCuesForTags(PawnData->CuesToPreload);
	}
}

void UGGYGOPawnExtensionComponent::CheckDefaultInitialization()
{
	// 先推进别人再推进自己。
	// 本组件的 DataInitialized 依赖所有 feature 到达 DataAvailable，
	// 如果不先给它们一次推进机会，第一次调用时它们可能还卡在 Spawned，
	// 于是本组件推不动，而它们要等本组件的状态变化才会被再次唤醒 —— 死锁。
	CheckDefaultInitializationForImplementers();

	static const TArray<FGameplayTag> StateChain = {
		GGYGOGameplayTags::InitState_Spawned,
		GGYGOGameplayTags::InitState_DataAvailable,
		GGYGOGameplayTags::InitState_DataInitialized,
		GGYGOGameplayTags::InitState_GameplayReady
	};

	// 一次调用可能连续推进多级，直到某一级的条件不满足为止。
	ContinueInitStateChain(StateChain);
}

bool UGGYGOPawnExtensionComponent::CanChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState) const
{
	check(Manager);

	APawn* Pawn = GetPawn<APawn>();

	if (!CurrentState.IsValid() && DesiredState == GGYGOGameplayTags::InitState_Spawned)
	{
		// 挂在合法 Pawn 上就算 Spawned。
		return Pawn != nullptr;
	}

	if (CurrentState == GGYGOGameplayTags::InitState_Spawned && DesiredState == GGYGOGameplayTags::InitState_DataAvailable)
	{
		// PawnData 是硬性前提：没有它就不知道该授予什么能力。
		if (!PawnData)
		{
			return false;
		}

		const bool bHasAuthority = Pawn->HasAuthority();
		const bool bIsLocallyControlled = Pawn->IsLocallyControlled();

		if (bHasAuthority || bIsLocallyControlled)
		{
			// 只有服务器和本地控制端需要等 Controller。
			// 模拟代理（别人的角色在我的客户端上）永远等不到 Controller，
			// 若一并要求就会卡在 Spawned，它身上的动画与表现组件全都初始化不了。
			if (!GetController<AController>())
			{
				return false;
			}
		}

		return true;
	}

	if (CurrentState == GGYGOGameplayTags::InitState_DataAvailable && DesiredState == GGYGOGameplayTags::InitState_DataInitialized)
	{
		// 等齐所有 feature。这一句就是"不必手工排初始化顺序"的全部原因。
		return Manager->HaveAllFeaturesReachedInitState(Pawn, GGYGOGameplayTags::InitState_DataAvailable);
	}

	if (CurrentState == GGYGOGameplayTags::InitState_DataInitialized && DesiredState == GGYGOGameplayTags::InitState_GameplayReady)
	{
		return true;
	}

	return false;
}

void UGGYGOPawnExtensionComponent::HandleChangeInitState(UGameFrameworkComponentManager* Manager, FGameplayTag CurrentState, FGameplayTag DesiredState)
{
	if (DesiredState == GGYGOGameplayTags::InitState_DataInitialized)
	{
		// 配置分发放在这一步，而不是 InitializeAbilitySystem 里。
		//
		// 原因是时序：Host 绑定与 PawnData 到达相互独立，
		// 运行时生成的角色可能先 SpawnActor 再 SetPawnData。
		// 而 DataInitialized 的前置条件里包含 DataAvailable，后者要求 PawnData 非空，
		// 所以走到这里 PawnData 一定有值。
		ApplyPawnDataToConsumers();
	}
}

void UGGYGOPawnExtensionComponent::OnActorInitStateChanged(const FActorInitStateChangedParams& Params)
{
	// 别人到了 DataAvailable，重新尝试推进自己 —— 可能正是它让"所有 feature 齐了"成立。
	// 排除自己是为了避免递归：本组件推进时也会触发这个回调。
	if (Params.FeatureName != NAME_ActorFeatureName)
	{
		if (Params.FeatureState == GGYGOGameplayTags::InitState_DataAvailable)
		{
			CheckDefaultInitialization();
		}
	}
}

void UGGYGOPawnExtensionComponent::OnAbilitySystemInitialized_RegisterAndCall(FSimpleMulticastDelegate::FDelegate Delegate)
{
	check(IsInGameThread());
	if (!Delegate.IsBound() || bLocalAbilitySystemAdmissionClosed || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Character/PawnExtension] Initialized registration rejected: Extension=%s, unbound delegate or closed lifecycle."),
			*GetPathNameSafe(this));
		return;
	}
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = LocalAbilitySystemResource;
	if (!OnAbilitySystemInitialized.IsBoundToObject(Delegate.GetUObject()))
	{
		OnAbilitySystemInitialized.Add(Delegate);
	}
	if (IsLocalAbilitySystemResourceReady(OriginalResource))
	{
		const FGGYGOAvatarBindingContext OriginalContext = OriginalResource.Resource->PublishedContext;
		Delegate.Execute();
		UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get();
		if (!LiveExtension || !LiveExtension->IsLocalAbilitySystemResourceReady(OriginalResource)
			|| !OriginalResource.Resource->PublishedContext.HasSameContext(OriginalContext))
		{
			UE_LOG(LogGGYGOAbilitySystem, Verbose,
				TEXT("[Character/PawnExtension] Initialized replay invalidated: Extension=%s Binding=%llu Write=%llu."),
				*GetPathNameSafe(OriginalExtension.Get()),
				static_cast<unsigned long long>(OriginalContext.Binding.Serial),
				static_cast<unsigned long long>(OriginalContext.LastActorInfoWrite.Serial));
		}
	}
}

void UGGYGOPawnExtensionComponent::OnAbilitySystemUninitialized_Register(FSimpleMulticastDelegate::FDelegate Delegate)
{
	check(IsInGameThread());
	if (!Delegate.IsBound() || bLocalAbilitySystemAdmissionClosed || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("[Character/PawnExtension] Uninitialized registration rejected: Extension=%s, unbound delegate or closed lifecycle."),
			*GetPathNameSafe(this));
		return;
	}
	if (!OnAbilitySystemUninitialized.IsBoundToObject(Delegate.GetUObject()))
	{
		OnAbilitySystemUninitialized.Add(Delegate);
	}
}

// K4-Character-L1 local resource implementation begin.
namespace
{
	FGGYGOPawnASCLocalResult MakePawnASCLocalResourceResult(
		const TWeakObjectPtr<UGGYGOPawnExtensionComponent>& Extension,
		const FGGYGOPawnASCResourceHandle& OriginalResource,
		EGGYGOPawnASCLocalOutcome Outcome, EGGYGOPawnASCLocalReason Reason,
		bool bLocalChanged = false)
	{
		FGGYGOPawnASCLocalResult Result;
		Result.Outcome = Outcome;
		Result.Reason = Reason;
		Result.Resource = OriginalResource;
		Result.bLocalChanged = bLocalChanged;
		if (Outcome != EGGYGOPawnASCLocalOutcome::Succeeded)
		{
			const FGGYGOPawnASCResourceIdentity Identity = OriginalResource.GetIdentity();
			UE_LOG(LogGGYGOAbilitySystem, Verbose,
				TEXT("[Character/PawnExtension] LocalResources Extension=%s ASC=%s Pawn=%s Binding=%llu Outcome=%u Reason=%u"),
				*GetNameSafe(Extension.Get()), *GetNameSafe(Identity.ASC.Get()), *GetNameSafe(Identity.Pawn.Get()),
				static_cast<unsigned long long>(Identity.Binding.Serial),
				static_cast<uint32>(Outcome), static_cast<uint32>(Reason));
		}
		return Result;
	}
}

bool FGGYGOPawnASCResourceIdentity::HasSameIdentity(const FGGYGOPawnASCResourceIdentity& Other) const
{
	return Binding.HasSameIdentity(Other.Binding)
		&& ASC.HasSameIndexAndSerialNumber(Other.ASC)
		&& Pawn.HasSameIndexAndSerialNumber(Other.Pawn);
}

bool FGGYGOPawnASCResourceHandle::HasResource() const
{
	return Resource.IsValid();
}

bool FGGYGOPawnASCResourceHandle::HasSameResource(const FGGYGOPawnASCResourceHandle& Other) const
{
	return Resource.IsValid() && Other.Resource.IsValid() && Resource == Other.Resource;
}

FGGYGOPawnASCResourceIdentity FGGYGOPawnASCResourceHandle::GetIdentity() const
{
	return Resource.IsValid() ? Resource->Identity : FGGYGOPawnASCResourceIdentity{};
}

bool UGGYGOPawnExtensionComponent::OwnsLocalAbilitySystemResource(
	const FGGYGOPawnASCResourceHandle& ExpectedResource) const
{
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> Self(
		const_cast<UGGYGOPawnExtensionComponent*>(this));
	return ExpectedResource.HasResource()
		&& ExpectedResource.Resource->Extension.HasSameIndexAndSerialNumber(Self);
}

FGGYGOPawnASCLocalResult UGGYGOPawnExtensionComponent::InstallLocalAbilitySystemResources(
	UGGYGOAbilitySystemComponent* ExpectedASC, APawn* ExpectedPawn,
	const FGGYGOAvatarBindingContext& CommittedContext)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle Empty;
	const auto Reject = [&](EGGYGOPawnASCLocalReason Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("[Character/PawnExtension] Install rejected: Extension=%s ExpectedASC=%s ExpectedPawn=%s Binding=%llu Reason=%u"),
			*GetNameSafe(OriginalExtension.Get()), *GetNameSafe(ExpectedASC), *GetNameSafe(ExpectedPawn),
			static_cast<unsigned long long>(CommittedContext.Binding.Serial), static_cast<uint32>(Reason));
		return MakePawnASCLocalResourceResult(OriginalExtension, Empty, EGGYGOPawnASCLocalOutcome::Rejected, Reason);
	};
	if (bLocalAbilitySystemAdmissionClosed || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(EGGYGOPawnASCLocalReason::LifecycleClosed);
	}
	if (!IsValid(ExpectedASC) || ExpectedASC->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(EGGYGOPawnASCLocalReason::InvalidASC);
	}
	if (!IsValid(ExpectedPawn) || ExpectedPawn->IsActorBeingDestroyed())
	{
		return Reject(EGGYGOPawnASCLocalReason::InvalidPawn);
	}
	if (GetPawn<APawn>() != ExpectedPawn)
	{
		return Reject(EGGYGOPawnASCLocalReason::WrongExtension);
	}
	if (!CommittedContext.HasIssuedContext())
	{
		return Reject(EGGYGOPawnASCLocalReason::InvalidBinding);
	}
	EGGYGOAvatarBindingReason ASCReason;
	if (ExpectedASC->CheckAvatarBindingContext(CommittedContext, ASCReason) != EGGYGOAvatarBindingOutcome::Succeeded
		|| ExpectedASC->GetAvatarActor() != ExpectedPawn)
	{
		return Reject(EGGYGOPawnASCLocalReason::ContextMismatch);
	}
	FGGYGOPawnASCResourceIdentity Identity;
	Identity.ASC = ExpectedASC;
	Identity.Pawn = ExpectedPawn;
	Identity.Binding = CommittedContext.Binding;
	const FGGYGOPawnASCResourceHandle Existing = LocalAbilitySystemResource;
	if (Existing.HasResource())
	{
		if (!OwnsLocalAbilitySystemResource(Existing) || !Existing.Resource->bInstalled
			|| !Existing.Resource->Identity.HasSameIdentity(Identity))
		{
			return Reject(EGGYGOPawnASCLocalReason::ResourceConflict);
		}
		return MakePawnASCLocalResourceResult(OriginalExtension, Existing,
			EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None);
	}
	FGGYGOPawnASCResourceHandle Installed;
	Installed.Resource = MakeShared<FGGYGOPawnASCResourceHandle::FLocalResource>(this, Identity, CommittedContext);
	// No external calls, PawnData distribution or legacy cache writes in this installation.
	LocalAbilitySystemResource = Installed;
	return MakePawnASCLocalResourceResult(OriginalExtension, Installed,
		EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, true);
}

FGGYGOPawnASCLocalResult UGGYGOPawnExtensionComponent::WithdrawLocalAbilitySystemResources(
	const FGGYGOPawnASCResourceHandle& ExpectedResource)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = ExpectedResource;
	if (!OriginalResource.HasResource())
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Rejected, EGGYGOPawnASCLocalReason::InvalidArguments);
	}
	if (!OwnsLocalAbilitySystemResource(OriginalResource))
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Rejected, EGGYGOPawnASCLocalReason::WrongExtension);
	}
	if (!OriginalResource.Resource->bInstalled)
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None);
	}
	if (!LocalAbilitySystemResource.HasSameResource(OriginalResource))
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::ResourceConflict);
	}
	// Detach before any future callbacks. Expired ASC/Pawn identities are not dereferenced.
	LocalAbilitySystemResource = FGGYGOPawnASCResourceHandle{};
	OriginalResource.Resource->bInstalled = false;
	return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
		EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, true);
}

bool UGGYGOPawnExtensionComponent::IsLocalAbilitySystemResourceInstalled(
	const FGGYGOPawnASCResourceHandle& ExpectedResource) const
{
	check(IsInGameThread());
	if (!IsValid(this) || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
		|| !OwnsLocalAbilitySystemResource(ExpectedResource)
		|| !LocalAbilitySystemResource.HasSameResource(ExpectedResource)
		|| !ExpectedResource.Resource->bInstalled)
	{
		return false;
	}
	const FGGYGOPawnASCResourceIdentity& Identity = ExpectedResource.Resource->Identity;
	UGGYGOAbilitySystemComponent* OriginalASC = Identity.ASC.Get();
	APawn* OriginalPawn = Identity.Pawn.Get();
	EGGYGOAvatarBindingReason ASCReason;
	return OriginalASC && OriginalPawn && !OriginalPawn->IsActorBeingDestroyed()
		&& GetPawn<APawn>() == OriginalPawn
		&& OriginalASC->CheckAvatarBindingIdentity(Identity.Binding, ASCReason) == EGGYGOAvatarBindingOutcome::Succeeded
		&& OriginalASC->GetAvatarActor() == OriginalPawn;
}

bool UGGYGOPawnExtensionComponent::IsLocalAbilitySystemResourceReady(
	const FGGYGOPawnASCResourceHandle& ExpectedResource) const
{
	check(IsInGameThread());
	if (bLocalAbilitySystemAdmissionClosed || IsBeingDestroyed()
		|| !IsLocalAbilitySystemResourceInstalled(ExpectedResource) || !ExpectedResource.Resource->bEverReady)
	{
		return false;
	}
	UGGYGOAbilitySystemComponent* OriginalASC = ExpectedResource.Resource->Identity.ASC.Get();
	return OriginalASC
		&& ExpectedResource.Resource->Identity.Binding.HasSameIdentity(ExpectedResource.Resource->PublishedContext.Binding)
		&& OriginalASC->IsAvatarBindingPublicationContextCurrent(ExpectedResource.Resource->PublishedContext);
}

FGGYGOPawnASCLocalResult UGGYGOPawnExtensionComponent::NotifyLocalResourcesReleased(
	const FGGYGOPawnASCResourceHandle& ReleasedResource)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = ReleasedResource;
	if (!OriginalResource.HasResource() || !OwnsLocalAbilitySystemResource(OriginalResource))
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Rejected, OriginalResource.HasResource()
				? EGGYGOPawnASCLocalReason::WrongExtension : EGGYGOPawnASCLocalReason::InvalidArguments);
	}
	if (OriginalResource.Resource->bInstalled)
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Rejected, EGGYGOPawnASCLocalReason::ResourceNotWithdrawn);
	}
	if (bLocalAbilitySystemAdmissionClosed || !OriginalExtension.IsValid() || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		// Retire only this local notification obligation; a closed Extension cannot call observers.
		const bool bChanged = !OriginalResource.Resource->bReleasedNotified;
		OriginalResource.Resource->bReleasedNotified = true;
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Failed, EGGYGOPawnASCLocalReason::LifecycleClosed, bChanged);
	}
	if (OriginalResource.Resource->bReleasedNotified)
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None);
	}
	const FGGYGOPawnASCResourceIdentity OriginalIdentity = OriginalResource.GetIdentity();
	const auto RecheckReleased = [&]()
	{
		UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get();
		return LiveExtension && !LiveExtension->bLocalAbilitySystemAdmissionClosed
			&& !LiveExtension->IsBeingDestroyed() && !LiveExtension->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)
			&& LiveExtension->OwnsLocalAbilitySystemResource(OriginalResource)
			&& OriginalResource.Resource->Identity.HasSameIdentity(OriginalIdentity)
			&& !OriginalResource.Resource->bInstalled && OriginalResource.Resource->bReleasedNotified;
	};
	OriginalResource.Resource->bReleasedNotified = true;
	FGGYGOPawnASCLocalNotice Notice;
	Notice.Kind = EGGYGOPawnASCLocalNoticeKind::Released;
	Notice.Resource = OriginalResource;
	LocalAbilitySystemNotice.Broadcast(Notice);
	if (!RecheckReleased())
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated, true);
	}
	UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get();
	if (LiveExtension->LocalAbilitySystemResource.HasResource())
	{
		// Identity consumers may finish old cleanup; a no-argument observer cannot identify a successor.
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated, true);
	}
	LiveExtension->OnAbilitySystemUninitialized.Broadcast();
	if (!RecheckReleased() || OriginalExtension.Get()->LocalAbilitySystemResource.HasResource())
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated, true);
	}
	return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
		EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, true);
}

FGGYGOPawnASCLocalResult UGGYGOPawnExtensionComponent::NotifyLocalResourcesReady(
	const FGGYGOPawnASCResourceHandle& ExpectedResource,
	const FGGYGOAvatarBindingPublicationReceipt& Publication)
{
	check(IsInGameThread());
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = ExpectedResource;
	const FGGYGOAvatarBindingPublicationReceipt OwnPublication = Publication;
	const auto Reject = [&](EGGYGOPawnASCLocalReason Reason)
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource, EGGYGOPawnASCLocalOutcome::Rejected, Reason);
	};
	if (bLocalAbilitySystemAdmissionClosed || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		return Reject(EGGYGOPawnASCLocalReason::LifecycleClosed);
	}
	if (!OriginalResource.HasResource()) { return Reject(EGGYGOPawnASCLocalReason::InvalidArguments); }
	if (!OwnsLocalAbilitySystemResource(OriginalResource)) { return Reject(EGGYGOPawnASCLocalReason::WrongExtension); }
	if (!OriginalResource.Resource->bInstalled || !LocalAbilitySystemResource.HasSameResource(OriginalResource))
	{
		return Reject(EGGYGOPawnASCLocalReason::ResourceNotInstalled);
	}
	UGGYGOAbilitySystemComponent* OriginalASC = OriginalResource.Resource->Identity.ASC.Get();
	if (!OriginalASC) { return Reject(EGGYGOPawnASCLocalReason::InvalidASC); }
	// This authentication MUST precede reading history, including duplicate and Refresh paths.
	if (!OriginalASC->IsAvatarBindingNoticeDispatching(OwnPublication))
	{
		return Reject(EGGYGOPawnASCLocalReason::PublicationNotDispatching);
	}
	FGGYGOAvatarBindingResult History;
	FGGYGOAvatarBindingNotice BindingNotice;
	if (!OwnPublication.TryGetCommittedEvidence(History, BindingNotice))
	{
		return Reject(EGGYGOPawnASCLocalReason::InvalidPublication);
	}
	const bool bInitialized = BindingNotice.Kind == EGGYGOAvatarBindingNoticeKind::Initialized;
	const bool bRefreshed = BindingNotice.Kind == EGGYGOAvatarBindingNoticeKind::Refreshed;
	const FGGYGOPawnASCResourceIdentity& Identity = OriginalResource.Resource->Identity;
	APawn* OriginalPawn = Identity.Pawn.Get();
	const TWeakObjectPtr<AActor> OriginalAvatar(OriginalPawn);
	if ((!bInitialized && !bRefreshed) || !History.bCommitted
		|| !History.CommittedContext.HasSameContext(BindingNotice.After)
		|| !Identity.Binding.HasSameIdentity(BindingNotice.After.Binding)
		|| !OriginalAvatar.HasSameIndexAndSerialNumber(BindingNotice.AvatarActor)
		|| !OriginalPawn || OriginalPawn->IsActorBeingDestroyed() || GetPawn<APawn>() != OriginalPawn)
	{
		return Reject(EGGYGOPawnASCLocalReason::InvalidPublication);
	}
	if (bInitialized && !OriginalResource.Resource->InstallationContext.HasSameContext(BindingNotice.After))
	{
		return Reject(EGGYGOPawnASCLocalReason::ContextMismatch);
	}
	if (bRefreshed && (!OriginalResource.Resource->bEverReady
		|| !Identity.Binding.HasSameIdentity(BindingNotice.Before.Binding)))
	{
		return Reject(EGGYGOPawnASCLocalReason::ReadyNotEstablished);
	}
	if (OriginalResource.Resource->bEverReady
		&& OriginalResource.Resource->PublishedContext.HasSameContext(BindingNotice.After))
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None);
	}
	const auto RecheckReady = [&]()
	{
		UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get();
		UGGYGOAbilitySystemComponent* LiveASC = Identity.ASC.Get();
		return LiveExtension && LiveASC && LiveExtension->IsLocalAbilitySystemResourceReady(OriginalResource)
			&& OriginalResource.Resource->PublishedContext.HasSameContext(BindingNotice.After)
			&& LiveASC->IsAvatarBindingNoticeDispatching(OwnPublication);
	};
	// Mark this exact publication before external callbacks; reentrant delivery is idempotent.
	OriginalResource.Resource->bEverReady = true;
	OriginalResource.Resource->PublishedContext = BindingNotice.After;
	FGGYGOPawnASCLocalNotice Notice;
	Notice.Kind = bInitialized ? EGGYGOPawnASCLocalNoticeKind::Ready : EGGYGOPawnASCLocalNoticeKind::Refreshed;
	Notice.Resource = OriginalResource;
	Notice.PublishedContext = BindingNotice.After;
	LocalAbilitySystemNotice.Broadcast(Notice);
	if (!RecheckReady())
	{
		return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
			EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated, true);
	}
	if (bInitialized)
	{
		OriginalExtension.Get()->OnAbilitySystemInitialized.Broadcast();
		if (!RecheckReady())
		{
			return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
				EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated, true);
		}
	}
	return MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
		EGGYGOPawnASCLocalOutcome::Succeeded, EGGYGOPawnASCLocalReason::None, true);
}

FDelegateHandle UGGYGOPawnExtensionComponent::RegisterLocalAbilitySystemNoticeAndCall(
	FGGYGOPawnASCLocalNoticeDelegate::FDelegate Delegate)
{
	check(IsInGameThread());
	if (!Delegate.IsBound() || bLocalAbilitySystemAdmissionClosed || !IsValid(this) || IsBeingDestroyed()
		|| HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
	{
		UE_LOG(LogGGYGOAbilitySystem, Verbose,
			TEXT("[Character/PawnExtension] Local notice registration rejected: Extension=%s, unbound delegate or closed lifecycle."),
			*GetNameSafe(this));
		return FDelegateHandle{};
	}
	const FDelegateHandle Handle = LocalAbilitySystemNotice.Add(Delegate);
	const TWeakObjectPtr<UGGYGOPawnExtensionComponent> OriginalExtension(this);
	const FGGYGOPawnASCResourceHandle OriginalResource = LocalAbilitySystemResource;
	if (IsLocalAbilitySystemResourceReady(OriginalResource))
	{
		FGGYGOPawnASCLocalNotice Notice;
		Notice.Kind = EGGYGOPawnASCLocalNoticeKind::Ready;
		Notice.Resource = OriginalResource;
		Notice.PublishedContext = OriginalResource.Resource->PublishedContext;
		Delegate.Execute(Notice);
		UGGYGOPawnExtensionComponent* LiveExtension = OriginalExtension.Get();
		if (!LiveExtension || !LiveExtension->IsLocalAbilitySystemResourceReady(OriginalResource)
			|| !OriginalResource.Resource->PublishedContext.HasSameContext(Notice.PublishedContext))
		{
			// The returned handle is registration history, never proof that this replay remains Ready.
			MakePawnASCLocalResourceResult(OriginalExtension, OriginalResource,
				EGGYGOPawnASCLocalOutcome::Stale, EGGYGOPawnASCLocalReason::CallbackInvalidated);
		}
	}
	return Handle;
}

void UGGYGOPawnExtensionComponent::UnregisterLocalAbilitySystemNotice(FDelegateHandle Handle)
{
	check(IsInGameThread());
	LocalAbilitySystemNotice.Remove(Handle);
}

FGGYGOPawnASCResourceHandle UGGYGOPawnExtensionComponent::GetCurrentLocalAbilitySystemResource() const
{
	check(IsInGameThread());
	return LocalAbilitySystemResource;
}

// K4-Character-L1 local resource implementation end.
