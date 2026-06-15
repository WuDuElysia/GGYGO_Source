/**
 * @file GGYGOAnimInstance.cpp
 * @brief 动画实例实现 — 状态驱动播放 + Config 混合时间 + 根运动提取
 *
 * 动画资产来源：自身 EditAnywhere 属性（IdleAnim / RunStartAnim / RunLoopAnim / RunEndAnim）
 * 混合时间来源：UCharConfigData（PerStateBlendOverrides），为空时回退 0.2s
 */
#include "Animation/GGYGOAnimInstance.h"
#include "BaseCharacter.h"
#include "Animation/AnimSequence.h"

/** 蒙太奇播放的 Slot 名称，与 ABP AnimGraph 中的 Slot 节点名一致 */
static const FName SlotName = FName("DefaultSlot");

/** 无 Config 时的全局默认混合时间 */
static const float LegacyBlendDuration = 0.2f;

// ================================================================
//  NativeUpdateAnimation — 每帧由引擎自动调用
// ================================================================

void UGGYGOAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

	ABaseCharacter* Owner = Cast<ABaseCharacter>(TryGetPawnOwner());
	if (!Owner) return;

	// 缓存 Config 引用
	CachedConfig = Owner->GetCharacterConfig();

	if (bFirstUpdate)
	{
		bFirstUpdate = false;

		// 从 Config 获取最短过渡时长
		if (CachedConfig)
		{
			Owner->SetRunMinDurations(
				CachedConfig->RunStartMinDuration,
				CachedConfig->RunEndMinDuration);
		}

		EnsureLoopingMontages();
		ApplyStateAnimation(Owner->GetCurrentState());
		LastState = Owner->GetCurrentState();
		return;
	}

	CurrentState = Owner->GetCurrentState();
	bIsMoving    = Owner->IsMoving();

	if (CurrentState != LastState)
	{
		ApplyStateAnimation(CurrentState);
		LastState = CurrentState;
	}

	TickAnimCompletion(DeltaSeconds);
	ExtractRootMotionDelta(DeltaSeconds);
}

// ================================================================
//  Config 查询方法（仅混合时间和播放速率从 Config 读取）
// ================================================================

UCharConfigData* UGGYGOAnimInstance::GetActiveConfig() const
{
	return CachedConfig;
}

float UGGYGOAnimInstance::GetBlendInDuration(ECharacterStateType TargetState) const
{
	if (CachedConfig) { return CachedConfig->GetBlendInTime(TargetState); }
	return LegacyBlendDuration;
}

float UGGYGOAnimInstance::GetBlendOutDuration(ECharacterStateType SourceState) const
{
	if (CachedConfig) { return CachedConfig->GetBlendOutTime(SourceState); }
	return LegacyBlendDuration;
}

UAnimMontage* UGGYGOAnimInstance::GetLoopMontage(ECharacterStateType State) const
{
	switch (State)
	{
	case ECharacterStateType::Idle:      return IdleMontage;
	case ECharacterStateType::RunLoop:    return RunLoopMontage;
	case ECharacterStateType::InAir:      return InAirMontage;
	case ECharacterStateType::Stunned:    return StunnedMontage;
	default:                              return nullptr;
	}
}

/**
 * 根据状态获取对应的 AnimSequence（直接读自身属性，仅支持 4 个基础状态）
 */
static UAnimSequence* GetAnimForState(ECharacterStateType State, UGGYGOAnimInstance* Instance)
{
	switch (State)
	{
	case ECharacterStateType::Idle:      return Instance->IdleAnim;
	case ECharacterStateType::RunStart:   return Instance->RunStartAnim;
	case ECharacterStateType::RunLoop:    return Instance->RunLoopAnim;
	case ECharacterStateType::RunEnd:     return Instance->RunEndAnim;
	default:                              return nullptr;
	}
}

