/** @file GGYGOPlayerComboAbility.cpp @brief 玩家连段原播放、动作及逐段表现资源生命周期 */
#include "AbilitySystem/Abilities/GGYGOPlayerComboAbility.h"

#include "AbilitySystem/Abilities/GGYGOComboCorrection.h"
#include "AbilitySystem/Abilities/GGYGOMontageActionResources.h"
#include "AbilitySystem/Executions/GGYGODamageExecution.h"
#include "AbilitySystem/GGYGOAbilitySystemComponent.h"
#include "AbilitySystem/GGYGOAbilitySystemLog.h"
#include "AbilitySystem/GGYGOAbilityInputRequestTypes.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_PlayMontageAndWaitForEvent.h"
#include "AbilitySystem/Tasks/GGYGOAbilityTask_WaitComboInput.h"
#include "Animation/Data/GGYGOActionMotionSourceBinding.h"
#include "Character/Components/GGYGOCharacterMovementComponent.h"
#include "Character/Data/GGYGOActionMotionEvaluation.h"
#include "Combat/HitDetection/GGYGOMeleeTraceComponent.h"
#include "FX/Tasks/GGYGOAbilityTask_PlayNiagaraEffect.h"
#include "Input/GGYGOPlayerInput.h"
#include "System/GGYGOGameData.h"
#include "System/GGYGOGameplayTags.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayEffect.h"
#include "NiagaraSystem.h"
#include "TimerManager.h"
#if WITH_EDITOR
#include "Misc/DataValidation.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(GGYGOPlayerComboAbility)

namespace
{
	bool IsConfiguredStepFacingPolicy(EGGYGOComboStepFacingPolicy Policy)
	{
		return Policy == EGGYGOComboStepFacingPolicy::CurrentFacing
			|| Policy == EGGYGOComboStepFacingPolicy::QualifiedInput;
	}

	enum class EGGYGOComboDamageMode : uint8 { NoGE, Absolute, Multiplier };

	// 模式来自实际GE的原生Execution配置；只读栈值，不保存第二份玩法状态。
	bool ResolveGGYGOComboDamageMode(const UGameplayEffect* Effect, EGGYGOComboDamageMode& OutMode, FString& OutError)
	{
		OutMode = EGGYGOComboDamageMode::NoGE;
		if (!Effect) { return true; }
		bool bMultiplier = false;
		bool bAbsoluteExecution = false;
		for (const FGameplayEffectExecutionDefinition& Execution : Effect->Executions)
		{
			const UClass* Calculation = Execution.CalculationClass.Get();
			const bool bDamageExecution = Calculation && Calculation->IsChildOf(UGGYGODamageExecution::StaticClass());
			const bool bTagged = Execution.PassedInTags.HasTagExact(GGYGOGameplayTags::DamageCalculation_AttackPowerMultiplier);
			if (bTagged && !bDamageExecution)
			{
				OutError = TEXT("ComboDamage: multiplier-mode-requires-DamageExecution");
				return false;
			}
			bMultiplier |= bTagged;
			bAbsoluteExecution |= bDamageExecution && !bTagged;
		}
		if (bMultiplier && bAbsoluteExecution)
		{
			OutError = TEXT("ComboDamage: conflicting-execution-damage-modes");
			return false;
		}
		OutMode = bMultiplier ? EGGYGOComboDamageMode::Multiplier : EGGYGOComboDamageMode::Absolute;
		return true;
	}

	// 段数值的有限／非负已由 PrepareComboSteps 校验；这里只核对GE决定的模式与各段倍率配置的关系，
	// 成功时输出该模式，命中只读取这一结果。
	bool ValidateGGYGOComboDamageModes(const TArray<FGGYGOComboStep>& Steps,
		const UGameplayEffect* Effect, EGGYGOComboDamageMode& OutMode, FString& OutError)
	{
		if (!ResolveGGYGOComboDamageMode(Effect, OutMode, OutError)) { return false; }
		for (int32 Index = 0; Index < Steps.Num(); ++Index)
		{
			if (OutMode == EGGYGOComboDamageMode::Multiplier && Steps[Index].DamageMultiplier == -1.0f)
			{
				OutError = TEXT("ComboDamage: required-multiplier-missing-or-invalid");
			}
			else if (OutMode == EGGYGOComboDamageMode::Absolute && Steps[Index].DamageMultiplier != -1.0f)
			{
				OutError = TEXT("ComboDamage: absolute-mode-requires-explicit-legacy-amount-and-unset-multiplier");
			}
			else { continue; }
			OutError += FString::Printf(TEXT("; Step=%d GE=%s"), Index, *GetPathNameSafe(Effect));
			return false;
		}
		return true;
	}

	const TCHAR* DescribeQualifiedMovementResult(EGGYGOQualifiedMovementIntentQueryResult Result)
	{
		switch (Result)
		{
		case EGGYGOQualifiedMovementIntentQueryResult::Unavailable: return TEXT("Unavailable");
		case EGGYGOQualifiedMovementIntentQueryResult::Qualified: return TEXT("Qualified");
		case EGGYGOQualifiedMovementIntentQueryResult::NotHeld: return TEXT("NotHeld");
		case EGGYGOQualifiedMovementIntentQueryResult::AwaitingPhysicalProof: return TEXT("AwaitingPhysicalProof");
		case EGGYGOQualifiedMovementIntentQueryResult::WaitingForAdmission: return TEXT("WaitingForAdmission");
		case EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed: return TEXT("ExecutionFailed");
		default: return TEXT("UnsupportedQueryValue");
		}
	}

	bool ResolveStepInterruption(const FGGYGOComboStep& Step, float RangeStart, float RangeEnd,
		FGGYGOMontageNotifyDeclaration& OutDeclaration, FString& OutError)
	{
		OutDeclaration = {};
		OutError.Reset();
		if (Step.InterruptionMode == EGGYGOActionInterruptionMode::Disabled)
		{
			if (!Step.InterruptionNotifyName.IsNone() || Step.NextStepIndex != INDEX_NONE)
			{
				OutError = TEXT("Disabled 打断模式必须显式关闭原生通知名称和攻击接续。");
				return false;
			}
			return true;
		}
		if (Step.InterruptionMode != EGGYGOActionInterruptionMode::AuthoredNotify)
		{
			OutError = TEXT("打断模式必须明确选择 AuthoredNotify 或 Disabled。");
			return false;
		}
		FGGYGOMontageNotifyDeclaration Declaration;
		if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
			Step.Montage, Step.InterruptionNotifyName, Declaration, OutError)) { return false; }
		const float Position = Declaration.TriggerPositionSeconds;
		if (Position < RangeStart || Position >= RangeEnd)
		{
			OutError = TEXT("作者打断点不在实际绑定可达范围内。");
			return false;
		}
		OutDeclaration = Declaration;
		return true;
	}

	/** 当前 Montage 的 Main 真实末端：按本段 EndSection 名解析的原生 CompositeSection::GetTime()。
	 *  不从 source 段、帧或容差推导；原 source 绑定只作一致性佐证，Main/End 区间声明与之不一致时拒绝。 */
	bool ResolveMainEndFloor(const FGGYGOComboStep& Step, const FGGYGOActionMotionSourceBinding& Source,
		float& OutFloorPosition, FString& OutError)
	{
		OutFloorPosition = 0.0f;
		OutError.Reset();
		const UAnimMontage* const Montage = Step.Montage;
		if (!Montage || Source.Montage.Get() != Montage)
		{
			OutError = TEXT("当前 Montage 缺失或不是原 source 绑定的 Montage，无法读取 Main 末端。");
			return false;
		}
		const int32 MainIndex = Montage->GetSectionIndex(Step.MainSection);
		const int32 EndIndex = Montage->GetSectionIndex(Step.EndSection);
		if (MainIndex == INDEX_NONE || EndIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("当前 Montage 缺少配置的 Main [%s] 或 End [%s] Section。"),
				*Step.MainSection.ToString(), *Step.EndSection.ToString());
			return false;
		}
		const float MainStart = Montage->GetAnimCompositeSection(MainIndex).GetTime();
		const float EndStart = Montage->GetAnimCompositeSection(EndIndex).GetTime();
		const float MontageLength = Montage->GetPlayLength();
		if (!FMath::IsFinite(MainStart) || !FMath::IsFinite(EndStart) || !FMath::IsFinite(MontageLength))
		{
			OutError = TEXT("当前 Montage 的 Main/End Section 时间或时长非有限。");
			return false;
		}
		if (!(MainStart < EndStart) || !(EndStart < MontageLength))
		{
			OutError = FString::Printf(TEXT("当前 Montage Section 顺序无效：Main %.9g、End %.9g、Length %.9g。"),
				MainStart, EndStart, MontageLength);
			return false;
		}
		const FGGYGOActionMotionSourceSection* const MainSource = Source.FindSection(Step.MainSection);
		const FGGYGOActionMotionSourceSection* const EndSource = Source.FindSection(Step.EndSection);
		if (!MainSource || !EndSource || MainSource->SectionIndex != MainIndex || EndSource->SectionIndex != EndIndex
			|| MainSource->MontageStartSeconds != MainStart || MainSource->MontageEndSeconds != EndStart
			|| EndSource->MontageStartSeconds != EndStart)
		{
			OutError = FString::Printf(TEXT("原 source 绑定的 Main/End 区间声明与当前 Montage Section（Main %.9g、End %.9g）不一致。"),
				MainStart, EndStart);
			return false;
		}
		OutFloorPosition = EndStart;
		return true;
	}

	/** 移动打断点的静态声明核对；开门下限为 ResolveMainEndFloor 读出的当前 Montage Main 真实末端。
	 *  原 Task 认证 stock BranchingPoint 声明；作者时间必须精确等于 Main 末端，合法 OffsetBefore 不提前开门。 */
	bool ResolveStepMovementInterruption(const FGGYGOComboStep& Step, const FGGYGOActionMotionSourceBinding& Source,
		FGGYGOMontageNotifyDeclaration& OutDeclaration, float& OutFloorPosition, FString& OutError)
	{
		OutDeclaration = {};
		OutFloorPosition = 0.0f;
		OutError.Reset();
		if (Step.MovementInterruptionMode == EGGYGOActionInterruptionMode::Disabled)
		{
			if (!Step.MovementInterruptionNotifyName.IsNone())
			{
				OutError = TEXT("移动打断 Disabled 模式必须显式填 None，不保留未使用的通知名称。");
				return false;
			}
			return true;
		}
		if (Step.MovementInterruptionMode != EGGYGOActionInterruptionMode::AuthoredNotify)
		{
			OutError = TEXT("移动打断模式必须明确选择 AuthoredNotify 或 Disabled。");
			return false;
		}
		if (Step.MovementInterruptionNotifyName.IsNone())
		{
			OutError = TEXT("AuthoredNotify 移动打断必须配置原生 Montage Notify 名称。");
			return false;
		}
		if (Step.MovementInterruptionNotifyName == Step.InterruptionNotifyName)
		{
			OutError = TEXT("移动打断点必须与攻击接续点不同名；一个名字不能同时授予两种权限。");
			return false;
		}
		float MainEndFloor = 0.0f;
		if (!ResolveMainEndFloor(Step, Source, MainEndFloor, OutError)) { return false; }
		FGGYGOMontageNotifyDeclaration Declaration;
		if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageNotifyDeclaration(
			Step.Montage, Step.MovementInterruptionNotifyName, Declaration, OutError)) { return false; }
		if (Declaration.AuthoredPositionSeconds != MainEndFloor
			|| Declaration.TriggerPositionSeconds < Source.MontageStartSeconds
			|| Declaration.TriggerPositionSeconds > MainEndFloor)
		{
			OutError = FString::Printf(TEXT("移动点作者时间 %.9g 必须精确等于当前 Montage Main 末端 %.9g，实际 trigger %.9g 必须在原 Main 范围 [%.9g,%.9g] 内。"),
				Declaration.AuthoredPositionSeconds, MainEndFloor, Declaration.TriggerPositionSeconds,
				Source.MontageStartSeconds, MainEndFloor);
			return false;
		}
		OutDeclaration = Declaration;
		OutFloorPosition = MainEndFloor;
		return true;
	}

	bool ValidateStepFXConfiguration(const FGGYGOComboStep& Step, float RangeStart, float RangeEnd,
		FString& OutError, FGGYGOMontageFXWindowDeclaration* OutWindow = nullptr)
	{
		return FGGYGOMontageActionResources::ValidateFXConfiguration(Step.Montage, Step.FX,
			RangeStart, RangeEnd, OutError, OutWindow);
	}

	/** 订阅词汇不表示资产必有窗口；声明仍只来自实际 Montage。 */
	FGameplayTagContainer GetStepWindowTags(const FGGYGOComboStep& Step)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(GGYGOGameplayTags::Event_Montage_ComboWindowBegin);
		Tags.AddTag(GGYGOGameplayTags::Event_Montage_ComboWindowEnd);
		Tags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowBegin);
		Tags.AddTag(GGYGOGameplayTags::Event_Montage_HitWindowEnd);
		if (Step.FX.Mode == EGGYGOComboFXMode::MontageWindow)
		{
			Tags.AddTag(Step.FX.BeginEventTag);
			Tags.AddTag(Step.FX.EndEventTag);
		}
		return Tags;
	}
}

/** 本段一个作者点权限。名字与触发位置来自本段配置和原 Task 的同次认证；
 *  Pending 只记录已认证原实例事实，不等于开门。两门互不读写。 */
struct UGGYGOPlayerComboAbility::FAuthorPointGate
{
	EGGYGOActionInterruptionMode Mode = EGGYGOActionInterruptionMode::Disabled;
	FName NotifyName;
	/** 原 Task 认证的原生触发时间（含 TriggerTimeOffset）；只用于核对 Fact／快照身份。 */
	float TriggerPosition = 0.0f;
	float AuthoredPosition = 0.0f;
	float TriggerTimeOffset = 0.0f;
	/** 开门所需原实例真实位置下限：攻击为其触发时间，移动为当前 Montage End Section::GetTime（Main 真实末端）。 */
	float OpenFloorPosition = 0.0f;
	int32 PendingInstanceId = INDEX_NONE;
	bool bOpen = false;

	bool IsAuthored() const { return Mode == EGGYGOActionInterruptionMode::AuthoredNotify; }
	void Close() { bOpen = false; PendingInstanceId = INDEX_NONE; }
};

struct FGGYGOPlayerComboObservation::FState
{
	FGGYGOPlayerComboObservationFacts Facts;
};

bool FGGYGOPlayerComboObservation::ReadHistory(FGGYGOPlayerComboObservationFacts& OutFacts) const
{
	check(IsInGameThread());
	OutFacts = {};
	if (!History.IsValid()) { return false; }
	OutFacts = History->Facts;
	return true;
}

/** GA owns these resources; section interpretation is derived only from its original Task. */
struct UGGYGOPlayerComboAbility::FStepMotionResources : FGGYGOMontageActionResources
{
	enum class ESection : uint8 { AwaitingSnapshot, Main, End, Retired };
	uint64 StepToken = 0;
	int32 StepIndex = INDEX_NONE;
	/** 请求策略随本段固定；EntryActorRotation 只来自原 CMC 成功接受值或权威重基。 */
	FGGYGOActionMotionStartFacing StartFacing;
	FQuat EntryActorRotation = FQuat::Identity;
	bool bHasEntryActorRotation = false;
	/** 仅移动门持有的原 CMC 订阅；攻击门从不订阅。 */
	FGGYGOQualifiedMovementIntentObserverId Observer;
	ESection Section = ESection::AwaitingSnapshot;
	FName EndSection;
	FAuthorPointGate AttackGate;
	FAuthorPointGate MovementGate;
	/** 仅关联当前 Owned Trace 的原作者窗口；不授予播放或命中权限。 */
	FGGYGOMontageWindowDeclaration TraceWindowSource;
	/** 仅为显式观察创建；不参与任何执行或完成判定，也不延长原资源／对象生命周期。 */
	TSharedPtr<FGGYGOPlayerComboObservation::FState> Observation;

	void Release(EGGYGOActionMotionReleaseReason Reason)
	{
		Section = ESection::Retired;
		UnregisterTaskCallbacks();
		AttackGate.Close();
		MovementGate.Close();
		TraceWindowSource = {};
		const auto OriginalObserver = Observer;
		Observer = {};
		FString Error;
		if (Movement.IsValid() && OriginalObserver.IsSet()
			&& !Movement->UnsubscribeQualifiedMovementIntent(OriginalObserver, TEXT("PlayerCombo.ResourceReleased"), Error))
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original movement observer cleanup failed: %s"), *AbilityPath, *Error);
		}
		ReleaseMotion(Reason);
	}
};

/** 原 press/激活请求的有限入场资源；不持方向值，不在新评价到来时改写原 edge。 */
struct UGGYGOPlayerComboAbility::FStepEntryAttempt
{
	enum class EPhase : uint8 { AwaitingInput, SamplingRejected, Starting, Retired };
	FGGYGOAbilityActivationHandle Activation;
	FGGYGOAbilityStepInputRequest InputRequest;
	TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> InputTask;
	TWeakObjectPtr<UWorld> World;
	TWeakPtr<FStepMotionResources> EnteringMotion;
	FDelegateHandle EvaluationSubscription;
	/** 首次准入检查捕获的原 CMC Scope；等待期间只比对，不借后继 Scope 续接。 */
	FGGYGOMovementOwnerSyncScopeId Scope;
	/** 原 press 处实际段的身份；拒绝请求的 SourceStep 可能不同，二者不互相改签。 */
	uint64 PreviousStepToken = 0;
	int32 PreviousStep = INDEX_NONE;
	int32 TargetStep = INDEX_NONE;
	bool bGateAccepted = false;
	EPhase Phase = EPhase::AwaitingInput;
};

/** 一段的静态声明，来自 PrepareComboSteps 对作者配置的同一次解析；运行期只读。 */
struct UGGYGOPlayerComboAbility::FPreparedComboStep
{
	FGGYGOActionMotionSourceBindingPtr MotionSource;
	FGGYGOMontageNotifyDeclaration AttackDeclaration;
	FGGYGOMontageNotifyDeclaration MovementDeclaration;
	/** 移动门开门下限：当前 Montage Main 真实末端。 */
	float MovementFloor = 0.0f;
	FGGYGOMontageFXWindowDeclaration FXWindow;
	TArray<FGGYGOMontageWindowDeclaration> WindowDeclarations;
};

/** 授予期预备配置：全部段的静态声明与伤害 GE／模式，来自同一次作者配置解析；只读。
 *  GE 以弱引用保存，不延长类生命周期；类卸载或被新版本替换（编辑器重编译）即视为失效。 */
struct UGGYGOPlayerComboAbility::FPreparedComboConfiguration
{
	TArray<FPreparedComboStep> Steps;
	TWeakObjectPtr<UClass> DamageEffect;
	bool bHasDamageEffect = false;
	EGGYGOComboDamageMode DamageMode = EGGYGOComboDamageMode::NoGE;

