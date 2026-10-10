// Generated with `xb buildshaders`.
#if 0
; SPIR-V
; Version: 1.0
; Generator: Khronos Glslang Reference Front End; 11
; Bound: 9012
; Schema: 0
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %oC0
               OpExecutionMode %main OriginUpperLeft
               OpSource GLSL 460
               OpName %main "main"
               OpName %oC0 "oC0"
               OpDecorate %oC0 Location 0
       %void = OpTypeVoid
       %1282 = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
        %oC0 = OpVariable %_ptr_Output_v4float Output
    %float_0 = OpConstant %float 0
       %2938 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0
       %main = OpFunction %void None %1282
       %9011 = OpLabel
               OpStore %oC0 %2938
               OpReturn
               OpFunctionEnd
#endif

const uint32_t placeholder_ps[] = {
    0x07230203, 0x00010000, 0x0008000B, 0x00002334, 0x00000000, 0x00020011,
    0x00000001, 0x0006000B, 0x00000001, 0x4C534C47, 0x6474732E, 0x3035342E,
    0x00000000, 0x0003000E, 0x00000000, 0x00000001, 0x0006000F, 0x00000004,
    0x0000161F, 0x6E69616D, 0x00000000, 0x00001238, 0x00030010, 0x0000161F,
    0x00000007, 0x00030003, 0x00000002, 0x000001CC, 0x00040005, 0x0000161F,
    0x6E69616D, 0x00000000, 0x00030005, 0x00001238, 0x0030436F, 0x00040047,
    0x00001238, 0x0000001E, 0x00000000, 0x00020013, 0x00000008, 0x00030021,
    0x00000502, 0x00000008, 0x00030016, 0x0000000D, 0x00000020, 0x00040017,
    0x0000001D, 0x0000000D, 0x00000004, 0x00040020, 0x0000029A, 0x00000003,
    0x0000001D, 0x0004003B, 0x0000029A, 0x00001238, 0x00000003, 0x0004002B,
    0x0000000D, 0x00000A0C, 0x00000000, 0x0007002C, 0x0000001D, 0x00000B7A,
    0x00000A0C, 0x00000A0C, 0x00000A0C, 0x00000A0C, 0x00050036, 0x00000008,
    0x0000161F, 0x00000000, 0x00000502, 0x000200F8, 0x00002333, 0x0003003E,
    0x00001238, 0x00000B7A, 0x000100FD, 0x00010038,
};
