using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>
    /// A prefab asset reference for a component field. Assign it in the Inspector to keep
    /// the prefab loaded with the scene for <see cref="EntityCommands.Spawn"/>.
    /// </summary>
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public readonly struct PrefabRef : IEquatable<PrefabRef>
    {
        private readonly ulong m_First;
        private readonly ulong m_Second;

        public PrefabRef(Guid guid)
        {
            Span<byte> bytes = stackalloc byte[16];
            guid.TryWriteBytes(bytes, bigEndian: true, out _);
            m_First = MemoryMarshal.Read<ulong>(bytes);
            m_Second = MemoryMarshal.Read<ulong>(bytes[8..]);
        }

        public Guid Guid
        {
            get
            {
                Span<byte> bytes = stackalloc byte[16];
                MemoryMarshal.Write(bytes, in m_First);
                MemoryMarshal.Write(bytes[8..], in m_Second);
                return new Guid(bytes, bigEndian: true);
            }
        }

        public bool IsNone => m_First == 0 && m_Second == 0;

        public bool Equals(PrefabRef other) => m_First == other.m_First && m_Second == other.m_Second;
        public override bool Equals(object? obj) => obj is PrefabRef other && Equals(other);
        public override int GetHashCode() => HashCode.Combine(m_First, m_Second);
        public override string ToString() => Guid.ToString();
        public static bool operator ==(PrefabRef left, PrefabRef right) => left.Equals(right);
        public static bool operator !=(PrefabRef left, PrefabRef right) => !left.Equals(right);
    }
}
