/**
 * @file MotionDriver.cpp
 * @brief 运动驱动器实现
 *
 * 动画曲线驱动架构：
 *   - DesiredWorldMoveDir 由摄像机相对输入生成；TurnBack Frozen 期间由锁定的进入方向覆盖，Released 首帧按 RootMotionDelta 与目标输入方向的一致性选择方向，后续使用当前输入方向
 *   - RM_Speed 是实际速度主值（cm/s），RootMotionScale 继续缩放最终动画速度
 *   - RM_Dist 仅保留为距离增量诊断，不参与最终速度计算
 *   - RM_PosX / RM_PosY 差分保留为局部曲线位移，并在 TurnBack Released 首帧作为方向候选；与目标输入同向时直接使用，相反时才取 180°反向
 *   - sig_turnback 解冻信号驱动 Actor 旋转；RM_Yaw 仅保留为源动画旋转诊断/姿态数据
 *   - SetRootMotionMode(IgnoreRootMotion) 保证曲线驱动不会与引擎 Root Motion 叠加
 */
#include "Drivers/MotionDriver.h"
#include "Data/Runtime/RuntimeData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/zzzAnim/ZZZAnimLog.h"
#include "Animation/zzzAnim/Locomotion/ZZZLocomotionRules.h"
#include "BaseCharacter.h"
#include "Data/Config/UCharConfigData.h"

void FMotionDriver::Init(ACharacter* InOwner)
{
	RootMotionScale = 1.f;
	Owner = InOwner;
	if (!Owner) return;

	Mesh = Owner->GetMesh();
	Movement = Owner->GetCharacterMovement();

	if (Mesh)
	{
		if (UAnimInstance* AnimInst = Mesh->GetAnimInstance())
		{
			AnimInst->SetRootMotionMode(ERootMotionMode::IgnoreRootMotion);
		}
	}

	// 仅读取 RootMotionScale；当前移动速度不从固定 Walk/Run 配置初始化。
	if (const ABaseCharacter* BaseOwner = Cast<ABaseCharacter>(Owner))
	{
		const UCharConfigData* CharacterConfig = BaseOwner->GetCharacterConfig();
		if (CharacterConfig)
		{
			RootMotionScale = CharacterConfig->MovementConfig.RootMotionScale;
		}
	}
}

FVector FMotionDriver::ResolveWorldMoveDirection(
	const FCharacterMovementCommand& Command) const
{
	FVector MoveDirection;
	switch (Command.TurnBackPhase)
	{
	case ETurnBackPhase::Frozen:
		// Frozen：保持进入转身前的方向，等待 sig_turnback 解冻。
		MoveDirection = Command.TurnBackEntryDirection;
		break;

	case ETurnBackPhase::Released:
		// Released 首帧优先使用刚由 RootMotion XY 与目标输入选择出的方向；后续帧跟随当前输入。
		MoveDirection = !FrameTurnBackReleaseDirection.IsNearlyZero()
			? FrameTurnBackReleaseDirection
			: Command.DesiredWorldMoveDir;
		break;

	default:
		// None：普通移动，直接使用当前输入方向。
		MoveDirection = Command.DesiredWorldMoveDir;
		break;
	}

	MoveDirection.Z = 0.f;
	return MoveDirection.GetSafeNormal();
}

