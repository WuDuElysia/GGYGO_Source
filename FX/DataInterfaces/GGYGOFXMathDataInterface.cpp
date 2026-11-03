/** @file GGYGOFXMathDataInterface.cpp @brief 用原生 float32 向量数学执行原有序纯公式，不让 VVM 跨调用内联。 */
#include "FX/DataInterfaces/GGYGOFXMathDataInterface.h"

#include "Math/VectorRegister.h"

DEFINE_LOG_CATEGORY_STATIC(LogGGYGOFXMath, Log, All);

namespace
{
	const FName ComposeAffineStepName(TEXT("ComposeAffineStep"));
	const FName EulerZXYName(TEXT("EulerZXY"));

	struct FGGYGOFXMathProxy final : FNiagaraDataInterfaceProxy
	{
		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }
	};

	VectorRegister4f PackVector(const FVector3f& Value)
	{
		return MakeVectorRegisterFloat(Value.X, Value.Y, Value.Z, 0.f);
	}

	VectorRegister4f PackQuaternion(const FQuat4f& Value)
	{
		return MakeVectorRegisterFloat(Value.X, Value.Y, Value.Z, Value.W);
	}

	FVector3f UnpackVector(VectorRegister4f Value)
	{
		FVector3f Result;
		VectorStoreFloat3(Value, &Result.X);
		return Result;
	}

	FQuat4f UnpackQuaternion(VectorRegister4f Value)
	{
		alignas(16) float Components[4];
		VectorStoreAligned(Value, Components);
		return FQuat4f(Components[0], Components[1], Components[2], Components[3]);
	}

	VectorRegister4f ApplyColumns(VectorRegister4f C0, VectorRegister4f C1, VectorRegister4f C2, VectorRegister4f Value)
	{
		// 与原 C0*v.x + C1*v.y + C2*v.z 相同的左结合顺序。
		return VectorAdd(VectorAdd(VectorMultiply(C0, VectorReplicate(Value, 0)),
			VectorMultiply(C1, VectorReplicate(Value, 1))), VectorMultiply(C2, VectorReplicate(Value, 2)));
	}

	VectorRegister4f RotateVector(VectorRegister4f Quaternion, VectorRegister4f Value)
	{
		const VectorRegister4f XYZ = VectorSet_W0(Quaternion);
		const VectorRegister4f Inner = VectorAdd(VectorCross(XYZ, Value), VectorMultiply(VectorReplicate(Quaternion, 3), Value));
		return VectorAdd(Value, VectorMultiply(MakeVectorRegisterFloat(2.f, 2.f, 2.f, 2.f), VectorCross(XYZ, Inner)));
	}

	VectorRegister4f MultiplyQuaternions(VectorRegister4f A, VectorRegister4f B)
	{
		const VectorRegister4f AV = VectorSet_W0(A);
		const VectorRegister4f BV = VectorSet_W0(B);
		const VectorRegister4f XYZ = VectorAdd(VectorAdd(VectorMultiply(VectorReplicate(A, 3), BV),
			VectorMultiply(VectorReplicate(B, 3), AV)), VectorCross(AV, BV));
		const VectorRegister4f W = VectorSubtract(VectorMultiply(VectorReplicate(A, 3), VectorReplicate(B, 3)), VectorDot3(AV, BV));
		alignas(16) float Components[4];
		alignas(16) float Scalar[4];
		VectorStoreAligned(XYZ, Components);
		VectorStoreAligned(W, Scalar);
		return MakeVectorRegisterFloat(Components[0], Components[1], Components[2], Scalar[0]);
	}
}

UGGYGOFXMathDataInterface::UGGYGOFXMathDataInterface(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Proxy.Reset(new FGGYGOFXMathProxy());
}

void UGGYGOFXMathDataInterface::PostInitProperties()
{
	Super::PostInitProperties();
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()),
			ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter);
	}
}

#if WITH_EDITORONLY_DATA
void UGGYGOFXMathDataInterface::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	FNiagaraFunctionSignature Compose;
	Compose.Name = ComposeAffineStepName;
	Compose.bMemberFunction = true;
	Compose.bRequiresContext = false;
	Compose.bSupportsCPU = true;
	Compose.bSupportsGPU = false;
	Compose.Inputs.Emplace(FNiagaraTypeDefinition(GetClass()), TEXT("Math"));
	for (const TCHAR* Name : {TEXT("ParentPosition"), TEXT("ParentColumn0"), TEXT("ParentColumn1"), TEXT("ParentColumn2"), TEXT("LocalPosition")})
	{
		Compose.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), Name);
	}
	Compose.Inputs.Emplace(FNiagaraTypeDefinition::GetQuatDef(), TEXT("LocalRotation"));
	Compose.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("LocalScale"));
	for (const TCHAR* Name : {TEXT("NextPosition"), TEXT("NextColumn0"), TEXT("NextColumn1"), TEXT("NextColumn2")})
	{
		Compose.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), Name);
	}
	OutFunctions.Add(Compose);

	FNiagaraFunctionSignature Euler;
	Euler.Name = EulerZXYName;
	Euler.bMemberFunction = true;
	Euler.bRequiresContext = false;
	Euler.bSupportsCPU = true;
	Euler.bSupportsGPU = false;
	Euler.Inputs.Emplace(FNiagaraTypeDefinition(GetClass()), TEXT("Math"));
	Euler.Inputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Euler"));
	Euler.Outputs.Emplace(FNiagaraTypeDefinition::GetQuatDef(), TEXT("Quaternion"));
	OutFunctions.Add(Euler);
}
#endif

