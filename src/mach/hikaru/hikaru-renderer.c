/* 
 * Valkyrie
 * Copyright (C) 2011-2013, Stefano Teso
 * 
 * Valkyrie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Valkyrie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Valkyrie.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "vk/input.h"

#include "mach/hikaru/hikaru-renderer.h"
#include "mach/hikaru/hikaru-renderer-private.h"

#define VP0	VP.scratch
#define MAT0	MAT.scratch
#define TEX0	TEX.scratch
#define LS0	LIT.scratchset

#define MAX_VIEWPORTS	4096
#define MAX_MODELVIEWS	4096
#define MAX_MATERIALS	4096
#define MAX_TEXHEADS	4096
#define MAX_LIGHTSETS	4096
#define MAX_MESHES	16384

#define INV255	(1.0f / 255.0f)

#define isnonnegative(x_) \
	(isfinite(x_) && (x_) >= 0.0)

/****************************************************************************
 Debug
****************************************************************************/

static const struct {
	int32_t min, max;
	uint32_t key;
} debug_controls[] = {
	[HR_DEBUG_LOG]			= {  0, 1,     ~0 },
	[HR_DEBUG_NO_LAYER1]		= {  0, 1, SDLK_1 },
	[HR_DEBUG_NO_LAYER2]		= {  0, 1, SDLK_2 },
	[HR_DEBUG_NO_3D]		= {  0, 1, SDLK_3 },
	[HR_DEBUG_SELECT_VIEWPORT]	= { -1, 7, SDLK_v },
	[HR_DEBUG_NO_TEXTURES]		= {  0, 1, SDLK_t },
	[HR_DEBUG_NO_MIPMAPS]		= {  0, 1, SDLK_u },
	[HR_DEBUG_SELECT_POLYTYPE]	= { -1, 7, SDLK_p },
	[HR_DEBUG_NO_INSTANCING]	= {  0, 1, SDLK_i },
	[HR_DEBUG_SELECT_INSTANCE]	= {  0, 3, SDLK_j }, 
	[HR_DEBUG_NO_LIGHTING]		= {  0, 1, SDLK_l },
	[HR_DEBUG_NO_AMBIENT]		= {  0, 1, SDLK_a },
	[HR_DEBUG_NO_DIFFUSE]		= {  0, 1, SDLK_d },
	[HR_DEBUG_NO_SPECULAR]		= {  0, 1, SDLK_s },
	[HR_DEBUG_FOG]			= {  0, 1, SDLK_g },
};

static void
init_debug_flags (hikaru_renderer_t *hr)
{
	unsigned i;

	for (i = 0; i < NUMELEM (debug_controls); i++)
		hr->debug.flags[i] = debug_controls[i].min;

	hr->debug.flags[HR_DEBUG_LOG] =
		vk_util_get_bool_option ("HR_LOG", false) ? 1 : 0;
}

static void
update_debug_flags (hikaru_renderer_t *hr)
{
	char *msg = hr->base.message;
	unsigned i;

	for (i = 0; i < NUMELEM (debug_controls); i++) {
		uint32_t key = debug_controls[i].key;
		if (key != ~0 && vk_input_get_key (key)) {
			hr->debug.flags[i] += 1;
			if (i == HR_DEBUG_NO_MIPMAPS)
				hikaru_renderer_invalidate_texcache (&hr->base, NULL);
			if (hr->debug.flags[i] > debug_controls[i].max)
				hr->debug.flags[i] = debug_controls[i].min;
		}
		msg += sprintf (msg, "%d|", hr->debug.flags[i]);
	}
}

/****************************************************************************
 Utils
****************************************************************************/

static GLuint
get_uniform_loc (GLuint prog, const char *id)
{
	GLuint loc;

	loc = glGetUniformLocation (prog, id);
	VK_ASSERT_NO_GL_ERROR ();
	if (loc == (GLuint) -1)
		VK_ABORT ("uniform '%s' has no location!", id);

	return loc;
}

/****************************************************************************
 Textures
****************************************************************************/

static bool
is_texhead_eq (hikaru_renderer_t *hr,
               hikaru_texhead_t *a, hikaru_texhead_t *b)
{
	return (a->format == b->format) &&
	       (a->logw == b->logw) &&
	       (a->logh == b->logh) &&
	       (a->bank == b->bank) &&
	       (a->slotx == b->slotx) &&
	       (a->sloty == b->sloty);
}

static void
destroy_texture (hikaru_texture_t *tex)
{
	if (tex->id) {
		glDeleteTextures (1, &tex->id);
		VK_ASSERT_NO_GL_ERROR ();
	}
	memset ((void *) tex, 0, sizeof (hikaru_texture_t));
}

static uint16_t
abgr1111_to_rgba4444 (uint32_t texel)
{
	static const uint16_t table[16] = {
		0x0000, 0xF000, 0x0F00, 0xFF00,
		0x00F0, 0xF0F0, 0x0FF0, 0xFFF0,
		0x000F, 0xF00F, 0x0F0F, 0xFF0F,
		0x00FF, 0xF0FF, 0x0FFF, 0xFFFF,
	};
	return table[texel & 15];
}

#define PUT16(x, y, t) \
	*(uint16_t *) &(data[(y)*w*2 + (x)*2]) = (t)

static void *
decode_texture_abgr1111 (vk_buffer_t *texram,
                         uint32_t w, uint32_t h,
                         uint32_t basex, uint32_t basey)
{
	uint32_t x, y;
	uint8_t *data;

	data = (uint8_t *) malloc (w * (h * 2) * 2);
	if (!data)
		return NULL;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x += 4) {
			uint32_t offs = (basey + y) * 4096 + (basex + x);
			uint32_t texels = vk_buffer_get (texram, 4, offs);
			PUT16 (x + 2, y*2 + 0, abgr1111_to_rgba4444 (texels >> 28));
			PUT16 (x + 3, y*2 + 0, abgr1111_to_rgba4444 (texels >> 24));
	      		PUT16 (x + 2, y*2 + 1, abgr1111_to_rgba4444 (texels >> 20));
			PUT16 (x + 3, y*2 + 1, abgr1111_to_rgba4444 (texels >> 16));
	      		PUT16 (x + 0, y*2 + 0, abgr1111_to_rgba4444 (texels >> 12));
			PUT16 (x + 1, y*2 + 0, abgr1111_to_rgba4444 (texels >> 8));
	      		PUT16 (x + 0, y*2 + 1, abgr1111_to_rgba4444 (texels >> 4));
			PUT16 (x + 1, y*2 + 1, abgr1111_to_rgba4444 (texels >> 0));
		}
	}
	return (void *) data;
}

#undef PUT16

static GLuint
upload_texture (hikaru_renderer_t *hr, hikaru_texhead_t *th)
{
	static const GLint la8_swizzle[4] = { GL_RED, GL_RED, GL_RED, GL_GREEN };

	uint32_t w, h, num_levels, level, basex, basey, bank;
	GLuint id;

	w = 16 << th->logw;
	h = 16 << th->logh;
	num_levels = hr->debug.flags[HR_DEBUG_NO_MIPMAPS] ? 1 :
	             MIN2 (th->logw, th->logh) + 4;

	get_texhead_coords (&basex, &basey, th);
	bank = th->bank;

	glGenTextures (1, &id);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 0);
	VK_ASSERT_NO_GL_ERROR ();

	glBindTexture (GL_TEXTURE_2D, id);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT);
//	                 (th->wrapu == 0) ? GL_CLAMP_TO_EDGE :
//	                 (th->repeatu == 0) ? GL_REPEAT : GL_MIRRORED_REPEAT);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT);
//	                 (th->wrapv == 0) ? GL_CLAMP_TO_EDGE :
//	                 (th->repeatv == 0) ? GL_REPEAT : GL_MIRRORED_REPEAT);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	VK_ASSERT_NO_GL_ERROR ();

	/* XXX hack to make textures slightly less blurry. */
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -1);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_levels - 1);
	VK_ASSERT_NO_GL_ERROR ();

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	VK_ASSERT_NO_GL_ERROR ();

	for (level = 0; level < num_levels; level++) {
		void *data = (void *) hr->gpu->texram[bank]->ptr;

		switch (th->format) {
		case HIKARU_FORMAT_ABGR1555:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			VK_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGB5_A1,
			              w, h, 0,
			              GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
			              data);
			VK_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_ABGR4444:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			VK_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGBA4,
			              w, h, 0,
			              GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV,
			              data);
			VK_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_LA8:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			VK_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGBA8,
			              w, h, 0,
			              GL_RG, GL_UNSIGNED_BYTE,
			              data);
			VK_ASSERT_NO_GL_ERROR ();

			glTexParameteriv (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA,
			                  la8_swizzle);
			VK_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_ABGR1111:
			data = decode_texture_abgr1111 (hr->gpu->texram[bank],
			                                w, h, basex, basey);
			if (!data)
				goto fail;

			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RGBA4,
			              w, h * 2, 0,
			              GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4_REV,
			              data);
			VK_ASSERT_NO_GL_ERROR ();

			free (data);
			break;
		default:
			goto fail;
		}

		w >>= 1;
		h >>= 1;
		VK_ASSERT (w && h);

		basex += (2048 - basex) / 2;
		basey += (1024 - basey) / 2;
		bank ^= 1;
	}

	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	VK_ASSERT_NO_GL_ERROR ();

	return id;

