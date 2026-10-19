/** @file 材质/预览资源归属、Toon 接口和主光生命周期专项。 */
#if WITH_DEV_AUTOMATION_TESTS
#include "Character/Components/GGYGOCharacterRenderComponent.h"
#include "Animation/Skeleton.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#if WITH_EDITOR
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Rendering/SkeletalMeshRenderData.h"
#endif
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"
#include <limits>

namespace
{
	USkeletalMesh* MakeRenderTestMesh(UMaterialInterface* Material)
	{
		USkeletalMesh* Mesh = NewObject<USkeletalMesh>();
		USkeleton* Skeleton = NewObject<USkeleton>();
		// A synthetic skeletal mesh must have a valid root BEFORE assigning it to
		// a component; USkeleton::IsCompatibleMesh assumes a non-empty hierarchy.
		{
			FReferenceSkeletonModifier Modifier(Mesh->GetRefSkeleton(), Skeleton);
			Modifier.Add(FMeshBoneInfo(FName(TEXT("root")), TEXT("root"), INDEX_NONE), FTransform::Identity);
		}
		Mesh->SetSkeleton(Skeleton);
		Skeleton->MergeAllBonesToBoneTree(Mesh);
		Mesh->CalculateInvRefMatrices();
#if WITH_EDITOR
		// 已注册的 LeaderPose 会求值必需骨，隐藏夹具也必须提供有效的 LOD 骨列表。
		// 此专项不绘制三角形或初始化 GPU 缓冲；正式角色仍在生产视口检查。
		Mesh->AllocateResourceForRendering();
		FSkeletalMeshLODRenderData* LOD = new FSkeletalMeshLODRenderData();
		LOD->ActiveBoneIndices.Add(0);
		LOD->RequiredBones.Add(0);
		Mesh->GetResourceForRendering()->LODRenderData.Add(LOD);
		Mesh->AddLODInfo();
#endif
		FSkeletalMaterial Slot;
		Slot.MaterialSlotName = TEXT("Surface");
		Slot.MaterialInterface = Material;
		Mesh->GetMaterials().Add(Slot);
		return Mesh;
	}

#if WITH_EDITOR
	UMaterial* MakeRenderTestToonMaterial(bool bIncludeVisibility = true)
	{
		UMaterial* Material = NewObject<UMaterial>();
		Material->SetShadingModel(MSM_Unlit);
		for (const FName Name : {FName(TEXT("KeyLightDirectionWS")), FName(TEXT("KeyLightColor"))})
		{
			UMaterialExpressionVectorParameter* Parameter = NewObject<UMaterialExpressionVectorParameter>(Material);
			Parameter->ParameterName = Name;
			Parameter->DefaultValue = FLinearColor::White;
			Material->GetExpressionCollection().AddExpression(Parameter);
		}
		for (const FName Name : {FName(TEXT("SceneLightStrength")), FName(TEXT("KeyLightVisibility"))})
		{
			if (!bIncludeVisibility && Name == TEXT("KeyLightVisibility")) continue;
			UMaterialExpressionScalarParameter* Parameter = NewObject<UMaterialExpressionScalarParameter>(Material);
			Parameter->ParameterName = Name;
			Parameter->DefaultValue = 1.0f;
			Material->GetExpressionCollection().AddExpression(Parameter);
		}
		Material->UpdateCachedExpressionData();
		return Material;
	}
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterRenderOwnershipTest, "GGYGO.Rendering.MaterialOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCharacterRenderOwnershipTest::RunTest(const FString& Parameters)
{
	UMaterial* Original = NewObject<UMaterial>();
	UMaterial* Replacement = NewObject<UMaterial>();
	USkeletalMesh* Asset = MakeRenderTestMesh(Original);
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>();
	Mesh->SetSkeletalMeshAsset(Asset);
	UGGYGOCharacterRenderComponent* Render = NewObject<UGGYGOCharacterRenderComponent>();
	TestTrue(TEXT("Generic renderer has no role bindings"), Render->MaterialSlots.IsEmpty());
	TestNull(TEXT("Generic renderer has no role outline material"), Render->OutlineMaterial.Get());
	auto Install = [&](UMaterialInterface* Previous)
	{
		Render->TargetMesh = Mesh;
		Render->BoundMeshAsset = Asset;
		UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Original, Render);
		FGGYGOCharacterRenderBinding& Binding = Render->MaterialBindings.AddDefaulted_GetRef();
		Binding.SlotIndex = 0;
		Binding.SlotName = TEXT("Surface");
		Binding.OriginalOverride = Previous;
		Binding.InstalledMaterial = MID;
		Render->DynamicMaterials.Add(MID);
		Mesh->SetMaterial(0, MID);
	};
	Install(nullptr);
	Render->ReleaseMaterials();
	TestNull(TEXT("Restores absent override, not a copied asset material"), Mesh->OverrideMaterials[0].Get());
	TestEqual(TEXT("Asset default visible"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Original));
	Render->ReleaseMaterials();
	TestTrue(TEXT("Release idempotent"), Render->MaterialBindings.IsEmpty());
	Install(Original);
	Mesh->SetMaterial(0, Replacement);
	Render->ReleaseMaterials();
	TestEqual(TEXT("External owner wins"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Replacement));
	Install(Original);
	USkeletalMesh* NewAsset = MakeRenderTestMesh(Replacement);
	Mesh->SetSkeletalMeshAsset(NewAsset);
	Render->ReleaseMaterials();
	TestEqual(TEXT("Late swap cleanup uses NEW asset default"), Mesh->GetMaterial(0), static_cast<UMaterialInterface*>(Replacement));
	Mesh->SetSkeletalMeshAsset(Asset);
	Install(Original);
	Asset->GetMaterials()[0].MaterialSlotName = TEXT("ChangedSlot");
	Render->ReleaseMaterials();
	TestNull(TEXT("Slot identity change cannot restore stale override"), Mesh->OverrideMaterials[0].Get());
	return true;
}

