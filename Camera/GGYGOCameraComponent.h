/**
 * @file GGYGOCameraComponent.h
 * @brief 由相机模式栈驱动的相机组件
 *
 * 取代直接用 `USpringArmComponent` + `UCameraComponent` 的常见做法。
 * 差别在于视角来源：这里的视角由 `UGGYGOCameraModeStack` 求值，
 * 因此能力可以临时推入模式并自动获得平滑过渡，而不必去改弹簧臂的参数
 * 再想办法恢复。
 *
 * ## 默认模式的来源
 * 栈底必须始终有一个模式，否则没有视角可算。默认模式通过
 * `DetermineCameraModeDelegate` 由外部提供 —— 通常是 HeroComponent，
 * 它知道当前角色的 PawnData 里配了哪个模式。
 *
 * 做成委托而不是组件上的一个字段，是因为默认模式会随状态变化：
 * 载具、潜行、锁定各有默认视角，而这些状态归各自的系统管，相机组件不该知道。
 */
#pragma once

#include "Camera/CameraComponent.h"
// FGGYGOCameraOffset 是值成员，需要完整定义而非前向声明。
#include "Camera/GGYGOCameraMode.h"

#include "GGYGOCameraComponent.generated.h"

class AActor;
class UGGYGOCameraComponent;
struct FGGYGOPreparedCameraViewState;
class UGGYGOCameraMode;
class UGGYGOCameraModeStack;
class UObject;

/** 由外部决定当前该用哪个相机模式；未绑定或返回无效模式时明确拒绝求值。 */
DECLARE_DELEGATE_RetVal(TSubclassOf<UGGYGOCameraMode>, FGGYGODetermineCameraModeSignature);

/**
 * One native-call candidate. Construct, preview and consume within the same synchronous call.
 * Move-only; no cross-frame/member storage. Native CameraCache remains the published authority.
 */
struct GGYGO_API FGGYGOPreparedCameraView
{
	FGGYGOPreparedCameraView();
	~FGGYGOPreparedCameraView();
	FGGYGOPreparedCameraView(FGGYGOPreparedCameraView&& Other) noexcept;
	FGGYGOPreparedCameraView& operator=(FGGYGOPreparedCameraView&& Other) noexcept;
	FGGYGOPreparedCameraView(const FGGYGOPreparedCameraView&) = delete;
	FGGYGOPreparedCameraView& operator=(const FGGYGOPreparedCameraView&) = delete;
	FGGYGOCameraEvaluationResult TryGetCandidateView(FMinimalViewInfo& InOutCandidateView) const;

private:
	friend class UGGYGOCameraComponent;
	TUniquePtr<FGGYGOPreparedCameraViewState> State;
};

UCLASS(meta = (BlueprintSpawnableComponent))
class GGYGO_API UGGYGOCameraComponent : public UCameraComponent
{
	GENERATED_BODY()

public:
	UGGYGOCameraComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** 取某个 Actor 上的本组件。 */
	UFUNCTION(BlueprintPure, Category = "GGYGO|Camera")
	static UGGYGOCameraComponent* FindCameraComponent(const AActor* Actor)
	{
		return Actor ? Actor->FindComponentByClass<UGGYGOCameraComponent>() : nullptr;
	}

	/** 视角跟随的目标。默认是组件拥有者。 */
	virtual AActor* GetTargetActor() const { return GetOwner(); }

	/** 由外部提供已仲裁的当前有效相机模式。 */
	FGGYGODetermineCameraModeSignature DetermineCameraModeDelegate;

	/** Evaluate once without publishing controller/component/output fields. */
	FGGYGOCameraEvaluationResult PrepareCameraView(float DeltaTime, FGGYGOPreparedCameraView& OutPrepared);

	/** Consume once, validate the original context, then publish without reevaluation. */
	FGGYGOCameraEvaluationResult CommitPreparedCameraView(
		FGGYGOPreparedCameraView&& Prepared, FMinimalViewInfo& DesiredView);

	/** Explicit fresh stack/pool rebuild; preserves arbiter and Offset ownership, publishes no view. */
	FGGYGOCameraEvaluationResult ReinitializeCameraEvaluation();

