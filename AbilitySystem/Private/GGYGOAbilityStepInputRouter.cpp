/**
 * @file GGYGOAbilityStepInputRouter.cpp
 * @brief ASC 分段输入路由实现：步骤输入来源认证、Initial 运输终态与 TargetData RPC 处理。
 */
#include "AbilitySystem/Private/GGYGOAbilityStepInputRouter.h"

#include "AbilitySystem/Abilities/GGYGOGameplayAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/Private/GGYGOAbilityInputBuffer.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Input/GGYGOPlayerInput.h"

#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

struct FGGYGOAbilityInitialInputRoute::FRouteProof
{
	enum class EState : uint8 { Pending, Submitted, Unavailable, ConsumedUnavailable };
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
	FGGYGOAbilityActivationHandle Activation;
	FGameplayAbilitySpecHandle Spec;
	FPredictionKey ActivationKey;
	bool bPredictingOwner = false;
	EState State = EState::Pending;
	TWeakPtr<FAbilityReplicatedDataCache> CancellationCache;
};

bool FGGYGOAbilityInitialInputRoute::HasSubmittedInitial() const
{
	return Proof.IsValid() && Proof->State == FRouteProof::EState::Submitted;
}

void FGGYGOAbilityInitialInputRoute::NotifyInitialUnavailable() const
{
	const FGGYGOAbilityInitialInputRoute Original = *this;
	if (UGGYGOAbilitySystemComponent* ASC = Original.Proof.IsValid() ? Original.Proof->ASC.Get() : nullptr)
		ASC->NotifyInitialInputUnavailable(Original);
}

/** 原请求身份不可变；EntryFact 是该请求唯一的一次提交记录，不是输入或能力状态。 */
struct FGGYGOAbilityStepInputRequest::FRequestProof
{
	TWeakObjectPtr<UGGYGOAbilitySystemComponent> ASC;
	TWeakObjectPtr<UGGYGOGameplayAbility> Ability;
	TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> Task;
	TWeakObjectPtr<APawn> Avatar;
	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<UNetConnection> Connection;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement;
	TWeakObjectPtr<UWorld> World;
	FGGYGOAbilityActivationHandle Activation;
	FGameplayAbilitySpecHandle Spec;
	FPredictionKey ActivationKey;
	FGGYGOMovementInputEvaluationToken Edge;
	EGGYGOAbilityStepInputKind Kind = EGGYGOAbilityStepInputKind::Initial;
	int32 SourceStep = INDEX_NONE;
	int32 RequestId = 0;
	bool bRemote = false;
	TSharedPtr<const FGGYGOMovementInputStepFact> EntryFact;
	FGGYGOMovementOwnerSyncScopeId EntryScope;
	uint64 EntryFrame = 0;
};

bool FGGYGOAbilityStepInputRequest::HasSubmittedEntryFact() const { return Proof.IsValid() && Proof->EntryFact.IsValid(); }
bool FGGYGOAbilityStepInputRequest::IsRemote() const { return Proof.IsValid() && Proof->bRemote; }
EGGYGOAbilityStepInputKind FGGYGOAbilityStepInputRequest::GetKind() const
{ return Proof.IsValid() ? Proof->Kind : EGGYGOAbilityStepInputKind::Initial; }
int32 FGGYGOAbilityStepInputRequest::GetSourceStep() const { return Proof.IsValid() ? Proof->SourceStep : INDEX_NONE; }
int32 FGGYGOAbilityStepInputRequest::GetRequestId() const { return Proof.IsValid() ? Proof->RequestId : INDEX_NONE; }
FGGYGOMovementInputEvaluationToken FGGYGOAbilityStepInputRequest::GetOriginalEdgeToken() const
{
	return Proof.IsValid() ? Proof->Edge : FGGYGOMovementInputEvaluationToken{};
}

bool FGGYGOAbilityStepInputRequest::IsCurrentFor(const UGGYGOCharacterMovementComponent* OriginalMovement,
	const FGGYGOMovementOwnerSyncScopeId& OriginalScope, FString& OutError) const
{
	OutError.Reset();
	const auto Original = Proof;
	UGGYGOAbilitySystemComponent* ASC = Original.IsValid() ? Original->ASC.Get() : nullptr;
	if (!ASC) { OutError = TEXT("原步骤输入请求或 ASC 已失效。"); return false; }
	if (!ASC->CheckStepInputRequestCurrent(*this, OutError)) return false;
	FGGYGOMovementOwnerSyncScopeId CurrentScope;
	if (!OriginalMovement || Original->Movement.Get() != OriginalMovement
		|| !OriginalMovement->GetMovementOwnerSyncScope(CurrentScope, OutError)
		|| CurrentScope != OriginalScope
		|| (Original->EntryScope.IsSet() && Original->EntryScope != OriginalScope))
	{
		OutError = TEXT("原步骤输入请求与 CMC/Scope 不匹配。");
		return false;
	}
	return true;
}