/** 判断状态是否为循环播放 */
static bool IsLoopingState(ECharacterStateType State)
{
	switch (State)
	{
	case ECharacterStateType::Idle:
	case ECharacterStateType::RunLoop:
		return true;
	default:
		return false;
	}
}

// ================================================================
//  EnsureLoopingMontages — 预创建循环状态的蒙太奇缓存
// ================================================================

void UGGYGOAnimInstance::EnsureLoopingMontages()
{
	UAnimSequence* IdleSeq = IdleAnim;
	UAnimSequence* RunSeq  = RunLoopAnim;

	float IdleBlend = GetBlendInDuration(ECharacterStateType::Idle);
	float RunBlend  = GetBlendInDuration(ECharacterStateType::RunLoop);

	if (IdleSeq && !IdleMontage)
	{
		IdleMontage = PlaySlotAnimationAsDynamicMontage(IdleSeq, SlotName, IdleBlend, IdleBlend, 1.0f, 9999, -1.f);
		if (IdleMontage) { IdleMontage->SyncGroup = FName("Locomotion"); }
	}
	if (RunSeq && !RunLoopMontage)
	{
		RunLoopMontage = PlaySlotAnimationAsDynamicMontage(RunSeq, SlotName, RunBlend, RunBlend, 1.0f, 9999, -1.f);
		if (RunLoopMontage) { RunLoopMontage->SyncGroup = FName("Locomotion"); }
	}
}

// ================================================================
//  ApplyStateAnimation — 核心方法：根据角色状态切换动画
// ================================================================

void UGGYGOAnimInstance::ApplyStateAnimation(ECharacterStateType State)
{
	// ---- 从自身属性解析目标动画 ----
	UAnimSequence* TargetSeq = GetAnimForState(State, this);
	UAnimMontage* LoopMont  = GetLoopMontage(State);
	bool bLoop = (LoopMont != nullptr);

	if (!TargetSeq && !LoopMont) { return; }

	// ---- 混合时间（★ 核心：每转换对独立控制） ----
	float BlendIn  = GetBlendInDuration(State);
	float BlendOut = GetBlendOutDuration(LastState);

	// ---- 播放速率 ----
	float PlayRate = 1.0f;
	if (CachedConfig)
	{
		PlayRate = CachedConfig->GetPlayRateForState(State);
	}

	// ---- 重置根运动提取缓存 ----
	CurrentPlayingAnim     = TargetSeq;
	bRootMotionInitialized = false;
	LastExtractedTime      = 0.f;
	RootMotionDelta        = FVector::ZeroVector;

	// ---- 执行切换 ----
	if (bLoop && LoopMont)
	{
		Montage_Play(LoopMont, PlayRate);
	}
	else if (!bLoop && TargetSeq)
	{
		UAnimMontage* DynamicMontage = PlaySlotAnimationAsDynamicMontage(
			TargetSeq, SlotName, BlendIn, BlendOut, PlayRate, 1, -1.f);
		if (DynamicMontage)
		{
			// 根据状态分配 Sync Group
			FName SyncGroup;
			switch (State)
			{
			case ECharacterStateType::Idle:      SyncGroup = FName("Locomotion"); break;
			case ECharacterStateType::RunStart:   SyncGroup = FName("Locomotion"); break;
			case ECharacterStateType::RunLoop:    SyncGroup = FName("Locomotion"); break;
			case ECharacterStateType::RunEnd:     SyncGroup = FName("Locomotion"); break;
			case ECharacterStateType::Dodging:    SyncGroup = FName("Combat");     break;
			case ECharacterStateType::Attacking:  SyncGroup = FName("Combat");     break;
			default:                              SyncGroup = NAME_None;            break;
			}
			DynamicMontage->SyncGroup = SyncGroup;
		}
	}

	AnimTimeElapsed = 0.f;
}

// ================================================================
//  TickAnimCompletion — 非循环动画播放完成检测
// ================================================================

