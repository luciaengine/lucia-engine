struct type_11 {
    member: f32,
    member_1: u32,
    member_2: f32,
    member_3: f32,
}

struct type_14 {
    member: vec3<f32>,
    member_1: f32,
    member_2: vec3<f32>,
    member_3: f32,
    member_4: vec3<f32>,
    member_5: f32,
    member_6: vec3<f32>,
    member_7: f32,
    member_8: vec3<f32>,
    member_9: f32,
    member_10: vec3<f32>,
    member_11: f32,
    member_12: f32,
    member_13: f32,
    member_14: f32,
    member_15: f32,
    member_16: vec3<f32>,
    member_17: f32,
    member_18: f32,
    member_19: f32,
    member_20: f32,
    member_21: f32,
    member_22: vec2<f32>,
    member_23: f32,
    member_24: f32,
    member_25: vec4<f32>,
    member_26: vec4<f32>,
    member_27: vec4<f32>,
    member_28: vec4<f32>,
    member_29: vec4<f32>,
    member_30: vec4<f32>,
}

struct type_18 {
    member: f32,
    member_1: f32,
    member_2: f32,
    member_3: f32,
    member_4: vec3<f32>,
    member_5: f32,
    member_6: vec3<f32>,
    member_7: f32,
    member_8: vec3<f32>,
    member_9: f32,
    member_10: vec3<f32>,
    member_11: vec3<f32>,
    member_12: vec3<f32>,
    member_13: vec3<f32>,
    member_14: f32,
    member_15: f32,
    member_16: f32,
    member_17: f32,
    member_18: f32,
    member_19: vec2<f32>,
    member_20: f32,
    member_21: f32,
    member_22: f32,
    member_23: vec3<f32>,
    member_24: u32,
    member_25: f32,
    member_26: vec3<f32>,
    member_27: f32,
    member_28: vec3<f32>,
    member_29: f32,
    member_30: vec3<f32>,
    member_31: f32,
}

@group(0) @binding(5)
var<storage> global: type_11;
@group(0) @binding(0)
var<uniform> global_1: type_14;
@group(0) @binding(65)
var global_2: sampler;
@group(0) @binding(1)
var global_3: texture_2d<f32>;
@group(0) @binding(2)
var<uniform> global_4: type_18;
@group(0) @binding(67)
var global_5: sampler;
@group(0) @binding(3)
var global_6: texture_2d<f32>;
var<private> global_7: vec4<f32>;
@group(0) @binding(68)
var global_8: sampler;
@group(0) @binding(4)
var global_9: texture_2d<f32>;
var<private> global_10: vec2<f32>;

