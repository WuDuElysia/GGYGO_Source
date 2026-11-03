/** @file GGYGOFXNiagaraComponent.cpp @brief 同一 Niagara 的场登记、CPU 首错持久回传与幂等归还。 */
#include "FX/Components/GGYGOFXNiagaraComponent.h"

#include "FX/Components/GGYGOFXNiagaraLightSource.h"
#include "FX/Components/GGYGOFXNiagaraSceneMaterials.h"
#include "Character/Components/GGYGOCharacterRenderComponent.h"
#include "Character/GGYGOCharacterLightSources.h"
#include "Distortion/GGYGODistortionFieldComponent.h"
#include "Distortion/GGYGODistortionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include "NiagaraDataInterfaceUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "NiagaraWorldManager.h"
#include <atomic>

#ifndef GGYGO_WITH_DISTORTION_FIELD
#error GGYGORendering.Build.cs must explicitly export GGYGO_WITH_DISTORTION_FIELD
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOFXNiagara, Log, All);

namespace
{
	/** 无 GPU 数据；只满足原 Niagara DI proxy 寿命。 */
	struct FGGYGOFailureProxy final : FNiagaraDataInterfaceProxy
	{
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }
	};

	struct FGGYGONativeFailureData
	{
		TWeakObjectPtr<UGGYGOFXNiagaraComponent> Owner;
		TWeakObjectPtr<UNiagaraComponent> PreviewComponent;
		TWeakObjectPtr<UNiagaraSystem> PreviewSystem;
		bool bAuthoringPreview = false;
		FString SourceReference;
		uint64 InstanceId = 0;
		// CPU VM 可并发执行；GT 只在原模拟完成后读取，存储随 Native DI 归还。
		std::atomic<int32> FirstFailure{0};
		bool bPublished = false;
	};

	const FName ReportRuntimeFailureName(TEXT("ReportFailure"));
	const FName AdditionalWPOBoundsRadiusName(TEXT("User.UEFX_AdditionalWPOBoundsRadiusCM"));
	const FMaterialParameterInfo SourceVertexWPOParameter(TEXT("UEFX_SourceVertexWPO"));
	const FMaterialParameterInfo SourceVertexWPOBoundsParameter(TEXT("UEFX_SourceVertexWPOBoundsRadiusCM"));

	bool IsAuthoringWorld(const UWorld* World)
	{
		return World && (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::EditorPreview);
	}
}

UGGYGONiagaraRuntimeFailureDataInterface::UGGYGONiagaraRuntimeFailureDataInterface(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Proxy.Reset(new FGGYGOFailureProxy());
}

void UGGYGONiagaraRuntimeFailureDataInterface::PostInitProperties()
{
	Super::PostInitProperties();
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()),
			ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter);
	}
}

int32 UGGYGONiagaraRuntimeFailureDataInterface::PerInstanceDataSize() const
{
	return sizeof(FGGYGONativeFailureData);
}

bool UGGYGONiagaraRuntimeFailureDataInterface::InitPerInstanceData(void* Data, FNiagaraSystemInstance* Instance)
{
	check(IsInGameThread());
	auto* Memory = new (Data) FGGYGONativeFailureData;
	UNiagaraComponent* Original = Instance ? Cast<UNiagaraComponent>(Instance->GetAttachComponent()) : nullptr;
	if (Original && IsAuthoringWorld(Instance->GetWorld()))
	{
		// 原生作者模式不需要 GAS owner；仍只观察同一真实预览实例。
		const auto Controller = Original->GetSystemInstanceController();
		if (!Controller.IsValid() || !Controller->IsValid() || Controller->GetSystemInstanceID() != Instance->GetId()
			|| Original->GetAsset() != Instance->GetSystem() || Original->GetWorld() != Instance->GetWorld())
		{
			UE_LOG(LogGGYGOFXNiagara, Error, TEXT("Niagara作者预览初始化身份不符 System='%s' Source='%s'。"),
				*GetPathNameSafe(Instance->GetSystem()), *SourceReference);
			return false;
		}
		Memory->bAuthoringPreview = true;
		Memory->PreviewComponent = Original;
		Memory->PreviewSystem = Instance->GetSystem();
		Memory->InstanceId = Instance->GetId();
		Memory->SourceReference = SourceReference;
		if (SourceReference.TrimStartAndEnd().IsEmpty())
		{
			Memory->FirstFailure.store(6);
			PublishFailure(Data);
			return false;
		}
		return true;
	}
	UGGYGOFXNiagaraComponent* Owner = Cast<UGGYGOFXNiagaraComponent>(Original);
	if (!Owner)
	{
		UE_LOG(LogGGYGOFXNiagara, Error, TEXT("Niagara运行失败接口缺原GGYGO组件 System='%s' Source='%s'。"),
			Instance ? *GetPathNameSafe(Instance->GetSystem()) : TEXT("<missing>"), *SourceReference);
		return false;
	}
	FString BindingError;
	if (!Owner->BindRuntimeFailureInstance(Instance, BindingError))
	{
		// 只为当前原生实例锁存初始化失败；已退休/旧实例不得改写后继。
		Owner->LatchRuntimeInitializationFailure(Instance, SourceReference, BindingError);
		return false;
	}
	Memory->Owner = Owner;
	Memory->InstanceId = Instance->GetId();
	Memory->SourceReference = SourceReference;
	if (SourceReference.TrimStartAndEnd().IsEmpty())
	{
		Memory->FirstFailure.store(6);
		PublishFailure(Data);
		return false;
	}
	return true;
}

void UGGYGONiagaraRuntimeFailureDataInterface::PublishFailure(void* Data)
{
	check(IsInGameThread());
	auto* Memory = static_cast<FGGYGONativeFailureData*>(Data);
	const int32 Code = Memory->FirstFailure.load();
	if (Code == 0 || Memory->bPublished) { return; }
	Memory->bPublished = true;
	if (Memory->bAuthoringPreview)
	{
		UNiagaraComponent* Component = Memory->PreviewComponent.Get();
		const auto Controller = Component ? Component->GetSystemInstanceController() : FNiagaraSystemInstanceControllerPtr();
		if (!Component || !IsAuthoringWorld(Component->GetWorld()) || Component->GetAsset() != Memory->PreviewSystem.Get()
			|| !Controller.IsValid() || !Controller->IsValid() || Controller->GetSystemInstanceID() != Memory->InstanceId) { return; }
		UE_LOG(LogGGYGOFXNiagara, Error, TEXT("Niagara作者预览失败 System='%s' Instance=%llu Source='%s' Code=%d。"),
			*GetPathNameSafe(Memory->PreviewSystem.Get()), Memory->InstanceId, *Memory->SourceReference, Code);
		const auto Original = Memory->PreviewComponent;
		const auto System = Memory->PreviewSystem;
		const uint64 InstanceId = Memory->InstanceId;
		FNiagaraWorldManager::EnqueueGlobalDeferredCallback([Original, System, InstanceId]
		{
			UNiagaraComponent* Preview = Original.Get();
			if (!Preview || !IsAuthoringWorld(Preview->GetWorld()) || Preview->GetAsset() != System.Get()) { return; }
			const auto Current = Preview->GetSystemInstanceController();
			if (Current.IsValid() && Current->IsValid() && Current->GetSystemInstanceID() == InstanceId && !Current->IsComplete())
			{ Preview->DeactivateImmediate(); }
		});
		return;
	}
	if (UGGYGOFXNiagaraComponent* Owner = Memory->Owner.Get())
	{ Owner->LatchRuntimeFailure(Memory->InstanceId, Memory->SourceReference, Code); }
}

