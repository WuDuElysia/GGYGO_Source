/** @file GGYGOMontageFXPublication.h @brief Montage 表现的派生复制、精确关闭与本地资源能力。 */
#pragma once

#include "CoreMinimal.h"
#include "AbilitySystem/GGYGOAvatarBindingTypes.h"
#include "FX/Tasks/GGYGOAbilityTask_PlayNiagaraEffect.h"
#include "UObject/Interface.h"
#include "GGYGOMontageFXPublication.generated.h"

class UAbilitySystemComponent;
class USkeletalMeshComponent;
class UGGYGORemoteMontageFXComponent;
class UGGYGOMontageFXPublicationOwner;
class APawn;
class AController;
class UWorld;
class AGGYGOCharacterBase;
class UNiagaraComponent;
class UGGYGOCharacterMovementComponent;
class UGGYGOLocomotionFXComponent;
class FGGYGOLocomotionSourceProof;
struct FGGYGOMontageFXLeaseState;
struct FGGYGOMontageFXLocalCapability;

/** 本机来源描述。只封装原模块已认证的证明，不签执行权，也不复制到网络。 */
enum class EGGYGOFXSourceKind : uint8 { Invalid, AbilityActivation, Locomotion };

class GGYGO_API FGGYGOFXSourceIdentity
{
public:
	static FGGYGOFXSourceIdentity FromAbilityActivation(const TSharedPtr<const void>& OriginalProof);
	static FGGYGOFXSourceIdentity FromLocomotion(const FGGYGOLocomotionSourceProof& OriginalProof,
		const FGGYGOAvatarBindingContext& OriginalContext);
	bool IsSet() const;
	EGGYGOFXSourceKind GetKind() const { return Kind; }
	const TSharedPtr<const void>& GetAbilityActivationProof() const { return ActivationProof; }
	/** 调用者先按 GetKind 分派；这里不把空证明解释为合法来源。 */
	const FGGYGOLocomotionSourceProof& GetLocomotionSourceProof() const;
	UGGYGOCharacterMovementComponent* GetOriginalMovement() const;
	const FGGYGOAvatarBindingContext& GetAvatarBindingContext() const { return BindingContext; }
	bool HasSameSource(const FGGYGOFXSourceIdentity& Other) const;
	bool operator==(const FGGYGOFXSourceIdentity& Other) const { return HasSameSource(Other); }
private:
	EGGYGOFXSourceKind Kind = EGGYGOFXSourceKind::Invalid;
	TSharedPtr<const void> ActivationProof;
	TSharedPtr<const FGGYGOLocomotionSourceProof> LocomotionProof;
	FGGYGOAvatarBindingContext BindingContext;
};

UENUM()
enum class EGGYGOMontageFXCloseReason : uint8 { Normal, Abort };

/** 原 ASC 签发的 occurrence 是唯一跨端身份；动画/activation 的认证身份不复制。 */
USTRUCT()
struct GGYGO_API FGGYGOMontageFXRecord
{
	GENERATED_BODY()
	UPROPERTY() uint64 Occurrence = 0;
	UPROPERTY() double BeginServerTime = 0.;
	UPROPERTY() TObjectPtr<AActor> Avatar = nullptr;
	UPROPERTY() TObjectPtr<USkeletalMeshComponent> Mesh = nullptr;
	UPROPERTY() TObjectPtr<UNiagaraSystem> System = nullptr;
	UPROPERTY() FGGYGOFXFieldSettings FieldSettings;
	UPROPERTY() EGGYGONiagaraEffectSpace Space = EGGYGONiagaraEffectSpace::Unspecified;
	UPROPERTY() FName SocketName;
	/** World 已在真实 Begin 组合；Attached 是原 Mesh/socket 的相对变换。 */
	UPROPERTY() FTransform Transform = FTransform::Identity;
	UPROPERTY() EGGYGONiagaraEffectStopMode NormalStopMode = EGGYGONiagaraEffectStopMode::Unspecified;
	UPROPERTY() EGGYGONiagaraEffectStopMode AbortStopMode = EGGYGONiagaraEffectStopMode::Unspecified;
	UPROPERTY() float RemoteSeekDeltaSeconds = 0.f;
	UPROPERTY() float RemoteMaxSimTimeSeconds = 0.f;
	bool Validate(FString& OutError) const;
};