fail:
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	VK_ASSERT_NO_GL_ERROR ();

	glDeleteTextures (1, &id);
	VK_ASSERT_NO_GL_ERROR ();

	return 0;
}

hikaru_texture_t *
get_texture (hikaru_renderer_t *hr, hikaru_texhead_t *th)
{
	hikaru_texture_t *cached;
	uint32_t bank, slotx, sloty;
	GLuint id;

	bank  = th->bank;
	slotx = th->slotx;
	sloty = th->sloty;

	if (slotx < 0x80 || sloty < 0xC0)
		return NULL;

	slotx -= 0x80;
	sloty -= 0xC0;

	cached = &hr->textures.cache[bank][sloty][slotx];
	if (is_texhead_eq (hr, th, &cached->th))
		return cached;

	destroy_texture (cached);

	id = upload_texture (hr, th);
	if (!id) {
		destroy_texture (cached);
		return NULL;
	}

	cached->th = *th;
	cached->id = id;

	hr->textures.is_clear[bank] = false;
	return cached;
}

static void
clear_texcache_bank (hikaru_renderer_t *hr, unsigned bank)
{
	unsigned x, y;

	if (hr->textures.is_clear[bank])
		return;
	hr->textures.is_clear[bank] = true;

	/* Free all allocated surfaces. */
	for (y = 0; y < 0x40; y++)
		for (x = 0; x < 0x80; x++)
			destroy_texture (&hr->textures.cache[bank][y][x]);

	/* Zero out the cache itself, to avoid spurious hits. Note that
	 * texture RAM origin is (80,C0), so (slotx, sloty) will never match
	 * a zeroed out cache entries. */
	memset ((void *) &hr->textures.cache[bank], 0, sizeof (hr->textures.cache[bank]));
}

void
hikaru_renderer_invalidate_texcache (vk_renderer_t *rend, hikaru_texhead_t *th)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;

	VK_ASSERT (hr);

	if (th == NULL) {
		clear_texcache_bank (hr, 0);
		clear_texcache_bank (hr, 1);
	} else
		clear_texcache_bank (hr, th->bank);
}

/****************************************************************************
 State
****************************************************************************/

static const char *mesh_vs_source =
"#version 140									\n \
										\n \
#extension GL_ARB_explicit_attrib_location : require				\n \
										\n \
%s										\n \
										\n \
uniform mat4 u_projection;							\n \
uniform mat4 u_modelview;							\n \
uniform mat3 u_normal;								\n \
										\n \
layout(location = 0) in vec3 i_position;					\n \
layout(location = 1) in vec3 i_normal;						\n \
layout(location = 2) in vec3 i_diffuse;						\n \
layout(location = 3) in vec3 i_ambient;						\n \
layout(location = 4) in vec4 i_specular;					\n \
layout(location = 5) in vec3 i_unknown;						\n \
layout(location = 6) in vec2 i_texcoords;					\n \
layout(location = 7) in float i_alpha;						\n \
										\n \
out vec4 p_position;								\n \
out vec3 p_normal;								\n \
out vec3 p_diffuse;								\n \
out vec4 p_specular;								\n \
out vec3 p_ambient;								\n \
out vec3 p_unknown;								\n \
out vec2 p_texcoords;								\n \
out float p_alpha;								\n \
										\n \
void main (void) {								\n \
	p_position = u_modelview * vec4 (i_position, 1.0);			\n \
	gl_Position = u_projection * p_position;				\n \
										\n \
	p_normal = u_normal * i_normal;						\n \
										\n \
	p_diffuse = i_diffuse;							\n \
	p_ambient = i_ambient;							\n \
	p_specular = i_specular;						\n \
	p_unknown = i_unknown;							\n \
	p_texcoords = i_texcoords;						\n \
	p_alpha = i_alpha;							\n \
}";

static const char *mesh_fs_source =
"#version 140									\n \
										\n \
%s										\n \
										\n \
struct light_t {								\n \
	vec3 position;								\n \
	vec3 direction;								\n \
	vec3 diffuse;								\n \
	vec3 specular;								\n \
	vec2 extents;								\n \
};										\n \
										\n \
uniform light_t		u_lights[4];						\n \
uniform vec3		u_ambient;						\n \
uniform sampler2D	u_texture;						\n \
uniform vec2		u_fog;							\n \
uniform vec3		u_fog_color;						\n \
										\n \
in vec4 p_position;								\n \
in vec3 p_normal;								\n \
in vec3 p_diffuse;								\n \
in vec4 p_specular;								\n \
in vec3 p_ambient;								\n \
in vec3 p_unknown;								\n \
in vec2 p_texcoords;								\n \
in float p_alpha;								\n \
										\n \
void										\n \
apply_light (inout vec3 diffuse,						\n \
             inout vec3 specular,						\n \
             in light_t light,							\n \
             in int type,							\n \
             in int att_type,							\n \
             in int has_specular)						\n \
{										\n \
	vec3 light_direction;							\n \
	float distance, attenuation, intensity;					\n \
										\n \
	if (type == 0) {							\n \
		light_direction = normalize (light.direction);			\n \
		distance = 0.001;						\n \
	} else {								\n \
		vec3 delta = light.position - p_position.xyz;			\n \
		distance = length (delta);					\n \
		light_direction = normalize (delta);				\n \
	}									\n \
										\n \
	if (att_type == 1)							\n \
		distance = distance*distance;					\n \
	else if (att_type == 2)							\n \
		distance = 1.0 / distance;					\n \
	else if (att_type == 3)							\n \
		distance = 1.0 / (distance*distance);				\n \
	attenuation = light.extents.x * (light.extents.y + distance);		\n \
	attenuation = clamp (attenuation, 0.0, 1.0);				\n \
										\n \
	vec3 normal = normalize (p_normal);					\n \
										\n \
#if HAS_TWO_SIDED_LIGHTING							\n \
	intensity = abs (dot (normal, light_direction));			\n \
#else										\n \
	intensity = max (dot (normal, light_direction), 0.0);			\n \
#endif										\n \
	if (type == 2) {							\n \
		vec3 spot_direction = normalize (light.direction);		\n \
		if (dot (spot_direction, light_direction) < 0.95)		\n \
			intensity = 0.0;					\n \
	}									\n \
										\n \
	diffuse += attenuation * intensity * p_diffuse * light.diffuse;		\n \
										\n \
	if (has_specular != 0) {								\n \
		vec3 view_direction = normalize (-p_position.xyz);				\n \
		vec3 reflect_direction = reflect (-light_direction, normal);			\n \
		float angle = max (dot (view_direction, reflect_direction), 0.0);		\n \
		specular += p_specular.rgb * light.specular * pow (angle, 255.0 * p_specular.a);	\n \
	}											\n \
}										\n \
										\n \
void										\n \
main (void)									\n \
{										\n \
	vec4 texel, color;							\n \
										\n \
#if HAS_TEXTURE									\n \
	texel = texture (u_texture, p_texcoords);				\n \
#else										\n \
	texel = vec4 (1.0);							\n \
#endif										\n \
										\n \
#if HAS_LIGHTING								\n \
	vec3 diffuse  = vec3 (0.0);						\n \
	vec3 specular = vec3 (0.0);						\n \
	vec3 ambient  = u_ambient * p_ambient;					\n \
										\n \
#if HAS_LIGHT0									\n \
	apply_light (diffuse, specular, u_lights[0], LIGHT0_TYPE, LIGHT0_ATT_TYPE, HAS_LIGHT0_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT1									\n \
	apply_light (diffuse, specular, u_lights[1], LIGHT1_TYPE, LIGHT1_ATT_TYPE, HAS_LIGHT1_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT2									\n \
	apply_light (diffuse, specular, u_lights[2], LIGHT2_TYPE, LIGHT2_ATT_TYPE, HAS_LIGHT2_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT3									\n \
	apply_light (diffuse, specular, u_lights[3], LIGHT3_TYPE, LIGHT3_ATT_TYPE, HAS_LIGHT3_SPECULAR);		\n \
#endif										\n \
										\n \
	color = vec4 (ambient +  diffuse, p_alpha) * texel + vec4 (specular, 0.0);	\n \
#else										\n \
	color = vec4 (p_ambient, p_alpha) * texel;				\n \
#endif										\n \
										\n \
#if HAS_FOG									\n \
	float z = gl_FragCoord.z / gl_FragCoord.w;				\n \
	float a = clamp (u_fog.x * (z - u_fog.y), 0.0, 1.0);			\n \
	gl_FragColor = mix (color, vec4 (u_fog_color, 1.0), a);			\n \
#else										\n \
	gl_FragColor = color;							\n \
#endif										\n \
}";