bool UGGYGONiagaraRuntimeFailureDataInterface::PerInstanceTickPostSimulate(
	void* Data, FNiagaraSystemInstance*, float)
{
	PublishFailure(Data);
	return false;
}

void UGGYGONiagaraRuntimeFailureDataInterface::DestroyPerInstanceData(void* Data, FNiagaraSystemInstance*)
{
	// Complete 可能跳过 post-simulate；此处先锁存，再由原生 OnComplete 通知原所有者。
	PublishFailure(Data);
	static_cast<FGGYGONativeFailureData*>(Data)->~FGGYGONativeFailureData();
}

#if WITH_EDITORONLY_DATA
void UGGYGONiagaraRuntimeFailureDataInterface::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	FNiagaraFunctionSignature Signature;
	Signature.Name = ReportRuntimeFailureName;
	Signature.bMemberFunction = true;
	Signature.bRequiresContext = false;
	Signature.bSupportsCPU = true;
	Signature.bSupportsGPU = false;
	Signature.bWriteFunction = true;
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition(GetClass()), TEXT("Failure")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Code")));
	Signature.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("OriginalAlive")));
	Signature.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetBoolDef(), TEXT("KeepAlive")));
	OutFunctions.Add(Signature);
}
#endif

void UGGYGONiagaraRuntimeFailureDataInterface::GetVMExternalFunction(
	const FVMExternalFunctionBindingInfo& Binding, void* Data, FVMExternalFunction& Out)
{
	if (Binding.Name == ReportRuntimeFailureName && Binding.GetNumInputs() == 3 && Binding.GetNumOutputs() == 1)
	{
		Out = FVMExternalFunction::CreateUObject(this, &ThisClass::VMReportFailure);
		return;
	}
	if (Data)
	{
		int32 Expected = 0;
		static_cast<FGGYGONativeFailureData*>(Data)->FirstFailure.compare_exchange_strong(Expected, 7);
		if (IsInGameThread()) { PublishFailure(Data); }
	}
	// 不绑定替代函数；原生 VM 也会拒绝不完整函数表。
}

void UGGYGONiagaraRuntimeFailureDataInterface::VMReportFailure(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<FGGYGONativeFailureData> Memory(Context);
	FNDIInputParam<float> CodeParam(Context);
	FNDIInputParam<FNiagaraBool> AliveParam(Context);
	FNDIOutputParam<FNiagaraBool> KeepAlive(Context);
	for (int32 Index = 0; Index < Context.GetNumInstances(); ++Index)
	{
		const float Value = CodeParam.GetAndAdvance();
		const bool bAlive = AliveParam.GetAndAdvance();
		const bool bFailed = bAlive && Value != 0.f;
		if (bFailed)
		{
			const int32 Code = FMath::IsFinite(Value) && Value >= 1.f && Value <= 4.f
				&& FMath::FloorToFloat(Value) == Value ? static_cast<int32>(Value) : 5;
			int32 Expected = 0;
			Memory->FirstFailure.compare_exchange_strong(Expected, Code);
		}
		// 原 ParticleState 已退休的粒子不重新激活，也不凭退休后的无效帧报错。
		KeepAlive.SetAndAdvance(bAlive && !bFailed);
	}
}

bool UGGYGONiagaraRuntimeFailureDataInterface::Equals(const UNiagaraDataInterface* Other) const
{
	return Super::Equals(Other) && SourceReference == CastChecked<ThisClass>(Other)->SourceReference;
}

bool UGGYGONiagaraRuntimeFailureDataInterface::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination)) { return false; }
	CastChecked<ThisClass>(Destination)->SourceReference = SourceReference;
	return true;
}

bool UGGYGOFXNiagaraComponent::BindRuntimeFailureInstance(FNiagaraSystemInstance* Instance, FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();
	if (!Instance || !bRuntimeFailureObservationOpen || !IsRegistered()
		|| Instance->GetAttachComponent() != this || Instance->GetSystem() != GetAsset()
		|| Instance->GetWorld() != GetWorld())
	{ OutError = TEXT("运行失败接口不属于活动原组件/World/System作用域。"); return false; }
	if (!EffectFailure.IsEmpty()) { OutError = EffectFailure; return false; }
	if (RuntimeFailureSystem.Get() != GetAsset())
	{ OutError = TEXT("原Activate保存的System与当前原生初始化System不同。"); return false; }
	const auto Controller = GetSystemInstanceController();
	if (!Controller.IsValid() || !Controller->IsValid() || Controller->GetSystemInstanceID() != Instance->GetId())
	{ OutError = TEXT("运行失败接口的原生实例不是当前Controller。"); return false; }
	// 原生重置可以重建 DI；身份只取当前真实 Controller，不接纳旧延后回调。
	RuntimeFailureInstanceId = Instance->GetId();
	return true;
}

void UGGYGOFXNiagaraComponent::LatchRuntimeInitializationFailure(
	FNiagaraSystemInstance* Instance, const FString& Source, const FString& Reason)
{
	check(IsInGameThread());
	const auto Current = GetSystemInstanceController();
	if (!Instance || !bRuntimeFailureObservationOpen || !EffectFailure.IsEmpty()
		|| Instance->GetAttachComponent() != this || Instance->GetWorld() != GetWorld()
		|| Instance->GetSystem() != GetAsset() || !Current.IsValid() || !Current->IsValid()
		|| Current->GetSystemInstanceID() != Instance->GetId()) { return; }
	QueueRuntimeFailure(Instance->GetId(), FString::Printf(
		TEXT("Niagara原实例初始化失败 System='%s' Instance=%llu Source='%s'：%s"),
		*GetPathNameSafe(GetAsset()), Instance->GetId(), *Source, *Reason));
}