fn function_() {
    var phi_6523_: vec3<f32>;
    var phi_6481_: vec3<f32>;
    var phi_6482_: vec3<f32>;
    var phi_6483_: vec3<f32>;
    var phi_6487_: vec3<f32>;
    var phi_6485_: f32;
    var phi_6488_: vec3<f32>;
    var phi_6489_: vec3<f32>;
    var local: bool;
    var local_1: f32;
    var phi_6498_: vec3<f32>;
    var phi_6496_: f32;
    var phi_6499_: vec3<f32>;
    var phi_6500_: vec3<f32>;
    var local_2: bool;
    var local_3: f32;
    var phi_6504_: vec3<f32>;
    var phi_6502_: f32;
    var phi_6505_: vec3<f32>;
    var phi_6506_: vec3<f32>;
    var phi_6507_: vec3<f32>;
    var phi_3088_: bool;
    var phi_6508_: vec2<f32>;
    var phi_6513_: f32;
    var phi_6509_: f32;
    var phi_6519_: vec3<f32>;
    var phi_6515_: vec2<f32>;
    var phi_6514_: vec2<f32>;
    var phi_6516_: f32;
    var phi_6522_: vec3<f32>;
    var phi_6521_: vec3<f32>;
    var phi_6408_: f32;
    var phi_5010_: bool;
    var phi_6404_: f32;
    var phi_6405_: vec3<f32>;
    var phi_6410_: f32;
    var phi_6470_: vec3<f32>;
    var phi_6412_: f32;
    var phi_6469_: vec3<f32>;
    var phi_6411_: f32;
    var phi_6414_: vec3<f32>;
    var phi_6413_: i32;
    var local_4: vec3<f32>;
    var phi_6424_: vec3<f32>;
    var phi_6427_: vec3<f32>;
    var phi_6425_: f32;
    var phi_6428_: vec3<f32>;
    var phi_6430_: vec3<f32>;
    var phi_6429_: i32;
    var local_5: vec3<f32>;
    var phi_6433_: f32;
    var phi_6432_: bool;
    var phi_6459_: f32;
    var phi_6451_: vec3<f32>;
    var phi_6046_: bool;
    var phi_6452_: f32;
    var phi_6453_: vec3<f32>;
    var phi_6461_: vec3<f32>;
    var phi_6462_: vec3<f32>;
    var phi_6463_: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e141 = global_10;
            let _e144 = ((_e141.x * 2f) - 1f);
            let _e148 = (((1f - _e141.y) * 2f) - 1f);
            let _e150 = global_1.member_21;
            let _e152 = global_1.member_20;
            let _e153 = max(_e152, 0.001f);
            if (_e150 <= 0f) {
                switch bitcast<i32>(0u) {
                    default: {
                        let _e157 = global_1.member_23;
                        let _e158 = clamp(_e157, 0f, 1f);
                        let _e160 = global_1.member_7;
                        let _e162 = (_e148 - clamp(_e160, -1f, 1f));
                        let _e165 = clamp(((1f - _e162) * 0.5f), 0f, 1f);
                        let _e168 = global_1.member_28[3u];
                        if (_e168 >= 0.5f) {
                            if (_e165 < 0.5f) {
                                let _e172 = global_1.member_28;
                                let _e175 = global_1.member_29;
                                phi_6523_ = mix(_e172.xyz, _e175.xyz, vec3(smoothstep(0f, 0.5f, _e165)));
                            } else {
                                let _e181 = global_1.member_29;
                                let _e184 = global_1.member_30;
                                phi_6523_ = mix(_e181.xyz, _e184.xyz, vec3(smoothstep(0.5f, 1f, _e165)));
                            }
                            let _e190 = phi_6523_;
                            let _e192 = global_1.member_1;
                            let _e195 = global.member_2;
                            let _e199 = (_e190 * (exp2(_e192) * select(1f, _e195, (_e195 > 0f))));
                            global_7 = vec4<f32>(_e199.x, _e199.y, _e199.z, 1f);
                            break;
                        }
                        let _e205 = global_4.member_30;
                        let _e207 = global_1.member;
                        let _e209 = normalize((_e207 - _e205));
                        let _e211 = global_1.member_6;
                        let _e214 = (_e211 - (_e209 * dot(_e211, _e209)));
                        phi_6481_ = _e214;
                        if (dot(_e214, _e214) < 0.000001f) {
                            let _e218 = global_1.member_4;
                            phi_6481_ = (_e218 - (_e209 * dot(_e218, _e209)));
                        }
                        let _e223 = phi_6481_;
                        phi_6482_ = _e223;
                        if (dot(_e223, _e223) < 0.000001f) {
                            let _e227 = global_1.member_2;
                            phi_6482_ = (_e227 - (_e209 * dot(_e227, _e209)));
                        }
                        let _e232 = phi_6482_;
                        phi_6483_ = _e232;
                        if (dot(_e232, _e232) < 0.000001f) {
                            let _e239 = select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 0f, 1f), vec3((abs(_e209.z) < 0.98f)));
                            phi_6483_ = (_e239 - (_e209 * dot(_e239, _e209)));
                        }
                        let _e244 = phi_6483_;
                        let _e245 = normalize(_e244);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e249 = global_1.member_26[1u];
                                let _e250 = (_e249 >= 0f);
                                local = _e250;
                                local_1 = _e249;
                                local_2 = _e250;
                                local_3 = _e249;
                                if _e250 {
                                    let _e253 = global_1.member_26[0u];
                                    let _e254 = sin(_e253);
                                    let _e255 = cos(_e253);
                                    let _e272 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e254 * _e209.x) + (_e255 * _e209.z)), ((_e255 * _e209.x) - (_e254 * _e209.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e209.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    phi_6489_ = (_e272.xyz * _e249);
                                    break;
                                }
                                let _e281 = normalize(cross(_e209, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e209.z) > 0.98f)))));
                                let _e285 = dot(_e209, _e209);
                                let _e295 = ((((asin(clamp(_e285, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e301 = (0.5f + ((0.5f * sign(_e295)) * sqrt(abs(_e295))));
                                let _e302 = vec2<f32>(((atan2(dot(_e209, normalize(cross(_e281, _e209))), dot(_e209, _e281)) * 0.15915494f) + 0.5f), _e301);
                                let _e307 = textureSampleLevel(global_3, global_2, vec2<f32>(_e302.x, clamp(_e301, 0.001953125f, 0.9980469f)), 0f);
                                let _e310 = global_4.member_24;
                                let _e311 = (_e310 == 2u);
                                let _e313 = global_4.member_8;
                                let _e315 = global_4.member_9;
                                let _e317 = global_4.member_10;
                                let _e319 = global_4.member_18;
                                let _e321 = global_4.member_23;
                                let _e323 = global_4.member_17;
                                let _e325 = global_4.member_13;
                                let _e328 = clamp(_e158, 0f, 1f);
                                let _e330 = mix((_e313 * max(_e315, 0f)), _e317, vec3(_e328));
                                phi_6487_ = _e307.xyz;
                                if (_e311 && (_e285 < 0f)) {
                                    phi_6487_ = mix(mix(_e330, _e321, vec3(clamp(_e319, 0f, 1f))), _e330, vec3(clamp((-(_e285) * max(_e323, 0.001f)), 0f, 1f)));
                                }
                                let _e343 = phi_6487_;
                                phi_6488_ = _e343;
                                if ((_e158 > 0f) && (_e285 > select(-1f, 0f, _e311))) {
                                    if _e311 {
                                        phi_6485_ = max(_e285, 0f);
                                    } else {
                                        phi_6485_ = abs(_e285);
                                    }
                                    let _e351 = phi_6485_;
                                    let _e359 = fract((((normalize(_e209) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e364 = (_e359 + vec3(dot(_e359, (_e359.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_6488_ = (_e343 + ((mix(_e325, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e351))) * (1f + ((fract(((_e364.x * _e364.y) * _e364.z)) - 0.5f) * 0.05f))) * _e328));
                                }
                                let _e378 = phi_6488_;
                                phi_6489_ = _e378;
                                break;
                            }
                        }
                        let _e380 = phi_6489_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e383 = local;
                                if _e383 {
                                    let _e386 = global_1.member_26[0u];
                                    let _e387 = sin(_e386);
                                    let _e388 = cos(_e386);
                                    let _e405 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e387 * _e245.x) + (_e388 * _e245.z)), ((_e388 * _e245.x) - (_e387 * _e245.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e245.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e408 = local_1;
                                    phi_6500_ = (_e405.xyz * _e408);
                                    break;
                                }
                                let _e416 = normalize(cross(_e209, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e209.z) > 0.98f)))));
                                let _e420 = dot(_e245, _e209);
                                let _e430 = ((((asin(clamp(_e420, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e436 = (0.5f + ((0.5f * sign(_e430)) * sqrt(abs(_e430))));
                                let _e437 = vec2<f32>(((atan2(dot(_e245, normalize(cross(_e416, _e209))), dot(_e245, _e416)) * 0.15915494f) + 0.5f), _e436);
                                let _e442 = textureSampleLevel(global_3, global_2, vec2<f32>(_e437.x, clamp(_e436, 0.001953125f, 0.9980469f)), 0f);
                                let _e445 = global_4.member_24;
                                let _e446 = (_e445 == 2u);
                                let _e448 = global_4.member_8;
                                let _e450 = global_4.member_9;
                                let _e452 = global_4.member_10;
                                let _e454 = global_4.member_18;
                                let _e456 = global_4.member_23;
                                let _e458 = global_4.member_17;
                                let _e460 = global_4.member_13;
                                let _e463 = clamp(_e158, 0f, 1f);
                                let _e465 = mix((_e448 * max(_e450, 0f)), _e452, vec3(_e463));
                                phi_6498_ = _e442.xyz;
                                if (_e446 && (_e420 < 0f)) {
                                    phi_6498_ = mix(mix(_e465, _e456, vec3(clamp(_e454, 0f, 1f))), _e465, vec3(clamp((-(_e420) * max(_e458, 0.001f)), 0f, 1f)));
                                }
                                let _e478 = phi_6498_;
                                phi_6499_ = _e478;
                                if ((_e158 > 0f) && (_e420 > select(-1f, 0f, _e446))) {
                                    if _e446 {
                                        phi_6496_ = max(_e420, 0f);
                                    } else {
                                        phi_6496_ = abs(_e420);
                                    }
                                    let _e486 = phi_6496_;
                                    let _e494 = fract((((normalize(_e245) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e499 = (_e494 + vec3(dot(_e494, (_e494.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_6499_ = (_e478 + ((mix(_e460, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e486))) * (1f + ((fract(((_e499.x * _e499.y) * _e499.z)) - 0.5f) * 0.05f))) * _e463));
                                }
                                let _e513 = phi_6499_;
                                phi_6500_ = _e513;
                                break;
                            }
                        }
                        let _e515 = phi_6500_;
                        let _e516 = -(_e209);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e519 = local_2;
                                if _e519 {
                                    let _e522 = global_1.member_26[0u];
                                    let _e523 = sin(_e522);
                                    let _e524 = cos(_e522);
                                    let _e541 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e523 * _e516.x) + (_e524 * _e516.z)), ((_e524 * _e516.x) - (_e523 * _e516.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e516.y, -1f, 1f)) * 0.31830987f)), 0f);
                                    let _e544 = local_3;
                                    phi_6506_ = (_e541.xyz * _e544);
                                    break;
                                }
                                let _e552 = normalize(cross(_e209, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e209.z) > 0.98f)))));
                                let _e556 = dot(_e516, _e209);
                                let _e566 = ((((asin(clamp(_e556, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                                let _e572 = (0.5f + ((0.5f * sign(_e566)) * sqrt(abs(_e566))));
                                let _e573 = vec2<f32>(((atan2(dot(_e516, normalize(cross(_e552, _e209))), dot(_e516, _e552)) * 0.15915494f) + 0.5f), _e572);
                                let _e578 = textureSampleLevel(global_3, global_2, vec2<f32>(_e573.x, clamp(_e572, 0.001953125f, 0.9980469f)), 0f);
                                let _e581 = global_4.member_24;
                                let _e582 = (_e581 == 2u);
                                let _e584 = global_4.member_8;
                                let _e586 = global_4.member_9;
                                let _e588 = global_4.member_10;
                                let _e590 = global_4.member_18;
                                let _e592 = global_4.member_23;
                                let _e594 = global_4.member_17;
                                let _e596 = global_4.member_13;
                                let _e599 = clamp(_e158, 0f, 1f);
                                let _e601 = mix((_e584 * max(_e586, 0f)), _e588, vec3(_e599));
                                phi_6504_ = _e578.xyz;
                                if (_e582 && (_e556 < 0f)) {
                                    phi_6504_ = mix(mix(_e601, _e592, vec3(clamp(_e590, 0f, 1f))), _e601, vec3(clamp((-(_e556) * max(_e594, 0.001f)), 0f, 1f)));
                                }
                                let _e614 = phi_6504_;
                                phi_6505_ = _e614;
                                if ((_e158 > 0f) && (_e556 > select(-1f, 0f, _e582))) {
                                    if _e582 {
                                        phi_6502_ = max(_e556, 0f);
                                    } else {
                                        phi_6502_ = abs(_e556);
                                    }
                                    let _e622 = phi_6502_;
                                    let _e630 = fract((((normalize(_e516) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                                    let _e635 = (_e630 + vec3(dot(_e630, (_e630.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                                    phi_6505_ = (_e614 + ((mix(_e596, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e622))) * (1f + ((fract(((_e635.x * _e635.y) * _e635.z)) - 0.5f) * 0.05f))) * _e599));
                                }
                                let _e649 = phi_6505_;
                                phi_6506_ = _e649;
                                break;
                            }
                        }
                        let _e651 = phi_6506_;
                        if (_e165 < 0.5f) {
                            phi_6507_ = mix(_e380, _e515, vec3(smoothstep(0f, 0.5f, _e165)));
                        } else {
                            phi_6507_ = mix(_e515, _e651, vec3(smoothstep(0.5f, 1f, _e165)));
                        }
                        let _e660 = phi_6507_;
                        let _e662 = global_4.member_15;
                        let _e664 = global_4.member_16;
                        let _e673 = mix(_e660, _e515, vec3(clamp((1f - smoothstep(0f, max((mix(_e662, _e664, _e158) * 3f), 0.015f), abs(_e162))), 0f, 1f)));
                        let _e675 = global_1.member_8;
                        let _e676 = normalize(_e675);
                        let _e678 = global_1.member_9;
                        let _e679 = (_e678 > 0f);
                        phi_3088_ = _e679;
                        if _e679 {
                            phi_3088_ = (_e676.y > -0.18f);
                        }
                        let _e683 = phi_3088_;
                        phi_6519_ = _e673;
                        if (_e683 && (_e158 < 0.98f)) {
                            let _e687 = global_1.member_2;
                            let _e688 = normalize(_e687);
                            let _e690 = global_1.member_4;
                            let _e691 = normalize(_e690);
                            let _e694 = global_1.member_3;
                            let _e698 = max(((_e678 * max(_e694, 0.1f)) * 16f), 0.055f);
                            let _e700 = global_1.member_13;
                            if (_e700 > 0.5f) {
                                let _e704 = vec2<f32>(dot(_e676, _e688), dot(_e676, _e691));
                                let _e705 = length(_e704);
                                phi_6508_ = _e704;
                                if (_e705 > 0.82f) {
                                    phi_6508_ = (_e704 * (0.82f / _e705));
                                }
                                let _e710 = phi_6508_;
                                let _e717 = vec2<f32>(((_e144 - (_e710.x / _e153)) * _e153), (_e162 - _e710.y));
                                let _e719 = -(dot(_e717, _e717));
                                let _e720 = (_e698 * _e698);
                                phi_6513_ = exp((_e719 / max(_e720, 0.000001f)));
                                phi_6509_ = exp((_e719 / max((_e720 * 26f), 0.000001f)));
                            } else {
                                let _e734 = vec2<f32>(((_e144 - (dot(_e676, _e688) / _e153)) * _e153), (_e162 - dot(_e676, _e691)));
                                let _e736 = smoothstep(-0.05f, 0.05f, dot(_e676, normalize(_e211)));
                                let _e738 = -(dot(_e734, _e734));
                                let _e739 = (_e698 * _e698);
                                phi_6513_ = (exp((_e738 / max(_e739, 0.000001f))) * _e736);
                                phi_6509_ = (exp((_e738 / max((_e739 * 26f), 0.000001f))) * _e736);
                            }
                            let _e750 = phi_6513_;
                            let _e752 = phi_6509_;
                            let _e754 = smoothstep(-0.05f, 0.05f, _e676.y);
                            let _e755 = smoothstep(-0.006f, 0.006f, _e162);
                            let _e756 = (1f - _e158);
                            let _e760 = global_1.member_16;
                            let _e764 = global_1.member_17;
                            phi_6519_ = ((_e673 + (((mix(_e760, vec3<f32>(1f, 0.48f, 0.18f), vec3((1f - smoothstep(0.04f, 0.55f, _e676.y)))) * ((clamp((_e764 * 0.018f), 0f, 1.2f) * _e754) * _e756)) * _e752) * _e755)) + (((_e760 * (((_e764 * 0.4f) * _e754) * _e756)) * _e750) * _e755));
                        }
                        let _e781 = phi_6519_;
                        let _e783 = global_1.member_10;
                        let _e784 = normalize(_e783);
                        let _e786 = smoothstep(-0.18f, 0.02f, _e784.y);
                        let _e788 = global_1.member_11;
                        phi_6521_ = _e781;
                        if ((_e788 > 0f) && (_e786 > 0f)) {
                            let _e793 = global_1.member_2;
                            let _e794 = normalize(_e793);
                            let _e796 = global_1.member_4;
                            let _e797 = normalize(_e796);
                            let _e800 = vec2<f32>(dot(_e784, _e794), dot(_e784, _e797));
                            let _e802 = global_1.member_13;
                            phi_6514_ = _e800;
                            if (_e802 > 0.5f) {
                                let _e804 = length(_e800);
                                phi_6515_ = _e800;
                                if (_e804 > 0.82f) {
                                    phi_6515_ = (_e800 * (0.82f / _e804));
                                }
                                let _e809 = phi_6515_;
                                phi_6514_ = _e809;
                            }
                            let _e811 = phi_6514_;
                            let _e816 = global_1.member_12;
                            let _e819 = global_1.member_5;
                            let _e822 = max(((_e816 * 6f) * max(_e819, 0.1f)), 0.05f);
                            let _e826 = vec2<f32>(((_e144 - (_e811.x / _e153)) * _e153), (_e162 - _e811.y));
                            let _e829 = smoothstep(-0.006f, 0.006f, _e162);
                            let _e830 = smoothstep(1f, 0.92f, (length(_e826) / _e822));
                            phi_6522_ = _e781;
                            if ((_e830 * _e829) > 0f) {
                                let _e836 = (((_e826 / vec2(_e822)) * 0.5f) + vec2<f32>(0.5f, 0.5f));
                                let _e841 = vec2<f32>(_e836.x, (1f - _e836.y));
                                let _e842 = textureSampleLevel(global_9, global_8, _e841, 0f);
                                let _e846 = dot(_e676, _e784);
                                let _e850 = global_1.member_15;
                                switch bitcast<i32>(0u) {
                                    default: {
                                        let _e854 = (((_e841 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                                        let _e855 = dot(_e854, _e854);
                                        if (_e855 > 1f) {
                                            phi_6516_ = 0f;
                                            break;
                                        }
                                        phi_6516_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e854.x, _e854.y, sqrt(max(0f, (1f - _e855)))), normalize(vec3<f32>(dot(_e676, _e794), -(dot(_e676, _e797)), -(_e846)))));
                                        break;
                                    }
                                }
                                let _e867 = phi_6516_;
                                phi_6522_ = (_e781 + (((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e846 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e867)) * (((_e788 / (0.000064439846f * max((0.5f * (1f - _e846)), 0.001f))) * (dot(_e842.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e850))) * _e830) * _e842.w) * _e829) * _e786));
                            }
                            let _e892 = phi_6522_;
                            phi_6521_ = _e892;
                        }
                        let _e894 = phi_6521_;
                        let _e896 = global_1.member_1;
                        let _e899 = global.member_2;
                        let _e903 = (_e894 * (exp2(_e896) * select(1f, _e899, (_e899 > 0f))));
                        global_7 = vec4<f32>(_e903.x, _e903.y, _e903.z, 1f);
                        break;
                    }
                }
                break;
            }
            let _e909 = global_1.member_2;
            let _e912 = global_1.member_4;
            let _e915 = global_1.member_6;
            let _e921 = normalize(vec3<f32>(((_e144 * _e153) * _e150), (_e148 * _e150), -1f));
            let _e931 = normalize((((normalize(_e909) * _e921.x) + (normalize(_e912) * _e921.y)) + (normalize(_e915) * -(_e921.z))));
            let _e934 = global_1.member_28[3u];
            if (_e934 >= 0.5f) {
                let _e937 = max(_e931.y, 0f);
                let _e939 = max(-(_e931.y), 0f);
                let _e943 = global_1.member_28;
                let _e947 = global_1.member_29;
                let _e952 = global_1.member_30;
                let _e957 = global_1.member_1;
                let _e960 = global.member_2;
                let _e964 = ((((_e943.xyz * _e937) + (_e947.xyz * ((1f - _e937) - _e939))) + (_e952.xyz * _e939)) * (exp2(_e957) * select(1f, _e960, (_e960 > 0f))));
                global_7 = vec4<f32>(_e964.x, _e964.y, _e964.z, 1f);
                break;
            }
            let _e971 = global_1.member_26[1u];
            if (_e971 >= 0f) {
                let _e975 = global_1.member_26[0u];
                let _e976 = sin(_e975);
                let _e977 = cos(_e975);
                let _e994 = textureSampleLevel(global_3, global_2, vec2<f32>(((atan2(((_e976 * _e931.x) + (_e977 * _e931.z)), ((_e977 * _e931.x) - (_e976 * _e931.z))) * 0.15915494f) + 0.5f), (acos(clamp(_e931.y, -1f, 1f)) * 0.31830987f)), 0f);
                let _e998 = global_1.member_1;
                let _e1001 = global.member_2;
                let _e1005 = ((_e994.xyz * _e971) * (exp2(_e998) * select(1f, _e1001, (_e1001 > 0f))));
                global_7 = vec4<f32>(_e1005.x, _e1005.y, _e1005.z, 1f);
                break;
            }
            let _e1011 = global_4.member_30;
            let _e1013 = global_1.member;
            let _e1014 = (_e1013 - _e1011);
            let _e1015 = length(_e1014);
            let _e1018 = (_e1014 / vec3(max(_e1015, 0.00001f)));
            let _e1019 = dot(_e931, _e1018);
            let _e1021 = global_1.member_8;
            let _e1022 = normalize(_e1021);
            let _e1024 = global_1.member_23;
            let _e1025 = clamp(_e1024, 0f, 1f);
            let _e1032 = normalize(cross(_e1018, select(vec3<f32>(0f, 0f, 1f), vec3<f32>(1f, 0f, 0f), vec3((abs(_e1018.z) > 0.98f)))));
            let _e1034 = normalize(cross(_e1032, _e1018));
            let _e1036 = global_1.member_11;
            let _e1039 = global_1.member_10;
            let _e1040 = normalize(_e1039);
            let _e1042 = global_1.member_12;
            let _e1045 = max(0.000001f, select(0.026f, _e1042, (_e1042 > 0f)));
            let _e1052 = normalize(cross(select(vec3<f32>(1f, 0f, 0f), vec3<f32>(0f, 1f, 0f), vec3((abs(_e1040.y) < 0.99f))), _e1040));
            let _e1053 = cross(_e1040, _e1052);
            let _e1064 = ((((asin(clamp(_e1019, -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
            let _e1070 = (0.5f + ((0.5f * sign(_e1064)) * sqrt(abs(_e1064))));
            let _e1071 = vec2<f32>(((atan2(dot(_e931, _e1034), dot(_e931, _e1032)) * 0.15915494f) + 0.5f), _e1070);
            let _e1076 = textureDimensions(global_3, 0i);
            let _e1078 = vec2<f32>(vec2<i32>(_e1076));
            let _e1080 = ((vec2<f32>(_e1071.x, clamp(_e1070, 0.001953125f, 0.9980469f)) * _e1078) - vec2<f32>(0.5f, 0.5f));
            let _e1081 = floor(_e1080);
            let _e1082 = (_e1080 - _e1081);
            let _e1083 = (_e1082 * _e1082);
            let _e1084 = (_e1083 * _e1082);
            let _e1085 = (_e1082 * 3f);
            let _e1087 = (_e1083 * 3f);
            let _e1093 = (_e1084 * 3f);
            let _e1095 = (((vec2<f32>(4f, 4f) - (_e1083 * 6f)) + _e1093) * vec2<f32>(0.16666667f, 0.16666667f));
            let _e1100 = (_e1084 * vec2<f32>(0.16666667f, 0.16666667f));
            let _e1101 = (((((vec2<f32>(1f, 1f) - _e1085) + _e1087) - _e1084) * vec2<f32>(0.16666667f, 0.16666667f)) + _e1095);
            let _e1102 = (((((vec2<f32>(1f, 1f) + _e1085) + _e1087) - _e1093) * vec2<f32>(0.16666667f, 0.16666667f)) + _e1100);
            let _e1106 = (((_e1081 - vec2<f32>(0.5f, 0.5f)) + (_e1095 / _e1101)) / _e1078);
            let _e1110 = (((_e1081 + vec2<f32>(1.5f, 1.5f)) + (_e1100 / _e1102)) / _e1078);
            let _e1113 = (0.5f / _e1078.y);
            let _e1114 = (1f - _e1113);
            let _e1115 = clamp(_e1106.y, _e1113, _e1114);
            let _e1117 = clamp(_e1110.y, _e1113, _e1114);
            let _e1122 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1106.x, _e1115), 0f);
            let _e1128 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1110.x, _e1115), 0f);
            let _e1135 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1106.x, _e1117), 0f);
            let _e1139 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1110.x, _e1117), 0f);
            let _e1144 = ((((_e1122.xyz * _e1101.x) + (_e1128.xyz * _e1102.x)) * _e1101.y) + (((_e1135.xyz * _e1101.x) + (_e1139.xyz * _e1102.x)) * _e1102.y));
            let _e1145 = dot(_e931, _e1022);
            let _e1147 = global_1.member_9;
            phi_6408_ = 0f;
            if ((_e1147 > 0f) && (_e1145 > 0f)) {
                let _e1153 = max(_e1147, 0.000001f);
                let _e1154 = (_e1153 * 0.25f);
                phi_6408_ = (clamp((1f - smoothstep((_e1153 - _e1154), (_e1153 + _e1154), acos(clamp(_e1145, -1f, 1f)))), 0f, 1f) * (1f - _e1025));
            }
            let _e1163 = phi_6408_;
            let _e1165 = (_e1014 / vec3(_e1015));
            let _e1167 = clamp(dot(_e931, _e1165), -1f, 1f);
            let _e1169 = global_4.member;
            let _e1171 = global_4.member_1;
            switch bitcast<i32>(0u) {
                default: {
                    let _e1173 = clamp(_e1015, _e1169, _e1171);
                    let _e1174 = (_e1173 * _e1167);
                    let _e1175 = (_e1167 < 0f);
                    phi_5010_ = _e1175;
                    if _e1175 {
                        phi_5010_ = (((_e1174 * _e1174) - ((_e1173 - _e1169) * (_e1173 + _e1169))) >= 0f);
                    }
                    let _e1183 = phi_5010_;
                    if _e1183 {
                        phi_6405_ = vec3<f32>(0f, 0f, 0f);
                        break;
                    }
                    let _e1184 = textureDimensions(global_6, 0i);
                    let _e1186 = vec2<f32>(vec2<i32>(_e1184));
                    let _e1190 = sqrt(((_e1171 - _e1169) * (_e1171 + _e1169)));
                    let _e1195 = sqrt(max(((_e1173 - _e1169) * (_e1173 + _e1169)), 0f));
                    let _e1196 = (_e1171 - _e1173);
                    let _e1198 = (_e1196 * (_e1171 + _e1173));
                    let _e1202 = sqrt(max((_e1198 + (_e1174 * _e1174)), 0f));
                    if (_e1174 > 0f) {
                        phi_6404_ = (_e1198 / (_e1202 + _e1174));
                    } else {
                        phi_6404_ = (_e1202 - _e1174);
                    }
                    let _e1208 = phi_6404_;
                    let _e1230 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1186.x) + (clamp(((_e1208 - _e1196) / max(((_e1195 + _e1190) - _e1196), 0.001f)), 0f, 1f) * (1f - (1f / _e1186.x)))), ((0.5f / _e1186.y) + (clamp((_e1195 / _e1190), 0f, 1f) * (1f - (1f / _e1186.y))))), 0f);
                    phi_6405_ = _e1230.xyz;
                    break;
                }
            }
            let _e1233 = phi_6405_;
            let _e1235 = global_1.member_1;
            let _e1238 = global.member_2;
            let _e1241 = (exp2(_e1235) * select(1f, _e1238, (_e1238 > 0f)));
            let _e1243 = global_1.member_17;
            let _e1244 = max(_e1147, 0.000001f);
            let _e1256 = global_1.member_16;
            phi_6469_ = vec3<f32>(0f, 0f, 0f);
            phi_6411_ = 0f;
            if (_e1036 > 0f) {
                let _e1260 = dot(_e931, _e1040);
                phi_6470_ = vec3<f32>(0f, 0f, 0f);
                phi_6412_ = 0f;
                if (_e1260 > cos((_e1045 * 1.02f))) {
                    let _e1270 = (_e931 - (_e1040 * _e1260));
                    let _e1279 = vec2<f32>((((dot(_e1270, _e1052) / _e1045) * 0.5f) + 0.5f), (0.5f - ((dot(_e1270, _e1053) / _e1045) * 0.5f)));
                    let _e1280 = textureSampleLevel(global_9, global_8, _e1279, 0f);
                    let _e1284 = dot(_e1022, _e1040);
                    let _e1288 = global_1.member_15;
                    switch bitcast<i32>(0u) {
                        default: {
                            let _e1292 = (((_e1279 * 2f) - vec2<f32>(1f, 1f)) * vec2<f32>(1.1248595f, 1.1248595f));
                            let _e1293 = dot(_e1292, _e1292);
                            if (_e1293 > 1f) {
                                phi_6410_ = 0f;
                                break;
                            }
                            phi_6410_ = smoothstep(-0.04f, 0.04f, dot(vec3<f32>(_e1292.x, _e1292.y, sqrt(max(0f, (1f - _e1293)))), normalize(vec3<f32>(dot(_e1022, _e1052), -(dot(_e1022, _e1053)), -(_e1284)))));
                            break;
                        }
                    }
                    let _e1305 = phi_6410_;
                    let _e1323 = smoothstep(1f, 0.92f, (sqrt(max(0f, (2f * (1f - _e1260)))) / _e1045));
                    phi_6470_ = ((((mix((vec3<f32>(0.937f, 1.008f, 1.109f) * (0.000055f * (_e1284 + 1f))), vec3<f32>(1.2142f, 0.9614f, 0.7521f), vec3(_e1305)) * (((_e1036 / (0.000064439846f * max((0.5f * (1f - _e1284)), 0.001f))) * (dot(_e1280.xyz, vec3<f32>(0.2126f, 0.7152f, 0.0722f)) * 2.832059f)) * exp2(_e1288))) * _e1323) * _e1280.w) * _e1233);
                    phi_6412_ = (_e1323 * _e1280.w);
                }
                let _e1330 = phi_6470_;
                let _e1332 = phi_6412_;
                phi_6469_ = _e1330;
                phi_6411_ = _e1332;
            }
            let _e1334 = phi_6469_;
            let _e1336 = phi_6411_;
            phi_6424_ = _e1144;
            if (_e1336 > 0f) {
                phi_6414_ = vec3<f32>(0f, 0f, 0f);
                phi_6413_ = 0i;
                loop {
                    let _e1339 = phi_6414_;
                    let _e1341 = phi_6413_;
                    local_4 = _e1339;
                    if (_e1341 < 4i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1344 = (f32(_e1341) * 1.5707964f);
                        let _e1354 = ((_e1040 * cos(_e1045)) + (((_e1052 * cos(_e1344)) + (_e1053 * sin(_e1344))) * sin(_e1045)));
                        let _e1366 = ((((asin(clamp(dot(_e1354, _e1018), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1372 = (0.5f + ((0.5f * sign(_e1366)) * sqrt(abs(_e1366))));
                        let _e1373 = vec2<f32>(((atan2(dot(_e1354, _e1034), dot(_e1354, _e1032)) * 0.15915494f) + 0.5f), _e1372);
                        let _e1378 = textureDimensions(global_3, 0i);
                        let _e1380 = vec2<f32>(vec2<i32>(_e1378));
                        let _e1382 = ((vec2<f32>(_e1373.x, clamp(_e1372, 0.001953125f, 0.9980469f)) * _e1380) - vec2<f32>(0.5f, 0.5f));
                        let _e1383 = floor(_e1382);
                        let _e1384 = (_e1382 - _e1383);
                        let _e1385 = (_e1384 * _e1384);
                        let _e1386 = (_e1385 * _e1384);
                        let _e1387 = (_e1384 * 3f);
                        let _e1389 = (_e1385 * 3f);
                        let _e1395 = (_e1386 * 3f);
                        let _e1397 = (((vec2<f32>(4f, 4f) - (_e1385 * 6f)) + _e1395) * vec2<f32>(0.16666667f, 0.16666667f));
                        let _e1402 = (_e1386 * vec2<f32>(0.16666667f, 0.16666667f));
                        let _e1403 = (((((vec2<f32>(1f, 1f) - _e1387) + _e1389) - _e1386) * vec2<f32>(0.16666667f, 0.16666667f)) + _e1397);
                        let _e1404 = (((((vec2<f32>(1f, 1f) + _e1387) + _e1389) - _e1395) * vec2<f32>(0.16666667f, 0.16666667f)) + _e1402);
                        let _e1408 = (((_e1383 - vec2<f32>(0.5f, 0.5f)) + (_e1397 / _e1403)) / _e1380);
                        let _e1412 = (((_e1383 + vec2<f32>(1.5f, 1.5f)) + (_e1402 / _e1404)) / _e1380);
                        let _e1415 = (0.5f / _e1380.y);
                        let _e1416 = (1f - _e1415);
                        let _e1417 = clamp(_e1408.y, _e1415, _e1416);
                        let _e1419 = clamp(_e1412.y, _e1415, _e1416);
                        let _e1424 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1408.x, _e1417), 0f);
                        let _e1430 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1412.x, _e1417), 0f);
                        let _e1437 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1408.x, _e1419), 0f);
                        let _e1441 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1412.x, _e1419), 0f);
                        phi_6414_ = (_e1339 + ((((_e1424.xyz * _e1403.x) + (_e1430.xyz * _e1404.x)) * _e1403.y) + (((_e1437.xyz * _e1403.x) + (_e1441.xyz * _e1404.x)) * _e1404.y)));
                        phi_6413_ = (_e1341 + 1i);
                    }
                }
                let _e1450 = local_4;
                phi_6424_ = mix(_e1144, (_e1450 * 0.25f), vec3(clamp((_e1336 * dot(_e1233, vec3<f32>(0.2126f, 0.7152f, 0.0722f))), 0f, 1f)));
            }
            let _e1458 = phi_6424_;
            let _e1460 = global_4.member_24;
            let _e1461 = (_e1460 == 2u);
            let _e1463 = global_4.member_8;
            let _e1465 = global_4.member_9;
            let _e1467 = global_4.member_10;
            let _e1469 = global_4.member_18;
            let _e1471 = global_4.member_23;
            let _e1473 = global_4.member_17;
            let _e1475 = global_4.member_13;
            let _e1476 = max(_e1465, 0f);
            let _e1478 = clamp(_e1025, 0f, 1f);
            let _e1480 = mix((_e1463 * _e1476), _e1467, vec3(_e1478));
            phi_6427_ = _e1458;
            if (_e1461 && (_e1019 < 0f)) {
                phi_6427_ = mix(mix(_e1480, _e1471, vec3(clamp(_e1469, 0f, 1f))), _e1480, vec3(clamp((-(_e1019) * max(_e1473, 0.001f)), 0f, 1f)));
            }
            let _e1493 = phi_6427_;
            phi_6428_ = _e1493;
            if ((_e1025 > 0f) && (_e1019 > select(-1f, 0f, _e1461))) {
                if _e1461 {
                    phi_6425_ = max(_e1019, 0f);
                } else {
                    phi_6425_ = abs(_e1019);
                }
                let _e1501 = phi_6425_;
                let _e1509 = fract((((normalize(_e931) * 37f) * 0.3183099f) + vec3<f32>(0.1f, 0.2f, 0.3f)));
                let _e1514 = (_e1509 + vec3(dot(_e1509, (_e1509.yzx + vec3<f32>(19.19f, 19.19f, 19.19f)))));
                phi_6428_ = (_e1493 + ((mix(_e1475, vec3<f32>(0.000000698f, 0.00000085f, 0.000001163f), vec3(smoothstep(0f, 0.85f, _e1501))) * (1f + ((fract(((_e1514.x * _e1514.y) * _e1514.z)) - 0.5f) * 0.05f))) * _e1478));
            }
            let _e1528 = phi_6428_;
            phi_6462_ = _e1528;
            if ((_e1460 == 1u) && (_e1019 < 0.005f)) {
                phi_6430_ = vec3<f32>(0f, 0f, 0f);
                phi_6429_ = 0i;
                loop {
                    let _e1533 = phi_6430_;
                    let _e1535 = phi_6429_;
                    local_5 = _e1533;
                    if (_e1535 < 16i) {
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e1537 = f32(_e1535);
                        let _e1539 = ((_e1537 + 0.5f) * 0.0625f);
                        let _e1540 = sqrt(_e1539);
                        let _e1544 = (_e1537 * 2.3999631f);
                        let _e1557 = ((((asin(clamp(sqrt(max((1f - _e1539), 0f)), -1f, 1f)) * 0.31830987f) + 0.5f) * 2f) - 1f);
                        let _e1563 = (0.5f + ((0.5f * sign(_e1557)) * sqrt(abs(_e1557))));
                        let _e1564 = vec2<f32>(((atan2((_e1540 * sin(_e1544)), (_e1540 * cos(_e1544))) * 0.15915494f) + 0.5f), _e1563);
                        let _e1569 = textureSampleLevel(global_3, global_2, vec2<f32>(_e1564.x, clamp(_e1563, 0.001953125f, 0.9980469f)), 0f);
                        phi_6430_ = (_e1533 + _e1569.xyz);
                        phi_6429_ = (_e1535 + 1i);
                    }
                }
                let _e1574 = local_5;
                let _e1577 = global_4.member_4;
                let _e1579 = global_4.member_2;
                let _e1581 = global_4.member_25;
                switch bitcast<i32>(0u) {
                    default: {
                        let _e1584 = (_e1015 - _e1169);
                        if (_e1584 <= (_e1169 * 0.0000008f)) {
                            phi_6459_ = 0f;
                            phi_6451_ = select(vec3<f32>(0f, 1f, 0f), _e1165, vec3((_e1015 > 0f)));
                        } else {
                            switch bitcast<i32>(0u) {
                                default: {
                                    let _e1590 = (_e1011 - _e1013);
                                    let _e1591 = dot(_e1590, _e931);
                                    let _e1593 = (_e1590 - (_e931 * _e1591));
                                    let _e1594 = dot(_e1593, _e1593);
                                    let _e1595 = (_e1169 * _e1169);
                                    if (_e1594 > _e1595) {
                                        phi_6433_ = f32();
                                        phi_6432_ = false;
                                        break;
                                    }
                                    phi_6433_ = (_e1591 - sqrt(max((_e1595 - _e1594), 0f)));
                                    phi_6432_ = true;
                                    break;
                                }
                            }
                            let _e1602 = phi_6433_;
                            let _e1604 = phi_6432_;
                            if (!(_e1604) || (_e1602 <= 0f)) {
                                phi_6461_ = _e1528;
                                break;
                            }
                            phi_6459_ = _e1602;
                            phi_6451_ = normalize(((_e1013 + (_e931 * _e1602)) - _e1011));
                        }
                        let _e1613 = phi_6459_;
                        let _e1615 = phi_6451_;
                        let _e1616 = dot(_e1615, _e1022);
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e1618 = clamp(_e1169, _e1169, _e1171);
                                let _e1619 = (_e1618 * _e1616);
                                let _e1620 = (_e1616 < 0f);
                                phi_6046_ = _e1620;
                                if _e1620 {
                                    phi_6046_ = (((_e1619 * _e1619) - ((_e1618 - _e1169) * (_e1618 + _e1169))) >= 0f);
                                }
                                let _e1628 = phi_6046_;
                                if _e1628 {
                                    phi_6453_ = vec3<f32>(0f, 0f, 0f);
                                    break;
                                }
                                let _e1629 = textureDimensions(global_6, 0i);
                                let _e1631 = vec2<f32>(vec2<i32>(_e1629));
                                let _e1635 = sqrt(((_e1171 - _e1169) * (_e1171 + _e1169)));
                                let _e1640 = sqrt(max(((_e1618 - _e1169) * (_e1618 + _e1169)), 0f));
                                let _e1641 = (_e1171 - _e1618);
                                let _e1643 = (_e1641 * (_e1171 + _e1618));
                                let _e1647 = sqrt(max((_e1643 + (_e1619 * _e1619)), 0f));
                                if (_e1619 > 0f) {
                                    phi_6452_ = (_e1643 / (_e1647 + _e1619));
                                } else {
                                    phi_6452_ = (_e1647 - _e1619);
                                }
                                let _e1653 = phi_6452_;
                                let _e1675 = textureSampleLevel(global_6, global_5, vec2<f32>(((0.5f / _e1631.x) + (clamp(((_e1653 - _e1641) / max(((_e1640 + _e1635) - _e1641), 0.001f)), 0f, 1f) * (1f - (1f / _e1631.x)))), ((0.5f / _e1631.y) + (clamp((_e1640 / _e1635), 0f, 1f) * (1f - (1f / _e1631.y))))), 0f);
                                phi_6453_ = _e1675.xyz;
                                break;
                            }
                        }
                        let _e1678 = phi_6453_;
                        phi_6461_ = mix(_e1528, mix(_e1528, ((((((((_e1463 * 0.31830987f) * max(_e1616, 0f)) * _e1678) * _e1256) * _e1243) + (_e1463 * (_e1574 * vec3<f32>(0.0625f, 0.0625f, 0.0625f)))) * _e1476) + (_e1467 * _e1478)), vec3(exp((-(_e1613) * ((dot(_e1577, vec3<f32>(0.299f, 0.587f, 0.114f)) * exp((-(_e1584) / _e1579))) * max(_e1581, 0f)))))), vec3(smoothstep(-0.004f, 0f, -(_e1019))));
                        break;
                    }
                }
                let _e1707 = phi_6461_;
                phi_6462_ = _e1707;
            }
            let _e1709 = phi_6462_;
            phi_6463_ = _e1709;
            if _e1461 {
                let _e1711 = global_4.member_15;
                let _e1713 = global_4.member_16;
                let _e1720 = global_4.member_11;
                let _e1722 = global_4.member_12;
                phi_6463_ = mix(_e1709, mix(_e1720, _e1722, vec3(_e1025)), vec3(clamp((1f - smoothstep(0f, max(mix(_e1711, _e1713, _e1025), 0.00001f), abs(_e1019))), 0f, 1f)));
            }
            let _e1729 = phi_6463_;
            let _e1732 = (((_e1729 + (((_e1256 * min((_e1243 / (3.1415927f * ((_e1244 * _e1244) + ((_e1244 * 0.05f) * (_e1244 * 0.25f))))), (32752f / max(_e1241, 0.000001f)))) * _e1163) * _e1233)) + _e1334) * _e1241);
            global_7 = vec4<f32>(_e1732.x, _e1732.y, _e1732.z, 1f);
            break;
        }
    }
    return;
}

@fragment
fn main(@location(0) param: vec2<f32>) -> @location(0) vec4<f32> {
    global_10 = param;
    function_();
    let _e3 = global_7;
    return _e3;
}