/** 一个反射父属性整体接收；引用未映射时仍保留 occurrence 成员。 */
USTRUCT()
struct GGYGO_API FGGYGOMontageFXState
{
	GENERATED_BODY()
	UPROPERTY() uint64 StateRevision = 0;
	UPROPERTY() TArray<FGGYGOMontageFXRecord> ActiveRecords;
	/** 同一网络 ASC 跨重注册保留唯一资源视图；不是复制数据。 */
	UPROPERTY(Transient, NotReplicated) TObjectPtr<UGGYGOMontageFXPublicationOwner> LocalOwner = nullptr;
	const FGGYGOMontageFXRecord* Find(uint64 Occurrence) const;
};

USTRUCT()
struct GGYGO_API FGGYGOMontageFXTerminal
{
	GENERATED_BODY()
	UPROPERTY() uint64 Occurrence = 0;
	UPROPERTY() uint64 CloseRevision = 0;
	UPROPERTY() EGGYGOMontageFXCloseReason Reason = EGGYGOMontageFXCloseReason::Abort;
};

#if WITH_EDITOR
/** 仅 Editor 观察窗消费的值；不授予播放、关闭或资源归还权限。 */
enum class EGGYGOMontageFXObservationKind : uint8
{
	Snapshot, Published, TerminalDispatched, TerminalReceived, RemoteCreated,
	ResourceChanged, TailTransferred, NaturalCompleted, ComponentUnregistered,
	ComponentDestroyed, PublisherClosed
};

struct GGYGO_API FGGYGOMontageFXObservationFact
{
	EGGYGOMontageFXObservationKind Kind = EGGYGOMontageFXObservationKind::Snapshot;
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Publisher;
	TWeakObjectPtr<UAbilitySystemComponent> Issuer;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AActor> Avatar;
	TWeakObjectPtr<USkeletalMeshComponent> Mesh;
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UNiagaraComponent> Component;
	/** 事件产生时复制的本地 UObject 身份；Drain 时弱对象可已销毁，不用于跨端配对。 */
	uint32 IssuerObjectId = 0;
	uint32 WorldObjectId = 0;
	uint32 AvatarObjectId = 0;
	uint32 MeshObjectId = 0;
	uint32 ComponentObjectId = 0;
	FString ComponentPath;
	FString AvatarPath;
	FString MeshPath;
	FString SystemPath;
	/** 写入原生事实时的 GFrameCounter，与宿主稍后 Drain 的帧分列。 */
	uint64 NativeFrame = 0;
	uint64 OwnerRegistration = 0;
	uint64 StateRevision = 0;
	uint64 Occurrence = 0;
	uint64 Generation = 0;
	/** Niagara 原生 SystemInstanceID；零不代表已获得 Controller 身份。 */
	uint64 ControllerId = 0;
	double BeginServerTime = 0.;
	double TargetAge = 0.;
	double ControllerAge = 0.;
	float DesiredAge = 0.f;
	float EffectiveStep = 0.f;
	float RemoteSeekDeltaSeconds = 0.f;
	float RemoteMaxSimTimeSeconds = 0.f;
	int32 Operations = 0;
	int32 ActualExecutionState = INDEX_NONE;
	int32 RequestedExecutionState = INDEX_NONE;
	FGGYGOMontageFXTerminal Terminal;
	bool bHasRecord = false;
	bool bActiveRecord = false;
	bool bHasTerminal = false;
	bool bRemoteEntry = false;
	bool bTargetAgeValid = false;
	bool bDesiredAgeValid = false;
	bool bControllerValid = false;
	bool bControllerAgeValid = false;
	bool bControllerComplete = false;
	bool bControllerPendingSpawn = false;
	bool bReached = false;
	bool bRenderingEnabled = false;
	bool bRegistered = false;
	bool bReleaseRequested = false;
	bool bReleased = false;
	bool bTailTransferred = false;
	bool bNaturalCompletion = false;
	bool bFailed = false;
	/** 仅由原组件 OnComponentDestroyed 写出；弱引用失效不能生成此事实。 */
	bool bComponentDestroyed = false;
};