static hikaru_light_att_t
get_light_attenuation_type (hikaru_light_t *lit)
{
	if (lit->att_type == 0 &&
	    lit->attenuation[0] == 1.0f &&
	    lit->attenuation[1] == 1.0f)
		return HIKARU_LIGHT_ATT_INF;
	return lit->att_type;
}

static hikaru_light_type_t
get_light_type (hikaru_light_t *lit)
{
	/* Violated by BRAVEFF, in the intro sequence; it recalls a lightset
	 * at the wrong base, and poof. */
	if (!lit->has_direction && !lit->has_position)
		VK_ERROR ("recalling light with no direction nor position!");
	if (get_light_attenuation_type (lit) == HIKARU_LIGHT_ATT_INF)
		return HIKARU_LIGHT_TYPE_DIRECTIONAL;
	else if (lit->has_direction && lit->has_position)
		return HIKARU_LIGHT_TYPE_SPOT;
	else if (lit->has_position)
		return HIKARU_LIGHT_TYPE_POSITIONAL;
	return HIKARU_LIGHT_TYPE_DIRECTIONAL;
}

static hikaru_glsl_variant_t
get_glsl_variant (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_material_t *mat	= &hr->mat_list[mesh->mat_index];
	hikaru_lightset_t *ls	= &hr->ls_list[mesh->ls_index];
	hikaru_glsl_variant_t variant;

	VK_ASSERT (mesh->vp_index != ~0);

	variant.full = 0;

	if (mesh->mat_index == ~0) {
		VK_ERROR ("no material.");
		return variant;
	}
	if (mesh->tex_index == ~0) {
		VK_ERROR ("no texhead.");
		return variant;
	}
	if (mesh->ls_index == ~0) {
		VK_ERROR ("no lightset.");
		return variant;
	}

	variant.has_texture	= mat->has_texture &&
	                          !hr->debug.flags[HR_DEBUG_NO_TEXTURES];

	variant.has_lighting	= ls->mask != 0xF &&
	                          mat->shading_mode != 0 &&
	                          !hr->debug.flags[HR_DEBUG_NO_LIGHTING];

	variant.has_fog		= mat->depth_blend == 0 &&
	                          hr->debug.flags[HR_DEBUG_FOG];

	if (!variant.has_lighting)
		return variant;

	variant.has_phong		= mat->shading_mode == 2;

	variant.has_light0		= !(ls->mask & (1 << 0));
	variant.light0_type		= get_light_type (&ls->lights[0]);
	variant.light0_att_type		= get_light_attenuation_type (&ls->lights[0]);
	variant.has_light0_specular	= ls->lights[0].has_specular;

	variant.has_light1		= !(ls->mask & (1 << 1));
	variant.light1_type		= get_light_type (&ls->lights[1]);
	variant.light1_att_type		= get_light_attenuation_type (&ls->lights[1]);
	variant.has_light1_specular	= ls->lights[1].has_specular;

	variant.has_light2		= !(ls->mask & (1 << 2));
	variant.light2_type		= get_light_type (&ls->lights[2]);
	variant.light2_att_type		= get_light_attenuation_type (&ls->lights[2]);
	variant.has_light2_specular	= ls->lights[2].has_specular;

	variant.has_light3		= !(ls->mask & (1 << 3));
	variant.light3_type		= get_light_type (&ls->lights[3]);
	variant.light3_att_type		= get_light_attenuation_type (&ls->lights[3]);
	variant.has_light3_specular	= ls->lights[3].has_specular;

	if (mesh->has_two_sided_lighting)
		variant.has_two_sided_lighting = 1;

	return variant;
}

#define MAX_PROGRAMS	256

static struct {
	hikaru_glsl_variant_t variant;
	GLuint program;
} program_cache[MAX_PROGRAMS];
unsigned num_programs;

static void
upload_glsl_program (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	static const char *definitions_template =
	"#define HAS_TEXTURE %d\n"
	"#define HAS_LIGHTING %d\n"
	"#define HAS_TWO_SIDED_LIGHTING %d\n"
	"#define HAS_PHONG %d\n"
	"#define HAS_LIGHT0 %d\n"
	"#define LIGHT0_TYPE %d\n"
	"#define LIGHT0_ATT_TYPE %d\n"
	"#define HAS_LIGHT0_SPECULAR %d\n"
	"#define HAS_LIGHT1 %d\n"
	"#define LIGHT1_TYPE %d\n"
	"#define LIGHT1_ATT_TYPE %d\n"
	"#define HAS_LIGHT1_SPECULAR %d\n"
	"#define HAS_LIGHT2 %d\n"
	"#define LIGHT2_TYPE %d\n"
	"#define LIGHT2_ATT_TYPE %d\n"
	"#define HAS_LIGHT2_SPECULAR %d\n"
	"#define HAS_LIGHT3 %d\n"
	"#define LIGHT3_TYPE %d\n"
	"#define LIGHT3_ATT_TYPE %d\n"
	"#define HAS_LIGHT3_SPECULAR %d\n"
	"#define HAS_FOG %d\n";

	hikaru_glsl_variant_t variant;
	char *definitions, *vs_source, *fs_source;
	int ret, i;

	variant = get_glsl_variant (hr, mesh);
	if (hr->meshes.variant.full == variant.full)
		return;

	hr->meshes.variant.full = variant.full;

	for (i = 0; i < num_programs; i++) {
		if (program_cache[i].variant.full == variant.full) {
			hr->meshes.program = program_cache[i].program;
			goto update_locations;
		}
	}

	VK_LOG ("compiling shader for variant %lX", variant.full);

	ret = asprintf (&definitions, definitions_template,
	                variant.has_texture,
	                variant.has_lighting,
	                variant.has_two_sided_lighting,
	                variant.has_phong,
	                variant.has_light0,
	                variant.light0_type,
	                variant.light0_att_type,
	                variant.has_light0_specular,
	                variant.has_light1,
	                variant.light1_type,
	                variant.light1_att_type,
	                variant.has_light1_specular,
	                variant.has_light2,
	                variant.light2_type,
	                variant.light2_att_type,
	                variant.has_light2_specular,
	                variant.has_light3,
	                variant.light3_type,
	                variant.light3_att_type,
	                variant.has_light3_specular,
	                variant.has_fog);
	VK_ASSERT (ret >= 0);

	ret = asprintf (&vs_source, mesh_vs_source, definitions);
	VK_ASSERT (ret >= 0);

	ret = asprintf (&fs_source, mesh_fs_source, definitions);
	VK_ASSERT (ret >= 0);

	hr->meshes.variant = variant;
	hr->meshes.program = vk_renderer_compile_program (vs_source, fs_source);

	free (definitions);
	free (vs_source);
	free (fs_source);

	if (0) {
		vk_renderer_print_uniforms (hr->meshes.program);
		VK_ASSERT_NO_GL_ERROR ();
	}

	program_cache[num_programs].variant.full = variant.full;
	program_cache[num_programs].program = hr->meshes.program;
	num_programs++;
	VK_ASSERT (num_programs < MAX_PROGRAMS);

update_locations:

	glUseProgram (hr->meshes.program);
	VK_ASSERT_NO_GL_ERROR ();

	hr->meshes.locs.u_projection =
		glGetUniformLocation (hr->meshes.program, "u_projection");
	hr->meshes.locs.u_modelview =
		glGetUniformLocation (hr->meshes.program, "u_modelview");
	hr->meshes.locs.u_normal =
		glGetUniformLocation (hr->meshes.program, "u_normal");
	for (i = 0; i < 4; i++) {
		char temp[64];

		sprintf (temp, "u_lights[%d].position", i);
		hr->meshes.locs.u_lights[i].position =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].direction", i);
		hr->meshes.locs.u_lights[i].direction =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].diffuse", i);
		hr->meshes.locs.u_lights[i].diffuse =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].specular", i);
		hr->meshes.locs.u_lights[i].specular =
			glGetUniformLocation (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].extents", i);
		hr->meshes.locs.u_lights[i].extents =
			glGetUniformLocation (hr->meshes.program, temp);
	}
	hr->meshes.locs.u_ambient =
		glGetUniformLocation (hr->meshes.program, "u_ambient");
	hr->meshes.locs.u_texture =
		glGetUniformLocation (hr->meshes.program, "u_texture");
	hr->meshes.locs.u_fog =
		glGetUniformLocation (hr->meshes.program, "u_fog");
	hr->meshes.locs.u_fog_color =
		glGetUniformLocation (hr->meshes.program, "u_fog_color");
	VK_ASSERT_NO_GL_ERROR ();
}

