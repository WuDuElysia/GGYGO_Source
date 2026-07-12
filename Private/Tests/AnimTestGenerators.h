/**
 * @file AnimTestGenerators.h
 * @brief 动画决策层属性测试的轻量随机生成器工具
 *
 * 基于 FRandomStream 提供确定性（种子可复现）随机值生成器，供 UE Automation
 * 属性测试复用。覆盖动画决策层的输入空间：随机角度、随机速度（含负值与极大值）、
 * 随机相位、随机步态、随机支撑脚，以及随机填充的完整 FAnimSnapshot。
 *
 * 生成器刻意覆盖边界与极端值（如负速度、极大速度、相位接近 1），
 * 以便属性测试暴露钳制、映射与分区逻辑的缺陷。
 */
#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Animation/GGYGOAnimInstance.h"      // FAnimSnapshot、FAnimSourceData
#include "StateMachine/CharacterStateType.h"  // EMovementGait
#include "Animation/LocomotionConfig.h"        // EAnimFoot

#if WITH_AUTOMATION_TESTS

/**
 * 动画测试随机生成器
 *
 * 纯静态工具类（F 前缀），所有方法接收 FRandomStream 引用以保持确定性。
 * 不持有状态、不接触引擎运行时，可在离线属性测试中自由复用。
 */
struct FAnimTestGen
{
	/**
	 * 生成随机移动角度
	 * @param InRng 随机流
	 * @return 角度（度），范围 [-180, 180]
	 */
	static float RandAngleDeg(FRandomStream& InRng)
	{
		return InRng.FRandRange(-180.f, 180.f);
	}

	/**
	 * 生成随机速度（含负值与极大值）
	 *
	 * 常规区间覆盖 [-200, 2000]（含负速度以检验钳制），
	 * 并以约 1/8 概率返回极端值（极大正速度或极大负速度），
	 * 用于压测映射与钳制在越界输入下的行为。
	 * @param InRng 随机流
	 * @return 速度（cm/s）
	 */
	static float RandSpeed(FRandomStream& InRng)
	{
		// 小概率抛出极端值，其余落在常规含负区间。
		const int32 Bucket = InRng.RandRange(0, 7);
		if (Bucket == 0)
		{
			return InRng.FRandRange(50000.f, 1000000.f);   // 极大正速度
		}
		if (Bucket == 1)
		{
			return InRng.FRandRange(-1000000.f, -50000.f); // 极大负速度
		}
		return InRng.FRandRange(-200.f, 2000.f);           // 常规含负区间
	}

	/**
	 * 生成随机归一化相位
	 * @param InRng 随机流
	 * @return 相位，范围 [0, 1)
	 */
	static float RandPhase(FRandomStream& InRng)
	{
		// FRand() 返回 [0,1)，直接满足相位定义域。
		return InRng.FRand();
	}

	/**
	 * 生成随机步态
	 * @param InRng 随机流
	 * @return None / Walk / Run / Sprint 之一
	 */
	static EMovementGait RandGait(FRandomStream& InRng)
	{
		return static_cast<EMovementGait>(InRng.RandRange(0, 3));
	}

	/**
	 * 生成随机支撑脚
	 * @param InRng 随机流
	 * @return Left / Right 之一
	 */
	static EAnimFoot RandFoot(FRandomStream& InRng)
	{
		return static_cast<EAnimFoot>(InRng.RandRange(0, 1));
	}

	/**
	 * 生成随机布尔值
	 * @param InRng 随机流
	 * @return true 或 false（各约 1/2 概率）
	 */
	static bool RandBool(FRandomStream& InRng)
	{
		return InRng.RandRange(0, 1) == 1;
	}

	/**
	 * 生成随机填充的完整快照
	 *
	 * 各字段用上述生成器填充，覆盖决策函数与输出计算的输入空间。
	 * @param InRng 随机流
	 * @return 随机快照
	 */
	static FAnimSnapshot RandSnapshot(FRandomStream& InRng)
	{
		FAnimSnapshot Out;
		Out.bWantMove = RandBool(InRng);
		Out.bGrounded = RandBool(InRng);
		Out.bJustLanded = RandBool(InRng);
		Out.bWantJump = RandBool(InRng);
		Out.bWantGlide = RandBool(InRng);
		Out.bInWater = RandBool(InRng);
		Out.Speed = RandSpeed(InRng);
		Out.VerticalVelocity = RandSpeed(InRng);
		Out.LandImpactSpeed = FMath::Abs(RandSpeed(InRng));
		Out.MoveAngleDeg = RandAngleDeg(InRng);
		Out.LocomotionPhase = RandPhase(InRng);
		Out.DesiredGait = RandGait(InRng);
		Out.CurrentFoot = RandFoot(InRng);
		return Out;
	}

	/**
	 * 生成随机填充的快照来源数据
	 *
	 * 用于 Property 1（快照映射恒等）测试：镜像 RandSnapshot 的字段填充，
	 * 供 BuildSnapshot 消费后逐字段比对。
	 * @param InRng 随机流
	 * @return 随机来源数据
	 */
	static FAnimSourceData RandSourceData(FRandomStream& InRng)
	{
		FAnimSourceData Out;
		Out.bWantMove = RandBool(InRng);
		Out.bGrounded = RandBool(InRng);
		Out.bJustLanded = RandBool(InRng);
		Out.bWantJump = RandBool(InRng);
		Out.bWantGlide = RandBool(InRng);
		Out.bInWater = RandBool(InRng);
		Out.Speed = RandSpeed(InRng);
		Out.VerticalVelocity = RandSpeed(InRng);
		Out.LandImpactSpeed = FMath::Abs(RandSpeed(InRng));
		Out.MoveAngleDeg = RandAngleDeg(InRng);
		Out.LocomotionPhase = RandPhase(InRng);
		Out.DesiredGait = RandGait(InRng);
		Out.CurrentFoot = RandFoot(InRng);
		return Out;
	}
};

#endif // WITH_AUTOMATION_TESTS
