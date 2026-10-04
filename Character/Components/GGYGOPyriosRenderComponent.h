#pragma once

#include "Character/Components/GGYGOCharacterRenderComponent.h"
#include "GGYGOPyriosRenderComponent.generated.h"

/**
 * Serialized compatibility class for existing StylizedRenderComponent subobjects.
 * All execution is generic; character assets must supply explicit bindings.
 * Keep the class and old flag until saved Blueprints have passed migration gates.
 */
UCLASS(meta = (DisplayName = "Character Render (Legacy Component Identity)"))
class GGYGO_API UGGYGOPyriosRenderComponent : public UGGYGOCharacterRenderComponent
{
	GENERATED_BODY()
public:
	/** Legacy migration input; false after explicit bindings have been saved. No runtime effect. */
	// Keep this serialized (not CPF_Deprecated): migration persists false here.
	// CPF_Edit is required by Python set_editor_property, even for a public C++ member.
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "GGYGO|Rendering|Migration", meta = (DisplayName = "Legacy Auto Configure (Migration Only)"))
	bool bAutoConfigurePyrios = true;
};
