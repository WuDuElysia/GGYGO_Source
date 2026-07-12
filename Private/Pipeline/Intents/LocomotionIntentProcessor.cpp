/**
 * @file LocomotionIntentProcessor.cpp
 * @brief 移动意图处理器实现 — 步态解析核心
 *
 * ============================================================================
 * 模块职责
 * ============================================================================
 *
 * 本处理器位于意图管线（IntentPipeline）中，负责：
 *   1. 将 2D 摇杆输入转换到世界空间期望移动方向
 *   2. 解析当前步态（Walk/Run/Sprint）写入 RuntimeData->ResolvedGait
 *
 * ResolvedGait 的消费者：
 *   - AnimInstance：据此选择 WalkAnim/RunAnim/SprintAnim
 *   - MotionDriver：据此决定速度上限（Walk/Run/Sprint 对应不同 MaxSpeed）
 *
 * ============================================================================
 * 步态解析优先级（从高到低）
 * ============================================================================
 *
 *   优先级 1: Ctrl 强制 Walk（bForceWalkHeld）
 *             无论速度多少，按住 Ctrl 一定是 Walk
 *             场景：玩家想精确移动、贴墙走、潜行
 *
 *   优先级 2: Shift 强制 Sprint（bSprintHeld）
 *             无论速度多少，按住 Shift 一定是 Sprint
 *             场景：玩家明确想冲刺（注意：实际能否达到 Sprint 速度由 MotionDriver 决定）
 *
 *   优先级 3: 速度驱动 Run（CurrentSpeed >= WalkThreshold）
 *             无按键修饰时，速度达到 Walk 阈值即升级为 Run
 *             场景：玩家推满摇杆，速度自然上升到 Run
 *
 *   优先级 4: 摇杆幅度预测（StickMag >= 0.5 → Run, 否则 Walk）
 *             速度尚未起来时的预判，避免动画延迟感
 *             场景：刚推摇杆瞬间，速度还在加速，但摇杆已推到一半以上
 *
 * ============================================================================
 * 阈值约定（来自 RuntimeData->GaitThresholds）
 * ============================================================================
 *
 *   Walk   = 95 cm/s    (CharacterConfig->MovementConfig.WalkSpeed)
 *   Run    = 450 cm/s   (CharacterConfig->MovementConfig.RunSpeed)
 *   Sprint = 600 cm/s   (CharacterConfig->MovementConfig.SprintSpeed)
 *
 * 注意：Sprint 通常只能通过闪避后进入的特殊状态触发，不是普通按键就能达到
 *
 * ============================================================================
 * 数据流
 * ============================================================================
 *
 *   输入：InputData.CurrentFrame
 *     ├─ Move (FVector2D)           摇杆 2D 输入
 *     ├─ bSprintHeld (bool)         Shift 按住
 *     └─ bForceWalkHeld (bool)      Ctrl 按住
 *
 *   输出：RuntimeData
 *     ├─ DesiredWorldMoveDir (FVector)  世界空间期望移动方向
 *     ├─ bWantsToSprint (bool)          原始冲刺意图（兼容旧消费者）
 *     ├─ bWantsToForceWalk (bool)       原始强制步行意图
 *     └─ ResolvedGait (EMovementGait)   解析后的步态（核心输出）
 */
#include "Pipeline/Intents/LocomotionIntentProcessor.h"
#include "Data/InputData.h"
#include "Data/RuntimeData.h"

