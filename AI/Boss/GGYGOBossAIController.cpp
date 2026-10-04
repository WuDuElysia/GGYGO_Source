/** @file GGYGOBossAIController.cpp */
#include "AI/Boss/GGYGOBossAIController.h"

#include "AI/Boss/GGYGOBossDefinition.h"
#include "AI/Boss/GGYGOBossActionSet.h"
#include "AI/Boss/GGYGOBossState.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/Abilities/GGYGOCombatActionAbility.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BrainComponent.h"
#include "GameplayAbilitySpec.h"
#include "Templates/UnrealTemplate.h"

#include <cmath>

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOBossAIController)

AGGYGOBossAIController::AGGYGOBossAIController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	bWantsPlayerState = false;

	// 形态交接只暂停 Brain，不能走 AAIController 默认的 Cleanup；否则黑板与 BT 实例会被重建。
	bStopAILogicOnUnposses = false;
	bStartAILogicOnPossess = true;
	DecisionRandom.Initialize(0);
}

void AGGYGOBossAIController::InitializeDecisionStream(int32 EncounterSeed)
{
	ClearActionSelection();
	DecisionRandom.Initialize(EncounterSeed);
	RuntimeActionWeights.Reset();
	WeightSource.Reset();
}

struct AGGYGOBossAIController::FInitialBossPossessContext
{
	TWeakObjectPtr<APawn> Avatar;
	TWeakObjectPtr<AGGYGOBossState> State;
	TWeakObjectPtr<const UGGYGOBossDefinition> Definition;
	TWeakObjectPtr<UBehaviorTree> RequestedTree;
	TWeakObjectPtr<UBehaviorTreeComponent> StartedBrain;
	FString Failure;
	EInitialBossPossessResult Result = EInitialBossPossessResult::Failed;
	bool bRequiresTree = false;
	bool bClaimed = false;
	bool bRetired = false;
	bool bNativeRunAccepted = false;
};

void AGGYGOBossAIController::RetireInitialPossess(const TCHAR* Reason)
{
	if (ActiveInitialPossess)
	{
		ActiveInitialPossess->bRetired = true;
		if (ActiveInitialPossess->Failure.IsEmpty())
		{
			ActiveInitialPossess->Failure = Reason;
		}
	}
}

bool AGGYGOBossAIController::IsInitialPossessContextCurrent(const FInitialBossPossessContext& Context) const
{
	APawn* Avatar = Context.Avatar.Get();
	AGGYGOBossState* State = Context.State.Get();
	const UGGYGOBossDefinition* Definition = Context.Definition.Get();
	if (ActiveInitialPossess != &Context || !Context.bClaimed || Context.bRetired
		|| !IsValid(this) || IsActorBeingDestroyed() || !HasAuthority()
		|| !IsValid(Avatar) || Avatar->IsActorBeingDestroyed()
		|| !IsValid(State) || State->IsActorBeingDestroyed() || !IsValid(Definition))
	{
		return false;
	}

	const IAbilitySystemInterface* AbilityInterface = Cast<IAbilitySystemInterface>(Avatar);
	const UAbilitySystemComponent* ASC = AbilityInterface ? AbilityInterface->GetAbilitySystemComponent() : nullptr;
	// The interface query is virtual. A reentrant possession cannot complete this old receipt.
	return ActiveInitialPossess == &Context && !Context.bRetired
		&& IsValid(this) && !IsActorBeingDestroyed() && HasAuthority()
		&& Context.Avatar.Get() == Avatar && Context.State.Get() == State && Context.Definition.Get() == Definition
		&& IsValid(Avatar) && !Avatar->IsActorBeingDestroyed()
		&& IsValid(State) && !State->IsActorBeingDestroyed() && IsValid(Definition)
		&& GetPawn() == Avatar && Avatar->GetController() == this && BossState == State
		&& State->GetAvatarPawn() == Avatar && State->GetBossDefinition() == Definition
		&& IsValid(ASC) && ASC->GetOwnerActor() == State && ASC->GetAvatarActor() == Avatar
		&& (Context.bRequiresTree ? IsValid(Context.RequestedTree.Get())
			&& Definition->BehaviorTree == Context.RequestedTree.Get() : !Definition->BehaviorTree);
}