	/** 无 GE 模式恒为 true；必需 GE 须仍是同一个未被替换的类。 */
	bool IsDamageEffectCurrent() const
	{
		const UClass* const Class = DamageEffect.Get();
		return !bHasDamageEffect || (Class && !Class->HasAnyClassFlags(CLASS_NewerVersionExists));
	}
};

/** StartStep 本栈的输入副本，只在三步之间传递。持有预备配置的共享引用，
 *  外调期间终止清理释放成员也不会让本段读到失效数据。 */
struct UGGYGOPlayerComboAbility::FStepStartInputs
{
	TSharedPtr<const FPreparedComboConfiguration> Prepared;
	const FPreparedComboStep* PreparedStep = nullptr;
	FGGYGOComboStep Step;
	UGGYGOCharacterMovementComponent* Movement = nullptr;
	FQuat ReplayBasis = FQuat::Identity;
	bool bHasReplayBasis = false;
};

bool UGGYGOPlayerComboAbility::IsStepEntryCurrent(const TSharedPtr<FStepEntryAttempt>& Attempt) const
{
	if (!Attempt.IsValid() || !StepEntryAttempts.Contains(Attempt) || Attempt->Phase == FStepEntryAttempt::EPhase::Retired
		|| !IsActivationCurrent(Attempt->Activation) || !Attempt->InputTask.IsValid()
		|| InputTask != Attempt->InputTask.Get() || !Attempt->World.IsValid() || Attempt->World != OriginalWorld)
	{
		return false;
	}
	if (Attempt->Phase == FStepEntryAttempt::EPhase::Starting)
	{
		const auto Motion = Attempt->EnteringMotion.Pin();
		return IsMotionResourceCurrent(Motion) && Motion->StepIndex == Attempt->TargetStep;
	}
	return CurrentStep == Attempt->PreviousStep && CurrentStepToken == Attempt->PreviousStepToken;
}

void UGGYGOPlayerComboAbility::ReleaseStepEntry(const TSharedPtr<FStepEntryAttempt>& Attempt)
{
	if (!Attempt.IsValid() || StepEntryAttempts.RemoveSingle(Attempt) == 0) { return; }
	Attempt->Phase = FStepEntryAttempt::EPhase::Retired;
	UGGYGOPlayerInput::UnsubscribeMovementInputEvaluation(
		Attempt->InputRequest.GetOriginalEdgeToken(), Attempt->EvaluationSubscription);
}

void UGGYGOPlayerComboAbility::ReleasePendingStepEntries()
{
	const auto Originals = MoveTemp(StepEntryAttempts);
	for (const auto& Original : Originals) { Original->Phase = FStepEntryAttempt::EPhase::Retired; }
	for (const auto& Original : Originals)
	{
		UGGYGOPlayerInput::UnsubscribeMovementInputEvaluation(
			Original->InputRequest.GetOriginalEdgeToken(), Original->EvaluationSubscription);
	}
}

void UGGYGOPlayerComboAbility::FailStepEntry(const TSharedPtr<FStepEntryAttempt>& Attempt, const FString& Diagnostic)
{
	if (!IsStepEntryCurrent(Attempt)) { return; }
	const FGGYGOAbilityActivationHandle Original = Attempt->Activation;
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("PlayerCombo [%s] 原入场请求失败：Kind=%u SourceStep=%d Request=%d，%s"),
		*GetPathName(), static_cast<uint32>(Attempt->InputRequest.GetKind()),
		Attempt->InputRequest.GetSourceStep(), Attempt->InputRequest.GetRequestId(), *Diagnostic);
	ReleaseStepEntry(Attempt);
	if (IsActivationCurrent(Original)) { RequestAbilityEnd(Original, true, true); }
}

void UGGYGOPlayerComboAbility::ResumeStepEntry(const TSharedPtr<FStepEntryAttempt>& Attempt)
{
	if (!IsStepEntryCurrent(Attempt) || Attempt->Phase != FStepEntryAttempt::EPhase::AwaitingInput) { return; }
	const TWeakObjectPtr<UGGYGOCharacterMovementComponent> Movement(Cast<UGGYGOCharacterMovementComponent>(
		GetCharacterFromActorInfo() ? GetCharacterFromActorInfo()->GetCharacterMovement() : nullptr));
	FGGYGOMovementOwnerSyncScopeId Scope;
	FString Error;
	if (!Movement.IsValid() || !Movement->GetMovementOwnerSyncScope(Scope, Error)
		|| !Attempt->InputRequest.IsCurrentFor(Movement.Get(), Scope, Error))
	{
		FailStepEntry(Attempt, FString::Printf(TEXT("原输入请求/CMC/Scope 不可用：%s"), *Error));
		return;
	}
	if (!Attempt->Scope.IsSet())
	{
		Attempt->Scope = Scope;
	}
	else if (Attempt->Scope != Scope)
	{
		FailStepEntry(Attempt, TEXT("原 CMC Scope 已退役；等待请求不能改签后继 Scope。"));
		return;
	}
	using EState = EGGYGOMovementInputEvaluationState;
	// 此处只核 readiness。Axis 与当前 ControlYaw 留在 Task Ready 等外调之后的 CMC 真准入处。
	const EState State = Attempt->InputRequest.QueryStepStartInputReadiness(Error);
	if (State == EState::Invalidated)
	{
		FailStepEntry(Attempt, FString::Printf(TEXT("原段首输入评价已失效：%s"), *Error));
		return;
	}
	if (State == EState::Waiting)
	{
		if (Attempt->EvaluationSubscription.IsValid()) { return; }
		const TWeakObjectPtr<ThisClass> WeakThis(this);
		const TWeakPtr<FStepEntryAttempt> WeakAttempt(Attempt);
		const bool bSubscribed = UGGYGOPlayerInput::SubscribeMovementInputEvaluation(
			Attempt->InputRequest.GetOriginalEdgeToken(), FGGYGOMovementInputEvaluationDelegate::CreateLambda(
				[WeakThis, WeakAttempt](EState, const FString&)
				{
					ThisClass* Self = WeakThis.Get();
					const auto OriginalAttempt = WeakAttempt.Pin();
					if (Self) { Self->ResumeStepEntry(OriginalAttempt); }
				}), Attempt->EvaluationSubscription, Error);
		// OutHandle 在同步回放前已安装；回放可完成或退役原尝试，返栈不写后继资源。
		if (!IsStepEntryCurrent(Attempt)) { return; }
		if (!bSubscribed || !Attempt->EvaluationSubscription.IsValid())
		{
			FailStepEntry(Attempt, FString::Printf(TEXT("原评价完成订阅失败：%s"), *Error));
		}
		return;
	}
	UGGYGOPlayerInput::UnsubscribeMovementInputEvaluation(
		Attempt->InputRequest.GetOriginalEdgeToken(), Attempt->EvaluationSubscription);
	if (!IsStepEntryCurrent(Attempt)) { return; }
	if (!Attempt->bGateAccepted)
	{
		// 原 press 的门资格已定。仍可靠提交真实 attempt，Authority 可按自己的原门裁决。
		Attempt->Phase = FStepEntryAttempt::EPhase::SamplingRejected;
		FGGYGOActionMotionStartFacing Facing;
		Facing.Scope = Scope;
		Facing.InputRequest = Attempt->InputRequest;
		FGGYGOActionMotionStartInputProof Proof;
		const EState Prepared = Movement->PrepareActionMotionStartInput(Facing, Proof, Error);
		if (!IsStepEntryCurrent(Attempt)) { return; }
		if (Prepared != EState::Completed)
		{
			FailStepEntry(Attempt, FString::Printf(TEXT("原拒绝请求的真实输入提交失败：%s"), *Error));
			return;
		}
		const FGGYGOAbilityActivationHandle Original = Attempt->Activation;
		const int32 RequestId = Attempt->InputRequest.GetRequestId();
		ReleaseStepEntry(Attempt);
		if (IsActivationCurrent(Original)) { RejectRequest(Original, RequestId); }
		return;
	}
	Attempt->Phase = FStepEntryAttempt::EPhase::Starting;
	const bool bStarted = StartStep(Attempt->Activation, Attempt->TargetStep, 0.0f, Attempt->InputRequest.GetRequestId(),
		false, nullptr, Attempt);
	if (!StepEntryAttempts.Contains(Attempt)) { return; }
	const bool bOriginalStarted = bStarted && IsStepEntryCurrent(Attempt);
	ReleaseStepEntry(Attempt);
	if (bOriginalStarted && Attempt->InputRequest.GetKind() == EGGYGOAbilityStepInputKind::Continue)
	{
		SendAuthoritativeStep(Attempt->Activation, Attempt->InputRequest.GetRequestId(), true);
	}
}

UGGYGOAbilityTask_PlayMontageAndWaitForEvent* UGGYGOPlayerComboAbility::GetMontageTask() const
{ return StepMotionResources.IsValid() ? StepMotionResources->GetTask() : nullptr; }
UGGYGOAbilityTask_PlayNiagaraEffect* UGGYGOPlayerComboAbility::GetStepFXTask() const
{ return StepMotionResources.IsValid() ? StepMotionResources->GetFXTask() : nullptr; }
bool UGGYGOPlayerComboAbility::HasMontageCallbackRegistration() const
{ return StepMotionResources.IsValid() && StepMotionResources->HasTaskRegistration(); }
FTimerHandle UGGYGOPlayerComboAbility::GetWatchdogHandle() const
{ return StepMotionResources.IsValid() ? StepMotionResources->GetWatchdogHandle() : FTimerHandle{}; }

UGGYGOPlayerComboAbility::UGGYGOPlayerComboAbility(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupTag = GGYGOGameplayTags::AbilityGroup_Attack;
	ActivationPriority = GGYGOAbilityGroupDefaults::Priority_LightAttack;
	SelfPolicy = EGGYGOAbilitySelfPolicy::Coexist;
	ActivationOwnedTags.AddTag(GGYGOGameplayTags::State_Attacking);
	ActivationBlockedTags.AddTag(GGYGOGameplayTags::State_Dead);
	ActivationBlockedTags.AddTag(GGYGOGameplayTags::Restriction_CantAttack);
	// 保留旧 CDO 默认存值供资产迁移读取；各段运行配置独立，不以此值回退。
	HitCueTag = GGYGOGameplayTags::GameplayCue_Hit_Flesh;
	bServerRespectsRemoteAbilityCancellation = false;
}

bool UGGYGOPlayerComboAbility::IsStepPlayable(int32 Index) const
{
	if (!ComboSteps.IsValidIndex(Index)) { return false; }
	const FGGYGOComboStep& Step = ComboSteps[Index];
	if (!Step.Montage || !FMath::IsFinite(Step.PlayRate) || Step.PlayRate <= 0.0f
		|| Step.MainSection == Step.EndSection || Step.MotionSlotName.IsNone()
		|| !FMath::IsFinite(Step.MotionTranslationScale) || Step.MotionTranslationScale <= 0.0f
		|| !IsConfiguredStepFacingPolicy(Step.StartFacingPolicy)) { return false; }
	const int32 MainIndex = Step.Montage->GetSectionIndex(Step.MainSection);
	const int32 EndIndex = Step.Montage->GetSectionIndex(Step.EndSection);
	return MainIndex != INDEX_NONE && EndIndex != INDEX_NONE
		&& Step.Montage->GetAnimCompositeSection(MainIndex).GetTime() < Step.Montage->GetAnimCompositeSection(EndIndex).GetTime()
		&& Step.Montage->GetAnimCompositeSection(EndIndex).GetTime() < Step.Montage->GetPlayLength();
}

bool UGGYGOPlayerComboAbility::ValidateComboConfiguration(FString& OutError) const
{
	return PrepareComboSteps(OutError).IsValid();
}

TSharedPtr<UGGYGOPlayerComboAbility::FPreparedComboConfiguration> UGGYGOPlayerComboAbility::PrepareComboSteps(
	FString& OutError) const
{
	OutError.Reset();
	if (ComboSteps.IsEmpty()) { OutError = TEXT("ComboSteps 为空。"); return nullptr; }
	if (!FMath::IsFinite(MontageCompletionGraceSeconds) || MontageCompletionGraceSeconds < 0.0f)
	{
		OutError = TEXT("MontageCompletionGraceSeconds 必须是非负有限秒数。");
		return nullptr;
	}
	const TSharedRef<FPreparedComboConfiguration> Prepared = MakeShared<FPreparedComboConfiguration>();
	Prepared->Steps.Reserve(ComboSteps.Num());
	for (int32 Index = 0; Index < ComboSteps.Num(); ++Index)
	{
		const FGGYGOComboStep& Step = ComboSteps[Index];
		if (!IsConfiguredStepFacingPolicy(Step.StartFacingPolicy))
		{
			OutError = FString::Printf(TEXT("段 %d 的 StartFacingPolicy 无效，须显式选择 CurrentFacing 或 QualifiedInput。"), Index);
			return nullptr;
		}
		if (Step.Montage && (!Step.Montage->bEnableAutoBlendOut
			|| !FMath::IsFinite(Step.Montage->BlendOutTriggerTime)
			|| !FMath::IsFinite(Step.Montage->GetDefaultBlendOutTime()) || Step.Montage->GetDefaultBlendOutTime() < 0.0f))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 必须启用自然混出，触发时间必须有限，混出时长必须有限且非负。"),
				Index, *GetPathNameSafe(Step.Montage));
			return nullptr;
		}
		if (Step.MotionSlotName.IsNone() || !FMath::IsFinite(Step.MotionTranslationScale) || Step.MotionTranslationScale <= 0.0f)
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 必须显式配置 MotionSlotName，MotionTranslationScale 必须有限且大于0。"),
				Index, *GetPathNameSafe(Step.Montage));
			return nullptr;
		}
		if (!IsStepPlayable(Index)
			|| !FMath::IsFinite(Step.DamageMultiplier)
			|| (Step.DamageMultiplier < 0.0f && Step.DamageMultiplier != -1.0f)
			|| (Step.DamageMultiplier == -1.0f && (!FMath::IsFinite(Step.Damage) || Step.Damage < 0.0f))
			|| !FMath::IsFinite(Step.PoiseDamage) || Step.PoiseDamage < 0.0f)
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 的 Montage/Section/速率/伤害无效。"),
				Index, *GetPathNameSafe(Step.Montage));
			return nullptr;
		}
		FString ShapeError;
		if (!UGGYGOMeleeTraceComponent::ValidateTraceShapeDefinition(Step.TraceShape, ShapeError))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 的 TraceShape 无效：%s；旧 TraceStartSocket/TraceEndSocket/TraceRadius 仅历史存值，不参与运行，请显式配置 TraceShape。"),
				Index, *GetPathNameSafe(Step.Montage), *ShapeError);
			return nullptr;
		}
		FPreparedComboStep PreparedStep;
		FString MotionError;
		if (!GGYGOActionMotionSource::BuildSourceBindingRange(Step.Montage, Step.MotionSlotName,
			Step.MainSection, Step.EndSection, PreparedStep.MotionSource, MotionError)
			|| !GGYGOActionMotionEvaluation::ValidateSource(*PreparedStep.MotionSource, MotionError))
		{
			OutError = FString::Printf(TEXT("段 %d 的原动画动作来源无效：%s"), Index, *MotionError);
			return nullptr;
		}
		const FGGYGOActionMotionSourceBinding& Source = *PreparedStep.MotionSource;
		if (Source.Sections.Num() != 2)
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 的 Main→End 必须相邻，原生默认路径不能跳过绑定内的 Section。"),
				Index, *GetPathNameSafe(Step.Montage));
			return nullptr;
		}
		FString FXError;
		if (!ValidateStepFXConfiguration(Step, Source.MontageStartSeconds, Source.MontageEndSeconds, FXError,
			&PreparedStep.FXWindow))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] FX [%s] 无效：%s"),
				Index, *GetPathNameSafe(Step.Montage), *GetPathNameSafe(Step.FX.System), *FXError);
			return nullptr;
		}
		if (!UGGYGOAbilityTask_PlayMontageAndWaitForEvent::ResolveOriginalMontageWindows(
			Step.Montage, GetStepWindowTags(Step), Source.MontageStartSeconds, Source.MontageEndSeconds,
			PreparedStep.WindowDeclarations, MotionError))
		{
			OutError = FString::Printf(TEXT("段 %d 原窗口声明无效：%s"), Index, *MotionError);
			return nullptr;
		}
		if (!ResolveStepInterruption(Step, Source.MontageStartSeconds, Source.MontageEndSeconds,
			PreparedStep.AttackDeclaration, MotionError))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 攻击接续模式 %d 与通知 [%s] 配置无效：%s"),
				Index, *GetPathNameSafe(Step.Montage), static_cast<int32>(Step.InterruptionMode),
				*Step.InterruptionNotifyName.ToString(), *MotionError);
			return nullptr;
		}
		if (!ResolveStepMovementInterruption(Step, Source, PreparedStep.MovementDeclaration,
			PreparedStep.MovementFloor, MotionError))
		{
			OutError = FString::Printf(TEXT("段 %d Montage [%s] 移动打断模式 %d 与通知 [%s] 配置无效：%s"),
				Index, *GetPathNameSafe(Step.Montage), static_cast<int32>(Step.MovementInterruptionMode),
				*Step.MovementInterruptionNotifyName.ToString(), *MotionError);
			return nullptr;
		}
		if (Step.NextStepIndex != INDEX_NONE && !ComboSteps.IsValidIndex(Step.NextStepIndex))
		{
			OutError = FString::Printf(TEXT("段 %d 的 NextStepIndex 必须是有效段或 -1（关闭信号后的攻击接续）。"), Index);
			return nullptr;
		}
		Prepared->Steps.Add(MoveTemp(PreparedStep));
	}
	return Prepared;
}

#if WITH_EDITOR
EDataValidationResult UGGYGOPlayerComboAbility::IsDataValid(FDataValidationContext& Context) const
{
	const EDataValidationResult BaseResult = Super::IsDataValid(Context);
	FString Error;
	TSubclassOf<UGameplayEffect> ExplicitEffect = DamageEffect;
	const UClass* ExplicitClass = ExplicitEffect.GetGCPtr().Get();
	if (ExplicitClass && (!IsValid(ExplicitClass) || !ExplicitClass->IsChildOf(UGameplayEffect::StaticClass())))
	{
		Context.AddError(FText::FromString(TEXT("显式伤害GE类无效。")));
		return EDataValidationResult::Invalid;
	}
	// 编辑器只检查显式GE，不读取共享加载器；CDO可能创建，数值检查放在其返回之后。
	const UGameplayEffect* Effect = ExplicitClass ? ExplicitClass->GetDefaultObject<UGameplayEffect>() : nullptr;
	EGGYGOComboDamageMode Mode;
	if (!ValidateComboConfiguration(Error)
		|| (ExplicitClass && !ValidateGGYGOComboDamageModes(ComboSteps, Effect, Mode, Error)))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return BaseResult == EDataValidationResult::Invalid ? BaseResult : EDataValidationResult::Valid;
}
#endif

