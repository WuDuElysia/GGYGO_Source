/** @file GGYGOFXMathDataInterface.h @brief 原 FX 纯数学的无状态 CPU VM external-call 边界。 */
#pragma once

#include "NiagaraDataInterface.h"
#include "GGYGOFXMathDataInterface.generated.h"

/**
 * 同阶段显式输入的仿射步骤与 ZXY Euler；不持有变换、时钟或随机状态。
 * ComposeAffineStep 输入 parent P/C0/C1/C2、local P/Q/S，输出 next P/C0/C1/C2。
 * EulerZXY 输入弧度 Euler，按 qy*(qx*qz) 输出 Quat；只支持原 CPU 模拟目标。
 */
UCLASS(EditInlineNew, Category = "GGYGO|FX", meta = (DisplayName = "GGYGO CPU FX 纯数学"))
class GGYGO_API UGGYGOFXMathDataInterface : public UNiagaraDataInterface
{
	GENERATED_BODY()
public:
	UGGYGOFXMathDataInterface(const FObjectInitializer& ObjectInitializer);
	virtual void PostInitProperties() override;
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return Target == ENiagaraSimTarget::CPUSim; }
	virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& Binding, void* Data, FVMExternalFunction& Out) override;
protected:
#if WITH_EDITORONLY_DATA
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#endif
private:
	void VMComposeAffineStep(FVectorVMExternalFunctionContext& Context);
	void VMEulerZXY(FVectorVMExternalFunctionContext& Context);
};
