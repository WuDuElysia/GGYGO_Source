// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class GGYGO : ModuleRules
{
	public GGYGO(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
	
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "GameplayAbilities", "GameplayTags", "GameplayTasks" });

		PrivateDependencyModuleNames.AddRange(new string[] { "Json" });

		PublicIncludePaths.AddRange(new string[] {
			Path.Combine(ModuleDirectory, "Public"),
			Path.Combine(ModuleDirectory, "Public/Abilities"),
			Path.Combine(ModuleDirectory, "Public/Attributes"),
			Path.Combine(ModuleDirectory, "Public/Movement"),
			Path.Combine(ModuleDirectory, "Public/StateMachine"),
			Path.Combine(ModuleDirectory, "Public/Combat"),
			Path.Combine(ModuleDirectory, "Public/Camera"),
			Path.Combine(ModuleDirectory, "Public/Core")
});


		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