void UGGYGOAnimInstance::TickAnimCompletion(float DeltaSeconds)
{
	if (IsLoopingState(LastState)) { return; }

	UAnimSequence* ActiveSeq = GetAnimForState(LastState, this);
	if (!ActiveSeq) return;

	AnimTimeElapsed += DeltaSeconds;
	if (AnimTimeElapsed >= ActiveSeq->GetPlayLength())
	{
		NotifyAnimFinished();
	}
}

// ================================================================
//  NotifyAnimFinished — 动画播完通知 BaseCharacter 切换状态
// ================================================================

void UGGYGOAnimInstance::NotifyAnimFinished()
{
	ABaseCharacter* Owner = Cast<ABaseCharacter>(TryGetPawnOwner());
	if (!Owner) return;

	switch (LastState)
	{
	case ECharacterStateType::RunStart:
		Owner->NotifyRunStartFinished();
		break;
	case ECharacterStateType::RunEnd:
		Owner->NotifyRunEndFinished();
		break;
	default:
		break;
	}

	AnimTimeElapsed = 0.f;
}

// ================================================================
//  AnimNotify 回调（备用接口）
// ================================================================

void UGGYGOAnimInstance::OnRunStartFinished()
{
	if (ABaseCharacter* Owner = Cast<ABaseCharacter>(TryGetPawnOwner()))
	{
		Owner->NotifyRunStartFinished();
	}
}

void UGGYGOAnimInstance::OnRunEndFinished()
{
	if (ABaseCharacter* Owner = Cast<ABaseCharacter>(TryGetPawnOwner()))
	{
		Owner->NotifyRunEndFinished();
	}
}

// ================================================================
//  ExtractRootMotionDelta — 从 AnimSequence 根骨骼轨道提取帧位移
// ================================================================

void UGGYGOAnimInstance::ExtractRootMotionDelta(float DeltaSeconds)
{
	RootMotionDelta = FVector::ZeroVector;

	if (!CurrentPlayingAnim) { return; }
	if (!CurrentPlayingAnim->HasRootMotion()) { return; }

	float CurrentTime = 0.f;
	bool bGotTimeFromMontage = false;

	const TArray<FAnimMontageInstance*>& ActiveInstances = MontageInstances;
	for (const FAnimMontageInstance* MI : ActiveInstances)
	{
		if (!MI || !MI->IsValid()) continue;
		UAnimMontage* PlayingMont = MI->Montage;
		if (PlayingMont)
		{
			CurrentTime = MI->GetPosition();
			bGotTimeFromMontage = true;
			break;
		}
	}

	if (!bGotTimeFromMontage)
	{
		CurrentTime = AnimTimeElapsed;
	}

	float SeqLength = CurrentPlayingAnim->GetPlayLength();
	if (SeqLength <= KINDA_SMALL_NUMBER) { return; }
	if (CurrentTime >= SeqLength)
	{
		CurrentTime = FMath::Fmod(CurrentTime, SeqLength);
	}

	float PrevTime = LastExtractedTime;
	float TimeDelta = CurrentTime - PrevTime;

	bool bCrossedLoopBoundary = (CurrentTime < PrevTime);
	if (bCrossedLoopBoundary && bRootMotionInitialized)
	{
		FTransform Part1 = CurrentPlayingAnim->ExtractRootMotion(PrevTime, SeqLength - PrevTime, false);
		FTransform Part2 = CurrentPlayingAnim->ExtractRootMotion(0.f, CurrentTime, false);
		RootMotionDelta = Part1.GetTranslation() + Part2.GetTranslation();
	}
	else if (bRootMotionInitialized)
	{
		FTransform RootTransform = CurrentPlayingAnim->ExtractRootMotion(PrevTime, TimeDelta, false);
		RootMotionDelta = RootTransform.GetTranslation();
	}
	else
	{
		bRootMotionInitialized = true;
	}

	LastExtractedTime = CurrentTime;
}
