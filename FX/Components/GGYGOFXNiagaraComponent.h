/** @file GGYGOFXNiagaraComponent.h @brief 原 Niagara 的场登记及 CPU 持久首错；复用原生激活与销毁。 */
#pragma once

#include "NiagaraComponent.h"
#include "NiagaraDataInterface.h"
#include "Niagara/GGYGOFXRendererFailureReceiver.h"
#include "FX/GGYGOFXFieldSettings.h"
#include "GGYGOFXNiagaraComponent.generated.h"

class UGGYGOCharacterLightSources;
struct FGGYGOCharacterPointLightFrame;
enum class EGGYGOCharacterLightSourceReadiness : uint8;
enum class EGGYGOCharacterLightingReadiness : uint8;
struct FGGYGOFXSceneMaterialState;

class UGGYGODistortionFieldComponent;
class UGGYGOFXNiagaraComponent;
class UGGYGONiagaraMaterialPreparation;

DECLARE_MULTICAST_DELEGATE_TwoParams(FGGYGOFXEffectFailed,
	UGGYGOFXNiagaraComponent*, const FString&);

/** CPU VM 在原实例内保留首个模拟错误；与粒子压缩及回调队列寿命分开。 */
UCLASS(EditInlineNew, Category = "GGYGO|FX", meta = (DisplayName = "GGYGO CPU FX runtime failure"))
class GGYGO_API UGGYGONiagaraRuntimeFailureDataInterface : public UNiagaraDataInterface
{
	GENERATED_BODY()
public:
	UGGYGONiagaraRuntimeFailureDataInterface(const FObjectInitializer& ObjectInitializer);
	/** 作者提供的准确来源；活动实例在 Init 时保存，不随资产后续编辑漂移。 */
	UPROPERTY(EditAnywhere, Category = "RuntimeFailure")
	FString SourceReference;
	virtual void PostInitProperties() override;
	virtual int32 PerInstanceDataSize() const override;
	virtual bool InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance) override;
	virtual void DestroyPerInstanceData(void* Data, FNiagaraSystemInstance* Instance) override;
	virtual bool PerInstanceTickPostSimulate(void* Data, FNiagaraSystemInstance* Instance, float DeltaSeconds) override;
	virtual bool HasPostSimulateTick() const override { return true; }
	virtual bool PostSimulateCanOverlapFrames() const override { return false; }
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return Target == ENiagaraSimTarget::CPUSim; }
	virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& Binding, void* Data, FVMExternalFunction& Out) override;
	virtual bool Equals(const UNiagaraDataInterface* Other) const override;
protected:
	virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;
#if WITH_EDITORONLY_DATA
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#endif
private:
	void VMReportFailure(FVectorVMExternalFunctionContext& Context);
	void PublishFailure(void* Data);
};