	/**
	 * 施加一份镜头微调，叠加在模式栈的求值结果上。
	 *
	 * 所有数值须有限，BlendIn/BlendOut 时间须非负；合法零时间和全零 Offset 可接受。
	 * 非法输入或 token 耗尽时诊断并返回无效 handle，不改变入口时的单槽、alpha 或发号计数器。
	 * 成功时保留当前 alpha。同一时刻只保留一份 —— 合法再次调用直接替换。多份叠加需要引用计数与撤销顺序，
	 * 而实际需求（能力期间的镜头调整）是互斥的：同时激活两个都要改镜头的能力时，
	 * 后者的镜头应当覆盖前者，而不是两份相加。
	 */
	FGGYGOCameraOffsetHandle SetCameraOffset(const FGGYGOCameraOffset& InOffset);

	/** 仅当前所有权凭证可撤销镜头微调；成功后按 BlendOutTime 回落。 */
	bool ClearCameraOffset(FGGYGOCameraOffsetHandle Handle);

	/** 清空模式栈。切换 Avatar 时调用。 */
	void ClearCameraModeStack();

	/** 立即清除模式、镜头微调和穿墙恢复状态；token 序号保持单调。 */
	void ResetCameraRuntimeState();

protected:
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	/** 引擎每帧取视角的入口。在这里求值模式栈。 */
	virtual void GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView) override;

	/** 把外部仲裁出的当前有效模式推到栈顶。 */
	virtual FGGYGOCameraEvaluationResult UpdateCameraModes();

	/** 相机模式栈。 */
	UPROPERTY()
	TObjectPtr<UGGYGOCameraModeStack> CameraModeStack;

	/** 把当前的镜头微调按 `CameraOffsetAlpha` 叠加到求值结果上。 */
	FGGYGOCameraEvaluationResult ApplyCameraOffset(FGGYGOCameraModeView& View) const;

	/** 推进镜头微调的进入/回落插值。 */
	FGGYGOCameraEvaluationResult UpdateCameraOffsetAlpha(float DeltaTime);

	/** 对 Offset 后的最终位置执行唯一穿透查询并应用遮挡/恢复。 */
	FGGYGOCameraEvaluationResult ResolveCameraPenetration(
		float DeltaTime,
		const FGGYGOCameraPenetrationRequest& Request,
		FGGYGOCameraModeView& View);

	/** 当前生效的镜头微调。 */
	FGGYGOCameraOffset CameraOffset;

	/**
	 * 微调的当前强度，[0, 1]。
	 *
	 * 与模式的混合权重分开：模式权重由栈管理、代表"哪个模式说话"，
	 * 而这个 alpha 代表"微调施加了多少"，两者可以同时变化
	 * （能力刚开始时模式还在混合，微调也还在进入）。
	 */
	float CameraOffsetAlpha = 0.0f;

	/** 微调是否处于施加状态。false 时 alpha 往 0 回落。 */
	bool bCameraOffsetActive = false;

private:
	friend struct FGGYGOPreparedCameraView;
	FGGYGOCameraEvaluationResult PrepareCameraViewInternal(
		float DeltaTime, FGGYGOPreparedCameraView& OutPrepared, FString& OutSelectedClassPath);
	FGGYGOCameraEvaluationResult ValidatePreparedCameraContext(
		const FGGYGOPreparedCameraViewState& State, bool bCheckPublishedInputs = true) const;
	void InvalidatePreparedCameraViews();
	void FinishCameraEvaluation();

	/** Candidate invalidation only; no simulation clock, stop policy or view cache. */
	uint64 CameraEvaluationRevision = 0;
	bool bCameraEvaluationBusy = false;
	bool bDeferredCameraModeStackClear = false;

	/** 仅抑制连续相同的诊断，不参与求值准入、停更或视图保存。 */
	void ReportCameraEvaluationFailure(
		const FGGYGOCameraEvaluationResult& Result,
		const AActor* OriginalTarget,
		const FString& SelectedClassPath);
	FString LastCameraEvaluationFailure;

	/** 最近发放的序号；生命周期重置不会清零。0 保留给无效 handle。 */
	uint64 LastIssuedCameraOffsetHandle = 0;

	/** 当前单槽微调的有效所有权凭证。 */
	FGGYGOCameraOffsetHandle ActiveCameraOffsetHandle;

	/** 当前最终相机位置沿枢轴至期望位置的安全比例。 */
	float CurrentPenetrationRecoveryRatio = 1.0f;
};
