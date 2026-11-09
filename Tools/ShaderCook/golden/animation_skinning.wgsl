struct type_9 {
    member: array<vec4<f32>>,
}

struct type_14 {
    member: array<u32>,
}

struct type_23 {
    member: u32,
    member_1: u32,
    member_2: u32,
}

struct type_27 {
    member: u32,
    member_1: u32,
    member_2: f32,
    member_3: u32,
    member_4: u32,
    member_5: array<u32, 3>,
}

struct type_29 {
    member: array<type_27>,
}

struct type_34 {
    member: u32,
    member_1: u32,
    member_2: u32,
    member_3: u32,
    member_4: u32,
    member_5: u32,
    member_6: u32,
    member_7: u32,
    member_8: u32,
    member_9: u32,
    member_10: array<u32, 2>,
    member_11: mat4x4<f32>,
    member_12: array<u32, 4>,
}

struct type_36 {
    member: array<type_34>,
}

struct type_40 {
    member: f32,
    member_1: u32,
    member_2: u32,
    member_3: u32,
    member_4: u32,
    member_5: u32,
    member_6: u32,
    member_7: array<u32, 9>,
}

struct type_42 {
    member: array<type_40>,
}

@group(0) @binding(5)
var<storage, read_write> global: type_9;
@group(0) @binding(1)
var<storage> global_1: type_14;
@group(0) @binding(3)
var<storage> global_2: type_14;
var<workgroup> global_3: array<vec4<f32>, 768>;
var<private> global_4: vec3<u32>;
@group(3) @binding(0)
var<uniform> global_5: type_23;
@group(0) @binding(4)
var<storage> global_6: type_29;
@group(0) @binding(6)
var<storage> global_7: type_14;
@group(0) @binding(0)
var<storage> global_8: type_36;
@group(0) @binding(2)
var<storage> global_9: type_42;
var<private> global_10: u32;