bool UGGYGOPlayerComboAbility::ValidateDamageEffectDependency(FString& OutError,
	TSubclassOf<UGameplayEffect>* OutResolvedEffect) const
{
	OutError.Reset();
	if (OutResolvedEffect) { *OutResolvedEffect = TSubclassOf<UGameplayEffect>(); }
	// Read the configured class before TSubclassOf's type filter can turn an invalid selection into null.
	TSubclassOf<UGameplayEffect> ConfiguredEffect = DamageEffect;
	const UClass* ConfiguredClass = ConfiguredEffect.GetGCPtr().Get();
	const auto IsChosenClassValid = [&OutError](const UClass* Class, const TCHAR* Mode, const TCHAR* Configuration)
	{
		if (!IsValid(Class) || !Class->IsChildOf(UGameplayEffect::StaticClass()))
		{
			OutError = FString::Printf(TEXT("模式=%s，配置=%s [%s]：已选GE类失效或不是GameplayEffect。"),
				Mode, Configuration, *GetPathNameSafe(Class));
			return false;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = FString::Printf(TEXT("模式=%s，配置=%s [%s]：已选GE类为抽象、已弃用或已被新版本替换。"),
				Mode, Configuration, *GetPathNameSafe(Class));
			return false;
		}
		return true;
	};
	if (ConfiguredClass && !IsChosenClassValid(ConfiguredClass, TEXT("Override"), TEXT("DamageEffect")))
	{
		return false; // An invalid explicit selection cannot enable shared or no-GE playback.
	}

	TSubclassOf<UGameplayEffect> ResolvedEffect = UGGYGOGameData::ResolveDamageGameplayEffect(
		ConfiguredEffect, bUseSharedDamageEffectWhenUnset);
	if (!ConfiguredClass && !bUseSharedDamageEffectWhenUnset)
	{
		return true; // Explicitly selected no-GE mode; no required damage dependency.
	}
	const UClass* ResolvedClass = ResolvedEffect.GetGCPtr().Get();
	if (!ResolvedClass)
	{
		OutError = TEXT("模式=Shared，配置=GameData.DamageGameplayEffect_SetByCaller：必需的共享预载GE不可用。");
		return false;
	}
	if (!IsChosenClassValid(ResolvedClass, ConfiguredClass ? TEXT("Override") : TEXT("Shared"),
		ConfiguredClass ? TEXT("DamageEffect") : TEXT("GameData.DamageGameplayEffect_SetByCaller")))
	{
		return false;
	}
	if (OutResolvedEffect) { *OutResolvedEffect = ResolvedEffect; }
	return true;
}

TSharedPtr<const UGGYGOPlayerComboAbility::FPreparedComboConfiguration> UGGYGOPlayerComboAbility::FindOrPrepareConfiguration(
	FString& OutError, bool* bOutDamageFailure) const
{
	OutError.Reset();
	if (bOutDamageFailure) { *bOutDamageFailure = false; }
	if (PreparedConfiguration.IsValid() && PreparedConfiguration->IsDamageEffectCurrent()) { return PreparedConfiguration; }
	PreparedConfiguration.Reset();

	// 顺序决定拒绝日志分类：GE 依赖 → 段声明 → GE 模式与段倍率。共享 GE 尚未加载时失败不缓存，下次准入重试。
	TSubclassOf<UGameplayEffect> Effect;
	if (!ValidateDamageEffectDependency(OutError, &Effect))
	{
		if (bOutDamageFailure) { *bOutDamageFailure = true; }
		return nullptr;
	}
	// GetDefaultObject 可能首次创建 CDO；模式检查在其返回后进行。
	const UGameplayEffect* const Definition = Effect ? Effect->GetDefaultObject<UGameplayEffect>() : nullptr;
	const TSharedPtr<FPreparedComboConfiguration> Prepared = PrepareComboSteps(OutError);
	if (!Prepared.IsValid()) { return nullptr; }
	if (!ValidateGGYGOComboDamageModes(ComboSteps, Definition, Prepared->DamageMode, OutError))
	{
		if (bOutDamageFailure) { *bOutDamageFailure = true; }
		return nullptr;
	}
	Prepared->DamageEffect = Effect.Get();
	Prepared->bHasDamageEffect = Effect.Get() != nullptr;
	// CDO 被纯查询直接调用，跨授予且不随 PIE 重建；只在授予实例上缓存，避免编辑器改资产后读到旧结果。
	if (IsInstantiated()) { PreparedConfiguration = Prepared; }
	return Prepared;
}

bool UGGYGOPlayerComboAbility::CanActivateAbilityAdditional(FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbilityAdditional(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags))
	{
		return false;
	}
	FString Error;
	return FindOrPrepareConfiguration(Error).IsValid();
}

void UGGYGOPlayerComboAbility::NativeOnAbilityFailedToActivate(const FGameplayTagContainer& FailedReason) const
{
	// 纯 CanActivate 查询不写日志；只在实际被拒的请求上按失败类别报告一次。
	FString Error;
	bool bDamageFailure = false;
	if (!FindOrPrepareConfiguration(Error, &bDamageFailure).IsValid())
	{
		if (bDamageFailure)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 激活失败时伤害GE依赖无效：%s"),
				*GetPathNameSafe(this), *Error);
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 激活失败时连段配置无效：%s"),
				*GetPathNameSafe(this), *Error);
		}
	}
	Super::NativeOnAbilityFailedToActivate(FailedReason);
}

void UGGYGOPlayerComboAbility::InitializeAbilityActivation(const FGGYGOAbilityActivationHandle& Original)
{
	if (!Original.HasActivation() || !CaptureCurrentActivation().HasSameActivation(Original))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Initialize rejected: missing or invalid GA original activation."),
			*GetPathNameSafe(this));
		return;
	}
	if (ResourceActivation.HasActivation() || InitialInputRoute.HasRoute() || !StepEntryAttempts.IsEmpty()
		|| PreparedActivation.IsValid()
		|| StepMotionResources.IsValid() || InputTask || TraceComponent || ActiveMesh || InputCallbackRegistration.IsValid()
		|| TraceWindow.HasWindow() || TraceHitSubscription.IsValid()
		|| OriginalWorld.IsValid() || bChangedMeshTick || bAddedMeshPrerequisite)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Initialize rejected: original resources have not been released."),
			*GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	ResourceActivation = Original;
	FString RouteError;
	UGGYGOAbilitySystemComponent* const ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	InitialInputRoute = ASC ? ASC->CaptureInitialInputRoute(this, Original, RouteError) : FGGYGOAbilityInitialInputRoute{};
	if (!InitialInputRoute.HasRoute())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 原 Initial 清理路由不可用：ASC='%s'，%s"),
			*GetPathName(), *GetPathNameSafe(ASC), *RouteError);
		RequestAbilityEnd(Original, true, true);
		return;
	}
	Window.Reset();
	CurrentStep = INDEX_NONE;
	LastRequestId = 0;
	CurrentStepRequestId = 0;
	StepSyncRevision = 0;
	CurrentStepToken = 0;
	Super::InitializeAbilityActivation(Original);
}

void UGGYGOPlayerComboAbility::ActivateAbilityBody(const FGGYGOAbilityActivationHandle& Original,
	FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo, FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!IsActivationCurrent(Original))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Body rejected: GA original activation/Initialize prerequisite is not current."),
			*GetPathNameSafe(this));
		if (Original.HasActivation()) { RequestAbilityEnd(Original, true, true); }
		return;
	}
	Super::ActivateAbilityBody(Original, Handle, ActorInfo, ActivationInfo, TriggerEventData);
	if (!IsActivationCurrent(Original)) { return; }

	// 准入已解析本授予期配置。段、GE 及其选择字段只能在类默认值编辑，蓝图激活回调改不了，
	// 所以这里只取快照、不重新校验；之后各段与命中只读该快照，运行期只核对 Mesh/Trace/CMC 等会变化的输入。
	FString ConfigurationError;
	bool bDamageFailure = false;
	TSharedPtr<const FPreparedComboConfiguration> Prepared = FindOrPrepareConfiguration(ConfigurationError, &bDamageFailure);
	if (!Prepared.IsValid())
	{
		if (bDamageFailure)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 提交前拒绝激活：%s"),
				*GetPathNameSafe(this), *ConfigurationError);
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] 提交前连段配置无效：%s"),
				*GetPathNameSafe(this), *ConfigurationError);
		}
		RequestAbilityEnd(Original, true, true);
		return;
	}
	PreparedActivation = MoveTemp(Prepared);

	ACharacter* Character = GetCharacterFromActorInfo();
	ActiveMesh = Character ? Character->GetMesh() : nullptr;
	TraceComponent = Character ? Character->FindComponentByClass<UGGYGOMeleeTraceComponent>() : nullptr;
	OriginalWorld = GetWorld();
	if (!Character || !Cast<UGGYGOCharacterMovementComponent>(Character->GetCharacterMovement())
		|| !IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Body rejected: Avatar/Mesh [%s], Trace [%s] or World [%s] is invalid."),
			*GetPathNameSafe(this), *GetPathNameSafe(ActiveMesh), *GetPathNameSafe(TraceComponent), *GetPathNameSafe(OriginalWorld.Get()));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	const bool bCommitted = CommitAbility(Handle, ActorInfo, ActivationInfo);
	if (!IsActivationCurrent(Original)) { return; }
	if (!bCommitted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：激活提交失败。"));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	if (!IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Mesh/Trace/World became invalid during Commit."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}

	if (HasAuthority(&ActivationInfo))
	{
		SavedMeshTick = ActiveMesh->VisibilityBasedAnimTickOption;
		bSavedUpdateRateOptimizations = ActiveMesh->bEnableUpdateRateOptimizations;
		ActiveMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		ActiveMesh->bEnableUpdateRateOptimizations = false;
		bChangedMeshTick = true;
		const auto HasMeshPrerequisite = [Trace = TraceComponent.Get(), Mesh = ActiveMesh.Get()]()
		{
			return Trace->PrimaryComponentTick.GetPrerequisites().ContainsByPredicate([Mesh](const FTickPrerequisite& Prerequisite)
			{
				return Prerequisite.PrerequisiteObject.Get() == Mesh && Prerequisite.Get() == &Mesh->PrimaryComponentTick;
			});
		};
		if (!HasMeshPrerequisite())
		{
			TraceComponent->AddTickPrerequisiteComponent(ActiveMesh);
			if (!IsActivationCurrent(Original)) { return; }
			bAddedMeshPrerequisite = HasMeshPrerequisite();
			if (!bAddedMeshPrerequisite)
			{
				UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Trace [%s] failed to add original Mesh [%s] tick prerequisite."),
					*GetPathNameSafe(this), *GetPathNameSafe(TraceComponent), *GetPathNameSafe(ActiveMesh));
				RequestAbilityEnd(Original, true, true);
				return;
			}
		}
	}
	UGGYGOAbilityTask_WaitComboInput* const StartedInputTask = UGGYGOAbilityTask_WaitComboInput::WaitComboInput(this, InitialInputRoute);
	if (!IsActivationCurrent(Original))
	{
		if (IsValid(StartedInputTask)) { StartedInputTask->TaskOwnerEnded(); }
		return;
	}
	if (!IsValid(StartedInputTask))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] WaitComboInput factory failed."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	InputTask = StartedInputTask;
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> WeakInputTask(StartedInputTask);
	const FDelegateHandle Registration = StartedInputTask->RegisterNativeCallback(
		FGGYGOComboInputNativeDelegate::CreateLambda([WeakThis, WeakInputTask, Original](const FGGYGOAbilityStepInputRequest& Request)
		{
			ThisClass* Self = WeakThis.Get();
			if (Self && WeakInputTask.IsValid() && Self->InputTask == WeakInputTask.Get() && Self->IsActivationCurrent(Original))
			{
				Self->HandleStepInputRequest(Original, Request);
			}
		}), FSimpleDelegate::CreateLambda([WeakThis, WeakInputTask, Original]()
		{
			if (ThisClass* Self = WeakThis.Get()) { Self->HandleInitialInputUnavailable(Original, WeakInputTask.Get()); }
		}));
	if (!IsActivationCurrent(Original) || InputTask != StartedInputTask)
	{
		if (WeakInputTask.IsValid())
		{
			StartedInputTask->UnregisterNativeCallback(Registration);
			if (WeakInputTask.IsValid()) { StartedInputTask->TaskOwnerEnded(); }
		}
		return;
	}
	InputCallbackRegistration = Registration;
	if (!Registration.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Input Task [%s] pre-Ready native registration failed."),
			*GetPathNameSafe(this), *GetPathNameSafe(StartedInputTask));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	if (!WeakInputTask.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Input Task became invalid before Ready."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}

	// 原 Task 的订阅与注销资源先就位，再消费本地 Initial 或标准缓存的远端 Initial。
	StartedInputTask->ReadyForActivation();
	if (!IsActivationCurrent(Original) || InputTask != StartedInputTask) { return; }
	if (!WeakInputTask.IsValid() || !StartedInputTask->IsActive())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] original Input Task did not remain active after Ready."), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
	}
}

bool UGGYGOPlayerComboAbility::IsActivationCurrent(const FGGYGOAbilityActivationHandle& Original) const
{
	return ResourceActivation.HasSameActivation(Original) && Super::IsActivationCurrent(Original);
}

bool UGGYGOPlayerComboAbility::IsStepCurrent(const FGGYGOAbilityActivationHandle& Original,
	uint64 ExpectedStepToken, const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* ExpectedTask) const
{
	return IsActivationCurrent(Original) && ExpectedStepToken != 0 && CurrentStepToken == ExpectedStepToken
		&& IsValid(ExpectedTask) && GetMontageTask() == ExpectedTask;
}

bool UGGYGOPlayerComboAbility::IsMotionResourceCurrent(const TSharedPtr<FStepMotionResources>& Resource) const
{
	return Resource.IsValid() && StepMotionResources == Resource && Resource->Section != FStepMotionResources::ESection::Retired
		&& CurrentStep == Resource->StepIndex
		&& IsActivationCurrent(Resource->Activation) && Resource->StepToken != 0 && CurrentStepToken == Resource->StepToken
		// Completed is authenticated while the original Task is live. Its subsequent EndTask
		// may invalidate the weak sender before CMC consumes the final native movement interval.
		&& (Resource->bMontageCompleted || IsStepCurrent(Resource->Activation, Resource->StepToken, Resource->Task.Get()));
}

bool UGGYGOPlayerComboAbility::CaptureOriginalStepObservation(FGGYGOPlayerComboObservation& OutObservation,
	FGGYGOMontageSectionSnapshot& OutSnapshot, FString& OutError)
{
	check(IsInGameThread());
	OutObservation.Reset();
	OutSnapshot = {};
	OutError.Reset();
	const auto Resource = StepMotionResources;
	if (HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed))
	{
		OutError = TEXT("[Combat.Observation] 只接受实际原 GA 实例。");
		return false;
	}
	const FGameplayAbilityActorInfo* Info = GetCurrentActorInfo();
	APawn* Pawn = Info ? Cast<APawn>(Info->AvatarActor.Get()) : nullptr;
	auto* ASC = Info ? Cast<UGGYGOAbilitySystemComponent>(Info->AbilitySystemComponent.Get()) : nullptr;
	if (!IsMotionResourceCurrent(Resource) || !Resource->Source.IsValid() || Resource->bMontageCompleted
		|| !Resource->Movement.IsValid() || Resource->MotionHandle == INDEX_NONE
		|| Resource->MontageInstanceId == INDEX_NONE || !IsValid(Pawn) || !IsValid(ASC)
		|| !IsValid(ActiveMesh) || Info->SkeletalMeshComponent.Get() != ActiveMesh
		|| ActiveMesh->GetOwner() != Pawn || !OriginalWorld.IsValid() || Pawn->GetWorld() != OriginalWorld.Get()
		|| ActiveMesh->GetWorld() != OriginalWorld.Get()
		|| !Resource->Task.IsValid() || !Resource->Task->TryGetOriginalSectionSnapshot(OutSnapshot)
		|| OutSnapshot.Montage != Resource->Source->Montage.Get()
		|| OutSnapshot.MontageInstanceId != Resource->MontageInstanceId
		|| !FMath::IsFinite(OutSnapshot.PositionSeconds) || !FMath::IsFinite(OutSnapshot.InstancePlayRate))
	{
		OutSnapshot = {};
		OutError = TEXT("[Combat.Observation] 原激活／段／Task／Mesh／World或实例快照无效，拒绝捕获。");
		return false;
	}
	if (!Resource->Observation.IsValid())
	{
		Resource->Observation = MakeShared<FGGYGOPlayerComboObservation::FState>();
		auto& Facts = Resource->Observation->Facts;
		Facts.Activation = Resource->Activation;
		Facts.Ability = this;
		Facts.ASC = ASC;
		Facts.Pawn = Pawn;
		Facts.Mesh = ActiveMesh;
		Facts.World = OriginalWorld;
		Facts.Task = Resource->Task;
		Facts.Montage = Resource->Source->Montage.Get();
		Facts.Movement = Resource->Movement;
		Facts.StepToken = Resource->StepToken;
		Facts.StepIndex = Resource->StepIndex;
		Facts.MontageInstanceId = Resource->MontageInstanceId;
		Facts.MotionHandle = Resource->MotionHandle;
		// 原 CMC 的完成通知可能早于捕获；只复制此原自有资源已认证的事实。
		Facts.bMotionCompleted = Resource->bMotionCompleted;
	}
	OutObservation.History = Resource->Observation;
	return true;
}