void UGGYGOFXNiagaraComponent::LatchRuntimeFailure(uint64 InstanceId, const FString& Source, int32 Code)
{
	check(IsInGameThread());
	const auto Controller = GetSystemInstanceController();
	if (!bRuntimeFailureObservationOpen || !IsRegistered() || !EffectFailure.IsEmpty()
		|| RuntimeFailureInstanceId != InstanceId || RuntimeFailureSystem.Get() != GetAsset()
		|| !Controller.IsValid() || !Controller->IsValid() || Controller->GetSystemInstanceID() != InstanceId) { return; }
	const TCHAR* Detail = Code == 1 ? TEXT("Root缩放奇异") : Code == 2 ? TEXT("node参考帧退化")
		: Code == 3 ? TEXT("必需旅行速度为零") : Code == 4 ? TEXT("旅行方向与up平行")
		: Code == 6 ? TEXT("必需SourceReference缺失") : Code == 7 ? TEXT("CPU VM必需函数或参数ABI无效")
		: TEXT("模拟提交了无效错误码");
	QueueRuntimeFailure(InstanceId, FString::Printf(TEXT("Niagara模拟失败 System='%s' Instance=%llu Source='%s' Code=%d：%s"),
		*GetPathNameSafe(GetAsset()), InstanceId, *Source, Code, Detail));
}

void UGGYGOFXNiagaraComponent::ReportFXRendererFailure(UNiagaraSystem* OriginalSystem, uint64 OriginalInstanceId,
	const FString& SourceReference, const FString& Reason)
{
	check(IsInGameThread());
	const auto Controller = GetSystemInstanceController();
	if (!bRuntimeFailureObservationOpen || !IsRegistered() || !EffectFailure.IsEmpty()
		|| !OriginalSystem || OriginalSystem != GetAsset() || RuntimeFailureSystem.Get() != OriginalSystem
		|| !GetWorld() || GetWorld()->bIsTearingDown || OriginalInstanceId == 0
		|| !Controller.IsValid() || !Controller->IsValid() || Controller->GetSystemInstanceID() != OriginalInstanceId)
	{ return; }
	QueueRuntimeFailure(OriginalInstanceId, FString::Printf(
		TEXT("Niagara渲染失败 System='%s' Instance=%llu Source='%s'：%s"),
		*GetPathNameSafe(OriginalSystem), OriginalInstanceId,
		SourceReference.IsEmpty() ? TEXT("缺准确来源") : *SourceReference,
		Reason.IsEmpty() ? TEXT("渲染消费者未提供失败原因") : *Reason));
}

void UGGYGOFXNiagaraComponent::QueueRuntimeFailure(uint64 InstanceId, const FString& Reason)
{
	if (!EffectFailure.IsEmpty()) { return; }
	EffectFailure = Reason;
	RuntimeFailureInstanceId = InstanceId;
	RuntimeFailureSystem = GetAsset();
	const TWeakObjectPtr<ThisClass> Original(this);
	FNiagaraWorldManager::EnqueueGlobalDeferredCallback([Original, InstanceId]
	{
		ThisClass* Component = Original.Get();
		if (!Component || !Component->bRuntimeFailureObservationOpen || Component->RuntimeFailureInstanceId != InstanceId) { return; }
		const auto Current = Component->GetSystemInstanceController();
		if (Current.IsValid() && Current->IsValid() && Current->GetSystemInstanceID() == InstanceId
			&& Component->RuntimeFailureSystem.Get() == Component->GetAsset())
		{ Component->DispatchEffectFailure(); }
	});
}

void UGGYGOFXNiagaraComponent::HandleOriginalSystemFinished(UNiagaraComponent* Component)
{
	if (Component != this) { return; }
	bRuntimeFailureObservationOpen = false;
	AdditionalWPOBoundsRadiusCM = 0.0f;
	ReleasePointLightSource();
	ReleaseSceneMaterials();
	if (!EffectFailure.IsEmpty()) { DispatchEffectFailure(); }
}

bool UGGYGOFXNiagaraComponent::RegisterPointLightSource(FString& OutError)
{
	UWorld* const World = GetWorld();
	UNiagaraSystem* const System = GetAsset();
	if (!IsValid(World) || World->bIsTearingDown || !IsValid(System))
	{ OutError = TEXT("Point灯来源缺原活动World/System"); return false; }
	if (World->IsNetMode(NM_DedicatedServer) || !GGYGOFXNiagaraLightSource::HasPointLightRenderer(*System))
	{ ReleasePointLightSource(); return true; }
	const auto Controller = GetSystemInstanceController();
	if (!Controller.IsValid() || !Controller->IsValid())
	{ OutError = TEXT("Point灯来源尚无原Controller"); return false; }
	const uint64 InstanceId = static_cast<uint64>(Controller->GetSystemInstanceID());
	if (PointLightSourceHandle && PointLightInstanceId == InstanceId && PointLightSystem == System
		&& PointLightSources.IsValid() && PointLightSources->GetWorld() == World) { return true; }
	ReleasePointLightSource();
	UGGYGOCharacterLightSources* Sources = World->GetSubsystem<UGGYGOCharacterLightSources>();
	if (!IsValid(Sources)) { OutError = TEXT("原World无Point灯来源接口"); return false; }
	const TWeakObjectPtr<ThisClass> Original(this);
	uint64 Handle = 0;
	if (!Sources->RegisterSource(this, InstanceId, FGGYGOReadCharacterPointLights::CreateUObject(
		this, &ThisClass::ReadPointLightFrame), Handle, OutError)) { return false; }
	if (!Original.IsValid() || !IsRegistered() || GetWorld() != World || World->bIsTearingDown
		|| GetAsset() != System || GetSystemInstanceController() != Controller || !Controller->IsValid()
		|| static_cast<uint64>(Controller->GetSystemInstanceID()) != InstanceId)
	{
		Sources->UnregisterSource(Handle);
		OutError = TEXT("Point灯登记期间原World/Controller/实例已改变");
		return false;
	}
	PointLightSources = Sources;
	PointLightSystem = System;
	PointLightInstanceId = InstanceId;
	PointLightSourceHandle = Handle;
	return true;
}