bool AGGYGOBossAIController::IsInitialPossessTreeCurrent(const FInitialBossPossessContext& Context) const
{
	const UBehaviorTreeComponent* BT = Context.StartedBrain.Get();
	return Context.bRequiresTree && IsValid(Context.RequestedTree.Get())
		&& Context.bNativeRunAccepted && IsValid(BT) && GetBrainComponent() == BT
		&& BT->GetOwner() == this && BT->GetAIOwner() == this
		&& BT->IsRegistered() && BT->HasBeenInitialized()
		&& BT->GetRootTree() == Context.RequestedTree.Get()
		&& BT->TreeHasBeenStarted() && BT->IsRunning();
}

EInitialBossPossessResult AGGYGOBossAIController::PossessInitialBoss(APawn* ExpectedAvatar,
	AGGYGOBossState* ExpectedState, const UGGYGOBossDefinition* ExpectedDefinition, FString& OutFailure)
{
	OutFailure.Reset();
	if (ActiveInitialPossess)
	{
		RetireInitialPossess(TEXT("初始 Possess 发生重入请求，原调用已退休。"));
		OutFailure = TEXT("同一 Controller 已有初始 Possess 调用。");
		return EInitialBossPossessResult::Failed;
	}
	if (!IsValid(this) || IsActorBeingDestroyed() || !HasAuthority()
		|| !IsValid(ExpectedAvatar) || ExpectedAvatar->IsActorBeingDestroyed()
		|| !IsValid(ExpectedState) || ExpectedState->IsActorBeingDestroyed() || !IsValid(ExpectedDefinition)
		|| GetPawn() || BossState || bBehaviorTreeStarted
		|| ExpectedState->GetAvatarPawn() != ExpectedAvatar || ExpectedState->GetBossDefinition() != ExpectedDefinition)
	{
		OutFailure = TEXT("需要有效权威、尚未附身的初始 Controller，以及原 State/Avatar/Definition 装配。");
		return EInitialBossPossessResult::Failed;
	}

	FInitialBossPossessContext Context;
	Context.Avatar = ExpectedAvatar;
	Context.State = ExpectedState;
	Context.Definition = ExpectedDefinition;
	Context.bRequiresTree = ExpectedDefinition->BehaviorTree != nullptr;
	Context.RequestedTree = ExpectedDefinition->BehaviorTree;
	TGuardValue<FInitialBossPossessContext*> ContextGuard(ActiveInitialPossess, &Context);
	Possess(ExpectedAvatar);

	if (!IsInitialPossessContextCurrent(Context))
	{
		RetireInitialPossess(TEXT("原初始 Possess 未完成，或 Context/Avatar/State/Definition/配置树已变化。"));
	}
	else if (Context.Result == EInitialBossPossessResult::TreeStarted && !IsInitialPossessTreeCurrent(Context))
	{
		RetireInitialPossess(TEXT("原配置树返回时已不再是同一有效、已启动且运行中的原生 BT。"));
	}
	if (Context.bRetired || Context.Result == EInitialBossPossessResult::Failed)
	{
		OutFailure = Context.Failure.IsEmpty() ? TEXT("原初始 Possess 未产生有效启动结果。") : Context.Failure;
		return EInitialBossPossessResult::Failed;
	}
	return Context.Result;
}

float AGGYGOBossAIController::GetActionWeight(const FGGYGOBossActionDefinition& Action) const
{
	if (!std::isfinite(static_cast<double>(Action.BaseWeight)) || Action.BaseWeight < 0.0f)
	{
		return 0.0f;
	}

	const float UpperBound = FMath::Max(Action.MaxWeight, Action.BaseWeight);
	if (!std::isfinite(static_cast<double>(UpperBound)) || UpperBound < 0.0f)
	{
		return Action.BaseWeight;
	}
	const double MaxWeight = static_cast<double>(UpperBound);

	if (const float* RuntimeWeight = RuntimeActionWeights.Find(Action.ActionTag))
	{
		if (!std::isfinite(static_cast<double>(*RuntimeWeight)))
		{
			return 0.0f;
		}
		return static_cast<float>(FMath::Clamp(static_cast<double>(*RuntimeWeight), 0.0, MaxWeight));
	}
	return static_cast<float>(FMath::Clamp(static_cast<double>(Action.BaseWeight), 0.0, MaxWeight));
}