bool UGGYGOPlayerComboAbility::ReadOriginalStepObservation(const FGGYGOPlayerComboObservation& Observation,
	FGGYGOMontageSectionSnapshot& OutSnapshot, FString& OutError, bool* OutResourceSourceCurrent) const
{
	check(IsInGameThread());
	OutSnapshot = {};
	OutError.Reset();
	if (OutResourceSourceCurrent) { *OutResourceSourceCurrent = false; }
	const auto Resource = StepMotionResources;
	const auto History = Observation.History;
	if (HasAnyFlags(RF_ClassDefaultObject | RF_BeginDestroyed | RF_FinishDestroyed) || !History.IsValid())
	{
		OutError = TEXT("[Combat.Observation] 须实际原 GA 及其原观察视图。");
		return false;
	}
	const FGameplayAbilityActorInfo* Info = GetCurrentActorInfo();
	if (!IsMotionResourceCurrent(Resource) || !Resource->Source.IsValid()
		|| Resource->Observation != History
		|| !Info || !History->Facts.Activation.HasSameActivation(Resource->Activation)
		|| History->Facts.Ability.Get() != this || History->Facts.StepToken != Resource->StepToken
		|| History->Facts.StepIndex != Resource->StepIndex || History->Facts.Task != Resource->Task
		|| History->Facts.Mesh.Get() != ActiveMesh || Info->SkeletalMeshComponent.Get() != ActiveMesh
		|| History->Facts.ASC.Get() != Info->AbilitySystemComponent.Get()
		|| History->Facts.Pawn.Get() != Info->AvatarActor.Get()
		|| !History->Facts.World.IsValid() || History->Facts.World != OriginalWorld
		|| !IsValid(ActiveMesh) || ActiveMesh->GetOwner() != History->Facts.Pawn.Get()
		|| ActiveMesh->GetWorld() != History->Facts.World.Get()
		|| Resource->Source->Montage.Get() != History->Facts.Montage.Get()
		|| Resource->MontageInstanceId != History->Facts.MontageInstanceId
		|| Resource->MotionHandle != History->Facts.MotionHandle
		|| !History->Facts.Movement.IsValid() || History->Facts.Movement != Resource->Movement)
	{
		OutError = TEXT("[Combat.Observation] 原 GA 的段资源源已结束／改变；保留原历史，不改绑后继。");
		return false;
	}
	if (OutResourceSourceCurrent) { *OutResourceSourceCurrent = true; }
	// 原 Task 结束、CMC 正在 drain 时，原 GA 仍可持此源；实时实例不可用是明确事实。
	if (!Resource->Task.IsValid() || !Resource->Task->TryGetOriginalSectionSnapshot(OutSnapshot)
		|| OutSnapshot.Montage != History->Facts.Montage.Get()
		|| OutSnapshot.MontageInstanceId != History->Facts.MontageInstanceId
		|| !FMath::IsFinite(OutSnapshot.PositionSeconds) || !FMath::IsFinite(OutSnapshot.InstancePlayRate))
	{
		OutSnapshot = {};
		OutError = TEXT("[Combat.Observation] 原段仍属此 GA，但原 Task 实时实例快照不可用。");
		return false;
	}
	return true;
}

bool UGGYGOPlayerComboAbility::IsInterruptionOpen() const
{
	return IsAttackContinuationOpen();
}

bool UGGYGOPlayerComboAbility::IsAttackContinuationOpen() const
{
	return IsMotionResourceCurrent(StepMotionResources) && StepMotionResources->AttackGate.bOpen;
}

bool UGGYGOPlayerComboAbility::IsMovementInterruptionOpen() const
{
	return IsMotionResourceCurrent(StepMotionResources) && StepMotionResources->MovementGate.bOpen;
}

bool UGGYGOPlayerComboAbility::ReadAuthorPointSnapshot(const TSharedPtr<FStepMotionResources>& Resource,
	const FAuthorPointGate& Gate, FGGYGOMontageNotifyFact& OutFact, float& OutPositionSeconds, FString& OutError) const
{
	OutFact = {};
	OutPositionSeconds = 0.0f;
	OutError.Reset();
	const UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const OriginalTask = Resource.IsValid() ? Resource->Task.Get() : nullptr;
	FGGYGOMontageNotifyFact Fact;
	float Position = 0.0f;
	// 只读取本门名字的原声明和固定原实例，不借另一门或同资产后继的快照。
	if (!OriginalTask || !Gate.IsAuthored()
		|| !OriginalTask->TryGetOriginalMontageNotifySnapshotByName(Gate.NotifyName, Fact, Position))
	{
		OutError = FString::Printf(TEXT("Original Task live point snapshot for [%s] is unavailable."), *Gate.NotifyName.ToString());
		return false;
	}
	if (Fact.NotifyName != Gate.NotifyName)
	{
		OutError = FString::Printf(TEXT("Original Task authenticates point [%s], not this gate's point [%s]."),
			*Fact.NotifyName.ToString(), *Gate.NotifyName.ToString());
		return false;
	}
	if (Fact.Montage != Resource->Source->Montage.Get() || Fact.Mesh != ActiveMesh
		|| Fact.MontageInstanceId == INDEX_NONE || Fact.MontageInstanceId != Resource->MontageInstanceId
		|| Fact.NotifyPositionSeconds != Gate.TriggerPosition
		|| Fact.NotifyAuthoredPositionSeconds != Gate.AuthoredPosition
		|| Fact.NotifyTriggerTimeOffsetSeconds != Gate.TriggerTimeOffset || !FMath::IsFinite(Position))
	{
		OutError = FString::Printf(TEXT("Original Task point [%s] snapshot differs from this step's Montage/Mesh/playback/original declaration."),
			*Gate.NotifyName.ToString());
		return false;
	}
	OutFact = Fact;
	OutPositionSeconds = Position;
	return true;
}

void UGGYGOPlayerComboAbility::FailStepMotion(const TSharedPtr<FStepMotionResources>& Resource, const FString& Reason)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] Step %d Montage [%s] Slot [%s] CMC [%s] motion failed: %s"),
		*Resource->AbilityPath, Resource->StepIndex, *GetPathNameSafe(Resource->Source->Montage.Get()),
		*Resource->Source->SlotName.ToString(), *GetPathNameSafe(Resource->Movement.Get()), *Reason);
	// A required dependency failure must end this original even if user cancellation is disabled.
	RequestAbilityEnd(Resource->Activation, true, true);
}

bool UGGYGOPlayerComboAbility::InitializeStepMotion(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	FGGYGOMontageSectionSnapshot Snapshot;
	FString SnapshotError;
	if (!Resource->ReadOriginalSnapshot(Snapshot, SnapshotError))
	{
		FailStepMotion(Resource, SnapshotError);
		return false;
	}
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	Resource->MontageInstanceId = Snapshot.MontageInstanceId;
	const auto* MainSource = Resource->Source->FindSection(Resource->Source->SectionName);
	const auto* EndSource = Resource->Source->FindSection(Resource->EndSection);
	if (!MainSource || !EndSource || Resource->Source->Sections.Num() != 2)
	{
		FailStepMotion(Resource, TEXT("Original binding does not contain the configured adjacent Main→End sections."));
		return false;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=OriginalSnapshot Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d Section='%s' Position=%.9g Main='%s' MainRange=[%.9g,%.9g] End='%s' EndRangeAvailable=%d EndRange=[%.9g,%.9g] MontageLength=%.9g EffectiveRate=%.9g"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Snapshot.Montage), Snapshot.MontageInstanceId, *Snapshot.SectionName.ToString(), Snapshot.PositionSeconds,
		*Resource->Source->SectionName.ToString(), MainSource->MontageStartSeconds, MainSource->MontageEndSeconds,
		*Resource->EndSection.ToString(), true, EndSource->MontageStartSeconds, EndSource->MontageEndSeconds,
		Resource->Source->MontageLength, Resource->EffectiveRate);
	FString Error;
	if (!Resource->Movement.IsValid() || !Resource->Movement->GetMovementOwnerSyncScope(Resource->Scope, Error))
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original movement owner scope is unavailable: %s"), *Error));
		return false;
	}
	if (Resource->StartFacing.InputRequest.HasRequest() && Resource->StartFacing.Scope != Resource->Scope)
	{
		FailStepMotion(Resource, TEXT("Task Ready 返栈后原输入请求的 CMC Scope 已改变。"));
		return false;
	}
	const bool bStartsInEnd = Snapshot.SectionName == Resource->EndSection;
	if (!bStartsInEnd && Snapshot.SectionName != Resource->Source->SectionName)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original initial section [%s] is neither configured Main nor End."), *Snapshot.SectionName.ToString()));
		return false;
	}
	Resource->Section = bStartsInEnd ? FStepMotionResources::ESection::End : FStepMotionResources::ESection::Main;
	Resource->StartFacing.Scope = Resource->Scope;
	FQuat AcceptedEntryRotation = FQuat::Identity;
	const bool bStarted = Resource->BeginMotion(Snapshot, Resource->StartFacing, AcceptedEntryRotation, Error);
	if (!IsMotionResourceCurrent(Resource))
	{
		Resource->Release(EGGYGOActionMotionReleaseReason::OwnerInvalidated);
		return false;
	}
	if (!bStarted || Resource->MotionHandle == INDEX_NONE)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original Main→End action request was rejected: %s"), *Error));
		return false;
	}
	if (AcceptedEntryRotation.ContainsNaN() || !AcceptedEntryRotation.IsNormalized())
	{
		FailStepMotion(Resource, TEXT("CMC 成功结果未提供有效的原段基础方向。"));
		return false;
	}
	Resource->EntryActorRotation = AcceptedEntryRotation;
	Resource->bHasEntryActorRotation = true;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.StepFacing] Ability='%s' Step=%d StepOrigin=%d StepToken=%llu Mode=%u Entry=%s Position=%.9g Frame=%llu World='%s' Handle=%d Instance=%d"),
		*Resource->AbilityPath, Resource->StepIndex, CurrentStepRequestId,
		static_cast<unsigned long long>(Resource->StepToken), static_cast<uint32>(Resource->StartFacing.Mode),
		*Resource->EntryActorRotation.ToString(), Snapshot.PositionSeconds,
		static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(OriginalWorld.Get()),
		Resource->MotionHandle, Resource->MontageInstanceId);
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakPtr<FStepMotionResources> WeakResource(Resource);
	const bool bObserved = Resource->ObserveMotion(
		FGGYGOActionMotionFailureDelegate::CreateLambda([WeakThis, WeakResource](int32 OriginalHandle, const FString& Diagnostic)
		{
			ThisClass* Self = WeakThis.Get();
			const auto OriginalResource = WeakResource.Pin();
			if (Self && Self->IsMotionResourceCurrent(OriginalResource) && OriginalResource->MotionHandle == OriginalHandle)
			{ Self->FailStepMotion(OriginalResource, Diagnostic); }
		}),
		FGGYGOActionMotionCompletionDelegate::CreateLambda([WeakThis, WeakResource](int32 OriginalHandle)
		{
			ThisClass* Self = WeakThis.Get();
			const auto OriginalResource = WeakResource.Pin();
			if (!Self || !Self->IsMotionResourceCurrent(OriginalResource)
				|| OriginalResource->MotionHandle != OriginalHandle || OriginalResource->bMotionCompleted) { return; }
			OriginalResource->bMotionCompleted = true;
			if (OriginalResource->Observation.IsValid()) { OriginalResource->Observation->Facts.bMotionCompleted = true; }
			Self->TryCompleteStepMotion(OriginalResource);
		}), [this, Resource]() { return IsMotionResourceCurrent(Resource); }, Error);
	if (!IsMotionResourceCurrent(Resource)) { return false; }
	if (!bObserved)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original action failure/completion observation could not be installed: %s"), *Error));
		return false;
	}
	// 起播／纠正边界只用原 Task 对各自名字的原实例实时快照恢复门；纠正载荷位置和窗口标志从不开门。
	if (Resource->AttackGate.IsAuthored())
	{
		FGGYGOMontageNotifyFact AttackSnapshot;
		float AttackPosition = 0.0f;
		if (!ReadAuthorPointSnapshot(Resource, Resource->AttackGate, AttackSnapshot, AttackPosition, Error)
			|| (Resource->AttackGate.PendingInstanceId != INDEX_NONE
				&& Resource->AttackGate.PendingInstanceId != Resource->MontageInstanceId))
		{
			FailStepMotion(Resource, FString::Printf(TEXT("Original attack point snapshot is unavailable or differs from its early fact: %s"), *Error));
			return false;
		}
		if (Resource->AttackGate.PendingInstanceId == Resource->MontageInstanceId
			|| AttackPosition >= Resource->AttackGate.OpenFloorPosition)
		{
			OpenAttackContinuation(Resource);
			if (!IsMotionResourceCurrent(Resource)) { return false; }
		}
	}
	if (Resource->MovementGate.IsAuthored())
	{
		if (Resource->MovementGate.PendingInstanceId != INDEX_NONE
			&& Resource->MovementGate.PendingInstanceId != Resource->MontageInstanceId)
		{
			FailStepMotion(Resource, TEXT("Early movement point fact differs from this step's original playback."));
			return false;
		}
		// 移动门可能同步结束本能力，必须最后处理。
		TryOpenMovementInterruption(Resource);
	}
	return IsMotionResourceCurrent(Resource);
}

void UGGYGOPlayerComboAbility::HandleMontageSection(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageSectionFact& Fact)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->MontageInstanceId == INDEX_NONE) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=SectionReceived Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d Section='%s' Looped=%d ExpectedInstance=%d End='%s' DerivedSection=%d"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Fact.Montage), Fact.MontageInstanceId, *Fact.SectionName.ToString(), Fact.bLooped,
		Resource->MontageInstanceId, *Resource->EndSection.ToString(), static_cast<int32>(Resource->Section));
	if (Fact.Montage != Resource->Source->Montage.Get() || Fact.MontageInstanceId != Resource->MontageInstanceId)
	{
		FailStepMotion(Resource, TEXT("Section fact does not identify this step's original playback."));
		return;
	}
	if (Fact.bLooped)
	{
		FailStepMotion(Resource, TEXT("Original Combo section loop is not an allowed Main→End path."));
		return;
	}
	if (Fact.SectionName == Resource->EndSection)
	{
		Resource->Section = FStepMotionResources::ESection::End;
		// End 事实只触发对移动点原实例快照的重查；是否开门仍由该名字快照位置决定。
		if (Resource->MovementGate.IsAuthored()) { TryOpenMovementInterruption(Resource); }
	}
	else if (Fact.SectionName != Resource->Source->SectionName)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original playback entered unconfigured section [%s]."), *Fact.SectionName.ToString()));
	}
	// A queued initial Main fact is history. It never reopens Main after an accepted End fact.
}

void UGGYGOPlayerComboAbility::HandleMontageNotify(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageNotifyFact& Fact)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->bMontageCompleted
		|| (!Resource->AttackGate.IsAuthored() && !Resource->MovementGate.IsAuthored())) { return; }
	// 名字唯一确定所属门；校验已拒绝两门同名，所以同位置的不同名点不能跨门授权。
	const bool bAttackPoint = Resource->AttackGate.IsAuthored() && Fact.NotifyName == Resource->AttackGate.NotifyName;
	const bool bMovementPoint = Resource->MovementGate.IsAuthored() && Fact.NotifyName == Resource->MovementGate.NotifyName;
	FAuthorPointGate* const Gate = bAttackPoint ? &Resource->AttackGate : (bMovementPoint ? &Resource->MovementGate : nullptr);
	if (!Gate || Fact.Montage != Resource->Source->Montage.Get() || Fact.Mesh != ActiveMesh
		|| Fact.MontageInstanceId == INDEX_NONE || Fact.NotifyPositionSeconds != Gate->TriggerPosition
		|| Fact.NotifyAuthoredPositionSeconds != Gate->AuthoredPosition
		|| Fact.NotifyTriggerTimeOffsetSeconds != Gate->TriggerTimeOffset
		|| (Resource->MontageInstanceId != INDEX_NONE && Fact.MontageInstanceId != Resource->MontageInstanceId)
		|| (Gate->PendingInstanceId != INDEX_NONE && Gate->PendingInstanceId != Fact.MontageInstanceId))
	{
		FailStepMotion(Resource, TEXT("Author notify fact does not identify this original step's exact Mesh, Montage, configured point or playback."));
		return;
	}
	Gate->PendingInstanceId = Fact.MontageInstanceId;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=OriginalSignal Gate=%s Ability='%s' Step=%d StepToken=%llu Instance=%d Notify='%s' Trigger=%.9g Floor=%.9g MotionHandle=%d Scope=%llu"),
		bAttackPoint ? TEXT("Attack") : TEXT("Movement"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		Fact.MontageInstanceId, *Fact.NotifyName.ToString(), Fact.NotifyPositionSeconds, Gate->OpenFloorPosition,
		Resource->MotionHandle, static_cast<unsigned long long>(Resource->Scope.GetScopeSerial()));
	// Ready may still be returning to the GA. Lock only this authenticated fact until
	// InitializeStepMotion has issued the actual original CMC Scope/Handle and rechecked ID.
	if (Resource->MontageInstanceId == INDEX_NONE || Resource->MotionHandle == INDEX_NONE || !Resource->Scope.IsSet()) { return; }
	if (bAttackPoint) { OpenAttackContinuation(Resource); }
	else { TryOpenMovementInterruption(Resource); }
}

void UGGYGOPlayerComboAbility::OpenAttackContinuation(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->bMontageCompleted || Resource->AttackGate.bOpen
		|| !Resource->AttackGate.IsAuthored()) { return; }
	if (Resource->MontageInstanceId == INDEX_NONE || Resource->MotionHandle == INDEX_NONE || !Resource->Scope.IsSet())
	{
		FailStepMotion(Resource, TEXT("Attack continuation cannot open without its original playback and issued CMC Scope/Handle."));
		return;
	}
	// 攻击接续只改变本门权限；同一 CMC Handle 继续持有轨迹，不查询或订阅移动。
	Resource->AttackGate.bOpen = true;
	Resource->AttackGate.PendingInstanceId = INDEX_NONE;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=PermissionOpen Gate=Attack Ability='%s' Step=%d StepToken=%llu Instance=%d MotionHandle=%d Scope=%llu"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		Resource->MontageInstanceId, Resource->MotionHandle, static_cast<unsigned long long>(Resource->Scope.GetScopeSerial()));
}

void UGGYGOPlayerComboAbility::TryOpenMovementInterruption(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->bMontageCompleted || Resource->MovementGate.bOpen
		|| !Resource->MovementGate.IsAuthored()) { return; }
	// 未签发原 CMC Scope/Handle 前只保留 Pending；InitializeStepMotion 签发后重查。
	if (Resource->MontageInstanceId == INDEX_NONE || Resource->MotionHandle == INDEX_NONE || !Resource->Scope.IsSet()) { return; }
	FGGYGOMontageNotifyFact LiveFact;
	float LivePosition = 0.0f;
	FString Error;
	if (!ReadAuthorPointSnapshot(Resource, Resource->MovementGate, LiveFact, LivePosition, Error))
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original movement point live snapshot is required: %s"), *Error));
		return;
	}
	// 原生触发可按 TriggerTimeOffset 早于作者时间且仍处 Main；只有原实例真实位置到达 Main 末端才开门。
	if (LivePosition < Resource->MovementGate.OpenFloorPosition) { return; }
	Resource->MovementGate.bOpen = true;
	Resource->MovementGate.PendingInstanceId = INDEX_NONE;
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=PermissionOpen Gate=Movement Ability='%s' Step=%d StepToken=%llu Instance=%d Position=%.9g Floor=%.9g MotionHandle=%d Scope=%llu"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		Resource->MontageInstanceId, LivePosition, Resource->MovementGate.OpenFloorPosition,
		Resource->MotionHandle, static_cast<unsigned long long>(Resource->Scope.GetScopeSerial()));
	BeginMovementInterruption(Resource);
}

