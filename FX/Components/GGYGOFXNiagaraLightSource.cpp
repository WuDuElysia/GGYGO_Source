/** @file GGYGOFXNiagaraLightSource.cpp @brief 按Native LightRenderer实际绑定派生同一CPU实例的当前可绘灯。 */
#include "FX/Components/GGYGOFXNiagaraLightSource.h"

#include "NiagaraDataSetAccessor.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"

namespace
{
	template<typename T>
	bool ReadBoundValue(const FNiagaraEmitterInstance& Emitter,
		const FNiagaraVariableAttributeBinding& Binding, T& OutValue)
	{
		const FNiagaraVariableBase& Variable = Binding.GetParamMapBindableVariable();
		const uint8* Data = Variable.IsValid() ? Emitter.GetRendererBoundVariables().GetParameterData(Variable) : nullptr;
		if (!Data || Variable.GetSizeInBytes() != static_cast<int32>(sizeof(T))) { return false; }
		FMemory::Memcpy(&OutValue, Data, sizeof(T));
		return true;
	}

	uint64 StableLightKey(uint64 InstanceId, const FGuid& EmitterId, int32 RendererIndex, bool bEmitterSource, int32 UniqueId)
	{
		uint64 Key = 14695981039346656037ull;
		const auto AddWord = [&Key](uint32 Word)
		{
			for (uint32 Shift = 0; Shift < 32; Shift += 8) { Key ^= (Word >> Shift) & 255u; Key *= 1099511628211ull; }
		};
		AddWord(static_cast<uint32>(InstanceId)); AddWord(static_cast<uint32>(InstanceId >> 32));
		AddWord(EmitterId.A); AddWord(EmitterId.B); AddWord(EmitterId.C); AddWord(EmitterId.D);
		AddWord(static_cast<uint32>(RendererIndex)); AddWord(bEmitterSource ? 1u : 0u);
		AddWord(static_cast<uint32>(UniqueId));
		return Key;
	}

	bool FiniteColor(const FLinearColor& Color)
	{
		return FMath::IsFinite(Color.R) && FMath::IsFinite(Color.G) && FMath::IsFinite(Color.B) && FMath::IsFinite(Color.A);
	}
}

bool GGYGOFXNiagaraLightSource::HasPointLightRenderer(const UNiagaraSystem& System)
{
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Handle.GetIsEnabled() || !Data) { continue; }
		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (IsValid(Renderer) && Renderer->GetIsEnabled() && Renderer->IsA<UNiagaraLightRendererProperties>()) { return true; }
		}
	}
	return false;
}

bool GGYGOFXNiagaraLightSource::HasOnlyPointLightRenderers(const UNiagaraSystem& System)
{
	bool bFound = false;
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Handle.GetIsEnabled()) { continue; }
		if (!Data) { return false; }
		for (const UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (!IsValid(Renderer)) { return false; }
			if (!Renderer->GetIsEnabled()) { continue; }
			if (Renderer->GetClass() != UNiagaraLightRendererProperties::StaticClass()) { return false; }
			bFound = true;
		}
	}
	return bFound;
}

