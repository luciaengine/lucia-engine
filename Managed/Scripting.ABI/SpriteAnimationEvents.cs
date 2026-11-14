using System;
using System.Runtime.InteropServices;

namespace GameEngine.Scripting
{
    /// <summary>Playback notifications from one AnimatedSprite2D update.</summary>
    [Flags]
    public enum SpriteAnimationEvent : uint
    {
        /// <summary>No pending notification.</summary>
        None = 0,
        /// <summary>The visible frame changed or playback crossed a loop boundary.</summary>
        FrameChanged = 1,
        /// <summary>Playback crossed one or more loop boundaries.</summary>
        Looped = 2,
        /// <summary>A one-shot reached its terminal frame and stopped playing.</summary>
        Finished = 4,
    }

    /// <summary>Summarizes one update. Payload fields are valid when Flags is not None.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct SpriteAnimationEvents
    {
        /// <summary>Notifications produced by the update.</summary>
        public SpriteAnimationEvent Flags;
        /// <summary>The final visible frame's zero-based index.</summary>
        public uint FrameIndex;
        /// <summary>Loop boundaries crossed: positive forwards, negative backwards.</summary>
        public long LoopCount;
        /// <summary>The normalized playback clock after the update, in seconds.</summary>
        public float TimeSeconds;
    }
}
