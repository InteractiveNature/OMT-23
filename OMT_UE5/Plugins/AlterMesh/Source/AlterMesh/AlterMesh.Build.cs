// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class AlterMesh : ModuleRules
{
	public AlterMesh(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		PublicDefinitions.Add("ALTERMESH_FREE=1");
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
				"AlterMeshRendering",
				"DeveloperSettings",
				"Projects",
				"RenderCore",
				"RHI",
				"Niagara",
				"Landscape",
				// ... add other public dependencies that you statically link with here ...
			}
			);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"UnrealEd",
				"Slate",
				"SlateCore",
				"MeshUtilities",
				"EditorStyle"
			});
		}

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);

		string dllPath = Path.Combine(PluginDirectory, "Binaries/", Target.Platform.ToString(), "AlterMesh.dll");
		Directory.CreateDirectory(Path.Combine(PluginDirectory, "Binaries/", Target.Platform.ToString()));
		File.Copy(Path.Combine(PluginDirectory, "Source/Extern/AlterMesh.dll"), dllPath, true);
		
		RuntimeDependencies.Add("$(TargetOutputDir)/AlterMesh.dll", Path.Combine(PluginDirectory, "Source/Extern/AlterMesh.dll"));

		PublicAdditionalLibraries.Add(Path.Combine(PluginDirectory, "Source/Extern/AlterMesh.lib"));
	}
}