fn function_() {
    var local: vec4<f32>;
    var local_1: vec4<f32>;
    var local_2: vec4<f32>;
    var phi_1109_: bool;
    var phi_1117_: bool;
    var phi_2919_: bool;
    var phi_2923_: u32;
    var phi_2924_: u32;
    var phi_1338_: bool;
    var phi_2953_: vec3<f32>;
    var phi_2950_: vec4<f32>;
    var phi_2947_: vec3<f32>;
    var phi_2941_: bool;
    var phi_2938_: u32;
    var phi_2967_: u32;
    var phi_2966_: u32;
    var phi_3103_: u32;
    var local_3: u32;
    var local_4: u32;
    var phi_2968_: u32;
    var local_5: u32;
    var phi_2970_: f32;
    var phi_2973_: u32;
    var phi_2971_: u32;
    var phi_2980_: u32;
    var phi_2978_: vec4<f32>;
    var phi_2977_: f32;
    var phi_2979_: vec4<f32>;
    var phi_3028_: vec3<f32>;
    var phi_3012_: vec4<f32>;
    var phi_3014_: vec3<f32>;
    var phi_2998_: vec4<f32>;
    var phi_2982_: vec3<f32>;
    var phi_3029_: bool;
    var phi_3013_: vec3<f32>;
    var phi_2997_: vec4<f32>;
    var phi_2981_: vec3<f32>;
    var phi_2951_: vec3<f32>;
    var phi_2948_: vec4<f32>;
    var phi_2945_: vec3<f32>;
    var phi_2939_: bool;
    var phi_2964_: mat4x4<f32>;
    var phi_2927_: u32;
    var phi_2928_: u32;
    var phi_2934_: u32;
    var phi_2932_: u32;
    var local_6: vec3<f32>;
    var local_7: vec4<f32>;
    var local_8: vec3<f32>;
    var local_9: bool;
    var local_10: u32;
    var local_11: u32;
    var local_12: vec3<f32>;
    var local_13: vec4<f32>;
    var local_14: vec3<f32>;
    var local_15: bool;

    let _e50 = global_4[0u];
    let _e52 = global_5.member;
    let _e53 = (_e50 < _e52);
    let _e56 = min(_e50, (max(_e52, 1u) - 1u));
    let _e59 = global_6.member[_e56];
    let _e66 = global_5.member_1;
    let _e67 = (_e66 != 0u);
    phi_1109_ = _e67;
    if _e67 {
        phi_1109_ = (_e59.member_4 != 0u);
    }
    let _e70 = phi_1109_;
    phi_1117_ = _e70;
    if _e70 {
        let _e72 = global_5.member_2;
        phi_1117_ = (_e59.member_4 < _e72);
    }
    let _e75 = phi_1117_;
    phi_2919_ = _e53;
    if _e75 {
        let _e78 = global_7.member[_e59.member_4];
        phi_2919_ = select(_e53, false, (_e78 == 0u));
    }
    let _e82 = phi_2919_;
    let _e85 = global_8.member[_e59.member];
    let _e98 = global_9.member[_e59.member_1];
    if _e82 {
        phi_2923_ = min(_e85.member, 256u);
    } else {
        phi_2923_ = 0u;
    }
    let _e107 = phi_2923_;
    let _e108 = global_10;
    phi_2924_ = _e108;
    loop {
        let _e110 = phi_2924_;
        if (_e110 < _e107) {
            let _e113 = (_e85.member_4 + (_e110 * 10u));
            let _e116 = global_1.member[_e113];
            let _e121 = global_1.member[(_e113 + 1u)];
            let _e126 = global_1.member[(_e113 + 2u)];
            let _e128 = vec3<f32>(bitcast<f32>(_e116), bitcast<f32>(_e121), bitcast<f32>(_e126));
            let _e132 = global_1.member[(_e113 + 3u)];
            let _e137 = global_1.member[(_e113 + 4u)];
            let _e142 = global_1.member[(_e113 + 5u)];
            let _e147 = global_1.member[(_e113 + 6u)];
            let _e149 = vec4<f32>(bitcast<f32>(_e132), bitcast<f32>(_e137), bitcast<f32>(_e142), bitcast<f32>(_e147));
            let _e153 = global_1.member[(_e113 + 7u)];
            let _e158 = global_1.member[(_e113 + 8u)];
            let _e163 = global_1.member[(_e113 + 9u)];
            let _e165 = vec3<f32>(bitcast<f32>(_e153), bitcast<f32>(_e158), bitcast<f32>(_e163));
            let _e169 = global_1.member[(_e85.member_9 + _e110)];
            let _e170 = (_e98.member_1 > 0u);
            phi_1338_ = _e170;
            if _e170 {
                phi_1338_ = (_e169 < _e98.member_6);
            }
            let _e173 = phi_1338_;
            phi_2951_ = _e165;
            phi_2948_ = _e149;
            phi_2945_ = _e128;
            phi_2939_ = false;
            if _e173 {
                let _e177 = global_2.member[(_e98.member_4 + _e169)];
                let _e181 = global_2.member[(_e98.member_5 + _e169)];
                phi_2953_ = _e165;
                phi_2950_ = _e149;
                phi_2947_ = _e128;
                phi_2941_ = false;
                phi_2938_ = 0u;
                loop {
                    let _e183 = phi_2953_;
                    let _e185 = phi_2950_;
                    let _e187 = phi_2947_;
                    let _e189 = phi_2941_;
                    let _e191 = phi_2938_;
                    local_12 = _e183;
                    local_13 = _e185;
                    local_14 = _e187;
                    local_15 = _e189;
                    if (_e191 < _e181) {
                        let _e195 = (_e98.member_2 + ((_e177 + _e191) * 4u));
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e200 = global_2.member[(_e195 + 1u)];
                                let _e204 = global_2.member[(_e195 + 2u)];
                                let _e208 = global_2.member[(_e195 + 3u)];
                                if (_e208 == 0u) {
                                    phi_3029_ = _e189;
                                    phi_3013_ = _e183;
                                    phi_2997_ = _e185;
                                    phi_2981_ = _e187;
                                    break;
                                }
                                let _e210 = (_e200 & 3u);
                                let _e215 = (_e200 >> bitcast<u32>(8u));
                                let _e216 = (_e210 == 1u);
                                let _e217 = select(3u, 4u, _e216);
                                let _e218 = (_e98.member_3 + _e204);
                                phi_2967_ = _e208;
                                phi_2966_ = 0u;
                                loop {
                                    let _e220 = phi_2967_;
                                    let _e222 = phi_2966_;
                                    local_3 = _e222;
                                    local_4 = _e222;
                                    local_5 = _e222;
                                    if (_e222 < _e220) {
                                        let _e226 = (_e222 + ((_e220 - _e222) / 2u));
                                        let _e231 = global_2.member[(_e218 + (_e226 * _e215))];
                                        let _e233 = (bitcast<f32>(_e231) < _e59.member_2);
                                        if _e233 {
                                            phi_3103_ = (_e226 + 1u);
                                        } else {
                                            phi_3103_ = _e222;
                                        }
                                        let _e236 = phi_3103_;
                                        local_10 = select(_e226, _e220, _e233);
                                        local_11 = _e236;
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        let _e879 = local_10;
                                        phi_2967_ = _e879;
                                        let _e882 = local_11;
                                        phi_2966_ = _e882;
                                    }
                                }
                                let _e239 = local_3;
                                if (_e239 == 0u) {
                                    phi_2968_ = 0u;
                                } else {
                                    let _e242 = local_4;
                                    phi_2968_ = (_e242 - 1u);
                                }
                                let _e245 = phi_2968_;
                                let _e248 = local_5;
                                let _e251 = (_e218 + (_e245 * _e215));
                                let _e253 = (_e218 + (min(_e248, (_e208 - 1u)) * _e215));
                                let _e256 = global_2.member[_e251];
                                let _e257 = bitcast<f32>(_e256);
                                let _e260 = global_2.member[_e253];
                                let _e262 = (bitcast<f32>(_e260) - _e257);
                                if (_e262 > 0f) {
                                    phi_2970_ = clamp(((_e59.member_2 - _e257) / _e262), 0f, 1f);
                                } else {
                                    phi_2970_ = 0f;
                                }
                                let _e268 = phi_2970_;
                                let _e272 = global_2.member[(_e251 + 1u)];
                                phi_2973_ = ((_e200 >> bitcast<u32>(2u)) & 3u);
                                if ((_e272 & 4u) != 0u) {
                                    phi_2973_ = (_e272 & 3u);
                                }
                                let _e277 = phi_2973_;
                                local = vec4<f32>(0f, 0f, 0f, 0f);
                                local_1 = vec4<f32>(0f, 0f, 0f, 0f);
                                phi_2971_ = 0u;
                                loop {
                                    let _e279 = phi_2971_;
                                    if (_e279 < _e217) {
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        let _e285 = global_2.member[((_e251 + 2u) + _e279)];
                                        local[_e279] = bitcast<f32>(_e285);
                                        let _e292 = global_2.member[((_e253 + 2u) + _e279)];
                                        local_1[_e279] = bitcast<f32>(_e292);
                                        phi_2971_ = (_e279 + bitcast<u32>(1i));
                                    }
                                }
                                if ((_e277 == 1u) || (_e262 <= 0f)) {
                                    let _e300 = local;
                                    local_2 = _e300;
                                } else {
                                    if (_e277 == 2u) {
                                        local_2 = vec4<f32>(0f, 0f, 0f, 0f);
                                        phi_2980_ = 0u;
                                        loop {
                                            let _e303 = phi_2980_;
                                            if (_e303 < _e217) {
                                                continue;
                                            } else {
                                                break;
                                            }
                                            continuing {
                                                let _e305 = (_e251 + 2u);
                                                let _e311 = global_2.member[((_e305 + (_e217 * 2u)) + _e303)];
                                                let _e318 = global_2.member[((_e305 + (_e217 * 4u)) + _e303)];
                                                let _e320 = (_e253 + 2u);
                                                let _e325 = global_2.member[((_e320 + _e217) + _e303)];
                                                let _e332 = global_2.member[((_e320 + (_e217 * 3u)) + _e303)];
                                                let _e335 = local[_e303];
                                                let _e337 = local_1[_e303];
                                                let _e346 = (_e268 * _e268);
                                                let _e348 = (1f - _e268);
                                                let _e349 = (_e348 * _e348);
                                                local_2[_e303] = (((((_e349 * _e348) * _e335) + (((3f * _e349) * _e268) * (_e335 + (bitcast<f32>(_e311) * (_e262 * clamp(bitcast<f32>(_e318), 0.05f, 0.49f)))))) + (((3f * _e348) * _e346) * (_e337 - (bitcast<f32>(_e325) * (_e262 * clamp(bitcast<f32>(_e332), 0.05f, 0.49f)))))) + ((_e346 * _e268) * _e337));
                                                phi_2980_ = (_e303 + bitcast<u32>(1i));
                                            }
                                        }
                                        if _e216 {
                                            let _e365 = local_2;
                                            local_2 = normalize(_e365);
                                        }
                                    } else {
                                        if _e216 {
                                            let _e367 = local;
                                            let _e368 = local_1;
                                            switch bitcast<i32>(0u) {
                                                default: {
                                                    let _e370 = dot(_e367, _e368);
                                                    phi_2978_ = _e368;
                                                    phi_2977_ = _e370;
                                                    if (_e370 < 0f) {
                                                        phi_2978_ = -(_e368);
                                                        phi_2977_ = -(_e370);
                                                    }
                                                    let _e375 = phi_2978_;
                                                    let _e377 = phi_2977_;
                                                    if (_e377 > 0.9995f) {
                                                        phi_2979_ = normalize(mix(_e367, _e375, vec4(_e268)));
                                                        break;
                                                    }
                                                    let _e383 = acos(clamp(_e377, -1f, 1f));
                                                    let _e384 = sin(_e383);
                                                    phi_2979_ = ((_e367 * (sin(((1f - _e268) * _e383)) / _e384)) + (_e375 * (sin((_e268 * _e383)) / _e384)));
                                                    break;
                                                }
                                            }
                                            let _e396 = phi_2979_;
                                            local_2 = _e396;
                                        } else {
                                            let _e397 = local;
                                            let _e398 = local_1;
                                            local_2 = mix(_e397, _e398, vec4(_e268));
                                        }
                                    }
                                }
                                if (_e210 == 0u) {
                                    let _e402 = local_2;
                                    phi_3014_ = _e183;
                                    phi_2998_ = _e185;
                                    phi_2982_ = _e402.xyz;
                                } else {
                                    if _e216 {
                                        let _e404 = local_2;
                                        phi_3028_ = _e183;
                                        phi_3012_ = _e404;
                                    } else {
                                        let _e405 = local_2;
                                        phi_3028_ = _e405.xyz;
                                        phi_3012_ = _e185;
                                    }
                                    let _e408 = phi_3028_;
                                    let _e410 = phi_3012_;
                                    phi_3014_ = _e408;
                                    phi_2998_ = _e410;
                                    phi_2982_ = _e187;
                                }
                                let _e412 = phi_3014_;
                                let _e414 = phi_2998_;
                                let _e416 = phi_2982_;
                                phi_3029_ = true;
                                phi_3013_ = _e412;
                                phi_2997_ = _e414;
                                phi_2981_ = _e416;
                                break;
                            }
                        }
                        let _e418 = phi_3029_;
                        let _e420 = phi_3013_;
                        let _e422 = phi_2997_;
                        let _e424 = phi_2981_;
                        local_6 = _e420;
                        local_7 = _e422;
                        local_8 = _e424;
                        local_9 = _e418;
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        let _e866 = local_6;
                        phi_2953_ = _e866;
                        let _e869 = local_7;
                        phi_2950_ = _e869;
                        let _e872 = local_8;
                        phi_2947_ = _e872;
                        let _e875 = local_9;
                        phi_2941_ = _e875;
                        phi_2938_ = (_e191 + bitcast<u32>(1i));
                    }
                }
                let _e906 = local_12;
                phi_2951_ = _e906;
                let _e909 = local_13;
                phi_2948_ = _e909;
                let _e912 = local_14;
                phi_2945_ = _e912;
                let _e915 = local_15;
                phi_2939_ = _e915;
            }
            let _e428 = phi_2951_;
            let _e430 = phi_2948_;
            let _e432 = phi_2945_;
            let _e434 = phi_2939_;
            if _e434 {
                let _e439 = (_e430.x + _e430.x);
                let _e440 = (_e430.y + _e430.y);
                let _e441 = (_e430.z + _e430.z);
                let _e442 = (_e430.x * _e439);
                let _e443 = (_e430.x * _e440);
                let _e444 = (_e430.x * _e441);
                let _e445 = (_e430.y * _e440);
                let _e446 = (_e430.y * _e441);
                let _e447 = (_e430.z * _e441);
                let _e448 = (_e430.w * _e439);
                let _e449 = (_e430.w * _e440);
                let _e450 = (_e430.w * _e441);
                let _e467 = (vec3<f32>((1f - (_e445 + _e447)), (_e443 + _e450), (_e444 - _e449)) * _e428.x);
                let _e473 = (vec3<f32>((_e443 - _e450), (1f - (_e442 + _e447)), (_e446 + _e448)) * _e428.y);
                let _e479 = (vec3<f32>((_e444 + _e449), (_e446 - _e448), (1f - (_e442 + _e445))) * _e428.z);
                phi_2964_ = mat4x4<f32>(vec4<f32>(_e467.x, _e467.y, _e467.z, 0f), vec4<f32>(_e473.x, _e473.y, _e473.z, 0f), vec4<f32>(_e479.x, _e479.y, _e479.z, 0f), vec4<f32>(_e432.x, _e432.y, _e432.z, 1f));
            } else {
                let _e490 = (_e85.member_5 + (_e110 * 16u));
                let _e493 = global_1.member[_e490];
                let _e498 = global_1.member[(_e490 + 1u)];
                let _e503 = global_1.member[(_e490 + 2u)];
                let _e508 = global_1.member[(_e490 + 3u)];
                let _e513 = global_1.member[(_e490 + 4u)];
                let _e518 = global_1.member[(_e490 + 5u)];
                let _e523 = global_1.member[(_e490 + 6u)];
                let _e528 = global_1.member[(_e490 + 7u)];
                let _e533 = global_1.member[(_e490 + 8u)];
                let _e538 = global_1.member[(_e490 + 9u)];
                let _e543 = global_1.member[(_e490 + 10u)];
                let _e548 = global_1.member[(_e490 + 11u)];
                let _e553 = global_1.member[(_e490 + 12u)];
                let _e558 = global_1.member[(_e490 + 13u)];
                let _e563 = global_1.member[(_e490 + 14u)];
                let _e568 = global_1.member[(_e490 + 15u)];
                phi_2964_ = mat4x4<f32>(vec4<f32>(bitcast<f32>(_e493), bitcast<f32>(_e498), bitcast<f32>(_e503), bitcast<f32>(_e508)), vec4<f32>(bitcast<f32>(_e513), bitcast<f32>(_e518), bitcast<f32>(_e523), bitcast<f32>(_e528)), vec4<f32>(bitcast<f32>(_e533), bitcast<f32>(_e538), bitcast<f32>(_e543), bitcast<f32>(_e548)), vec4<f32>(bitcast<f32>(_e553), bitcast<f32>(_e558), bitcast<f32>(_e563), bitcast<f32>(_e568)));
            }
            let _e576 = phi_2964_;
            let _e577 = (3u * _e110);
            global_3[_e577] = vec4<f32>(_e576[0].x, _e576[1].x, _e576[2].x, _e576[3].x);
            global_3[(_e577 + 1u)] = vec4<f32>(_e576[0].y, _e576[1].y, _e576[2].y, _e576[3].y);
            global_3[(_e577 + 2u)] = vec4<f32>(_e576[0].z, _e576[1].z, _e576[2].z, _e576[3].z);
            continue;
        } else {
            break;
        }
        continuing {
            phi_2924_ = (_e110 + 64u);
        }
    }
    workgroupBarrier();
    if _e82 {
        let _e614 = global_6.member[_e56].member;
        let _e618 = global_8.member[_e614].member_2;
        phi_2927_ = _e618;
    } else {
        phi_2927_ = 0u;
    }
    let _e620 = phi_2927_;
    phi_2928_ = 1u;
    loop {
        let _e622 = phi_2928_;
        if (_e622 < 64u) {
            if (_e622 < _e620) {
                let _e625 = (_e85.member_8 + _e622);
                let _e628 = global_1.member[_e625];
                let _e632 = global_1.member[(_e625 + 1u)];
                phi_2934_ = (_e628 + _e108);
                loop {
                    let _e636 = phi_2934_;
                    if (_e636 < min(_e632, _e107)) {
                        let _e641 = global_1.member[(_e85.member_3 + _e636)];
                        if (_e641 < _e636) {
                            let _e643 = (3u * _e641);
                            let _e644 = (3u * _e636);
                            let _e646 = global_3[_e643];
                            let _e649 = global_3[(_e643 + 1u)];
                            let _e652 = global_3[(_e643 + 2u)];
                            let _e654 = global_3[_e644];
                            let _e657 = global_3[(_e644 + 1u)];
                            let _e660 = global_3[(_e644 + 2u)];
                            global_3[_e644] = ((((_e654 * _e646.x) + (_e657 * _e646.y)) + (_e660 * _e646.z)) + vec4<f32>(0f, 0f, 0f, _e646.w));
                            global_3[(_e644 + 1u)] = ((((_e654 * _e649.x) + (_e657 * _e649.y)) + (_e660 * _e649.z)) + vec4<f32>(0f, 0f, 0f, _e649.w));
                            global_3[(_e644 + 2u)] = ((((_e654 * _e652.x) + (_e657 * _e652.y)) + (_e660 * _e652.z)) + vec4<f32>(0f, 0f, 0f, _e652.w));
                        }
                        continue;
                    } else {
                        break;
                    }
                    continuing {
                        phi_2934_ = (_e636 + 64u);
                    }
                }
            }
            workgroupBarrier();
            continue;
        } else {
            break;
        }
        continuing {
            phi_2928_ = (_e622 + bitcast<u32>(1i));
        }
    }
    phi_2932_ = _e108;
    loop {
        let _e699 = phi_2932_;
        if (_e699 < select(0u, _e85.member_1, _e82)) {
            let _e704 = global_1.member[(_e85.member_7 + _e699)];
            if (_e704 < _e107) {
                let _e707 = (_e85.member_6 + (_e699 * 16u));
                let _e710 = global_1.member[_e707];
                let _e715 = global_1.member[(_e707 + 1u)];
                let _e720 = global_1.member[(_e707 + 2u)];
                let _e725 = global_1.member[(_e707 + 3u)];
                let _e730 = global_1.member[(_e707 + 4u)];
                let _e735 = global_1.member[(_e707 + 5u)];
                let _e740 = global_1.member[(_e707 + 6u)];
                let _e745 = global_1.member[(_e707 + 7u)];
                let _e750 = global_1.member[(_e707 + 8u)];
                let _e755 = global_1.member[(_e707 + 9u)];
                let _e760 = global_1.member[(_e707 + 10u)];
                let _e765 = global_1.member[(_e707 + 11u)];
                let _e770 = global_1.member[(_e707 + 12u)];
                let _e775 = global_1.member[(_e707 + 13u)];
                let _e780 = global_1.member[(_e707 + 14u)];
                let _e785 = global_1.member[(_e707 + 15u)];
                let _e792 = (3u * _e704);
                let _e794 = global_3[_e792];
                let _e797 = global_3[(_e792 + 1u)];
                let _e800 = global_3[(_e792 + 2u)];
                let _e819 = ((_e85.member_11 * mat4x4<f32>(vec4<f32>(_e794.x, _e797.x, _e800.x, 0f), vec4<f32>(_e794.y, _e797.y, _e800.y, 0f), vec4<f32>(_e794.z, _e797.z, _e800.z, 0f), vec4<f32>(_e794.w, _e797.w, _e800.w, 1f))) * mat4x4<f32>(vec4<f32>(bitcast<f32>(_e710), bitcast<f32>(_e715), bitcast<f32>(_e720), bitcast<f32>(_e725)), vec4<f32>(bitcast<f32>(_e730), bitcast<f32>(_e735), bitcast<f32>(_e740), bitcast<f32>(_e745)), vec4<f32>(bitcast<f32>(_e750), bitcast<f32>(_e755), bitcast<f32>(_e760), bitcast<f32>(_e765)), vec4<f32>(bitcast<f32>(_e770), bitcast<f32>(_e775), bitcast<f32>(_e780), bitcast<f32>(_e785))));
                let _e821 = (3u * (_e59.member_3 + _e699));
                global.member[_e821] = vec4<f32>(_e819[0].x, _e819[1].x, _e819[2].x, _e819[3].x);
                global.member[(_e821 + 1u)] = vec4<f32>(_e819[0].y, _e819[1].y, _e819[2].y, _e819[3].y);
                global.member[(_e821 + 2u)] = vec4<f32>(_e819[0].z, _e819[1].z, _e819[2].z, _e819[3].z);
            }
            continue;
        } else {
            break;
        }
        continuing {
            phi_2932_ = (_e699 + 64u);
        }
    }
    return;
}

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(workgroup_id) param: vec3<u32>, @builtin(local_invocation_index) param_1: u32) {
    global_4 = param;
    global_10 = param_1;
    function_();
}
