/**
 * @file GGYGOAnimInstanceBase.h
 * @brief 向 AnimBP 发布只读语义帧的项目通用薄基类
 */
#pragma once

#include "CoreMinimal.h"
#include "Animation/Data/GGYGOLocomotionSourceBinding.h"
#include "Animation/Runtime/GGYGOMontageGuardAnimInstance.h"
#include "Animation/Debug/GGYGOAnimationDebugFrame.h"
#include "Animation/Runtime/GGYGOAnimationRemotePose.h"
#include "Animation/Runtime/GGYGOAnimationStateCapture.h"
#include "Animation/Runtime/GGYGOAnimationStateFrame.h"
#include "Animation/AnimExecutionContext.h"
#include "SequenceEvaluatorLibrary.h"

#include "GGYGOAnimInstanceBase.generated.h"

class ACharacter;
class UGGYGOCharacterMovementComponent;

/** 一次只读查询的姿态与来源值。来源仅可复制、比较；不签发播放权，也不推进时间。 */
USTRUCT(BlueprintType)
struct GGYGO_API FGGYGOAnimationMotionPoseView
{
	GENERATED_BODY()

	/** 即使当前没有所查动作，仍可据宿主上下文变化清理图内表现记忆。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	bool bContextAvailable = false;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	bool bHasPose = false;

	/** 仅本机有效未结束区间或远端 Current；Held 不允许新锁存入场表现。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	bool bCanEnter = false;

	/** 原已进入段的合法等待：可比较原来源，但没有可采样的 Sequence 或时间。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	bool bRetainedWait = false;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	bool bEnded = false;

	/** 仅 bHasPose 时可用；已在原 Sequence 坐标，不得重复乘 RateScale。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	TObjectPtr<UAnimSequence> Sequence = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	float SequenceSeconds = 0.0f;

	/** 与姿态或保留等待出自同一条已认证记录。 */
	UPROPERTY(BlueprintReadOnly, Category = "Animation|Motion Pose")
	EGGYGOTurnBackPhase TurnBackPhase = EGGYGOTurnBackPhase::None;

private:
	friend class UGGYGOAnimInstanceBase;

	// 保留原始身份宽度。反射复制保留完整值，蓝图不拆解或构造来源。
	UPROPERTY(Transient)
	TWeakObjectPtr<const UObject> Instance;
	UPROPERTY(Transient)
	TWeakObjectPtr<const UObject> Provider;
	UPROPERTY(Transient)
	uint64 LifecycleGeneration = 0;
	UPROPERTY(Transient)
	bool bRemote = false;
	UPROPERTY(Transient)
	bool bHasSource = false;
	UPROPERTY(Transient)
	EGGYGOLocomotionMotionType MotionType = EGGYGOLocomotionMotionType::None;
	UPROPERTY(Transient)
	uint64 ExecutionRequestSerial = 0;
	UPROPERTY(Transient)
	uint16 MotionSequence = 0;
	UPROPERTY(Transient)
	uint32 PublisherEpoch = 0;
	UPROPERTY(Transient)
	uint32 SegmentSerial = 0;
};

/**
 * 不知道任何 AnimBP 状态名或过渡拓扑，只负责游戏线程抓取与只读发布。
 */
UCLASS(Abstract, BlueprintType, Blueprintable)
class GGYGO_API UGGYGOAnimInstanceBase : public UGGYGOMontageGuardAnimInstance
{
	GENERATED_BODY()

public:
	virtual void NativeInitializeAnimation() override;
	virtual void NativeUninitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

