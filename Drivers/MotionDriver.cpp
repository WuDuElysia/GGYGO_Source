/**
 * @file MotionDriver.cpp
 * @brief 运动驱动器实现
 *
 * 动画曲线驱动架构：
 *   - RM_Speed 是实际速度主值（cm/s），RootMotionScale 只缩放最终速度
 *   - 普通 walkrun 无 dir 曲线：移动方向 = 玩家输入方向 DesiredWorldMoveDir
 *   - TurnBack 有固定方向曲线（dir 约定 X=左右、Y=前后）：
 *       · d0 段移动方向 = dir 曲线相对进入时捕获的角色前/右方向映射到世界
 *       · d1 段像 walkrun 朝输入方向移动
 *   - RM_Dist 仅用于判断累计曲线是否回退和当前是否存在曲线源，不参与位移叠加
 *   - SetRootMotionMode(IgnoreRootMotion) 保证曲线驱动不会与引擎 Root Motion 叠加
 */
#include "Drivers/MotionDriver.h"
#include "CoreGlobals.h"
#include "Data/Runtime/RuntimeData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"
#include "BaseCharacter.h"
#include "Data/Config/UCharConfigData.h"

namespace
{
	const TCHAR* GetCurveDirectionSource(const FCharacterMovementCommand& Command)
	{
		if (!Command.AnimCurveVelocityDirection.IsNearlyZero())
		{
			return Command.bHasAuthoredVelocityDirection
				? TEXT("RM_VelocityDir")
				: TEXT("RM_PosDelta");
		}

		if (!Command.RootMotionDelta.IsNearlyZero())
		{
			return TEXT("RM_PosDelta");
		}

		if (Command.TurnBackPhase != ETurnBackPhase::None)
		{
			return Command.bTurnBackSecondSegment
				? TEXT("TurnBackD1Fallback")
				: TEXT("TurnBackD0Fallback");
		}

		if (!Command.DesiredWorldMoveDir.IsNearlyZero())
		{
			return TEXT("DesiredWorldMoveDirFallback");
		}

		return TEXT("None");
	}
}

void FMotionDriver::EnsureRootMotionIgnored()
{
	if (!Mesh)
	{
		return;
	}

	if (UAnimInstance* AnimInst = Mesh->GetAnimInstance())
	{
		AnimInst->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
	}
}

void FMotionDriver::Init(ACharacter* InOwner)
{
	RootMotionScale = 1.f;
	Owner = InOwner;
	Movement = nullptr;
	Mesh = nullptr;
	bTurnBackDirectionActive = false;
	bTurnBackRotationOverrideActive = false;
	bSavedOrientRotationToMovement = false;
	bSavedUseControllerDesiredRotation = false;
	bSavedUseControllerRotationYaw = false;
	TurnBackDirectionBasisForward = FVector(1.f, 0.f, 0.f);
	TurnBackAnimationElapsed = 0.f;
	TurnBackAnimationLength = 0.f;
	bTurnBackAnimationActive = false;
	bTurnBackAnimationComplete = false;
	if (!Owner)
	{
		return;
	}

	Mesh = Owner->GetMesh();
	Movement = Owner->GetCharacterMovement();

	if (Mesh)
	{
		EnsureRootMotionIgnored();

		if (UAnimInstance* AnimInst = Mesh->GetAnimInstance())
		{
			if (const UZZZAnimInstance* ZZZAnim = Cast<UZZZAnimInstance>(AnimInst))
			{
				if (const UAnimSequence* TurnBackSequence =
					ZZZAnim->GetSeqByKey(FName(TEXT("TurnBack"))))
				{
					TurnBackAnimationLength = FMath::Max(
						TurnBackSequence->GetPlayLength(),
						0.f);
				}
			}
		}
	}

	// 读取 RootMotionScale；移动速度不从固定 Walk/Run 配置初始化。
	if (const ABaseCharacter* BaseOwner = Cast<ABaseCharacter>(Owner))
	{
		const UCharConfigData* CharacterConfig = BaseOwner->GetCharacterConfig();
		if (CharacterConfig)
		{
			RootMotionScale = CharacterConfig->MovementConfig.RootMotionScale;
		}
	}
}

