/**
 * @file CharacterControlPipeline.cpp
 * @brief 角色控制运行时的唯一纯 C++ 所有权和时序中心实现
 */
#include "Pipeline/CharacterControlPipeline.h"

#include "Animation/zzzAnim/ZZZAnimInstance.h"
#include "BaseCharacter.h"
#include "Components/SkeletalMeshComponent.h"
#include "Data/Input/InputData.h"
#include "Data/Runtime/RuntimeData.h"
#include "Drivers/MotionDriver.h"
#include "Pipeline/ArbiterPipeline.h"
#include "Pipeline/InputPipeline.h"
#include "Pipeline/IntentPipeline.h"
#include "StateMachine/GGYGOStateManager.h"

FCharacterControlPipeline::FCharacterControlPipeline()
{
	InputData = MakeUnique<FInputData>();
	RuntimeData = MakeUnique<FRuntimeData>();
	InputPipeline = MakeUnique<FInputPipeline>(*InputData);
	IntentPipeline = MakeUnique<FIntentPipeline>();
	ArbiterPipeline = MakeUnique<FArbiterPipeline>();
	StateManager = MakeUnique<FGYGOStateManager>();
	MotionDriver = MakeUnique<FMotionDriver>();
}

FCharacterControlPipeline::~FCharacterControlPipeline() = default;

void FCharacterControlPipeline::Initialize(
	ABaseCharacter* InOwner,
	UAbilitySystemComponent* InASC,
	USkeletalMeshComponent* InMesh)
{
	if (bInitialized)
	{
		return;
	}

	Owner = InOwner;
	ASC = InASC;
	Mesh = InMesh;

	// 保持 BaseCharacter::BeginPlay 的依赖顺序：
	// ASC/仲裁 → Intent → Motion → StateManager::Init → StateManager::InitASC。
	if (ASC)
	{
		ArbiterPipeline->Init(ASC, StateManager.Get());
	}

	IntentPipeline->Init(Owner, Mesh);
	MotionDriver->Init(Owner);
	StateManager->Init(*RuntimeData);
	StateManager->InitASC(ASC);

	bInitialized = true;
}

void FCharacterControlPipeline::ProcessFrame(float DeltaTime)
{
	if (!bInitialized)
	{
		return;
	}

	// 1. Capture：只采集输入和当前外部约束，不在此阶段选择角色动作。
	CaptureInputAndConstraints(DeltaTime);

	// 2. Intent：输入 → 角色意图。
	BuildIntent();

	// 3. Decision：意图/约束 → 步态、参数、状态和统一本帧计划。
	ResolveDecision(DeltaTime);

	// 4. Commit：把已形成的移动结果提交给 Unreal CharacterMovement。
	CommitMovement(DeltaTime);

	// 5. Animation Publish：所有逻辑生产者完成后发布本帧动画输入。
	PublishAnimation(DeltaTime);

	// 6. Reset：只清理一次性输入意图，保留跨帧模型和最近一次计划。
	ResetFrame();
}

void FCharacterControlPipeline::CaptureInputAndConstraints(float DeltaTime)
{
	// 保持现有行为：仲裁先于输入，输入阶段仍只负责把 Pending 数据整理为 CurrentFrame。
	ArbiterPipeline->Process(*RuntimeData, DeltaTime);
	InputPipeline->Process(DeltaTime);
}

void FCharacterControlPipeline::BuildIntent()
{
	IntentPipeline->ProcessIntents(*InputData, *RuntimeData);
}

void FCharacterControlPipeline::ResolveDecision(float DeltaTime)
{
	// 步态和参数属于本帧决策的前置结果；StateManager 使用这些结果完成状态决策。
	IntentPipeline->ProcessGait(*InputData, *RuntimeData, DeltaTime);
	IntentPipeline->ProcessParameters(*RuntimeData, DeltaTime);

	FStateUpdateResult StateUpdateResult;
	StateManager->Update(DeltaTime, StateUpdateResult);

	BuildFramePlan(StateUpdateResult);
}

void FCharacterControlPipeline::CommitMovement(float DeltaTime)
{
	if (!LastFramePlan.Commands.Movement.bShouldCommit)
	{
		return;
	}

	MotionDriver->Process(
		DeltaTime,
		LastFramePlan.Commands.Movement,
		*RuntimeData);
}

