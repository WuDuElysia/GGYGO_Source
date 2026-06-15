/**
 * @file GGYGOAnimInstance.h
 * @brief 动画实例 C++ 基类 — 状态驱动动画播放 + 根运动提取
 *
 * 动画资产：通过自身 EditAnywhere 属性配置（IdleAnim / RunStartAnim / RunLoopAnim / RunEndAnim）
 * 运行时参数（混合时间/过渡时长/播放速率）：从 BaseCharacter.CharacterConfig（UCharConfigData）读取
 *
 * 动画播放策略：
 *   循环动画（Idle/RunLoop）：预创建 UAnimMontage 缓存复用
 *   非循环动画（RunStart/RunEnd）：动态 Montage 播一次
 *
 * 混合时间机制：
 *   每对状态转换使用独立的混合时间，通过 Config.PerStateBlendOverrides 配置：
 *     - Idle→RunStart:   0.05~0.1s （快速响应）
 *     - RunLoop→RunEnd:  0.15~0.2s （自然减速）
 *     - Any→HitStun:     0.0~0.05s  （受击瞬切）
 *     - Any→Dodge:       0.0s        （闪避瞬切）
 *     - Any→Dead:        0.3~0.5s    （死亡慢入）
 *   Config 为空时回退全局固定 0.2s。
 *
 * 根运动提取：
 *   每帧从当前播放的 AnimSequence 内置根骨骼轨道提取位移数据，
 *   通过 RootMotionDelta 暴露给 RootMotionParameterProcessor。
 *
 * ABP_Miyabi AnimGraph: Slot(DefaultSlot) → Output Pose
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "StateMachine/CharacterStateType.h"
#include "Data/UCharConfigData.h"
#include "GGYGOAnimInstance.generated.h"

/**
 * 动画实例 C++ 基类
 *
 * 使用 PlaySlotAnimationAsDynamicMontage / Montage_Play 在 DefaultSlot 上播放动画。
 * 不依赖动画蓝图连线，所有动画切换由 NativeUpdateAnimation 驱动。
 */
UCLASS()
class GGYGO_API UGGYGOAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	// ============================================================
	// AnimNotify 回调（动画末尾通知状态机，保留作备用）
	// ============================================================

	UFUNCTION(BlueprintCallable)
	void OnRunStartFinished();

	UFUNCTION(BlueprintCallable)
	void OnRunEndFinished();

	// ============================================================
	// 动画资产（在 AnimBP 细节面板配置）
	// ============================================================

	/** Idle 待机动画（循环） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "动画资产")
	UAnimSequence* IdleAnim;

	/** RunStart 跑步启动动画（不循环） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "动画资产")
	UAnimSequence* RunStartAnim;

	/** RunLoop 跑步循环动画（循环） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "动画资产")
	UAnimSequence* RunLoopAnim;

	/** RunEnd 跑步停止动画（不循环） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "动画资产")
	UAnimSequence* RunEndAnim;

	// ============================================================
	// 运行时状态（ABP 只读 / C++ 处理器读取）
	// ============================================================

	/** 当前角色状态，由 FCharacterStateMachine 写入 */
	UPROPERTY(BlueprintReadOnly, Category = "Runtime")
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	/**
	 * 动画驱动速度（cm/s），从根运动位移向量换算而来
	 * 公式：AnimSpeed = |RootMotionDelta| / DeltaTime
	 * 由 RootMotionParameterProcessor 写入（每帧同步自 RootMotionDelta）
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Runtime")
	float AnimSpeed = 0.f;

	/** 是否在移动中，由 MotionDriver 写入 */
	UPROPERTY(BlueprintReadOnly, Category = "Runtime")
	bool bIsMoving = false;

	/**
	 * 当前帧的根骨骼位移向量（cm/帧）
	 * 从当前播放的 AnimSequence 内置根骨骼轨道直接提取。
	 * 方向 = 动画前进方向，大小 = 该帧根骨骼移动距离。
	 *
	 * 数据消费者：
	 *   - RootMotionParameterProcessor：读此值 → 写入 RuntimeData.RootMotionDelta / AnimSpeed
	 *   - 调试 UI：显示实际 RM 向量
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Runtime")
	FVector RootMotionDelta = FVector::ZeroVector;

	/** 获取根运动位移（C++ 接口，供 RootMotionParameterProcessor 调用） */
	const FVector& GetRootMotionDelta() const { return RootMotionDelta; }