EGGYGOMovementInputEvaluationState FGGYGOAbilityStepInputRequest::QueryStepStartInputReadiness(FString& OutError) const
{
	OutError.Reset();
	UGGYGOAbilitySystemComponent* ASC = Proof.IsValid() ? Proof->ASC.Get() : nullptr;
	if (!ASC) { OutError = TEXT("原步骤输入请求或 ASC 已失效。"); return EGGYGOMovementInputEvaluationState::Invalidated; }
	if (!ASC->CheckStepInputRequestCurrent(*this, OutError)) return EGGYGOMovementInputEvaluationState::Invalidated;
	if (Proof->EntryFact.IsValid()) return EGGYGOMovementInputEvaluationState::Completed;
	if (Proof->bRemote) { OutError = TEXT("远端没有原 owned 事实。"); return EGGYGOMovementInputEvaluationState::Invalidated; }
	return UGGYGOPlayerInput::QueryStepStartInputReadiness(Proof->Edge, OutError);
}

EGGYGOMovementInputEvaluationState FGGYGOAbilityStepInputRequest::QueryStepStartInputFact(
	FGGYGOMovementInputStepFact& OutFact, FString& OutError) const
{
	OutFact = {};
	OutError.Reset();
	const auto Original = Proof;
	UGGYGOAbilitySystemComponent* ASC = Original.IsValid() ? Original->ASC.Get() : nullptr;
	if (!ASC) { OutError = TEXT("原步骤输入请求或 ASC 已失效。"); return EGGYGOMovementInputEvaluationState::Invalidated; }
	if (Original->EntryFact.IsValid())
	{
		if (!ValidateStepStartInputFact(*Original->EntryFact, OutError))
			return EGGYGOMovementInputEvaluationState::Invalidated;
		OutFact = *Original->EntryFact;
		return EGGYGOMovementInputEvaluationState::Completed;
	}
	if (!ASC->CheckStepInputRequestCurrent(*this, OutError))
		return EGGYGOMovementInputEvaluationState::Invalidated;
	if (Original->bRemote)
	{
		OutError = TEXT("远端步骤请求没有原 owned 入站事实。");
		return EGGYGOMovementInputEvaluationState::Invalidated;
	}
	return UGGYGOPlayerInput::QueryStepStartInputFact(Original->Edge, OutFact, OutError);
}

bool FGGYGOAbilityStepInputRequest::SubmitStepStartInputFact(
	const FGGYGOMovementInputStepFact& Fact, FString& OutError) const
{
	OutError.Reset();
	const FGGYGOAbilityStepInputRequest Original = *this;
	UGGYGOAbilityTask_WaitComboInput* Task = Original.Proof.IsValid() ? Original.Proof->Task.Get() : nullptr;
	if (!Task || Original.IsRemote()) { OutError = TEXT("缺少原本地输入 Task，不能提交步骤事实。"); return false; }
	return Task->SubmitEntryFact(Original, Fact, OutError);
}

bool FGGYGOAbilityStepInputRequest::ValidateStepStartInputFact(
	const FGGYGOMovementInputStepFact& Fact, FString& OutError) const
{
	OutError.Reset();
	const auto Original = Proof;
	UGGYGOAbilitySystemComponent* ASC = Original.IsValid() ? Original->ASC.Get() : nullptr;
	if (!ASC) { OutError = TEXT("原步骤输入请求或 ASC 已失效。"); return false; }
	if (!ASC->CheckStepInputRequestCurrent(*this, OutError)) return false;
	if (Original->EntryFact.IsValid())
	{
		const auto& Stored = *Original->EntryFact;
		if (Stored.SourceSessionSerial != Fact.SourceSessionSerial
			|| Stored.OriginalEdgeEvaluationSerial != Fact.OriginalEdgeEvaluationSerial
			|| Stored.StepEvaluationSerial != Fact.StepEvaluationSerial || Stored.InputState != Fact.InputState
			|| Stored.ActionValue != Fact.ActionValue || Stored.ControlYaw != Fact.ControlYaw)
		{ OutError = TEXT("已提交步骤事实不可替换。"); return false; }
		if (!Original->bRemote && (Original->EntryFrame != GFrameCounter
			|| UGGYGOPlayerInput::QueryMovementInputEvaluationToken(Original->Edge, OutError)
				!= EGGYGOMovementInputEvaluationState::Completed))
		{
			if (OutError.IsEmpty()) OutError = TEXT("已提交本地事实不能跨入场帧或原输入 Session 重用。");
			return false;
		}
		return true;
	}
	else if (Original->bRemote) { OutError = TEXT("远端缺少原接收封装。"); return false; }
	// 尚未提交的本地候选才查 Input；封存后只核原 owned 资源和同一事实。
	return UGGYGOPlayerInput::ValidateStepStartInputFact(Original->Edge, Fact, OutError);
}

