/**
 * @file GGYGOPlayerInput.h
 * @brief Native 物理输入事实、原始移动输入会话及同步事件发布。
 *
 * Source 持有硬件观察与会话身份；CMC 持有位移执行和准入，ASC 持有能力输入状态。
 */
#pragma once

#include "EnhancedPlayerInput.h"
#include "Engine/EngineTypes.h"
#include "Input/GGYGOMovementInputTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/Optional.h"

#include "GGYGOPlayerInput.generated.h"

class APawn;
class AController;
class AActor;
class UInputAction;
class UInputComponent;
class UEnhancedInputLocalPlayerSubsystem;
class UGGYGOMovementInputOriginResource;

/** One immutable session per native delegate subscription, including callbacks already in flight. */
UCLASS(Transient)
class UGGYGOMovementInputRouteObserver : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(UEnhancedInputLocalPlayerSubsystem* Subsystem, APawn* Pawn,
		const FGGYGOMovementInputSessionIdentity& Session);
	void Detach();

private:
	UFUNCTION()
	void OnMappingsRebuilt();
	UFUNCTION()
	void OnControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);
	UFUNCTION()
	void OnPawnEndPlay(AActor* Actor, EEndPlayReason::Type EndPlayReason);

	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> OriginalSubsystem;
	TWeakObjectPtr<APawn> OriginalPawn;
	FGGYGOMovementInputSessionIdentity OriginalSession;
};

/** Native input provenance only; movement execution and failure admission remain in CMC. */
UCLASS(Config = Input, Transient)
class GGYGO_API UGGYGOPlayerInput : public UEnhancedPlayerInput
{
	GENERATED_BODY()

public:
	bool BeginMovementInputSession(APawn* Pawn, UInputComponent* Component, const UInputAction* Action,
		FGGYGOMovementInputSessionIdentity& OutSession, FString& OutError);
	bool AttachMovementInputReceiver(const FGGYGOMovementInputSessionIdentity& Session,
		const FGGYGOMovementInputConsumerBindingId& Binding,
		FGGYGOMovementInputFactDelegate Receiver, FString& OutError);
	void EndMovementInputSession(const FGGYGOMovementInputSessionIdentity& Session, FName Reason);
	/** Queries only this original session. Non-Held results clear OutRequest; NotHeld clears OutError.
	 *  AwaitingPhysicalProof does not prove release or authorize a request. No callbacks or state writes. */
	EGGYGOMovementInputRequestQueryResult QueryMovementInputRequest(
		const FGGYGOMovementInputSessionIdentity& OriginalSession,
		FGGYGOMovementInputRequestIdentity& OutRequest, FString& OutError) const;
	/**
	 * 读取原请求在本帧自然评价完成后的 Axis2D；投影及段首快照由 CMC 消费。
	 * @param OriginalRequest 原 Source 已发行的非零请求，含原 Session，不能借用后继请求。
	 * @param OutActionValue 仅 Held 时有效；真实零轴仍为 Held，不发布中立或释放事实。
	 * @param OutError Held／NotHeld 清空诊断；其它结果说明失效或尚未就绪的原因。
	 * @return NotHeld 保留真实释放语义；AwaitingPhysicalProof 也表示本轮 Action 尚未可读。
	 * 其它结果清空输出并给出原因；查询不写状态、不派发回调，也不读取旧帧方向。
	 */
	EGGYGOMovementInputRequestQueryResult QueryMovementInputActionValue(
		const FGGYGOMovementInputRequestIdentity& OriginalRequest,
		FVector2D& OutActionValue, FString& OutError) const;
	/** 仅在原自然评价回调中捕获在途身份；未完成的 Token 不授输入许可。 */
	bool CaptureMovementInputEvaluationToken(const FGGYGOMovementInputSessionIdentity& OriginalSession,
		FGGYGOMovementInputEvaluationToken& OutToken, FString& OutError);
	/** 查询该原 edge 的完成事实；后继评价不覆盖它，原 Session/路由退役则失效。 */
	static EGGYGOMovementInputEvaluationState QueryMovementInputEvaluationToken(
		const FGGYGOMovementInputEvaluationToken& OriginalToken, FString& OutError);
	/** 只核当前评价与原来源可用，不读取 Axis/Yaw；Ready 外调后仍须在 CMC Resolve 真正取值。 */
	static EGGYGOMovementInputEvaluationState QueryStepStartInputReadiness(
		const FGGYGOMovementInputEvaluationToken& OriginalEdgeToken, FString& OutError);
	/** 真正段首准入点查询当帧完整 Axis2D 与原 PC 此刻 Yaw，不读取 edge 的历史方向。
	 * 仅 Completed 交付事实；Waiting/Invalidated 清输出，原 edge 身份保持不变。 */
	static EGGYGOMovementInputEvaluationState QueryStepStartInputFact(
		const FGGYGOMovementInputEvaluationToken& OriginalEdgeToken,
		FGGYGOMovementInputStepFact& OutFact, FString& OutError);
	/** 仅本地首次提交前核已取事实仍 fresh；不替换事实，已封存请求不再用当前输入重验。 */
	static bool ValidateStepStartInputFact(const FGGYGOMovementInputEvaluationToken& OriginalEdgeToken,
		const FGGYGOMovementInputStepFact& Fact, FString& OutError);
	/** 订阅原 Session 的自然完成/退役；先交 OutHandle 再允许同步回放，回调须重新核原请求。
	 * 消费者在成功、取消或失效时用同一 Token 退订，不以该事件激活能力或另设帧消费。 */
	static bool SubscribeMovementInputEvaluation(const FGGYGOMovementInputEvaluationToken& OriginalToken,
		FGGYGOMovementInputEvaluationDelegate Observer, FDelegateHandle& OutHandle, FString& OutError);
	static void UnsubscribeMovementInputEvaluation(const FGGYGOMovementInputEvaluationToken& OriginalToken,
		FDelegateHandle& Handle);