void UGGYGOPlayerComboAbility::BeginMovementInterruption(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource) || !Resource->MovementGate.bOpen) { return; }
	FString Error;
	if (!Resource->Movement.IsValid())
	{
		FailStepMotion(Resource, TEXT("Original CMC expired at movement interruption point."));
		return;
	}
	// 门已开、原 Scope/Handle/StepToken 已在资源中；只有已认证 Qualified Intent 才取消同一 CMC Handle。
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakPtr<FStepMotionResources> WeakResource(Resource);
	const auto ReceiveIntent = [WeakThis, WeakResource](EGGYGOQualifiedMovementIntentQueryResult Result,
		const FGGYGOQualifiedMovementIntent& Intent, const FString& Diagnostic, const TCHAR* Delivery)
	{
		ThisClass* Self = WeakThis.Get();
		const TSharedPtr<FStepMotionResources> OriginalResource = WeakResource.Pin();
		if (!Self || !Self->IsMotionResourceCurrent(OriginalResource)
			|| !OriginalResource->MovementGate.bOpen) { return; }
		// Read the exact Task once at this boundary; an unavailable snapshot is diagnostic only.
		FGGYGOMontageSectionSnapshot CurrentSnapshot;
		const auto* OriginalTask = OriginalResource->Task.Get();
		const bool bSnapshotAvailable = OriginalTask && OriginalTask->TryGetOriginalSectionSnapshot(CurrentSnapshot);
		const FString OriginalLogContext = FString::Printf(
			TEXT("Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d MotionHandle=%d Scope=%llu"),
			*OriginalResource->AbilityPath, OriginalResource->StepIndex, static_cast<unsigned long long>(OriginalResource->StepToken),
			*GetPathNameSafe(OriginalResource->Source->Montage.Get()), OriginalResource->MontageInstanceId,
			OriginalResource->MotionHandle, static_cast<unsigned long long>(OriginalResource->Scope.GetScopeSerial()));
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=MovementIntent Delivery=%s %s Result=%s(%d) Provenance=%d IntentScope=%llu Binding=%llu Session=%llu Request=%llu Execution=%llu SnapshotAvailable=%d SnapshotSection='%s' Position=%.9g MontageLength=%.9g Diagnostic='%s'"),
			Delivery, *OriginalLogContext, DescribeQualifiedMovementResult(Result), static_cast<int32>(Result), static_cast<int32>(Intent.Provenance),
			static_cast<unsigned long long>(Intent.Scope.GetScopeSerial()), static_cast<unsigned long long>(Intent.BindingSerial),
			static_cast<unsigned long long>(Intent.SessionSerial), static_cast<unsigned long long>(Intent.RequestSerial),
			static_cast<unsigned long long>(Intent.ExecutionRequestSerial), bSnapshotAvailable,
			*CurrentSnapshot.SectionName.ToString(), CurrentSnapshot.PositionSeconds, OriginalResource->Source->MontageLength, *Diagnostic);
		if (Result == EGGYGOQualifiedMovementIntentQueryResult::Unavailable
			|| Result == EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed)
		{
			Self->FailStepMotion(OriginalResource, FString::Printf(TEXT("Original movement-point dependency failed: %s"), *Diagnostic));
			return;
		}
		if (Result != EGGYGOQualifiedMovementIntentQueryResult::Qualified) { return; }
		if (Intent.Scope != OriginalResource->Scope || !OriginalResource->Movement.IsValid())
		{
			Self->FailStepMotion(OriginalResource, TEXT("Qualified movement-point intent belongs to a different owner scope or expired CMC."));
			return;
		}
		FString CancelError;
		FGGYGOQualifiedMovementIntent CurrentIntent;
		const auto CurrentResult = OriginalResource->Movement->QueryQualifiedMovementIntent(OriginalResource->Scope, CurrentIntent, CancelError);
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=IntentRevalidation %s Result=%s(%d) SameIntent=%d Diagnostic='%s'"),
			*OriginalLogContext, DescribeQualifiedMovementResult(CurrentResult), static_cast<int32>(CurrentResult), CurrentIntent == Intent, *CancelError);
		if (CurrentResult != EGGYGOQualifiedMovementIntentQueryResult::Qualified || !(CurrentIntent == Intent))
		{
			if (CurrentResult == EGGYGOQualifiedMovementIntentQueryResult::Unavailable
				|| CurrentResult == EGGYGOQualifiedMovementIntentQueryResult::ExecutionFailed)
			{
				Self->FailStepMotion(OriginalResource, FString::Printf(TEXT("Original movement-point intent revalidation failed: %s"), *CancelError));
			}
			return; // A retired/replaced notification cannot cancel with a successor request.
		}
		if (OriginalResource->MotionHandle == INDEX_NONE)
		{
			Self->FailStepMotion(OriginalResource, TEXT("Qualified movement-point intent has no issued original action handle."));
			return;
		}
		{
			const bool bMotionCancelled = OriginalResource->Movement->CancelMontageActionMotionForMovement(OriginalResource->MotionHandle, Intent, CancelError);
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=MotionCancellation %s Succeeded=%d Diagnostic='%s'"),
				*OriginalLogContext, bMotionCancelled, *CancelError);
			if (!bMotionCancelled)
			{
				Self->FailStepMotion(OriginalResource, FString::Printf(TEXT("Original movement-point cancellation was rejected: %s"), *CancelError));
				return;
			}
		}
		if (!Self->IsMotionResourceCurrent(OriginalResource)) { return; }
		OriginalResource->AttackGate.Close();
		OriginalResource->MovementGate.Close();
		OriginalResource->Section = FStepMotionResources::ESection::Retired;
		OriginalResource->MotionHandle = INDEX_NONE;
		// The original CMC cancellation precedes this GA's selected End lifecycle rule.
		const FGGYGOAbilityTerminationResult EndResult = Self->RequestAbilityEnd(OriginalResource->Activation, true, true);
		if (EndResult.IsAccepted())
		{
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=AbilityEndResult %s Outcome=%d"),
				*OriginalLogContext, static_cast<int32>(EndResult.Outcome));
		}
		else
		{
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=AbilityEndResult %s Outcome=%d; original End request was not accepted"),
				*OriginalLogContext, static_cast<int32>(EndResult.Outcome));
		}
		// Only local immutable diagnostic data is accessed after the external End request.
	};
	FGGYGOQualifiedMovementIntent InitialIntent;
	const auto InitialResult = Resource->Movement->QueryQualifiedMovementIntent(Resource->Scope, InitialIntent, Error);
	ReceiveIntent(InitialResult, InitialIntent, Error, TEXT("MovementPoint"));
	if (!IsMotionResourceCurrent(Resource)) { return; }
	const bool bSubscribed = Resource->Movement->SubscribeQualifiedMovementIntent(Resource->Scope,
		FGGYGOQualifiedMovementIntentDelegate::CreateLambda(
			[WeakThis, WeakResource, ReceiveIntent](const FGGYGOQualifiedMovementIntentObserverId& OriginalObserver,
				EGGYGOQualifiedMovementIntentQueryResult Result, const FGGYGOQualifiedMovementIntent& Intent, const FString& Diagnostic)
			{
				ThisClass* Self = WeakThis.Get();
				const TSharedPtr<FStepMotionResources> OriginalResource = WeakResource.Pin();
				if (Self && Self->IsMotionResourceCurrent(OriginalResource)
					&& OriginalResource->Observer == OriginalObserver && OriginalObserver.GetScope() == OriginalResource->Scope)
				{
					ReceiveIntent(Result, Intent, Diagnostic, TEXT("Observer"));
				}
			}), Resource->Observer, Error);
	// OutObserver is installed before Replay, so reentrant termination can clean the original record.
	if (!IsMotionResourceCurrent(Resource)) { return; }
	if (!bSubscribed || !Resource->Observer.IsSet())
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original movement-point subscription failed: %s"), *Error));
	}
}

void UGGYGOPlayerComboAbility::ReleaseStepMotion(EGGYGOActionMotionReleaseReason Reason)
{
	const TSharedPtr<FStepMotionResources> Original = MoveTemp(StepMotionResources);
	const auto OriginalEntries = StepEntryAttempts;
	for (const auto& Entry : OriginalEntries)
	{
		if (Entry->Phase != FStepEntryAttempt::EPhase::Starting) { ReleaseStepEntry(Entry); }
	}
	if (Original.IsValid()) { Original->Release(Reason); Original->ClearWatchdog(); Original->ReleaseTask(false); }
}

bool UGGYGOPlayerComboAbility::StartStep(const FGGYGOAbilityActivationHandle& Original, int32 Index, float Position, int32 SourceRequestId,
	bool bCorrectionReplay, const FQuat* ReplayEntryActorRotation, TSharedPtr<FStepEntryAttempt> EntryAttempt)
{
	FStepStartInputs Inputs;
	if (!ReadStepStartInputs(Original, Index, SourceRequestId, bCorrectionReplay, ReplayEntryActorRotation,
		EntryAttempt, Inputs) || !ReleaseCurrentStep(Original))
	{
		return false;
	}
	return BeginStep(Original, Index, Position, SourceRequestId, bCorrectionReplay, EntryAttempt, Inputs);
}

bool UGGYGOPlayerComboAbility::ReadStepStartInputs(const FGGYGOAbilityActivationHandle& Original, int32 Index,
	int32 SourceRequestId, bool bCorrectionReplay, const FQuat* ReplayEntryActorRotation,
	const TSharedPtr<FStepEntryAttempt>& EntryAttempt, FStepStartInputs& OutInputs)
{
	if (!IsActivationCurrent(Original)) { return false; }
	if (EntryAttempt.IsValid())
	{
		if (!StepEntryAttempts.Contains(EntryAttempt) || EntryAttempt->Phase != FStepEntryAttempt::EPhase::Starting
			|| !EntryAttempt->Activation.HasSameActivation(Original) || EntryAttempt->TargetStep != Index
			|| EntryAttempt->InputRequest.GetRequestId() != SourceRequestId || CurrentStep != EntryAttempt->PreviousStep
			|| CurrentStepToken != EntryAttempt->PreviousStepToken || bCorrectionReplay)
		{
			return false;
		}
	}
	else
	{
		// 显式重放或原 CurrentFacing 直调取代旧入场尝试，不能恢复原段已退役的等待。
		ReleasePendingStepEntries();
		if (!IsActivationCurrent(Original)) { return false; }
	}
	if (SourceRequestId < 0 || SourceRequestId > 65535)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] StartStep %d rejected: invalid original input request %d."),
			*GetPathNameSafe(this), Index, SourceRequestId);
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	// 本段静态声明只来自 Body 取得的预备配置快照；缺失即本次激活未完成准入，不在此重新解析。
	const TSharedPtr<const FPreparedComboConfiguration> Prepared = PreparedActivation;
	if (!Prepared.IsValid() || !Prepared->Steps.IsValidIndex(Index) || !ComboSteps.IsValidIndex(Index)
		|| !IsValid(ActiveMesh) || !IsValid(TraceComponent) || !OriginalWorld.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] StartStep %d rejected: prepared step/Mesh/Trace/original World is invalid."),
			*GetPathNameSafe(this), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	OutInputs.Step = ComboSteps[Index];
	const FGGYGOComboStep& Step = OutInputs.Step;
	if (!bCorrectionReplay && Step.StartFacingPolicy == EGGYGOComboStepFacingPolicy::QualifiedInput
		&& (!EntryAttempt.IsValid() || !EntryAttempt->InputRequest.HasRequest()))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayerCombo [%s] step %d QualifiedInput 缺少本次受验 owned 请求，拒绝起播。"), *GetPathNameSafe(this), Index);
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	// 后续清理旧资源／Ready 可同步重入；只在本栈保存原消息值，不跨外调借用入口内存。
	const FQuat ReplayBasis = ReplayEntryActorRotation ? *ReplayEntryActorRotation : FQuat::Identity;
	if ((!bCorrectionReplay && ReplayEntryActorRotation)
		|| (ReplayEntryActorRotation && (ReplayBasis.ContainsNaN() || !ReplayBasis.IsNormalized()))
		|| (bCorrectionReplay && !ReplayEntryActorRotation && Step.StartFacingPolicy == EGGYGOComboStepFacingPolicy::QualifiedInput))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayerCombo [%s] step %d 纠正必须复用有效原段 basis，不能重采输入或猜测朝向。"), *GetPathNameSafe(this), Index);
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	UGGYGOCharacterMovementComponent* const Movement = Cast<UGGYGOCharacterMovementComponent>(
		GetCharacterFromActorInfo() ? GetCharacterFromActorInfo()->GetCharacterMovement() : nullptr);
	if (!Movement)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Montage [%s] motion rejected: original CMC is unavailable."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage));
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	// Socket 与 TraceShape 依赖当前 Mesh 资产，换装后可能变化，所以每段起播时核对。
	if (Step.FX.Mode == EGGYGOComboFXMode::MontageWindow
		&& !Step.FX.SocketName.IsNone() && !ActiveMesh->DoesSocketExist(Step.FX.SocketName))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("[Combat.PlayerCombo.FX] Ability='%s' Step=%d Montage='%s' System='%s' Mesh='%s' Socket='%s' configuration rejected: Required original Mesh socket does not exist"),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage), *GetPathNameSafe(Step.FX.System),
			*GetPathNameSafe(ActiveMesh), *Step.FX.SocketName.ToString());
		RequestAbilityEnd(Original, true, true);
		return false;
	}
	FString ShapeError;
	if (!TraceComponent->ValidateTraceShapeOnCurrentMesh(Step.TraceShape, ActiveMesh.Get(), ShapeError))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] StartStep %d Montage [%s] Trace [%s] Mesh [%s] 的 TraceShape 无效：%s"),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage),
			*GetPathNameSafe(TraceComponent.Get()), *GetPathNameSafe(ActiveMesh.Get()), *ShapeError);
		RequestAbilityCancel(Original, true);
		return false;
	}
	OutInputs.Prepared = Prepared;
	OutInputs.PreparedStep = &Prepared->Steps[Index];
	OutInputs.Movement = Movement;
	OutInputs.ReplayBasis = ReplayBasis;
	OutInputs.bHasReplayBasis = ReplayEntryActorRotation != nullptr;
	return true;
}

bool UGGYGOPlayerComboAbility::ReleaseCurrentStep(const FGGYGOAbilityActivationHandle& Original)
{
	const uint64 PreviousStepToken = CurrentStepToken;
	// 本栈持有旧段资源，FX 外调期间成员被清理也不会释放正在使用的对象。
	const TSharedPtr<FStepMotionResources> PreviousMotion = StepMotionResources;
	// 先解绑旧段 Task 回调：之后关闭 Trace、FX 与位移引起的引擎事件不再进入本 GA。
	if (PreviousMotion.IsValid()) { PreviousMotion->UnregisterTaskCallbacks(); }
	// 关闭 Trace 只改组件内部状态并析构弱捕获的命中回调，没有外调。
	ReleaseTraceWindow(Original);
	// 停止 FX 与关闭发布会执行失败委托，可同步结束原激活。
	ReleaseStepFX(PreviousMotion, false);
	if (!IsActivationCurrent(Original) || CurrentStepToken != PreviousStepToken) { return false; }
	// Watchdog 随位移一并清除；结束原 Task 与 CMC 位移会回调观察者。
	ReleaseStepMotion(EGGYGOActionMotionReleaseReason::Replaced);
	return IsActivationCurrent(Original) && CurrentStepToken == PreviousStepToken;
}