FGGYGOAbilityStepInputRouter::FGGYGOAbilityStepInputRouter(UGGYGOAbilitySystemComponent& InASC)
	: ASC(InASC)
{
}

FGGYGOAbilityStepInputRouter::FScopedDispatch::FScopedDispatch(FGGYGOAbilityStepInputRouter& InRouter,
	const FDispatch& InDispatch)
	: Router(InRouter), Dispatch(InDispatch), Previous(InRouter.ActiveDispatch)
{
	Router.ActiveDispatch = &Dispatch;
}

FGGYGOAbilityStepInputRouter::FScopedDispatch::~FScopedDispatch()
{
	if (Router.ActiveDispatch == &Dispatch)
	{
		Router.ActiveDispatch = Previous;
	}
}

FGGYGOAbilityInitialInputRoute FGGYGOAbilityStepInputRouter::CaptureInitialInputRoute(
	UGGYGOGameplayAbility* Ability, const FGGYGOAbilityActivationHandle& Original, FString& OutError)
{
	OutError.Reset();
	FGGYGOAbilityInitialInputRoute Route;
	if (!IsInGameThread() || !IsValid(&ASC) || !IsValid(Ability)
		|| Ability->GetAbilitySystemComponentFromActorInfo() != &ASC || !Original.HasActivation()
		|| !Ability->IsActivationCurrent(Original))
	{ OutError = TEXT("Initial 取消路由缺少原 ASC/激活来源。"); return Route; }
	InitialInputRoutes.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
	for (const auto& Weak : InitialInputRoutes)
	{
		const auto Existing = Weak.Pin();
		if (Existing.IsValid() && Existing->Ability.Get() == Ability && Existing->Activation.HasSameActivation(Original))
		{ Route.Proof = Existing; return Route; }
	}
	Route.Proof = MakeShared<FGGYGOAbilityInitialInputRoute::FRouteProof>();
	Route.Proof->ASC = &ASC; Route.Proof->Ability = Ability; Route.Proof->Activation = Original;
	Route.Proof->Spec = Ability->GetCurrentAbilitySpecHandle();
	Route.Proof->ActivationKey = Ability->GetCurrentActivationInfo().GetActivationPredictionKey();
	Route.Proof->bPredictingOwner = Ability->IsPredictingClient()
		&& Ability->GetNetExecutionPolicy() == EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	InitialInputRoutes.Add(Route.Proof);
	return Route;
}

bool FGGYGOAbilityStepInputRouter::CheckInitialInputRoute(const FGGYGOAbilityInitialInputRoute& Route, FString& OutError) const
{
	OutError.Reset();
	if (!IsInGameThread()) { OutError = TEXT("Initial 路由只能在 GameThread 核验。"); return false; }
	const auto Origin = Route.Proof;
	UGGYGOGameplayAbility* Ability = Origin.IsValid() ? Origin->Ability.Get() : nullptr;
	if (!Origin.IsValid() || Origin->ASC.Get() != &ASC || !IsValid(Ability)
		|| Ability->GetCurrentAbilitySpecHandle() != Origin->Spec
		|| Ability->GetCurrentActivationInfo().GetActivationPredictionKey() != Origin->ActivationKey)
	{ OutError = TEXT("Initial 路由不再属于原活动实例/激活；不借后继发送或消费。"); return false; }
	const bool bLiveOriginal = Ability->IsActivationCurrent(Origin->Activation);
	// 结束期间禁止新的业务准入；原激活自己的清理仍可归还它的路由。
	const bool bOriginalCleanup = Ability->bEndingActivation
		&& Ability->CurrentActivation.HasSameActivation(Origin->Activation);
	if (!bLiveOriginal && !bOriginalCleanup)
	{ OutError = TEXT("Initial 路由既非原活动来源，也非本原 Cleanup；不向后继归还。"); return false; }
	return true;
}

