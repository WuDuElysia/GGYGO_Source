/**
 * @file GGYGOAnimCurveSampler.h
 * @brief 从动画曲线提取每帧运动量
 *
 * ZZZ 的移动动画是 in-place 的（骨骼原地踏步，不带 root motion），
 * 真实位移信息被烘焙成一组 `RM_*` 浮点曲线。本采样器把这些曲线读出来，
 * 差分成"这一帧应该移动多快、朝哪个方向、转多少度"。
 *
 * ## 为什么用曲线而不是 root motion
 * 曲线可以被缩放、可以只取速度而自己决定方向、可以在运行时被程序覆盖，
 * 这些是 in-place 动画配合曲线才能做到的。引擎的 root motion 则把位移
 * 硬编码在动画里，攻击的位移终点无法由代码解算。
 *
 * 代价是曲线值属于本地动画状态：它不复制、也不参与 CMC 的移动预测。
 * 处理办法见 `UGGYGOCharacterMovementComponent` 对采样时机与 SavedMove 的说明。
 *
 * ## 曲线的坐标系
 * `RM_PosX` / `RM_PosY` 与 `RM_VelocityDirX` / `RM_VelocityDirY` 用的是
 * **动画侧的分量约定：X 是左右，Y 是前后**，与 UE 局部空间（X 前、Y 右）相反。
 * 本采样器**不做转换**，原样输出，由消费方在需要世界方向时转换 ——
 * 在这里转会让"曲线原始值"这个概念在调试时对不上动画编辑器里看到的数值。
 *
 * ## 曲线含义
 * | 曲线 | 含义 | 用法 |
 * |---|---|---|
 * | `RM_Speed`         | 该帧速度（cm/s） | 直接作为速度，是消除脚滑的主值 |
 * | `RM_PosX` / `PosY` | 从动画段起点累计的位移 | 相邻帧差分得到本帧位移 |
 * | `RM_Dist`          | 从动画段起点累计的路程 | 单调递增，回退即表示换了动画段 |
 * | `RM_Yaw`           | 从动画段起点累计的转角（度） | 相邻帧差分得到本帧转角 |
 * | `RM_VelocityDirX/Y`| 烘焙好的速度方向 | 优先于位移差分，因为位移差分在低速时噪声大 |
 */
#pragma once

#include "CoreMinimal.h"

class UAnimInstance;

/** 一帧的曲线运动量。全部处于动画侧分量系（X 左右、Y 前后）。 */
struct FGGYGOAnimCurveMotion
{
	/** 本帧速度（cm/s）。非负。 */
	float Speed = 0.0f;

	/** 本帧转角增量（度）。可正可负。 */
	float YawDeltaDegrees = 0.0f;

	/** 本帧位移量。 */
	FVector PositionDelta = FVector::ZeroVector;

	/** 位移量除以 DeltaTime 得到的速度向量。 */
	FVector Velocity = FVector::ZeroVector;

	/** 归一化的速度方向。 */
	FVector Direction = FVector::ZeroVector;

	/** 速度方向角（度）。 */
	float DirectionAngle = 0.0f;

	/** 方向来自烘焙曲线（而非位移差分）。烘焙值更稳，低速时尤其明显。 */
	bool bHasAuthoredDirection = false;

	/** 本帧有非零位移。 */
	bool bHasPositionDelta = false;

	/**
	 * 当前动画确实带曲线数据。
	 *
	 * 为 false 表示当前播放的动画没有烘焙 `RM_*` 曲线，此时不该用曲线速度 ——
	 * 与"曲线存在但这一帧速度恰好是 0"（例如起步的第一帧）是两种不同情况，
	 * 后者应该让角色停住，前者应该回退到配置的固定速度。
	 */
	bool bHasCurveSource = false;

	/** 全部归零。 */
	void Reset();

	/** 是否有可用于驱动移动的速度。 */
	bool HasUsableSpeed() const;
};

/**
 * 曲线采样器。
 *
 * 持有跨帧基线（上一帧的累计值），因此**必须每帧恰好调用一次** `Sample`。
 * 一帧内多次调用会让第二次的差分结果为零；漏帧则会让位移被累计到下一帧，
 * 表现为角色顿一下再窜一段。
 */
class FGGYGOAnimCurveSampler
{
public:
	/**
	 * 丢弃基线。
	 *
	 * 下一次采样只重建基线、不输出增量。切换角色、Mesh 失效、
	 * 或任何"接下来的曲线值与之前不连续"的情况都要调用它。
	 */
	void ResetBaseline();

	/**
	 * 采样一帧。
	 *
	 * @param AnimInstance 曲线的来源。
	 * @param DeltaTime    本帧时长。非有限或非正时只更新基线，输出保持归零。
	 * @param OutMotion    输出。函数开头整体归零，不保留上一帧残留。
	 */
	void Sample(const UAnimInstance& AnimInstance, float DeltaTime, FGGYGOAnimCurveMotion& OutMotion);

private:
	/** 上一帧的累计位移。 */
	float PreviousPosX = 0.0f;
	float PreviousPosY = 0.0f;

	/** 上一帧的累计路程。用于检测动画段切换。 */
	float PreviousDistance = 0.0f;

	/** 上一帧的累计转角（度）。 */
	float PreviousYaw = 0.0f;

	/** 基线是否有效。false 时下一次采样只建基线。 */
	bool bHasBaseline = false;
};