/** Editor 持有的有限收件窗。Runtime 仅弱引用，写值不调用观察者；Stop 立即清空。
 * 宿主须在触发前 Begin，按原回调 Capture/Drain，并在 finally Stop。 */
class GGYGO_API FGGYGOMontageFXObservation
{
public:
	bool Drain(TArray<FGGYGOMontageFXObservationFact>& OutFacts, FString& OutDiagnostic);
	void Stop();
	bool IsActive() const { return !bStopped && !bOverflowed; }
private:
	friend class UGGYGOMontageFXPublicationOwner;
	friend class UGGYGORemoteMontageFXComponent;
	void Add(const FGGYGOMontageFXObservationFact& Fact);
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Publisher;
	TWeakObjectPtr<UAbilitySystemComponent> Issuer;
	TWeakObjectPtr<UWorld> World;
	uint64 Registration = 0;
	uint32 IssuerObjectId = 0;
	uint32 WorldObjectId = 0;
	int32 Capacity = 0;
	bool bStopped = false;
	bool bOverflowed = false;
	TArray<FGGYGOMontageFXObservationFact> Pending;
};
#endif

enum class EGGYGOMontageFXCloseResult : uint8 { Closed, AlreadyClosed, Rejected };

/** 本地 opaque 许可；关闭只认原签发者及原 occurrence，不查新 Current。 */
class GGYGO_API FGGYGOMontageFXLease
{
public:
	bool HasLease() const { return State.IsValid(); }
	uint64 GetOccurrence() const;
private:
	TSharedPtr<FGGYGOMontageFXLeaseState> State;
	friend class UGGYGOMontageFXPublicationOwner;
};

/** 纯写退休携出的义务；析构不执行网络或资源外调。调用方须在安全出口显式 Dispatch。 */
class GGYGO_API FGGYGOMontageFXCloseObligation
{
public:
	FGGYGOMontageFXCloseObligation() = default;
	FGGYGOMontageFXCloseObligation(FGGYGOMontageFXCloseObligation&& Other);
	FGGYGOMontageFXCloseObligation& operator=(FGGYGOMontageFXCloseObligation&& Other);
	FGGYGOMontageFXCloseObligation(const FGGYGOMontageFXCloseObligation&) = delete;
	FGGYGOMontageFXCloseObligation& operator=(const FGGYGOMontageFXCloseObligation&) = delete;
	bool IsSet() const { return Terminal.Occurrence != 0; }
	/** 每次从原 weak issuer 重新取得，调用者不能改取当前 ASC owner。 */
	UGGYGOMontageFXPublicationOwner* GetIssuer() const { return Issuer.Get(); }
private:
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> Issuer;
	FGGYGOMontageFXTerminal Terminal;
#if WITH_EDITOR
	/** PrepareClose 仅复制原记录身份；DispatchClose 的安全出口才写入观察窗。 */
	FGGYGOMontageFXObservationFact Observation;
#endif
	friend class UGGYGOMontageFXPublicationOwner;
};

/** Ticket 是本地 Task 请求身份；Ready 事实不是永久启动权。 */
struct GGYGO_API FGGYGOMontageFXLocalStartTicket
{
	uint64 ScopeEpoch = 0;
	uint64 Nonce = 0;
	bool IsSet() const { return ScopeEpoch != 0 && Nonce != 0; }
	bool operator==(const FGGYGOMontageFXLocalStartTicket& Other) const
	{ return ScopeEpoch == Other.ScopeEpoch && Nonce == Other.Nonce; }
};

enum class EGGYGOMontageFXLocalStartResult : uint8
{
	ReadyToActivate, WaitingForRemoteRelease, DedicatedServerNoPresentation, Cancelled, Rejected
};
enum class EGGYGOMontageFXPermitResult : uint8 { Granted, AlreadyConsumed, StillWaiting, Closed, Rejected };
enum class EGGYGOMontageFXLocalResourceFact : uint8 { Unknown, NoProductionNow, Pending, Released };