void UGGYGOFXMathDataInterface::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& Binding, void*, FVMExternalFunction& Out)
{
	// 无 per-instance data，不读取或注入 userptr；输入数就是实际 float 寄存器数。
	Out.Unbind();
	if (Binding.Name == ComposeAffineStepName && Binding.GetNumInputs() == 22 && Binding.GetNumOutputs() == 12)
	{
		Out = FVMExternalFunction::CreateUObject(this, &ThisClass::VMComposeAffineStep);
	}
	else if (Binding.Name == EulerZXYName && Binding.GetNumInputs() == 3 && Binding.GetNumOutputs() == 4)
	{
		Out = FVMExternalFunction::CreateUObject(this, &ThisClass::VMEulerZXY);
	}
	else
	{
		UE_LOG(LogGGYGOFXMath, Error, TEXT("CPU math DI 函数或寄存器签名不符：%s Inputs=%d Outputs=%d。"),
			*Binding.Name.ToString(), Binding.GetNumInputs(), Binding.GetNumOutputs());
	}
}

void UGGYGOFXMathDataInterface::VMComposeAffineStep(FVectorVMExternalFunctionContext& Context)
{
	FNDIInputParam<FVector3f> ParentPosition(Context), ParentColumn0(Context), ParentColumn1(Context), ParentColumn2(Context);
	FNDIInputParam<FVector3f> LocalPosition(Context);
	FNDIInputParam<FQuat4f> LocalRotation(Context);
	FNDIInputParam<FVector3f> LocalScale(Context);
	FNDIOutputParam<FVector3f> NextPosition(Context), NextColumn0(Context), NextColumn1(Context), NextColumn2(Context);
	for (int32 Index = 0; Index < Context.GetNumInstances(); ++Index)
	{
		const VectorRegister4f P = PackVector(ParentPosition.GetAndAdvance());
		const VectorRegister4f C0 = PackVector(ParentColumn0.GetAndAdvance());
		const VectorRegister4f C1 = PackVector(ParentColumn1.GetAndAdvance());
		const VectorRegister4f C2 = PackVector(ParentColumn2.GetAndAdvance());
		const VectorRegister4f LocalP = PackVector(LocalPosition.GetAndAdvance());
		const VectorRegister4f Q = PackQuaternion(LocalRotation.GetAndAdvance());
		const FVector3f Scale = LocalScale.GetAndAdvance();
		NextPosition.SetAndAdvance(UnpackVector(VectorAdd(P, ApplyColumns(C0, C1, C2, LocalP))));
		NextColumn0.SetAndAdvance(UnpackVector(ApplyColumns(C0, C1, C2, RotateVector(Q, MakeVectorRegisterFloat(Scale.X, 0.f, 0.f, 0.f)))));
		NextColumn1.SetAndAdvance(UnpackVector(ApplyColumns(C0, C1, C2, RotateVector(Q, MakeVectorRegisterFloat(0.f, Scale.Y, 0.f, 0.f)))));
		NextColumn2.SetAndAdvance(UnpackVector(ApplyColumns(C0, C1, C2, RotateVector(Q, MakeVectorRegisterFloat(0.f, 0.f, Scale.Z, 0.f)))));
	}
}

void UGGYGOFXMathDataInterface::VMEulerZXY(FVectorVMExternalFunctionContext& Context)
{
	FNDIInputParam<FVector3f> Euler(Context);
	FNDIOutputParam<FQuat4f> Quaternion(Context);
	for (int32 Index = 0; Index < Context.GetNumInstances(); ++Index)
	{
		const VectorRegister4f Half = VectorMultiply(PackVector(Euler.GetAndAdvance()), MakeVectorRegisterFloat(0.5f, 0.5f, 0.5f, 0.5f));
		alignas(16) float Sin[4];
		alignas(16) float Cos[4];
		// 与 VM 的 sin/cos 使用同一公开 float32 vector 数学族，不换成 stdlib/double 三角。
		VectorStoreAligned(VectorSin(Half), Sin);
		VectorStoreAligned(VectorCos(Half), Cos);
		const VectorRegister4f QX = MakeVectorRegisterFloat(Sin[0], 0.f, 0.f, Cos[0]);
		const VectorRegister4f QY = MakeVectorRegisterFloat(0.f, Sin[1], 0.f, Cos[1]);
		const VectorRegister4f QZ = MakeVectorRegisterFloat(0.f, 0.f, Sin[2], Cos[2]);
		Quaternion.SetAndAdvance(UnpackQuaternion(MultiplyQuaternions(QY, MultiplyQuaternions(QX, QZ))));
	}
}