static void
upload_viewport (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_viewport_t *vp = &hr->vp_list[mesh->vp_index];
	const float h = vp->clip.t - vp->clip.b;
	const float w = vp->clip.r - vp->clip.l;
	const float n_over_f = vp->clip.n / vp->clip.f;
	const float hh_at_n = (h / 2.0f) * n_over_f;
	const float hw_at_n = hh_at_n * (w / h);
	const float dcx = (vp->offset.x - (w / 2.0f));
	const float dcy = (vp->offset.y - (h / 2.0f));
	mtx4x4f_t projection;

	VK_ASSERT (mesh->vp_index != ~0);

	if (!isnonnegative (vp->clip.l) || !isnonnegative (vp->clip.r) ||
	    !isnonnegative (vp->clip.b) || !isnonnegative (vp->clip.t) ||
	    !isnonnegative (vp->clip.f) || !isnonnegative (vp->clip.n)) {
		VK_ERROR ("non-positive viewport clipping planes: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	if ((vp->clip.l >= vp->clip.r) ||
	    (vp->clip.b >= vp->clip.t) ||
	    (vp->clip.n >= vp->clip.f)) {
		VK_ERROR ("inverted viewport clipping planes: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	if (!isnonnegative (vp->offset.x) || (vp->offset.x >= 640.0f) ||
	    !isnonnegative (vp->offset.y) || (vp->offset.y >= 480.0f)) {
		VK_ERROR ("invalid viewport offset: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	LOG ("vp  = %s : [w=%f h=%f dcx=%f dcy=%f]",
	     get_viewport_str (vp), w, h, dcx, dcy);

	vk_renderer_frustum (projection,
	                     -hw_at_n, hw_at_n,
	                     -hh_at_n, hh_at_n,
	                     vp->clip.n, 1e5);
//	vk_renderer_translate (projection, dcx, -dcy, 0.0f);

	glUniformMatrix4fv (hr->meshes.locs.u_projection, 1, GL_FALSE,
	                    (const GLfloat *) projection);

	glViewport (vp->clip.l,
	            vp->clip.b,
	            vp->clip.r - vp->clip.l,
	            vp->clip.t - vp->clip.b);

	if (hr->meshes.variant.has_fog) {
		vec2f_t fog;
		vec3f_t fog_color;

		fog[0] = vp->depth.density;
		fog[1] = vp->depth.bias;
		glUniform2fv (hr->meshes.locs.u_fog, 1, fog);

		fog_color[0] = vp->depth.mask[0] * INV255;
		fog_color[1] = vp->depth.mask[1] * INV255;
		fog_color[2] = vp->depth.mask[2] * INV255;
		glUniform3fv (hr->meshes.locs.u_fog_color, 1, fog_color);
	}
}

static void
upload_modelview (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, unsigned i)
{
	hikaru_modelview_t *mv = &hr->mv_list[mesh->mv_index + i];
	mtx3x3f_t normal;

	if (mesh->mv_index == ~0) {
		static const hikaru_modelview_t identity_mv = {
			.mtx = {
				{ 1.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f, 0.0f },
				{ 0.0f, 0.0f, 0.0f, 1.0f }
			}
		};

		VK_ERROR ("attempting to draw with no modelview!");

		/* Attempt to render something anyway. */
		mv = (hikaru_modelview_t *) &identity_mv;
	}

	vk_renderer_compute_normal_matrix (normal, mv->mtx);

	LOG ("mv  = [%u+%u] %s", mesh->mv_index, i, get_modelview_str (mv));
	glUniformMatrix4fv (hr->meshes.locs.u_modelview, 1, GL_FALSE,
	                    (const GLfloat *) mv->mtx);

	glUniformMatrix3fv (hr->meshes.locs.u_normal, 1, GL_FALSE,
	                    (const GLfloat *) normal);
}

static void
upload_material_texhead (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_texture_t *tex;

	if (!hr->meshes.variant.has_texture)
		return;

	tex = get_texture (hr, &hr->tex_list[mesh->tex_index]);

	glActiveTexture (GL_TEXTURE0 + 0);
	VK_ASSERT_NO_GL_ERROR ();

	glBindTexture (GL_TEXTURE_2D, tex ? tex->id : 0);
	VK_ASSERT_NO_GL_ERROR ();

	glUniform1i (hr->meshes.locs.u_texture, 0);
	VK_ASSERT_NO_GL_ERROR ();
}

static void
get_light_ambient (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, float *out)
{
	hikaru_viewport_t *vp =
		(mesh->vp_index == ~0) ? NULL : &hr->vp_list[mesh->vp_index];

	if (hr->debug.flags[HR_DEBUG_NO_AMBIENT] || !vp)
		out[0] = out[1] = out[2] = 0.0f;
	else {
		out[0] = vp->color.ambient[0] * INV255;
		out[1] = vp->color.ambient[1] * INV255;
		out[2] = vp->color.ambient[2] * INV255;
	}
}

static void
get_light_diffuse (hikaru_renderer_t *hr, hikaru_light_t *lit, float *out)
{
	if (hr->debug.flags[HR_DEBUG_NO_DIFFUSE])
		out[0] = out[1] = out[2] = 0.0f;
	else {
		out[0] = lit->diffuse[0] * INV255;
		out[1] = lit->diffuse[1] * INV255;
		out[2] = lit->diffuse[2] * INV255;
	}
}

static void
get_light_specular (hikaru_renderer_t *hr, hikaru_light_t *lit, float *out)
{
	if (!lit->has_specular || hr->debug.flags[HR_DEBUG_NO_SPECULAR])
		out[0] = out[1] = out[2] = 0.0f;
	else {
		/* Make sure that specular enabled matches non-zero specular
		 * colors. */
		if (lit->specular[0] == 0.0f &&
		    lit->specular[1] == 0.0f &&
		    lit->specular[0] == 0.0f)
			VK_ERROR ("light with specular enabled but no specular color!");

		out[0] = lit->specular[0] * INV255;
		out[1] = lit->specular[1] * INV255;
		out[2] = lit->specular[2] * INV255;
	}
}

static void
upload_lightset (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_lightset_t *ls = &hr->ls_list[mesh->ls_index];
	float tmp[4];
	unsigned i;

	if (!hr->meshes.variant.has_lighting)
		return;

	LOG ("lightset = %s", get_lightset_str (ls));

	get_light_ambient (hr, mesh, tmp);
	glUniform3fv (hr->meshes.locs.u_ambient, 1, (const GLfloat *) tmp);
	VK_ASSERT_NO_GL_ERROR ();

	for (i = 0; i < 4; i++) {
		hikaru_light_t *lt = &ls->lights[i];

		if (ls->mask & (1 << i))
			continue;

		glUniform3fv (hr->meshes.locs.u_lights[i].position, 1,
		              lt->position);
		VK_ASSERT_NO_GL_ERROR ();

		glUniform3fv (hr->meshes.locs.u_lights[i].direction, 1,
		              lt->direction);
		VK_ASSERT_NO_GL_ERROR ();

		get_light_diffuse (hr, lt, tmp);
		glUniform3fv (hr->meshes.locs.u_lights[i].diffuse, 1, tmp);
		VK_ASSERT_NO_GL_ERROR ();

		get_light_specular (hr, lt, tmp);
		glUniform3fv (hr->meshes.locs.u_lights[i].specular, 1, tmp);
		VK_ASSERT_NO_GL_ERROR ();

		glUniform2fv (hr->meshes.locs.u_lights[i].extents, 1,
		              lt->attenuation);
	}
}

/****************************************************************************
 Meshes
****************************************************************************/

#define VK_COPY_VEC2F(dst_, src_) \
	do { \
		dst_[0] = src_[0]; \
		dst_[1] = src_[1]; \
	} while (0)

#define VK_COPY_VEC3F(dst_, src_) \
	do { \
		dst_[0] = src_[0]; \
		dst_[1] = src_[1]; \
		dst_[2] = src_[2]; \
	} while (0)

static float
clampf (float x, float min_, float max_)
{
	return (min_ > x) ? min_ :
	       (max_ < x) ? max_ : x;
}

static void
copy_colors (hikaru_renderer_t *hr, hikaru_vertex_t *dst, hikaru_vertex_t *src)
{
	hikaru_gpu_t *gpu = hr->gpu;
	hikaru_material_t *mat = &MAT.scratch;
	float alpha;
	unsigned i;

	for (i = 0; i < 3; i++)
		dst->body.diffuse[i] = mat->diffuse[i];

	for (i = 0; i < 3; i++)
		dst->body.ambient[i] = mat->ambient[i];

	for (i = 0; i < 4; i++)
		dst->body.specular[i] = mat->specular[i];

	for (i = 0; i < 3; i++)
		dst->body.unknown[i] = mat->unknown[i];

	/* Patch diffuse alpha depending on poly type. NOTE: transparent
	 * polygons also have an alpha, with unknown meaning (it seems to have
	 * opposite sign w.r.t. translucent alpha though). */
	alpha = 1.0f;
	if (POLY.type == HIKARU_POLYTYPE_TRANSLUCENT) {
		float p_alpha = POLY.alpha;
		float v_alpha = src->info.alpha * INV255;
		alpha = clampf (p_alpha * v_alpha, 0.0f, 1.0f);
	}
	dst->body.alpha = (uint8_t) (alpha * 255.0f);
}

static void
copy_texcoords (hikaru_renderer_t *hr,
                hikaru_vertex_t *dst, hikaru_vertex_t *src)
{
	hikaru_gpu_t *gpu = hr->gpu;
	hikaru_texhead_t *th = &TEX.scratch;
	float w = 16 << th->logw;
	float h = 16 << th->logh;

	if (th->format == HIKARU_FORMAT_ABGR1111)
		h *= 2;

	dst->body.texcoords[0] = (src->body.texcoords[0] + (0.5f * gpu->texoffset_x)) / w;
	dst->body.texcoords[1] = (src->body.texcoords[1] + (0.5f * gpu->texoffset_y)) / h;
}

static void
add_triangle (hikaru_renderer_t *hr)
{
	if (hr->push.num_verts >= 3) {
		uint32_t index = hr->push.num_tris * 3;
		hikaru_vertex_body_t *dst = &hr->push.all[index];

		VK_ASSERT ((index + 2) < MAX_VERTICES_PER_MESH);

		if (hr->push.tmp[2].info.twosided &&
		    !hr->push.tmp[2].info.nocull)
			VK_ERROR ("got a vertex with culling and two-sided lighting!");

		if (hr->push.tmp[2].info.twosided)
			hr->meshes.current->has_two_sided_lighting = 1;

		if (hr->push.tmp[2].info.nocull) {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[2].body;
			dst[2] = hr->push.tmp[1].body;
			dst[3] = hr->push.tmp[0].body;
			dst[4] = hr->push.tmp[1].body;
			dst[5] = hr->push.tmp[2].body;
			hr->push.num_tris += 1;
		} else if (hr->push.tmp[2].info.winding) {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[2].body;
			dst[2] = hr->push.tmp[1].body;
		} else {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[1].body;
			dst[2] = hr->push.tmp[2].body;
		}
		hr->push.num_tris += 1;
	}
}

void
hikaru_renderer_push_vertices (vk_renderer_t *rend,
                               hikaru_vertex_t *v,
                               uint32_t flags,
                               unsigned num)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned i, vp_index = VP0.depth.func;

	VK_ASSERT (hr);
	VK_ASSERT (v);
	VK_ASSERT (num == 1 || num == 3);
	VK_ASSERT (v->info.tricap == 0 || v->info.tricap == 7);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	if (hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] >= 0 &&
	    hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] != vp_index)
		return;

	switch (num) {

	case 1:
		/* Note that VTX(2) always points to the last pushed vertex,
		 * which for instructions 12x, 1Ax and 1Bx means the vertex
		 * pushed by the instruction itself, and for instructions 1Ex
		 * and 15x the vertex pushed by the previous "push"
		 * instruction.
		 */

		/* If the incoming vertex includes the position, push it
		 * in the temporary buffer, updating it according to the
		 * p(osition)pivot bit. */
		if (flags & HR_PUSH_POS) {

			/* Do not change the pivot if it is not required */
			if (!v->info.ppivot)
				hr->push.tmp[0] = hr->push.tmp[1];
			hr->push.tmp[1] = hr->push.tmp[2];
			memset ((void *) &hr->push.tmp[2], 0, sizeof (hikaru_vertex_t));

			/* Set the position, colors and alpha. */
			VK_COPY_VEC3F (hr->push.tmp[2].body.position, v->body.position);
			copy_colors (hr, &hr->push.tmp[2], v);

			/* Account for the added vertex. */
			hr->push.num_verts += 1;
			VK_ASSERT (hr->push.num_verts < MAX_VERTICES_PER_MESH);
		}

		/* Set the normal. */
		if (flags & HR_PUSH_NRM)
			VK_COPY_VEC3F (hr->push.tmp[2].body.normal, v->body.normal);

		/* Set the texcoords. */
		if (flags & HR_PUSH_TXC)
			copy_texcoords (hr, &hr->push.tmp[2], v);
		break;

	case 3:
		VK_ASSERT (flags == HR_PUSH_TXC);

		if (hr->push.num_verts < 3)
			return;

		for (i = 0; i < 3; i++)
			copy_texcoords (hr, &hr->push.tmp[2 - i], &v[i]);
		break;

	default:
		VK_ASSERT (!"num is neither 1 nor 3");
		break;
	}

	/* Finish the previous triangle. */
	if (v[0].info.tricap == 7) {
		hr->push.tmp[2].info.full = v[0].info.full;
		add_triangle (hr);
	}
}

