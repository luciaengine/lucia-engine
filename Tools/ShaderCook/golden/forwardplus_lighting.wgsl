struct type_5 {
    member: vec4<u32>,
    member_1: array<vec2<u32>>,
}

struct type_9 {
    member: vec4<u32>,
}

struct type_19 {
    member: mat4x4<f32>,
    member_1: mat4x4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
}

struct type_27 {
    member: array<u32>,
}

struct type_30 {
    member: vec4<u32>,
    member_1: vec4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
    member_4: vec4<f32>,
    member_5: vec4<f32>,
    member_6: vec4<f32>,
    member_7: vec4<f32>,
    member_8: vec4<i32>,
}

struct type_32 {
    member: vec4<u32>,
    member_1: array<type_30>,
}

@group(0) @binding(1)
var<storage> global: type_5;
@group(0) @binding(4)
var<uniform> global_1: type_9;
var<private> global_2: vec4<f32>;
@group(0) @binding(64)
var global_3: sampler;
@group(0) @binding(0)
var global_4: texture_2d<f32>;
@group(0) @binding(5)
var<uniform> global_5: type_19;
var<private> global_6: vec4<f32>;
@group(0) @binding(2)
var<storage> global_7: type_27;
@group(0) @binding(3)
var<storage> global_8: type_32;

fn function_() {
    var phi_197_: bool;
    var phi_585_: vec3<f32>;
    var phi_583_: vec3<f32>;
    var phi_582_: u32;
    var phi_586_: f32;
    var phi_592_: vec3<f32>;
    var phi_591_: vec3<f32>;
    var local: vec3<f32>;
    var local_1: vec3<f32>;
    var local_2: vec3<f32>;

    switch bitcast<i32>(0u) {
        default: {
            let _e34 = global.member[0u];
            let _e35 = max(_e34, 1u);
            let _e38 = global.member[1u];
            let _e39 = max(_e38, 1u);
            let _e42 = global.member[2u];
            let _e46 = global.member[3u];
            let _e49 = global_1.member;
            let _e51 = max(_e49.xy, vec2<u32>(1u, 1u));
            let _e58 = vec2<f32>(f32((_e35 * _e51.x)), f32((_e39 * _e51.y)));
            let _e59 = global_2;
            let _e60 = _e59.xy;
            let _e63 = vec2<u32>(clamp(_e60, vec2<f32>(0f, 0f), (_e58 - vec2<f32>(1f, 1f))));
            let _e65 = textureLoad(global_4, bitcast<vec2<i32>>(_e63), 0i);
            let _e69 = (((_e60 / _e58) * 2f) - vec2<f32>(1f, 1f));
            let _e71 = global_5.member;
            let _e75 = (_e71 * vec4<f32>(_e69.x, _e69.y, _e65.x, 1f));
            let _e80 = (_e75.xyz / vec3(max(_e75.w, 0.000001f)));
            let _e86 = vec4<f32>(_e80.x, vec4<f32>().y, vec4<f32>().z, vec4<f32>().w);
            let _e92 = vec4<f32>(_e86.x, _e80.y, _e86.z, _e86.w);
            let _e99 = vec4<f32>(_e92.x, _e92.y, _e80.z, _e92.w).xyz;
            let _e100 = dpdx(_e99);
            let _e101 = dpdy(_e99);
            let _e103 = normalize(cross(_e100, _e101));
            if (_e65.x <= 0.000001f) {
                global_6 = vec4<f32>(0f, 0f, 0f, 0f);
                break;
            }
            let _e105 = (_e63 / _e51);
            let _e107 = (_e105.x >= _e35);
            phi_197_ = _e107;
            if !(_e107) {
                phi_197_ = (_e105.y >= _e39);
            }
            let _e112 = phi_197_;
            if _e112 {
                global_6 = vec4<f32>(0f, 0f, 0f, 0f);
                break;
            }
            let _e115 = global_5.member_2[0u];
            let _e120 = global_5.member[2][2u];
            let _e124 = global_5.member[3][2u];
            let _e128 = global_5.member[2][3u];
            let _e132 = global_5.member[3][3u];
            let _e141 = global_5.member_2[2u];
            let _e158 = global.member_1[((_e105.x + (_e105.y * _e35)) + (u32(floor((clamp((log(((((_e120 * _e65.x) + _e124) / max(((_e128 * _e65.x) + _e132), 0.000000000000000000000000000001f)) / max(_e115, 0.0001f))) * _e141), 0f, 0.999999f) * f32(max(_e42, 1u))))) * (_e35 * _e39)))];
            phi_585_ = _e103;
            if (dot(_e103, -(normalize(_e99))) < 0f) {
                phi_585_ = -(_e103);
            }
            let _e168 = phi_585_;
            phi_583_ = vec3<f32>(0f, 0f, 0f);
            phi_582_ = 0u;
            loop {
                let _e170 = phi_583_;
                let _e172 = phi_582_;
                local = _e170;
                local_1 = _e170;
                local_2 = _e170;
                if (_e172 < min(_e158.y, max(_e46, 1u))) {
                    let _e177 = global_7.member[(_e158.x + _e172)];
                    let _e180 = global_8.member[0u];
                    if (_e177 >= _e180) {
                        phi_591_ = _e170;
                        continue;
                    }
                    let _e184 = global_8.member_1[_e177];
                    if (_e184.member.w == 0u) {
                        phi_591_ = _e170;
                        continue;
                    }
                    let _e193 = _e184.member_3.xyz;
                    if (_e184.member.x == 0u) {
                        let _e198 = normalize(_e184.member_2.xyz);
                        let _e200 = global_5.member_1;
                        phi_592_ = (_e170 + (_e193 * (_e184.member_2.w * max(dot(_e168, normalize(-(normalize((_e200 * vec4<f32>(_e198.x, _e198.y, _e198.z, 0f)).xyz)))), 0f))));
                    } else {
                        let _e218 = global_5.member_1;
                        let _e225 = ((_e218 * vec4<f32>(_e184.member_1.x, _e184.member_1.y, _e184.member_1.z, 1f)).xyz - _e99);
                        let _e226 = length(_e225);
                        let _e229 = (_e225 / vec3(max(_e226, 0.000001f)));
                        let _e234 = clamp((1f - (_e226 / max(_e184.member_1.w, 0.001f))), 0f, 1f);
                        phi_586_ = 1f;
                        if (_e184.member.x == 2u) {
                            let _e237 = normalize(_e184.member_2.xyz);
                            phi_586_ = smoothstep(_e184.member_5.x, _e184.member_4.w, dot(normalize(-(_e229)), normalize(normalize((_e218 * vec4<f32>(_e237.x, _e237.y, _e237.z, 0f)).xyz))));
                        }
                        let _e253 = phi_586_;
                        phi_592_ = (_e170 + (_e193 * ((((_e184.member_2.w * max(dot(_e168, _e229), 0f)) * _e234) * _e234) * _e253)));
                    }
                    let _e261 = phi_592_;
                    phi_591_ = _e261;
                    continue;
                } else {
                    break;
                }
                continuing {
                    let _e263 = phi_591_;
                    phi_583_ = _e263;
                    phi_582_ = (_e172 + bitcast<u32>(1i));
                }
            }
            let _e267 = local;
            let _e270 = local_1;
            let _e273 = local_2;
            global_6 = vec4<f32>(_e267.x, _e270.y, _e273.z, 1f);
            break;
        }
    }
    return;
}

@fragment
fn main(@builtin(position) param: vec4<f32>) -> @location(0) vec4<f32> {
    global_2 = param;
    function_();
    let _e3 = global_6;
    return _e3;
}