void FMotionDriver::UpdateTurnBackRotationMode(const FCharacterMovementCommand& Command)
{
	if (!Owner || !Movement)
	{
		return;
	}

	const bool bShouldOverrideAutomaticYaw =
		Command.bShouldCommit
		&& Command.TurnBackPhase != ETurnBackPhase::None
		&& !Command.bTurnBackSecondSegment;

	if (bShouldOverrideAutomaticYaw)
	{
		if (!bTurnBackRotationOverrideActive)
		{
			bSavedOrientRotationToMovement = Movement->bOrientRotationToMovement;
			bSavedUseControllerDesiredRotation = Movement->bUseControllerDesiredRotation;
			bSavedUseControllerRotationYaw = Owner->bUseControllerRotationYaw;
			bTurnBackRotationOverrideActive = true;

#if !UE_BUILD_SHIPPING
			UE_LOG(LogZZZAnim, Log,
				TEXT("[TurnBack][RotationMode] D0 automatic yaw disabled"));
#endif
		}

		Movement->bOrientRotationToMovement = false;
		Movement->bUseControllerDesiredRotation = false;
		Owner->bUseControllerRotationYaw = false;
		return;
	}

	if (!bTurnBackRotationOverrideActive)
	{
		return;
	}

	Movement->bOrientRotationToMovement = bSavedOrientRotationToMovement;
	Movement->bUseControllerDesiredRotation = bSavedUseControllerDesiredRotation;
	Owner->bUseControllerRotationYaw = bSavedUseControllerRotationYaw;
	bTurnBackRotationOverrideActive = false;

#if !UE_BUILD_SHIPPING
	UE_LOG(LogZZZAnim, Log,
		TEXT("[TurnBack][RotationMode] automatic yaw restored Orient=%d ControllerDesired=%d PawnYaw=%d"),
		Movement->bOrientRotationToMovement ? 1 : 0,
		Movement->bUseControllerDesiredRotation ? 1 : 0,
		Owner->bUseControllerRotationYaw ? 1 : 0);
#endif
}

void FMotionDriver::UpdateTurnBackDirectionIntent(
	const FCharacterMovementCommand& Command)
{
	if (Command.TurnBackPhase == ETurnBackPhase::None)
	{
		ResetTurnBackDirection();
		return;
	}

	if (!bTurnBackDirectionActive)
	{
		bTurnBackDirectionActive = true;

		// 只跨帧保存入口前向的 yaw；右向在每帧按当前 Bone_Root 重新计算。
		ResolveTurnBackBoneRootBasis(TurnBackDirectionBasisForward);
	}
}

void FMotionDriver::ResolveTurnBackBoneRootBasis(
	FVector& OutForward,
	FVector* OutRight) const
{
	OutForward = Owner
		? Owner->GetActorForwardVector().GetSafeNormal2D()
		: FVector(1.f, 0.f, 0.f);
	if (OutRight)
	{
		*OutRight = Owner
			? Owner->GetActorRightVector().GetSafeNormal2D()
			: FVector(0.f, 1.f, 0.f);
	}

	if (!Mesh)
	{
		return;
	}

	const FName BoneRootName(TEXT("Bone_Root"));
	if (Mesh->GetBoneIndex(BoneRootName) == INDEX_NONE)
	{
		return;
	}

	const FQuat BoneRootWorldRotation = Mesh->GetComponentQuat()
		* Mesh->GetBoneQuaternion(BoneRootName, EBoneSpaces::ComponentSpace);
	const FVector BoneForward = BoneRootWorldRotation.GetForwardVector().GetSafeNormal2D();
	const FVector BoneRight = BoneRootWorldRotation.GetRightVector().GetSafeNormal2D();
	if (!BoneForward.IsNearlyZero() && !BoneRight.IsNearlyZero())
	{
		OutForward = BoneForward;
		if (OutRight)
		{
			*OutRight = BoneRight;
		}
	}
}