static void
print_rendstate (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, char *prefix)
{
	LOG ("RENDSTATE %s @%p #instances = %u", prefix, mesh, mesh->num_instances);
	if (mesh->vp_index < MAX_VIEWPORTS)
		LOG ("RENDSTATE %s %u vp:  %s", prefix, mesh->num,
		     get_viewport_str (&hr->vp_list[mesh->vp_index]));
	if (mesh->mv_index < MAX_MODELVIEWS)
		LOG ("RENDSTATE %s %u mv:  %s", prefix, mesh->num,
		     get_modelview_str (&hr->mv_list[mesh->mv_index]));
	if (mesh->mat_index < MAX_MATERIALS)
		LOG ("RENDSTATE %s %u mat: %s", prefix, mesh->num,
		     get_material_str (&hr->mat_list[mesh->mat_index]));
	if (mesh->tex_index < MAX_TEXHEADS)
		LOG ("RENDSTATE %s %u tex: %s", prefix, mesh->num,
		     get_texhead_str (&hr->tex_list[mesh->tex_index]));
	if (mesh->ls_index < MAX_LIGHTSETS)
		LOG ("RENDSTATE %s %u ls:  %s", prefix, mesh->num,
		     get_lightset_str (&hr->ls_list[mesh->ls_index]));
}

/* TODO check boundary conditions when nothing is uploaded in a frame. */
/* TODO alpha threshold */
static void
update_and_set_rendstate (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned i;

	memset ((void *) mesh, 0, sizeof (hikaru_mesh_t));

	LOG ("RENDSTATE updating vp %u/%u", hr->num_vps, MAX_VIEWPORTS);
	hr->vp_list[hr->num_vps++] = VP0;
	VK_ASSERT (hr->num_vps < MAX_VIEWPORTS);

	LOG ("RENDSTATE updating mat %u/%u", hr->num_mats, MAX_MATERIALS);
	hr->mat_list[hr->num_mats++] = MAT0;
	VK_ASSERT (hr->num_mats < MAX_MATERIALS);

	LOG ("RENDSTATE updating tex %u/%u", hr->num_texs, MAX_TEXHEADS);
	hr->tex_list[hr->num_texs++] = TEX0;
	VK_ASSERT (hr->num_texs < MAX_TEXHEADS);

	LOG ("RENDSTATE updating ls %u/%u", hr->num_lss, MAX_LIGHTSETS);
	hr->ls_list[hr->num_lss++] = LS0;
	VK_ASSERT (hr->num_lss < MAX_LIGHTSETS);

	/* Copy the per-instance modelviews from last to first. */
	/* TODO optimize by setting MV.total to 0 (and fix the fallback). */
	if (MV.total == ~0) {
		LOG ("RENDSTATE adding no mvs %u/%u [#instances=%u]",
		     hr->num_mvs, MAX_MODELVIEWS, hr->num_instances);

		mesh->mv_index = hr->num_mvs - 1;
		mesh->num_instances = hr->num_instances;
	} else {
		MV.total = 1;

		mesh->mv_index = hr->num_mvs;
		mesh->num_instances = MV.total;
		hr->num_instances = MV.total;

		for (i = 0; i < MV.total; i++, hr->num_mvs++) {
			LOG ("RENDSTATE adding mv %u/%u [#instances=%u]",
			     hr->num_mvs, MAX_MODELVIEWS, MV.total);
	
			VK_ASSERT (hr->num_mvs < MAX_MODELVIEWS);
			hr->mv_list[hr->num_mvs] = MV.table[i];
		}

		MV.total = 0;
		MV.depth = 0;
	}

	mesh->depth_bias = gpu->depth_bias;

	mesh->vp_index = hr->num_vps - 1;
	mesh->mat_index = hr->num_mats - 1;
	mesh->tex_index = hr->num_texs - 1;
	mesh->ls_index = hr->num_lss - 1;

	mesh->num = hr->total_meshes++;
	print_rendstate (hr, mesh, "U");
}

#define OFFSET(member_) \
	((const GLvoid *) offsetof (hikaru_vertex_body_t, member_))

#define VAP(loc_, num_, type_, member_, normalize_) \
	if (loc_ != (GLuint) -1) { \
		glVertexAttribPointer (loc_, num_, type_, normalize_, \
		                       sizeof (hikaru_vertex_body_t), \
		                       OFFSET (member_)); \
		VK_ASSERT_NO_GL_ERROR (); \
		\
		glEnableVertexAttribArray (loc_); \
		VK_ASSERT_NO_GL_ERROR (); \
	}