double AGGYGOBossAIController::DrawActionWeight(double TotalWeight)
{
	if (!std::isfinite(TotalWeight) || TotalWeight <= 0.0)
	{
		return 0.0;
	}
	return static_cast<double>(DecisionRandom.GetFraction()) * TotalWeight;
}

void AGGYGOBossAIController::RecordActionSelection(
	const TArray<FGGYGOBossActionDefinition>& Actions, FGameplayTag SelectedActionTag)
{
	for (const FGGYGOBossActionDefinition& Action : Actions)
	{
		if (!Action.ActionTag.IsValid() || !std::isfinite(static_cast<double>(Action.BaseWeight)) ||
			Action.BaseWeight < 0.0f || !std::isfinite(static_cast<double>(Action.MaxWeight)) ||
			Action.MaxWeight < 0.0f)
		{
			continue;
		}

		if (Action.BaseWeight == 0.0f)
		{
			RuntimeActionWeights.Add(Action.ActionTag, 0.0f);
			continue;
		}

		const float CurrentWeight = GetActionWeight(Action);
		const double UpperBound = static_cast<double>(FMath::Max(Action.MaxWeight, Action.BaseWeight));
		const double Current = static_cast<double>(CurrentWeight);
		double NewWeight = Current;
		if (Action.ActionTag == SelectedActionTag)
		{
			const double RepeatPenalty = std::isfinite(static_cast<double>(Action.RepeatPenalty))
				? FMath::Clamp(static_cast<double>(Action.RepeatPenalty), 0.0, 1.0)
				: 0.0;
			NewWeight = Current * RepeatPenalty;
		}
		else if (std::isfinite(static_cast<double>(Action.UnusedWeightGain)) && Action.UnusedWeightGain >= 0.0f)
		{
			NewWeight = Current + static_cast<double>(Action.UnusedWeightGain);
		}

		if (!std::isfinite(NewWeight) || NewWeight > UpperBound)
		{
			NewWeight = UpperBound;
		}
		RuntimeActionWeights.Add(Action.ActionTag,
			static_cast<float>(FMath::Clamp(NewWeight, 0.0, UpperBound)));
	}
}

const FGGYGOBossActionDefinition* AGGYGOBossAIController::SelectAction(
	const UGGYGOBossActionSet* ActionSet, const TArray<FGameplayTag>& EligibleActionTags)
{
	ClearActionSelection();
	if (!IsValid(ActionSet))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] 拒绝空或无效 ActionSet。"), *GetNameSafe(this));
		return nullptr;
	}

	FString ValidationError;
	if (!ActionSet->ValidateConfiguration(ValidationError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] 拒绝无效 ActionSet [%s]：%s"),
			*GetNameSafe(this), *GetNameSafe(ActionSet), *ValidationError);
		return nullptr;
	}

	if (WeightSource.Get() != ActionSet)
	{
		RuntimeActionWeights.Reset();
		WeightSource = ActionSet;
	}

	struct FWeightedCandidate
	{
		const FGGYGOBossActionDefinition* Action = nullptr;
		double Weight = 0.0;
	};

	TSet<FGameplayTag> EligibleTags;
	for (const FGameplayTag& Tag : EligibleActionTags)
	{
		if (Tag.IsValid())
		{
			EligibleTags.Add(Tag);
		}
	}

	TArray<FWeightedCandidate> Candidates;
	double MaximumWeight = 0.0;
	for (const FGGYGOBossActionDefinition& Action : ActionSet->Actions)
	{
		if (Action.BaseWeight <= 0.0f || !EligibleTags.Contains(Action.ActionTag))
		{
			continue;
		}

		const double Weight = static_cast<double>(GetActionWeight(Action));
		const double FiniteWeight = std::isfinite(Weight) && Weight > 0.0 ? Weight : 0.0;
		Candidates.Add({ &Action, FiniteWeight });
		MaximumWeight = FMath::Max(MaximumWeight, FiniteWeight);
	}

	if (Candidates.IsEmpty())
	{
		return nullptr;
	}

	if (MaximumWeight <= 0.0)
	{
		for (FWeightedCandidate& Candidate : Candidates)
		{
			Candidate.Weight = static_cast<double>(Candidate.Action->BaseWeight);
			RuntimeActionWeights.Add(Candidate.Action->ActionTag, Candidate.Action->BaseWeight);
			MaximumWeight = FMath::Max(MaximumWeight, Candidate.Weight);
		}
	}

	if (!std::isfinite(MaximumWeight) || MaximumWeight <= 0.0)
	{
		return nullptr;
	}

	// Normalize by the largest weight before summing. The sum is bounded by the candidate count,
	// so neither a large configured weight nor repeated updates can overflow the draw range.
	double TotalWeight = 0.0;
	for (const FWeightedCandidate& Candidate : Candidates)
	{
		TotalWeight += Candidate.Weight / MaximumWeight;
	}
	if (!std::isfinite(TotalWeight) || TotalWeight <= 0.0)
	{
		return nullptr;
	}

	const double Roll = DrawActionWeight(TotalWeight);
	double AccumulatedWeight = 0.0;
	const FGGYGOBossActionDefinition* Selected = Candidates.Last().Action;
	for (const FWeightedCandidate& Candidate : Candidates)
	{
		AccumulatedWeight += Candidate.Weight / MaximumWeight;
		if (Roll < AccumulatedWeight)
		{
			Selected = Candidate.Action;
			break;
		}
	}

	RecordActionSelection(ActionSet->Actions, Selected->ActionTag);
	return Selected;
}

