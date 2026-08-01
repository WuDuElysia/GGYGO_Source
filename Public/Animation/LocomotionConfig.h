/**
 * @file LocomotionConfig.h
 * @brief NTE 风格移动动画的配置类型
 *
 * 定义动画决策层使用的配置数据：
 *   EAnimFoot          支撑脚枚举（Left/Right）
 *   FLocomotionAnimSet key→value 动画资产表（BlendSpace/Loop/Enter/Stop/Misc）
 *   FLocomotionTuning  数值参数（步态速度、停止阈值、转身角度、Lean 钳制等）
 *
 * @NTEAnim: 连接点H - NTEAnim 的配表与参数配置
 * AnimSet 由美术在 AnimBP 细节面板按 key 填 value；Tuning 使用 NTE 实证默认值。
 * 过渡混合时长不在本文件配置，由蓝图过渡的 Blend Duration 承担。
 */
#pragma once

#include "CoreMinimal.h"
#include "StateMachine/CharacterStateType.h"  // EMovementGait
#include "LocomotionConfig.generated.h"

// 前向声明（减少编译依赖，资产指针在运行时解析）
class UBlendSpace;
class UAnimSequence;

/**
 * 支撑脚枚举
 * 表示当前支撑相所在脚，由循环动画的 SyncMarker 相位查询得出
 */
UENUM(BlueprintType)
enum class EAnimFoot : uint8
{
	/** 左脚支撑 */
	Left,

	/** 右脚支撑 */
	Right
};

/**
 * 移动动画资产表
 * 以 key→value 形式存放各类动画资产引用，美术在细节面板按 key 填 value。
 * 五组资产分别对应循环混合、待机循环、起步、停步与杂项一次性动画。
 */
USTRUCT(BlueprintType)
struct FLocomotionAnimSet
{
	GENERATED_BODY()

	/** 循环混合空间：如 "WalkRun_L"、"WalkRun_R"、"Lean_BS" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "BlendSpace")
	TMap<FName, TObjectPtr<UBlendSpace>> BlendSpaces;

	/** 循环序列：如 "Idle" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loop")
	TMap<FName, TObjectPtr<UAnimSequence>> LoopSequences;

	/** 起步序列：如 "run_enterFL"、"run_enterFR"、"run_enterL"、"run_enterR"、"run_enterB_Lstart"、"run_enterB_Rstart" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Enter")
	TMap<FName, TObjectPtr<UAnimSequence>> EnterSequences;

	/** 停步序列：如 "run_stopL"、"run_stopR"、"walk_stopL"、"walk_stopR"、"sprint_stopL"、"sprint_stopR" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stop")
	TMap<FName, TObjectPtr<UAnimSequence>> StopSequences;

	/** 杂项一次性序列：如 "turn_180"、"land" */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Misc")
	TMap<FName, TObjectPtr<UAnimSequence>> MiscSequences;
};

/**
 * 移动数值参数
 * 存放决策与输出计算所需的数值，默认值复刻 NTE（kuhara）实证数据。
 * 不含过渡混合时长——过渡时长在蓝图过渡的 Blend Duration 中配置。
 */
USTRUCT(BlueprintType)
struct FLocomotionTuning
{
	GENERATED_BODY()

	/** 步行速度阈值（cm/s），WalkRun 轴映射下界 */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float WalkSpeed = 115.f;

	/** 奔跑速度阈值（cm/s），WalkRun 轴映射上界 */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float RunSpeed = 450.f;

	/** 冲刺速度阈值（cm/s） */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float SprintSpeed = 620.f;

	/** 触发急停的速度阈值（cm/s），低于此值判定进入停步 */
	UPROPERTY(EditAnywhere, Category = "Gait")
	float ThresholdToStop = 160.f;

	/** 触发转身的移动角度阈值（度），移动角绝对值大于此值判定转身 */
	UPROPERTY(EditAnywhere, Category = "Turn")
	float TurnBackAngle = 150.f;

	/** Lean 倾斜量钳制范围（±值） */
	UPROPERTY(EditAnywhere, Category = "Lean")
	float LeanAmountClamp = 0.22f;

	/** 速度混合插值速率，用于平滑 WalkRun 轴值 */
	UPROPERTY(EditAnywhere, Category = "Blend")
	float VelocityBlendInterp = 2.f;

	/** 落地重击阈值（cm/s），落地冲击速度大于此值判定为重着地 */
	UPROPERTY(EditAnywhere, Category = "Land")
	float JumpLandedThreshold = 1700.f;
};