static void
draw_mesh (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	unsigned i;

	VK_ASSERT (mesh);
	VK_ASSERT (mesh->vbo);

	LOG ("==== DRAWING MESH @%p (#vertices=%u #instances=%u) ====",
	     mesh, mesh->num_tris * 3, mesh->num_instances);

	print_rendstate (hr, mesh, "D");

	glBindVertexArray (hr->meshes.vao);
	VK_ASSERT_NO_GL_ERROR ();

	glBindBuffer (GL_ARRAY_BUFFER, mesh->vbo);
	VK_ASSERT_NO_GL_ERROR ();

	upload_glsl_program (hr, mesh);
	VK_ASSERT_NO_GL_ERROR ();

	upload_viewport (hr, mesh);
	upload_material_texhead (hr, mesh);
	upload_lightset (hr, mesh);
	VK_ASSERT_NO_GL_ERROR ();

	/* We must do it here since locs are computed in upload_glsl_program. */
	VAP (0, 3, GL_FLOAT,          position,  GL_FALSE);
	VAP (1, 3, GL_FLOAT,          normal,    GL_FALSE);
	VAP (2, 3, GL_UNSIGNED_BYTE,  diffuse,   GL_TRUE);
	VAP (3, 3, GL_UNSIGNED_BYTE,  ambient,   GL_TRUE);
	VAP (4, 4, GL_UNSIGNED_BYTE,  specular,  GL_TRUE);
	VAP (5, 3, GL_UNSIGNED_SHORT, unknown,   GL_TRUE);
	VAP (6, 2, GL_FLOAT,          texcoords, GL_FALSE);
	VAP (7, 1, GL_UNSIGNED_BYTE,  alpha,     GL_TRUE);

	glPolygonOffset (0.0f, -mesh->depth_bias);

	if (hr->debug.flags[HR_DEBUG_NO_INSTANCING]) {
		unsigned i = MIN2 (hr->debug.flags[HR_DEBUG_SELECT_INSTANCE],
		                   mesh->num_instances - 1);
		upload_modelview (hr, mesh, i);
		glDrawArrays (GL_TRIANGLES, 0, mesh->num_tris * 3);
	} else {
		for (i = 0; i < mesh->num_instances; i++) {
			upload_modelview (hr, mesh, i);
			glDrawArrays (GL_TRIANGLES, 0, mesh->num_tris * 3);
		}
	}

	glBindVertexArray (0);
}

#undef OFFSET

static void
upload_vertex_data (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	VK_ASSERT (mesh);

	mesh->num_tris = hr->push.num_tris;

	/* Generate the VAO if required. */
	if (!hr->meshes.vao) {
		glGenVertexArrays (1, &hr->meshes.vao);
		VK_ASSERT_NO_GL_ERROR ();
	}

	/* Bind the VAO. */
	glBindVertexArray (hr->meshes.vao);
	VK_ASSERT_NO_GL_ERROR ();

	/* Generate the mesh VBO. */
	glGenBuffers (1, &mesh->vbo);

	/* Upload the vertex data to the VBO. */
	glBindBuffer (GL_ARRAY_BUFFER, mesh->vbo);
	glBufferData (GL_ARRAY_BUFFER,
	              sizeof (hikaru_vertex_body_t) * mesh->num_tris * 3,
	              (const GLvoid *) hr->push.all, GL_DYNAMIC_DRAW);
	VK_ASSERT_NO_GL_ERROR ();
}

void
hikaru_renderer_begin_mesh (vk_renderer_t *rend, uint32_t addr,
                            bool is_static)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned vp_index = VP0.depth.func;
	unsigned polytype = POLY.type;
	unsigned mesh_index = hr->num_meshes[vp_index][polytype];
	hikaru_mesh_t *mesh;

	VK_ASSERT (hr);
	VK_ASSERT (!hr->meshes.current);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	/* Create a new mesh. */
	mesh = &hr->mesh_list[vp_index][polytype][mesh_index++];
	VK_ASSERT (mesh_index < MAX_MESHES);

	hr->num_meshes[vp_index][polytype] = mesh_index;

	/* Make the mesh current and set the rendering state. */
	hr->meshes.current = mesh;
	update_and_set_rendstate (hr, mesh);
	mesh->addr[0] = addr;

	/* Clear the push buffer. */
	hr->push.num_verts = 0;
	hr->push.num_tris = 0;
}

void
hikaru_renderer_end_mesh (vk_renderer_t *rend, uint32_t addr)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;

	VK_ASSERT (hr);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	VK_ASSERT (hr->meshes.current);

	/* Upload the pushed vertex data. */
	upload_vertex_data (hr, hr->meshes.current);
	hr->meshes.current->addr[1] = addr;

	/* Make sure there is no current mesh bound. */
	hr->meshes.current = NULL;
}

static void
draw_meshes_for_polytype (hikaru_renderer_t *hr, unsigned vpi, int polytype)
{
	hikaru_mesh_t *meshes = hr->mesh_list[vpi][polytype];
	unsigned num = hr->num_meshes[vpi][polytype];
	int j;

	if (num == 0)
		return;

	if (hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] >= 0 &&
	    hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] != polytype)
		goto destroy_meshes;

	LOG (" ==== DRAWING VP %u, POLYTYPE %d ====", vpi, polytype);

	switch (polytype) {
	case HIKARU_POLYTYPE_TRANSPARENT:
	case HIKARU_POLYTYPE_TRANSLUCENT:
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask (GL_FALSE);
		glEnable (GL_POLYGON_OFFSET_FILL);
		break;
	case HIKARU_POLYTYPE_BACKGROUND:
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
		glDepthMask (GL_FALSE);
		glDisable (GL_POLYGON_OFFSET_FILL);
		break;
	default:
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
		glDisable (GL_POLYGON_OFFSET_FILL);
		break;
	}
	
	for (j = 0; j < num; j++) {
		hikaru_mesh_t *mesh = &meshes[j];
		draw_mesh (hr, mesh);
	}

destroy_meshes:
	for (j = 0; j < num; j++) {
		hikaru_mesh_t *mesh = &meshes[j];
		if (mesh->vbo)
			glDeleteBuffers (1, &mesh->vbo);
	}
}

static void
draw_scene (hikaru_renderer_t *hr)
{
	static const int sorted_polytypes[] = {
		HIKARU_POLYTYPE_BACKGROUND,
		HIKARU_POLYTYPE_SHADOW_A,
		HIKARU_POLYTYPE_SHADOW_B,
		HIKARU_POLYTYPE_OPAQUE,
		HIKARU_POLYTYPE_TRANSLUCENT,
		HIKARU_POLYTYPE_TRANSPARENT,
	};
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned vpi, i;

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	/* Note that "the pixel ownership test, the scissor test, dithering,
	 * and the buffer writemasks affect the operation of glClear". */
	glDepthMask (GL_TRUE);
	glDisable (GL_SCISSOR_TEST);
	glClearColor (gpu->fb_config.clear[0] * INV255,
	              gpu->fb_config.clear[1] * INV255,
	              gpu->fb_config.clear[2] * INV255,
	              gpu->fb_config.clear[3] * INV255);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glEnable (GL_SCISSOR_TEST);

	glEnable (GL_DEPTH_TEST);
	glDepthFunc (GL_LEQUAL);

	glEnable (GL_CULL_FACE);
	glCullFace (GL_BACK);

	for (vpi = 0; vpi < 8; vpi++) {
		glDepthMask (GL_TRUE);
		glClear (GL_DEPTH_BUFFER_BIT);
		for (i = 0; i < NUMELEM (sorted_polytypes); i++)
			draw_meshes_for_polytype (hr, vpi, sorted_polytypes[i]);
	}

	glDepthMask (GL_TRUE);

	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, 640, 480);
}

static void
destroy_3d_state (hikaru_renderer_t *hr)
{
	unsigned vpi, i;

	free (hr->vp_list);
	free (hr->mv_list);
	free (hr->mat_list);
	free (hr->tex_list);
	free (hr->ls_list);

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			free (hr->mesh_list[vpi][i]);

	vk_renderer_destroy_program (hr->meshes.program);
	VK_ASSERT_NO_GL_ERROR ();

	if (hr->meshes.vao) {
		glBindVertexArray (0);
		glDeleteVertexArrays (1, &hr->meshes.vao);
	}
}

static int
build_3d_state (hikaru_renderer_t *hr)
{
	unsigned vpi, i;

	hr->vp_list = (hikaru_viewport_t *)
			malloc (sizeof (hikaru_viewport_t) * MAX_VIEWPORTS);
	hr->mv_list = (hikaru_modelview_t *)
			malloc (sizeof (hikaru_modelview_t) * MAX_MODELVIEWS);
	hr->mat_list = (hikaru_material_t *)
			malloc (sizeof (hikaru_material_t) * MAX_MATERIALS);
	hr->tex_list = (hikaru_texhead_t *)
			malloc (sizeof (hikaru_texhead_t) * MAX_TEXHEADS);
	hr->ls_list = (hikaru_lightset_t *)
			malloc (sizeof (hikaru_lightset_t) * MAX_LIGHTSETS);

	if (!hr->vp_list || !hr->mv_list || !hr->mat_list || !hr->tex_list || !hr->ls_list)
		return -1;

	for (vpi = 0; vpi < 8; vpi++) {
		for (i = 0; i < 8; i++) {
			hr->mesh_list[vpi][i] =
				(hikaru_mesh_t *) malloc (sizeof (hikaru_mesh_t) * MAX_MESHES);
			if (!hr->mesh_list[vpi][i])
				return -1;
		}
	}

	return 0;
}