bool AGGYGOBossAIController::StoreActionSelection(const UGGYGOBossActionSet* ActionSet,
	FGameplayTag PhaseTag, UGGYGOAbilitySystemComponent* ASC, FGameplayTag ActionTag,
	FGameplayAbilitySpecHandle SpecHandle)
{
	ClearActionSelection();
	if (!IsValid(ActionSet) || !IsValid(ASC) || !PhaseTag.IsValid() || !ActionTag.IsValid() ||
		!SpecHandle.IsValid())
	{
		return false;
	}

	FString ValidationError;
	if (!ActionSet->ValidateConfiguration(ValidationError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] 无法保存选择：ActionSet [%s] 无效：%s"),
			*GetNameSafe(this), *GetNameSafe(ActionSet), *ValidationError);
		return false;
	}

	const FGGYGOBossActionDefinition* Action = ActionSet->FindAction(ActionTag);
	UClass* AbilityClass = Action ? Action->AbilityClass.Get() : nullptr;
	AActor* Avatar = ASC->GetAvatarActor();
	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(SpecHandle);
	if (!Action || Action->BaseWeight <= 0.0f || !IsValid(AbilityClass) || !IsValid(Avatar) || !Spec || !Spec->Ability ||
		Spec->Ability->GetClass() != AbilityClass)
	{
		return false;
	}

	const UGGYGOCombatActionAbility* AbilityCDO =
		AbilityClass->GetDefaultObject<UGGYGOCombatActionAbility>();
	if (!AbilityCDO || AbilityCDO->GetActionTag() != ActionTag)
	{
		return false;
	}

	PendingActionSelection.ActionSet = ActionSet;
	PendingActionSelection.ASC = ASC;
	PendingActionSelection.AbilityClass = AbilityClass;
	PendingActionSelection.Avatar = Avatar;
	PendingActionSelection.PhaseTag = PhaseTag;
	PendingActionSelection.ActionTag = ActionTag;
	PendingActionSelection.SpecHandle = SpecHandle;
	return true;
}