	/** Records this construction only inside the original resource's explicit native scope. */
	virtual void PostInitProperties() override;
	virtual bool InputKey(const FInputKeyEventArgs& Params) override;
	virtual void FlushPressedKeys() override;
	virtual void BeginDestroy() override;

protected:
	virtual void EvaluateInputDelegates(const TArray<UInputComponent*>& InputComponentStack,
		float DeltaTime, bool bGamePaused, const TArray<TPair<FKey, FKeyState*>>& KeysWithEvents) override;
	virtual void EvaluateBlockedInputComponent(UInputComponent* Component) override;

private:
	static EGGYGOMovementInputEvaluationState ValidateMovementInputEvaluationToken(
		const FGGYGOMovementInputEvaluationToken& OriginalToken,
		UGGYGOPlayerInput*& OutProducer, FString& OutError);
	static EGGYGOMovementInputEvaluationState ValidateStepStartInputReadiness(
		const FGGYGOMovementInputEvaluationToken& OriginalEdgeToken, UGGYGOPlayerInput*& OutProducer,
		EGGYGOMovementInputRequestQueryResult& OutInputState, FString& OutError);
	void InvalidatePendingMovementInputEvaluation(const TCHAR* Reason);
	bool HasCurrentCompletedMovementInputEvaluation(FString& OutError) const;
	EGGYGOMovementInputRequestQueryResult ReadEvaluatedMovementInputActionValue(
		FVector2D& OutActionValue, FString& OutError) const;

	enum class ESourceProof : uint8 { Unknown, Neutral, Held };
	/** Actual native observations only; movement teardown never changes this record. */
	struct FPhysicalSource
	{
		FKey Key;
		FInputDeviceId Device;
		FVector RawValue = FVector::ZeroVector;
		uint8 ObservedAxes = 0;
		bool bDigitalDown = false;
		bool bUnproven = false;
		FName UnprovenReason = NAME_None;
		uint64 NativeProofGapRevision = 0;
		uint64 LastRealReleaseRevision = 0;
		// All required axis samples must follow a movement barrier, not only the final sample.
		uint64 LastRealReleaseProofStartRevision = 0;
		uint64 AxisObservationRevisions[3] = {};