FVector FMotionDriver::ResolveTurnBackReleaseDirection(
	const FCharacterMovementCommand& Command,
	FMotionDriver::ETurnBackReleaseDirectionSource* OutSource) const
{
	if (OutSource)
	{
		*OutSource = ETurnBackReleaseDirectionSource::EntryDirectionFallback;
	}

	if (!Owner)
	{
		return FVector::ZeroVector;
	}

	const FVector DesiredDirection = Command.DesiredWorldMoveDir.GetSafeNormal2D();
	if (Command.bHasRootMotion)
	{
		// RootMotionDelta 是动画局部空间位移，先按旋转前 Actor 朝向转到世界。
		// 动画的 Root/自定义位移可能已经包含 180° 转身：只有候选方向与目标输入相反时，
		// 才取反向，避免把动画内置的转身再叠加一次。
		const FVector LocalRootMotionDirection = Command.RootMotionDelta.GetSafeNormal2D();
		if (!LocalRootMotionDirection.IsNearlyZero())
		{
			FVector WorldRootMotionDirection = Owner->GetActorTransform()
				.TransformVectorNoScale(LocalRootMotionDirection);
			WorldRootMotionDirection.Z = 0.f;
			WorldRootMotionDirection = WorldRootMotionDirection.GetSafeNormal2D();
			if (!WorldRootMotionDirection.IsNearlyZero())
			{
				if (!DesiredDirection.IsNearlyZero())
				{
					const float DirectionAlignment = FVector::DotProduct(
						WorldRootMotionDirection,
						DesiredDirection);
					if (OutSource)
					{
						*OutSource = DirectionAlignment >= 0.f
							? ETurnBackReleaseDirectionSource::RootMotionXY
							: ETurnBackReleaseDirectionSource::RootMotionXYReversed;
					}
					return (DirectionAlignment >= 0.f
						? WorldRootMotionDirection
						: -WorldRootMotionDirection).GetSafeNormal2D();
				}

				// 没有目标输入时保留旧契约：RootMotion 位移按反向方向作为安全回退。
				if (OutSource)
				{
					*OutSource = ETurnBackReleaseDirectionSource::RootMotionXYReversed;
				}
				return (-WorldRootMotionDirection).GetSafeNormal2D();
			}
		}
	}

	// 没有可用位移曲线时，直接使用输入方向作为解冻目标。
	if (!DesiredDirection.IsNearlyZero())
	{
		if (OutSource)
		{
			*OutSource = ETurnBackReleaseDirectionSource::DesiredWorldMoveDir;
		}
		return DesiredDirection;
	}

	// 最后使用入口方向的严格反向，保证仍能完成固定 180° 的安全回退。
	return (-Command.TurnBackEntryDirection).GetSafeNormal2D();
}

void FMotionDriver::ApplyTurnBackReleaseRotation(
	const FCharacterMovementCommand& Command)
{
	FrameTurnBackReleaseDirection = FVector::ZeroVector;

	if (!Owner)
	{
		return;
	}

	if (Command.TurnBackPhase == ETurnBackPhase::None)
	{
		// 一次转身结束后清除闩锁，下一段 TurnBack 可以再次触发。
		bTurnBackReleaseRotationApplied = false;
		return;
	}

	// 只有 sig_turnback 已经把相位推进到 Released 后才旋转 Actor；Frozen 期间保持旧方向。
	if (Command.TurnBackPhase != ETurnBackPhase::Released
		|| bTurnBackReleaseRotationApplied)
	{
		return;
	}

	ETurnBackReleaseDirectionSource DirectionSource =
		ETurnBackReleaseDirectionSource::EntryDirectionFallback;
	const FVector CorrectedDirection = ResolveTurnBackReleaseDirection(Command, &DirectionSource);
	if (CorrectedDirection.IsNearlyZero())
	{
		return;
	}

	const float CorrectedYaw = CorrectedDirection.Rotation().Yaw;
	Owner->SetActorRotation(FRotator(0.f, CorrectedYaw, 0.f));
	FrameTurnBackReleaseDirection = CorrectedDirection;
	bTurnBackReleaseRotationApplied = true;

	const TCHAR* SourceName = TEXT("EntryDirectionFallback");
	switch (DirectionSource)
	{
	case ETurnBackReleaseDirectionSource::RootMotionXY:
		SourceName = TEXT("RootMotionXY");
		break;
	case ETurnBackReleaseDirectionSource::RootMotionXYReversed:
		SourceName = TEXT("RootMotionXY_Reversed");
		break;
	case ETurnBackReleaseDirectionSource::DesiredWorldMoveDir:
		SourceName = TEXT("DesiredWorldMoveDir");
		break;
	default:
		break;
	}

	const FVector DesiredDirection = Command.DesiredWorldMoveDir.GetSafeNormal2D();
	UE_LOG(LogZZZAnim, Log,
		TEXT("[TurnBack][ReleaseRotation] ActorYaw=%.2f CorrectedYaw=%.2f Source=%s RootDelta=(%.3f,%.3f) DesiredYaw=%.2f"),
		Owner->GetActorRotation().Yaw,
		CorrectedYaw,
		SourceName,
		Command.RootMotionDelta.X,
		Command.RootMotionDelta.Y,
		DesiredDirection.IsNearlyZero() ? 0.0f : DesiredDirection.Rotation().Yaw);
}

