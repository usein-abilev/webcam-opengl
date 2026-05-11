#version 330 core

#define PI 3.14159265359

const float pixel_size = 8.0;
const float num_chars = 10.0;
const float num_chars_edges = 4.0;

in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D cam_texture;
uniform sampler2D atlas_texture;
uniform sampler2D atlas_edges_texture;
uniform float width;
uniform float height;
uniform float time;
uniform float zoom = 1.0;

float calc_luminance(vec3 color) {
    return dot(color.rgb, vec3(0.299, 0.587, 0.114));
}

// Sobel Edge Detection Filter
// Reference: https://gist.github.com/Hebali/6ebfc66106459aacee6a9fac029d0115#file-glslsobel-frag
vec2 calc_sobel(sampler2D tex, vec2 coord) 
{
	// vec4 n[9];
    float w = 1.0 / width;
    float h = 1.0 / height;

    float n[9];
    for(int i=0; i<3; i++) {
        for(int j=0; j<3; j++) {
            n[i*3+j] = calc_luminance(texture(tex, coord + vec2(float(i-1)*w, float(j-1)*h)).rgb);
        }
    }
    float gx = (n[2] + 2.0*n[5] + n[8]) - (n[0] + 2.0*n[3] + n[6]);
    float gy = (n[0] + 2.0*n[1] + n[2]) - (n[6] + 2.0*n[7] + n[8]);

    return vec2(gx, gy);
}

vec3 saturate_color(vec3 color, float value) {
    float luminance = calc_luminance(color);
    vec3 grayscale = vec3(luminance);
    return mix(grayscale, color, value); 
}

vec3 preprocess(vec3 color) {
    vec3 mixed = saturate_color(color, 2.0);
    mixed += 0.2;
    return mixed;
}

vec2 scale_vec(vec2 coord, float v) {
    return (coord - 0.5) / v + 0.5;
}

void main() {
    vec2 uv = vec2(width, height);
    vec2 blocks = uv / pixel_size;
    vec2 grid_uv = scale_vec(floor(TexCoord * blocks) / blocks, zoom);
    vec2 local_uv = fract(TexCoord * blocks);

    vec4 cam_color = texture(cam_texture, grid_uv);

    // Sobel Filter - calculate edges and angles of edges
    vec2 sobel = calc_sobel(cam_texture, grid_uv);
    float magnitude = sqrt(sobel.x*sobel.x + sobel.y*sobel.y);
    float angle = atan(sobel.y, sobel.x); // angle needed for mapping
    if (angle < 0) angle += PI;

    vec4 final_char;
    float edge_threshold = 0.2;
    if (magnitude > edge_threshold) {
        float char_index = 0;
        if (angle < PI/8.0 || angle > 7.0*PI/8.0) char_index = 0;      // |
        else if (angle < 3.0*PI/8.0) char_index = 2;                   // /
        else if (angle < 5.0*PI/8.0) char_index = 1;                   // -
        else char_index = 3;                                           // \


        float atlas_x = (char_index + local_uv.x) / num_chars_edges;
        vec2 atlas_uv = vec2(atlas_x, local_uv.y);
        vec4 ascii_edge = texture(atlas_edges_texture, atlas_uv);

        vec3 red = vec3(1.0, 0.0, 0.0);
        vec3 color = cam_color.rgb * ascii_edge.r * 1.0;
        final_char = vec4(saturate_color(color, 2.0), 1.0);
    } else {
        // get coords inside of the 8x8 cell 
        float lum = calc_luminance(cam_color.rgb);
        float char_index = floor(lum * (num_chars * 1.0));
        float atlas_x = (char_index + local_uv.x) / num_chars;
        vec2 atlas_uv = vec2(atlas_x, local_uv.y);
        vec4 ascii_glyph = texture(atlas_texture, atlas_uv);

        vec3 mixed = cam_color.rgb * ascii_glyph.r;
        final_char = vec4(mixed, 1.0);
    }

    FragColor = final_char;
}
