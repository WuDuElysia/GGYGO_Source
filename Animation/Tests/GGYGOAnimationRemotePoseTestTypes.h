/**
 * @file GGYGOAnimationRemotePoseTestTypes.h
 * @brief 远端姿态消费在真实动画更新中的原生测试夹具类型；不进入生产角色或 AnimBP。
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Runtime/GGYGOAnimInstanceBase.h"
#include "GameFramework/Character.h"

#include "GGYGOAnimationRemotePoseTestTypes.generated.h"

/** 夹具角色：使用项目 CMC，使 AnimInstanceBase 的原 GT 捕获路径找到 Provider 身份。 */
UCLASS(Transient, NotBlueprintable)
class AGGYGOAnimationRemotePoseFixtureCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AGGYGOAnimationRemotePoseFixtureCharacter(const FObjectInitializer& ObjectInitializer);
};

/**
 * 夹具 AnimInstance：仅把快照读取点换成测试显式注入的值（未注入时为默认 Initial，即无事实），
 * 其余捕获、消费记录、进入证据与节点写入全部走生产基类原路径；不读取未运行的 CMC 复制状态。
 * 只作 Saved 验证宿主在 /Temp 建立的夹具 AnimBlueprint 的父类。
 * 注入值是测试输入，不代表 CMC 已执行区间、网络运输或原生呈现样本。
 */
UCLASS(Transient, Blueprintable)
class UGGYGOAnimationRemotePoseFixtureAnimInstance : public UGGYGOAnimInstanceBase
{
	GENERATED_BODY()

public:
	/** GT：下一次 NativeUpdate 捕获使用的快照；持续生效直到再次设置。 */
	void SetFixtureSnapshot(const FGGYGORemoteMotionPoseSnapshot& Snapshot) { FixtureSnapshot = Snapshot; }

protected:
	virtual FGGYGORemoteMotionPoseSnapshot CaptureRemoteMotionPoseSnapshot(
		const UGGYGOCharacterMovementComponent& Movement) const override
	{
		return FixtureSnapshot;
	}

private:
	FGGYGORemoteMotionPoseSnapshot FixtureSnapshot;
};
