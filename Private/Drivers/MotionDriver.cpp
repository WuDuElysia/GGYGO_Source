/**
 * @file MotionDriver.cpp
 * @brief 运动驱动器实现
 *
 * 防滑步架构：
 *   - 输入方向（WASD）决定移动方向
 *   - 动画速度（AnimSpeed）决定移动快慢
 *   - 两者融合：RequestDirectMove(输入方向 × 动画速度)
 *   - 结果：动画跑多快 → 角色就跑多快 → 帧级同步无滑步
 *   - bHasRootMotion 仅保留给 Montage/技能动画使用
 */
#include "Drivers/MotionDriver.h"
#include "Data/Logic/RuntimeData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "BaseCharacter.h"
#include "Data/UCharConfigData.h"

void FMotionDriver::Init(ACharacter* InOwner)
{
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

	DefaultMaxWalkSpeed = Movement ? Movement->MaxWalkSpeed : 600.f;

	// 速度阈值由运动驱动初始化，不由 BaseCharacter 直接写 RuntimeData。
	if (const ABaseCharacter* BaseOwner = Cast<ABaseCharacter>(Owner))
	{
		FRuntimeData* RuntimeData = BaseOwner->GetRuntimeData();
		const UCharConfigData* CharacterConfig = BaseOwner->GetCharacterConfig();
		if (RuntimeData && CharacterConfig)
		{
			RuntimeData->GaitThresholds.Walk = CharacterConfig->MovementConfig.WalkSpeed;
			RuntimeData->GaitThresholds.Run = CharacterConfig->MovementConfig.RunSpeed;
			RuntimeData->GaitThresholds.Sprint = CharacterConfig->MovementConfig.SprintSpeed;
		}
	}
}

void FMotionDriver::Process(float DeltaTime, FRuntimeData& RuntimeData)
{
	if (!Owner || !Movement) return;

	// 被仲裁阻止移动时，只更新运行时数据，不驱动位移
	if (RuntimeData.bBlockMove)
	{
		UpdateRuntimeData(RuntimeData);
		return;
	}

	// 路径 A: Root Motion 模式（Montage/技能动画专用）
	if (RuntimeData.bHasRootMotion)
	{
		ProcessRootMotionMovement(RuntimeData.RootMotionDelta, DeltaTime);
	}
	// 路径 B: 常规移动（AnimSpeed 由 RootMotionParameterProcessor 从 Speed 曲线读取）
	else if (!RuntimeData.DesiredWorldMoveDir.IsNearlyZero())
	{
		ProcessLocomotion(DeltaTime, RuntimeData.DesiredWorldMoveDir, RuntimeData.AnimSpeed, RuntimeData);
	}

	// 回写实际速度、移动状态到 RuntimeData 供调试/UI 使用
	UpdateRuntimeData(RuntimeData);

	// ★ 优化：根据统一解析的步态锁定 MaxWalkSpeed
	if (Movement)
	{
		if (RuntimeData.bBlockMove)
			Movement->MaxWalkSpeed = 0.f;
		else switch (RuntimeData.ResolvedGait)
		{
		case EMovementGait::Walk:   Movement->MaxWalkSpeed = RuntimeData.GaitThresholds.Walk;   break;
		case EMovementGait::Sprint: Movement->MaxWalkSpeed = RuntimeData.GaitThresholds.Sprint; break;
		default:                    Movement->MaxWalkSpeed = RuntimeData.GaitThresholds.Run;    break;
		}
	}
}

void FMotionDriver::ProcessRootMotionMovement(const FVector& Delta, float DeltaTime)
{
	if (DeltaTime <= KINDA_SMALL_NUMBER) return;

	// Root Motion Delta → 瞬时速度 → 直接覆写角色速度
	FVector Velocity = Delta / DeltaTime;
	Movement->RequestDirectMove(Velocity, false);
}

void FMotionDriver::ProcessLocomotion(float DeltaTime, const FVector& WorldDir, float InAnimSpeed, const FRuntimeData& RuntimeData)
{
	FVector Dir = WorldDir.GetSafeNormal();

	// ★ 优化：根据统一解析的步态限速（由 LocomotionIntentProcessor 每帧写入）
	float SpeedCap;
	switch (RuntimeData.ResolvedGait)
	{
	case EMovementGait::Walk:   SpeedCap = RuntimeData.GaitThresholds.Walk;   break;
	case EMovementGait::Run:    SpeedCap = RuntimeData.GaitThresholds.Run;    break;
	case EMovementGait::Sprint: SpeedCap = RuntimeData.GaitThresholds.Sprint; break;
	default:                    SpeedCap = RuntimeData.GaitThresholds.Run;    break;
	}
	float CappedSpeed = FMath::Min(InAnimSpeed, SpeedCap);

	if (CappedSpeed > 0.f)
	{
		// 防滑步核心：输入方向 × 动画速度 → 直设速度
		FVector TargetVelocity = Dir * CappedSpeed;
		Movement->RequestDirectMove(TargetVelocity, false);
	}
	else
	{
		Owner->AddMovementInput(Dir, 1.0f);
	}
}

void FMotionDriver::UpdateRuntimeData(FRuntimeData& RuntimeData)
{
	if (!Owner) return;

	FVector Velocity = Owner->GetVelocity();
	RuntimeData.CurrentSpeed = Velocity.Size2D();
	RuntimeData.AnimData.VelocityLength = RuntimeData.CurrentSpeed;
	RuntimeData.bIsMoving = RuntimeData.CurrentSpeed > 10.f;

	if (RuntimeData.bIsMoving)
	{
		FVector LocalVelocity = Owner->GetActorTransform()
			.InverseTransformVector(Velocity);
		RuntimeData.MoveAngle = FMath::RadiansToDegrees(
			FMath::Atan2(LocalVelocity.Y, LocalVelocity.X));
	}
	else
	{
		RuntimeData.MoveAngle = 0.f;
	}
}
