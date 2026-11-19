const OpTypeVoid = 19;
const OpTypeImage = 25;
const OpTypeSampler = 26;
const OpTypeSampledImage = 27;
const OpTypePointer = 32;
const OpTypeLast = 39;
const OpFunction = 54;
const OpVariable = 59;
const OpDecorate = 71;
const LastDecorationOpcode = 75;
const DecorationBinding = 33;
const DecorationDescriptorSet = 34;
const SamplerBindingOffset = 64;
const DecorationNonReadable = 25;
const StorageClassUniform = 2;
const StorageClassStorageBuffer = 12;

const opcode = instruction => instruction[0] & 0xffff;
const canReferenceType = instruction => {
    const kind = opcode(instruction);
    return kind >= OpTypeVoid && (kind < OpDecorate || kind > LastDecorationOpcode);
};

export function normalizeSpirv(bytes) {
    const aligned = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    const words = new Uint32Array(aligned);
    if (words.length < 5 || words[0] !== 0x07230203) {
        throw new Error('Invalid SPIR-V header');
    }
    const instructions = [];
    for (let offset = 5; offset < words.length;) {
        const count = words[offset] >>> 16;
        if (!count || offset + count > words.length) throw new Error('Invalid SPIR-V instruction');
        instructions.push(words.subarray(offset, offset + count));
        offset += count;
    }
    const functionIndex = instructions.findIndex(item => opcode(item) === OpFunction);
    const globalCount = functionIndex < 0 ? instructions.length : functionIndex;

    const definitions = instructions.slice(0, globalCount);
    const samplerTypes = new Set(definitions.filter(item => opcode(item) === OpTypeSampler).map(item => item[1]));
    const samplerDefinitions = definitions.filter(item => opcode(item) === OpTypeSampler ||
        (opcode(item) === OpTypePointer && samplerTypes.has(item[3])));
    const hoisted = new Set(samplerDefinitions.map(item => item[1]));
    const firstSamplerUse = definitions.findIndex(item => !samplerDefinitions.includes(item) &&
        canReferenceType(item) && item.subarray(1).some(word => hoisted.has(word)));
    if (firstSamplerUse >= 0 && firstSamplerUse <= definitions.indexOf(samplerDefinitions[0])) {
        const remaining = definitions.filter(item => !samplerDefinitions.includes(item));
        remaining.splice(firstSamplerUse, 0, ...samplerDefinitions);
        instructions.splice(0, globalCount, ...remaining);
    }

    // Re-evaluate positions as each pointer is moved ahead of its first use.
    while (true) {
        const positions = new Map();
        for (let index = 0; index < globalCount; index += 1) {
            const instruction = instructions[index];
            const kind = opcode(instruction);
            if (kind >= OpTypeVoid && kind <= OpTypeLast && instruction.length >= 2) {
                positions.set(instruction[1], index);
            }
        }
        let moved = false;
        for (let index = 0; index < globalCount && !moved; index += 1) {
            const instruction = instructions[index];
            if (!canReferenceType(instruction)) continue;
            for (const operand of instruction.subarray(1)) {
                const definition = positions.get(operand);
                if (definition === undefined || definition <= index) continue;
                const pointer = instructions[definition];
                const pointee = positions.get(pointer[3]);
                if (opcode(pointer) !== OpTypePointer || pointee === undefined || pointee >= index) continue;
                instructions.splice(index, 0, ...instructions.splice(definition, 1));
                moved = true;
                break;
            }
        }
        if (!moved) break;
    }

    const buffers = new Set(instructions.filter(item => opcode(item) === OpVariable &&
        (item[3] === StorageClassUniform || item[3] === StorageClassStorageBuffer)).map(item => item[2]));
    const normalized = new Uint32Array(words.length);
    normalized.set(words.subarray(0, 5));
    let offset = 5;
    for (const instruction of instructions) {
        const forBuffer = opcode(instruction) === OpDecorate && buffers.has(instruction[1]);
        if (forBuffer && instruction[2] === DecorationNonReadable) continue;
        normalized.set(instruction, offset);
        offset += instruction.length;
    }
    return remapSamplerBindings(normalized.subarray(0, offset));
}

// The splitter leaves the new sampler in the image slot. An already separate
// sampler can keep its own binding.
function remapSamplerBindings(words) {
    const types = new Map();
    const variables = new Map();
    const bindings = new Map();
    const descriptorSets = new Map();
    for (let offset = 5; offset < words.length;) {
        const count = words[offset] >>> 16;
        const kind = words[offset] & 0xffff;
        const result = words[offset + 1];
        if (kind === OpTypeImage || kind === OpTypeSampledImage) {
            types.set(result, 'image');
        } else if (kind === OpTypeSampler) {
            types.set(result, 'sampler');
        } else if (kind === OpTypePointer) {
            types.set(result, types.get(words[offset + 3]));
        } else if (kind === OpVariable) {
            variables.set(words[offset + 2], types.get(result));
        } else if (kind === OpDecorate) {
            const decoration = words[offset + 2];
            if (decoration === DecorationBinding) {
                bindings.set(result, offset + 3);
            } else if (decoration === DecorationDescriptorSet) {
                descriptorSets.set(result, words[offset + 3]);
            }
        }
        offset += count;
    }

    const slot = variable => `${descriptorSets.get(variable) ?? 0}/${words[bindings.get(variable)]}`;
    const imageSlots = new Set();
    for (const [variable, type] of variables) {
        if (type === 'image' && bindings.has(variable)) {
            imageSlots.add(slot(variable));
        }
    }
    for (const [variable, type] of variables) {
        if (type === 'sampler' && bindings.has(variable) && imageSlots.has(slot(variable))) {
            words[bindings.get(variable)] += SamplerBindingOffset;
        }
    }
    return words;
}