bool AGGYGOBossAIController::ConsumeActionSelection(const UGGYGOBossActionSet* ActionSet,
	FGameplayTag PhaseTag, UGGYGOAbilitySystemComponent* ASC, FGameplayTag ActionTag,
	FGameplayAbilitySpecHandle& OutSpecHandle)
{
	OutSpecHandle = FGameplayAbilitySpecHandle();
	const FPendingActionSelection Selection = PendingActionSelection;
	ClearActionSelection();

	const UGGYGOBossActionSet* RecordedSet = Selection.ActionSet.Get();
	UGGYGOAbilitySystemComponent* RecordedASC = Selection.ASC.Get();
	UClass* RecordedClass = Selection.AbilityClass.Get();
	AActor* RecordedAvatar = Selection.Avatar.Get();
	if (!IsValid(ActionSet))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] 消费选择失败：当前 ActionSet 为空或无效。"), *GetNameSafe(this));
		return false;
	}
	if (!IsValid(ASC) || !IsValid(RecordedSet) || !IsValid(RecordedASC) ||
		!IsValid(RecordedClass) || !IsValid(RecordedAvatar) || RecordedSet != ActionSet ||
		RecordedASC != ASC || Selection.PhaseTag != PhaseTag || Selection.ActionTag != ActionTag ||
		!Selection.SpecHandle.IsValid())
	{
		return false;
	}

	FString ValidationError;
	if (!ActionSet->ValidateConfiguration(ValidationError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] 消费选择失败：ActionSet [%s] 无效：%s"),
			*GetNameSafe(this), *GetNameSafe(ActionSet), *ValidationError);
		return false;
	}

	const FGGYGOBossActionDefinition* Action = ActionSet->FindAction(ActionTag);
	UClass* CurrentClass = Action ? Action->AbilityClass.Get() : nullptr;
	AActor* CurrentAvatar = ASC->GetAvatarActor();
	const FGameplayAbilitySpec* Spec = ASC->FindAbilitySpecFromHandle(Selection.SpecHandle);
	if (!Action || Action->BaseWeight <= 0.0f || !IsValid(CurrentClass) || CurrentClass != RecordedClass ||
		!IsValid(CurrentAvatar) || CurrentAvatar != RecordedAvatar ||
		!Spec || !Spec->Ability || Spec->Ability->GetClass() != RecordedClass)
	{
		return false;
	}

	const UGGYGOCombatActionAbility* AbilityCDO =
		CurrentClass->GetDefaultObject<UGGYGOCombatActionAbility>();
	if (!AbilityCDO || AbilityCDO->GetActionTag() != ActionTag)
	{
		return false;
	}

	OutSpecHandle = Selection.SpecHandle;
	return true;
}

void AGGYGOBossAIController::ClearActionSelection()
{
	PendingActionSelection = FPendingActionSelection();
}

