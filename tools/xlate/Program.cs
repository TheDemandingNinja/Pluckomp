// Adapted from TotK Recomp tools/xlate/Program.cs @ afae80c (MIT, see LICENSE; Copyright (c) 2026 Valiantsin Dzerakh),
// itself the Vulkan adaptation of MacNativeGames games/totk/tools/totk_xlate/Program.cs (MNG commit b75be8cf), which is
// based on D2R Mobile dm/gfx/tools/dmsh_xlate/Program.cs. Changes here: the motion-vector queries (TotK Recomp's patch
// 0015) are gone and the comments are English only.
//
// Translates a Maxwell shader of the game into SPIR-V for Vulkan with Ryujinx 1.3.2's translator.
// Protocol (stdout carries replies only; diagnostics go to stderr):
//   pluckomp_xlate --serve              one request per stdin line: {"job": {...}, "out": "<dir>"}; one reply line per
//                                       request: "ok <key>" or "fail <key> <message>"; a malformed line gets
//                                       "fail ? <message>" and the loop continues.
//   pluckomp_xlate <jobs.json> <outdir> batch mode: a JSON array of jobs, prints a summary, exit 1 if any failed.
//   job: { "key", "stage": "vs|ps|cs", "code": path (graphics = 0x50-byte SPH + instructions, compute = instructions),
//          "c1": hex, "tex": {slot: type}, "img": {slot: {type, fmt}}, "attr": {loc: float|sint|uint},
//          "fout": {idx: float|sint|uint}, "local": [x, y, z], "shared": bytes, "debug": bool, "reduced": bool,
//          "softfloat64": bool, "zero_locals": bool, "sbuf_align": int, "subgroup": int, "gather_bias": int,
//          "early_fragment_tests": bool }
//   Output: <out>/<key>.spv and <out>/<key>.res.json (bindings, queried slots, notes).
using Ryujinx.Common.Memory;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;

static class Xlate
{
    // Byte base of NVN's separate texture and sampler handles in c[2], 8 bytes per slot.
    const int TexBase = 0x168, SampBase = 0x568;

    sealed class Accessor : IGpuAccessor
    {
        readonly byte[] _code; readonly JsonObject _job; int _binding = 1;
        public readonly List<string> Notes = new();
        public readonly SortedSet<int> TexQueried = new(), ImgQueried = new();
        readonly byte[] _c1;
        public Accessor(byte[] code, JsonObject job) { _code = code; _job = job; _c1 = job["c1"] is JsonNode h ? Convert.FromHexString((string)h) : Array.Empty<byte>(); }

        // The c1 constant segment (the 0x100 bytes before the control section in the shader package) holds the jump
        // tables of indirect BRX branches (LDC c[1]); without it the translator cannot resolve their targets.
        public uint ConstantBuffer1Read(int offset)
        {
            if (offset >= 0 && offset + 4 <= _c1.Length) return BitConverter.ToUInt32(_c1, offset);
            Notes.Add($"c1 read out of range +{offset:X} (c1 is {_c1.Length} bytes)");
            return 0;
        }