void FMotionDriver::Process(
	float DeltaTime,
	const FCharacterMovementCommand& Command,
	FRuntimeData& RuntimeData)
{
	if (!Owner || !Movement || !Command.bShouldCommit)
	{
		return;
	}

	// sig_turnback 已将相位推进到 Released：首次 Commit 按 RootMotion/输入候选设置 Actor 朝向，
	// 并用同一帧选出的方向提交移动。
	ApplyTurnBackReleaseRotation(Command);

	// 被仲裁阻止移动时，清零速度上限，只更新运行时数据，不驱动位移。
	if (Command.bBlockMove)
	{
		Movement->MaxWalkSpeed = 0.f;
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

	// MaxWalkSpeed 仅承载当前 RM_Speed * RootMotionScale 的引擎速度上限，不是固定速度配置；
	// 必须先于两条移动路径设置为有效曲线速度，随后移动请求才能立即采用该值。
	Movement->MaxWalkSpeed = ScaledCurveSpeed;

	const FVector MoveDirection = ResolveWorldMoveDirection(Command);

	// 路径 A：有有效 RM_PosX/RM_PosY 差分时走曲线位移分支；世界方向由当前相位方向提供。
	if (Command.bHasRootMotion)
	{
		ProcessRootMotionMovement(DeltaTime, Command);
	}
	// 路径 B：没有有效位置差分时使用同一 RM_Speed 曲线速度的常规移动路径。
	else if (!MoveDirection.IsNearlyZero())
	{
		ProcessLocomotion(MoveDirection, Command);
	}

	// 回写实际速度、移动状态到 RuntimeData 供调试/UI 使用。
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

	// TurnBack Frozen 期间使用进入转身前锁定的方向；Released 首帧使用解冻时选择出的方向，
	// 其它相位使用当前摄像机相对输入方向。
	FVector WorldMoveDir = ResolveWorldMoveDirection(Command);

	// Stop/减速动画可能没有本帧输入；只回退到角色自身的水平前向，不使用摄像机方向。
	if (WorldMoveDir.IsNearlyZero())
	{
		WorldMoveDir = Owner->GetActorForwardVector();
		WorldMoveDir.Z = 0.f;
		WorldMoveDir = WorldMoveDir.GetSafeNormal();
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
		// RM_Speed 末尾为零时不再被 RM_Dist 推着移动。
		return;
	}

	const float ScaledSpeed = AnimCurveSpeed * RootMotionScale;
	if (!FMath::IsFinite(ScaledSpeed) || FMath::IsNearlyZero(ScaledSpeed))
	{
		return;
	}

	const FVector WorldVelocity = WorldMoveDir * ScaledSpeed;

	// RequestDirectMove 的第二个参数是 bForceMaxSpeed：true 走 Unreal 的
	// 强制直接请求语义，不再依赖加速度/减速度逐步逼近；MaxWalkSpeed 已在
	// 调用前设置为当前 ScaledSpeed，仅作为该帧曲线速度的引擎上限承载。
	Movement->RequestDirectMove(WorldVelocity, true);
}

void FMotionDriver::ProcessLocomotion(
	const FVector& WorldDir,
	const FCharacterMovementCommand& Command)
{
	FVector Dir = WorldDir.GetSafeNormal();
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

	// 防滑步核心：输入方向 × RM_Speed × RootMotionScale → 直接速度请求。
	// 与根曲线路径一致使用 bForceMaxSpeed=true，避免请求受加速度/减速度逼近影响。
	const FVector TargetVelocity = Dir * ScaledCurveSpeed;
	Movement->RequestDirectMove(TargetVelocity, true);
}

void FMotionDriver::UpdateRuntimeData(FRuntimeData& RuntimeData)
{
	if (!Owner) return;

	FVector Velocity = Owner->GetVelocity();
	RuntimeData.Movement.CurrentSpeed = Velocity.Size2D();
	RuntimeData.ZZZAnim.VelocityLength = RuntimeData.Movement.CurrentSpeed;
	RuntimeData.Movement.bIsMoving = RuntimeData.Movement.CurrentSpeed > 10.f;

	if (RuntimeData.Movement.bIsMoving)
	{
		FVector LocalVelocity = Owner->GetActorTransform()
			.InverseTransformVector(Velocity);
		RuntimeData.Movement.MoveAngle = FMath::RadiansToDegrees(
			FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
	}
	else
	{
		RuntimeData.Movement.MoveAngle = 0.f;
	}
}