void FMotionDriver::ApplyTurnBackYaw(const FCharacterMovementCommand& Command)
{
	if (!Owner
		|| Command.TurnBackPhase == ETurnBackPhase::None
		|| Command.bTurnBackSecondSegment)
	{
		return;
	}

	const float YawDelta = FMath::IsFinite(Command.AnimCurveYawDelta)
		? Command.AnimCurveYawDelta
		: 0.f;
	if (FMath::IsNearlyZero(YawDelta))
	{
		return;
	}

	Owner->AddActorWorldRotation(FRotator(0.f, YawDelta, 0.f));

#if !UE_BUILD_SHIPPING
	UE_LOG(LogZZZAnim, Log,
		TEXT("[TurnBack][ActorYaw] Phase=%d D1=0 RMYawDelta=%.3f ActorYaw=%.3f"),
		static_cast<uint8>(Command.TurnBackPhase),
		YawDelta,
		Owner->GetActorRotation().Yaw);
#endif
}

void FMotionDriver::ResetTurnBackDirection()
{
	bTurnBackDirectionActive = false;
	TurnBackDirectionBasisForward = FVector(1.f, 0.f, 0.f);
}

FVector FMotionDriver::ResolveTurnBackWorldMoveDirection(
	const FCharacterMovementCommand& Command) const
{
	// d1（CanYaw 之后）：像 walkrun 一样直接朝玩家输入方向移动，方向被输入实时修正。
	if (Command.bTurnBackSecondSegment)
	{
		FVector InputDir = Command.DesiredWorldMoveDir;
		InputDir.Z = 0.f;
		return InputDir.GetSafeNormal2D();
	}

	// d0（CanYaw 之前）：读取原始曲线分量；缺失时使用入口 Bone_Root 前向保持世界直线。
	FVector CurveDir = Command.AnimCurveVelocityDirection.GetSafeNormal2D();
	if (CurveDir.IsNearlyZero() && Command.bHasRootMotion)
	{
		CurveDir = Command.RootMotionDelta.GetSafeNormal2D();
	}

	if (CurveDir.IsNearlyZero())
	{
		// 无曲线方向时退回入口 Bone_Root 前向，仍保持世界直线。
		return TurnBackDirectionBasisForward;
	}

	FVector CurrentForward;
	FVector CurrentRight;
	ResolveTurnBackBoneRootBasis(CurrentForward, &CurrentRight);
	if (CurrentForward.IsNearlyZero() || CurrentRight.IsNearlyZero())
	{
		return TurnBackDirectionBasisForward;
	}

	// CurveDir 的 X/Y 是 Bone_Root 局部右/前分量；先转成 UE 局部 X=前、Y=右。
	const FVector CurveLocalDirection(CurveDir.Y, CurveDir.X, 0.f);
	const float EntryYaw = TurnBackDirectionBasisForward.Rotation().Yaw;
	const float CurrentYaw = CurrentForward.Rotation().Yaw;
	const float RelativeBoneRootYaw = FMath::FindDeltaAngleDegrees(
		EntryYaw,
		CurrentYaw);

	// 当前 Bone_Root 已因 Actor 转身旋转了 RelativeBoneRootYaw；逆旋转 authored 方向，
	// 再用当前 Bone_Root 基准映射回世界，保持入口时的世界位移直线。
	const FVector CorrectedLocalDirection = FRotator(
		0.f,
		-RelativeBoneRootYaw,
		0.f).RotateVector(CurveLocalDirection);
	FVector WorldDirection = CurrentForward * CorrectedLocalDirection.X
		+ CurrentRight * CorrectedLocalDirection.Y;
	WorldDirection.Z = 0.f;
	return WorldDirection.GetSafeNormal2D();
}