FGGYGOAbilityInitialInputRoute FGGYGOAbilityStepInputRouter::FindInitialInputRoute(
	FGameplayAbilitySpecHandle Spec, FPredictionKey Key) const
{
	FGGYGOAbilityInitialInputRoute Result;
	for (const auto& Weak : InitialInputRoutes)
	{
		FGGYGOAbilityInitialInputRoute Candidate;
		Candidate.Proof = Weak.Pin();
		FString Error;
		if (Candidate.HasRoute() && Candidate.Proof->Spec == Spec && Candidate.Proof->ActivationKey == Key
			&& CheckInitialInputRoute(Candidate, Error))
		{
			if (Result.HasRoute()) return {};
			Result = Candidate;
		}
	}
	return Result;
}

void FGGYGOAbilityStepInputRouter::NotifyInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route)
{
	const auto Origin = Route.Proof;
	if (!Origin.IsValid() || !Origin->bPredictingOwner
		|| Origin->State != FGGYGOAbilityInitialInputRoute::FRouteProof::EState::Pending) return;
	FString Error;
	if (!CheckInitialInputRoute(Route, Error))
	{ UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("InitialUnavailable 原路由发送拒绝：%s"), *Error); return; }
	FScopedPredictionWindow Prediction(&ASC, true);
	if (!CheckInitialInputRoute(Route, Error)
		|| Origin->State != FGGYGOAbilityInitialInputRoute::FRouteProof::EState::Pending) return;
	Origin->State = FGGYGOAbilityInitialInputRoute::FRouteProof::EState::Unavailable;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[AbilitySystem.OwnedStepInput] Boundary=InitialUnavailableSend ASC='%s' Spec=%s ActivationKey=%s ScopedKey=%s"),
		*ASC.GetPathName(), *Origin->Spec.ToString(), *Origin->ActivationKey.ToString(), *ASC.ScopedPredictionKey.ToString());
	ASC.ServerSetReplicatedTargetDataCancelled(Origin->Spec, Origin->ActivationKey, ASC.ScopedPredictionKey);
}

bool FGGYGOAbilityStepInputRouter::ConsumeInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route,
	const FGGYGOAbilityActivationHandle& Original)
{
	const auto Origin = Route.Proof;
	FString Error;
	if (!Origin.IsValid() || Origin->bPredictingOwner || !Origin->Activation.HasSameActivation(Original)
		|| Origin->State != FGGYGOAbilityInitialInputRoute::FRouteProof::EState::Unavailable
		|| !CheckInitialInputRoute(Route, Error)) return false;
	const auto Cached = ASC.AbilityTargetDataMap.Find(FGameplayAbilitySpecHandleAndPredictionKey(Origin->Spec, Origin->ActivationKey));
	if (!Cached.IsValid() || Cached != Origin->CancellationCache.Pin() || !Cached->bTargetCancelled) return false;
	Origin->State = FGGYGOAbilityInitialInputRoute::FRouteProof::EState::ConsumedUnavailable;
	ASC.ConsumeClientReplicatedTargetData(Origin->Spec, Origin->ActivationKey);
	return true;
}

void FGGYGOAbilityStepInputRouter::HandleServerTargetDataCancelled(
	FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey, FPredictionKey CurrentPredictionKey)
{
	auto Route = FindInitialInputRoute(AbilityHandle, AbilityOriginalPredictionKey);
	if (!Route.HasRoute())
	{
		ASC.ForwardNativeTargetDataCancelled(AbilityHandle, AbilityOriginalPredictionKey, CurrentPredictionKey);
		return;
	}
	DispatchInitialInputUnavailable(Route, CurrentPredictionKey, TEXT("InitialUnavailableRPC"));
}

