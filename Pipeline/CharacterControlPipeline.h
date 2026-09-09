/**
 * @file CharacterControlPipeline.h
 * @brief 角色控制运行时的唯一纯 C++ 所有权和时序中心
 *
 * Unreal 对象只提供生命周期和引擎服务；本类负责角色逻辑帧内的显式阶段顺序。
 * 当前阶段顺序：Capture → Intent → Decision → Motion Commit → Animation Publish → Reset。
 */
#pragma once

#include "CoreMinimal.h"
#include "Contracts/Pipeline/CharacterFrameCommands.h"
#include "Contracts/State/StateUpdateResult.h"

class ABaseCharacter;
class UAbilitySystemComponent;
class USkeletalMeshComponent;

class FInputData;
class FInputPipeline;
class FIntentPipeline;
class FArbiterPipeline;
class FGYGOStateManager;
class FMotionDriver;
struct FRuntimeData;

/**
 * 角色控制管线在 ResolveDecision 阶段形成的本帧计划。
 *
 * 计划由两部分组成：StateManager 已完成的状态结果快照，以及由 Pipeline
 * 在 Commit/Publish 阶段消费的轻量命令缓冲。它不拥有 Unreal 对象，也不
 * 虚构当前不存在的 GameplayAbility 执行链。
 */
struct FCharacterFramePlan
{
	/** 当前意图是否包含攻击/闪避请求。 */
	bool bWantsAttack = false;
	bool bWantsDodge = false;

	/** 当前仲裁结果是否允许攻击/闪避/移动。 */
	bool bBlockMove = false;
	bool bBlockAttack = false;
	bool bBlockDodge = false;

	/** Arbiter 当前批准的动作；Idle 表示没有批准动作。 */
	ECharacterStateType ActionGranted = ECharacterStateType::Idle;

	/** StateManager 在本帧决策阶段完成后的主状态。 */
	ECharacterStateType CurrentState = ECharacterStateType::Idle;

	/** 当前帧统一解析出的步态和世界移动方向。 */
	EMovementGait ResolvedGait = EMovementGait::None;
	FVector DesiredWorldMoveDir = FVector::ZeroVector;

	/** 逻辑侧提供给动画的移动意图结果。 */
	bool bShouldMove = false;

	/** StateManager 本帧已完成的状态操作结果；Pipeline 只消费，不重复执行。 */
	FStateUpdateResult StateUpdate;

	/** 本帧由 Pipeline 统一提交的移动和动画命令。 */
	FCharacterFrameCommandBuffer Commands;

	/** 清空本帧计划，供下一帧 ResolveDecision 前重新生成。 */
	void Reset()
	{
		bWantsAttack = false;
		bWantsDodge = false;
		bBlockMove = false;
		bBlockAttack = false;
		bBlockDodge = false;
		ActionGranted = ECharacterStateType::Idle;
		CurrentState = ECharacterStateType::Idle;
		ResolvedGait = EMovementGait::None;
		DesiredWorldMoveDir = FVector::ZeroVector;
		bShouldMove = false;
		StateUpdate.Reset();
		Commands.Reset();
	}
};

/**
 * 角色控制运行时的唯一纯 C++ 所有权和时序中心。
 *
 * 本类拥有输入、RuntimeData、现有阶段管线、状态管理器和运动驱动器；
 * 它不拥有 Unreal 对象，也不启用独立 Tick。ABaseCharacter::Tick 是唯一入口。
 */
class FCharacterControlPipeline
{
public:
	FCharacterControlPipeline();
	~FCharacterControlPipeline();

	/** 注入非拥有的 Unreal 依赖，并按 BeginPlay 依赖顺序初始化所有纯 C++ 子系统。 */
	void Initialize(
		ABaseCharacter* InOwner,
		UAbilitySystemComponent* InASC,
		USkeletalMeshComponent* InMesh);

	/**
	 * 唯一角色逻辑帧入口。
	 * 顺序：Capture → Intent → Decision → Motion Commit → Animation Publish → Reset。
	 */
	void ProcessFrame(float DeltaTime);

	/** 返回管线拥有的运行时黑板，返回非拥有指针。 */
	FRuntimeData* GetRuntimeData() const;

	/** 输入边界：只把外部输入写入 InputPipeline 的 Pending 区域。 */
	void SetMoveInput(const FVector2D& Value);
	void ClearMoveInput();
	void SetLookInput(const FVector2D& Value);
	void SetSprintHeld(bool bHeld);
	void SetForceWalkHeld(bool bHeld);

	/** 接收 AnimNotify_CanYaw，并转发给 TurnBack 参数处理器。 */
	void NotifyCanYaw();

	/** 返回最近一次完成 ResolveDecision 的本帧计划，只读且由管线拥有。 */
	const FCharacterFramePlan& GetLastFramePlan() const { return LastFramePlan; }

	/** 返回初始化状态。 */
	bool IsInitialized() const { return bInitialized; }

private:
	/** 采集外部输入和 ASC/状态约束；不做角色决策。 */
	void CaptureInputAndConstraints(float DeltaTime);

	/** 将稳定 InputData 转换为 RuntimeData 意图。 */
	void BuildIntent();

	/** 执行步态、参数和 StateManager 决策，并生成状态结果与本帧命令。 */
	void ResolveDecision(float DeltaTime);

	/** 消费本帧移动命令，提交给 CharacterMovement 并回写实际运动结果。 */
	void CommitMovement(float DeltaTime);

	/** 消费本帧动画发布命令，提交给已有的 UZZZAnimInstance。 */
	void PublishAnimation(float DeltaTime);

	/** 只清理帧级意图，不清理跨帧状态或最近一次计划。 */
	void ResetFrame();

	/** 从 RuntimeData 和 StateManager 结果生成统一的本帧计划与命令。 */
	void BuildFramePlan(const FStateUpdateResult& StateUpdateResult);

	/** 纯 C++ 数据和阶段对象的唯一所有权。 */
	TUniquePtr<FInputData> InputData;
	TUniquePtr<FRuntimeData> RuntimeData;
	TUniquePtr<FInputPipeline> InputPipeline;
	TUniquePtr<FIntentPipeline> IntentPipeline;
	TUniquePtr<FArbiterPipeline> ArbiterPipeline;
	TUniquePtr<FGYGOStateManager> StateManager;
	TUniquePtr<FMotionDriver> MotionDriver;

	/** Unreal 依赖均为非拥有引用，仅用于阶段初始化和提交副作用。 */
	ABaseCharacter* Owner = nullptr;
	UAbilitySystemComponent* ASC = nullptr;
	USkeletalMeshComponent* Mesh = nullptr;

	FCharacterFramePlan LastFramePlan;
	bool bInitialized = false;
};
