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
#include "Data/RuntimeData.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"

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
		ProcessLocomotion(DeltaTime, RuntimeData.DesiredWorldMoveDir, RuntimeData.AnimSpeed);
	}

	// 回写实际速度、移动状态到 RuntimeData 供调试/UI 使用
	UpdateRuntimeData(RuntimeData);
}

void FMotionDriver::ProcessRootMotionMovement(const FVector& Delta, float DeltaTime)
{
	if (DeltaTime <= KINDA_SMALL_NUMBER) return;

	// Root Motion Delta → 瞬时速度 → 直接覆写角色速度
	FVector Velocity = Delta / DeltaTime;
	Movement->RequestDirectMove(Velocity, false);
}

void FMotionDriver::ProcessLocomotion(float DeltaTime, const FVector& WorldDir, float InAnimSpeed)
{
	FVector Dir = WorldDir.GetSafeNormal();

	if (InAnimSpeed > 0.f)
	{
		// 防滑步核心：输入方向 × 动画速度 → 直设速度，绕过 CMC 加速/减速
		FVector TargetVelocity = Dir * InAnimSpeed;
		Movement->RequestDirectMove(TargetVelocity, false);
	}
	else
	{
		// 降级路径：AnimSpeed 不可用时退化为标准 AddMovementInput
		// 注意：此路径走 CMC 加速度，会产生 ~0.3s 启动延迟
		Owner->AddMovementInput(Dir, 1.0f);
	}
}

void FMotionDriver::UpdateRuntimeData(FRuntimeData& RuntimeData)
{
	if (!Owner) return;

	FVector Velocity = Owner->GetVelocity();
	RuntimeData.CurrentSpeed = Velocity.Size2D();
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