void FGGYGOAbilityStepInputRouter::DispatchInitialInputUnavailable(const FGGYGOAbilityInitialInputRoute& Route,
	FPredictionKey CurrentPredictionKey, const TCHAR* Boundary)
{
	const auto Origin = Route.Proof;
	FString Error;
	if (!CheckInitialInputRoute(Route, Error)) return;
	if (Origin->State != FGGYGOAbilityInitialInputRoute::FRouteProof::EState::Pending) return;
	// 已提交 Initial 不进 native Reset；普通取消政策及已起段权限保持原样。
	if (!ASC.GetOwnerActor() || !ASC.GetOwnerActor()->HasAuthority() || !ASC.GetOwner() || !ASC.GetOwner()->GetNetConnection()) return;
	ASC.AbilityTargetDataCancelledDelegate(Origin->Spec, Origin->ActivationKey);
	Origin->CancellationCache = ASC.AbilityTargetDataMap.Find(FGameplayAbilitySpecHandleAndPredictionKey(Origin->Spec, Origin->ActivationKey));
	Origin->State = FGGYGOAbilityInitialInputRoute::FRouteProof::EState::Unavailable;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[AbilitySystem.OwnedStepInput] Boundary=%s ASC='%s' Spec=%s ActivationKey=%s"),
		Boundary, *ASC.GetPathName(), *Origin->Spec.ToString(), *Origin->ActivationKey.ToString());
	ASC.ForwardNativeTargetDataCancelled(Origin->Spec, Origin->ActivationKey, CurrentPredictionKey);
}

FGGYGOAbilityStepInputRequest FGGYGOAbilityStepInputRouter::MakeStepInputRequest(
	UGGYGOGameplayAbility* Ability, UGGYGOAbilityTask_WaitComboInput* Task,
	EGGYGOAbilityStepInputKind Kind, int32 SourceStep, int32 RequestId, bool bRemote, FString& OutError)
{
	OutError.Reset();
	FGGYGOAbilityStepInputRequest Result;
	if (!IsInGameThread()) { OutError = TEXT("步骤输入来源只能在 GameThread 捕获。"); return Result; }
	APawn* Avatar = Cast<APawn>(ASC.GetAvatarActor());
	APlayerController* PC = Avatar ? Cast<APlayerController>(Avatar->GetController()) : nullptr;
	UGGYGOCharacterMovementComponent* Movement = Avatar ? Cast<UGGYGOCharacterMovementComponent>(Avatar->GetMovementComponent()) : nullptr;
	const auto Activation = IsValid(Ability) ? Ability->CaptureCurrentActivation() : FGGYGOAbilityActivationHandle{};
	if (!IsValid(&ASC) || !IsValid(Ability) || !Ability->IsActive()
		|| Ability->GetAbilitySystemComponentFromActorInfo() != &ASC || !Activation.HasActivation()
		|| !IsValid(Avatar) || !IsValid(PC) || PC->GetPawn() != Avatar || !IsValid(Movement)
		|| Movement->GetOwner() != Avatar
		|| Avatar->GetWorld() != ASC.GetWorld()
		|| ASC.GetAvatarActor() != Avatar
		|| (bRemote ? (!Avatar->HasAuthority() || Avatar->IsLocallyControlled()
			|| !PC->GetNetConnection() || !ASC.GetOwner() || ASC.GetOwner()->GetNetConnection() != PC->GetNetConnection())
			: !Avatar->IsLocallyControlled()))
	{
		OutError = TEXT("步骤输入缺少原 owned ASC/活动激活/Avatar/Controller/绑定来源。");
		return Result;
	}
	FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(Ability->GetCurrentAbilitySpecHandle());
	if (!Spec || !Spec->IsActive() || !Spec->GetAbilityInstances().Contains(Ability))
	{ OutError = TEXT("步骤输入的原 Spec/活动实例不匹配。"); return Result; }
	Result.Proof = MakeShared<FGGYGOAbilityStepInputRequest::FRequestProof>();
	auto& Proof = *Result.Proof;
	Proof.ASC = &ASC; Proof.Ability = Ability; Proof.Task = Task; Proof.Avatar = Avatar;
	Proof.Controller = PC; Proof.Connection = PC->GetNetConnection(); Proof.Movement = Movement; Proof.World = ASC.GetWorld();
	Proof.Activation = Activation;
	Proof.Spec = Ability->GetCurrentAbilitySpecHandle();
	Proof.ActivationKey = Ability->GetCurrentActivationInfo().GetActivationPredictionKey();
	Proof.Kind = Kind; Proof.SourceStep = SourceStep; Proof.RequestId = RequestId; Proof.bRemote = bRemote;
	return Result;
}