FVector FMotionDriver::ResolveWorldMoveDirection(
	const FCharacterMovementCommand& Command) const
{
	// TurnBack 是动画自带位移的特例：d0 用 dir 曲线×进入锁定基准，d1 用玩家输入方向。
	if (Command.TurnBackPhase != ETurnBackPhase::None)
	{
		return ResolveTurnBackWorldMoveDirection(Command);
	}

	// 普通移动（无 dir/yaw 曲线）：移动方向就是玩家输入方向。
	// DesiredWorldMoveDir 已由 LocomotionIntentProcessor 按摄像机水平 Yaw 把摇杆输入解析成世界方向。
	FVector MoveDirection = Command.DesiredWorldMoveDir;
	MoveDirection.Z = 0.f;
	return MoveDirection.GetSafeNormal2D();
}

void FMotionDriver::UpdateTurnBackReleaseState(
	float DeltaTime,
	const FCharacterMovementCommand& Command)
{
	if (!Owner)
	{
		return;
	}

#if !UE_BUILD_SHIPPING
	if (Command.TurnBackPhase != ETurnBackPhase::None)
	{
		const FRotator ActorRotation = Owner->GetActorRotation();
		const FRotator ControlRotation = Owner->GetControlRotation();
		const FVector Velocity = Owner->GetVelocity();
		const FVector Acceleration = Movement
			? Movement->GetCurrentAcceleration()
			: FVector::ZeroVector;
		const FRotator VelocityRotation = Velocity.IsNearlyZero()
			? FRotator::ZeroRotator
			: Velocity.GetSafeNormal2D().Rotation();
		const FRotator AccelerationRotation = Acceleration.IsNearlyZero()
			? FRotator::ZeroRotator
			: Acceleration.GetSafeNormal2D().Rotation();
		const FVector RequestedDirection = Command.bTurnBackSecondSegment
			? Command.DesiredWorldMoveDir.GetSafeNormal2D()
			: Command.AnimCurveVelocityDirection.GetSafeNormal2D();
		const FVector RequestedVelocity = RequestedDirection
			* FMath::Max(Command.AnimCurveSpeed, 0.f)
			* RootMotionScale;

		const FName RootBoneName(TEXT("Root"));
		const FName BoneRootName(TEXT("Bone_Root"));
		const FName Bip001BoneName(TEXT("Bip001"));
		const int32 RootBoneIndex = Mesh
			? Mesh->GetBoneIndex(RootBoneName)
			: INDEX_NONE;
		const int32 BoneRootIndex = Mesh
			? Mesh->GetBoneIndex(BoneRootName)
			: INDEX_NONE;
		const int32 Bip001BoneIndex = Mesh
			? Mesh->GetBoneIndex(Bip001BoneName)
			: INDEX_NONE;
		const FRotator RootComponentRotation = RootBoneIndex != INDEX_NONE
			? Mesh->GetBoneQuaternion(
				RootBoneName,
				EBoneSpaces::ComponentSpace).Rotator()
			: FRotator::ZeroRotator;
		const FRotator BoneRootComponentRotation = BoneRootIndex != INDEX_NONE
			? Mesh->GetBoneQuaternion(
				BoneRootName,
				EBoneSpaces::ComponentSpace).Rotator()
			: FRotator::ZeroRotator;
		const FRotator Bip001ComponentRotation = Bip001BoneIndex != INDEX_NONE
			? Mesh->GetBoneQuaternion(
				Bip001BoneName,
				EBoneSpaces::ComponentSpace).Rotator()
			: FRotator::ZeroRotator;
		const FRotator MeshRelativeRotation = Mesh
			? Mesh->GetRelativeRotation()
			: FRotator::ZeroRotator;
		const FRotator MeshComponentRotation = Mesh
			? Mesh->GetComponentRotation()
			: FRotator::ZeroRotator;

		UE_LOG(LogZZZAnim, Log,
			TEXT("[TurnBack][RotationDiag] Frame=%llu Phase=%d D1=%d "
				"Actor=(P%.2f Y%.2f R%.2f) Control=(P%.2f Y%.2f R%.2f) "
				"VelocityYaw=%.2f AccelYaw=%.2f "
				"Desired=(%.2f,%.2f,%.2f) CurveDir=(%.2f,%.2f,%.2f) "
				"Requested=(%.2f,%.2f,%.2f) "
				"Flags[Orient=%d ControllerDesired=%d PawnYaw=%d] "
				"MeshRel=(P%.2f Y%.2f R%.2f) MeshComp=(P%.2f Y%.2f R%.2f) "
				"BoneIndex[Root=%d Bone_Root=%d Bip001=%d] "
				"BoneCSYaw[Root=%.2f Bone_Root=%.2f Bip001=%.2f]"),
			GFrameCounter,
			static_cast<uint8>(Command.TurnBackPhase),
			Command.bTurnBackSecondSegment ? 1 : 0,
			ActorRotation.Pitch,
			ActorRotation.Yaw,
			ActorRotation.Roll,
			ControlRotation.Pitch,
			ControlRotation.Yaw,
			ControlRotation.Roll,
			VelocityRotation.Yaw,
			AccelerationRotation.Yaw,
			Command.DesiredWorldMoveDir.X,
			Command.DesiredWorldMoveDir.Y,
			Command.DesiredWorldMoveDir.Z,
			Command.AnimCurveVelocityDirection.X,
			Command.AnimCurveVelocityDirection.Y,
			Command.AnimCurveVelocityDirection.Z,
			RequestedVelocity.X,
			RequestedVelocity.Y,
			RequestedVelocity.Z,
			Movement ? (Movement->bOrientRotationToMovement ? 1 : 0) : -1,
			Movement ? (Movement->bUseControllerDesiredRotation ? 1 : 0) : -1,
			Owner->bUseControllerRotationYaw ? 1 : 0,
			MeshRelativeRotation.Pitch,
			MeshRelativeRotation.Yaw,
			MeshRelativeRotation.Roll,
			MeshComponentRotation.Pitch,
			MeshComponentRotation.Yaw,
			MeshComponentRotation.Roll,
			RootBoneIndex,
			BoneRootIndex,
			Bip001BoneIndex,
			RootComponentRotation.Yaw,
			BoneRootComponentRotation.Yaw,
			Bip001ComponentRotation.Yaw);
	}
#endif

	const bool bTurnBackActive =
		Command.TurnBackPhase == ETurnBackPhase::Frozen
		|| Command.TurnBackPhase == ETurnBackPhase::Released;
	if (!bTurnBackActive)
	{
		if (Command.TurnBackPhase == ETurnBackPhase::None)
		{
			bTurnBackAnimationActive = false;
			bTurnBackAnimationComplete = false;
			TurnBackAnimationElapsed = 0.f;
		}
		return;
	}

	// TurnBack 期间继续确保曲线驱动不会被引擎 Root Motion 叠加。
	EnsureRootMotionIgnored();

	if (!bTurnBackAnimationActive)
	{
		bTurnBackAnimationActive = true;
		TurnBackAnimationElapsed = 0.f;

		// Init 时 AnimBP 可能尚未完成实例化或配置；首次进入 TurnBack 时再尝试一次。
		if (TurnBackAnimationLength <= KINDA_SMALL_NUMBER && Mesh)
		{
			if (const UZZZAnimInstance* ZZZAnim =
				Cast<UZZZAnimInstance>(Mesh->GetAnimInstance()))
			{
				if (const UAnimSequence* TurnBackSequence =
					ZZZAnim->GetSeqByKey(FName(TEXT("TurnBack"))))
				{
					TurnBackAnimationLength = FMath::Max(
						TurnBackSequence->GetPlayLength(),
						0.f);
				}
			}
		}
	}

	if (FMath::IsFinite(DeltaTime) && DeltaTime > 0.f)
	{
		TurnBackAnimationElapsed += DeltaTime;
	}

	if (Command.TurnBackPhase != ETurnBackPhase::Released
		|| bTurnBackAnimationComplete)
	{
		return;
	}

	const bool bAnimationComplete = TurnBackAnimationLength <= KINDA_SMALL_NUMBER
		|| TurnBackAnimationElapsed + KINDA_SMALL_NUMBER >= TurnBackAnimationLength;
	if (!bAnimationComplete)
	{
		return;
	}

	bTurnBackAnimationComplete = true;
	const FVector DesiredDirection = Command.DesiredWorldMoveDir.GetSafeNormal2D();
	UE_LOG(LogZZZAnim, Log,
		TEXT("[TurnBack][AnimationComplete] RootDelta=(%.3f,%.3f) DesiredYaw=%.2f AnimationTime=%.3f/%.3f"),
		Command.RootMotionDelta.X,
		Command.RootMotionDelta.Y,
		DesiredDirection.IsNearlyZero() ? 0.0f : DesiredDirection.Rotation().Yaw,
		TurnBackAnimationElapsed,
		TurnBackAnimationLength);
}