EGGYGOCharacterLightSourceReadiness UGGYGOFXNiagaraComponent::ReadPointLightFrame(
	FGGYGOCharacterPointLightFrame& OutFrame, FString& OutError)
{
	check(IsInGameThread());
	OutFrame = {};
	OutError.Reset();
	if (bReadingPointLights)
	{ OutError = TEXT("原Niagara Finalize回调中的同来源重入，等待原模拟栈归还"); return EGGYGOCharacterLightSourceReadiness::WaitingForSimulation; }
	const TWeakObjectPtr<ThisClass> Original(this);
	const auto Controller = GetSystemInstanceController();
	const TWeakObjectPtr<UNiagaraSystem> System(PointLightSystem);
	const TWeakObjectPtr<UWorld> OriginalWorld(GetWorld());
	const uint64 Handle = PointLightSourceHandle;
	const uint64 InstanceId = PointLightInstanceId;
	const auto IsOriginal = [&]()
	{
		const ThisClass* Component = Original.Get();
		const UWorld* World = OriginalWorld.Get();
		return Component && Component->IsRegistered() && World && !World->bIsTearingDown && System.IsValid()
			&& Component->GetWorld() == World && Component->GetAsset() == System.Get()
			&& Component->PointLightSourceHandle == Handle && Component->PointLightInstanceId == InstanceId
			&& Component->GetSystemInstanceController() == Controller && Controller.IsValid() && Controller->IsValid()
			&& static_cast<uint64>(Controller->GetSystemInstanceID()) == InstanceId;
	};
	if (!Handle || !InstanceId || !IsOriginal())
	{ OutError = TEXT("原Point灯来源/Controller身份已归还或改变"); return EGGYGOCharacterLightSourceReadiness::Invalid; }
	if (!EffectFailure.IsEmpty()) { OutError = EffectFailure; return EGGYGOCharacterLightSourceReadiness::Invalid; }
	if (!GetRenderingEnabled())
	{
		const double Age = Controller->GetAge();
		if (!FMath::IsFinite(Age) || Age < 0)
		{
			OutError = TEXT("显式未渲染的原Point灯实例年龄无效");
			QueueRuntimeFailure(InstanceId, OutError);
			return EGGYGOCharacterLightSourceReadiness::Invalid;
		}
		OutFrame.InstanceId = InstanceId;
		OutFrame.SimulationAgeSeconds = Age;
		return EGGYGOCharacterLightSourceReadiness::Ready; // 远端seek隐藏期确证不绘，不提前照亮角色。
	}
	bReadingPointLights = true;
	ON_SCOPE_EXIT { if (ThisClass* Component = Original.Get()) { Component->bReadingPointLights = false; } };
	// 仅收束原已排工作，不推进时间。Finalize可能Complete/回调/销毁，之后必须重新核原身份。
	Controller->WaitForConcurrentTickAndFinalize();
	if (!IsOriginal())
	{ OutError = TEXT("原Point灯实例在Native Finalize中结束或换代"); return EGGYGOCharacterLightSourceReadiness::WaitingForSimulation; }
	if (!EffectFailure.IsEmpty()) { OutError = EffectFailure; return EGGYGOCharacterLightSourceReadiness::Invalid; }
	FNiagaraSystemInstance* Instance = Controller->GetSystemInstance_Unsafe();
	if (!Instance || static_cast<uint64>(Instance->GetId()) != InstanceId)
	{ OutError = TEXT("原Point灯CPU实例不可用"); return EGGYGOCharacterLightSourceReadiness::Invalid; }
	const auto Readiness = GGYGOFXNiagaraLightSource::ReadCompletedInstance(*Instance, GetComponentTransform(), OutFrame, OutError);
	if (Readiness == EGGYGOCharacterLightSourceReadiness::Invalid)
	{
		const FString Reason = FString::Printf(TEXT("FX.PointLightSource System='%s' Instance=%llu：%s"),
			*GetPathNameSafe(System.Get()), InstanceId, *OutError);
		QueueRuntimeFailure(InstanceId, Reason); // 先锁存原首错，沿原所有者延后结束；不发布空灯成功。
	}
	return Readiness;
}

void UGGYGOFXNiagaraComponent::ReleasePointLightSource()
{
	UGGYGOCharacterLightSources* Sources = PointLightSources.Get();
	const uint64 Handle = PointLightSourceHandle;
	PointLightSourceHandle = 0;
	PointLightInstanceId = 0;
	PointLightSystem.Reset();
	PointLightSources.Reset();
	if (IsValid(Sources) && Handle) { Sources->UnregisterSource(Handle); }
}

bool FGGYGOFXFieldSettings::ValidateConfiguration(FString& OutError) const
{
	OutError.Reset();
	if (Mode == EGGYGOFXFieldMode::NoCustomField)
	{
		if (Profile || !FieldMaterials.IsEmpty() || !MaterialContractEvidence.TrimStartAndEnd().IsEmpty())
		{ OutError = TEXT("NoCustomField 不接受残留 Profile、field 材质或契约。"); return false; }
		return true;
	}
	if (Mode != EGGYGOFXFieldMode::DualSignedField || !IsValid(Profile)
		|| FieldMaterials.IsEmpty() || MaterialContractEvidence.TrimStartAndEnd().IsEmpty())
	{ OutError = TEXT("DualSignedField 必须有明确 Profile、field 材质及契约。"); return false; }
	TSet<UMaterialInterface*> Unique;
	for (UMaterialInterface* Material : FieldMaterials)
	{
		if (!IsValid(Material) || Unique.Contains(Material))
		{ OutError = TEXT("field 材质必须有效且不能重复。"); return false; }
		Unique.Add(Material);
	}
	return Profile->ValidateConfiguration(OutError);
}

bool FGGYGOFXFieldSettings::HasSameValue(const FGGYGOFXFieldSettings& Other) const
{
	return Mode == Other.Mode && Profile == Other.Profile && FieldMaterials == Other.FieldMaterials
		&& MaterialContractEvidence == Other.MaterialContractEvidence;
}

UGGYGOFXNiagaraComponent::UGGYGOFXNiagaraComponent()
{
	bAutoActivate = false;
	bWantsOnUpdateTransform = true;
	OnSystemFinished.AddDynamic(this, &ThisClass::HandleOriginalSystemFinished);
}

bool UGGYGOFXNiagaraComponent::ConfigureField(const FGGYGOFXFieldSettings& Settings, FString& OutError)
{
	if (!Settings.ValidateConfiguration(OutError)) { return false; }
#if !GGYGO_WITH_DISTORTION_FIELD
	if (Settings.Mode == EGGYGOFXFieldMode::DualSignedField)
	{
		const UWorld* World = GetWorld();
		if (IsValid(World) && !World->IsNetMode(NM_DedicatedServer))
		{
			OutError = TEXT("本构建 Field 显式关闭，图形 World 的 DualSignedField 请求被拒绝。");
			return false;
		}
		// 尚无 World 时只暂存配置；真正激活仍须再次核能力及原 World。
	}
#endif
	if (bFieldSettingsConfigured)
	{
		if (FieldSettings.HasSameValue(Settings)) { return true; }
		OutError = TEXT("原 Niagara 实例的 field 配置不能被后继请求覆盖。");
		return false;
	}
	if (GetSystemInstanceController().IsValid() || !EffectFailure.IsEmpty())
	{ OutError = TEXT("field 配置须在原 Niagara 激活前明确提交。"); return false; }
	FieldSettings = Settings;
	bFieldSettingsConfigured = true;
	return true;
}

