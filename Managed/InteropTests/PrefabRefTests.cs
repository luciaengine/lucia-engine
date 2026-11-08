using System;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.InteropTests
{
    /// <summary>Prefab references share the native packed GUID representation.</summary>
    public sealed class PrefabRefTests
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct Component
        {
            public byte Enabled;
            public PrefabRef Prefab;
        }

        /// <summary>A prefab field occupies sixteen bytes without adding alignment padding.</summary>
        [Test]
        public void GuidBytesUseNativeOrderAndPackedComponentOffsets()
        {
            var guid = Guid.Parse("00112233-4455-6677-8899-aabbccddeeff");
            var prefab = new PrefabRef(guid);
            Span<byte> bytes = stackalloc byte[16];
            MemoryMarshal.Write(bytes, in prefab);
            Assert.That(bytes.ToArray(), Is.EqualTo(new byte[] {
                0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff }));
            Assert.That(prefab.Guid, Is.EqualTo(guid));
            Assert.That(Marshal.SizeOf<PrefabRef>(), Is.EqualTo(16));
            Assert.That(Marshal.OffsetOf<Component>(nameof(Component.Prefab)).ToInt32(), Is.EqualTo(1));
            Assert.That(Marshal.SizeOf<Component>(), Is.EqualTo(17));
        }

        /// <summary>Empty references and value equality retain GUID semantics.</summary>
        [Test]
        public void EmptyReferenceAndEqualityFollowTheGuid()
        {
            Assert.That(default(PrefabRef).IsNone, Is.True);
            var guid = Guid.Parse("00112233-4455-6677-8899-aabbccddeeff");
            var prefab = new PrefabRef(guid);
            Assert.That(prefab.IsNone, Is.False);
            Assert.That(prefab == new PrefabRef(guid), Is.True);
            Assert.That(prefab != default, Is.True);
        }
    }
}