#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterRenderPreviewTest, "GGYGO.Rendering.EditorPreviewOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCharacterRenderPreviewTest::RunTest(const FString& Parameters)
{
	UGGYGOCharacterRenderComponent* Render = NewObject<UGGYGOCharacterRenderComponent>();
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>();
	Mesh->SetCustomPrimitiveDataFloat(14, 7.0f);
	const TArray<float> Defaults = Mesh->GetDefaultCustomPrimitiveData().Data;
	Render->PreviewMesh = Mesh;
	Render->PreviewOriginalData = Mesh->GetCustomPrimitiveData().Data;
	const float First[9] = {1, 0, 0, 1, 1, 1, 1, 0.65f, 1};
	const float Dimmed[9] = {1, 0, 0, 1, 1, 1, 1, 0.325f, 1};
	TestTrue(TEXT("独占瞬态输入首次写入"), Render->WritePreviewLighting(First));
	TestTrue(TEXT("同一输入契约实际消费变暗主光"), Render->WritePreviewLighting(Dimmed));
	TestEqual(TEXT("强度写入真实 CPD"), Mesh->GetCustomPrimitiveData().Data[7], 0.325f);
	TestTrue(TEXT("序列化默认未改变"), Defaults == Mesh->GetDefaultCustomPrimitiveData().Data);
	Mesh->SetCustomPrimitiveDataFloat(4, 0.25f);
	TestFalse(TEXT("外部写入后拒绝继续覆盖"), Render->WritePreviewLighting(First));
	Render->ReleasePreviewLighting();
	TestEqual(TEXT("外部颜色保持"), Mesh->GetCustomPrimitiveData().Data[4], 0.25f);
	TestEqual(TEXT("只释放自有有效标志"), Mesh->GetCustomPrimitiveData().Data[0], 0.0f);
	TestEqual(TEXT("其它职责尾部数据保持"), Mesh->GetCustomPrimitiveData().Data[14], 7.0f);
	Render->ReleasePreviewLighting();
	TestTrue(TEXT("预览释放幂等"), Render->PreviewWrittenData.IsEmpty());
	Render->PreviewMesh = Mesh;
	Render->PreviewOriginalData = Mesh->GetCustomPrimitiveData().Data;
	TestTrue(TEXT("新租用保存已有原值"), Render->WritePreviewLighting(First));
	Mesh->SetCustomPrimitiveDataFloat(0, 0.5f);
	const TArray<float> Foreign = Mesh->GetCustomPrimitiveData().Data;
	Render->ReleasePreviewLighting();
	TestTrue(TEXT("外部接管有效标志后全部保留"), Foreign == Mesh->GetCustomPrimitiveData().Data);
	TestFalse(TEXT("模板无编辑器场景写权限"), Render->UsesEditorSceneLighting());
	if (!TestNotNull(TEXT("Engine"), GEngine)) return false;
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("独立 Editor 世界"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Editor).SetCurrentWorld(World);
	struct FPreviewWorldCleanup
	{
		UWorld* World;
		~FPreviewWorldCleanup()
		{
			World->DestroyWorld(false);
			GEngine->DestroyWorldContext(World);
			World->GetPackage()->SetDirtyFlag(false);
		}
	} Cleanup{World};
	UMaterial* Material = MakeRenderTestToonMaterial();
	const TCHAR* Names[] = {TEXT("ScenePreviewValid"), TEXT("ScenePreviewDirectionX"), TEXT("ScenePreviewDirectionY"),
		TEXT("ScenePreviewDirectionZ"), TEXT("ScenePreviewColorR"), TEXT("ScenePreviewColorG"), TEXT("ScenePreviewColorB"),
		TEXT("ScenePreviewStrength"), TEXT("ScenePreviewVisibility")};
	for (int32 Index = 0; Index < 9; ++Index)
	{
		UMaterialExpressionScalarParameter* Parameter = NewObject<UMaterialExpressionScalarParameter>(Material);
		Parameter->ParameterName = Names[Index];
		Parameter->bUseCustomPrimitiveData = true;
		Parameter->PrimitiveDataIndex = Index;
		Material->GetExpressionCollection().AddExpression(Parameter);
	}
	Material->UpdateCachedExpressionData();
	UMaterial* OutlineMaterial = NewObject<UMaterial>();
	OutlineMaterial->SetShadingModel(MSM_Unlit);
	for (const FName Name : {FName(TEXT("OutlineWidth")), FName(TEXT("OutlineOpacity"))})
	{
		UMaterialExpressionScalarParameter* Parameter = NewObject<UMaterialExpressionScalarParameter>(OutlineMaterial);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = 1.0f;
		OutlineMaterial->GetExpressionCollection().AddExpression(Parameter);
	}
	OutlineMaterial->UpdateCachedExpressionData();
	ACharacter* Character = World->SpawnActor<ACharacter>();
	ADirectionalLight* Light = World->SpawnActor<ADirectionalLight>();
	if (!TestNotNull(TEXT("实际角色"), Character) || !TestNotNull(TEXT("实际方向光"), Light)) return false;
	Light->GetLightComponent()->SetIntensity(10);
	USkeletalMeshComponent* CharacterMesh = Character->GetMesh();
	// 此专项只验证已注册组件的数据传输/释放；只有骨 LOD 的夹具不进入场景绘制。
	// 正式角色可见性和 Shader 编译必须在独立生产视口检查。
	CharacterMesh->SetVisibility(false);
	CharacterMesh->SetCastShadow(false);
	USkeletalMesh* PreviewAsset = MakeRenderTestMesh(Material);
	FSkeletalMaterial UnconfiguredSlot;
	UnconfiguredSlot.MaterialSlotName = TEXT("Unconfigured");
	UnconfiguredSlot.MaterialInterface = MakeRenderTestToonMaterial();
	PreviewAsset->GetMaterials().Add(UnconfiguredSlot);
	CharacterMesh->SetSkeletalMeshAsset(PreviewAsset);
	const bool bDirtyBefore = Character->GetPackage()->IsDirty();
	UGGYGOCharacterRenderComponent* Preview = NewObject<UGGYGOCharacterRenderComponent>(Character);
	FGGYGOCharacterRenderSlot& Binding = Preview->MaterialSlots.AddDefaulted_GetRef();
	Binding.SlotName = TEXT("Surface");
	Binding.Material = Material;
	Preview->bTraceKeyLightOcclusion = false;
	Preview->bPreviewSceneLighting = true;
	Preview->ReferenceKeyLightIntensity = 10;
	Preview->KeyLightIntensityResponse = 0.5f;
	Preview->KeyLightOutputScale = 0.65f;
	Preview->bEnableOutline = true;
	Preview->OutlineMaterial = OutlineMaterial;
	Preview->OutlineWidth = 0.6f;
	Preview->RegisterComponent();
	if (!TestEqual(TEXT("非 PIE 注册获得预览租用"), Preview->PreviewWrittenData.Num(), 9)) return false;
	if (!TestNotNull(TEXT("非 PIE 创建真实瞬态描边壳"), Preview->OutlineMesh.Get())) return false;
	TestTrue(TEXT("描边注册与主 Mesh 共用姿态"), Preview->OutlineMesh->IsRegistered()
		&& Preview->OutlineMesh->LeaderPoseComponent.Get() == CharacterMesh);
	TestTrue(TEXT("描边不序列化或复制为资产组件"), Preview->OutlineMesh->HasAllFlags(RF_Transient | RF_DuplicateTransient));
	UGGYGOCharacterRenderComponent* DuplicatePreview = DuplicateObject<UGGYGOCharacterRenderComponent>(Preview, Character);
	if (!TestNotNull(TEXT("原生复制表现组件"), DuplicatePreview)) return false;
	TestNull(TEXT("复制组件不继承原预览描边所有权"), DuplicatePreview->OutlineMesh.Get());
	TestFalse(TEXT("复制组件不继承原预览主 Mesh 租用"), DuplicatePreview->PreviewMesh.IsValid());
	DuplicatePreview->ReleaseMaterials();
	TestTrue(TEXT("复制组件释放不销毁原描边"), Preview->OutlineMesh->IsRegistered());
	TestFalse(TEXT("隐藏主 Mesh 的预览壳同步隐藏"), Preview->OutlineMesh->IsVisible());
	float OutlineValue = -1.0f;
	TestTrue(TEXT("外部配置的描边宽度输入实际存在"), Preview->OutlineMesh->GetMaterial(0)->GetScalarParameterValue(
		FHashedMaterialParameterInfo(TEXT("OutlineWidth")), OutlineValue));
	TestEqual(TEXT("描边宽度消费角色配置"), OutlineValue, 0.6f);
	TestTrue(TEXT("配置槽的描边可见输入实际存在"), Preview->OutlineMesh->GetMaterial(0)->GetScalarParameterValue(
		FHashedMaterialParameterInfo(TEXT("OutlineOpacity")), OutlineValue));
	TestEqual(TEXT("配置槽开启描边"), OutlineValue, 1.0f);
	TestTrue(TEXT("未配置槽的描边可见输入实际存在"), Preview->OutlineMesh->GetMaterial(1)->GetScalarParameterValue(
		FHashedMaterialParameterInfo(TEXT("OutlineOpacity")), OutlineValue));
	TestEqual(TEXT("未配置槽不额外绘制"), OutlineValue, 0.0f);
	TestEqual(TEXT("预览创建不使角色所在包变脏"), Character->GetPackage()->IsDirty(), bDirtyBefore);
	TestEqual(TEXT("注册实际主光强度"), CharacterMesh->GetCustomPrimitiveData().Data[7], 0.65f);
	Light->GetLightComponent()->SetIntensity(2.5f);
	Preview->TickComponent(0.1f, LEVELTICK_ViewportsOnly, nullptr);
	TestEqual(TEXT("非 PIE Tick 实际消费变化"), CharacterMesh->GetCustomPrimitiveData().Data[7], 0.325f);
	TestEqual(TEXT("预览未创建材质覆盖"), CharacterMesh->OverrideMaterials.Num(), 0);
	TestTrue(TEXT("预览未写序列化 CPD"), CharacterMesh->GetDefaultCustomPrimitiveData().Data.IsEmpty());
	TWeakObjectPtr<USkeletalMeshComponent> FirstOutline = Preview->OutlineMesh.Get();
	CharacterMesh->SetSkeletalMeshAsset(MakeRenderTestMesh(Material));
	Preview->TickComponent(0.1f, LEVELTICK_ViewportsOnly, nullptr);
	TestTrue(TEXT("换 Mesh 后仅绑定新资产"), Preview->PreviewMeshAsset.Get() == CharacterMesh->GetSkeletalMeshAsset());
	TestTrue(TEXT("换 Mesh 后原描边退出场景"), !FirstOutline.IsValid() || !FirstOutline->IsRegistered());
	if (!TestNotNull(TEXT("换 Mesh 重建当前描边"), Preview->OutlineMesh.Get())) return false;
	TestEqual(TEXT("描边使用新网格"), Preview->OutlineMesh->GetSkeletalMeshAsset(), CharacterMesh->GetSkeletalMeshAsset());
	Preview->bPreviewSceneLighting = false;
	Preview->TickComponent(0.1f, LEVELTICK_ViewportsOnly, nullptr);
	TestEqual(TEXT("停用切回 MI 预览"), CharacterMesh->GetCustomPrimitiveData().Data[0], 0.0f);
	TestNull(TEXT("停用释放自有描边"), Preview->OutlineMesh.Get());
	Preview->bPreviewSceneLighting = true;
	Preview->RefreshMaterials();
	TestEqual(TEXT("零值槽可重新租用"), Preview->PreviewWrittenData.Num(), 9);
	TestNotNull(TEXT("重新租用创建描边"), Preview->OutlineMesh.Get());
	World->WorldType = EWorldType::EditorPreview;
	Preview->RefreshMaterials();
	TestEqual(TEXT("EditorPreview 无场景驱动，不伪造灯"), CharacterMesh->GetCustomPrimitiveData().Data[0], 0.0f);
	TestNull(TEXT("退出关卡预览无残留描边"), Preview->OutlineMesh.Get());
	Preview->UnregisterComponent();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterRenderContractTest, "GGYGO.Rendering.ToonInputContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCharacterRenderContractTest::RunTest(const FString& Parameters)
{
	UGGYGOCharacterRenderComponent* Render = NewObject<UGGYGOCharacterRenderComponent>();
	UMaterial* Toon = MakeRenderTestToonMaterial();
	USkeletalMeshComponent* Mesh = NewObject<USkeletalMeshComponent>();
	USkeletalMesh* Asset = MakeRenderTestMesh(Toon);
	Mesh->SetSkeletalMeshAsset(Asset);
	FGGYGOCharacterRenderSlot& Slot = Render->MaterialSlots.AddDefaulted_GetRef();
	Slot.SlotName = TEXT("Surface");
	Slot.Material = Toon;
	FString Error;
	TestTrue(TEXT("Unlit material exposes the complete scene-light input contract"), Render->ValidateMaterialBindings(Mesh, Error));
	Slot.Material = NewObject<UMaterial>();
	TestFalse(TEXT("Lit BRDF cannot consume an already shaded Toon output"), Render->ValidateMaterialBindings(Mesh, Error));
	TestTrue(TEXT("Lit failure identifies the slot and shading responsibility"), Error.Contains(TEXT("Surface")) && Error.Contains(TEXT("Unlit")));
	Slot.Material = MakeRenderTestToonMaterial(false);
	TestFalse(TEXT("Missing scalar cannot be silently ignored by SetParameter"), Render->ValidateMaterialBindings(Mesh, Error));
	TestTrue(TEXT("Failure names the missing input"), Error.Contains(TEXT("KeyLightVisibility")));
	Slot.Material = nullptr;
	TestFalse(TEXT("Missing configured material is a failure"), Render->ValidateMaterialBindings(Mesh, Error));
	Slot.Material = Toon;
	Slot.SlotName = TEXT("Absent");
	TestFalse(TEXT("Unknown configured slot cannot silently skip"), Render->ValidateMaterialBindings(Mesh, Error));
	Slot.SlotName = TEXT("Surface");
	const FGGYGOCharacterRenderSlot DuplicateBinding = Slot;
	Render->MaterialSlots.Add(DuplicateBinding);
	TestFalse(TEXT("Two configured writers for the same slot are rejected"), Render->ValidateMaterialBindings(Mesh, Error));
	Render->MaterialSlots.RemoveAt(1);
	const FSkeletalMaterial DuplicateSlot = Asset->GetMaterials()[0];
	Asset->GetMaterials().Add(DuplicateSlot);
	TestFalse(TEXT("Duplicate mesh slot identity is rejected"), Render->ValidateMaterialBindings(Mesh, Error));
	Asset->GetMaterials().RemoveAt(1);
	FSkeletalMaterial FXSlot;
	FXSlot.MaterialSlotName = TEXT("UncontrolledFX");
	FXSlot.MaterialInterface = NewObject<UMaterial>();
	Asset->GetMaterials().Add(FXSlot);
	TestTrue(TEXT("An unbound FX slot is outside the surface contract"), Render->ValidateMaterialBindings(Mesh, Error));
	TestTrue(TEXT("Configured light settings are valid"), Render->ValidateLightSettings(Error));
	Render->ReferenceKeyLightIntensity = 0.0f;
	TestFalse(TEXT("Zero reference is rejected instead of clamped to a hidden reference"), Render->ValidateLightSettings(Error));
	Render->ReferenceKeyLightIntensity = 1.0f;
	Render->KeyLightIntensityResponse = 2.0f;
	TestFalse(TEXT("Out-of-range response is rejected instead of selecting a different policy"), Render->ValidateLightSettings(Error));
	Render->KeyLightIntensityResponse = 0.0f;
	Render->KeyLightTint.R = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("Nonfinite tint is rejected"), Render->ValidateLightSettings(Error));
	Render->KeyLightTint = FLinearColor::White;
	Render->ReferenceKeyLightIntensity = std::numeric_limits<float>::quiet_NaN();
	TestTrue(TEXT("Disabled intensity response does not require an unused reference intensity"), Render->ValidateLightSettings(Error));
	Render->bTraceKeyLightOcclusion = false;
	Render->OccludedKeyLightVisibility = -1.0f;
	TestTrue(TEXT("Disabled occlusion does not require unused visibility configuration"), Render->ValidateLightSettings(Error));
	Render->bEnableOutline = true;
	TestFalse(TEXT("An enabled outline requires its explicit material"), Render->ValidateMaterialBindings(Mesh, Error));
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGGYGOCharacterRenderLightTest, "GGYGO.Rendering.KeyLightLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGGYGOCharacterRenderLightTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("Engine"), GEngine)) return false;
	UWorld::InitializationValues Init;
	Init.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).SetTransactional(false);
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Init);
	if (!TestNotNull(TEXT("World"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	struct FCleanup
	{
		UWorld* World;
		UEngine* Engine;
		~FCleanup()
		{
			// Keep the context valid through actor/component teardown.
			World->DestroyWorld(false);
			Engine->DestroyWorldContext(World);
			// CreateWorld allocated this fixture's package; never clear other worlds.
			World->GetPackage()->SetDirtyFlag(false);
		}
	} Cleanup{World, GEngine};
	AActor* Owner = World->SpawnActor<AActor>();
	UGGYGOCharacterRenderComponent* Render = NewObject<UGGYGOCharacterRenderComponent>(Owner);
	Render->RegisterComponent();
	TestNull(TEXT("No light yields no synthetic key"), Render->ResolveKeyLight());
	ADirectionalLight* First = World->SpawnActor<ADirectionalLight>();
	First->GetLightComponent()->SetIntensity(10.0f);
	TestEqual(TEXT("Discovers light"), Render->ResolveKeyLight(), First);
	TestNull(TEXT("Discovery never writes explicit configuration"), Render->KeyLight.Get());
	ADirectionalLight* Second = World->SpawnActor<ADirectionalLight>();
	Second->GetLightComponent()->SetIntensity(10.0f);
	First->GetLightComponent()->SetVisibility(false);
	TestEqual(TEXT("Hidden cached light replaced"), Render->ResolveKeyLight(), Second);
	Render->KeyLight = First;
	TestNull(TEXT("Hidden explicit light does not silently select another"), Render->ResolveKeyLight());
	First->GetLightComponent()->SetVisibility(true);
	TestEqual(TEXT("Explicit light resumes"), Render->ResolveKeyLight(), First);
	First->GetLightComponent()->SetIntensity(0.0f);
	TestNull(TEXT("Zero intensity means off even with intensity response zero"), Render->ResolveKeyLight());
	Render->KeyLight = nullptr;
	TestEqual(TEXT("Returns to auto selection"), Render->ResolveKeyLight(), Second);
	Second->Destroy();
	TestNull(TEXT("Destroyed weak cache invalidated"), Render->ResolveKeyLight());
	First->GetLightComponent()->SetIntensity(10.0f);
	TestEqual(TEXT("Auto selection recovers"), Render->ResolveKeyLight(), First);
#if WITH_EDITOR
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(MakeRenderTestToonMaterial(), Render);
	Render->DynamicMaterials.Add(MID);
	Render->bTraceKeyLightOcclusion = false;
	Render->ReferenceKeyLightIntensity = 10.0f;
	Render->KeyLightIntensityResponse = 0.5f;
	Render->KeyLightOutputScale = 0.65f;
	Render->MaxKeyLightStrength = 1.0f;
	float Strength = -1.0f;
	auto CheckStrength = [&](float Intensity, float Expected, const TCHAR* Message)
	{
		First->GetLightComponent()->SetIntensity(Intensity);
		Render->UpdateLighting();
		TestTrue(TEXT("Runtime MID exposes strength"), MID->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("SceneLightStrength")), Strength));
		TestTrue(Message, FMath::IsNearlyEqual(Strength, Expected));
	};
	CheckStrength(10.0f, 0.65f, TEXT("Authored reference strength is preserved"));
	CheckStrength(2.5f, 0.325f, TEXT("Configured nonlinear response is preserved"));
	CheckStrength(40.0f, 1.0f, TEXT("Configured cap is preserved"));
	Render->KeyLightIntensityResponse = 0.0f;
	Render->ReferenceKeyLightIntensity = std::numeric_limits<float>::quiet_NaN();
	CheckStrength(0.0f, 0.0f, TEXT("Off remains off when physical intensity response is disabled"));
	CheckStrength(40.0f, 0.65f, TEXT("Art-directed strength still uses a real scene light"));
	Render->KeyLightIntensityResponse = 1.0f;
	Render->ReferenceKeyLightIntensity = 0.01f;
	Render->KeyLightOutputScale = std::numeric_limits<float>::max();
	CheckStrength(std::numeric_limits<float>::max(), 1.0f, TEXT("Large finite inputs reach the cap without a nonfinite MID value"));
	Render->ReferenceKeyLightIntensity = 0.0f;
	Render->SetComponentTickEnabled(true);
	TestTrue(TEXT("Tick is enabled before the invalid live edit"), Render->IsComponentTickEnabled());
	AddExpectedError(TEXT("ReferenceKeyLightIntensity must be finite and >= 0.01"), EAutomationExpectedErrorFlags::Contains, 1);
	Render->UpdateLighting();
	TestTrue(TEXT("An invalid live edit releases materials instead of continuing with a clamped value"), Render->DynamicMaterials.IsEmpty());
	TestFalse(TEXT("Invalid configuration stops tick; recovery requires explicit refresh"), Render->IsComponentTickEnabled());
#endif
	Render->ReleaseMaterials();
	TestFalse(TEXT("Release clears cache"), Render->ResolvedKeyLight.IsValid());
	return true;
}
#endif