bool UGGYGOPlayerComboAbility::BeginStep(const FGGYGOAbilityActivationHandle& Original, int32 Index, float Position,
	int32 SourceRequestId, bool bCorrectionReplay, const TSharedPtr<FStepEntryAttempt>& EntryAttempt,
	const FStepStartInputs& Inputs)
{
	const FGGYGOComboStep& Step = Inputs.Step;
	const FPreparedComboStep& Prepared = *Inputs.PreparedStep;
	const TWeakObjectPtr<UWorld> StepWorld = OriginalWorld;
	FString MotionError;
	Window.Reset();
	CurrentStep = Index;
	CurrentStepRequestId = SourceRequestId;
	const uint64 ThisStepToken = ++StepTokenCounter;
	CurrentStepToken = ThisStepToken;
	if (InputTask) { InputTask->SetSourceStep(Index); }
	const FGameplayTagContainer Events = GetStepWindowTags(Step);
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const StartedMontageTask =
		UGGYGOAbilityTask_PlayMontageAndWaitForEvent::PlayMontageAndWaitForEvent(
		this, TEXT("PlayerCombo"), Step.Montage, Events, Step.PlayRate,
		Position > 0.0f ? NAME_None : Step.MainSection);
	if (!IsActivationCurrent(Original) || CurrentStepToken != ThisStepToken)
	{
		if (IsValid(StartedMontageTask)) { StartedMontageTask->TaskOwnerEnded(); }
		return false;
	}
	if (!IsValid(StartedMontageTask))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Montage Task factory failed for [%s]."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(Step.Montage));
		RequestAbilityCancel(Original, true);
		return false;
	}
	const TSharedPtr<FStepMotionResources> Motion = MakeShared<FStepMotionResources>();
	Motion->Activation = Original;
	Motion->StepToken = ThisStepToken;
	Motion->StepIndex = Index;
	if (EntryAttempt.IsValid())
	{
		Motion->StartFacing.InputRequest = EntryAttempt->InputRequest;
		Motion->StartFacing.Scope = EntryAttempt->Scope;
	}
	if (bCorrectionReplay && Inputs.bHasReplayBasis)
	{
		Motion->StartFacing.Mode = EGGYGOActionMotionStartFacingMode::ReplayBasis;
		Motion->StartFacing.EntryActorRotation = Inputs.ReplayBasis;
	}
	else
	{
		// 无 basis 的旧纠正只保留显式 CurrentFacing 兼容；QualifiedInput 已在前置拒绝。
		Motion->StartFacing.Mode = !bCorrectionReplay && Step.StartFacingPolicy == EGGYGOComboStepFacingPolicy::QualifiedInput
			? EGGYGOActionMotionStartFacingMode::QualifiedInput : EGGYGOActionMotionStartFacingMode::CurrentFacing;
	}
	if (!Motion->AdoptTask(StartedMontageTask))
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d original Task [%s] resource ownership was rejected."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(StartedMontageTask));
		StartedMontageTask->TaskOwnerEnded();
		if (IsActivationCurrent(Original)) { RequestAbilityEnd(Original, true, true); }
		return false;
	}
	Motion->FX = Step.FX;
	Motion->FXWindow = Prepared.FXWindow;
	Motion->Movement = Inputs.Movement;
	Motion->Source = Prepared.MotionSource;
	Motion->TranslationScale = Step.MotionTranslationScale;
	Motion->EndSection = Step.EndSection;
	Motion->AttackGate.Mode = Step.InterruptionMode;
	Motion->AttackGate.NotifyName = Step.InterruptionNotifyName;
	Motion->AttackGate.TriggerPosition = Prepared.AttackDeclaration.TriggerPositionSeconds;
	Motion->AttackGate.AuthoredPosition = Prepared.AttackDeclaration.AuthoredPositionSeconds;
	Motion->AttackGate.TriggerTimeOffset = Prepared.AttackDeclaration.TriggerTimeOffsetSeconds;
	Motion->AttackGate.OpenFloorPosition = Prepared.AttackDeclaration.TriggerPositionSeconds;
	Motion->MovementGate.Mode = Step.MovementInterruptionMode;
	Motion->MovementGate.NotifyName = Step.MovementInterruptionNotifyName;
	Motion->MovementGate.TriggerPosition = Prepared.MovementDeclaration.TriggerPositionSeconds;
	Motion->MovementGate.AuthoredPosition = Prepared.MovementDeclaration.AuthoredPositionSeconds;
	Motion->MovementGate.TriggerTimeOffset = Prepared.MovementDeclaration.TriggerTimeOffsetSeconds;
	Motion->MovementGate.OpenFloorPosition = Prepared.MovementFloor;
	Motion->AbilityPath = GetPathNameSafe(this);
	Motion->bExpectFXBegin = Step.FX.Mode == EGGYGOComboFXMode::MontageWindow
		&& (Position > 0.0f ? Position : Motion->Source->MontageStartSeconds) < Motion->FXWindow.EndSeconds;
	// A correction starting beyond the authored window does not replay presentation from the skipped interval.
	Motion->bFXWindowClosed = !Motion->bExpectFXBegin;
	StepMotionResources = Motion;
	if (EntryAttempt.IsValid()) { EntryAttempt->EnteringMotion = Motion; }
	const float EffectivePlayRate = StartedMontageTask->GetEffectivePlayRate();
	Motion->EffectiveRate = EffectivePlayRate;
	const float MontageLength = Step.Montage->GetPlayLength();
	if (!FMath::IsFinite(EffectivePlayRate) || EffectivePlayRate <= 0.0f
		|| !FMath::IsFinite(MontageLength) || MontageLength <= 0.0f)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo：段 %d 的实际播放速率或时长无效。"), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	TArray<FName> AuthoredPointNames;
	if (!StartedMontageTask->ConfigureOriginalMontageWindows(Prepared.WindowDeclarations, MotionError))
	{
		FailStepMotion(Motion, FString::Printf(TEXT("原 Montage 窗口配置失败：%s"), *MotionError));
		return false;
	}
	if (Motion->AttackGate.IsAuthored()) { AuthoredPointNames.Add(Motion->AttackGate.NotifyName); }
	if (Motion->MovementGate.IsAuthored()) { AuthoredPointNames.Add(Motion->MovementGate.NotifyName); }
	// Ready 前一次原子配置全部启用点；两个 Disabled 动作不配置不存在的点。
	if (!AuthoredPointNames.IsEmpty() && !StartedMontageTask->ConfigureOriginalMontageNotifies(AuthoredPointNames, MotionError))
	{
		FailStepMotion(Motion, FString::Printf(TEXT("Required original authored point configuration failed: %s"), *MotionError));
		return false;
	}
	StartedMontageTask->SetStartTimeSeconds(Position);
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> WeakTask(StartedMontageTask);
	const TWeakPtr<FStepMotionResources> WeakMotion(Motion);
	// 段资源释放时先解绑这些回调；这里只确认回调仍属于当前段资源。
	const auto ResolveOriginalCaller = [WeakThis, WeakMotion]() -> ThisClass*
	{
		ThisClass* Self = WeakThis.Get();
		const TSharedPtr<FStepMotionResources> Pinned = WeakMotion.Pin();
		return Self && Pinned.IsValid() && Self->StepMotionResources == Pinned
			&& Pinned->Section != FStepMotionResources::ESection::Retired ? Self : nullptr;
	};
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent::FNativeCallbacks Callbacks;
	Callbacks.OnCompleted = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, WeakMotion](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageCompleted(WeakMotion.Pin()); }
		});
	Callbacks.OnInterrupted = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, Original](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageInterrupted(Original, Tag, Data); }
		});
	Callbacks.OnCancelled = Callbacks.OnInterrupted;
	// 原 GA 已进入 Cancel 时由 GAS 完成本次结束，Montage Task 的通知不再重入 Cancel。
	Callbacks.bNotifyNativeOnOwnerCancellation = false;
	Callbacks.OnBlendOut = FGGYGOPlayMontageAndWaitForEventDelegate::CreateLambda(
		[ResolveOriginalCaller, Original](FGameplayTag Tag, FGameplayEventData Data)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageBlendOut(Original, Tag, Data); }
		});
	Callbacks.WindowReceived = FGGYGOMontageWindowFactDelegate::CreateLambda(
		[ResolveOriginalCaller, WeakMotion](const FGGYGOMontageWindowFact& Fact)
		{
			if (ThisClass* Self = ResolveOriginalCaller()) { Self->HandleMontageWindow(WeakMotion.Pin(), Fact); }
		});
	Callbacks.SectionReceived = FGGYGOMontageSectionFactDelegate::CreateLambda(
		[WeakThis, WeakMotion](const FGGYGOMontageSectionFact& Fact)
		{
			if (ThisClass* Self = WeakThis.Get()) { Self->HandleMontageSection(WeakMotion.Pin(), Fact); }
		});
	Callbacks.NotifyReceived = FGGYGOMontageNotifyFactDelegate::CreateLambda(
		[WeakThis, WeakMotion](const FGGYGOMontageNotifyFact& Fact)
		{
			if (ThisClass* Self = WeakThis.Get()) { Self->HandleMontageNotify(WeakMotion.Pin(), Fact); }
		});
	Callbacks.OnFailed = FGGYGOMontageTaskFailureDelegate::CreateLambda(
		[ResolveOriginalCaller, WeakTask, WeakMotion, Original, ThisStepToken](const FGGYGOMontageTaskFailureFact& Fact)
		{
			// 失败在原 Task EndTask 之后交付，发送者已标记 Garbage；只认所属段资源仍为当前段。
			ThisClass* Self = ResolveOriginalCaller();
			UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const OriginalTask = WeakTask.Get(true);
			const TSharedPtr<FStepMotionResources> OriginalResource = WeakMotion.Pin();
			if (!Self || !OriginalTask || !OriginalResource.IsValid()) { return; }

			const FString OriginalLogContext = FString::Printf(
				TEXT("Ability='%s' Step=%d StepToken=%llu Task='%s' Montage='%s' Slot='%s' SourceValid=%d ExpectedMontage='%s' ExpectedSlot='%s' Stage=%u Instance=%d ExpectedInstance=%d Diagnostic='%s'"),
				*OriginalResource->AbilityPath, OriginalResource->StepIndex, static_cast<unsigned long long>(ThisStepToken),
				*GetPathNameSafe(OriginalTask), *GetPathNameSafe(Fact.Montage.Get()), *Fact.SlotName.ToString(),
				OriginalResource->Source.IsValid(),
				*GetPathNameSafe(OriginalResource->Source.IsValid() ? OriginalResource->Source->Montage.Get() : nullptr),
				*(OriginalResource->Source.IsValid() ? OriginalResource->Source->SlotName.ToString() : FString(TEXT("Unavailable"))),
				static_cast<uint32>(Fact.Stage), Fact.MontageInstanceId, OriginalResource->MontageInstanceId, *Fact.Diagnostic);
			if (!OriginalResource->MatchesFailure(Fact))
			{
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=RejectedFact %s; required failure did not identify this original resource"),
					*OriginalLogContext);
				return;
			}
			UE_LOG(LogGGYGOAbilitySystem, Error,
				TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=RequiredDependencyFailed %s"), *OriginalLogContext);
			// Required dependency failure ends this original even when user cancellation is disabled.
			const FGGYGOAbilityTerminationResult Result = Self->RequestAbilityEnd(Original, true, true);
			if (Result.IsAccepted())
			{
				UE_LOG(LogGGYGOAbilitySystem, Display,
					TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=AbilityEndResult %s Outcome=%d"),
					*OriginalLogContext, static_cast<int32>(Result.Outcome));
			}
			else
			{
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("[Combat.PlayerCombo.PoseFailure] Boundary=AbilityEndResult %s Outcome=%d; original End request was not accepted"),
					*OriginalLogContext, static_cast<int32>(Result.Outcome));
			}
			// Only copied diagnostic context and the returned result are accessed after the external End request.
		});
	const bool bRegistered = Motion->RegisterCallbacks(MoveTemp(Callbacks));
	if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get()))
	{
		Motion->ReleaseTask(false);
		return false;
	}
	if (!bRegistered)
	{
		FailStepMotion(Motion, TEXT("Original Task pre-Ready native registration failed."));
		return false;
	}
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：%s 段 %d，Montage=%s，Rate=%.2f。"),
		HasAuthority(&CurrentActivationInfo) ? TEXT("服务器") : TEXT("预测端"), Index + 1,
		*GetNameSafe(Step.Montage), Step.PlayRate);
	StartedMontageTask->ReadyForActivation();
	if (!IsStepCurrent(Original, ThisStepToken, WeakTask.Get()) || CurrentStep != Index) { return false; }
	if (!StartedMontageTask->IsActive())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d Task [%s] did not remain active after Ready."),
			*GetPathNameSafe(this), Index, *GetPathNameSafe(StartedMontageTask));
		RequestAbilityCancel(Original, true);
		return false;
	}
	// 默认收尾路径显式钉住，防止编辑器中意外关联到其他段或循环。
	// 引擎入口只改当前 Montage 实例的 NextSection，再更新复制数据或发 Server RPC，不执行本地回调。
	if (UAbilitySystemComponent* ASC = GetAbilitySystemComponentFromActorInfo())
	{
		ASC->CurrentMontageSetNextSectionName(Step.MainSection, Step.EndSection);
		ASC->CurrentMontageSetNextSectionName(Step.EndSection, NAME_None);
	}
	else
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] step %d original ASC is unavailable."), *GetPathNameSafe(this), Index);
		RequestAbilityCancel(Original, true);
		return false;
	}
	if (!InitializeStepMotion(Motion) || !IsMotionResourceCurrent(Motion)) { return false; }
	const FTimerDelegate WatchdogDelegate = FTimerDelegate::CreateLambda([WeakThis, WeakMotion, Original, ThisStepToken]()
	{
		ThisClass* Self = WeakThis.Get();
		const auto OriginalMotion = WeakMotion.Pin();
		if (Self && OriginalMotion.IsValid() && Self->StepMotionResources == OriginalMotion)
		{ Self->HandleWatchdog(Original, ThisStepToken); }
	});
	if (!Motion->ArmWatchdog(ActiveMesh.Get(), StepWorld.Get(), Position,
		MontageCompletionGraceSeconds, WatchdogDelegate, MotionError))
	{
		FailStepMotion(Motion, MotionError);
		return false;
	}
	return IsMotionResourceCurrent(Motion);
}

void UGGYGOPlayerComboAbility::ReleaseTraceWindow(const FGGYGOAbilityActivationHandle& Original)
{
	if (!ResourceActivation.HasSameActivation(Original)) { return; }
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const FGGYGOMeleeTraceWindowHandle OriginalWindow = TraceWindow;
	const FDelegateHandle Subscription = TraceHitSubscription;
	TraceWindow = {};
	TraceHitSubscription.Reset();
	if (StepMotionResources.IsValid()) { StepMotionResources->TraceWindowSource = {}; }
	if (!OriginalTrace.IsValid()) { return; }
	if (Subscription.IsValid()) { OriginalTrace->UnsubscribeWindowHit(Subscription); }
	if (OriginalTrace.IsValid() && OriginalWindow.HasWindow()) { OriginalTrace->CloseOwnedTraceWindow(OriginalWindow); }
}

void UGGYGOPlayerComboAbility::HandleStepInputRequest(const FGGYGOAbilityActivationHandle& Original,
	const FGGYGOAbilityStepInputRequest& Request)
{
	if (!IsActivationCurrent(Original) || !Request.HasRequest() || !IsValid(InputTask)) { return; }
	if (!OriginalWorld.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] input rejected: original World is unavailable."), *GetPathNameSafe(this));
		RequestAbilityCancel(Original, true);
		return;
	}
	const FGGYGOAbilityStepInputRequest OriginalRequest = Request;
	const int32 SourceStep = OriginalRequest.GetSourceStep();
	const int32 RequestId = OriginalRequest.GetRequestId();
	const bool bInitial = OriginalRequest.GetKind() == EGGYGOAbilityStepInputKind::Initial;
	if (bInitial)
	{
		if (RequestId != 0 || SourceStep != INDEX_NONE || CurrentStep != INDEX_NONE || CurrentStepToken != 0) { return; }
	}
	else
	{
		if (RequestId <= LastRequestId || RequestId > 65535) { return; }
		LastRequestId = RequestId;
	}
	if (StepEntryAttempts.ContainsByPredicate([&OriginalRequest](const auto& Entry)
		{ return Entry->InputRequest.HasSameRequest(OriginalRequest); })) { return; }
	const TSharedPtr<FStepMotionResources> OriginalMotion = StepMotionResources;
	const int32 Next = bInitial ? 0
		: (ComboSteps.IsValidIndex(CurrentStep) ? ComboSteps[CurrentStep].NextStepIndex : INDEX_NONE);
	const bool bGateAccepted = bInitial || (SourceStep == CurrentStep && IsMotionResourceCurrent(OriginalMotion)
		&& OriginalMotion->AttackGate.bOpen && Next != INDEX_NONE);
	const auto Attempt = MakeShared<FStepEntryAttempt>();
	Attempt->Activation = Original;
	Attempt->InputRequest = OriginalRequest;
	Attempt->InputTask = InputTask.Get();
	Attempt->World = OriginalWorld;
	Attempt->PreviousStepToken = CurrentStepToken;
	Attempt->PreviousStep = CurrentStep;
	Attempt->TargetStep = Next;
	Attempt->bGateAccepted = bGateAccepted;
	StepEntryAttempts.Add(Attempt);
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EntryRequest] Ability='%s' Frame=%llu World='%s' Kind=%u SourceStep=%d Request=%d CurrentStep=%d StepToken=%llu GateAccepted=%d Remote=%d"),
		*GetPathName(), static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(OriginalWorld.Get()),
		static_cast<uint32>(OriginalRequest.GetKind()), SourceStep, RequestId, CurrentStep,
		static_cast<unsigned long long>(CurrentStepToken), bGateAccepted, OriginalRequest.IsRemote());
	if (!bInitial && bGateAccepted)
	{
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[Combat.PlayerCombo.InterruptionDiagnostics] Boundary=AttackRequest Ability='%s' Step=%d StepToken=%llu Instance=%d MotionHandle=%d Request=%d TargetStep=%d"),
			*OriginalMotion->AbilityPath, CurrentStep, static_cast<unsigned long long>(CurrentStepToken),
			OriginalMotion->MontageInstanceId, OriginalMotion->MotionHandle, RequestId, Next);
		if (!IsStepPlayable(Next))
		{
			FailStepEntry(Attempt, FString::Printf(TEXT("Configured author-point attack target %d is not playable."), Next));
			return;
		}
	}
	// 门资格在原 press 处只判一次；评价完成事件只恢复该请求，不能把门前按键变为后来有效。
	ResumeStepEntry(Attempt);
}

void UGGYGOPlayerComboAbility::HandleInitialInputUnavailable(const FGGYGOAbilityActivationHandle& Original,
	const UGGYGOAbilityTask_WaitComboInput* OriginalInputTask)
{
	// ASC 已认证原终止来源；同 Binding Refresh 可撤销 work 资格，但不撤销原 pending 的归还。
	if (!ResourceActivation.HasSameActivation(Original) || !IsValid(OriginalInputTask) || InputTask != OriginalInputTask
		|| !HasAuthority(&CurrentActivationInfo)) { return; }
	// 第 0 段可已创建 Task，但尚未获 CMC 接受；段号本身不证明 Initial 已接受。
	// 完整 Initial 已提交或动作已接受时，此控制通知都不授终止权限。
	if (InitialInputRoute.HasSubmittedInitial()
		|| (StepMotionResources.IsValid() && StepMotionResources->bHasEntryActorRotation)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.InitialUnavailable] Boundary=Receive Ability='%s' Spec=%s ActivationKey=%s Task='%s' Step=%d StepToken=%llu"),
		*GetPathName(), *CurrentSpecHandle.ToString(), *CurrentActivationInfo.GetActivationPredictionKey().ToString(),
		*GetPathNameSafe(OriginalInputTask), CurrentStep, static_cast<unsigned long long>(CurrentStepToken));
	RequestAbilityEnd(Original, true, true);
}

void UGGYGOPlayerComboAbility::HandleMontageWindow(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageWindowFact& Fact)
{
	if (!IsMotionResourceCurrent(Resource) || !Resource->Task.IsValid()
		|| !Resource->Task->IsOriginalMontageWindowFact(Fact)) { return; }
	if (Fact.Montage != Resource->Source->Montage.Get() || Fact.Mesh != ActiveMesh
		|| Fact.MontageInstanceId == INDEX_NONE
		|| (Resource->MontageInstanceId != INDEX_NONE && Fact.MontageInstanceId != Resource->MontageInstanceId))
	{
		FailStepMotion(Resource, TEXT("已认证窗口不属于本段持有的原 Montage/Mesh/播放。"));
		return;
	}
	const FGGYGOMontageWindowDeclaration& Declaration = Fact.Declaration;
	if (Resource->FX.Mode == EGGYGOComboFXMode::MontageWindow
		&& Declaration.BeginEventTag == Resource->FX.BeginEventTag
		&& Declaration.EndEventTag == Resource->FX.EndEventTag)
	{
		HandleStepFXWindow(Resource, Fact);
	}
	else if (Declaration.BeginEventTag == GGYGOGameplayTags::Event_Montage_ComboWindowBegin
		&& Declaration.EndEventTag == GGYGOGameplayTags::Event_Montage_ComboWindowEnd)
	{
		if (Fact.Phase == EGGYGOMontageWindowPhase::Begin)
		{
			if (!Window.bClosed) { Window.bOpen = true; }
		}
		else if (Fact.Phase == EGGYGOMontageWindowPhase::End)
		{
			const int32 Pending = Window.PendingRequestId;
			Window.Close();
			if (Pending > 0) { RejectRequest(Resource->Activation, Pending); }
		}
	}
	else if (Declaration.BeginEventTag == GGYGOGameplayTags::Event_Montage_HitWindowBegin
		&& Declaration.EndEventTag == GGYGOGameplayTags::Event_Montage_HitWindowEnd
		&& HasAuthority(&CurrentActivationInfo))
	{
		if (Fact.Phase == EGGYGOMontageWindowPhase::Begin) { OpenTraceWindow(Resource, Declaration); }
		else if (Fact.Phase == EGGYGOMontageWindowPhase::End && Resource->TraceWindowSource.HasSameWindow(Declaration))
		{
			ReleaseTraceWindow(Resource->Activation);
		}
	}
}

void UGGYGOPlayerComboAbility::FailStepFX(const TSharedPtr<FStepMotionResources>& Resource, const FString& Reason)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("[Combat.PlayerCombo.FX] Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d Task='%s' required presentation failed: %s"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Resource->Source->Montage.Get()), Resource->MontageInstanceId,
		*GetPathNameSafe(GetStepFXTask()), *Reason);
	RequestAbilityEnd(Resource->Activation, true, true);
}

