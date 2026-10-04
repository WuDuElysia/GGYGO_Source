#if WITH_DEV_AUTOMATION_TESTS
#include "Character/Components/GGYGOCharacterRenderComponent.h"
#include "Animation/Skeleton.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "ReferenceSkeleton.h"
#include "UObject/Package.h"

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
		FSkeletalMaterial Slot;
		Slot.MaterialSlotName = TEXT("Surface");
		Slot.MaterialInterface = Material;
		Mesh->GetMaterials().Add(Slot);
		return Mesh;
	}
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
	Render->ReleaseMaterials();
	TestFalse(TEXT("Release clears cache"), Render->ResolvedKeyLight.IsValid());
	return true;
}
#endif
