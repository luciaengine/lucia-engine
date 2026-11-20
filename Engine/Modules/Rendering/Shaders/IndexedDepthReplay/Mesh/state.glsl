layout(std430, set=0, binding=STATE_BINDING) buffer ReplayState
{
    uint fallbackCount;
    uint selectedCount;
    uint dispatchX;
    uint dispatchY;
    uint dispatchZ;
    uint originalCount;
    uint originalInstances;
    uint retainedInstances;
    uint originalTriangles;
    uint retainedTriangles;
    uint flags;
    uint mergeShift;
    uint slotBegin;
    uint slotEnd;
    uint meshX;
    uint meshY;
    uint meshZ;
} state;

layout(push_constant) uniform Packet
{
    uint recordBase;
    uint cursorIndex;
    uint maxDrawCount;
    uint collectStats;
    uint mergeShift;
    uint emit;
} packet;

uint wordsPerSlot() { return 256u; }