bool UGGYGOFXNiagaraComponent::ConfigureSceneLightingSource(AActor* Avatar, FString& OutError)
{
	OutError.Reset();
	if (!IsValid(Avatar) || Avatar->IsActorBeingDestroyed() || Avatar->GetWorld() != GetWorld())
	{ OutError = TEXT("FX场景灯缺同World原Avatar来源"); return false; }
	if (bSceneLightingSourceConfigured)
	{
		if (SceneLightingAvatar.Get() == Avatar) { return true; }
		OutError = TEXT("原FX的场景灯Avatar来源不能被后继请求覆盖"); return false;
	}
	if (SceneMaterials.IsValid() || GetSystemInstanceController().IsValid())
	{ OutError = TEXT("场景灯来源须在原NS激活前配置"); return false; }
	SceneLightingAvatar = Avatar;
	bSceneLightingSourceConfigured = true;
	return true;
}

bool UGGYGOFXNiagaraComponent::IsSceneMaterialStateCurrent(const TSharedPtr<FGGYGOFXSceneMaterialState>& State) const
{
	return IsValid(this) && State.IsValid() && SceneMaterials == State && State->System.IsValid()
		&& GetAsset() == State->System.Get() && State->World.IsValid() && GetWorld() == State->World.Get()
		&& !State->World->bIsTearingDown && IsRegistered();
}

bool UGGYGOFXNiagaraComponent::AreSceneMaterialsApplied() const
{
	return EffectFailure.IsEmpty() && (!SceneMaterials.IsValid()
		|| (SceneMaterials->HostHandle != 0 && SceneMaterials->bInstalled && !SceneMaterials->bRenderingSuppressed));
}

void UGGYGOFXNiagaraComponent::ObserveSceneMaterialReadiness(const TSharedPtr<FGGYGOFXSceneMaterialState>& State)
{
	if (State->ReadinessSubscription.IsValid() || !State->Host.IsValid()) { return; }
	State->ReadinessSubscription = State->Host->OnSceneLightingReadinessChanged().AddUObject(this,
		&ThisClass::HandleSceneMaterialReadiness, TWeakPtr<FGGYGOFXSceneMaterialState>(State));
}

void UGGYGOFXNiagaraComponent::TryRegisterSceneMaterials(TSharedPtr<FGGYGOFXSceneMaterialState> State)
{
	if (!IsSceneMaterialStateCurrent(State) || State->HostHandle || State->bRegistering) { return; }
	UGGYGOCharacterRenderComponent* const Host = State->Host.Get();
	if (!IsValid(Host) || Host->GetWorld() != GetWorld() || !SceneLightingAvatar.IsValid()
		|| Host->GetOwner() != SceneLightingAvatar.Get())
	{ RejectEffect(TEXT("FX场景材质原Avatar/灯宿主已失效")); return; }
	ObserveSceneMaterialReadiness(State);
	TArray<UMaterialInstanceDynamic*> Materials;
	for (const auto& Binding : State->Materials) { Materials.Add(Binding.InstalledMaterial.Get()); }
	const TWeakObjectPtr<ThisClass> Self(this);
	const TWeakObjectPtr<UGGYGOCharacterRenderComponent> OriginalHost(Host);
	uint64 Handle = 0;
	FString Error;
	State->bRegistering = true;
	const auto Readiness = Host->RegisterSceneMaterialTargets(this, Materials,
		UGGYGOCharacterRenderComponent::FSceneMaterialsInvalidated::CreateUObject(this,
			&ThisClass::HandleSceneMaterialsInvalidated, TWeakPtr<FGGYGOFXSceneMaterialState>(State)), Handle, Error);
	State->bRegistering = false;
	if (!Self.IsValid() || !IsSceneMaterialStateCurrent(State) || State->Host != OriginalHost)
	{
		if (Handle && OriginalHost.IsValid()) { OriginalHost->UnregisterSceneMaterialTargets(Handle); }
		return;
	}
	if (Readiness == EGGYGOCharacterLightingReadiness::WaitingForRegistration
		|| Readiness == EGGYGOCharacterLightingReadiness::SourceFailed) { return; }
	if (Readiness != EGGYGOCharacterLightingReadiness::Ready || !Handle)
	{ RejectEffect(FString::Printf(TEXT("FX场景MID真实灯登记失败：%s"), *Error)); return; }
	State->HostHandle = Handle;
	Host->OnSceneLightingReadinessChanged().Remove(State->ReadinessSubscription);
	State->ReadinessSubscription.Reset();
	const auto IsOriginal = [&]() { return Self.IsValid() && IsSceneMaterialStateCurrent(State); };
	if (!State->bInstalled && !GGYGOFXNiagaraSceneMaterials::Install(*this, *State, IsOriginal, Error))
	{ if (IsOriginal()) { RejectEffect(Error); } return; }
	if (State->bActivationPending)
	{
		const bool bReset = State->bResetWhenReady;
		State->bActivationPending = false;
		StartNativeEffect(bReset);
		return;
	}
	if (State->bRenderingSuppressed)
	{
		State->bRenderingSuppressed = false;
		SetRenderingEnabled(State->bOriginalRendering);
	}
}

void UGGYGOFXNiagaraComponent::HandleSceneMaterialReadiness(EGGYGOCharacterLightingReadiness Readiness,
	const FString& Reason, TWeakPtr<FGGYGOFXSceneMaterialState> WeakState)
{
	const auto State = WeakState.Pin();
	if (!IsSceneMaterialStateCurrent(State) || State->HostHandle) { return; }
	if (Readiness == EGGYGOCharacterLightingReadiness::Ready) { TryRegisterSceneMaterials(State); }
	else if (Readiness == EGGYGOCharacterLightingReadiness::Invalid)
	{ RejectEffect(FString::Printf(TEXT("FX原场景灯宿主拒绝：%s"), *Reason)); }
}

void UGGYGOFXNiagaraComponent::HandleSceneMaterialsInvalidated(uint64 Handle,
	EGGYGOCharacterLightingReadiness Readiness, const FString& Reason, TWeakPtr<FGGYGOFXSceneMaterialState> WeakState)
{
	const auto State = WeakState.Pin();
	if (!IsSceneMaterialStateCurrent(State) || !Handle || State->HostHandle != Handle) { return; }
	State->HostHandle = 0; // 宿主已先撤登记；旧handle不能清后继登记。
	if (Readiness != EGGYGOCharacterLightingReadiness::WaitingForRegistration
		&& Readiness != EGGYGOCharacterLightingReadiness::SourceFailed)
	{ RejectEffect(FString::Printf(TEXT("FX场景MID登记失效：%s"), *Reason)); return; }
	ObserveSceneMaterialReadiness(State);
	if (!State->bRenderingSuppressed)
	{
		State->bRenderingSuppressed = true;
		State->bOriginalRendering = GetRenderingEnabled();
		SetRenderingEnabled(false);
	}
}