void AGGYGOBossAIController::OnPossess(APawn* InPawn)
{
	FInitialBossPossessContext* InitialContext = nullptr;
	if (ActiveInitialPossess)
	{
		if (!ActiveInitialPossess->bClaimed && !ActiveInitialPossess->bRetired
			&& ActiveInitialPossess->Avatar.Get() == InPawn)
		{
			// Claim before Super: nested OnPossess cannot write the original call's result.
			ActiveInitialPossess->bClaimed = true;
			InitialContext = ActiveInitialPossess;
		}
		else
		{
			RetireInitialPossess(TEXT("初始 Possess 内进入了后继 OnPossess，原调用已退休。"));
		}
	}
	ClearActionSelection();
	Super::OnPossess(InPawn);
	if (InitialContext && (InitialContext->bRetired || ActiveInitialPossess != InitialContext
		|| !IsValid(this) || IsActorBeingDestroyed() || !IsValid(InPawn)
		|| InPawn->IsActorBeingDestroyed() || GetPawn() != InitialContext->Avatar.Get()))
	{
		RetireInitialPossess(TEXT("原初始 OnPossess 在 Super 调用中失效。"));
		return;
	}

	AGGYGOBossState* ResolvedState = nullptr;
	if (const IAbilitySystemInterface* AbilityInterface = Cast<IAbilitySystemInterface>(InPawn))
	{
		UAbilitySystemComponent* ASC = AbilityInterface->GetAbilitySystemComponent();
		if (InitialContext && (InitialContext->bRetired || ActiveInitialPossess != InitialContext
			|| !IsValid(this) || IsActorBeingDestroyed() || !IsValid(InPawn) || InPawn->IsActorBeingDestroyed()
			|| InitialContext->Avatar.Get() != InPawn || GetPawn() != InPawn || !IsValid(ASC)))
		{
			RetireInitialPossess(TEXT("原初始 OnPossess 在 Avatar ASC 查询中失效，或 ASC 无效。"));
			return;
		}
		if (ASC)
		{
			ResolvedState = Cast<AGGYGOBossState>(ASC->GetOwnerActor());
		}
	}

	if (!ResolvedState)
	{
		if (InitialContext) { RetireInitialPossess(TEXT("原 Avatar 的 ASC Owner 不能解析为 BossState。")); }
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("BossAIController [%s] Possess [%s] 时未找到 BossState ASC Owner。"),
			*GetNameSafe(this), *GetNameSafe(InPawn));
		return;
	}

	if (InitialContext && (InitialContext->bRetired || ActiveInitialPossess != InitialContext
		|| !IsValid(this) || IsActorBeingDestroyed() || !IsValid(InPawn) || InPawn->IsActorBeingDestroyed()
		|| !IsValid(ResolvedState) || ResolvedState->IsActorBeingDestroyed()
		|| GetPawn() != InitialContext->Avatar.Get() || ResolvedState != InitialContext->State.Get()
		|| ResolvedState->GetBossDefinition() != InitialContext->Definition.Get()))
	{
		RetireInitialPossess(TEXT("原初始 OnPossess 的 Avatar/ASC Owner/Definition 已变化。"));
		return;
	}
	BossState = ResolvedState;
	const UGGYGOBossDefinition* Definition = BossState->GetBossDefinition();
	if (InitialContext && !IsInitialPossessContextCurrent(*InitialContext))
	{
		RetireInitialPossess(TEXT("原初始 OnPossess 的装配或配置树已变化。"));
		return;
	}
	if (!bBehaviorTreeStarted && Definition && Definition->BehaviorTree)
	{
		UBehaviorTree* RequestedTree = Definition->BehaviorTree;
		const bool bNativeAccepted = RunBehaviorTree(RequestedTree);
		if (InitialContext)
		{
			InitialContext->bNativeRunAccepted = bNativeAccepted;
			// Capture the returned native instance before the virtual ASC query can reenter.
			InitialContext->StartedBrain = Cast<UBehaviorTreeComponent>(GetBrainComponent());
			if (!IsInitialPossessContextCurrent(*InitialContext))
			{
				RetireInitialPossess(TEXT("原初始 OnPossess 在 RunBehaviorTree 调用中失效。"));
				return;
			}
		}
		bBehaviorTreeStarted = bNativeAccepted;
		if (!bNativeAccepted || (InitialContext && !IsInitialPossessTreeCurrent(*InitialContext)))
		{
			if (InitialContext)
			{
				RetireInitialPossess(bNativeAccepted
					? TEXT("原生调用已接收，但原配置树未同步建立有效的 Started/Running BT 实例。")
					: TEXT("原生 RunBehaviorTree 拒绝了原配置树启动。"));
			}
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("BossAIController [%s] 无法启动行为树 [%s]，Definition [%s]。%s"),
				*GetNameSafe(this), *GetNameSafe(RequestedTree), *GetNameSafe(Definition),
				InitialContext ? *InitialContext->Failure : TEXT("原生 RunBehaviorTree 拒绝启动。"));
		}
		else
		{
			if (InitialContext) { InitialContext->Result = EInitialBossPossessResult::TreeStarted; }
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("BossAIController [%s] 已启动行为树 [%s]。"),
				*GetNameSafe(this), *GetNameSafe(RequestedTree));
		}
	}
	else if (bBehaviorTreeStarted)
	{
		if (InitialContext)
		{
			RetireInitialPossess(TEXT("原初始装配不能借用旧的行为树启动记录或 Resume 结果。"));
			return;
		}
		if (UBrainComponent* Brain = GetBrainComponent())
		{
			Brain->ResumeLogic(TEXT("Boss Avatar ready"));
		}
	}
	else if (InitialContext && !InitialContext->bRequiresTree)
	{
		InitialContext->Result = EInitialBossPossessResult::AssemblyOnly;
	}
}

void AGGYGOBossAIController::OnUnPossess()
{
	RetireInitialPossess(TEXT("初始 Possess 内发生 UnPossess，原调用已退休。"));
	ClearActionSelection();
	if (UBrainComponent* Brain = GetBrainComponent())
	{
		Brain->PauseLogic(TEXT("Boss Avatar handoff"));
	}

	// BossState 与 Brain 都保留；阶段 E 的新形态 Possess 后继续使用同一 Controller。
	Super::OnUnPossess();
}

void AGGYGOBossAIController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RetireInitialPossess(TEXT("初始 Possess 内发生 EndPlay，原调用已退休。"));
	ClearActionSelection();
	Super::EndPlay(EndPlayReason);
}
