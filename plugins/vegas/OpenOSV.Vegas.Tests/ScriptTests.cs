// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ScriptTests.cs - the smoke-test script compiles the way VEGAS compiles it.
//
// VEGAS compiles a .cs script at run time with .NET Framework's own CodeDom C#
// compiler, which speaks C# 5.  A string interpolation or a ?. slipped into
// scripts/vegas/OpenOSV Smoke Test.cs would only show up as a compile error
// dialog in the user's VEGAS; this test compiles it with the same compiler,
// against the ScriptPortal.Vegas.dll the build found, and fails first.

using System;
using System.CodeDom.Compiler;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Microsoft.CSharp;

namespace OpenOSV.Vegas.Tests
{
    public static class ScriptTests
    {
        [Test]
        public static void SmokeTestCompilesAsVegasCompilesIt()
        {
            string script = TestContext.SourceFile("scripts/vegas/OpenOSV Smoke Test.cs");
            if (string.IsNullOrEmpty(TestContext.ScriptPortal) || !File.Exists(TestContext.ScriptPortal))
            {
                throw new SkipException("no --scriptportal (no VEGAS install found by the build)");
            }
            var options = new Dictionary<string, string> { { "CompilerVersion", "v4.0" } };
            using (var provider = new CSharpCodeProvider(options))
            {
                var parameters = new CompilerParameters
                {
                    GenerateInMemory = false,
                    GenerateExecutable = false,
                    OutputAssembly = Path.Combine(TestContext.NewDir("script"), "smoke.dll"),
                    TreatWarningsAsErrors = false,
                };
                parameters.ReferencedAssemblies.Add("System.dll");
                parameters.ReferencedAssemblies.Add("System.Core.dll");
                parameters.ReferencedAssemblies.Add("System.Drawing.dll");
                parameters.ReferencedAssemblies.Add("System.Windows.Forms.dll");
                parameters.ReferencedAssemblies.Add(TestContext.ScriptPortal);
                CompilerResults results = provider.CompileAssemblyFromFile(parameters, script);
                var errors = new StringBuilder();
                foreach (CompilerError e in results.Errors)
                {
                    if (!e.IsWarning)
                    {
                        errors.Append("\n    line ").Append(e.Line).Append(": ").Append(e.ErrorNumber).Append(' ').Append(e.ErrorText);
                    }
                }
                Check.True(errors.Length == 0, "the smoke test compiles as C# 5 against " + TestContext.ScriptPortal + errors);
            }
        }
    }
}