void UGGYGOFXNiagaraComponent::ReleaseSceneMaterials()
{
	const auto State = MoveTemp(SceneMaterials);
	SceneMaterials.Reset();
	if (!State.IsValid()) { return; }
	const TWeakObjectPtr<ThisClass> Self(this);
	const auto OriginalController = GetSystemInstanceController();
	const uint64 Handle = State->HostHandle;
	State->HostHandle = 0;
	if (UGGYGOCharacterRenderComponent* Host = State->Host.Get())
	{
		Host->OnSceneLightingReadinessChanged().Remove(State->ReadinessSubscription);
		State->ReadinessSubscription.Reset();
		if (Handle) { Host->UnregisterSceneMaterialTargets(Handle); }
	}
	GGYGOFXNiagaraSceneMaterials::Restore(*this, *State, [&]()
	{
		return Self.IsValid() && !SceneMaterials.IsValid() && GetAsset() == State->System.Get()
			&& GetWorld() == State->World.Get() && GetSystemInstanceController() == OriginalController;
	});
}

void UGGYGOFXNiagaraComponent::Activate(bool bReset)
{
	if (!EffectFailure.IsEmpty()) { return; }
	if (!bFieldSettingsConfigured)
	{ RejectEffect(TEXT("原 Niagara 缺显式 field 模式配置。")); return; }
#if !GGYGO_WITH_DISTORTION_FIELD
	if (FieldSettings.Mode == EGGYGOFXFieldMode::DualSignedField)
	{
		const UWorld* World = GetWorld();
		if (!IsValid(World) || World->bIsTearingDown || !World->IsNetMode(NM_DedicatedServer))
		{
			RejectEffect(TEXT("本构建 Field 显式关闭，DualSignedField 不能启动图形模拟或场登记。"));
			return;
		}
	}
#endif
	if (SceneMaterials.IsValid())
	{
		if (!IsSceneMaterialStateCurrent(SceneMaterials))
		{ RejectEffect(TEXT("再次激活时原场景材质NS/World已改变")); return; }
		if (SceneMaterials->HostHandle) { StartNativeEffect(bReset); }
		else
		{
			SceneMaterials->bActivationPending = true;
			SceneMaterials->bResetWhenReady |= bReset;
			TryRegisterSceneMaterials(SceneMaterials);
		}
		return;
	}
	FString Error;
	TSharedPtr<FGGYGOFXSceneMaterialState> Prepared;
	const auto Preparation = GGYGOFXNiagaraSceneMaterials::Prepare(*this, Prepared, Error);
	if (Preparation == EGGYGOFXSceneMaterialPreparation::Invalid) { RejectEffect(Error); return; }
	if (Preparation == EGGYGOFXSceneMaterialPreparation::NotRequired) { StartNativeEffect(bReset); return; }
	if (!bSceneLightingSourceConfigured || !SceneLightingAvatar.IsValid()
		|| SceneLightingAvatar->GetWorld() != GetWorld())
	{ RejectEffect(TEXT("需要场景灯的FX缺明确原Avatar来源")); return; }
	TArray<UGGYGOCharacterRenderComponent*> Hosts;
	SceneLightingAvatar->GetComponents(Hosts);
	if (Hosts.Num() != 1 || !IsValid(Hosts[0]))
	{ RejectEffect(TEXT("FX原Avatar需要唯一的角色灯宿主组件")); return; }
	Prepared->Host = Hosts[0];
	Prepared->bActivationPending = true;
	Prepared->bResetWhenReady = bReset;
	SceneMaterials = Prepared;
	TryRegisterSceneMaterials(Prepared);
}

void UGGYGOFXNiagaraComponent::StartNativeEffect(bool bReset)
{
	float RadiusCM = 0.0f;
	FString Error;
	if (!ReadAdditionalWPOBoundsRadius(RadiusCM, Error)) { RejectEffect(Error); return; }
	if (bReset) { ReleasePointLightSource(); }
	const auto OriginalScene = SceneMaterials;
	RuntimeFailureSystem = GetAsset();
	bRuntimeFailureObservationOpen = true;
	AdditionalWPOBoundsRadiusCM = RadiusCM;
	const TWeakObjectPtr<ThisClass> Original(this);
	const TWeakObjectPtr<UNiagaraSystem> OriginalSystem(GetAsset());
	Super::Activate(bReset);
	if (!Original.IsValid() || !IsRegistered() || GetAsset() != OriginalSystem.Get()
		|| SceneMaterials != OriginalScene) { return; }
	if (!EffectFailure.IsEmpty()) { DispatchEffectFailure(); return; }
	// 只有原生尚未建立 Controller 的暂态交还 deferred Activate；不新增轮询。
	if (!GetSystemInstanceController().IsValid()) { return; }
	const auto OriginalController = GetSystemInstanceController();
	if (OriginalScene.IsValid())
	{
		OriginalScene->Controller = OriginalController;
		OriginalScene->InstanceId = OriginalController->GetSystemInstanceID();
		if (!GGYGOFXNiagaraSceneMaterials::UpdateBounds(*this, *OriginalScene, [&]()
		{
			return Original.IsValid() && IsSceneMaterialStateCurrent(OriginalScene)
				&& GetSystemInstanceController() == OriginalController
				&& OriginalController->GetSystemInstanceID() == OriginalScene->InstanceId;
		}, Error))
		{ if (Original.IsValid() && IsSceneMaterialStateCurrent(OriginalScene)) { RejectEffect(Error); } return; }
	}
	if (!RegisterPointLightSource(Error))
	{
		if (Original.IsValid() && GetAsset() == OriginalSystem.Get() && GetSystemInstanceController() == OriginalController) { RejectEffect(Error); }
		return;
	}
	if (!RefreshFieldBinding(Error) && Original.IsValid()) { RejectEffect(Error); }
}