void UGGYGOPlayerComboAbility::HandleStepFXFailed(UGGYGOAbilityTask_PlayNiagaraEffect* Sender, const FString& Reason)
{
	// A failure sender may already be Finished. Authenticate the exact owned pointer and original step,
	// rather than requiring the failed FX dependency to remain live.
	if (!Sender || GetStepFXTask() != Sender) { return; }
	const TSharedPtr<FStepMotionResources> OriginalResource = StepMotionResources;
	FailStepFX(OriginalResource, Reason);
}

void UGGYGOPlayerComboAbility::ReleaseStepFX(const TSharedPtr<FStepMotionResources>& Resource, bool bNormalExit)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	Resource->ReleaseFX(bNormalExit);
}

void UGGYGOPlayerComboAbility::OnRemoveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec)
{
	// GAS 先尝试结束活动实例再通知移除；原生清理被拒（如本实例 ScopeLock 中）时实例随后被标记为垃圾，
	// 本段 FX lease 仍由本实例持有。发布只能由持有者关闭，这里只按 Abort 归还 FX，不代做其它资源清理。
	if (const TSharedPtr<FStepMotionResources> Remaining = StepMotionResources)
	{
		Remaining->ReleaseFX(false);
	}
	Super::OnRemoveAbility(ActorInfo, Spec);
}

void UGGYGOPlayerComboAbility::HandleStepFXWindow(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageWindowFact& Fact)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakPtr<FStepMotionResources> WeakResource(Resource);
	FGGYGOMontageFXLocalStartReadyDelegate Ready = FGGYGOMontageFXLocalStartReadyDelegate::CreateWeakLambda(
		this, [this, WeakResource](const FGGYGOMontageFXLocalStartReadyFact& ReadyFact)
		{
			HandleStepFXLocalStartReady(WeakResource.Pin(), ReadyFact);
		});
	FString Error;
	if (!Resource->ConsumeFXWindow(this, GET_FUNCTION_NAME_CHECKED(ThisClass, HandleStepFXFailed),
		Fact, MoveTemp(Ready), [WeakThis, Resource]()
		{
			const ThisClass* Self = WeakThis.Get();
			return Self && Self->IsMotionResourceCurrent(Resource);
		}, Error))
	{
		if (ThisClass* Self = WeakThis.Get()) { Self->FailStepFX(Resource, Error); }
	}
}

void UGGYGOPlayerComboAbility::HandleStepFXLocalStartReady(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageFXLocalStartReadyFact& Fact)
{
	if (!Resource.IsValid()) { return; }
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	FString Error;
	if (!Resource->ContinueFXLocalStart(Fact,
		[WeakThis, Resource]()
		{
			const ThisClass* Self = WeakThis.Get();
			return Self && Self->IsMotionResourceCurrent(Resource);
		}, Error))
	{
		if (ThisClass* Self = WeakThis.Get()) { Self->FailStepFX(Resource, Error); }
	}
}

void UGGYGOPlayerComboAbility::OpenTraceWindow(const TSharedPtr<FStepMotionResources>& Resource,
	const FGGYGOMontageWindowDeclaration& Declaration)
{
	if (!IsMotionResourceCurrent(Resource) || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	const FGGYGOAbilityActivationHandle Original = Resource->Activation;
	const FGGYGOComboStep Step = ComboSteps[CurrentStep];
	const int32 ExpectedStepIndex = CurrentStep;
	const uint64 ExpectedStepToken = CurrentStepToken;
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const TWeakObjectPtr<USkeletalMeshComponent> OriginalMesh(ActiveMesh.Get());
	const TWeakObjectPtr<UGGYGOAbilityTask_PlayMontageAndWaitForEvent> WeakTask(GetMontageTask());
	ReleaseTraceWindow(Original);
	ThisClass* Self = WeakThis.Get();
	if (!Self || !Self->IsMotionResourceCurrent(Resource)) { return; }
	if (!OriginalTrace.IsValid() || !OriginalMesh.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace open rejected: original Trace [%s] or Mesh [%s] is invalid."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalTrace.Get()), *GetPathNameSafe(OriginalMesh.Get()));
		RequestAbilityCancel(Original, true);
		return;
	}
	FGGYGOMeleeTraceWindowHandle OpenedWindow;
	const EGGYGOMeleeTraceWindowOpenResult OpenResult = OriginalTrace->TryOpenOwnedTraceWindow(
		Step.TraceShape, OriginalMesh.Get(), {}, OpenedWindow);
	Self = WeakThis.Get();
	if (!Self || !Self->IsMotionResourceCurrent(Resource))
	{
		if (OriginalTrace.IsValid() && OpenedWindow.HasWindow()) { OriginalTrace->CloseOwnedTraceWindow(OpenedWindow); }
		return;
	}
	if (OpenResult != EGGYGOMeleeTraceWindowOpenResult::Opened || !OpenedWindow.HasWindow())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace open failed: step %d, Trace [%s], Montage [%s], Mesh [%s], TraceShape Mode=%d Chains=%d, result %d."),
			*GetPathNameSafe(this), ExpectedStepIndex, *GetPathNameSafe(OriginalTrace.Get()), *GetPathNameSafe(Step.Montage),
			*GetPathNameSafe(OriginalMesh.Get()), static_cast<int32>(Step.TraceShape.Mode),
			Step.TraceShape.Chains.Num(), static_cast<int32>(OpenResult));
		RequestAbilityCancel(Original, true);
		return;
	}
	TraceWindow = OpenedWindow;
	Resource->TraceWindowSource = Declaration;
	FDelegateHandle Subscription;
	const EGGYGOMeleeTraceWindowSubscribeResult SubscribeResult = OriginalTrace->SubscribeWindowHit(OpenedWindow,
		FGGYGOMeleeTraceWindowHitDelegate::CreateLambda(
			[WeakThis, WeakTask, OriginalTrace, Original, ExpectedStepToken, OpenedWindow](
				const FGGYGOMeleeTraceWindowHandle& ReportedWindow, AActor* HitActor, const FHitResult& HitResult)
			{
				ThisClass* Self = WeakThis.Get();
				if (Self && Self->IsStepCurrent(Original, ExpectedStepToken, WeakTask.Get())
					&& OriginalTrace.IsValid() && Self->TraceComponent == OriginalTrace.Get()
					&& ReportedWindow == OpenedWindow && Self->TraceWindow == OpenedWindow
					&& OriginalTrace->QueryOwnedTraceWindow(OpenedWindow) == EGGYGOMeleeTraceWindowQueryResult::Active)
				{
					Self->HandleMeleeHit(Original, ExpectedStepToken, OpenedWindow, HitActor, HitResult);
				}
			}), Subscription);
	Self = WeakThis.Get();
	if (!Self || !Self->IsMotionResourceCurrent(Resource) || Self->TraceWindow != OpenedWindow)
	{
		if (OriginalTrace.IsValid() && Subscription.IsValid()) { OriginalTrace->UnsubscribeWindowHit(Subscription); }
		if (OriginalTrace.IsValid()) { OriginalTrace->CloseOwnedTraceWindow(OpenedWindow); }
		return;
	}
	TraceHitSubscription = Subscription;
	if (SubscribeResult != EGGYGOMeleeTraceWindowSubscribeResult::Subscribed || !Subscription.IsValid())
	{
		UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] owned trace subscription failed: Trace [%s], result %d."),
			*GetPathNameSafe(this), *GetPathNameSafe(OriginalTrace.Get()), static_cast<int32>(SubscribeResult));
		RequestAbilityCancel(Original, true);
	}
}


void UGGYGOPlayerComboAbility::RejectRequest(const FGGYGOAbilityActivationHandle& Original, int32 RequestId)
{
	SendAuthoritativeStep(Original, RequestId, false);
}

void UGGYGOPlayerComboAbility::SendAuthoritativeStep(const FGGYGOAbilityActivationHandle& Original, int32 RequestId, bool bAccepted)
{
	if (!IsActivationCurrent(Original) || !HasAuthority(&CurrentActivationInfo)
		|| !CurrentActorInfo || CurrentActorInfo->IsLocallyControlled()) { return; }
	UGGYGOAbilitySystemComponent* ASC = GetGGYGOAbilitySystemComponentFromActorInfo();
	UAnimInstance* AnimInstance = CurrentActorInfo->GetAnimInstance();
	if (ASC && AnimInstance && ComboSteps.IsValidIndex(CurrentStep))
	{
		const TSharedPtr<FStepMotionResources> Motion = StepMotionResources;
		if (!IsMotionResourceCurrent(Motion) || !Motion->bHasEntryActorRotation)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning,
				TEXT("PlayerCombo [%s] 请求 %d 的原段 basis 未就绪，拒绝发送猜测方向。"), *GetPathNameSafe(this), RequestId);
			return;
		}
		if (StepSyncRevision == MAX_int32)
		{
			UE_LOG(LogGGYGOAbilitySystem, Warning, TEXT("PlayerCombo：纠正修订序号已耗尽，跳过后续同步。"));
			return;
		}
		const float MontagePosition = AnimInstance->Montage_GetPosition(ComboSteps[CurrentStep].Montage);
		if (!FMath::IsFinite(MontagePosition) || MontagePosition < 0.0f
			|| MontagePosition > ComboSteps[CurrentStep].Montage->GetPlayLength()) { return; }
		FGameplayAbilityTargetDataHandle Correction(new FGGYGOComboCorrectionData(
			++StepSyncRevision, RequestId, CurrentStep, MontagePosition, Window.bOpen, Window.bClosed, bAccepted,
			CurrentStepRequestId, &Motion->EntryActorRotation));
		UE_LOG(LogGGYGOAbilitySystem, Display,
			TEXT("[Combat.PlayerCombo.CorrectionSend] Ability='%s' Frame=%llu World='%s' Spec=%s ActivationKey=%s Revision=%d Request=%d Step=%d StepOrigin=%d Accepted=%d Handle=%d Position=%.9g Basis=%s"),
			*GetPathName(), static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(OriginalWorld.Get()),
			*CurrentSpecHandle.ToString(), *CurrentActivationInfo.GetActivationPredictionKey().ToString(),
			StepSyncRevision, RequestId, CurrentStep, CurrentStepRequestId, bAccepted, Motion->MotionHandle,
			MontagePosition, *Motion->EntryActorRotation.ToString());
		ASC->ClientCorrectAbilityState(CurrentSpecHandle,
			CurrentActivationInfo.GetActivationPredictionKey(), Correction);
	}
}

void UGGYGOPlayerComboAbility::ReceiveAbilityCorrection(const FGameplayAbilityTargetDataHandle& Correction)
{
	const FGGYGOAbilityActivationHandle Original = ResourceActivation;
	if (!IsActivationCurrent(Original) || HasAuthority(&CurrentActivationInfo) || Correction.Num() != 1) { return; }
	const FGameplayAbilityTargetData* TargetData = Correction.Get(0);
	if (!TargetData || TargetData->GetScriptStruct() != FGGYGOComboCorrectionData::StaticStruct()) { return; }
	if (!IsActivationCurrent(Original)) { return; }
	const FGGYGOComboCorrectionData& ComboCorrection = static_cast<const FGGYGOComboCorrectionData&>(*TargetData);
	if (!ComboCorrection.HasValidFields() || !IsStepPlayable(ComboCorrection.ServerStep)) { return; }
	const float ServerMontageLength = ComboSteps[ComboCorrection.ServerStep].Montage->GetPlayLength();
	if (!FMath::IsFinite(ServerMontageLength) || ComboCorrection.Position > ServerMontageLength) { return; }
	const FQuat AuthoritativeEntryActorRotation = ComboCorrection.EntryActorRotation;
	// 只读接收当刻的预测资源；栈内裸指针和值不延长原资源或 Task 寿命。
	const FStepMotionResources* const PreMotion = StepMotionResources.Get();
	UGGYGOAbilityTask_PlayMontageAndWaitForEvent* const PreTask = GetMontageTask();
	const bool bPreTaskExists = IsValid(PreTask);
	const bool bPreTaskActive = bPreTaskExists && PreTask->IsActive();
	const bool bPreSourceCurrent = IsMotionResourceCurrent(StepMotionResources);
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.CorrectionReceive] Ability='%s' Frame=%llu World='%s' Spec=%s ActivationKey=%s Revision=%d Request=%d Step=%d StepOrigin=%d Accepted=%d Position=%.9g HasBasis=%d Basis=%s PreStep=%d PreOrigin=%d PreStepToken=%llu PreTask='%s' PreTaskExists=%d PreTaskActive=%d PreHandle=%d PreInstance=%d PreSourceCurrent=%d"),
		*GetPathName(), static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(OriginalWorld.Get()),
		*CurrentSpecHandle.ToString(), *CurrentActivationInfo.GetActivationPredictionKey().ToString(),
		ComboCorrection.Revision, ComboCorrection.RequestId, ComboCorrection.ServerStep,
		ComboCorrection.ServerStepRequestId, ComboCorrection.bAccepted, ComboCorrection.Position,
		ComboCorrection.bHasEntryActorRotation, *AuthoritativeEntryActorRotation.ToString(),
		CurrentStep, CurrentStepRequestId, static_cast<unsigned long long>(CurrentStepToken),
		*GetPathNameSafe(bPreTaskExists ? PreTask : nullptr), bPreTaskExists, bPreTaskActive,
		PreMotion ? PreMotion->MotionHandle : INDEX_NONE, PreMotion ? PreMotion->MontageInstanceId : INDEX_NONE, bPreSourceCurrent);
	CorrectPredictedStepForActivation(Original, ComboCorrection.Revision, ComboCorrection.RequestId, ComboCorrection.ServerStep,
		ComboCorrection.Position, ComboCorrection.bWindowOpen, ComboCorrection.bWindowClosed, ComboCorrection.bAccepted,
		ComboCorrection.ServerStepRequestId, ComboCorrection.bHasEntryActorRotation ? &AuthoritativeEntryActorRotation : nullptr);
}

void UGGYGOPlayerComboAbility::CorrectPredictedStep(int32 Revision, int32 RequestId, int32 ServerStep, float Position,
	bool bWindowOpen, bool bWindowClosed, bool bAccepted)
{
	const FGGYGOAbilityActivationHandle Original = ResourceActivation;
	CorrectPredictedStepForActivation(Original, Revision, RequestId, ServerStep, Position, bWindowOpen, bWindowClosed, bAccepted);
}

void UGGYGOPlayerComboAbility::CorrectPredictedStepForActivation(const FGGYGOAbilityActivationHandle& Original,
	int32 Revision, int32 RequestId, int32 ServerStep, float Position,
	bool bWindowOpen, bool bWindowClosed, bool bAccepted,
	int32 ServerStepRequestId, const FQuat* EntryActorRotation)
{
	if (!IsActivationCurrent(Original) || HasAuthority(&CurrentActivationInfo)
		|| Revision <= StepSyncRevision || RequestId <= 0 || RequestId > 65535
		|| !FMath::IsFinite(Position) || !IsStepPlayable(ServerStep)
		|| Position < 0.0f || Position > ComboSteps[ServerStep].Montage->GetPlayLength()
		|| (bWindowOpen && bWindowClosed)
		|| (EntryActorRotation && (ServerStepRequestId < 0 || ServerStepRequestId > 65535
			|| EntryActorRotation->ContainsNaN() || !EntryActorRotation->IsNormalized()))
		|| (!EntryActorRotation && ServerStepRequestId != INDEX_NONE)) { return; }
	if (!EntryActorRotation && ComboSteps[ServerStep].StartFacingPolicy == EGGYGOComboStepFacingPolicy::QualifiedInput)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayerCombo [%s] 服务器纠正缺少原段 basis，QualifiedInput 不可重采或补方向。"), *GetPathNameSafe(this));
		RequestAbilityEnd(Original, true, true);
		return;
	}
	StepSyncRevision = Revision;
	// 已确认的旧请求不回滚客户端更新的预测；服务器拒绝则撤销依赖该预测的段。
	if (bAccepted && (EntryActorRotation ? ServerStepRequestId < CurrentStepRequestId : RequestId <= CurrentStepRequestId)) { return; }
	if (!bAccepted) { Window.PendingRequestId = 0; }
	const int32 AuthoritativeStepOrigin = EntryActorRotation ? ServerStepRequestId : RequestId;
	if (ServerStep != CurrentStep || (EntryActorRotation && AuthoritativeStepOrigin != CurrentStepRequestId))
	{
		const int32 PreviousStep = CurrentStep;
		const uint64 PreviousStepToken = CurrentStepToken;
		const float SafePosition = FMath::Clamp(Position, 0.0f,
			FMath::Max(0.0f, ComboSteps[ServerStep].Montage->GetPlayLength() - 0.001f));
		if (!StartStep(Original, ServerStep, SafePosition, AuthoritativeStepOrigin, true, EntryActorRotation))
		{
			if (!IsActivationCurrent(Original) || StepSyncRevision != Revision) { return; }
			// ReadyForActivation can synchronously move the same activation to a newer step.
			// In that case this correction stack must leave the new task and its timer alone.
			if (CurrentStep != PreviousStep || CurrentStepToken != PreviousStepToken)
			{
				if (!IsValid(GetMontageTask()) || !GetMontageTask()->IsActive()) { RequestAbilityCancel(Original, true); }
				return;
			}
			RequestAbilityCancel(Original, true);
			return;
		}
		if (!IsActivationCurrent(Original) || StepSyncRevision != Revision || CurrentStep != ServerStep
			|| CurrentStepRequestId != AuthoritativeStepOrigin) { return; }
		Window.bOpen = bWindowOpen;
		Window.bClosed = bWindowClosed;
	}
	else if (EntryActorRotation)
	{
		const TSharedPtr<FStepMotionResources> Motion = StepMotionResources;
		if (!IsMotionResourceCurrent(Motion) || !Motion->bHasEntryActorRotation || !Motion->Movement.IsValid())
		{
			RequestAbilityEnd(Original, true, true);
			return;
		}
		if (!Motion->EntryActorRotation.Equals(*EntryActorRotation))
		{
			FString Error;
			const bool bRebased = Motion->Movement->RebaseMontageActionMotion(
				Motion->Scope, Motion->MotionHandle, *EntryActorRotation, Error);
			if (!IsMotionResourceCurrent(Motion) || StepSyncRevision != Revision
				|| CurrentStepRequestId != AuthoritativeStepOrigin) { return; }
			if (!bRebased)
			{
				FailStepMotion(Motion, FString::Printf(TEXT("原段权威基础方向重基失败：%s"), *Error));
				return;
			}
			Motion->EntryActorRotation = *EntryActorRotation;
			UE_LOG(LogGGYGOAbilitySystem, Display,
				TEXT("[Combat.PlayerCombo.StepFacing] Ability='%s' Step=%d StepOrigin=%d RebasedEntry=%s Frame=%llu World='%s' Handle=%d Instance=%d"),
				*Motion->AbilityPath, Motion->StepIndex, AuthoritativeStepOrigin, *Motion->EntryActorRotation.ToString(),
				static_cast<unsigned long long>(GFrameCounter), *GetPathNameSafe(OriginalWorld.Get()),
				Motion->MotionHandle, Motion->MontageInstanceId);
		}
	}
	// An accepted same-step correction also records its request origin; numeric step
	// order no longer expresses whether a subsequent confirmation belongs to the past.
	if (bAccepted) { CurrentStepRequestId = EntryActorRotation ? AuthoritativeStepOrigin : FMath::Max(CurrentStepRequestId, RequestId); }
	UE_LOG(LogGGYGOAbilitySystem, Display, TEXT("PlayerCombo：请求 %d，服务器%s，同步到段 %d。"),
		RequestId, bAccepted ? TEXT("确认") : TEXT("拒绝"), ServerStep + 1);
}