		bool HasNativeReleaseGap() const
		{
			return NativeProofGapRevision != 0 && LastRealReleaseRevision <= NativeProofGapRevision;
		}
	};
	/** Derived movement membership/recovery only; never copies raw values or held. */
	struct FMovementSourceObligation
	{
		FKey Key;
		FInputDeviceId Device;
		bool bParticipant = false;
		TOptional<uint64> ReleaseAfterObservationRevision;
		FName ReleaseReason = NAME_None;
	};
	struct FNativePhysicalSourceObservation
	{
		FKey Key;
		FInputDeviceId Device;
		bool bRealHeldEdge = false;
		bool bRealDigitalPress = false;
		bool bRealRelease = false;
	};
	struct FNativePhysicalObservation
	{
		uint64 Revision = 0;
		TArray<FNativePhysicalSourceObservation, TInlineAllocator<2>> Sources;
	};
	struct FPhysicalObservation
	{
		uint64 Revision = 0;
		ESourceProof Before = ESourceProof::Unknown;
		ESourceProof After = ESourceProof::Unknown;
		bool bRealHeldEdge = false;
		bool bHadParticipantsBefore = false;
		bool bRealDigitalPress = false;
	};
	struct FRouteMapping
	{
		FKey Key;
		TArray<TWeakObjectPtr<UObject>> Objects;
		int32 MappingModifierCount = 0;
		int32 MappingTriggerCount = 0;
		int32 ActionModifierCount = 0;
		int32 ActionTriggerCount = 0;
		bool Matches(const FRouteMapping& Other) const;
	};
	struct FPendingFact
	{
		FGGYGOMovementInputConsumerBindingId Binding;
		FGGYGOMovementInputFact Fact;
		FGGYGOMovementInputFactDelegate Receiver;
	};

	bool ReadRoute(const UInputAction* Action, TArray<FRouteMapping>& OutMappings, FString& OutError) const;
	bool IsCurrentSession(const FGGYGOMovementInputSessionIdentity& Session) const;
	bool ValidateCurrentRoute(FString& OutError) const;
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> SnapshotOriginForCleanup() const;
	bool CaptureOriginalOrigin(FString& OutError);
	bool ValidateOriginalOrigin(FString& OutError) const;
	void RetireOriginalQualification(FName Reason, bool bEstablishBarrier = false);
	bool HasParticipatingMovementSources() const;
	/** Commits the original physical request and publishes its prepared Started fact synchronously.
	 *  @pre UpdateRequestFromSource validated this route in the same call, with no intervening callout.
	 *  Successful qualification consumption must remain free of external callbacks.
	 *  @return Whether the original session is still live after fact delivery. */
	bool CommitPhysicalRequest(EGGYGOMovementInputStartProof Proof,
		const FGGYGOMovementInputSessionIdentity& ExpectedSession);
	ESourceProof ReadSourceProof() const;
	FString DescribeSourceProof() const;
	bool IsMappedMovementSource(const FKey& Key) const;
	void AdoptKnownMovementSources();
	void EstablishMovementReleaseBarrier(FName Reason);
	const FPhysicalSource* FindPhysicalSource(const FKey& Key, FInputDeviceId Device) const;
	const FMovementSourceObligation* FindMovementSourceObligation(const FPhysicalSource& Source) const;
	FMovementSourceObligation& FindOrAddMovementSourceObligation(const FPhysicalSource& Source);
	bool IsMovementReleaseRequired(const FPhysicalSource& Source) const;
	void InvalidatePhysicalInputObservations(FName Reason);
	FNativePhysicalObservation ObservePhysicalInput(const FInputKeyEventArgs& Params);
	FPhysicalObservation ProjectMovementObservation(const FNativePhysicalObservation& Native,
		ESourceProof Before, bool bHadParticipantsBefore);
	void UpdateRequestFromSource(const FPhysicalObservation& Observation,
		const FGGYGOMovementInputSessionIdentity& ExpectedSession);
	bool Publish(EGGYGOMovementInputFactKind Kind, uint64 RequestSerial, FName Reason,
		const FGGYGOMovementInputSessionIdentity& ExpectedSession);
	void DeliverPendingFacts();
	bool AllocateSerial(uint64& Counter, uint64& OutSerial);
	FString MakeError(const FString& Reason) const;
	void ReportOnce(FName Reason, const FString& Detail);