bool UGGYGOFXNiagaraComponent::ReadAdditionalWPOBoundsRadius(float& OutRadiusCM, FString& OutError) const
{
	const UNiagaraSystem* System = GetAsset();
	if (!IsValid(System)) { OutError = TEXT("WPO包围范围缺原System"); return false; }
	bool bRequiresAdditionalBounds = false;
	float RequiredRadiusCM = 0.0f;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Handle.GetIsEnabled() || !Data) { continue; }
		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			const auto* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderer);
			if (!IsValid(Mesh) || !Mesh->GetIsEnabled()) { continue; }
			TArray<UMaterialInterface*> Materials;
			if (Mesh->bOverrideMaterials)
			{
				for (const FNiagaraMeshMaterialOverride& Slot : Mesh->OverrideMaterials)
				{
					UMaterialInterface* Material = Slot.ExplicitMat;
					const FNiagaraVariable& Variable = Slot.UserParamBinding.Parameter;
					if (Variable.IsValid())
					{
						if (Variable.GetType() != FNiagaraTypeDefinition::GetUMaterialDef())
						{ OutError = TEXT("WPO能力核对遇到无效UserMaterial类型"); return false; }
						const FNiagaraParameterStore& Overrides = GetOverrideParameters();
						Material = Cast<UMaterialInterface>(Overrides.IndexOf(Variable) != INDEX_NONE
							? Overrides.GetUObject(Variable).Get() : System->GetExposedParameters().GetUObject(Variable).Get());
					}
					Materials.Add(Material);
				}
			}
			else { Mesh->GetUsedMaterials(nullptr, Materials); }
			for (UMaterialInterface* Material : Materials)
			{
				float Mode = 0.0f;
				if (!IsValid(Material) || !Material->GetScalarParameterValue(SourceVertexWPOParameter, Mode)) { continue; }
				// 该参数在新VS图内实际乘源外扩，只有明确开启的配置能消费本能力。
				if (Mode != 1.0f) { OutError = TEXT("原有效材质的UEFX_SourceVertexWPO必须明确为1"); return false; }
				float MaterialRadiusCM = 0.0f;
				if (!Material->GetScalarParameterValue(SourceVertexWPOBoundsParameter, MaterialRadiusCM)
					|| !FMath::IsFinite(MaterialRadiusCM) || MaterialRadiusCM <= 0.0f)
				{ OutError = TEXT("原有效材质缺真实顶点WPO的有限正半径"); return false; }
				RequiredRadiusCM = FMath::Max(RequiredRadiusCM, MaterialRadiusCM);
				bRequiresAdditionalBounds = true;
			}
		}
	}
	const FNiagaraVariable Parameter(FNiagaraTypeDefinition::GetFloatDef(), AdditionalWPOBoundsRadiusName);
	const FNiagaraUserRedirectionParameterStore& Parameters = System->GetExposedParameters();
	if (Parameters.IndexOf(Parameter) == INDEX_NONE)
	{
		const bool bWrongType = Parameters.ReadParameterVariables().ContainsByPredicate([](const FNiagaraVariableWithOffset& Variable)
		{ return Variable.GetName() == AdditionalWPOBoundsRadiusName; });
		if (bRequiresAdditionalBounds || bWrongType)
		{ OutError = TEXT("源顶点WPO的原System缺正确Float User.UEFX_AdditionalWPOBoundsRadiusCM"); return false; }
		// 既有材质未声明额外WPO能力，沿原Native几何与bounds；没有缺失的必需半径。
		OutRadiusCM = 0.0f;
		return true;
	}
	const float RadiusCM = Parameters.GetParameterValue<float>(Parameter);
	if (!FMath::IsFinite(RadiusCM) || RadiusCM < 0.0f || (bRequiresAdditionalBounds ? RadiusCM < RequiredRadiusCM : RadiusCM != 0.0f))
	{ OutError = TEXT("原System WPO半径须有限，Native普通模式为0，源顶点WPO模式须覆盖原有效材质最大半径"); return false; }
	OutRadiusCM = RadiusCM;
	return true;
}

FBoxSphereBounds UGGYGOFXNiagaraComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const FBoxSphereBounds NativeBounds = Super::CalcBounds(LocalToWorld);
	// 半径已包含源完整矩阵的最大位移；不能再乘组件缩放，也不改变原生中心。
	return AdditionalWPOBoundsRadiusCM > 0.0f && bRuntimeFailureObservationOpen && RuntimeFailureSystem.Get() == GetAsset()
		? NativeBounds.ExpandBy(AdditionalWPOBoundsRadiusCM) : NativeBounds;
}

void UGGYGOFXNiagaraComponent::Deactivate()
{
	// 尚未启动的等待也属于原窗口；取消后移除订阅，不能在后继Ready迟播。
	if (SceneMaterials.IsValid() && (SceneMaterials->bActivationPending || !GetSystemInstanceController().IsValid()))
	{ ReleaseSceneMaterials(); }
	Super::Deactivate();
}

void UGGYGOFXNiagaraComponent::OnUpdateTransform(EUpdateTransformFlags Flags, ETeleportType Teleport)
{
	const TWeakObjectPtr<ThisClass> Self(this);
	const auto State = SceneMaterials;
	Super::OnUpdateTransform(Flags, Teleport);
	if (!Self.IsValid() || !IsSceneMaterialStateCurrent(State) || !State->bInstalled || !State->InstanceId) { return; }
	const auto Controller = State->Controller.Pin();
	if (!Controller.IsValid() || !Controller->IsValid() || GetSystemInstanceController() != Controller
		|| Controller->GetSystemInstanceID() != State->InstanceId) { return; }
	FString Error;
	if (!GGYGOFXNiagaraSceneMaterials::UpdateBounds(*this, *State, [&]()
	{
		return Self.IsValid() && IsSceneMaterialStateCurrent(State) && GetSystemInstanceController() == Controller
			&& Controller->GetSystemInstanceID() == State->InstanceId;
	}, Error) && Self.IsValid() && IsSceneMaterialStateCurrent(State)) { RejectEffect(Error); }
}

