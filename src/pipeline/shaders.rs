// Fragment shader source for webcam mask and drop shadow.

pub const WEBCAM_MASK_FRAGMENT_SHADER: &str = r#"
#version 100
precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D s_texture;

// 0 = Rectangle, 1 = Square, 2 = Circle
uniform int u_shape;
uniform float u_corner_radius;
uniform float u_shadow_blur;
uniform vec2 u_shadow_offset;
uniform float u_aspect_ratio;

// Signed distance field calculation for a 2D box with rounded corners
float sd_rounded_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r, r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Signed distance field calculation for a circle
float sd_circle(vec2 p, float r) {
    return length(p) - r;
}

void main() {
    vec2 uv = v_texcoord;
    vec2 p = uv - vec2(0.5, 0.5);

    // Maintain aspect ratio adjustment for distance checks
    vec2 aspect_p = vec2(p.x * u_aspect_ratio, p.y);

    float d = 0.0;
    if (u_shape == 2) {
        // Circle shape
        d = sd_circle(aspect_p, 0.45);
    } else if (u_shape == 1) {
        // Square shape
        d = sd_rounded_box(aspect_p, vec2(0.45, 0.45), u_corner_radius);
    } else {
        // Rectangle shape
        d = sd_rounded_box(aspect_p, vec2(0.45 * u_aspect_ratio, 0.45), u_corner_radius);
    }

    // Anti-aliased mask for webcam content
    float mask = 1.0 - smoothstep(-0.005, 0.005, d);

    // Drop shadow calculation
    vec2 shadow_p = aspect_p - u_shadow_offset;
    float d_shadow = 0.0;
    if (u_shape == 2) {
        d_shadow = sd_circle(shadow_p, 0.45);
    } else if (u_shape == 1) {
        d_shadow = sd_rounded_box(shadow_p, vec2(0.45, 0.45), u_corner_radius);
    } else {
        d_shadow = sd_rounded_box(shadow_p, vec2(0.45 * u_aspect_ratio, 0.45), u_corner_radius);
    }

    float shadow = 1.0 - smoothstep(0.0, u_shadow_blur, d_shadow);
    vec4 shadow_color = vec4(0.0, 0.0, 0.0, 0.6) * shadow;

    // Sample video texture
    vec4 video_color = texture2D(s_texture, uv);

    // Composite video over shadow
    vec4 final_color = mix(shadow_color, video_color, mask);
    final_color.a = max(mask, shadow * 0.6);

    gl_FragColor = final_color;
}
"#;