	/** 线程安全地查询本帧是否具有某个 ASC 状态 Tag。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool HasAnimationStateTag(FGameplayTag Tag) const;

	/** 当前权威步态是否为 Run。只描述语义事实，不知道 AnimBP 的状态拓扑。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool IsAnimationRunGait() const;

	/** 转身是否处于 Turning / Braking 的曲线接管段（明确排除 RunOut）。远端观察端的相位与姿态出自同一条已接受记录。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool IsTurnBackCurveDriven() const;

	/** 同源单Sequence姿态（只读查询，不记录消费）。本机执行角色取已执行完成区间终点；远端观察端取权威复制姿态
	 * （Current 的 SequenceSeconds，或已由真实节点消费进入的同段 Held 认证终点）。
	 * 时间已是原 Sequence 坐标，不得再乘 RateScale。返回false时不得设置播放器时间；不推进节点或动作时钟。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool TryGetMotionPose(EGGYGOLocomotionMotionType MotionType, UAnimSequence*& Sequence,
		float& SequenceSeconds, bool& bEnded) const;

	/** 一次查询本机完成区间或远端认证姿态及其来源。
	 * bHasPose=false 时不得读取缺省时间；bRetainedWait 只保原已进入段，不产生姿态或入场资格。
	 * 无所查片段仍可返回宿主上下文；图先比较上下文，再决定是否保留自有显示角。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	FGGYGOAnimationMotionPoseView QueryMotionPose(EGGYGOLocomotionMotionType MotionType) const;

	/** 只比较本次捕获值中的实例生命周期、原 Provider 和本机/远端角色；两个空上下文不相等。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	static bool AreMotionPoseContextsEqual(const FGGYGOAnimationMotionPoseView& A, const FGGYGOAnimationMotionPoseView& B);

	/** 比较同宿主下的原请求/片段，或原发布端 Epoch/Segment；不把时间、逐帧发布号当来源。
	 * 合法 Held/保留等待保留原键；空键、退休或 Invalid 不相等，比较本身不授予入场。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	static bool AreMotionPoseSourcesEqual(const FGGYGOAnimationMotionPoseView& A, const FGGYGOAnimationMotionPoseView& B);

	/** 由 SequenceEvaluator 的 UpdateFunction 调用：把同源姿态写入该节点（SetSequence/SetExplicitTime）。
	 * 远端 Current 可写入任意实际被更新的节点并记录真实消费；Held 只写入已消费进入该段的同一节点；
	 * Provider 资格保留等待时该节点不写任何值、保持原输出，只记录参与并返回 false。
	 * 本 AnimInstance 之外的上下文、非 SequenceEvaluator 节点或无可用姿态时返回 false 且不写节点。
	 * 只在原生图更新（工作线程）期间写本实例的消费记录，不访问 CMC/Provider/Actor，不推进时间。
	 */
	UFUNCTION(BlueprintCallable, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool ApplyMotionPoseToEvaluator(const FAnimUpdateContext& UpdateContext,
		const FSequenceEvaluatorReference& Evaluator, EGGYGOLocomotionMotionType MotionType);

	/** 已进入段的认证自然末端：本机为已执行完成区间 bEnded；远端为可采样终点，或 Provider 资格保留等待中
	 * 保留的真实关闭事实。只用于关闭判定，不产生姿态、时间或 Finished。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool IsMotionPoseEnded(EGGYGOLocomotionMotionType MotionType) const;

	/** 允许进入该单Sequence运动：本机为已验证且未结束的完成区间；远端仅 Current 且未自然结束。
	 * Held、等待与终点姿态都不授予入场。RunOut相位本身不作为源结束判据。
	 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Animation", meta = (BlueprintThreadSafe))
	bool IsMotionPoseActive(EGGYGOLocomotionMotionType MotionType) const;

	const FGGYGOAnimationStateFrame& GetAnimationStateFrame() const { return AnimationState; }

	/** 本帧远端权威复制姿态视图；bRemote=false 时姿态来自本机完成区间。只读，供原生观察。 */
	const FGGYGOAnimationRemotePoseFrame& GetRemoteMotionPoseFrame() const { return RemoteMotionPose; }

	/** 原生 GT 求值屏障后的只读事实；计数器不是帧号，也不证明某节点参与了求值。
	 * 调用方只观察，不推进代理或动作时间；RootMotion 提取选择来自当前原生代理。
	 */
	void GetNativePoseEvaluationFacts(int16& OutUpdateCounter, int16& OutEvaluationCounter,
		bool& bOutExtractRootMotion) const;

protected:
	/** C++ 权威层发布的一帧只读语义事实。 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "GGYGO|Animation")
	FGGYGOAnimationStateFrame AnimationState;

	/** 与 AnimationState 同次 GT 抓取的远端姿态视图；不写入 MotionExecution 的本机执行事实。 */
	UPROPERTY(Transient)
	FGGYGOAnimationRemotePoseFrame RemoteMotionPose;

	/** 生命周期复位时清理派生 AnimInstance 自己持有的运行时表现状态。 */
	virtual void OnAnimationLifecycleReset();

	/** GT：本帧远端姿态快照的读取点，默认就是原 CMC 的 GetRemoteMotionPoseSnapshot。
	 * 仅供原生测试夹具派生覆盖以注入显式输入，从而在真实动画更新中验证消费合同；
	 * 生产派生不得覆盖。返回值只经 Resolve 消费，不写回 CMC，不建立执行权或时钟。
	 */
	virtual FGGYGORemoteMotionPoseSnapshot CaptureRemoteMotionPoseSnapshot(const UGGYGOCharacterMovementComponent& Movement) const;

	/** Resolve configured assets only. Curve evaluation and all motion clocks belong to Movement. */
	virtual void ResolveLocomotionSourceBinding(FGGYGOLocomotionSourceBinding& OutBinding) const;
	/** Compare current configuration against the last immutable publication without allocating asset holders. */
	virtual bool IsLocomotionSourceConfigurationCurrent(const FGGYGOLocomotionSourceBinding& Binding) const;

	const FGGYGOAnimationDebugFrame& GetAnimationDebugFrame() const { return AnimationDebug; }

private:
	/** 幂等地清理基类运行时状态并通知派生表现层。 */
	void ResetAnimationLifecycleState();
	void RefreshAnimationStateFrame();
	/** 同次 GT 抓取远端姿态；远端时以同记录相位覆盖本帧相位，避免混用 CMC 实时相位。 */
	void CaptureRemoteMotionPose();
	void RefreshLocomotionSourceBinding();
	void RetireLocomotionSourceBinding();

	TWeakObjectPtr<ACharacter> CharacterOwner;
	/** 记录上次绑定过有效 Character，避免弱引用失效后无法识别 nullptr -> nullptr。 */
	bool bHadCharacterOwner = false;
	FGGYGOAnimationStateCapture StateCapture;
	FGGYGOAnimationDebugFrame AnimationDebug;
	/** GT 拥有：已由真实节点消费进入的段。 */
	FGGYGOAnimationRemotePoseContinuity RemotePoseContinuity;
	/** 仅工作线程图更新期间由 ApplyMotionPoseToEvaluator 写入；下一次 GT 捕获读取并清空。 */
	FGGYGOAnimationRemotePoseWitness RemotePoseWitness;
	/** 上一帧视图所属的原 CMC；只作 GT 身份比较。 */
	TWeakObjectPtr<const UObject> RemotePosePreviousProvider;
	/** 发布号永不回绕复用；生命周期复位后旧消费记录也不能匹配新帧。 */
	uint64 RemotePoseTokenCounter = 0;
	/** 上次捕获时的原生图更新计数；只判断两次捕获之间是否发生过图更新，不作为节点参与证据。 */
	int16 RemotePoseLastUpdateCounter = 0;
	bool bHasRemotePoseUpdateCounter = false;
	/** Invalid 诊断按内容去重，同一失败不逐帧刷屏。 */
	FString LastLoggedRemotePoseDiagnostic;
	/** 工作线程消费入口的配置错误只诊断一次（仅该线程读写）。 */
	bool bLoggedInvalidPoseConsumer = false;
	/** Only the publication/resource handle is owned here; no movement state is mirrored. */
	FGGYGOLocomotionSourceBindingPtr PublishedLocomotionSource;
	TWeakObjectPtr<UGGYGOCharacterMovementComponent> LocomotionSourceConsumer;
	uint64 AnimationLifecycleGeneration = 0;
	/** Never reset across reinitialization: an old retirement cannot match a later publication. */
	uint64 LocomotionSourceConfigurationGeneration = 0;
	bool bSourceGenerationExhausted = false;
};
