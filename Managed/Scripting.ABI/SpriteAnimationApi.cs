using System;
using System.Runtime.InteropServices;
using GameEngine.ECS;

namespace GameEngine.Scripting
{
    /// <summary>Playback notifications for AnimatedSprite2D components.</summary>
    public static class SpriteAnimationApi
    {
        /// <summary>
        /// Consumes the latest update's notifications once. Call from the main thread each frame.
        /// Large steps produce one summary, with the final frame and a signed loop count.
        /// The next playback update replaces any unconsumed notifications.
        /// </summary>
        public static SpriteAnimationEvents PollEvents(WorldHandle world, uint entityId)
        {
            int result = Native.PollEvents(world.Handle, entityId, out SpriteAnimationEvents events);
            if (result != 0)
                throw new InvalidOperationException(
                    $"Sprite PollEvents failed ({result}). Call on the main thread with a live AnimatedSprite2D entity.");
            return events;
        }

        private static class Native
        {
            [DllImport("GameEngine.Native", EntryPoint = "GE_SpriteAnimation_PollEvents", CallingConvention = CallingConvention.Cdecl)]
            internal static extern int PollEvents(ulong worldHandle, ulong entityId, out SpriteAnimationEvents events);
        }
    }
}