FGGYGOAbilityStepInputRequest FGGYGOAbilityStepInputRouter::CaptureStepInputRequest(
	UGGYGOAbilityTask_WaitComboInput* Task, UGGYGOGameplayAbility* Ability,
	EGGYGOAbilityStepInputKind Kind, int32 SourceStep, int32 RequestId, FString& OutError)
{
	OutError.Reset();
	const FDispatch* Dispatch = ActiveDispatch;
	if (!IsValid(Task) || !IsValid(Ability) || !Dispatch || Dispatch->Spec != Ability->GetCurrentAbilitySpecHandle()
		|| Dispatch->bInitial != (Kind == EGGYGOAbilityStepInputKind::Initial)
		|| Dispatch->InputRevision != ASC.InputBuffer->GetInputRevision()
		|| (Kind == EGGYGOAbilityStepInputKind::Continue && !Dispatch->bDelivering) || Dispatch->bClaimed
		|| !Dispatch->OriginalEvaluationToken.IsSet())
	{ OutError = TEXT("步骤请求没有本次原 Process dispatch 的真实 edge 来源。"); return {}; }
	const auto Edge = Dispatch->OriginalEvaluationToken;
	if (!ASC.InputBuffer->HasLiveRequestForEdge(Edge, Dispatch->Spec))
	{ OutError = TEXT("原 Press 来源已退休，不能借仍有效的输入评价重建请求。"); return {}; }
	Dispatch->bClaimed = true;
	if (UGGYGOPlayerInput::QueryMovementInputEvaluationToken(Edge, OutError) == EGGYGOMovementInputEvaluationState::Invalidated)
		return {};
	FGGYGOAbilityStepInputRequest Result = MakeStepInputRequest(Ability, Task, Kind, SourceStep, RequestId, false, OutError);
	if (!Result.HasRequest()) return Result;
	const auto Session = Edge.GetSourceSession();
	if (!Session.Producer.IsValid() || Session.SessionSerial == 0
		|| Result.Proof->Controller->PlayerInput != Session.Producer.Get())
	{ OutError = TEXT("原 edge Producer/Session 与该 ASC 的 Controller 不匹配。"); return {}; }
	Result.Proof->Edge = Edge;
	return Result;
}

bool FGGYGOAbilityStepInputRouter::CheckStepInputRequestCurrent(
	const FGGYGOAbilityStepInputRequest& Request, FString& OutError) const
{
	OutError.Reset();
	if (!IsInGameThread()) { OutError = TEXT("步骤输入来源只能在 GameThread 核验。"); return false; }
	const auto Original = Request.Proof;
	UGGYGOGameplayAbility* Ability = Original.IsValid() ? Original->Ability.Get() : nullptr;
	APawn* Avatar = Original.IsValid() ? Original->Avatar.Get() : nullptr;
	APlayerController* PC = Original.IsValid() ? Original->Controller.Get() : nullptr;
	const FGameplayAbilitySpec* Spec = Original.IsValid() ? ASC.FindAbilitySpecFromHandle(Original->Spec) : nullptr;
	if (!IsValid(&ASC) || !Original.IsValid() || Original->ASC.Get() != &ASC
		|| !IsValid(Ability) || !Ability->IsActive() || !Spec || !Spec->IsActive()
		|| !Spec->GetAbilityInstances().Contains(Ability)
		|| !Ability->CaptureCurrentActivation().HasSameActivation(Original->Activation)
		|| Ability->GetCurrentAbilitySpecHandle() != Original->Spec
		|| Ability->GetCurrentActivationInfo().GetActivationPredictionKey() != Original->ActivationKey
		|| ASC.GetAvatarActor() != Avatar || !IsValid(Avatar) || !IsValid(PC)
		|| Avatar->GetController() != PC || PC->GetPawn() != Avatar
		|| Avatar->GetMovementComponent() != Original->Movement.Get() || !Original->Movement.IsValid()
		|| !Original->World.IsValid() || ASC.GetWorld() != Original->World.Get()
		|| (Original->bRemote ? (!Avatar->HasAuthority() || Avatar->IsLocallyControlled()
				|| !Original->Connection.IsValid() || PC->GetNetConnection() != Original->Connection.Get()
				|| !ASC.GetOwner() || ASC.GetOwner()->GetNetConnection() != Original->Connection.Get())
			: (!Avatar->IsLocallyControlled() || !Original->Task.IsValid()
				|| !Original->Task->IsActive()
				|| Original->Edge.GetSourceSession().Producer.Get() != PC->PlayerInput)))
	{
		OutError = TEXT("原步骤输入的激活/owned 端点/绑定来源已失效。");
		return false;
	}
	return true;
}