void UGGYGOPlayerComboAbility::HandleMontageCompleted(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource) || Resource->bMontageCompleted) { return; }
	// 原段真实结束即撤销入场等待；CMC 的合法末帧 drain 不延长原 press 的接续资格。
	ReleasePendingStepEntries();
	// 由原 Task 已认证的自然 Ended 回调到达此原消费者；与后续 CMC 接受分开。
	const auto Observation = Resource->Observation;
	if (Observation.IsValid()) { Observation->Facts.bTaskNaturalEnded = true; }
	if (Resource->bExpectFXBegin && !Resource->bFXBeginConsumed)
	{
		FailStepFX(Resource, TEXT("Original Montage completed without the required authenticated FX begin event."));
		return;
	}
	// Task completion accepts a drain; CMC still owns its consumed final interval.
	// 两门都关闭；之后晚到的点或 Section 事实因 bMontageCompleted 不能重开。
	Resource->AttackGate.Close();
	Resource->MovementGate.Close();
	ReleaseStepFX(Resource, true);
	if (!IsMotionResourceCurrent(Resource)) { return; }
	if (Observation.IsValid()) { Observation->Facts.bConsumerAccepted = true; }
	FString Error;
	const bool bDrainAccepted = Resource->AcceptMontageCompletion(Error);
	// 原 CMC 外调可以同步结束本 GA。此处只写已捕获原历史，不授退役源新的工作权。
	if (Observation.IsValid())
	{
		Observation->Facts.bDrainRequestReturned = true;
		Observation->Facts.bDrainAccepted = bDrainAccepted;
		Observation->Facts.bSourceCurrentAfterDrainReturn = IsMotionResourceCurrent(Resource);
		Observation->Facts.DrainDiagnostic = Error;
	}
	if (!bDrainAccepted)
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original Task completed but its CMC natural drain was rejected: %s"), *Error));
		return;
	}
	if (IsMotionResourceCurrent(Resource)) { TryCompleteStepMotion(Resource); }
}

void UGGYGOPlayerComboAbility::TryCompleteStepMotion(const TSharedPtr<FStepMotionResources>& Resource)
{
	if (!IsMotionResourceCurrent(Resource)) { return; }
	UE_LOG(LogGGYGOAbilitySystem, Display,
		TEXT("[Combat.PlayerCombo.EndDiagnostics] Boundary=NaturalCompletion Ability='%s' Step=%d StepToken=%llu Montage='%s' Instance=%d MotionHandle=%d TaskCompleted=%d MotionCompleted=%d"),
		*Resource->AbilityPath, Resource->StepIndex, static_cast<unsigned long long>(Resource->StepToken),
		*GetPathNameSafe(Resource->Source->Montage.Get()), Resource->MontageInstanceId, Resource->MotionHandle,
		Resource->bMontageCompleted, Resource->bMotionCompleted);
	if (Resource->IsComplete())
	{
		RequestAbilityEnd(Resource->Activation, HasAuthority(&CurrentActivationInfo), false);
	}
}

void UGGYGOPlayerComboAbility::HandleMontageInterrupted(const FGGYGOAbilityActivationHandle& Original,
	FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (IsActivationCurrent(Original)) { RequestAbilityCancel(Original, true); }
}

void UGGYGOPlayerComboAbility::HandleMontageBlendOut(const FGGYGOAbilityActivationHandle& Original,
	FGameplayTag EventTag, FGameplayEventData EventData)
{
	if (!IsActivationCurrent(Original)) { return; }
	const TSharedPtr<FStepMotionResources> Resource = StepMotionResources;
	if (!IsMotionResourceCurrent(Resource)) { return; }
	if (Resource->bExpectFXBegin && !Resource->bFXBeginConsumed)
	{
		FailStepFX(Resource, TEXT("Original natural blend-out began without the required authenticated FX begin event."));
		return;
	}
	ReleaseStepFX(Resource, true);
	if (!IsMotionResourceCurrent(Resource)) { return; }
	FString Error;
	// The original Task sends this callback only for a native non-interrupted blend-out.
	// It grants CMC permission to consume the configured last-frame tail, never motion completion.
	if (!Resource->AcceptNaturalBlendOut(Error))
	{
		FailStepMotion(Resource, FString::Printf(TEXT("Original natural blend-out fact was rejected by CMC: %s"), *Error));
		return;
	}
	if (!IsMotionResourceCurrent(Resource)) { return; }
	const int32 Pending = Window.PendingRequestId;
	Window.Close();
	const uint64 ExpectedStepToken = CurrentStepToken;
	ReleaseTraceWindow(Original);
	if (!IsActivationCurrent(Original) || CurrentStepToken != ExpectedStepToken) { return; }
	if (Pending > 0) { RejectRequest(Original, Pending); }
}

void UGGYGOPlayerComboAbility::HandleWatchdog(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken)
{
	if (!IsActivationCurrent(Original) || CurrentStepToken != ExpectedStepToken) { return; }
	const TSharedPtr<FStepMotionResources> Resource = StepMotionResources;
	if (!Resource.IsValid() || Resource->Section == FStepMotionResources::ESection::Retired
		|| Resource->StepToken != ExpectedStepToken || CurrentStep != Resource->StepIndex
		|| !Resource->Activation.HasSameActivation(Original)) { return; }
	// Missing completion may include a vanished Task. Authenticate our exact owned resource,
	// rather than requiring the failed dependency to stay alive before we can end its owner.
	UE_LOG(LogGGYGOAbilitySystem, Error,
		TEXT("PlayerCombo [%s] Step %d Montage [%s] CMC [%s] bounded natural completion timed out: StepToken=%llu Instance=%d Handle=%d TaskCompleted=%d MotionCompleted=%d TaskAvailable=%d."),
		*Resource->AbilityPath, Resource->StepIndex, *GetPathNameSafe(Resource->Source->Montage.Get()),
		*GetPathNameSafe(Resource->Movement.Get()), static_cast<unsigned long long>(Resource->StepToken),
		Resource->MontageInstanceId, Resource->MotionHandle, Resource->bMontageCompleted, Resource->bMotionCompleted, Resource->Task.IsValid());
	RequestAbilityEnd(Original, true, true);
}

void UGGYGOPlayerComboAbility::HandleMeleeHit(const FGGYGOAbilityActivationHandle& Original, uint64 ExpectedStepToken,
	const FGGYGOMeleeTraceWindowHandle& OriginalWindow, AActor* HitActor, const FHitResult& HitResult)
{
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> OriginalTrace(TraceComponent.Get());
	const TWeakObjectPtr<ThisClass> WeakThis(this);
	const auto IsOriginalHitCurrent = [WeakThis, Original, ExpectedStepToken, OriginalWindow, OriginalTrace]()
	{
		const ThisClass* Self = WeakThis.Get();
		return Self && Self->IsActivationCurrent(Original) && Self->CurrentStepToken == ExpectedStepToken && Self->TraceWindow == OriginalWindow
			&& OriginalTrace.IsValid() && Self->TraceComponent == OriginalTrace.Get()
			&& OriginalTrace->QueryOwnedTraceWindow(OriginalWindow) == EGGYGOMeleeTraceWindowQueryResult::Active;
	};
	if (!IsOriginalHitCurrent() || !HasAuthority(&CurrentActivationInfo)
		|| !IsValid(HitActor) || HitActor == GetAvatarActorFromActorInfo() || !ComboSteps.IsValidIndex(CurrentStep)) { return; }
	const FGGYGOComboStep Step = ComboSteps[CurrentStep];
	const FGameplayTag OriginalHitCueTag = Step.HitCueTag;
	UAbilitySystemComponent* TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(HitActor);
	if (!IsOriginalHitCurrent()) { return; }
	AActor* SourceAvatar = GetAvatarActorFromActorInfo();
	if (!IsValid(TargetASC) || !IsValid(SourceAvatar)) { return; }
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalTargetASC(TargetASC);
	const TWeakObjectPtr<UAbilitySystemComponent> OriginalSourceASC(GetAbilitySystemComponentFromActorInfo());
	const FString AbilityPath = GetPathNameSafe(this);
	const FString MontagePath = GetPathNameSafe(Step.Montage);
	const FString SourcePath = GetPathNameSafe(SourceAvatar);
	const FString TargetPath = GetPathNameSafe(HitActor);
	const int32 OriginalStep = CurrentStep;
	const auto AbortOriginalHit = [WeakThis, Original, AbilityPath, MontagePath, SourcePath, TargetPath, OriginalStep](const FString& Reason)
	{
		UE_LOG(LogGGYGOAbilitySystem, Error,
			TEXT("PlayerCombo runtime hit Ability [%s] Source [%s] Target [%s] Step %d Montage [%s] failed: %s."),
			*AbilityPath, *SourcePath, *TargetPath, OriginalStep, *MontagePath, *Reason);
		if (ThisClass* Self = WeakThis.Get())
		{
			// Internal dependency failure ends this captured original, even if user cancellation is disabled.
			// GA validates stale/busy sources and owns cleanup/continuation; there is no alternate request.
			const FGGYGOAbilityTerminationResult Result = Self->RequestAbilityEnd(Original, true, true);
			if (!Result.IsAccepted())
			{
				UE_LOG(LogGGYGOAbilitySystem, Error,
					TEXT("PlayerCombo runtime hit Ability [%s] Step %d original failure End was not accepted: outcome %d."),
					*AbilityPath, OriginalStep, static_cast<int32>(Result.Outcome));
			}
		}
	};
	// GE 与伤害模式来自本次激活的预备配置快照，段数值已在准入校验；GE 类在激活期间被卸载或替换时中止原激活。
	const TSharedPtr<const FPreparedComboConfiguration> Prepared = PreparedActivation;
	if (!Prepared.IsValid() || !Prepared->IsDamageEffectCurrent())
	{
		AbortOriginalHit(TEXT("ComboDamage: prepared-damage-effect-unavailable"));
		return;
	}
	const TSubclassOf<UGameplayEffect> PreparedDamageEffect(Prepared->DamageEffect.Get());
	const FString EffectPath = GetPathNameSafe(PreparedDamageEffect.Get());
	FGGYGOHitEffectPayload HitPayload;
	if (!BuildHitEffectPayload(TargetASC, PreparedDamageEffect, GetAbilityLevel(), HitResult,
		SourceAvatar->GetActorLocation(), HitPayload))
	{
		AbortOriginalHit(FString::Printf(TEXT("BuildHitEffectPayload returned false for GE [%s]; see the original Builder diagnostic."), *EffectPath));
		return;
	}
	if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
	if (HitPayload.EffectSpec.IsValid())
	{
		FGameplayEffectSpec& Spec = *HitPayload.EffectSpec.Data.Get();
		// Builder 的蓝图扩展可能替换 Def 或写入键。预备模式只对所选 GE 的 CDO 成立，Def 不同即沿原身份收尾。
		const UClass* const PreparedClass = PreparedDamageEffect.Get();
		if (!PreparedClass || Spec.Def.Get() != PreparedClass->GetDefaultObject(false))
		{
			AbortOriginalHit(TEXT("ComboDamage: returned-spec-definition-mismatch"));
			return;
		}
		if (Prepared->DamageMode == EGGYGOComboDamageMode::Multiplier)
		{
			const float* Existing = Spec.SetByCallerTagMagnitudes.Find(GGYGOGameplayTags::SetByCaller_DamageMultiplier);
			if (Spec.SetByCallerTagMagnitudes.Contains(GGYGOGameplayTags::SetByCaller_Damage)
				|| (Existing && (!FMath::IsFinite(*Existing) || *Existing != Step.DamageMultiplier)))
			{
				AbortOriginalHit(TEXT("ComboDamage: conflicting-damage-keys-or-multiplier"));
				return;
			}
			Spec.SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_DamageMultiplier, Step.DamageMultiplier);
		}
		else
		{
			if (Spec.SetByCallerTagMagnitudes.Contains(GGYGOGameplayTags::SetByCaller_DamageMultiplier))
			{
				AbortOriginalHit(TEXT("ComboDamage: multiplier-key-in-absolute-mode"));
				return;
			}
			Spec.SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_Damage, Step.Damage);
		}
		HitPayload.EffectSpec.Data->SetSetByCallerMagnitude(GGYGOGameplayTags::SetByCaller_PoiseDamage, Step.PoiseDamage);
		if (OriginalSourceASC.IsValid())
		{
			OriginalSourceASC->ApplyGameplayEffectSpecToTarget(*HitPayload.EffectSpec.Data.Get(), OriginalTargetASC.Get());
			if (!IsOriginalHitCurrent() || !OriginalTargetASC.IsValid()) { return; }
		}
	}
	if (OriginalHitCueTag.IsValid())
	{
		// 碰撞已经成立；GE 被免疫或拒绝不应吞掉命中表现。
		OriginalTargetASC->ExecuteGameplayCue(OriginalHitCueTag, HitPayload.CueParameters);
	}
}

void UGGYGOPlayerComboAbility::CleanupAbilityResourcesForTermination(const FGGYGOAbilityTerminationContext& Context)
{
	if (!ResourceActivation.HasSameActivation(Context.GetOriginalActivation()))
	{
		if (ResourceActivation.HasActivation() || InitialInputRoute.HasRoute() || !StepEntryAttempts.IsEmpty()
			|| PreparedActivation.IsValid()
			|| StepMotionResources.IsValid() || InputTask || TraceComponent || ActiveMesh || InputCallbackRegistration.IsValid()
			|| TraceWindow.HasWindow() || TraceHitSubscription.IsValid()
			|| OriginalWorld.IsValid() || bChangedMeshTick || bAddedMeshPrerequisite)
		{
			UE_LOG(LogGGYGOAbilitySystem, Error, TEXT("PlayerCombo [%s] resource cleanup rejected: GA original activation does not match resource source."),
				*GetPathNameSafe(this));
		}
		Super::CleanupAbilityResourcesForTermination(Context);
		return;
	}
	const TWeakObjectPtr<UGGYGOAbilityTask_WaitComboInput> EndingInputTask(InputTask.Get());
	const TWeakObjectPtr<UGGYGOMeleeTraceComponent> EndingTraceComponent(TraceComponent.Get());
	const TWeakObjectPtr<USkeletalMeshComponent> EndingMesh(ActiveMesh.Get());
	const FDelegateHandle EndingInputRegistration = InputCallbackRegistration;
	const FGGYGOMeleeTraceWindowHandle EndingTraceWindow = TraceWindow;
	const FDelegateHandle EndingTraceSubscription = TraceHitSubscription;
	const bool bRestoreMesh = bChangedMeshTick;
	const bool bRemovePrerequisite = bAddedMeshPrerequisite;
	const EVisibilityBasedAnimTickOption MeshTickToRestore = SavedMeshTick;
	const bool bUpdateRateToRestore = bSavedUpdateRateOptimizations;
	const TSharedPtr<FStepMotionResources> EndingMotion = MoveTemp(StepMotionResources);
	const FGGYGOAbilityInitialInputRoute EndingInitialRoute = MoveTemp(InitialInputRoute);
	const auto EndingEntries = MoveTemp(StepEntryAttempts);
	for (const auto& Entry : EndingEntries) { Entry->Phase = FStepEntryAttempt::EPhase::Retired; }
	const bool bNaturalEnd = !Context.WasCancelled() && EndingMotion.IsValid()
		&& EndingMotion->bMontageCompleted && EndingMotion->bMotionCompleted;
	if (EndingMotion.IsValid() && EndingMotion->Observation.IsValid())
	{
		auto& Facts = EndingMotion->Observation->Facts;
		Facts.bCleanupObserved = true;
		Facts.bCleanupCancelled = Context.WasCancelled();
	}

	// Detach every original member before unregistering, restoring or ending external resources.
	ResourceActivation = {};
	PreparedActivation.Reset();
	Window.Close();
	CurrentStep = INDEX_NONE;
	CurrentStepRequestId = 0;
	CurrentStepToken = 0;
	InputTask = nullptr;
	TraceComponent = nullptr;
	ActiveMesh = nullptr;
	OriginalWorld.Reset();
	InputCallbackRegistration.Reset();
	TraceWindow = {};
	TraceHitSubscription.Reset();
	bChangedMeshTick = false;
	bAddedMeshPrerequisite = false;
	bSavedUpdateRateOptimizations = false;
	SavedMeshTick = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;

	// 先摘所有原成员，再沿原终止来源归还；已提交 Initial 的路由不会发取消。
	EndingInitialRoute.NotifyInitialUnavailable();
	for (const auto& Entry : EndingEntries)
	{
		UGGYGOPlayerInput::UnsubscribeMovementInputEvaluation(
			Entry->InputRequest.GetOriginalEdgeToken(), Entry->EvaluationSubscription);
	}
	if (EndingMotion.IsValid())
	{
		const EGGYGOActionMotionReleaseReason MotionReason = bNaturalEnd
			? EGGYGOActionMotionReleaseReason::Completed
			: (Context.WasCancelled() ? EGGYGOActionMotionReleaseReason::Cancelled : EGGYGOActionMotionReleaseReason::OwnerInvalidated);
		EndingMotion->Release(MotionReason);
	}

	if (EndingTraceComponent.IsValid())
	{
		if (EndingTraceSubscription.IsValid()) { EndingTraceComponent->UnsubscribeWindowHit(EndingTraceSubscription); }
		if (EndingTraceComponent.IsValid() && EndingTraceWindow.HasWindow()) { EndingTraceComponent->CloseOwnedTraceWindow(EndingTraceWindow); }
		if (EndingTraceComponent.IsValid() && EndingMesh.IsValid() && bRemovePrerequisite)
		{
			EndingTraceComponent->RemoveTickPrerequisiteComponent(EndingMesh.Get());
		}
	}
	if (EndingMotion.IsValid())
	{
		EndingMotion->ReleaseFX(bNaturalEnd);
		EndingMotion->ClearWatchdog();
	}
	if (EndingMesh.IsValid() && bRestoreMesh)
	{
		EndingMesh->VisibilityBasedAnimTickOption = MeshTickToRestore;
		EndingMesh->bEnableUpdateRateOptimizations = bUpdateRateToRestore;
	}
	if (EndingInputTask.IsValid())
	{
		EndingInputTask->UnregisterNativeCallback(EndingInputRegistration);
		if (EndingInputTask.IsValid()) { EndingInputTask->TaskOwnerEnded(); }
	}
	if (EndingMotion.IsValid()) { EndingMotion->ReleaseTask(true); }
	Super::CleanupAbilityResourcesForTermination(Context);
}