private:
	// ============================================================
	// 内部状态
	// ============================================================

	/** 上一帧的角色状态，用于检测状态变化 */
	ECharacterStateType LastState = ECharacterStateType::Idle;

	/** 非循环动画已播放时长（秒），用于检测动画播完 */
	float AnimTimeElapsed = 0.f;

	/** 是否第一次 NativeUpdateAnimation，第一帧做初始化 */
	bool bFirstUpdate = true;

	/** 缓存的角色配置引用（每帧从 BaseCharacter 获取，避免重复 Cast） */
	UCharConfigData* CachedConfig = nullptr;

	// ============================================================
	// 根运动提取相关
	// ============================================================

	/** 当前正在播放的 AnimSequence（ApplyStateAnimation 时更新） */
	UAnimSequence* CurrentPlayingAnim = nullptr;

	/** 上一帧提取根运动时的动画播放时间（秒），用于计算帧间位移 */
	float LastExtractedTime = 0.f;

	/** 根运动提取是否已通过首帧初始化（切换动画后重置为 false，首帧只记录时间基准不产生位移） */
	bool bRootMotionInitialized = false;

	/**
	 * 从当前播放的 AnimSequence 提取根骨骼帧间位移
	 * @param DeltaSeconds 帧间隔（保留参数，当前实现未直接使用，
	 *   位移大小由 ExtractRootMotion API 根据时间差自动计算）
	 *
	 * 实现原理：
	 *   1. 获取当前动画播放时间（MontageInstance 优先，回退内部计时器）
	 *   2. 调用 UAnimSequence::ExtractRootMotion(StartTime, DeltaTime, bAllowLooping)
	 *      提取 [PrevTime, PrevTime+DeltaTime] 区间的根骨骼累积 Transform（返回值）
	 *   3. 取返回值 GetTranslation() 写入 RootMotionDelta
	 */
	void ExtractRootMotionDelta(float DeltaSeconds);

	// ============================================================
	// 循环蒙太奇缓存（UPROPERTY 防止 GC 回收）
	// 支持所有循环状态：Idle / RunLoop / InAir / Stunned
	// ============================================================

	/** Idle 循环蒙太奇 */
	UPROPERTY()
	UAnimMontage* IdleMontage = nullptr;

	/** RunLoop 循环蒙太奇 */
	UPROPERTY()
	UAnimMontage* RunLoopMontage = nullptr;

	/** InAir 循环蒙太奇 */
	UPROPERTY()
	UAnimMontage* InAirMontage = nullptr;

	/** Stunned 循环蒙太奇 */
	UPROPERTY()
	UAnimMontage* StunnedMontage = nullptr;

	// ============================================================
	// 私有方法
	// ============================================================

	/**
	 * 根据新状态切换动画（自身属性取动画 + Config 取混合时间）
	 * @param State 目标角色状态
	 */
	void ApplyStateAnimation(ECharacterStateType State);

	/**
	 * 每帧检查非循环动画是否已播完
	 * @param DeltaSeconds 帧间隔
	 */
	void TickAnimCompletion(float DeltaSeconds);

	/**
	 * 非循环动画播完时通知状态机
	 */
	void NotifyAnimFinished();

	/**
	 * 启动时预创建所有循环状态的蒙太奇（Idle/RunLoop/InAir/Stunned）
	 */
	void EnsureLoopingMontages();

	// ============================================================
	// Config 查询方法（混合时间 / 播放速率从 Config 读取）
	// ============================================================

	/** 获取当前帧缓存的角色配置引用 */
	UCharConfigData* GetActiveConfig() const;

	/**
	 * 获取目标状态的混合淡入时间
	 * 有 Config → Config.GetBlendInTime，无 Config → 全局固定 0.2s
	 */
	float GetBlendInDuration(ECharacterStateType TargetState) const;

	/** 获取源状态的混合淡出时间 */
	float GetBlendOutDuration(ECharacterStateType SourceState) const;

	/**
	 * 获取循环状态的缓存 Montage
	 * @param State 循环状态（Idle/RunLoop/InAir/Stunned）
	 * @return 对应的缓存 Montage，不匹配返回 nullptr
	 */
	UAnimMontage* GetLoopMontage(ECharacterStateType State) const;
};