struct GGYGO_API FGGYGOMontageFXLocalStartReadyFact
{
	TWeakObjectPtr<UGGYGOAbilityTask_PlayNiagaraEffect> Task;
	FGGYGOMontageFXLocalStartTicket Ticket;
	uint64 ResourceRevision = 0;
};

DECLARE_DELEGATE_OneParam(FGGYGOMontageFXSendTerminal, const FGGYGOMontageFXTerminal&);

/** ASC 只提供 FX owner；FX 公共类型不依赖项目 ASC/GA 或 Combo。 */
UINTERFACE(MinimalAPI)
class UGGYGOMontageFXHost : public UInterface { GENERATED_BODY() };
class GGYGO_API IGGYGOMontageFXHost
{
	GENERATED_BODY()
public:
	virtual UGGYGOMontageFXPublicationOwner* GetMontageFXPublicationOwner() = 0;
};

/** 原 GA/运动 owner 保持业务来源。本对象只拥有表现派生数据、资源能力及客户端实例，无 Tick。 */
UCLASS()
class GGYGO_API UGGYGOMontageFXPublicationOwner : public UObject
{
	GENERATED_BODY()
public:
	UGGYGOMontageFXPublicationOwner();
	/** UE 热重载构造须在 FState 完整定义后实现，避免生成代码删除不完整类型。 */
	UGGYGOMontageFXPublicationOwner(FVTableHelper& Helper);
	virtual ~UGGYGOMontageFXPublicationOwner() override;
	static UGGYGOMontageFXPublicationOwner* RegisterHost(UAbilitySystemComponent* OriginalIssuer,
		FGGYGOMontageFXState& State, FGGYGOMontageFXSendTerminal SendTerminal);
	uint64 GetRegistrationSerial() const;
	/** State 地址属于同一原 ASC，持续至 Shutdown；调用前 ASC 应已安装强引用 owner。 */
	void Initialize(UAbilitySystemComponent* OriginalIssuer, FGGYGOMontageFXState& State,
		FGGYGOMontageFXSendTerminal SendTerminal);
	/** Request 的 BeginServerTime 必须来自已认证的原生事件采样；不重写为接收时刻。 */
	bool Begin(const FGGYGOFXSourceIdentity& OriginalSource, const FGGYGOMontageFXRecord& Request,
		FGGYGOMontageFXLease& OutLease, FString& OutError);
	EGGYGOMontageFXCloseResult PrepareClose(const FGGYGOMontageFXLease& Lease,
		EGGYGOMontageFXCloseReason Reason, FGGYGOMontageFXCloseObligation& OutObligation);
	/** 按原种类与 owner 证明的历史身份匹配；不调用 Current 谓词或虚函数。 */
	void PrepareCloseSource(const FGGYGOFXSourceIdentity& OriginalSource,
		EGGYGOMontageFXCloseReason Reason, TArray<FGGYGOMontageFXCloseObligation>& OutObligations);
	/** 复制全部未关闭来源；生产者分类与撤绑均消费同一 typed 描述。 */
	void GetSourceIdentities(TArray<FGGYGOFXSourceIdentity>& OutIdentities) const;
	bool DispatchClose(FGGYGOMontageFXCloseObligation&& Obligation);
	void ReceiveState();
	void ReceiveTerminal(const FGGYGOMontageFXTerminal& Terminal);
	void Shutdown(uint64 ExpectedRegistration = 0);