/****************************************************************************
 Framebuffers
****************************************************************************/

static const struct {
       vec3f_t position;
       vec2f_t texcoords;
} fb_vbo_data[] = {
       { { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
       { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
       { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
       { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
};

static const char *fb_vs_source =
"#version 140									\n"
"										\n"
"#extension GL_ARB_explicit_attrib_location : require				\n"
"										\n"
"uniform mat4 u_projection =							\n"
"	mat4 (vec4 ( 2.0,  0.0,  0.0,  0.0),					\n"
"	      vec4 ( 0.0, -2.0,  0.0,  0.0),					\n"
"	      vec4 ( 0.0,  0.0, -1.0,  0.0),					\n"
"	      vec4 (-1.0,  1.0,  0.0,  1.0));					\n"
"										\n"
"layout(location = 0) in vec3 i_position;					\n"
"layout(location = 1) in vec2 i_texcoords;					\n"
"										\n"
"out vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	gl_Position = u_projection * vec4 (i_position, 1.0);			\n"
"	p_texcoords = i_texcoords;						\n"
"}										\n";

static const char *fb_fs_source =
"#version 140									\n"
"										\n"
"uniform sampler2D u_front;							\n"
"uniform sampler2D u_back;							\n"
"uniform sampler2D u_layer1;							\n"
"uniform sampler2D u_layer2;							\n"
"uniform float u_factor = 0.0;							\n"
"uniform float u_mult_f = 1.0;							\n"
"uniform float u_mult_b = 1.0;							\n"
"uniform float u_mult_1 = 1.0;							\n"
"uniform float u_mult_2 = 1.0;							\n"
"uniform int u_invert_f = 0;							\n"
"uniform int u_invert_b = 0;							\n"
"										\n"
"in vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	vec2 p_texcoords_inv = vec2 (p_texcoords.s, 1 - p_texcoords.t);		\n"
"	vec4 texel_f, texel_b;							\n"
"										\n"
"	if (u_invert_f == 0)							\n"
"		texel_f = texture (u_front, p_texcoords);			\n"
"	else									\n"
"		texel_f = texture (u_front, p_texcoords_inv);			\n"
"	texel_f = texel_f * u_mult_f;						\n"
"										\n"
"	if (u_invert_b == 0)							\n"
"		texel_b = texture (u_back,  p_texcoords);			\n"
"	else									\n"
"		texel_b = texture (u_back,  p_texcoords_inv);			\n"
"	texel_b = texel_b * u_mult_b;						\n"
"										\n"
"	vec4 texel_1 = texture (u_layer1, p_texcoords_inv) * u_mult_1;		\n"
"	vec4 texel_2 = texture (u_layer2, p_texcoords_inv) * u_mult_2;		\n"
"										\n"
"	gl_FragColor = mix (texel_f, texel_b, u_factor) + texel_1 + texel_2;	\n"
"}\n";

#define NUM_FRAMEBUFFERS 2

#define OFFSET(member_) \
	((const GLvoid *) offsetof (typeof (fb_vbo_data[0]), member_))

static int
build_fb_state (hikaru_renderer_t *hr)
{
	static const GLenum targets[1] = { GL_COLOR_ATTACHMENT0 };
	unsigned i;

	/* Compile the framebuffer GLSL program and retrieve the uniforms. */
	hr->framebuffer.program =
		vk_renderer_compile_program (fb_vs_source, fb_fs_source);
	VK_ASSERT_NO_GL_ERROR ();

	hr->framebuffer.locs.u_front =
		get_uniform_loc (hr->framebuffer.program, "u_front");
	hr->framebuffer.locs.u_back =
		get_uniform_loc (hr->framebuffer.program, "u_back");
	hr->framebuffer.locs.u_layer1 =
		get_uniform_loc (hr->framebuffer.program, "u_layer1");
	hr->framebuffer.locs.u_layer2 =
		get_uniform_loc (hr->framebuffer.program, "u_layer2");
	hr->framebuffer.locs.u_factor =
		get_uniform_loc (hr->framebuffer.program, "u_factor");
	hr->framebuffer.locs.u_mult_f =
		get_uniform_loc (hr->framebuffer.program, "u_mult_f");
	hr->framebuffer.locs.u_mult_b =
		get_uniform_loc (hr->framebuffer.program, "u_mult_b");
	hr->framebuffer.locs.u_mult_1 =
		get_uniform_loc (hr->framebuffer.program, "u_mult_1");
	hr->framebuffer.locs.u_mult_2 =
		get_uniform_loc (hr->framebuffer.program, "u_mult_2");
	hr->framebuffer.locs.u_invert_f =
		get_uniform_loc (hr->framebuffer.program, "u_invert_f");
	hr->framebuffer.locs.u_invert_b =
		get_uniform_loc (hr->framebuffer.program, "u_invert_b");

	/* Generate two framebuffers (the front and the back buffers), each
	 * associated with a colorbuffer (texture) and a depth buffer
	 * (renderbuffer). */
	glGenFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	glGenTextures (NUM_FRAMEBUFFERS, hr->framebuffer.cb);
	glGenRenderbuffers (NUM_FRAMEBUFFERS, hr->framebuffer.db);
	VK_ASSERT_NO_GL_ERROR ();

	for (i = 0; i < NUM_FRAMEBUFFERS; i++) {

		/* Bind the i'th framebuffer */
		glBindFramebuffer (GL_FRAMEBUFFER, hr->framebuffer.fb[i]);
		VK_ASSERT_NO_GL_ERROR ();

		/* Create and attach a texture as color buffer. */
		glBindTexture (GL_TEXTURE_2D, hr->framebuffer.cb[i]);
		VK_ASSERT_NO_GL_ERROR ();
		glTexStorage2D (GL_TEXTURE_2D, 1, GL_RGBA8, 640, 480);
		VK_ASSERT_NO_GL_ERROR ();
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, 640, 480,
		                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		VK_ASSERT_NO_GL_ERROR ();
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		VK_ASSERT_NO_GL_ERROR ();
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		VK_ASSERT_NO_GL_ERROR ();

		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, hr->framebuffer.cb[i], 0);
		VK_ASSERT_NO_GL_ERROR ();

		glDrawBuffers (1, targets);
		VK_ASSERT_NO_GL_ERROR ();

		/* Create and attach a renderbuffer as depth buffer. */
		glBindRenderbuffer (GL_RENDERBUFFER, hr->framebuffer.db[i]);
		glRenderbufferStorage (GL_RENDERBUFFER, GL_DEPTH_COMPONENT, 640, 480);
		VK_ASSERT_NO_GL_ERROR ();

		glFramebufferRenderbuffer (GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                           GL_RENDERBUFFER, hr->framebuffer.db[i]);
		VK_ASSERT_NO_GL_ERROR ();

		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return -1;
	}

	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	VK_ASSERT_NO_GL_ERROR ();

	/* Create the VAO/VBO. */
	glGenVertexArrays (1, &hr->framebuffer.vao);
	glBindVertexArray (hr->framebuffer.vao);
	VK_ASSERT_NO_GL_ERROR ();

	glGenBuffers (1, &hr->framebuffer.vbo);
	glBindBuffer (GL_ARRAY_BUFFER, hr->framebuffer.vbo);
	glBufferData (GL_ARRAY_BUFFER,
	              sizeof (fb_vbo_data), fb_vbo_data, GL_STATIC_DRAW);
	VK_ASSERT_NO_GL_ERROR ();

	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE,
	                       sizeof (fb_vbo_data[0]),
	                       OFFSET (position));
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
	                       sizeof (fb_vbo_data[0]),
	                       OFFSET (texcoords));
	VK_ASSERT_NO_GL_ERROR ();

	glEnableVertexAttribArray (0);
	glEnableVertexAttribArray (1);
	VK_ASSERT_NO_GL_ERROR ();

	glBindVertexArray (0);
	VK_ASSERT_NO_GL_ERROR ();

	return 0;
}

#undef OFFSET

static void
destroy_fb_state (hikaru_renderer_t *hr)
{
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	glDeleteFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	glDeleteTextures (NUM_FRAMEBUFFERS, hr->framebuffer.cb);
	glDeleteRenderbuffers (NUM_FRAMEBUFFERS, hr->framebuffer.db);
	VK_ASSERT_NO_GL_ERROR ();

	vk_renderer_destroy_program (hr->framebuffer.program);
}

static void
upload_layer (hikaru_renderer_t *hr, hikaru_layer_t *layer, GLuint *id, float *mult)
{
	void *data;

	LOG ("uploading LAYER %s", get_layer_str (layer));

	data = vk_buffer_get_ptr (hr->gpu->fb,
	                          layer->y0 * 4096 + layer->x0 * 4);

	/* Upload the layer data to a new texture. */
	glGenTextures (1, id);
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, *id);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	VK_ASSERT_NO_GL_ERROR ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	VK_ASSERT_NO_GL_ERROR ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	VK_ASSERT_NO_GL_ERROR ();

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	switch (layer->format) {
	case HIKARU_FORMAT_ABGR1555:
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);

		glTexImage2D (GL_TEXTURE_2D, 0,
		              GL_RGB5_A1,
		              640, 480, 0,
		              GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
		              data);

		*mult = 1.0f;
		break;
	case HIKARU_FORMAT_A2BGR10:
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 1024);

		glTexImage2D (GL_TEXTURE_2D, 0,
		              GL_RGB10_A2,
		              640, 480, 0,
		              GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
		              data);

		*mult = 4.0f;
		break;
	default:
		VK_ASSERT (0);
	}
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	VK_ASSERT_NO_GL_ERROR ();
}