void FMotionDriver::Process(
	float DeltaTime,
	const FCharacterMovementCommand& Command,
	FRuntimeData& RuntimeData)
{
	if (!Owner || !Movement)
	{
		return;
	}

	UpdateTurnBackRotationMode(Command);
	if (!Command.bShouldCommit)
	{
		return;
	}

	// TurnBack Phase 和 CanYaw 第二段标记由参数处理器维护；D0 的 RM_Yaw 差分在 Motion Commit
	// 阶段累加到 Actor，d0 位移方向随后按当前 Bone_Root yaw 做逆变换。
	UpdateTurnBackDirectionIntent(Command);
	ApplyTurnBackYaw(Command);
	UpdateTurnBackReleaseState(DeltaTime, Command);

	// 被仲裁阻止移动时，清零速度上限，只更新运行时数据，不驱动位移。
	if (Command.bBlockMove)
	{
		Movement->MaxWalkSpeed = 0.f;
		UpdateRuntimeData(RuntimeData);
		return;
	}

	// 零输入时，普通移动必须立即停止；显式 TurnBack 或有效 RootMotion 曲线源允许
	// 动画收尾继续提交，直到后续状态逻辑退出该状态。
	const bool bAllowTurnBackRootMotion =
		Command.TurnBackPhase != ETurnBackPhase::None;
	if (!Command.bShouldMove
		&& !bAllowTurnBackRootMotion
		&& !Command.bHasRootMotionCurveSource)
	{
		Movement->MaxWalkSpeed = 0.f;
		Movement->StopMovementImmediately();
		UpdateRuntimeData(RuntimeData);
		return;
	}

	// RM_Speed 为本帧唯一的移动速度来源；RootMotionScale 只缩放该曲线速度。
	const float AnimCurveSpeed = FMath::IsFinite(Command.AnimCurveSpeed)
		&& Command.AnimCurveSpeed > KINDA_SMALL_NUMBER
		? Command.AnimCurveSpeed
		: 0.f;
	const float ScaledCurveSpeed = AnimCurveSpeed * RootMotionScale;
	const bool bHasValidCurveSpeed = AnimCurveSpeed > KINDA_SMALL_NUMBER
		&& FMath::IsFinite(ScaledCurveSpeed)
		&& ScaledCurveSpeed > KINDA_SMALL_NUMBER;

	if (!bHasValidCurveSpeed)
	{
		// 没有有效曲线速度时不把 MaxWalkSpeed 或固定速度作为回退；仅置零并停止上一帧可能残留的惯性。
		Movement->MaxWalkSpeed = 0.f;
		Movement->StopMovementImmediately();
		UpdateRuntimeData(RuntimeData);
		return;
	}

	// MaxWalkSpeed 仅承载当前 RM_Speed * RootMotionScale 的引擎速度上限，不是固定速度配置。
	Movement->MaxWalkSpeed = ScaledCurveSpeed;

	const FVector MoveDirection = ResolveWorldMoveDirection(Command);

	// 路径 A：有有效 RM_PosX/RM_PosY 差分时走曲线位移分支；世界方向由上面的规则统一决定。
	if (Command.bHasRootMotion)
	{
		ProcessRootMotionMovement(DeltaTime, Command);
	}
	// 路径 B：没有有效位置差分时使用同一 RM_Speed 曲线速度的常规移动路径。
	else if (!MoveDirection.IsNearlyZero())
	{
		ProcessLocomotion(MoveDirection, Command);
	}

	UpdateRuntimeData(RuntimeData);
}