        // Every resource goes into set 0 with a binding number of its own; the res.json lists which is which.
        public SetBindingPair CreateConstantBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateImageBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public SetBindingPair CreateStorageBufferBinding(int index) => new(0, _binding++);
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer) { int b = _binding; _binding += count; return new(0, b); }
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize) => MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code)[(int)address..]);
        public int QuerySamplerArrayLengthFromPool() => 32;
        public int QueryTextureArrayLengthFromBuffer(int slot) => 32;
        public int QueryTextureArrayLengthFromPool() => 32;

        string Get(string table, int slot) => _job[table] is JsonObject t && t[slot.ToString()] is JsonNode n ? (n is JsonObject o ? (string)o["type"] : (string)n) : null;
        int TexSlot(int handle, int cbufSlot)
        {
            (int off0, _, _) = TextureHandle.UnpackOffsets(handle);
            int bytes = off0 * 4;
            return (cbufSlot < 0 || (cbufSlot & 0xffff) == 2) && bytes >= TexBase && bytes < SampBase && (bytes - TexBase) % 8 == 0 ? (bytes - TexBase) / 8 : -1;
        }
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) TexQueried.Add(slot);
            string t = slot >= 0 ? Get("tex", slot) ?? Get("img", slot) : null;
            if (t == null) { Notes.Add($"texture handle {handle:X} (bank {cbufSlot}, slot {slot}) has no type, taken as 2D"); return SamplerType.Texture2D; }
            if (t.EndsWith("_depth")) t = t.Substring(0, t.Length - 6);   // depth-format texture: same sampler type here; the host binds a depth view
            return t switch
            {
                "1d" => SamplerType.Texture1D, "1darray" => SamplerType.Texture1D | SamplerType.Array,
                "buffer" => SamplerType.TextureBuffer, "2d" => SamplerType.Texture2D, "2darray" => SamplerType.Texture2D | SamplerType.Array,
                "2dms" => SamplerType.Texture2D | SamplerType.Multisample, "2dmsarray" => SamplerType.Texture2D | SamplerType.Multisample | SamplerType.Array,
                "3d" => SamplerType.Texture3D, "cube" => SamplerType.TextureCube, "cubearray" => SamplerType.TextureCube | SamplerType.Array,
                _ => SamplerType.Texture2D,
            };
        }
        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            int slot = TexSlot(handle, cbufSlot);
            if (slot >= 0) ImgQueried.Add(slot);
            string f = _job["img"] is JsonObject t && slot >= 0 && t[slot.ToString()] is JsonObject o ? (string)o["fmt"] : null;
            if (f == null) { Notes.Add($"image handle {handle:X} slot {slot} has no format, taken as R8G8B8A8Unorm"); return TextureFormat.R8G8B8A8Unorm; }
            return Enum.TryParse(f, true, out TextureFormat tf) ? tf : TextureFormat.R8G8B8A8Unorm;
        }
        int Local(int i) => _job["local"] is JsonArray a && a.Count > i ? (int)a[i] : 1;
        public int QueryComputeLocalSizeX() => Local(0);
        public int QueryComputeLocalSizeY() => Local(1);
        public int QueryComputeLocalSizeZ() => Local(2);
        public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;
        static AttributeType Attr(string s) => s switch { "sint" => AttributeType.Sint, "uint" => AttributeType.Uint, _ => AttributeType.Float };
        public GpuGraphicsState QueryGraphicsState()
        {
            Array32<AttributeType> attr = default; Array8<AttributeType> fout = default;
            if (_job["attr"] is JsonObject a) foreach (var kv in a) attr[int.Parse(kv.Key)] = Attr((string)kv.Value);
            if (_job["fout"] is JsonObject f) foreach (var kv in f) fout[int.Parse(kv.Key)] = Attr((string)kv.Value);
            // VK_EXT_depth_clip_control handles NVN's near = -w, so the shader is never rewritten for it.
            const bool depthMinusW = false;
            // "early_fragment_tests": the fragment shader's NVN control byte +0x718 (the driver's early-Z force, Ryujinx's
            // EarlyZForce); the SPIR-V then declares EarlyFragmentTests.
            bool earlyZ = _job["early_fragment_tests"] is JsonValue ez && ez.TryGetValue<bool>(out bool eb) && eb;
            return new GpuGraphicsState(earlyZ, InputTopology.Triangles, false, TessPatchType.Triangles, TessSpacing.EqualSpacing,
                false, false, false, depthMinusW, false, 1f, AlphaTestOp.Always, 0f, attr, false, fout, false,
                false /* no Y flip */, true /* origin upper-left */, false);
        }
        public bool QueryHostSupportsDepthClipControl() => true;
        // Vulkan wants the ConstOffset / ConstOffsets image operands Ryujinx emits to be constants; false makes
        // TexturePass emulate the non-constant ones (a TLD4.PTP with offsets from registers).
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        // "softfloat64": true lowers FP64 operations to FP32 (Ryujinx's DoubleToFloat pass), for a host without shaderFloat64.
        public bool QueryHostSupportsShaderFloat64() => _job["softfloat64"] is not JsonValue v || !(v.TryGetValue<bool>(out bool b) && b);
        public bool QueryHostSupportsTextureShadowLod() => true;
        // Sub-texel precision bits of a host whose gathers round differently from Maxwell (AMD, Intel): Ryujinx then
        // biases gather coordinates so the same texels are picked. 0: no bias.
        public int QueryHostGatherBiasPrecision() => _job["gather_bias"] is JsonNode n ? (int)n : 0;
        // "reduced": true marks NoContraction only on operations that must be exact; by default every float operation
        // keeps Maxwell's separately rounded results.
        public bool QueryHostReducedPrecision() => _job["reduced"] is JsonNode n && (bool)n;
        // "sbuf_align": n: the host binds each storage buffer at its guest address rounded down to n
        // (minStorageBufferOffsetAlignment above 4), so storage offsets count from there.
        public bool QueryHasUnalignedStorageBuffer() => _job["sbuf_align"] is JsonNode;
        public int QueryHostStorageBufferOffsetAlignment() => _job["sbuf_align"] is JsonNode n ? (int)n : 16;
        // "subgroup": n: lanes per subgroup when the host cannot pin 32: ballots and votes span n lanes.
        public int QueryHostSubgroupSize() => _job["subgroup"] is JsonNode n ? (int)n : 32;
        // "zero_locals": true: function-local variables start at 0 (patch 0016).
        public bool QueryZeroLocals() => _job["zero_locals"] is JsonValue v && v.TryGetValue<bool>(out bool b) && b;
    }

    static string Translate(JsonObject job, string outDir)
    {
        string key = (string)job["key"];
        try
        {
            byte[] code = File.ReadAllBytes((string)job["code"]);
            var acc = new Accessor(code, job);
            // "debug": false turns DebugMode off; DebugMode skips the AST optimizer (same semantics, more temporaries).
            var flags = (job["debug"] is JsonNode dbg && !(bool)dbg ? 0 : TranslationFlags.DebugMode) | ((string)job["stage"] == "cs" ? TranslationFlags.Compute : 0);
            var ctx = Translator.CreateContext(0, acc, new TranslationOptions(TargetLanguage.Spirv, TargetApi.Vulkan, flags));
            ShaderProgram p = ctx.Translate();
            File.WriteAllBytes(Path.Combine(outDir, key + ".spv"), p.BinaryCode);
            var res = new JsonObject
            {
                ["key"] = key, ["stage"] = (string)job["stage"], ["notes"] = new JsonArray(acc.Notes.ConvertAll(n => (JsonNode)n).ToArray()),
                ["cbufs"] = new JsonArray(), ["sbufs"] = new JsonArray(), ["textures"] = new JsonArray(), ["images"] = new JsonArray(),
                ["uses_frag_coord"] = p.Info.UsesFragCoord, ["frag_out_map"] = p.Info.FragmentOutputMap,
                ["tex_queried"] = new JsonArray(acc.TexQueried.Select(x => (JsonNode)x).ToArray()),
                ["img_queried"] = new JsonArray(acc.ImgQueried.Select(x => (JsonNode)x).ToArray()),
            };
            foreach (var b in p.Info.CBuffers) res["cbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot });
            foreach (var b in p.Info.SBuffers) res["sbufs"].AsArray().Add((JsonNode)new JsonObject { ["binding"] = b.Binding, ["slot"] = b.Slot, ["cb_slot"] = b.SbCbSlot, ["cb_offset"] = b.SbCbOffset, ["flags"] = (int)b.Flags });
            foreach (var (list, name) in new[] { (p.Info.Textures, "textures"), (p.Info.Images, "images") })
                foreach (var t in list) res[name].AsArray().Add((JsonNode)new JsonObject { ["binding"] = t.Binding, ["type"] = (int)t.Type, ["format"] = t.Format.ToString(), ["cbuf_slot"] = t.CbufSlot, ["handle"] = t.HandleIndex, ["array"] = t.ArrayLength, ["separate"] = t.Separate, ["flags"] = (int)t.Flags });
            File.WriteAllText(Path.Combine(outDir, key + ".res.json"), res.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            return null;
        }
        catch (Exception e)
        {
            if (Environment.GetEnvironmentVariable("PLUCKOMP_XLATE_STACK") != null) Console.Error.WriteLine(e.StackTrace);
            return $"{e.GetType().Name}: {e.Message.Split('\n')[0]}";
        }
    }

    static int Main(string[] args)
    {
        if (args.Length == 1 && args[0] == "--serve")
        {
            string line;
            while ((line = Console.ReadLine()) != null)
            {
                if (line.Length == 0) continue;
                string key = "?", err;
                try
                {
                    var req = JsonNode.Parse(line).AsObject(); var job = req["job"].AsObject(); string o = (string)req["out"];
                    key = (string)job["key"];
                    Directory.CreateDirectory(o);
                    err = Translate(job, o);
                }
                catch (Exception e) { err = $"{e.GetType().Name}: {e.Message.Split((char)10)[0]}"; }
                Console.WriteLine(err == null ? $"ok {key}" : $"fail {key} {err}");
                Console.Out.Flush();
            }
            return 0;
        }
        if (args.Length != 2)
        {
            Console.Error.WriteLine("usage: pluckomp_xlate --serve | pluckomp_xlate <jobs.json> <outdir>");
            return 2;
        }
        var jobs = JsonNode.Parse(File.ReadAllText(args[0])).AsArray(); string outDir = args[1]; Directory.CreateDirectory(outDir);
        int ok = 0, bad = 0;
        foreach (JsonObject job in jobs)
        {
            string err = Translate(job, outDir);
            if (err == null) ok++; else { bad++; Console.Error.WriteLine($"failed {(string)job["key"]}: {err}"); }
        }
        Console.WriteLine($"translated {ok}, failed {bad}");
        return bad == 0 ? 0 : 1;
    }
}