	EGGYGOMontageFXLocalResourceFact ReadLocalResourceFact(AActor* Avatar) const;
#if WITH_EDITOR
	/** 先安装弱窗再采快照；容量为 1..4096，溢出明确失败。无需本地 GA。 */
	TSharedPtr<FGGYGOMontageFXObservation> BeginObservation(int32 MaxRecords, FString& OutDiagnostic);
	/** 只读原 owner/registration 的当前事实，不推进模拟、不等待异步 Tick。 */
	bool CaptureObservation(const TSharedPtr<FGGYGOMontageFXObservation>& Window, FString& OutDiagnostic) const;
#endif
private:
	friend class UGGYGOAbilityTask_PlayNiagaraEffect;
	friend class UGGYGOLocomotionFXComponent;
	friend class UGGYGORemoteMontageFXComponent;
	friend class FGGYGOMontageFXResourceFactsTest;
	/** 工厂在返回 Task 前登记；Task 是资源事实唯一写端，订阅者不以弱空代替归还。 */
	TSharedPtr<FGGYGOMontageFXLocalCapability> RegisterLocalTask(
		UGGYGOAbilityTask_PlayNiagaraEffect* Task, AActor* Avatar, UWorld* World, FString& OutError);
	TSharedPtr<FGGYGOMontageFXLocalCapability> RegisterLocalProducer(UObject* Producer,
		EGGYGOFXSourceKind Kind, AActor* Avatar, UWorld* World, FSimpleDelegate Shutdown, FString& OutError,
		const FGGYGOFXSourceIdentity& Source = FGGYGOFXSourceIdentity());
	void BeginLocalOperation(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Capability);
	void EndLocalOperation(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Capability);
	void SetLocalOwnership(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Capability, bool bOwned);
	void SealLocalProducer(const TSharedPtr<FGGYGOMontageFXLocalCapability>& Capability);
	void FlushResourceChanges();
	EGGYGOMontageFXLocalStartResult RequestLocalStart(
		const TSharedPtr<FGGYGOMontageFXLocalCapability>& Capability,
		FGGYGOMontageFXLocalStartTicket& OutTicket, FString& OutError);
	bool ObserveLocalStart(const FGGYGOMontageFXLocalStartTicket& Ticket,
		FGGYGOMontageFXLocalStartReadyDelegate Callback);
	bool ObserveLocalProducerStart(const FGGYGOMontageFXLocalStartTicket& Ticket, FSimpleDelegate Callback);
	EGGYGOMontageFXPermitResult ConsumeLocalStart(const FGGYGOMontageFXLocalStartTicket& Ticket);
	void CancelLocalStart(const FGGYGOMontageFXLocalStartTicket& Ticket);
	bool HasLocalPermit(const FGGYGOMontageFXLocalStartTicket& Ticket) const;

public:
	/** 远端组件同一原生 Tick 的事实；generation 防旧清理触及同 key 的后继实例。 */
	bool CanRestore(uint64 Occurrence, uint64 Generation) const;
	void BeginRemoteOperation(uint64 Occurrence, uint64 Generation);
	void EndRemoteOperation(uint64 Occurrence, uint64 Generation);
	void RemoteDestroyed(uint64 Occurrence, uint64 Generation, bool bNaturalCompletion, bool bFailed = false);

	static bool CanCollectClosedKey(uint64 ReceivedRevision, uint64 CloseRevision,
		bool bStillInActiveRecords, bool bHasUnreturnedResource);
private:
	struct FState;
	TUniquePtr<FState> Data;
#if WITH_EDITOR
	friend class UGGYGORemoteMontageFXComponent;
	void FillObservationContext(FGGYGOMontageFXObservationFact& Fact) const;
	void CaptureObservationFact(uint64 Occurrence, FGGYGOMontageFXObservationFact& Fact) const;
	void AppendObservation(FGGYGOMontageFXObservationFact Fact);
	TArray<TWeakPtr<FGGYGOMontageFXObservation>> ObservationWindows;
#endif
	void Reconcile();
	void ReleaseRemote(uint64 Occurrence, EGGYGONiagaraEffectStopMode Mode, bool bTerminal);
	void CollectClosedKeys();
	void ObserveAvatar(AActor* Avatar);
	void HandleRoleChanged(AGGYGOCharacterBase* Character);
	UFUNCTION() void HandleControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);
	UPROPERTY(Transient) TArray<TObjectPtr<UGGYGORemoteMontageFXComponent>> OwnedRemoteComponents;
};