	// Native device observations survive session teardown/flush; synthetic releases never clear them.
	TArray<FPhysicalSource> PhysicalSources;
	TArray<FMovementSourceObligation> MovementSourceObligations;
	TArray<FRouteMapping> RouteMappings;
	TWeakObjectPtr<APawn> SessionPawn;
	TWeakObjectPtr<UInputComponent> SessionComponent;
	TWeakObjectPtr<const UInputAction> SessionAction;
	TWeakObjectPtr<UEnhancedInputLocalPlayerSubsystem> SessionSubsystem;
	UPROPERTY(Transient)
	TObjectPtr<UGGYGOMovementInputRouteObserver> RouteObserver;
	FGGYGOMovementInputSessionIdentity CurrentSession;
	/** 既有 Evaluate 入口预留、自然返回才完成的处理凭据；不保存方向或键态。
	 * 仅原 Session、同帧及同物理观察有效；InputKey/后继评价撤销当前凭据，不改已完成 edge Proof。 */
	struct FMovementActionEvaluation
	{
		FGGYGOMovementInputSessionIdentity Session;
		uint64 FrameNumber = 0;
		uint64 ObservationRevision = 0;
		uint64 EvaluationSerial = 0;
		bool bCompleted = false;
	};
	TOptional<FMovementActionEvaluation> MovementActionEvaluation;
	// 序号在原评价入口预留；仅有 edge 捕获时才分配私有 Proof，不保存评价历史。
	uint64 LastMovementInputEvaluationSerial = 0;
	TSharedPtr<FGGYGOMovementInputEvaluationProof> ActiveMovementInputEvaluationProof;
	// End 的外调前先封住该 Session 的 Proof；新 Session 的不同序号不受旧栈影响。
	uint64 RetiringMovementInputEvaluationSessionSerial = 0;
	using FMovementInputEvaluationEvent = TMulticastDelegate<void(EGGYGOMovementInputEvaluationState, const FString&)>;
	FMovementInputEvaluationEvent MovementInputEvaluationObservers;
	// 仅覆盖原生 Evaluate 在途栈；重入或在栈内 InputKey 不得认证旧 Action 数据。
	bool bEvaluatingMovementAction = false;
	bool bMovementActionEvaluationInterrupted = false;
	// Captured once from authenticated Cold; a new producer cannot infer transferred observations.
	TWeakObjectPtr<UGGYGOMovementInputOriginResource> ProducerOriginResource;
	EGGYGOMovementInputSessionMode CurrentSessionMode = EGGYGOMovementInputSessionMode::Invalid;
	// This session's outstanding Claim obligation, always checked against the resource authority.
	bool bSessionInitialClaimPending = false;
	FGGYGOMovementInputConsumerBindingId ReceiverBinding;
	FGGYGOMovementInputFactDelegate FactReceiver;
	TSet<FName> ReportedReasons;
	// Original outgoing fact copies only; drained synchronously on the same call stack.
	TArray<FPendingFact> PendingFacts;
	uint64 LastSessionSerial = 0;
	uint64 LastRequestSerial = 0;
	uint64 LastEventSerial = 0;
	uint64 ActiveRequestSerial = 0;
	uint64 ObservationRevision = 0;
	uint64 PreparedObservationRevision = 0;
	bool bNeutralConfirmed = false;
	bool bRequestSourceUnresolved = false;
	bool bInsideFlush = false;
	bool bEndingInput = false;
	bool bSerialExhausted = false;
	bool bDeliveringFacts = false;
	bool bBatchingFacts = false;
};
