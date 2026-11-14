using System;
using System.IO;
using System.Runtime.InteropServices;

namespace GameEngine.TileMap
{
    /// <summary>
    /// Minimal script surface over an entity's TileMapLayerRef: cell reads and
    /// writes (writes go through the engine's runtime store, so rendering and
    /// collision pick them up) and cell/world conversions through the entity's
    /// transform. The entity id is the ECS id scripts already hold.
    /// </summary>
    public static class TileMap
    {
        public const uint EmptyCell = 0;

        /// <summary>Packs a tile id and orientation bits into a cell value.</summary>
        public static uint MakeCell(uint tileId, bool flipH = false, bool flipV = false, bool transpose = false)
        {
            uint cell = tileId & 0x1FFFFFFFu;
            if (flipH) cell |= 1u << 29;
            if (flipV) cell |= 1u << 30;
            if (transpose) cell |= 1u << 31;
            return cell;
        }

        public static uint CellTileId(uint cell) => cell & 0x1FFFFFFFu;

        /// <summary>Packed cell at (x, y) of the entity's tilemap layer; 0 = empty.</summary>
        public static uint GetCell(uint entity, int x, int y)
        {
            int rc = Native.GetCell(entity, x, y, out uint cell);
            if (rc != 0) throw new InvalidOperationException($"TileMap GetCell failed rc={rc}");
            return cell;
        }

        /// <summary>Writes a packed cell; the render and collision systems rebuild the touched chunk.</summary>
        public static void SetCell(uint entity, int x, int y, uint cell)
        {
            int rc = Native.SetCell(entity, x, y, cell);
            if (rc != 0) throw new InvalidOperationException($"TileMap SetCell failed rc={rc}");
        }

        /// <summary>World position of the cell's center, through the entity's transform.</summary>
        public static (float X, float Y, float Z) CellToWorld(uint entity, int x, int y)
        {
            int rc = Native.CellToWorld(entity, x, y, out float wx, out float wy, out float wz);
            if (rc != 0) throw new InvalidOperationException($"TileMap CellToWorld failed rc={rc}");
            return (wx, wy, wz);
        }

        /// <summary>Cell containing the world-space point, through the entity's transform.</summary>
        public static (int X, int Y) WorldToCell(uint entity, float worldX, float worldY, float worldZ = 0.0f)
        {
            int rc = Native.WorldToCell(entity, worldX, worldY, worldZ, out int cx, out int cy);
            if (rc != 0) throw new InvalidOperationException($"TileMap WorldToCell failed rc={rc}");
            return (cx, cy);
        }
    }

    internal static unsafe class Native
    {
        private const uint kAbiMajor = 1;
        private const uint kAbiMinor = 0;
        private static readonly uint kAbi = (kAbiMajor << 16) | kAbiMinor;

        [StructLayout(LayoutKind.Sequential)]
        private struct GE_TileMap_Interface_v1
        {
            public uint sizeBytes;
            public uint abiVersion;
            public nint GetCell;
            public nint SetCell;
            public nint CellToWorld;
            public nint WorldToCell;
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GE_TileMap_GetInterface_Delegate(uint abiVersion, out nint table, out uint sizeBytes);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int GetCell_Delegate(uint entity, int cellX, int cellY, out uint cell);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int SetCell_Delegate(uint entity, int cellX, int cellY, uint cell);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int CellToWorld_Delegate(uint entity, int cellX, int cellY, out float worldX, out float worldY, out float worldZ);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
        private delegate int WorldToCell_Delegate(uint entity, float worldX, float worldY, float worldZ, out int cellX, out int cellY);

        private static readonly object s_lock = new object();
        private static bool s_loaded;
        private static nint s_nativeHandle;

        private static GetCell_Delegate? s_getCell;
        private static SetCell_Delegate? s_setCell;
        private static CellToWorld_Delegate? s_cellToWorld;
        private static WorldToCell_Delegate? s_worldToCell;