bool UGGYGOFXNiagaraComponent::RefreshFieldBinding(FString& OutError)
{
	check(IsInGameThread());
	if (!FieldSettings.ValidateConfiguration(OutError)) { return false; }
	UWorld* const World = GetWorld();
	if (!IsValid(World) || World->bIsTearingDown)
	{ OutError = TEXT("field 资格缺原活动 World。"); return false; }
	if (World->IsNetMode(NM_DedicatedServer)) { return true; }
#if !GGYGO_WITH_DISTORTION_FIELD
	if (FieldSettings.Mode == EGGYGOFXFieldMode::DualSignedField)
	{ OutError = TEXT("本构建 Field 显式关闭，DualSignedField 不能进入图形登记。"); return false; }
	// 显式普通模式没有自定义场登记；材质的合法异步状态沿原 Niagara renderer 处理。
	// 此结果只表示无需登记，不认证 ShaderMap／自定义 VF 就绪，也不改变原材质。
	return true;
#else
	const auto Controller = GetSystemInstanceController();
	const TWeakObjectPtr<ThisClass> Original(this);
	TArray<UMaterialInterface*> Used;
	GetUsedMaterials(Used, false);
	if (Used.IsEmpty())
	{
		if (FieldSettings.Mode == EGGYGOFXFieldMode::NoCustomField && IsValid(GetAsset())
			&& GGYGOFXNiagaraLightSource::HasOnlyPointLightRenderers(*GetAsset())) { return true; }
		OutError = TEXT("原 Controller 尚无实际材质集合；不能假作 NoCustomField。"); return false;
	}
	TSet<UMaterialInterface*> Seen;
	TSet<UMaterialInterface*> ActualFields;
	for (UMaterialInterface* Material : Used)
	{
		if (!IsValid(Material)) { OutError = TEXT("原 Niagara 使用了无效材质。"); return false; }
		if (Seen.Contains(Material)) { continue; }
		Seen.Add(Material);
		FString Reason;
		// 普通模式只核完整声明；真实异步材质状态交原 Niagara。双场仍要求完整编译资格。
		const auto Role = FieldSettings.Mode == EGGYGOFXFieldMode::NoCustomField
			? UGGYGODistortionFieldComponent::QueryMaterialFieldDeclaration(Material, World, Reason)
			: UGGYGODistortionFieldComponent::QueryMaterialFieldRole(Material, World, Reason);
		if (!Original.IsValid() || !IsRegistered() || !IsValid(World) || GetWorld() != World || World->bIsTearingDown
			|| GetSystemInstanceController() != Controller)
		{ OutError = TEXT("查询材质资格时原 World／Controller 已改变。"); return false; }
		if (Role == EGGYGODistortionMaterialRole::DualSignedField) { ActualFields.Add(Material); }
		else if (Role != EGGYGODistortionMaterialRole::NoCustomField)
		{
			OutError = FString::Printf(TEXT("原材质 %s 的 field 声明或编译资格不可用或无效：%s"),
				*GetPathNameSafe(Material), *Reason);
			return false;
		}
	}
	if (FieldSettings.Mode == EGGYGOFXFieldMode::NoCustomField)
	{
		if (!ActualFields.IsEmpty()) { OutError = TEXT("NoCustomField 实例实际使用了双场材质。"); return false; }
		return true;
	}
	if (ActualFields.Num() != FieldSettings.FieldMaterials.Num())
	{ OutError = TEXT("原 Niagara 实际 field 集与显式配置数量不符。"); return false; }
	for (UMaterialInterface* Material : FieldSettings.FieldMaterials)
	{
		if (!ActualFields.Contains(Material))
		{ OutError = TEXT("显式 field 材质不属于原 Niagara 实际 field 集。"); return false; }
	}
	if (IsValid(FieldRegistration))
	{
		if (FieldRegistration->IsRegistered() && FieldRegistration->GetWorld() == World
			&& FieldRegistration->bEnableField) { return true; }
		OutError = TEXT("原 field 伴随件已退役，不能把有效指针当作已登记。");
		return false;
	}
	UGGYGODistortionFieldComponent* const Created = NewObject<UGGYGODistortionFieldComponent>(this, NAME_None, RF_Transient);
	if (!IsValid(Created)) { OutError = TEXT("无法创建原 Niagara 的渲染伴随件。"); return false; }
	FieldRegistration = Created;
	Created->TargetPrimitive.OverrideComponent = this;
	Created->Profile = FieldSettings.Profile;
	Created->FieldMaterials = FieldSettings.FieldMaterials;
	Created->MaterialContractEvidence = FieldSettings.MaterialContractEvidence;
	// OnRegister 的关闭模式不提交 source；注册返栈核原件后只显式 Refresh 一次。
	Created->bEnableField = false;
	Created->RegisterComponentWithWorld(World);
	if (!Original.IsValid() || FieldRegistration != Created || !IsValid(Created)
		|| GetWorld() != World || GetSystemInstanceController() != Controller)
	{ OutError = TEXT("注册伴随件时原 Niagara／Controller 已退役。"); return false; }
	Created->bEnableField = true;
	const bool bFieldRegistered = Created->RefreshFieldRegistration();
	if (!Original.IsValid() || FieldRegistration != Created || !IsValid(Created)
		|| GetWorld() != World || GetSystemInstanceController() != Controller)
	{ OutError = TEXT("登记 field 时原 Niagara／Controller 已退役。"); return false; }
	if (!bFieldRegistered) { OutError = TEXT("原 Niagara 的 field source 被渲染登记拒绝。"); }
	return bFieldRegistered;
#endif
}

void UGGYGOFXNiagaraComponent::ReleaseFieldRegistration()
{
	TStrongObjectPtr<UGGYGODistortionFieldComponent> Original(FieldRegistration);
	FieldRegistration = nullptr;
	if (IsValid(Original.Get()))
	{
		Original->bEnableField = false;
		Original->DestroyComponent();
	}
}

void UGGYGOFXNiagaraComponent::RejectEffect(const FString& Reason)
{
	if (EffectFailure.IsEmpty())
	{ EffectFailure = Reason.IsEmpty() ? TEXT("原 Niagara 被拒绝但未提供失败原因。") : Reason; }
	DispatchEffectFailure();
}

void UGGYGOFXNiagaraComponent::DispatchEffectFailure()
{
	if (EffectFailure.IsEmpty() || bEffectFailureDispatched) { return; }
	bEffectFailureDispatched = true;
	const FString OriginalReason = EffectFailure;
	const TWeakObjectPtr<ThisClass> Original(this);
	const TWeakObjectPtr<UNiagaraSystem> OriginalSystem(GetAsset());
	const auto OriginalController = GetSystemInstanceController();
	const uint64 OriginalId = OriginalController.IsValid() && OriginalController->IsValid()
		? OriginalController->GetSystemInstanceID() : 0;
	const auto IsOriginal = [&]()
	{
		const ThisClass* Component = Original.Get();
		if (!Component || Component->GetAsset() != OriginalSystem.Get()) { return false; }
		const auto Current = Component->GetSystemInstanceController();
		const uint64 CurrentId = Current.IsValid() && Current->IsValid() ? Current->GetSystemInstanceID() : 0;
		return Current == OriginalController && CurrentId == OriginalId;
	};
	ReleasePointLightSource();
	ReleaseSceneMaterials();
	ReleaseFieldRegistration();
	if (!IsOriginal()) { return; }
	SetRenderingEnabled(false);
	UE_LOG(LogGGYGOFXNiagara, Error, TEXT("%s：%s"), *GetPathName(), *OriginalReason);
	OnEffectFailed.Broadcast(this, OriginalReason);
	if (IsOriginal()) { DestroyComponent(); }
}

void UGGYGOFXNiagaraComponent::OnUnregister()
{
	bRuntimeFailureObservationOpen = false;
	AdditionalWPOBoundsRadiusCM = 0.0f;
	ReleasePointLightSource();
	ReleaseSceneMaterials();
	ReleaseFieldRegistration();
	Super::OnUnregister();
}

void UGGYGOFXNiagaraComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	bRuntimeFailureObservationOpen = false;
	AdditionalWPOBoundsRadiusCM = 0.0f;
	ReleasePointLightSource();
	ReleaseSceneMaterials();
	ReleaseFieldRegistration();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}
