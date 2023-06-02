// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class AlterMeshEditor : ModuleRules
{
	public AlterMeshEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		bUsePrecompiled = true;

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);


		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);


		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Json",
				"JsonUtilities",
				"InputCore",
				"PropertyEditor",
				"UnrealEd",
				"EditorStyle",
				"Projects",
				"RenderCore",
				"RHI",
				"DeveloperSettings",
				"AlterMeshRendering",
				"AlterMesh",
				"DetailCustomizations",
				"MeshDescription",
				"StaticMeshDescription",
				"Foliage",
				"Landscape",
				"ComponentVisualizers",
				"EditorWidgets",
				"Niagara",
				"HTTP",
				// ... add other public dependencies that you statically link with here ...
			}
			);


		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"PropertyEditor",
				// ... add private dependencies that you statically link with here ...
			}
			);


		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