        private static void EnsureLoaded()
        {
            if (s_loaded) return;
            lock (s_lock)
            {
                if (s_loaded) return;

                s_nativeHandle = LoadNative();
                if (s_nativeHandle == 0)
                    throw new InvalidOperationException("Failed to load native engine (GameEngine.Native)");

                if (!NativeLibrary.TryGetExport(s_nativeHandle, "GE_TileMap_GetInterface", out nint getIfacePtr) || getIfacePtr == 0)
                    throw new MissingMethodException("GE_TileMap_GetInterface export not found in GameEngine.Native");

                var getIface = Marshal.GetDelegateForFunctionPointer<GE_TileMap_GetInterface_Delegate>(getIfacePtr);
                int rc = getIface(kAbi, out nint tablePtr, out uint sizeBytes);
                if (rc != 0 || tablePtr == 0 || sizeBytes == 0)
                    throw new InvalidOperationException($"GE_TileMap_GetInterface failed rc={rc}");

                var iface = Marshal.PtrToStructure<GE_TileMap_Interface_v1>(tablePtr);
                int csSize = Marshal.SizeOf<GE_TileMap_Interface_v1>();
                if (iface.sizeBytes != (uint)csSize)
                    throw new InvalidOperationException("TileMap ABI struct size mismatch");

                s_getCell = iface.GetCell != 0 ? Marshal.GetDelegateForFunctionPointer<GetCell_Delegate>(iface.GetCell) : null;
                s_setCell = iface.SetCell != 0 ? Marshal.GetDelegateForFunctionPointer<SetCell_Delegate>(iface.SetCell) : null;
                s_cellToWorld = iface.CellToWorld != 0 ? Marshal.GetDelegateForFunctionPointer<CellToWorld_Delegate>(iface.CellToWorld) : null;
                s_worldToCell = iface.WorldToCell != 0 ? Marshal.GetDelegateForFunctionPointer<WorldToCell_Delegate>(iface.WorldToCell) : null;

                s_loaded = true;
            }
        }

        private static readonly string[] s_nativeFileNames =
        {
            "GameEngine.Native.dll",        // Windows
            "libGameEngine.Native.dylib",   // macOS
            "GameEngine.Native.dylib",
            "libGameEngine.Native.so",      // Linux
        };

        private static nint LoadNative()
        {
            // Prefer GE_NATIVE_DIR if provided by the native host (ScriptingABI does this).
            string? dir = null;
            try { dir = Environment.GetEnvironmentVariable("GE_NATIVE_DIR"); } catch { dir = null; }

            foreach (var root in new[] { dir, AppContext.BaseDirectory })
            {
                if (string.IsNullOrWhiteSpace(root)) continue;
                foreach (var file in s_nativeFileNames)
                {
                    try
                    {
                        var candidate = Path.Combine(root!, file);
                        if (File.Exists(candidate))
                            return NativeLibrary.Load(candidate);
                    }
                    catch { /* try the next candidate */ }
                }
            }

            return NativeLibrary.Load("GameEngine.Native");
        }

        public static int GetCell(uint entity, int x, int y, out uint cell)
        {
            EnsureLoaded();
            if (s_getCell == null) { cell = 0; return -1; }
            return s_getCell(entity, x, y, out cell);
        }

        public static int SetCell(uint entity, int x, int y, uint cell)
        {
            EnsureLoaded();
            if (s_setCell == null) return -1;
            return s_setCell(entity, x, y, cell);
        }

        public static int CellToWorld(uint entity, int x, int y, out float wx, out float wy, out float wz)
        {
            EnsureLoaded();
            if (s_cellToWorld == null) { wx = wy = wz = 0.0f; return -1; }
            return s_cellToWorld(entity, x, y, out wx, out wy, out wz);
        }

        public static int WorldToCell(uint entity, float wx, float wy, float wz, out int cx, out int cy)
        {
            EnsureLoaded();
            if (s_worldToCell == null) { cx = cy = 0; return -1; }
            return s_worldToCell(entity, wx, wy, wz, out cx, out cy);
        }
    }
}