/* TODO make use of the framebuffer configuration in the GPU:1A registers,
 * right now it only uses the information in the 781/181 commands. */
static void
draw (hikaru_renderer_t *hr)
{
	hikaru_gpu_t *gpu = hr->gpu;
	GLuint n1, n2, f, b, layer1 = 0, layer2 = 0;
	float factor, mult1 = 0.0f, mult2 = 0.0f;

	/* Figure out the GPU framebuffer assignments */
	VK_PRINT ("FRAMEBUFFER 781=%08X 181=%08X", gpu->fb_config._781, gpu->fb_config._181);

	n1 = gpu->fb_config.num_1;
	f = (n1 < 2) ? hr->framebuffer.cb[n1] : (n1 == 2) ? layer1 : layer2;

	n2 = gpu->fb_config.num_2;
	b = (n2 < 2) ? hr->framebuffer.cb[n2] : (n2 == 2) ? layer1 : layer2;

	factor = 0.0f;
	if (gpu->fb_config.blend)
		factor = gpu->fb_config.alpha * (1.0f / 255);


	/* We only care about unit 0 for now. (Unit 1 is probably only used
	 * for the dual-monitor case.) */
	if ((LAYERS.layer[0][0].enabled || n1 == 2 || n2 == 2) &&
	    !hr->debug.flags[HR_DEBUG_NO_LAYER1])
		upload_layer (hr, &LAYERS.layer[0][0], &layer1, &mult1);

	if ((LAYERS.layer[0][1].enabled || n1 == 3 || n2 == 3) &&
	    !hr->debug.flags[HR_DEBUG_NO_LAYER2])
		upload_layer (hr, &LAYERS.layer[0][1], &layer2, &mult2);


	/* Draw the 3D scene to the front buffer. */
	glBindFramebuffer (GL_FRAMEBUFFER, f);
	VK_ASSERT_NO_GL_ERROR ();

	draw_scene (hr);
	VK_ASSERT_NO_GL_ERROR ();

	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	VK_ASSERT_NO_GL_ERROR ();


	/* Setup the GL state for 2D drawing. */
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	glDisable (GL_POLYGON_OFFSET_FILL);
	VK_ASSERT_NO_GL_ERROR ();


	/* Composite all the buffers (front, back, layers). */
	glUseProgram (hr->framebuffer.program);
	VK_ASSERT_NO_GL_ERROR ();

	/* Figure out the GPU framebuffer assignments. */
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, f);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 1);
	glBindTexture (GL_TEXTURE_2D, b);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 2);
	glBindTexture (GL_TEXTURE_2D, (f != layer1 && b != layer1) ? layer1 : 0);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 3);
	glBindTexture (GL_TEXTURE_2D, (f != layer2 && b != layer2) ? layer2 : 0);
	VK_ASSERT_NO_GL_ERROR ();

	glUniform1i (hr->framebuffer.locs.u_front,  0);
	glUniform1i (hr->framebuffer.locs.u_back,   1);
	glUniform1i (hr->framebuffer.locs.u_layer1, 2);
	glUniform1i (hr->framebuffer.locs.u_layer2, 3);
	glUniform1f (hr->framebuffer.locs.u_factor, factor);
	if (f == layer1 && mult1 != 1.0)
		glUniform1f (hr->framebuffer.locs.u_mult_f, mult1);
	else if (f == layer2 && mult2 != 1.0)
		glUniform1f (hr->framebuffer.locs.u_mult_f, mult2);
	if (b == layer1 && mult1 != 1.0)
		glUniform1f (hr->framebuffer.locs.u_mult_b, mult1);
	else if (b == layer2 && mult2 != 1.0)
		glUniform1f (hr->framebuffer.locs.u_mult_b, mult2);
	if (mult1 != 1.0)
		glUniform1f (hr->framebuffer.locs.u_mult_1, mult1);
	if (mult2 != 1.0)
		glUniform1f (hr->framebuffer.locs.u_mult_2, mult2);
	if (n1 >= 2)
		glUniform1i (hr->framebuffer.locs.u_invert_f, 1);
	if (n2 >= 2)
		glUniform1i (hr->framebuffer.locs.u_invert_b, 1);
	VK_ASSERT_NO_GL_ERROR ();

	glBindVertexArray (hr->framebuffer.vao);
	VK_ASSERT_NO_GL_ERROR ();

	glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	VK_ASSERT_NO_GL_ERROR ();

	glBindVertexArray (0);
	glUseProgram (0);
	VK_ASSERT_NO_GL_ERROR ();


	/* Get rid of the layer texture. */
	if (layer1)
		glDeleteTextures (1, &layer1);
	if (layer2)
		glDeleteTextures (1, &layer2);
	VK_ASSERT_NO_GL_ERROR ();
}

/****************************************************************************
 Interface
****************************************************************************/

static void
hikaru_renderer_begin_frame (vk_renderer_t *renderer)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;
	unsigned vpi, i;

	hr->meshes.variant.full = ~0;

	hr->num_vps = 0;
	hr->num_mvs = 0;
	hr->num_instances = 0;
	hr->num_mats = 0;
	hr->num_texs = 0;
	hr->num_lss = 0;

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			hr->num_meshes[vpi][i] = 0;
	hr->total_meshes = 0;

	update_debug_flags (hr);

	VK_ASSERT_NO_GL_ERROR ();
}

static void
hikaru_renderer_end_frame (vk_renderer_t *renderer)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;

	VK_ASSERT_NO_GL_ERROR ();

	draw (hr);
	VK_ASSERT_NO_GL_ERROR ();

	LOG (" ==== RENDSTATE STATISTICS ==== ");
	LOG ("  vp  : %u", hr->num_vps);
	LOG ("  mv  : %u", hr->num_mvs);
	LOG ("  mat : %u", hr->num_mats);
	LOG ("  tex : %u", hr->num_texs);
	LOG ("  ls  : %u", hr->num_lss);
}

static void
hikaru_renderer_reset (vk_renderer_t *renderer)
{
	hikaru_renderer_invalidate_texcache (renderer, NULL);
}

static void
hikaru_renderer_destroy (vk_renderer_t **renderer_)
{
	if (renderer_) {
		hikaru_renderer_t *hr = (hikaru_renderer_t *) *renderer_;

		destroy_3d_state (hr);
		destroy_fb_state (hr);

		hikaru_renderer_invalidate_texcache (*renderer_, NULL);
	}
}

vk_renderer_t *
hikaru_renderer_new (vk_buffer_t *fb, vk_buffer_t *texram[2])
{
	hikaru_renderer_t *hr;
	int ret;

	hr = ALLOC (hikaru_renderer_t);
	if (!hr)
		goto fail;

	hr->base.width = 640;
	hr->base.height = 480;

	hr->base.destroy = hikaru_renderer_destroy;
	hr->base.reset = hikaru_renderer_reset;
	hr->base.begin_frame = hikaru_renderer_begin_frame;
	hr->base.end_frame = hikaru_renderer_end_frame;

	ret = vk_renderer_init ((vk_renderer_t *) hr);
	if (ret)
		goto fail;

	VK_ASSERT_NO_GL_ERROR ();

	memset ((void *) program_cache, 0, sizeof (program_cache));
	num_programs = 0;

	init_debug_flags (hr);

	if (build_3d_state (hr))
		goto fail;
	VK_ASSERT_NO_GL_ERROR ();

	if (build_fb_state (hr))
		goto fail;
	VK_ASSERT_NO_GL_ERROR ();

	return (vk_renderer_t *) hr;

fail:
	hikaru_renderer_destroy ((vk_renderer_t **) &hr);
	return NULL;
}

void
hikaru_renderer_set_gpu (vk_renderer_t *renderer, void *gpu_as_void)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) gpu_as_void;

	hr->gpu = gpu;
}
