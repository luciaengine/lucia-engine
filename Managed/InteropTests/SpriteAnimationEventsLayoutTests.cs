using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using GameEngine.Scripting;
using NUnit.Framework;

namespace GameEngine.ManagedInteropTests
{
    /// <summary>Checks the snapshot layout written by the native sprite event export.</summary>
    public class SpriteAnimationEventsLayoutTests
    {
        /// <summary>The record uses the native offsets and eight-byte loop-count alignment.</summary>
        [Test]
        public void MatchesTheNativeSnapshotLayout()
        {
            Assert.That(Unsafe.SizeOf<SpriteAnimationEvents>(), Is.EqualTo(24));
            Assert.That(Marshal.OffsetOf<SpriteAnimationEvents>(nameof(SpriteAnimationEvents.Flags)).ToInt32(), Is.EqualTo(0));
            Assert.That(Marshal.OffsetOf<SpriteAnimationEvents>(nameof(SpriteAnimationEvents.FrameIndex)).ToInt32(), Is.EqualTo(4));
            Assert.That(Marshal.OffsetOf<SpriteAnimationEvents>(nameof(SpriteAnimationEvents.LoopCount)).ToInt32(), Is.EqualTo(8));
            Assert.That(Marshal.OffsetOf<SpriteAnimationEvents>(nameof(SpriteAnimationEvents.TimeSeconds)).ToInt32(), Is.EqualTo(16));
        }
    }
}