/** 持原实例的渲染伴随件及失败观察；原 Task／Publication 仍独占播放资源。 */
UCLASS(Transient)
class GGYGO_API UGGYGOFXNiagaraComponent : public UNiagaraComponent, public IGGYGOFXRendererFailureReceiver
{
	GENERATED_BODY()
public:
	UGGYGOFXNiagaraComponent();
	/** 在原生激活前配置一次；同值重入幂等，不能改写活动原实例的配置。 */
	bool ConfigureField(const FGGYGOFXFieldSettings& Settings, FString& OutError);
	/** 原Task/Publication明确提供Avatar；World空间组件的NativeOwner不作为灯宿主来源。 */
	bool ConfigureSceneLightingSource(AActor* Avatar, FString& OutError);
	/** 远端原seek揭示前查询；等待真实灯快照的MID不能被seek路径提前显示。 */
	bool AreSceneMaterialsApplied() const;
	const FString& GetEffectFailure() const { return EffectFailure; }
	/** 仅接真实原生实例的GT渲染失败；旧实例通知不影响后继，失败仍由原播放所有者清理。 */
	virtual void ReportFXRendererFailure(UNiagaraSystem* OriginalSystem, uint64 OriginalInstanceId,
		const FString& SourceReference, const FString& Reason) override;
	FGGYGOFXEffectFailed OnEffectFailed;
	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;
	virtual void DeactivateImmediate() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
protected:
	virtual bool RequiresGameThreadEndOfFrameUpdates() const override;
	virtual bool RequiresGameThreadEndOfFrameRecreate() const override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
	virtual void OnUpdateTransform(EUpdateTransformFlags UpdateTransformFlags, ETeleportType Teleport) override;
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
private:
	friend class UGGYGONiagaraRuntimeFailureDataInterface;
	bool HasEnabledProjectedDecalRenderer() const;
	bool BindRuntimeFailureInstance(FNiagaraSystemInstance* Instance, FString& OutError);
	void LatchRuntimeInitializationFailure(FNiagaraSystemInstance* Instance, const FString& Source, const FString& Reason);
	void QueueRuntimeFailure(uint64 InstanceId, const FString& Reason);
	void LatchRuntimeFailure(uint64 InstanceId, const FString& SourceReference, int32 FailureCode);
	void DispatchEffectFailure();
	UFUNCTION() void HandleOriginalSystemFinished(UNiagaraComponent* Component);
	bool RefreshFieldBinding(FString& OutError);
	void RejectEffect(const FString& Reason);
	void ReleaseFieldRegistration();
	bool RegisterPointLightSource(FString& OutError);
	EGGYGOCharacterLightSourceReadiness ReadPointLightFrame(FGGYGOCharacterPointLightFrame& OutFrame, FString& OutError);
	void ReleasePointLightSource();
	struct FFieldActivationRequest
	{
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UWorld> World;
		bool bReset = false;
	};
	void StartNativeEffect(bool bReset);
	void TryStartFieldActivation(TSharedPtr<FFieldActivationRequest> Request);
	void ActivateNativeEffect(bool bReset);
	void ActivatePreparedEffect(bool bReset);
	UFUNCTION() void HandleMaterialPreparationFinished(UGGYGONiagaraMaterialPreparation* Request);
	void CancelMaterialPreparation();
	bool ReadAdditionalWPOBoundsRadius(float& OutRadiusCM, FString& OutError) const;
	bool IsSceneMaterialStateCurrent(const TSharedPtr<FGGYGOFXSceneMaterialState>& State) const;
	void TryRegisterSceneMaterials(TSharedPtr<FGGYGOFXSceneMaterialState> State);
	void ObserveSceneMaterialReadiness(const TSharedPtr<FGGYGOFXSceneMaterialState>& State);
	void HandleSceneMaterialReadiness(EGGYGOCharacterLightingReadiness Readiness, const FString& Reason,
		TWeakPtr<FGGYGOFXSceneMaterialState> State);
	void HandleSceneMaterialsInvalidated(uint64 Handle, EGGYGOCharacterLightingReadiness Readiness,
		const FString& Reason, TWeakPtr<FGGYGOFXSceneMaterialState> State);
	void ReleaseSceneMaterials();
	/** 原Avatar只提供宿主来源；MID与订阅/原覆盖回执随本次SceneMaterials归还。 */
	UPROPERTY(Transient) TWeakObjectPtr<AActor> SceneLightingAvatar;
	bool bSceneLightingSourceConfigured = false;
	TSharedPtr<FGGYGOFXSceneMaterialState> SceneMaterials;
	/** World登记回执，仅覆盖本Controller/NS；注销先摘本句柄，不保存模拟帧。 */
	UPROPERTY(Transient) TWeakObjectPtr<UGGYGOCharacterLightSources> PointLightSources;
	UPROPERTY(Transient) TWeakObjectPtr<UNiagaraSystem> PointLightSystem;
	uint64 PointLightSourceHandle = 0;
	uint64 PointLightInstanceId = 0;
	/** 仅保护同步原Native Finalize会回调重入的读取栈，不是灯状态缓存。 */
	bool bReadingPointLights = false;
	UPROPERTY(Transient) FGGYGOFXFieldSettings FieldSettings;
	UPROPERTY(Transient) TObjectPtr<UGGYGODistortionFieldComponent> FieldRegistration;
	/** 原激活请求的短期等待，随本组件停止／归还取消。 */
	TSharedPtr<FFieldActivationRequest> FieldActivation;
	/** 最终参数已安装后的原资源请求；请求自身拥有就绪事实，组件只保取消权与原激活参数。 */
	UPROPERTY(Transient) TObjectPtr<UGGYGONiagaraMaterialPreparation> MaterialPreparation;
	bool bMaterialActivationReset = false;
	/** 记录调用者已明确提交配置，随本组件失效；不是 ShaderMap ready 缓存。 */
	bool bFieldSettingsConfigured = false;
	/** 唯一首错供原所有者读取；异步通知只引用此事实，不另建失败权威。 */
	FString EffectFailure;
	/** 派生的原生实例观察身份；在真实 DI Init 更新，完成／注销后关闭。 */
	uint64 RuntimeFailureInstanceId = 0;
	TWeakObjectPtr<UNiagaraSystem> RuntimeFailureSystem;
	bool bRuntimeFailureObservationOpen = false;
	/** 从原 System 的 User Float 派生的世界空间 WPO 半径；原激活至尾粒子完成有效。 */
	float AdditionalWPOBoundsRadiusCM = 0.0f;
	bool bEffectFailureDispatched = false;
};