void FCharacterControlPipeline::PublishAnimation(float DeltaTime)
{
	if (!LastFramePlan.Commands.Animation.bShouldPublish || !Mesh)
	{
		return;
	}

	// AnimInstance 仍由 SkeletalMesh/AnimBP 拥有；Pipeline 只在明确的发布阶段驱动它。
	if (UZZZAnimInstance* ZAI = Cast<UZZZAnimInstance>(Mesh->GetAnimInstance()))
	{
		ZAI->PipelineDrive(DeltaTime);
	}
}

void FCharacterControlPipeline::ResetFrame()
{
	RuntimeData->ResetFrameIntents();
}

void FCharacterControlPipeline::BuildFramePlan(const FStateUpdateResult& StateUpdateResult)
{
	LastFramePlan.Reset();

	LastFramePlan.bWantsAttack = RuntimeData->Intent.bWantsToAttack;
	LastFramePlan.bWantsDodge = RuntimeData->Intent.bWantsToDodge;
	LastFramePlan.bBlockMove = RuntimeData->Arbiter.bBlockMove;
	LastFramePlan.bBlockAttack = RuntimeData->Arbiter.bBlockAttack;
	LastFramePlan.bBlockDodge = RuntimeData->Arbiter.bBlockDodge;
	LastFramePlan.ActionGranted = RuntimeData->Arbiter.ActionGranted;
	LastFramePlan.CurrentState = RuntimeData->State.CurrentState;
	LastFramePlan.ResolvedGait = RuntimeData->Gait.ResolvedGait;
	LastFramePlan.DesiredWorldMoveDir = RuntimeData->Intent.DesiredWorldMoveDir;
	LastFramePlan.bShouldMove = RuntimeData->ZZZAnim.bShouldMove;
	LastFramePlan.StateUpdate = StateUpdateResult;

	// 只把当前源码已有的移动提交所需字段复制到命令缓冲；RuntimeData 仍是 canonical model。
	FCharacterMovementCommand& MovementCommand = LastFramePlan.Commands.Movement;
	MovementCommand.bShouldCommit = true;
	MovementCommand.bShouldMove = RuntimeData->ZZZAnim.bShouldMove;
	MovementCommand.bBlockMove = RuntimeData->Arbiter.bBlockMove;
	MovementCommand.DesiredWorldMoveDir = RuntimeData->Intent.DesiredWorldMoveDir;
	MovementCommand.TurnBackPhase = RuntimeData->Movement.TurnBack.Phase;
	MovementCommand.bTurnBackSecondSegment = RuntimeData->Movement.TurnBack.bSecondSegment;
	MovementCommand.AnimCurveSpeed = RuntimeData->RootMotion.AnimCurveSpeed;
	MovementCommand.AnimCurveYawDelta = RuntimeData->RootMotion.AnimCurveYawDelta;
	MovementCommand.AnimCurveVelocity = RuntimeData->RootMotion.AnimCurveVelocity;
	MovementCommand.AnimCurveVelocityDirection = RuntimeData->RootMotion.AnimCurveVelocityDirection;
	MovementCommand.bHasAuthoredVelocityDirection = RuntimeData->RootMotion.bHasAuthoredVelocityDirection;
	MovementCommand.bHasRootMotionCurveSource = RuntimeData->RootMotion.bHasRootMotionCurveSource;
	MovementCommand.RootMotionDelta = RuntimeData->RootMotion.RootMotionDelta;
	MovementCommand.bHasRootMotion = RuntimeData->RootMotion.bHasRootMotion;

	// 动画发布命令只表示 PipelineDrive 调用；AnimInstance/AnimBP 仍由 Mesh 拥有。
	LastFramePlan.Commands.Animation.bShouldPublish = Mesh != nullptr;
}

FRuntimeData* FCharacterControlPipeline::GetRuntimeData() const
{
	return RuntimeData.Get();
}

void FCharacterControlPipeline::SetMoveInput(const FVector2D& Value)
{
	InputPipeline->SetMoveInput(Value);
}

void FCharacterControlPipeline::ClearMoveInput()
{
	InputPipeline->ClearMoveInput();
}

void FCharacterControlPipeline::SetLookInput(const FVector2D& Value)
{
	InputPipeline->SetLookInput(Value);
}

void FCharacterControlPipeline::SetSprintHeld(bool bHeld)
{
	InputPipeline->SetSprintHeld(bHeld);
}

void FCharacterControlPipeline::SetForceWalkHeld(bool bHeld)
{
	InputPipeline->SetForceWalkHeld(bHeld);
}

void FCharacterControlPipeline::NotifyCanYaw()
{
	if (IntentPipeline)
	{
		IntentPipeline->NotifyCanYaw();
	}
}

