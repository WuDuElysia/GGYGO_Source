/**
 * @file UCharConfigData.h
 * @brief 角色配置数据资产 — 混合时间 / 过渡参数 / 移动 / GAS 配置
 *
 * 集中存放每个角色的差异化运行时参数：
 *     - 混合时间（每对状态转换独立控制，替代全局固定值）
 *     - 最短过渡时长（防止动画被过早打断）
 *     - 播放速率
 *     - 移动参数（FMovementConfig 内联）
 *     - GAS 默认能力/效果列表
 *
 * 使用方式：
 *   1. 内容浏览器 → 右键 → Miscellaneous → 数据资产(DataAsset) → 选 GGYGO.UCharConfigData
 *   2. 命名如 DA_MiyabiConfig
 *   3. 在细节面板填入该角色的混合时间、移动参数等
 *   4. 角色蓝图在 BaseCharacter.CharacterConfig 字段引用此资产
 *
 * 动画资产本身仍在 AnimBP 的 EditAnywhere 属性上配置（IdleAnim / RunLoopAnim 等），
 * 本 DataAsset 不重复存储动画引用。
 */
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Movement/MovementConfig.h"
#include "StateMachine/CharacterStateType.h"
#include "UCharConfigData.generated.h"

class UGameplayAbility;
class UGameplayEffect;

/**
 * 单个状态的过渡混合参数
 * 控制进入/离开该状态时的淡入淡出时间
 */
USTRUCT(BlueprintType)
struct FStateTransitionBlend
{
	GENERATED_BODY()

	/** 进入此状态时的淡入时间（秒），0 = 瞬切 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BlendInTime = 0.2f;

	/** 离开此状态时的淡出时间（秒），0 = 瞬切 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BlendOutTime = 0.2f;
};

/**
 * 角色配置数据资产
 *
 * 每个 DataAsset 实例代表一个角色的运行时参数配置。
 * 动画资产不在本类中，由 AnimBP 自身属性管理。
 */
UCLASS()
class GGYGO_API UCharConfigData : public UDataAsset
{
	GENERATED_BODY()

public:
	UCharConfigData();

	// ============================================================
	// 混合时间配置（核心：每状态独立控制）
	// ============================================================

	/** 全局默认混合时间（秒），PerStateBlendOverrides 未覆盖的状态使用此值 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "混合时间")
	float DefaultBlendDuration = 0.2f;

	/**
	 * 每状态独立混合时间覆盖表
	 * Key = 状态类型，Value = 该状态的 BlendIn/BlendOut 时间
	 *
	 * 推荐值：
	 *   Idle:      BlendIn=0.1  BlendOut=0.1
	 *   RunStart:  BlendIn=0.05 BlendOut=0.1
	 *   RunLoop:   BlendIn=0.15 BlendOut=0.15
	 *   RunEnd:    BlendIn=0.15 BlendOut=0.1
	 *   InAir:     BlendIn=0.1  BlendOut=0.1
	 *   Attacking: BlendIn=0.0  BlendOut=0.1
	 *   Dodging:   BlendIn=0.0  BlendOut=0.0
	 *   HitStun:   BlendIn=0.0  BlendOut=0.05
	 *   Stunned:   BlendIn=0.05 BlendOut=0.1
	 *   Dead:      BlendIn=0.3  BlendOut=0.0
	 *   Interact:  BlendIn=0.1  BlendOut=0.1
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "混合时间")
	TMap<ECharacterStateType, FStateTransitionBlend> PerStateBlendOverrides;

	// ============================================================
	// 最短播放时长（防止动画被过早打断）
	// ============================================================

	/** RunStart 最短播放时间（秒），此后才允许切换到 RunLoop */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "过渡时间")
	float RunStartMinDuration = 0.25f;

	/** RunEnd 最短播放时间（秒），此后才允许切换到 Idle */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "过渡时间")
	float RunEndMinDuration = 0.25f;

	// ============================================================
	// 播放速率
	// ============================================================

	/** 循环动画默认播放倍率（1.0 = 原速） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "播放速率")
	float LoopAnimPlayRate = 1.0f;

	/** 非循环动画默认播放倍率（1.0 = 原速） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "播放速率")
	float NonLoopAnimPlayRate = 1.0f;

	// ============================================================
	// 移动参数（内联 MovementConfig）
	// ============================================================

	/** 移动系统配置参数 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "移动")
	FMovementConfig MovementConfig;

	// ============================================================
	// GAS 默认配置
	// ============================================================

	/** 角色创建时自动授予的 Ability 列表 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GAS")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	/** 初始化属性用的 GE 列表 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GAS")
	TArray<TSubclassOf<UGameplayEffect>> DefaultEffects;

public:
	// ============================================================
	// 查询方法
	// ============================================================

	/** 获取目标状态的混合淡入时间，未覆盖则返回 DefaultBlendDuration */
	float GetBlendInTime(ECharacterStateType TargetState) const;

	/** 获取源状态的混合淡出时间，未覆盖则返回 DefaultBlendDuration */
	float GetBlendOutTime(ECharacterStateType SourceState) const;

	/** 获取指定状态的播放倍率：循环状态用 LoopAnimPlayRate，否则用 NonLoopAnimPlayRate */
	float GetPlayRateForState(ECharacterStateType State) const;
};
