/**
 * @file MovementConfig.h
 * @brief 移动系统配置参数（USTRUCT，蓝图可调）
 * 
 * 在角色蓝图的细节面板里调参。
 * 所有移动相关的可调参数集中在这里管理。
 */

#pragma once

#include "CoreMinimal.h"
#include "MovementConfig.generated.h"

/**
 * 移动系统配置参数
 * 挂在 BaseCharacter 上，蓝图里调参
 */
USTRUCT(BlueprintType)
struct FMovementConfig
{
	GENERATED_BODY()

	/** 冲刺速度倍率（相对于基础速度） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sprint")
	float SprintMultiplier = 1.5f;

	/** Walk 连续保持自动升 Run 的时长阈值（秒）；面板钳制只约束输入，运行期按 Gait_Authority 的回退规则解析 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gait", meta = (ClampMin = "0.1", ClampMax = "60.0"))
	float WalkToRunHoldSeconds = 5.f;

	/** 冲刺最小速度阈值（cm/s，≥此值为 Sprint） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gait")
	float SprintSpeed = 600.f;

	/** 空中移动控制系数（0=完全不能控制，1=和地面一样） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air")
	float AirControlFactor = 0.3f;

	/** 闪避速度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dodge")
	float DodgeSpeed = 1200.f;

	/** 闪避持续时间（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Dodge")
	float DodgeDuration = 0.3f;

	/** 击飞衰减速率（每秒衰减多少） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Knockback")
	float KnockbackDecay = 5.f;

	/** 转向插值速度（越大转向越快） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rotation")
	float RotationInterpSpeed = 10.f;

	/** Root Motion 位移缩放系数（1.0=原始数据） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RootMotion")
	float RootMotionScale = 1.0f;

	/** 调试：可视化移动数据 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugMotion = false;
};