void FMotionDriver::ProcessRootMotionMovement(
	float DeltaTime,
	const FCharacterMovementCommand& Command)
{
	if (!Owner || !Movement || !FMath::IsFinite(DeltaTime) || DeltaTime <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	FVector WorldMoveDir = ResolveWorldMoveDirection(Command);

	// 只有曲线和输入方向都无效时，才回退到角色自身的水平前向。
	if (WorldMoveDir.IsNearlyZero())
	{
		WorldMoveDir = Owner->GetActorForwardVector();
		WorldMoveDir.Z = 0.f;
		WorldMoveDir = WorldMoveDir.GetSafeNormal2D();
	}

	if (WorldMoveDir.IsNearlyZero())
	{
		return;
	}

	// RM_Speed 是曲线最终速度主值（cm/s）；非法或负值按零处理，RM_Dist 只保留诊断用途。
	const float AnimCurveSpeed = FMath::IsFinite(Command.AnimCurveSpeed)
		&& Command.AnimCurveSpeed >= 0.f
		? Command.AnimCurveSpeed
		: 0.f;
	if (AnimCurveSpeed <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	const float ScaledSpeed = AnimCurveSpeed * RootMotionScale;
	if (!FMath::IsFinite(ScaledSpeed) || FMath::IsNearlyZero(ScaledSpeed))
	{
		return;
	}

	const FVector WorldVelocity = WorldMoveDir * ScaledSpeed;

#if !UE_BUILD_SHIPPING
	// 只在 TurnBack 期间打印，避免普通移动每帧刷屏。
	if (Command.TurnBackPhase != ETurnBackPhase::None)
	{
	UE_LOG(LogZZZAnim, Log,
		TEXT("[MoveDiag][RequestDirectMove] Frame=%llu Path=RootMotion TurnBackPhase=%d SecondSegment=%d CurveDirectionSource=%s CurveDirectionLocal=(%.3f,%.3f) DesiredWorldMoveDir=(%.3f,%.3f,%.3f) RootMotionDelta=(%.3f,%.3f,%.3f) RequestDirectMoveDir=(%.3f,%.3f,%.3f) Speed=%.3f"),
		GFrameCounter,
		static_cast<uint8>(Command.TurnBackPhase),
		Command.bTurnBackSecondSegment ? 1 : 0,
		GetCurveDirectionSource(Command),
		Command.AnimCurveVelocityDirection.X,
		Command.AnimCurveVelocityDirection.Y,
		Command.DesiredWorldMoveDir.X,
		Command.DesiredWorldMoveDir.Y,
		Command.DesiredWorldMoveDir.Z,
		Command.RootMotionDelta.X,
		Command.RootMotionDelta.Y,
		Command.RootMotionDelta.Z,
		WorldMoveDir.X,
		WorldMoveDir.Y,
		WorldMoveDir.Z,
		ScaledSpeed);
	}
#endif

	// RequestDirectMove 的第二个参数是 bForceMaxSpeed：true 走 Unreal 的强制直接请求语义，
	// 不再依赖加速度/减速度逐步逼近；MaxWalkSpeed 已承载当前曲线速度。
	Movement->RequestDirectMove(WorldVelocity, true);
}

void FMotionDriver::ProcessLocomotion(
	const FVector& WorldDir,
	const FCharacterMovementCommand& Command)
{
	FVector Dir = WorldDir.GetSafeNormal2D();
	if (Dir.IsNearlyZero())
	{
		return;
	}

	// 常规路径同样只接受有效 RM_Speed；不使用任何固定速度配置、速度 cap 或输入累加回退。
	const float AnimCurveSpeed = FMath::IsFinite(Command.AnimCurveSpeed)
		&& Command.AnimCurveSpeed > KINDA_SMALL_NUMBER
		? Command.AnimCurveSpeed
		: 0.f;
	const float ScaledCurveSpeed = AnimCurveSpeed * RootMotionScale;
	if (!FMath::IsFinite(ScaledCurveSpeed) || ScaledCurveSpeed <= KINDA_SMALL_NUMBER)
	{
		return;
	}

	// 防滑步核心：世界方向 × RM_Speed × RootMotionScale → 直接速度请求。
	const FVector TargetVelocity = Dir * ScaledCurveSpeed;

#if !UE_BUILD_SHIPPING
	// 只在 TurnBack 期间打印，避免普通移动每帧刷屏。
	if (Command.TurnBackPhase != ETurnBackPhase::None)
	{
	UE_LOG(LogZZZAnim, Log,
		TEXT("[MoveDiag][RequestDirectMove] Frame=%llu Path=Locomotion TurnBackPhase=%d SecondSegment=%d CurveDirectionSource=%s CurveDirectionLocal=(%.3f,%.3f) DesiredWorldMoveDir=(%.3f,%.3f,%.3f) RootMotionDelta=(%.3f,%.3f,%.3f) RequestDirectMoveDir=(%.3f,%.3f,%.3f) Speed=%.3f"),
		GFrameCounter,
		static_cast<uint8>(Command.TurnBackPhase),
		Command.bTurnBackSecondSegment ? 1 : 0,
		GetCurveDirectionSource(Command),
		Command.AnimCurveVelocityDirection.X,
		Command.AnimCurveVelocityDirection.Y,
		Command.DesiredWorldMoveDir.X,
		Command.DesiredWorldMoveDir.Y,
		Command.DesiredWorldMoveDir.Z,
		Command.RootMotionDelta.X,
		Command.RootMotionDelta.Y,
		Command.RootMotionDelta.Z,
		Dir.X,
		Dir.Y,
		Dir.Z,
		ScaledCurveSpeed);
	}
#endif

	Movement->RequestDirectMove(TargetVelocity, true);
}

void FMotionDriver::UpdateRuntimeData(FRuntimeData& RuntimeData)
{
	if (!Owner)
	{
		return;
	}

	FVector Velocity = Owner->GetVelocity();
	const FVector HorizontalVelocity(Velocity.X, Velocity.Y, 0.f);
	RuntimeData.Movement.CurrentSpeed = HorizontalVelocity.Size2D();
	RuntimeData.ZZZAnim.VelocityLength = RuntimeData.Movement.CurrentSpeed;
	RuntimeData.Movement.bIsMoving = RuntimeData.Movement.CurrentSpeed > 10.f;

	RuntimeData.ZZZAnim.ActualVelocityDirection = FVector::ZeroVector;
	RuntimeData.ZZZAnim.ActualVelocityBlendX = 0.f;
	RuntimeData.ZZZAnim.ActualVelocityBlendY = 0.f;
	RuntimeData.ZZZAnim.ActualVelocityAngle = 0.f;

	const FVector LocalVelocity = Owner->GetActorTransform()
		.InverseTransformVector(HorizontalVelocity);
	const float LocalVelocityLength = LocalVelocity.Size2D();
	if (LocalVelocityLength > KINDA_SMALL_NUMBER)
	{
		RuntimeData.ZZZAnim.ActualVelocityDirection = HorizontalVelocity.GetSafeNormal2D();
		RuntimeData.ZZZAnim.ActualVelocityBlendX = LocalVelocity.Y / LocalVelocityLength;
		RuntimeData.ZZZAnim.ActualVelocityBlendY = LocalVelocity.X / LocalVelocityLength;
		RuntimeData.ZZZAnim.ActualVelocityAngle = FMath::RadiansToDegrees(
			FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
	}

	if (RuntimeData.Movement.bIsMoving)
	{
		RuntimeData.Movement.MoveAngle = FMath::RadiansToDegrees(
			FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
	}
	else
	{
		RuntimeData.Movement.MoveAngle = 0.f;
	}
}
