using System;
using System.IO;
using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
	public class KytheraEnkiTS : ModuleRules
	{
		public KytheraEnkiTS(ReadOnlyTargetRules Target) : base(Target)
		{
			// this is an empty module so files in the External directory show up in Visual Studio
			Type = ModuleType.External;
			PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "src"));
			PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "src"));
		}
	}
}