bool FGGYGOAbilityStepInputRouter::CheckStepInputRequestForTask(const FGGYGOAbilityStepInputRequest& Request,
	const UGameplayAbility* Ability, const FGGYGOAbilityActivationHandle& Activation, FString& OutError) const
{
	if (!CheckStepInputRequestCurrent(Request, OutError)) return false;
	if (Request.Proof->Ability.Get() != Ability || !Request.Proof->Activation.HasSameActivation(Activation))
	{ OutError = TEXT("owned 请求不属于该 Task 创建时的原活动实例/激活。"); return false; }
	return true;
}

bool FGGYGOAbilityStepInputRouter::CommitStepInputFact(UGGYGOAbilityTask_WaitComboInput* Task,
	const FGGYGOAbilityStepInputRequest& Request, const FGGYGOMovementInputStepFact& Fact,
	const FGGYGOAbilityInitialInputRoute& Route, FString& OutError)
{
	if (!CheckStepInputRequestCurrent(Request, OutError)) return false;
	const auto Original = Request.Proof;
	if (!CheckInitialInputRoute(Route, OutError)) return false;
	if (Route.Proof->Ability != Original->Ability
		|| !Route.Proof->Activation.HasSameActivation(Original->Activation))
	{ OutError = TEXT("步骤事实与 Initial 路由不属于同一原激活。"); return false; }
	using EInitial = FGGYGOAbilityInitialInputRoute::FRouteProof::EState;
	const bool bInitial = Original->Kind == EGGYGOAbilityStepInputKind::Initial;
	if (Route.Proof->State != (bInitial ? EInitial::Pending : EInitial::Submitted))
	{ OutError = TEXT("Initial 已不可提供，或 Continue 没有原已提交 Initial。"); return false; }
	if (Original->bRemote || Original->Task.Get() != Task || Original->EntryFact.IsValid())
	{ OutError = TEXT("原步骤事实已提交或不属于该本地 Task。"); return false; }
	FGGYGOAbilityStepInputTargetData Shape;
	Shape.Kind = Original->Kind; Shape.SourceStep = Original->SourceStep; Shape.RequestId = Original->RequestId; Shape.StepFact = Fact;
	if (!Shape.HasValidFields() || !UGGYGOPlayerInput::ValidateStepStartInputFact(Original->Edge, Fact, OutError))
	{ if (OutError.IsEmpty()) OutError = TEXT("原步骤事实字段不合法。"); return false; }
	FGGYGOMovementOwnerSyncScopeId Scope;
	if (!Original->Movement->GetMovementOwnerSyncScope(Scope, OutError)) return false;
	Original->EntryScope = Scope;
	Original->EntryFrame = GFrameCounter;
	Original->EntryFact = MakeShared<const FGGYGOMovementInputStepFact>(Fact);
	if (bInitial) Route.Proof->State = EInitial::Submitted;
	return true;
}

FGGYGOAbilityStepInputRequest FGGYGOAbilityStepInputRouter::WrapRemoteStepInputRequest(
	FGameplayAbilitySpecHandle SpecHandle, FPredictionKey ActivationKey, EGGYGOAbilityStepInputKind Kind,
	int32 SourceStep, int32 RequestId, const FGGYGOMovementInputStepFact& Fact, FString& OutError)
{
	FGameplayAbilitySpec* Spec = ASC.FindAbilitySpecFromHandle(SpecHandle);
	if (!Spec || !Spec->IsActive()) { OutError = TEXT("远端步骤的原 Spec 已不活动。"); return {}; }
	UGGYGOGameplayAbility* MatchingAbility = nullptr;
	for (UGameplayAbility* Instance : Spec->GetAbilityInstances())
	{
		UGGYGOGameplayAbility* Ability = Cast<UGGYGOGameplayAbility>(Instance);
		if (!Ability || !Ability->IsActive() || Ability->GetCurrentAbilitySpecHandle() != SpecHandle
			|| Ability->GetCurrentActivationInfo().GetActivationPredictionKey() != ActivationKey) continue;
		if (MatchingAbility) { OutError = TEXT("远端 Spec/激活预测键不能唯一定位活动实例。"); return {}; }
		MatchingAbility = Ability;
	}
	if (!MatchingAbility) { OutError = TEXT("远端步骤没有匹配的原活动实例/激活预测键。"); return {}; }
	const auto Route = FindInitialInputRoute(SpecHandle, ActivationKey);
	using EInitial = FGGYGOAbilityInitialInputRoute::FRouteProof::EState;
	if (!Route.HasRoute() || Route.Proof->State != (Kind == EGGYGOAbilityStepInputKind::Initial
		? EInitial::Pending : EInitial::Submitted))
	{ OutError = TEXT("远端没有原 Initial 路由，或该 Initial 已终止/尚未提交。"); return {}; }
	auto Request = MakeStepInputRequest(MatchingAbility, nullptr, Kind, SourceStep, RequestId, true, OutError);
	if (!Request.HasRequest()) return {};
	// 入站只认证输入来源。Scope 若尚未建立，由 GA/CMC 的真实准入明确等待或拒绝，不能丢掉唯一 Initial。
	FString ScopeError;
	Request.Proof->Movement->GetMovementOwnerSyncScope(Request.Proof->EntryScope, ScopeError);
	Request.Proof->EntryFact = MakeShared<const FGGYGOMovementInputStepFact>(Fact);
	if (Kind == EGGYGOAbilityStepInputKind::Initial) Route.Proof->State = EInitial::Submitted;
	return Request;
}

