/**
 * @file RootMotionParameterProcessor.cpp
 * @brief 根运动数据提取处理器实现
 *
 * @NTEAnim: 连接点C - 本处理器读取 NTEAnim 的 RootMotionDelta 输出
 * 从 NTEAnimInstance 读取当前帧的根骨骼位移（RootMotionDelta）。
 * NTEAnimInstance 在 NativeUpdateAnimation 中从 AnimSequence 内置的
 * 根骨骼轨道提取每帧位移，本处理器直接读取该值写入 RuntimeData。
 *
 * 对比旧方案（Speed 曲线）：
 *   旧：FBX → Python 脚本 → AnimSequence.Speed 曲线 → GetCurveValue("Speed")
 *   新：AnimSequence 内置 RM → NTEAnimInstance 提取 → 读 RootMotionDelta
 *
 * 新方案优势：
 *   - 不需要 Python 预处理脚本生成曲线
 *   - 不需要每个动画手动配置 Speed 曲线
 *   - 位移数据与动画 100% 一致（来自同一数据源）
 */
#include "Pipeline/Parameters/RootMotionParameterProcessor.h"
#include "Data/RuntimeData.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/NTEAnimInstance.h"

void FRootMotionParameterProcessor::Init(USkeletalMeshComponent* InMesh)
{
	Mesh = InMesh;
}

void FRootMotionParameterProcessor::Process(FRuntimeData& RuntimeData, float DeltaTime)
{
	// Phase 9: 根运动提取已移除，移动由 MotionDriver 直接驱动
	RuntimeData.bBip001Found   = false;
	RuntimeData.AnimSpeed      = 0.f;
	RuntimeData.RootMotionDelta = FVector::ZeroVector;
	RuntimeData.bHasRootMotion  = false;
}