EGGYGOCharacterLightSourceReadiness GGYGOFXNiagaraLightSource::ReadCompletedInstance(FNiagaraSystemInstance& Instance,
	const FTransform& ComponentToWorld, FGGYGOCharacterPointLightFrame& OutFrame, FString& OutError)
{
	check(IsInGameThread());
	OutFrame = {};
	OutError.Reset();
	FGGYGOCharacterPointLightFrame Frame;
	Frame.InstanceId = static_cast<uint64>(Instance.GetId());
	Frame.SimulationAgeSeconds = Instance.GetAge();
	if (!FMath::IsFinite(Frame.SimulationAgeSeconds) || Frame.SimulationAgeSeconds < 0 || !ComponentToWorld.IsValid())
	{ OutError = TEXT("原CPU Point灯实例年龄或组件变换无效"); return EGGYGOCharacterLightSourceReadiness::Invalid; }
	if (Instance.IsComplete()) { OutFrame = MoveTemp(Frame); return EGGYGOCharacterLightSourceReadiness::Ready; }
	if (Instance.HasPendingFinalize())
	{ OutError = TEXT("原CPU实例尚无已完成模拟帧"); return EGGYGOCharacterLightSourceReadiness::WaitingForSimulation; }
	TSet<uint64> Keys;
	for (const FNiagaraEmitterInstanceRef& EmitterRef : Instance.GetEmitters())
	{
		const FNiagaraEmitterInstance& Emitter = EmitterRef.Get();
		if (Emitter.IsComplete()) { continue; }
		// 同实例公开版本数据正是stateful Native GetRenderers的来源；不访问protected数组。
		const FVersionedNiagaraEmitterData* EmitterData = Emitter.GetVersionedEmitter().GetEmitterData();
		if (!EmitterData)
		{
			bool bHasNativeLight = false;
			Emitter.ForEachEnabledRenderer([&](const UNiagaraRendererProperties* Renderer)
			{ bHasNativeLight |= Renderer->IsA<UNiagaraLightRendererProperties>(); });
			if (bHasNativeLight)
			{ OutError = TEXT("原Point灯缺同实例版本Renderer来源"); return EGGYGOCharacterLightSourceReadiness::Invalid; }
			continue; // 原生没有可绘灯的stateless/初始化暂态，不造CPU数据。
		}
		const auto& Renderers = EmitterData->GetRenderers();
		for (int32 RendererIndex = 0; RendererIndex < Renderers.Num(); ++RendererIndex)
		{
			const UNiagaraRendererProperties* Renderer = Emitter.GetRenderer(RendererIndex);
			if (Renderer != Renderers[RendererIndex])
			{ OutError = TEXT("原实例公开Renderer与同版本来源不匹配"); return EGGYGOCharacterLightSourceReadiness::Invalid; }
			const auto* Properties = Cast<UNiagaraLightRendererProperties>(Renderer);
			if (!IsValid(Properties) || !Properties->GetIsEnabled()) { continue; }
			const auto Fail = [&](const TCHAR* Reason)
			{
				OutError = FString::Printf(TEXT("FX Point灯 Emitter='%s' Renderer=%d：%s"),
					*Emitter.GetEmitterHandle().GetName().ToString(), RendererIndex, Reason);
				return EGGYGOCharacterLightSourceReadiness::Invalid;
			};
			if (Properties->GetClass() != UNiagaraLightRendererProperties::StaticClass())
			{ return Fail(TEXT("自定义LightRenderer未声明与Native相同的灯事实合同")); }
			if (Emitter.GetSimTarget() != ENiagaraSimTarget::CPUSim)
			{ return Fail(TEXT("需要原CPU数据，不能以GPU或缺数据当空灯")); }
			if (Properties->RendererEnabledBinding.GetParamMapBindableVariable().IsValid())
			{
				FNiagaraBool Enabled;
				if (!ReadBoundValue(Emitter, Properties->RendererEnabledBinding, Enabled)) { return Fail(TEXT("显式RendererEnabled绑定未由原实例提供")); }
				if (!Enabled.GetValue()) { continue; }
			}
			if (Emitter.IsWaitingForPSOCaching())
			{ OutError = TEXT("原灯所属Emitter尚待PSO就绪"); return EGGYGOCharacterLightSourceReadiness::WaitingForSimulation; }
			const FNiagaraDataSet& Data = Emitter.GetParticleData();
			const FNiagaraDataBuffer* Buffer = Data.GetCurrentData();
			const bool bEmitterSource = Properties->SourceMode == ENiagaraRendererSourceDataMode::Emitter;
			if (!bEmitterSource && Properties->SourceMode != ENiagaraRendererSourceDataMode::Particles)
			{ return Fail(TEXT("未支持的原生灯数据模式")); }
			// NiagaraRendererLights对同实例CurrentData==nullptr明确不绘制；这不是缺资产或错绑定。
			// 若已有buffer，即使TickCount==0也继续读真实Native值，不按帧数伪造空灯。
			if (!Buffer) { continue; }
			const int32 Count = bEmitterSource ? 1 : Buffer->GetNumInstances();
			if (!Count) { continue; }
			const auto Positions = Properties->PositionDataSetAccessor.GetReader(Buffer);
			const auto Colors = Properties->ColorDataSetAccessor.GetReader(Buffer);
			const auto Radii = Properties->RadiusDataSetAccessor.GetReader(Buffer);
			const auto Enabled = Properties->EnabledDataSetAccessor.GetReader(Buffer);
			const auto Visibility = Properties->RendererVisibilityTagAccessor.GetReader(Buffer);
			const auto Exponents = Properties->ExponentDataSetAccessor.GetReader(Buffer);
			const auto UniqueIds = Properties->LightIdAccessor.GetReader(Buffer);
			const bool bHasVisibility = Properties->RendererVisibilityTagBinding.GetParamMapBindableVariable().IsValid();
			if (!bEmitterSource && (!Enabled.IsValid() || !Radii.IsValid() || !UniqueIds.IsValid()
				|| (bHasVisibility && !Visibility.IsValid())))
			{ return Fail(TEXT("实际Enabled/Radius/Visibility/Native UniqueID typed列未提供")); }
			const FTransform SimToWorld = Emitter.IsLocalSpace() ? ComponentToWorld : Instance.GetLWCSimToWorld(false);
			if (!SimToWorld.IsValid() || !FMath::IsFinite(Properties->RadiusScale) || Properties->ColorAdd.ContainsNaN())
			{ return Fail(TEXT("实际Native灯变换/RadiusScale/ColorAdd无效")); }
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FNiagaraBool LightEnabled;
				float Radius;
				int32 VisTag = Properties->RendererVisibility;
				if (bEmitterSource)
				{
					if (!ReadBoundValue(Emitter, Properties->LightRenderingEnabledBinding, LightEnabled)
						|| !ReadBoundValue(Emitter, Properties->RadiusBinding, Radius)
						|| (bHasVisibility && !ReadBoundValue(Emitter, Properties->RendererVisibilityTagBinding, VisTag)))
					{ return Fail(TEXT("实际Emitter Enabled/Radius/Visibility绑定未提供")); }
				}
				else { LightEnabled = Enabled.Get(Index); Radius = Radii.Get(Index); if (bHasVisibility) VisTag = Visibility.Get(Index); }
				if (!LightEnabled.GetValue() || VisTag != Properties->RendererVisibility) { continue; }
				Radius *= Properties->RadiusScale;
				if (!FMath::IsFinite(Radius)) { return Fail(TEXT("实际半径非有限")); }
				if (Radius <= 0) { continue; } // Native明确不绘制的半径，合法空灯。
				FVector3f Position;
				FLinearColor Color;
				float Exponent = 0;
				if (bEmitterSource)
				{
					if (!ReadBoundValue(Emitter, Properties->PositionBinding, Position)
						|| !ReadBoundValue(Emitter, Properties->ColorBinding, Color)
						|| (!Properties->bUseInverseSquaredFalloff && !ReadBoundValue(Emitter, Properties->LightExponentBinding, Exponent)))
					{ return Fail(TEXT("实际Emitter Position/Color/Exponent绑定未提供")); }
				}
				else
				{
					if (!Positions.IsValid() || !Colors.IsValid() || (!Properties->bUseInverseSquaredFalloff && !Exponents.IsValid()))
					{ return Fail(TEXT("实际Particle Position/Color/Exponent typed列未提供")); }
					Position = Positions.Get(Index); Color = Colors.Get(Index);
					if (!Properties->bUseInverseSquaredFalloff) Exponent = Exponents.Get(Index);
				}
				if (Position.ContainsNaN() || !FiniteColor(Color) || !FMath::IsFinite(Exponent) || Exponent < 0)
				{ return Fail(TEXT("实际Native灯坐标/颜色/衰减无效")); }
				const float Brightness = Properties->bAlphaScalesBrightness ? Color.A : 1.f;
				const FVector3f FinalRGB = FVector3f(Color.R, Color.G, Color.B) * Brightness + Properties->ColorAdd;
				FGGYGOCharacterPointLight Light;
				Light.StableKey = StableLightKey(Frame.InstanceId, Emitter.GetEmitterHandle().GetId(), RendererIndex,
					bEmitterSource, bEmitterSource ? 0 : UniqueIds.Get(Index));
				Light.PositionWorldCm = SimToWorld.TransformPosition(FVector(Position));
				Light.Color = FLinearColor(FinalRGB.X, FinalRGB.Y, FinalRGB.Z, 1.f);
				Light.RadiusCm = Radius;
				Light.NativeFalloffExponent = Exponent;
				if (Keys.Contains(Light.StableKey) || Light.PositionWorldCm.ContainsNaN() || !FiniteColor(Light.Color))
				{ return Fail(TEXT("Native灯最终值无效或稳定key冲突")); }
				Keys.Add(Light.StableKey);
				Frame.Lights.Add(MoveTemp(Light));
			}
		}
	}
	OutFrame = MoveTemp(Frame);
	return EGGYGOCharacterLightSourceReadiness::Ready;
}