void FGGYGOAbilityStepInputRouter::HandleServerTargetData(
	FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey,
	const FGameplayAbilityTargetDataHandle& TargetData, FGameplayTag ApplicationTag, FPredictionKey CurrentPredictionKey)
{
	const FGameplayAbilityTargetData* First = TargetData.Get(0);
	if (!First || First->GetScriptStruct() != FGGYGOAbilityStepInputTargetData::StaticStruct())
	{
		ASC.ForwardNativeTargetData(AbilityHandle, AbilityOriginalPredictionKey,
			TargetData, ApplicationTag, CurrentPredictionKey);
		return;
	}
	const auto Raw = *static_cast<const FGGYGOAbilityStepInputTargetData*>(First);
	FString Error;
	if (TargetData.Num() != 1 || ApplicationTag.IsValid() || !Raw.HasValidFields())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("OwnedStepInput 入站字段拒绝：ASC=%s Spec=%s ActivationKey=%s Num=%d Tag=%s Kind=%u SourceStep=%d Request=%d。"),
			*ASC.GetPathName(), *AbilityHandle.ToString(), *AbilityOriginalPredictionKey.ToString(), TargetData.Num(),
			*ApplicationTag.ToString(), static_cast<uint32>(Raw.Kind), Raw.SourceStep, Raw.RequestId);
		return;
	}
	auto Request = WrapRemoteStepInputRequest(AbilityHandle, AbilityOriginalPredictionKey,
		Raw.Kind, Raw.SourceStep, Raw.RequestId, Raw.StepFact, Error);
	if (!Request.HasRequest())
	{
		UE_LOG(LogGGYGOAbilitySystem, Warning,
			TEXT("OwnedStepInput 入站来源拒绝：ASC=%s Spec=%s ActivationKey=%s Kind=%u Request=%d 原因=%s。"),
			*ASC.GetPathName(), *AbilityHandle.ToString(), *AbilityOriginalPredictionKey.ToString(), static_cast<uint32>(Raw.Kind), Raw.RequestId, *Error);
		if (Raw.Kind == EGGYGOAbilityStepInputKind::Initial)
		{
			const auto Pending = FindInitialInputRoute(AbilityHandle, AbilityOriginalPredictionKey);
			if (Pending.HasRoute()) DispatchInitialInputUnavailable(Pending, CurrentPredictionKey, TEXT("InitialSourceRejected"));
		}
		return;
	}
	auto* Owned = new FGGYGOAbilityStepInputTargetData(Raw);
	Owned->OwnedRequest = Request;
	const FGameplayAbilityTargetDataHandle OwnedData(Owned);
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[AbilitySystem.OwnedStepInput] Boundary=RPCImplementation ASC='%s' World='%s' Spec=%s ActivationKey=%s ScopedKey=%s Kind=%u SourceStep=%d Request=%d Edge=%llu Entry=%llu Frame=%llu"),
		*ASC.GetPathName(), *GetPathNameSafe(ASC.GetWorld()), *AbilityHandle.ToString(), *AbilityOriginalPredictionKey.ToString(),
		*CurrentPredictionKey.ToString(), static_cast<uint32>(Raw.Kind), Raw.SourceStep, Raw.RequestId,
		static_cast<unsigned long long>(Raw.StepFact.OriginalEdgeEvaluationSerial),
		static_cast<unsigned long long>(Raw.StepFact.StepEvaluationSerial), static_cast<unsigned long long>(GFrameCounter));
	ASC.ForwardNativeTargetData(AbilityHandle, AbilityOriginalPredictionKey,
		OwnedData, ApplicationTag, CurrentPredictionKey);
}