void FLocomotionIntentProcessor::Process(const FInputData& InputData, FRuntimeData& RuntimeData)
{
	// 读取本帧摇杆输入（2D 向量，X=右 Y=前，范围 [-1,1]）
	const FVector2D& MoveInput = InputData.CurrentFrame.Move;

	// ------------------------------------------------------------
	// 空输入处理：清零期望方向 + 步态置 None
	// 此时不进入任何步态解析逻辑，ResolvedGait=None
	// AnimInstance 见 None 不会触发步态切换（见 Moving 阶段逻辑）
	// ------------------------------------------------------------
	if (MoveInput.IsNearlyZero())
	{
		RuntimeData.DesiredWorldMoveDir = FVector::ZeroVector;
		RuntimeData.ResolvedGait = EMovementGait::None;
		return;
	}

	// ------------------------------------------------------------
	// 输入空间 → 世界空间转换
	//
	// 摇杆输入是相对摄像机的局部空间（Y=摄像机前方，X=摄像机右方）
	// 需要旋转到世界空间得到实际移动方向
	//
	// ControlRotation.Yaw 是摄像机水平朝向（ViewRotationProcessor 已写入）
	// 构造仅含 Yaw 的旋转矩阵，提取 Forward 和 Right 单位向量
	// ------------------------------------------------------------
	FRotator YawRot(0.f, RuntimeData.ControlRotation.Yaw, 0.f);
	FVector Forward = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);  // 摄像机前方（水平）
	FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);    // 摄像机右方（水平）

	// 线性组合：摇杆 Y 分量乘 Forward + X 分量乘 Right
	// 例：摇杆 (0,1) → 纯 Forward → 角色向前走
	//     摇杆 (1,0) → 纯 Right   → 角色向右走
	//     摇杆 (1,1) → 45° 方向   → 角色向右前方走
	FVector WorldDir = (Forward * MoveInput.Y + Right * MoveInput.X);

	// 清零 Z 分量（强制水平移动，避免因摄像机俯仰导致 Y 向量有 Z 分量）
	WorldDir.Z = 0.f;

	// 归一化后写入期望方向（单位向量）
	RuntimeData.DesiredWorldMoveDir = WorldDir.GetSafeNormal();

	// ------------------------------------------------------------
	// 保留原始意图（兼容旧消费者）
	// 这些 bool 标志是"玩家想要什么"，未经速度仲裁
	// ------------------------------------------------------------
	RuntimeData.bWantsToSprint    = InputData.CurrentFrame.bSprintHeld;
	RuntimeData.bWantsToForceWalk = InputData.CurrentFrame.bForceWalkHeld;

	// ============================================================
	// ★ 步态解析核心（统一逻辑）
	//
	// 优先级：Ctrl > Shift > 速度 > 摇杆幅度
	//
	// 设计原则：
	//   - CurrentSpeed 是主驱动力（实际跑起来才该用 Run 动画）
	//   - WalkSpeed/RunSpeed 是切换阈值（不是目标速度）
	//   - Ctrl/Shift 是意图覆盖（如 Ctrl 强制 Walk，即使速度已超阈值）
	//
	// 注意：ResolvedGait 每帧重新解析，可能频繁变化
	//   AnimInstance 内部有 GaitCooldown 冷却机制防抖，见 GGYGOAnimInstance.cpp
	// ============================================================

	if (InputData.CurrentFrame.bForceWalkHeld)
	{
		// 优先级 1: Ctrl 强制 Walk
		// 无论速度多少，按住 Ctrl 一定是 Walk
		// 场景：精确移动、贴墙走、潜行
		RuntimeData.ResolvedGait = EMovementGait::Walk;
	}
	else if (InputData.CurrentFrame.bSprintHeld)
	{
		// 优先级 2: Shift 强制 Sprint
		// 无论速度多少，按住 Shift 一定是 Sprint
		// 注意：实际速度上限由 MotionDriver 根据 ResolvedGait 决定
		//       所以这里设 Sprint 后，MotionDriver 会放开到 Sprint 阈值（600）
		RuntimeData.ResolvedGait = EMovementGait::Sprint;
	}
	else
	{
		// 优先级 3 & 4: 无按键修饰，根据速度/摇杆幅度自动判定
		float Spd = RuntimeData.CurrentSpeed;       // 当前水平速度（cm/s）
		float StickMag = MoveInput.Size();           // 摇杆幅度（0~1.414）

		if (Spd >= RuntimeData.GaitThresholds.Walk)
		{
			// 优先级 3: 速度已达 Walk 阈值（95 cm/s）→ Run
			// 场景：玩家推满摇杆，速度自然上升到 Run
			// 注意：这里没有判断 Run 阈值（450），因为：
			//   - 摇杆推满 MotionDriver 会给到 Run 速度（450）
			//   - 速度从 0 加速到 450 的过程中，超过 95 就该用 Run 动画
			//   - 否则加速过程中一直播 Walk 动画会很违和
			RuntimeData.ResolvedGait = EMovementGait::Run;
		}
		else
		{
			// 优先级 4: 速度未达 Walk 阈值，用摇杆幅度预测
			//   摇杆幅度 >= 0.5 → 预测 Run（玩家意图跑，速度还没起来）
			//   摇杆幅度 <  0.5 → Walk（轻推摇杆，意图慢走）
			//
			// 为什么需要预测：
			//   刚推摇杆瞬间，速度从 0 开始加速，若等速度达 95 才切 Run，
			//   动画会先播 Walk 再切 Run，有明显的"先走后跑"延迟感
			//   预测让动画立即响应玩家意图，速度随后跟上
			RuntimeData.ResolvedGait = (StickMag >= 0.5f) ? EMovementGait::Run : EMovementGait::Walk;
		}

		// 步态解析日志（前缀 [GAIT]，便于过滤）
		// 仅在无按键修饰分支打印，避免 Ctrl/Shift 时刷屏
		UE_LOG(LogTemp, Log, TEXT("[GAIT] Spd=%.0f Stick=%.2f WalkThr=%.0f → %s"),
			Spd, StickMag, RuntimeData.GaitThresholds.Walk,
			RuntimeData.ResolvedGait == EMovementGait::Run ? TEXT("Run") : TEXT("Walk"));
	}
}
