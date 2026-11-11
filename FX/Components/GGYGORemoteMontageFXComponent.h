/** @file GGYGORemoteMontageFXComponent.h @brief 单一远端 Niagara 实例的原生 Age 恢复与释放。 */
#pragma once

#include "FX/Components/GGYGOFXNiagaraComponent.h"
#include "FX/GGYGOMontageFXPublication.h"
#include "GGYGORemoteMontageFXComponent.generated.h"

DECLARE_DELEGATE_TwoParams(FGGYGOLocalNiagaraClosed, UNiagaraComponent*, bool);

/** 只扩展自己的 Niagara TickFunction；不驱动 Montage，不维护第二个动作时钟。 */
UCLASS(Transient)
class GGYGO_API UGGYGORemoteMontageFXComponent : public UGGYGOFXNiagaraComponent
{
	GENERATED_BODY()
public:
	void InitializePresentation(UGGYGOMontageFXPublicationOwner* Owner,
		const FGGYGOMontageFXRecord& Record, uint64 Generation);
	/** 非 GA 本地实例复用同一原生 Niagara 路径；Begin 为原完成事件的 World 采样。
	 * 不发布 occurrence、不构造运动来源；调用者在本方法前安装自身资源及关闭回调。 */
	bool InitializeLocalPresentation(UGGYGOMontageFXPublicationOwner* Owner,
		const FGGYGOFXSourceIdentity& Source, const FGGYGOMontageFXRecord& Request,
		double OriginalBeginWorldTime, FString& OutError);
	void SetLocalClosedCallback(FGGYGOLocalNiagaraClosed Callback) { LocalClosed = MoveTemp(Callback); }
	bool HasNaturallyCompleted() const { return bNaturalCompletion; }
	void StartPresentation();
	void RefreshSourceReferences(const FGGYGOMontageFXRecord& Record);
	void ReleasePresentation(EGGYGONiagaraEffectStopMode Mode);
	EGGYGONiagaraEffectStopMode GetStopMode(EGGYGOMontageFXCloseReason Reason) const;
	virtual void TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
#if WITH_EDITOR
	/** 不等待或推进原生模拟；Controller 无效时 Age 明确不可用。 */
	void ReadObservation(FGGYGOMontageFXObservationFact& OutFact) const;
	void AddObservationWindow(const TSharedPtr<FGGYGOMontageFXObservation>& Window);
#endif
protected:
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
private:
	void HandleLocalStartReady();
	bool CanPresent() const;
	/** 同时提交组件与原 Niagara 渲染代理状态，不额外推进模拟。 */
	void SetPresentationRenderingEnabled(bool bEnabled);
	void RestoreAge(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction);
	void FailPresentation(const FString& Reason);
	void HandleEffectFailed(UGGYGOFXNiagaraComponent* Component, const FString& Reason);
	UFUNCTION() void HandlePresentationFinished(UNiagaraComponent* Component);
	UPROPERTY(Transient) FGGYGOMontageFXRecord Playback;
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> ResourceOwner;
	uint64 ResourceGeneration = 0;
	/** 仅私有本地资源能力；与网络 occurrence 不互换。 */
	bool bLocalProducer = false;
	double LocalBeginWorldTime = 0.;
	TSharedPtr<FGGYGOMontageFXLocalCapability> LocalCapability;
	FGGYGOMontageFXLocalStartTicket LocalStartTicket;
	FGGYGOLocalNiagaraClosed LocalClosed;
	bool bReached = false;
	bool bTransferred = false;
	bool bNaturalCompletion = false;
	bool bFailed = false;
	bool bActivationRequested = false;
	bool bOriginalForceSolo = false;
	bool bOriginalRendering = true;
	ENiagaraAgeUpdateMode OriginalAgeMode = ENiagaraAgeUpdateMode::TickDeltaTime;
#if WITH_EDITOR
	void AppendObservation(EGGYGOMontageFXObservationKind Kind, bool bDestroyed = false);
	TWeakObjectPtr<UGGYGOMontageFXPublicationOwner> ObservationPublisher;
	uint64 ObservationRegistration = 0;
	/** Age 恢复达到后，StopComponent 真实返回且原组件仍存活的 Deactivate 尾部置真。
	 * 仅记录本组件的观察事实，随组件销毁失效，不驱动生产状态。 */
	bool bObservationTailTransferred = false;
	/** 原 OnSystemFinished 回调置真，覆盖 bTransferred 后生产完成位不再写入的阶段。
	 * 仅记录本组件的观察事实，随组件销毁失效，不驱动生产状态。 */
	bool bObservationNaturalCompletion = false;
	TArray<TWeakPtr<FGGYGOMontageFXObservation>> ObservationWindows;
#endif
};
