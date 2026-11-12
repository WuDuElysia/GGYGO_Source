/** @file GGYGONiagaraAttachmentTest.cpp @brief 独立 FX 挂点的继承、覆盖及配置拒绝。 */
#include "FX/Tasks/GGYGOAbilityTask_PlayNiagaraEffect.h"

#include "Misc/AutomationTest.h"
#include "NiagaraSystem.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGONiagaraAttachmentConfigurationTest,
	"GGYGO.FX.Configuration.PerUnitAttachment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGONiagaraAttachmentConfigurationTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UNiagaraSystem> System(NewObject<UNiagaraSystem>());
	FGGYGONiagaraEffectUnit Inherited;
	Inherited.System = System.Get();
	Inherited.FieldSettings.Mode = EGGYGOFXFieldMode::NoCustomField;
	const FName DefaultSocket(TEXT("Weapon"));
	const FTransform DefaultTransform(FRotator(10.0, 20.0, 30.0), FVector(1.0, 2.0, -64.0));
	FName Socket;
	FTransform Transform;
	Inherited.ResolveAttachment(DefaultSocket, DefaultTransform, Socket, Transform);
	TestEqual(TEXT("未覆盖单元继承原挂点"), Socket, DefaultSocket);
	TestTrue(TEXT("未覆盖单元完整继承原变换"), Transform.Equals(DefaultTransform));

	FGGYGONiagaraEffectUnit Override = Inherited;
	Override.bOverrideAttachment = true;
	Override.SocketName = NAME_None;
	Override.LocalTransform = FTransform(FVector(4.0, 5.0, 6.0));
	Override.ResolveAttachment(DefaultSocket, DefaultTransform, Socket, Transform);
	TestTrue(TEXT("显式空挂点选择组件根，不偷换默认骨骼"), Socket.IsNone());
	TestTrue(TEXT("单元相对变换替换原变换，不重复叠加"), Transform.Equals(Override.LocalTransform));

	TArray<FGGYGONiagaraEffectUnit> Units{Inherited, Override};
	FString Error;
	TestTrue(TEXT("Attached 接受继承与覆盖并存"),
		UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfigurations(Units, EGGYGONiagaraEffectSpace::Attached, DefaultTransform, Error));
	TestFalse(TEXT("World 拒绝独立挂点配置"),
		UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfigurations(Units, EGGYGONiagaraEffectSpace::World, DefaultTransform, Error));
	TestTrue(TEXT("拒绝信息定位具体单元"), Error.Contains(TEXT("Unit[1]")));
	Units[1].LocalTransform.SetScale3D(FVector(1.0, 0.0, 1.0));
	TestFalse(TEXT("有效默认变换不能掩盖覆盖单元的零缩放"),
		UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfigurations(Units, EGGYGONiagaraEffectSpace::Attached, DefaultTransform, Error));
	Units[1].bOverrideAttachment = false;
	TestTrue(TEXT("关闭覆盖后只消费原默认变换"),
		UGGYGOAbilityTask_PlayNiagaraEffect::ValidateConfigurations(Units, EGGYGONiagaraEffectSpace::Attached, DefaultTransform, Error));
	return true;
}
#endif
