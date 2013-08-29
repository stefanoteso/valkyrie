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

#include "mach/hikaru/hikaru-gpu.h"
#include "mach/hikaru/hikaru-gpu-private.h"

/*
 * GPU CP
 * ======
 *
 * The GPU CP is the Command Processor that performs 3D drawing.
 *
 *
 * CP Execution
 * ============
 *
 * CP execution is likely initiated by writing 15000058=3 and 1A000024=1, but
 * may actually begin only on the next vblank-in event.
 *
 * (From a software perspective, the CP program is uploaded when both IRQs 2 of
 * GPU 15 and GPU 1A are fired, meaning that at this point the CP is supposed
 * to have consumed the previous command stream and is ready to accept a new
 * one.)
 *
 * Note also that a CP program is uploaded to two different areas in CMDRAM on
 * odd and even frames. This may mean that there are *two* CPs performing
 * double-buffered 3D rendering.
 *
 * When execution ends:
 *
 *  - 1A00000C bit 0 is set (not sure)
 *    - 1A000018 bit 1 is set as a consequence
 *      - 15000088 bit 7 is set as a consequence
 *  - 15000088 bit 1 is set; a GPU IRQ is raised (if not masked by 15000084)
 *  - 15002000 bit 0 is set on some HW revisions (not sure)
 *  - 1A000024 bit 0 is cleared if some additional condition occurred (not sure)
 *
 * 15002000 and 1A000024 signal different things; see the termination condition
 * in sync().
 *
 * (Guesstimate: 15000088 bit 1 is set when the CP ends verifying/processing
 * the CS (if there is such a thing!); 15002000 is cleared when the CP submits
 * the completely rasterized frame buffer to the GPU 1A; 1A000024 is cleared
 * when the GPU 1A is done compositing the frame buffer with the 2D layers and
 * has displayed them on-screen.)
 */

static void
on_cp_begin (hikaru_gpu_t *gpu)
{
	gpu->cp.is_running = true;

	gpu->cp.pc = REG15 (0x70);
	gpu->cp.sp[0] = REG15 (0x74);
	gpu->cp.sp[1] = REG15 (0x78);

	gpu->in_mesh = false;
	gpu->static_mesh_precision = -1.0f;

	gpu->materials.base = 0;
	gpu->texheads.base  = 0;
	gpu->lights.base    = 0;
}

static void
on_cp_end (hikaru_gpu_t *gpu)
{
	/* Turn off the busy bits */
	REG15 (0x58) &= ~3;
	REG1A (0x24) &= ~1;

	/* Notify that GPU 15 is done and needs feeding */
	hikaru_gpu_raise_irq (gpu, _15_IRQ_DONE, _1A_IRQ_DONE);
}

void
hikaru_gpu_cp_vblank_in (hikaru_gpu_t *gpu)
{
	/* no-op */
}

void
hikaru_gpu_cp_vblank_out (hikaru_gpu_t *gpu)
{
	/* no-op */
}

void
hikaru_gpu_cp_on_put (hikaru_gpu_t *gpu)
{
	/* Check the GPU 15 execute bits */
	if (REG15 (0x58) == 3)
		on_cp_begin (gpu);
	else
		REG1A (0x24) = 0; /* XXX really? */
}

/*
 * CP Objects
 * ==========
 *
 * The CP manipulates six kinds of objects: viewports, modelviews, materials,
 * textures/texheads (that's what they are called in PHARRIER, and here),
 * lights/lightsets (a lightset is a set of four lights), and meshes.
 *
 * The CP has a table for each object type, except meshes, namely:
 *
 *  viewports	8 [confirmed by system16 specs]
 *  modelviews  < 256 [
 *  materials	?unknown?
 *  texheads	?unknown?
 *  lights	1024
 *  lightsets	200 or 256
 *
 * CP instructions do not manipulate the objects in the tables directly.
 * Instead, they work on a special object, the "scratch" or "active" object:
 * "recall" instructions load an object from the object table into the scratch
 * object, "set" instructions set the properties of the scratch objects, and
 * "commit" instructions store the scratch object back into the table.
 *
 * When a mesh is drawn, the current scratch objects affect its rendering,
 * e.g., the scratch material determines the mesh colors, shininess, etc.
 *
 *
 * CP Instructions
 * ===============
 *
 * Each GPU instruction is made of 1, 2, 4, or 8 32-bit words. The opcode is
 * specified by the lower 9 bits of the first word.
 *
 * In general, opcodes of the form:
 *
 *  xx1 Set properties of the current object.
 *  xx2 Do control-flow.
 *  xx3 Recall the current object or set an offset into the object table.
 *  xx4 Commit the current object.
 *  xx6 ?
 *  xx8 ?
 *
 * There are of course some variations on this pattern.
 */

#define PC        gpu->cp.pc
#define UNHANDLED gpu->cp.unhandled
#define HR        ((hikaru_renderer_t *) gpu->renderer)

#define FLAG_JUMP	(1 << 0)
#define FLAG_BEGIN	(1 << 1)
#define FLAG_CONTINUE	(1 << 2)
#define FLAG_PUSH	(FLAG_BEGIN | FLAG_CONTINUE)
#define FLAG_STATIC	(1 << 3)
#define FLAG_INVALID	(1 << 4)

static struct {
	void (* handler)(hikaru_gpu_t *, uint32_t *);
	uint32_t flags;
} insns[0x200];

void (* disasm[0x200])(hikaru_gpu_t *, uint32_t *);

static uint32_t
get_insn_size (uint32_t *inst)
{
	return 1 << (((inst[0] >> 4) & 3) + 2);
}

static void
print_disasm (hikaru_gpu_t *gpu, uint32_t *inst, const char *fmt, ...)
{
	char out[256], *tmp = out;
	uint32_t nwords;
	va_list args;
	unsigned i;

	nwords = get_insn_size (inst) / 4;

	VK_ASSERT (gpu);
	VK_ASSERT (inst);
	VK_ASSERT (nwords <= 8);

	if (!gpu->options.log_cp)
		return;

	va_start (args, fmt);
	tmp += sprintf (tmp, "CP @%08X : ", PC);
	for (i = 0; i < 8; i++) {
		if (i < nwords)
			tmp += sprintf (tmp, "%08X ", inst[i]);
		else
			tmp += sprintf (tmp, "........ ");
	}
	tmp += sprintf (tmp, "%c %c ",
	                UNHANDLED ? '!' : ' ',
	                gpu->in_mesh ? 'M' : ' ');
	vsnprintf (tmp, sizeof (out), fmt, args);
	va_end (args);

	VK_LOG ("%s", out);
}

#define DISASM(fmt_, args_...) \
	print_disasm (gpu, inst, fmt_, ##args_)

static void
check_self_loop (hikaru_gpu_t *gpu, uint32_t target)
{
	/* XXX at some point we'll need something better than this */
	if (target == PC) {
		VK_ERROR ("CP: @%08X: self-jump, terminating", target);
		gpu->cp.is_running = false;
	}
}

static void
push_pc (hikaru_gpu_t *gpu)
{
	unsigned i = gpu->frame_type;
	VK_ASSERT ((gpu->cp.sp[i] >> 24) == 0x48);
	vk_buffer_put (gpu->cmdram, 4, gpu->cp.sp[i] & 0x3FFFFFF, PC);
	gpu->cp.sp[i] -= 4;
}

static void
pop_pc (hikaru_gpu_t *gpu)
{
	unsigned i = gpu->frame_type;
	gpu->cp.sp[i] += 4;
	VK_ASSERT ((gpu->cp.sp[i] >> 24) == 0x48);
	PC = vk_buffer_get (gpu->cmdram, 4, gpu->cp.sp[i] & 0x3FFFFFF) + 8;
}

static int
fetch (hikaru_gpu_t *gpu, uint32_t **inst)
{
	hikaru_t *hikaru = (hikaru_t *) gpu->base.mach;

	/* The CP program has been observed to lie only in CMDRAM and slave
	 * RAM so far. */
	switch (PC >> 24) {
	case 0x40:
	case 0x41:
		*inst = (uint32_t *) vk_buffer_get_ptr (hikaru->ram_s,
		                                        PC & 0x01FFFFFF);
		return 0;
	case 0x48:
	case 0x4C: /* XXX not sure */
		*inst = (uint32_t *) vk_buffer_get_ptr (hikaru->cmdram,
		                                        PC & 0x003FFFFF);
		return 0;
	}
	return -1;
}

void
hikaru_gpu_cp_exec (hikaru_gpu_t *gpu, int cycles)
{
	if (!gpu->cp.is_running)
		return;

	while (cycles > 0 && gpu->cp.is_running) {
		uint32_t *inst, op;
		uint16_t flags;

		if (fetch (gpu, &inst)) {
			VK_ERROR ("CP %08X: invalid PC, skipping CS", PC);
			gpu->cp.is_running = false;
			break;
		}

		op = inst[0] & 0x1FF;

		flags = insns[op].flags;
		if (flags & FLAG_INVALID) {
			VK_LOG ("CP @%08X: invalid instruction [%08X]", PC, *inst);
			gpu->cp.is_running = false;
			break;
		}

		if (!gpu->in_mesh && (flags & FLAG_BEGIN)) {
			bool is_static = (flags & FLAG_STATIC) != 0;
			hikaru_renderer_begin_mesh (HR, PC, is_static);
			gpu->in_mesh = true;
		} else if (gpu->in_mesh && !(flags & FLAG_CONTINUE)) {
			hikaru_renderer_end_mesh (HR, PC);
			gpu->in_mesh = false;
		}

		if (gpu->options.log_cp) {
			gpu->cp.unhandled = false;
			disasm[op] (gpu, inst);
			if (gpu->cp.unhandled)
				VK_ERROR ("CP @%08X : unhandled instruction", PC);
		}

		insns[op].handler (gpu, inst);

		if (!(flags & FLAG_JUMP))
			PC += get_insn_size (inst);

		cycles--;
	}

	if (!gpu->cp.is_running)
		on_cp_end (gpu);
}

#define I(name_) \
	static void hikaru_gpu_inst_##name_ (hikaru_gpu_t *gpu, uint32_t *inst)

#define D(name_) \
	static void hikaru_gpu_disasm_##name_ (hikaru_gpu_t *gpu, uint32_t *inst)

/*
 * Control Flow
 * ============
 *
 * The CP supports jumps and subroutine calls, including conditional calls
 * (for selecting the right mesh LOD probably?)
 *
 * The call stack is probably held in CMDRAM at the addresses specified by
 * MMIOs 1500007{4,8}.
 */

static uint32_t
get_jump_address (hikaru_gpu_t *gpu, uint32_t *inst)
{
	uint32_t addr;

	addr = inst[1] * 4;
	if (inst[0] & 0x800)
		addr += PC;

	return addr;
}

static float
get_distance_to_current_mesh (hikaru_gpu_t *gpu)
{
	hikaru_gpu_modelview_t *mv = &gpu->modelviews.table[gpu->modelviews.depth];
	float xx, yy, zz;

	/* Compute the distance between the camera (0,0,0) and the
	 * to-be-drawn object origin, i.e. the origin transformed
	 * by the current modelview matrix. */

	xx = mv->mtx[3][0] * mv->mtx[3][0];
	yy = mv->mtx[3][1] * mv->mtx[3][1];
	zz = mv->mtx[3][2] * mv->mtx[3][2];

	return sqrtf (xx + yy + zz);
}

/* 000	Nop
 *
 *	-------- -------- -------o oooooooo
 */

I (0x000)
{
}

D (0x000)
{
	DISASM ("nop");

	UNHANDLED |= (inst[0] != 0);
}

/* 012	Jump
 *
 *	IIIIIIII IIIIIIII UUUUR--o oooooooo
 *	AAAAAAAA AAAAAAAA AAAAAAAA AAAAAAAA
 *
 * I = Branch identifier?
 * U = Unknown
 *
 *	See instruction 095.
 *
 * R = Relative
 * A = Address or Offset in 32-bit words.
 */

I (0x012)
{
	uint32_t addr = get_jump_address (gpu, inst);

	check_self_loop (gpu, addr);
	if (!(inst[0] & 0xF000))
		PC = addr;
	else
		PC += 8;
}

D (0x012)
{
	uint32_t addr = get_jump_address (gpu, inst);

	UNHANDLED |= !!(inst[0] & 0x00000600);

	if (!(inst[0] & 0xF000))
		DISASM ("jump @%08X", addr);
	else
		DISASM ("cond jump @%08X [%X %04X]",
		        addr, (inst[0] >> 12) & 0xF, inst[0] >> 16);
}

/* 052	Call
 *
 *	-------- -------- x---R--o oooooooo
 *	AAAAAAAA AAAAAAAA AAAAAAAA AAAAAAAA
 *
 * C = Conditional. Maybe if-true?
 * R = Relative
 * A = Address or Offset in 32-bit words.
 *
 * Used in conjunction with command 005. See SGNASCAR.
 */

I (0x052)
{
	uint32_t addr = get_jump_address (gpu, inst);

	check_self_loop (gpu, addr);
	push_pc (gpu);
	PC = addr;
}

D (0x052)
{
	uint32_t addr = get_jump_address (gpu, inst);

	UNHANDLED |= !!(inst[0] & 0xFFFF7600);

	if (!(inst[0] & 0xC000))
		DISASM ("call @%08X", addr);
	else
		DISASM ("cond call @%08X", addr);
}

/* 082	Return
 *
 *	-------- -------- -C-----o oooooooo
 *
 * C = Conditional. Maybe if-false?
 *
 * Used in conjunction with command 005. See SGNASCAR.
 */

I (0x082)
{
	if (!(inst[0] & 0xC000))
		pop_pc (gpu);
	else
		PC += 4;
}

D (0x082)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFBE00);

	if (!(inst[0] & 0xC000))
		DISASM ("ret");
	else
		DISASM ("cond ret");
}

/* 1C2	Kill
 *
 *	-------- -------- -------o oooooooo
 */

I (0x1C2)
{
	gpu->cp.is_running = false;
}

D (0x1C2)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFFE00);

	DISASM ("kill");
}

/* 005	Set Condition Threshold Lower-Bound
 *
 *	TTTTTTTT TTTTTTTT TTTT---o oooooooo
 *
 * T = Truncated floating-point threshold.
 *
 *	Since the threshold is always positive, truncating the lower 12 bits
 *	makes the resulting value smaller = a lower bound.
 *
 * Used in conjunction with conditional control-flow. See SGNASCAR.
 */

I (0x005)
{
}

D (0x005)
{
	uint32_t x = inst[0] & 0xFFFFF000;
	float f = *(float *) &x;

	UNHANDLED |= !!(inst[0] & 0x00000C00);

	DISASM ("set cond threshold lower bound [%f, current=%f]",
	        f, get_distance_to_current_mesh (gpu));
}

/* 055	Unk: Set Condition Threshold
 *
 *	-------- -------- -------o oooooooo
 *	TTTTTTTT TTTTTTTT TTTTTTTT TTTTTTTT
 *
 * T = Floating-point threshold
 *
 * Used in conjunction with conditional control-flow. See SGNASCAR.
 */

I (0x055)
{
}

D (0x055)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFFE00);

	DISASM ("set cond threshold [%f, current=%f]",
	        *(float *) &inst[1], get_distance_to_current_mesh (gpu));
}

/* 095	Unk: Set Branch IDs
 *
 *	-------- -------- tf-----o oooooooo
 *	FFFFFFFF FFFFFFFF TTTTTTTT TTTTTTTT
 *
 * F = ID of if-false branch?
 * T = ID of if-true branch?
 *
 *	Identify the jump (012) to take if the condition is true/false?
 *
 * Used in conjunction with conditional control-flow.  See SGNASCAR.
 *
 * Also note that T < F, always (or the other way around).
 */

I (0x095)
{
}

D (0x095)
{
	UNHANDLED |= !!(inst[0] & 0xFFFF3E00);

	DISASM ("set branch ids [%X:%x %X:%x]",
	        inst[1] & 0xFFFF, (inst[0] & 0x4000) ? 1 : 0,
	        inst[1] >> 16, (inst[0] & 0x8000) ? 1 : 0);
}

/*
 * Viewports
 * =========
 *
 * These specify an on-screen rectangle (a subregion of the framebuffer,
 * presumably), a projection matrix, the depth-buffer and depth queue
 * configuration, ambient lighting and clear color. The exact meaning of the
 * various fields are still partially unknown.
 */

static uint32_t
get_viewport_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_VIEWPORTS - 1);
}

static float
decode_clip_xy (uint32_t c)
{
	return (float) ((((int32_t) (int16_t) c) << 3) >> 3);
}

/* 021	Viewport: Set Z Clip
 *
 *	-------- -------- -----00o oooooooo
 *	FFFFFFFF FFFFFFFF FFFFFFFF FFFFFFFF
 *	ffffffff ffffffff ffffffff ffffffff
 *	nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn
 *
 * F = far depth clipping plane
 * f = far depth clipping plane (?)
 * n = near depth clipping plane
 *
 * Both f and F are computed as:
 *
 *  (height / 2) / tanf(some_angle / 2)
 *
 * which assuming some_angle is fovy, is the formula for computing the far
 * clipping planes. I have no idea why there are two identical entries tho.
 * Note however that in SGNASCAR F and f may be different!
 *
 * See PH:@0C01587C, PH:@0C0158A4, PH:@0C0158E8.
 *
 *
 * 221	Viewport: Set XY Clip
 *
 *	-------- -------- -----01o oooooooo
 *	jjjjjjjj jjjjjjjj cccccccc cccccccc
 *	--YYYYYY YYYYYYYY -XXXXXXX XXXXXXXX
 *	--yyyyyy yyyyyyyy -xxxxxxx xxxxxxxx
 *
 * c,j = center?
 * x,y = left, bottom clipping planes
 * X,Y = right, top clipping planes
 *
 * NOTE: according to the code, X can be at most 640, Y can be at most 512;
 * and at least one of x and y must be zero.
 *
 * See PH:@0C015924
 *
 *
 * 421	Viewport: Set Z Buffer Config
 *
 *	-------- -------- -----10o oooooooo
 *	nnnnnnnn nnnnnnnn nnnnnnnn nnnnnnnn
 *	ffffffff ffffffff ffffffff ffffffff
 *	FFF----- -------- -------- --------
 *
 * n = depth buffer minimum
 * f = depth buffer maximum
 * F = depth function
 *
 * See PH:@0C015AA6
 *
 *
 * 621	Viewport: Set Z Queue Config
 *
 *	-------- ----ttDu -----11o oooooooo
 *	AAAAAAAA BBBBBBBB GGGGGGGG RRRRRRRR
 *	dddddddd dddddddd dddddddd dddddddd 
 *	gggggggg gggggggg gggggggg gggggggg
 *
 * t = Type
 * D = Disable?
 * u = Unknown
 * RGBA = color/mask?
 *
 * f = queue density
 *
 *	Computed as 1.0f (constant), 1.0f / zdelta, or 1.0f / sqrt (zdelta**2);
 *	where zdelta = zmax - zmin.
 *
 * g = queue bias
 *
 *	Computed as depth_near / depth_far.
 *
 * See PH:@0C0159C4, PH:@0C015A02, PH:@0C015A3E.
 */

I (0x021)
{
	hikaru_gpu_viewport_t *vp = &gpu->viewports.scratch;

	switch ((inst[0] >> 8) & 7) {
	case 0:
		vp->clip.f = *(float *) &inst[1];
		vp->clip.f2 = *(float *) &inst[2];
		vp->clip.n = *(float *) &inst[3];
		break;
	case 2:
		vp->offset.x = (float) (inst[1] & 0xFFFF);
		vp->offset.y = (float) (inst[1] >> 16);
		vp->clip.l = decode_clip_xy (inst[2]);
		vp->clip.r = decode_clip_xy (inst[3]);
		vp->clip.b = decode_clip_xy (inst[2] >> 16);
		vp->clip.t = decode_clip_xy (inst[3] >> 16);
		break;
	case 4:
		vp->depth.min = *(float *) &inst[1];
		vp->depth.max = *(float *) &inst[2];
		vp->depth.func = inst[3] >> 29;
		break;
	case 6:
		vp->depth.q_type	= (inst[0] >> 18) & 3;
		vp->depth.q_enabled	= ((inst[0] >> 17) & 1) ^ 1;
		vp->depth.q_unknown	= (inst[0] >> 16) & 1;
		vp->depth.mask[0]	= inst[1] & 0xFF;
		vp->depth.mask[1]	= (inst[1] >> 8) & 0xFF;
		vp->depth.mask[2]	= (inst[1] >> 16) & 0xFF;
		vp->depth.mask[3]	= inst[1] >> 24;
		vp->depth.density	= *(float *) &inst[2];
		vp->depth.bias		= *(float *) &inst[3];
		break;
	default:
		VK_ASSERT (0);
		break;
	}

	vp->flags |= HIKARU_GPU_OBJ_DIRTY;
}

D (0x021)
{
	switch ((inst[0] >> 8) & 7) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);

		DISASM ("vp: set clip Z [f=%f f2=%f n=%f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;

	case 2:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("vp: set clip XY [clipxy=(%f %f %f %f) offs=(%f,%f)]",
		        decode_clip_xy (inst[2]), decode_clip_xy (inst[3]),
		        decode_clip_xy (inst[2] >> 16), decode_clip_xy (inst[3] >> 16),
		        (float) (inst[1] & 0xFFFF), (float) (inst[1] >> 16));
		break;

	case 4:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
		UNHANDLED |= !!(inst[3] & 0x1FFFFFFF);

		DISASM ("vp: set depth [func=%u range=(%f,%f)]",
		        inst[3] >> 29, *(float *) &inst[1], *(float *) &inst[2]);
		break;

	case 6:
		UNHANDLED |= !!(inst[0] & 0xFFF0F800);
	
		DISASM ("vp: set depth queue [type=%u ena=%u unk=%u mask=(%08X) density=%f bias=%f]",
		        (inst[0] >> 18) & 3, ((inst[0] >> 17) & 1) ^ 1,
		        (inst[0] >> 16) & 1, inst[1],
		        *(float *) &inst[2], *(float *) &inst[3]);
		break;
	}
}

/* 011	Viewport: Set Ambient Color
 *
 *	rrrrrrrr rrrrrrrr ----1--o oooooooo
 *	bbbbbbbb bbbbbbbb gggggggg gggggggg
 *
 * r,g,b = color
 *
 * See PH:@0C037840.
 */

I (0x011)
{
	hikaru_gpu_viewport_t *vp = &gpu->viewports.scratch;

	vp->color.ambient[0] = inst[0] >> 16;
	vp->color.ambient[1] = inst[1] & 0xFFFF;
	vp->color.ambient[2] = inst[1] >> 16;

	vp->flags |= HIKARU_GPU_OBJ_DIRTY;
}

D (0x011)
{
	UNHANDLED |= !!(inst[0] & 0x0000F600);
	UNHANDLED |= !(inst[0] & 0x00000800);

	DISASM ("vp: set ambient [%X %X %X]",
	        inst[0] >> 16, inst[1] & 0xFFFF, inst[1] >> 16);
}

/* 191	Viewport: Set Clear Color
 *
 *	-------- -------- ----1--o oooooooo
 *	-------a gggggggg bbbbbbbb rrrrrrrr
 *
 * a,r,g,b = color.
 *
 * NOTE: yes, apparently blue and green _are_ swapped.
 *
 * XXX double check the alpha mask.
 *
 * See PH:@0C016368, PH:@0C016396, PH:@0C037760.
 */

I (0x191)
{
	hikaru_gpu_viewport_t *vp = &gpu->viewports.scratch;

	vp->color.clear[0] = inst[1] & 0xFF;
	vp->color.clear[1] = (inst[1] >> 8) & 0xFF;
	vp->color.clear[2] = (inst[1] >> 16) & 0xFF;
	vp->color.clear[3] = ((inst[1] >> 24) & 1) ? 0xFF : 0;

	vp->flags |= HIKARU_GPU_OBJ_DIRTY;
}

D (0x191)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFF600);
	UNHANDLED |= !(inst[0] & 0x00000800);
	UNHANDLED |= !!(inst[0] & 0xFE000000);

	DISASM ("vp: set clear [%X]", inst[1]);
}

/* 004	Commit Viewport
 *
 *	-------- -----iii -------o oooooooo
 *
 * i = Index
 *
 * See PH:@0C015AD0.
 */

I (0x004)
{
	hikaru_gpu_viewport_t *vp = &gpu->viewports.table[get_viewport_index (inst)];

	*vp = gpu->viewports.scratch;

	vp->flags = HIKARU_GPU_OBJ_SET | HIKARU_GPU_OBJ_DIRTY;
}

D (0x004)
{
	UNHANDLED |= !!(inst[0] & 0xFFF8FE00);

	DISASM ("vp: commit @%u", get_viewport_index (inst));
}

/* 003	Recall Viewport
 *
 *	-------- -----iii -UU----o oooooooo
 *
 * i = Index
 * U = Unknown (2003 and 4003 variants are used in BRAVEFF title screen)
 *
 * See PH:@0C015AF6, PH:@0C015B12, PH:@0C015B32.
 */

I (0x003)
{
	hikaru_gpu_viewport_t *vp = &gpu->viewports.scratch;

	*vp = gpu->viewports.table[get_viewport_index (inst)];
}

D (0x003)
{
	UNHANDLED |= !!(inst[0] & 0xFFF89E00);

	DISASM ("vp: recall @%u", get_viewport_index (inst));
}

/*
 * Modelview Matrix
 * ================
 *
 * The CP uses command 161 to upload each (row) vector of the modelview matrix
 * separately.  The CP can also perform instanced drawing, and instructed to do
 * se through command 161.
 *
 * The other commands here set various vectors used for lighting (i.e. light
 * position and direction) but are not well understood.
 */

/* 161	Set Matrix Vector
 *
 *	-------- ----UPnn ----000o oooooooo
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *
 * U = Unknown (Multiply? Mutually exclusive with P)
 *
 * P = Push
 *
 *	The current modelview matrix is used for instancing. Matrices with
 *	P set are stored in a stack/table/list. The first mesh rendered
 *	afterwards is rendered n times, one for each matrix in the list.
 *
 *	Used in AIRTRIX attract mode.
 *
 * n = Element index
 * x,y,z = Elements
 *
 * This command sets a column vector of the current modelview matrix. Typically
 * four of these commands follow and set the whole 4x3 modelview matrix.
 *
 * Clearly the fourth row is fixed to (0, 0, 0, 1).
 *
 * See @0C008080.
 *
 *
 * 561	Set Vector
 *
 *	-------- ------nn ----010o oooooooo
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *
 * Used together with instructions 005, 055, 095 and conditional control-flow
 * in SGNASCAR.
 *
 *
 * 961	Set Light Vector 2
 *
 *	-------- -------e nnnn100o oooooooo
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *
 * e = Unknown
 * n = Unknown
 * x,y,z = position (XXX not necessarily)
 *
 * Variants include 16961, 10961, 8961. Apparently the 8961 variant makes use
 * of the 194 ramp data.
 *
 *
 * B61	Set Light Vector 3
 *
 *	-------- -------- Unnn110o oooooooo
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *
 * U = Unknown
 * n = Unknown
 * x,y,z = direction (XXX not quite)
 */

I (0x161)
{
	hikaru_gpu_modelview_t *mv = &gpu->modelviews.table[gpu->modelviews.depth];
	hikaru_gpu_light_t *lt = &gpu->lights.scratch;
	uint32_t push, elem;

	switch ((inst[0] >> 8) & 0xF) {

	case 1:
		push = (inst[0] >> 18) & 1;
		elem = (inst[0] >> 16) & 3;

		/* First element during upload. */
		if (elem == 3) {
			unsigned i, j;

			for (i = 0; i < 4; i++)
				for (j = 0; j < 4; j++)
					mv->mtx[i][j] = (i == j) ? 1.0f : 0.0;
		}

		/* We store the columns as rows to facilitate the
		 * translation to GL column-major convertion in the
		 * renderer. */
		mv->mtx[elem][0] = *(float *) &inst[1];
		mv->mtx[elem][1] = *(float *) &inst[2];
		mv->mtx[elem][2] = *(float *) &inst[3];
		mv->mtx[elem][3] = (elem == 3) ? 1.0f : 0.0f;

		/* Last element during upload. */
		if (elem == 0) {
			if (push) {
				gpu->modelviews.depth++;
				gpu->modelviews.total++;
				VK_ASSERT (gpu->modelviews.depth < NUM_MODELVIEWS);
			} else
				gpu->modelviews.depth = 0;
		}
		break;
	case 5:
		break;
	case 9:
		lt->vec9[0] = *(float *) &inst[1];
		lt->vec9[1] = *(float *) &inst[2];
		lt->vec9[2] = *(float *) &inst[3];
		break;
	case 0xB:
		lt->vecB[0] = *(float *) &inst[1];
		lt->vecB[1] = *(float *) &inst[2];
		lt->vecB[2] = *(float *) &inst[3];
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

D (0x161)
{
	uint32_t push, elem;

	switch ((inst[0] >> 8) & 0xF) {

	case 1:
		push = (inst[0] >> 18) & 1;
		elem = (inst[0] >> 16) & 3;

		UNHANDLED |= !!(inst[0] & 0xFFF0F000);

		DISASM ("mtx: set vector [%c %u (%f %f %f)]",
		        push ? 'P' : ' ', elem,
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	case 5:
		UNHANDLED |= !!(inst[0] & 0xFFFC0000);

		DISASM ("lit: set vector 5 [%f %f %f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	case 9:
		UNHANDLED |= !!(inst[0] & 0xFFFE0000);

		DISASM ("lit: set vector 9 [%f %f %f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	case 0xB:
		UNHANDLED |= !!(inst[0] & 0xFFFF0000);

		DISASM ("lit: set vector B [%f %f %f]",
		        *(float *) &inst[1], *(float *) &inst[2], *(float *) &inst[3]);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

/*
 * Materials
 * =========
 *
 * It supports flat, diffuse and phong shading. XXX more to come.
 */

static uint32_t
get_material_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_MATERIALS - 1);
}

/* 091	Material: Set Primary Color
 *
 *	-------- -------- -----00o oooooooo
 *	uuuuuuuu bbbbbbbb gggggggg rrrrrrrr
 *
 * u = Unknown, seen in BRAVEFF title screen.
 * r,g,b = RGB color
 *
 * See PH:@0C0CF742.
 *
 *
 * 291	Material: Set Secondary Color
 *
 *	-------- -------- -----01o oooooooo
 *	uuuuuuuu bbbbbbbb gggggggg rrrrrrrr
 *
 * r,g,b = RGB color
 *
 * See PH:@0C0CF742.
 *
 *
 * 491	Material: Set Shininess
 *
 *	-------- -------- -----10o oooooooo
 *	ssssssss bbbbbbbb gggggggg rrrrrrrr
 *
 * s = Specular shininess
 *
 * See PH:@0C0CF798, PH:@0C01782C.
 *
 *
 * 691	Material: Set Material Color
 *
 *	rrrrrrrr rrrrrrrr -----11o oooooooo
 *	bbbbbbbb bbbbbbbb gggggggg gggggggg
 *
 * See PH:@0C0CF7CC.
 *
 * NOTE: A91 and C91 are used in BRAVEFF title screen. They clearly alias A81
 * and C81.
 */

static void hikaru_gpu_inst_0x081 (hikaru_gpu_t *, uint32_t *);
static void hikaru_gpu_disasm_0x081 (hikaru_gpu_t *, uint32_t *);

I (0x091)
{
	hikaru_gpu_material_t *mat = &gpu->materials.scratch;
	uint32_t i;

	switch ((inst[0] >> 8) & 15) {
	case 0:
	case 2:
		i = (inst[0] >> 9) & 1;

		mat->color[i][0] = inst[1] & 0xFF;
		mat->color[i][1] = (inst[1] >> 8) & 0xFF;
		mat->color[i][2] = (inst[1] >> 16) & 0xFF;
		break;
	case 4:
		mat->shininess[0] = inst[1] & 0xFF;
		mat->shininess[1] = (inst[1] >> 8) & 0xFF;
		mat->shininess[2] = (inst[1] >> 16) & 0xFF;
		mat->specularity = inst[1] >> 24;
		break;
	case 6:
		mat->material_color[0] = inst[0] >> 16;
		mat->material_color[1] = inst[1] & 0xFFFF;
		mat->material_color[2] = inst[1] >> 16;
		break;
	case 0xA:
	case 0xC:
		hikaru_gpu_inst_0x081 (gpu, inst);
		/* XXX HACK */
		PC -= 4;
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

D (0x091)
{
	switch ((inst[0] >> 8) & 15) {
	case 0:
	case 2:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("mat: set color %u [%08X]",
		        (inst[0] >> 9) & 1, inst[1]);
		break;
	case 4:
		UNHANDLED |= !!(inst[0] & 0xFFFFF800);
	
		DISASM ("mat: set shininess [%X %X]",
		        inst[1] >> 24, inst[1] & 0xFFFFFF);
		break;

	case 6:
		UNHANDLED |= !!(inst[0] & 0x0000F800);
	
		DISASM ("mat: set material [%X %X %X]",
		        inst[0] >> 16, inst[1] & 0xFFFF, inst[1] >> 16);
		break;
	case 0xA:
	case 0xC:
		hikaru_gpu_disasm_0x081 (gpu, inst);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

/* 081	Material: Set Unknown
 *
 *	-------- ----mmmm ---n000o oooooooo
 *
 * n, m = Unknown
 *
 *
 * 881	Material: Set Flags
 *
 *	-------- --hatzSS ----ssso oooooooo
 *
 * S = Shading mode (flat, linear, phong)
 * z = Depth blend (fog)
 * t = Enable texture
 * a = Alpha mode
 * h = Highlight mode
 *
 * See PH:@0C0CF700.
 *
 *
 * A81	Material: Set Blending Mode
 *
 *	-------- ------mm ----ssso oooooooo
 *
 * m = Blending Mode
 *
 * See PH:@0C0CF7FA.
 *
 *
 * C81	Material: Set Unknown
 *
 *	-------- --U----- ----ssso oooooooo
 *
 * U = Unknown
 *
 * It may be related to command 154, see PH:@0C0CF872 and
 * PH:@0C0CF872.
 *
 * These can come in pairs, see e.g., AT:@0C0380AC.
 */

I (0x081)
{
	hikaru_gpu_material_t *mat = &gpu->materials.scratch;

	switch ((inst[0] >> 8) & 0xF) {
	case 0:
		DISASM ("mat: set unknown");

		UNHANDLED |= !!(inst[0] & 0xFFF0E000);
		break;

	case 8:
		mat->shading_mode	= (inst[0] >> 16) & 3;
		mat->depth_blend	= (inst[0] >> 18) & 1;
		mat->has_texture	= (inst[0] >> 19) & 1;
		mat->has_alpha		= (inst[0] >> 20) & 1;
		mat->has_highlight	= (inst[0] >> 21) & 1;

		DISASM ("mat: set flags [mode=%u zblend=%u tex=%u alpha=%u highl=%u",
			mat->shading_mode,
			mat->depth_blend,
			mat->has_texture,
			mat->has_alpha,
			mat->has_highlight);

		UNHANDLED |= !!(inst[0] & 0xFFC0F000);
		break;

	case 0xA:
		mat->blending_mode = (inst[0] >> 16) & 3;

		DISASM ("mat: set blending mode [mode=%u]", mat->blending_mode);

		UNHANDLED |= !!(inst[0] & 0xFFFCF000);
		break;

	case 0xC:
		DISASM ("mat: set unknown");

		UNHANDLED |= !!(inst[0] & 0xFFC0F000);
		break;
	}
}

D (0x081)
{
	switch ((inst[0] >> 8) & 0xF) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFFF0E000);

		DISASM ("mat: set unknown");
		break;
	case 8:
		UNHANDLED |= !!(inst[0] & 0xFFC0F000);

		DISASM ("mat: set flags [mode=%u zblend=%u tex=%u alpha=%u highl=%u",
		        (inst[0] >> 16) & 3,
		        (inst[0] >> 18) & 1,
		        (inst[0] >> 19) & 1,
		        (inst[0] >> 20) & 1,
		        (inst[0] >> 21) & 1);
		break;
	case 0xA:
		UNHANDLED |= !!(inst[0] & 0xFFFCF000);

		DISASM ("mat: set blending mode [mode=%u]", (inst[0] >> 16) & 3);
		break;
	case 0xC:
		UNHANDLED |= !!(inst[0] & 0xFFC0F000);

		DISASM ("mat: set unknown [%x %X]",
		        (inst[0] >> 21) & 1, (inst[0] >> 16) & 0x1F);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

/* 084	Commit Material
 *
 *	------nn nnnnnnnn ---1---o oooooooo
 *
 * n = Index
 *
 * See PH:@0C0153D4, PH:@0C0CF878.
 */

I (0x084)
{
	uint32_t index = gpu->materials.base + get_material_index (inst);

	if (index >= NUM_MATERIALS) {
		VK_ERROR ("CP: material commit index exceeds MAX (%u >= %u), skipping",
		          index, NUM_MATERIALS);
		return;
	}

	gpu->materials.table[index] = gpu->materials.scratch;
	gpu->materials.table[index].set = true;
}

D (0x084)
{
	UNHANDLED |= !!(inst[0] & 0xFC00E000);
	UNHANDLED |= !(inst[0] & 0x1000);

	DISASM ("mat: commit @base+%u", get_material_index (inst));
}

/* 083	Recall Material
 *
 *	---nnnnn nnnnnnnn ---A---o oooooooo
 *
 * n = Index
 * A = Active
 *
 * See @0C00657C, PH:@0C0CF882.
 */

I (0x083)
{
	uint32_t index = get_material_index (inst);

	if (!(inst[0] & 0x1000))
		gpu->materials.base = index;
	else {
		index += gpu->materials.base;
		if (index >= NUM_MATERIALS) {
			VK_ERROR ("CP: material recall index exceeds MAX (%u >= %u), skipping",
			          index, NUM_MATERIALS);
			return;
		}

		gpu->materials.scratch = gpu->materials.table[index];
	}
}

D (0x083)
{
	UNHANDLED |= !!(inst[0] & 0xC000E000);

	if (!(inst[0] & 0x1000))
		DISASM ("mat: set base %u", get_material_index (inst));
	else
		DISASM ("mat: recall @base+%u", get_material_index (inst));
}

/*
 * Texheads
 * ========
 *
 * Textures used for 3D rendering are stored (through the GPU IDMA) in the two
 * available TEXRAM banks.
 */

static uint32_t
get_texhead_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_TEXHEADS - 1);
}

/* 0C1	Texhead: Set Bias
 *
 *	----VVVV VVVV--MM -------o oooooooo
 *
 * V = Unknown value.
 * M = Unknown mode.
 *
 *	Mode 0 is used frequently with values 0 and FF.
 *
 *	Mode 1 is used with a variety of values.
 *
 *	Mode 2 is only used in BRAVEFF.
 *
 *
 * 2C1	Texhead: Set Format/Size
 *
 *	UUUFFFrr wwHHHWWW uu-----o oooooooo
 *
 * U = Unknown
 *
 * F = Format
 *
 * r = Repeat mode
 *
 *	0 = Normal repeat.
 *	1 = Mirrored repeat.
 *
 *	Bit 0 for V, bit 1 for U. Only meaningful if wrapping is enabled.
 *
 * w = Wrap mode
 *
 *	0 = Clamp.
 *	1 = Wrap.
 *
 *	Bit 0 for V, bit 1 for U.
 *
 * H = log16 of Height 
 * W = log16 of Width
 *
 * u = Unknown
 *
 * See PH:@0C015BCC.
 *
 *
 * 4C1	Texhead: Set Slot
 *
 *	nnnnnnnn mmmmmmmm ---b---o oooooooo
 *
 * n = Slot Y
 * m = Slot X
 * b = TEXRAM bank
 *
 * NOTE: for some reason the BOOTROM uploads a couple of 2C1/4C1 instructions
 * with their parameters swapped. This hasn't been seen in any game so far.
 *
 * See PH:@0C015BA0.
 */

I (0x0C1)
{
	hikaru_gpu_texhead_t *th = &gpu->texheads.scratch;

	switch ((inst[0] >> 8) & 7) {
	case 0:
		th->_0C1_mode	= (inst[0] >> 16) & 0xF;
		th->_0C1_value  = (inst[0] >> 20) & 0xFF;
		break;
	case 2:
		th->width	= 16 << ((inst[0] >> 16) & 7);
		th->height	= 16 << ((inst[0] >> 19) & 7);
		th->wrap_u	= (inst[0] >> 22) & 1;
		th->wrap_v	= (inst[0] >> 23) & 1;
		th->repeat_u	= (inst[0] >> 24) & 1;
		th->repeat_v	= (inst[0] >> 25) & 1;
		th->format	= (inst[0] >> 26) & 7;
		th->_2C1_unk	=  ((inst[0] >> 14) & 3) |
		                  (((inst[0] >> 29) & 7) << 2);

		/* XXX move this to the renderer */
		if (th->format == HIKARU_FORMAT_ABGR1111)
			th->width *= 2; /* pixels per word */
		break;
	case 4:
		th->bank  = (inst[0] >> 12) & 1;
		th->slotx = (inst[0] >> 16) & 0xFF;
		th->sloty = inst[0] >> 24;
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

D (0x0C1)
{
	switch ((inst[0] >> 8) & 7) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xF00CF800);

		DISASM ("tex: set bias [mode=%u %X]",
		        (inst[0] >> 16) & 0xF, (inst[0] >> 20) & 0xFF);
		break;

	case 2:
		UNHANDLED |= !!(inst[0] & 0x00003800);

		DISASM ("tex: set format [%ux%u fmt=%u wrap=(%u %u|%u %u) unk=%X]",
		        16 << ((inst[0] >> 16) & 7),
		        16 << ((inst[0] >> 19) & 7),
		        (inst[0] >> 26) & 7,
		        (inst[0] >> 22) & 1,
		        (inst[0] >> 23) & 1,
		        (inst[0] >> 24) & 1,
		        (inst[0] >> 25) & 1,
		        ((inst[0] >> 14) & 3) | (((inst[0] >> 29) & 7) << 2));
		break;
	case 4:
		DISASM ("tex: set slot [bank=%u (%X,%X)]",
		        (inst[0] >> 12) & 1,(inst[0] >> 16) & 0xFF, inst[0] >> 24);

		UNHANDLED |= !!(inst[0] & 0x0000E000);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

/* 0C4	Commit Texhead
 *
 *	------nn nnnnnnnn ---uoooo oooooooo
 *
 * n = Index
 * u = Unknown; always 1?
 *
 * See PH:@0C01545C. 
 */

I (0x0C4)
{
	uint32_t index = gpu->texheads.base + get_texhead_index (inst);

	if (index >= NUM_TEXHEADS) {
		VK_ERROR ("CP: texhead commit index exceedes MAX (%u >= %u), skipping",
		          index, NUM_MATERIALS);
		return;
	}

	gpu->texheads.table[index] = gpu->texheads.scratch;
	gpu->texheads.table[index].set = true;
}

D (0x0C4)
{
	UNHANDLED |= !!(inst[0] & 0xFC00E000);
	UNHANDLED |= ((inst[0] & 0x1000) != 0x1000);

	DISASM ("tex: commit @base+%u", get_texhead_index (inst));

}

/* 0C3	Recall Texhead
 *
 *	--nnnnnn nnnnnnnn ---M---o oooooooo
 *
 * n = Index
 * M = Modifier: 0 = set base only, 1 = recall for real
 *
 * XXX n here is likely too large.
 */

I (0x0C3)
{
	uint32_t index = get_texhead_index (inst);

	if (!(inst[0] & 0x1000))
		gpu->texheads.base = index;
	else {
		index += gpu->texheads.base;
		if (index >= NUM_TEXHEADS) {
			VK_ERROR ("CP: texhead recall index exceeds MAX (%u >= %u), skipping",
			          index, NUM_TEXHEADS);
			return;
		}

		gpu->texheads.scratch = gpu->texheads.table[index];
	}
}

D (0x0C3)
{
	UNHANDLED |= !!(inst[0] & 0xC000E000);

	if (!(inst[0] & 0x1000))
		DISASM ("tex: set base %u", get_texhead_index (inst));
	else
		DISASM ("tex: recall @base+%u", get_texhead_index (inst));
}

/*
 * Lights
 * ======
 *
 * According to the system16.com specs, the hardware supports 1024 lights per
 * scene, and 4 lights per polygon. It supports several light types (ambient,
 * spot, etc.) and several emission types (constant, infinite linear, square,
 * reciprocal, and reciprocal squared.)
 *
 * AFAICS, the hardware supports two light-related objects: lights and
 * lightsets. A light specifies the position, direction, emission properties
 * and more of a single light. A lightset specifies a set of (up to) four
 * lights that insist on the mesh being rendered. This setup is consistent with
 * the system16 specs.
 */

static uint32_t
get_light_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_LIGHTS - 1);
}

static uint32_t
get_lightset_index (uint32_t *inst)
{
	return (inst[0] >> 16) & (NUM_LIGHTSETS - 1);
}

/* 061	Set Light Type/Unknown
 *
 *	-------- ------tt ----oooo oooooooo
 *	pppppppp pppppppp pppppppp pppppppp
 *	qqqqqqqq qqqqqqqq qqqqqqqq qqqqqqqq
 *	???????? ???????? ???????? ????????
 *
 * t = Light type
 * p = Unknown \ power/emission/exponent/dacay/XXX
 * q = Unknown /
 * ? = Used in BRAVEFF title?
 *
 * Type 0:
 *
 *  p = 1.0f or 1.0f / (FR4 - FR5)
 *  q = 1.0f or -FR5
 *
 * NOTE:
 *  - The first variant sets (16,GBR) to +INF and (17,GBR) to +INF, see
 *    PH:@0C0178FC.
 *  - The second variant sets (16,GBR) to FR5 and (17,GBR) to FR5**2, see
 *    PH:@0C017934.
 *
 * Type 1:
 *
 *  p = 1.0f / (FR4**2 - FR5**2)
 *  q = -FR5**2
 *
 * NOTE: it sets (16,GBR) to X and (17,GBR) to Y.
 *
 * Type 2:
 *
 *  p = (FR4 * FR5) / (FR4 - FR5)
 *  q = 1.0f / |FR5|
 *
 * NOTE: it sets (16,GBR) to X and (17,GBR) to Y.
 *
 * Type 3:
 *
 *  p = (FR4**2 * FR5**2) / (FR5**2 - FR4**2)
 *  q = 1.0 / |FR5**2|
 *
 * NOTE: it sets (16,GBR) to X and (17,GBR) to Y.
 *
 * NOTE: light types according to the PHARRIER text are: constant, infinite,
 * square, reciprocal, reciprocal2, linear.
 */

I (0x061)
{
	hikaru_gpu_light_t *lit = &gpu->lights.scratch;

	lit->emission_type	= (inst[0] >> 16) & 3;
	lit->emission_p		= *(float *) &inst[1];
	lit->emission_q		= *(float *) &inst[2];
}

D (0x061)
{
	UNHANDLED |= !!(inst[0] & 0xFFFCF000);

	DISASM ("lit: set unknown [%u p=%f q=%f]",
	        (inst[0] >> 16) & 3, *(float *) &inst[1], *(float *) &inst[2]);
}

/* 051	Light: Set Color-like
 *
 *	-------- nnnnnnnn -----0-o oooooooo
 *	--aaaaaa aaaabbbb bbbbbbcc cccccccc
 *
 * n = Index? Index into the 194 ramp data?
 * a,b,c = Color? = FP * 255.0f and then truncated and clamped to [0,FF].
 *
 * This may well be a 10-10-10 color format.
 *
 * See PH:@0C0178C6; for a,b,c computation see PH:@0C03DC66.
 *
 *
 * 451	Light: Set Color-like 2
 *
 *	-------u -------- -----1-o oooooooo
 *	???????? ???????? ???????? ????????
 *
 * u = Unknown
 * ? = Color-like if B61 was called on this light, garbage otherwise.
 *
 * See PH:@0C017A7C, PH:@0C017B6C, PH:@0C017C58, PH:@0C017CD4, PH:@0C017D64.
 */

I (0x051)
{
	hikaru_gpu_light_t *lit = &gpu->lights.scratch;

	switch ((inst[0] >> 8) & 7) {
	case 0:
		lit->_051_index    = (inst[0] >> 16) & 0xFF;
		lit->_051_color[0] = inst[1] & 0x3FF;
		lit->_051_color[1] = (inst[1] >> 10) & 0x3FF;
		lit->_051_color[2] = (inst[1] >> 20) & 0x3FF;
		break;
	case 4:
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

D (0x051)
{
	switch ((inst[0] >> 8) & 7) {
	case 0:
		UNHANDLED |= !!(inst[0] & 0xFF00F000);
		UNHANDLED |= !!(inst[1] & 0xC0000000);

		DISASM ("lit: set color-like [%u %X]",
		        (inst[0] >> 16) & 0xFF, inst[1]);
		break;
	case 4:
		DISASM ("lit: set unknown");

		UNHANDLED |= !!(inst[0] & 0xFEFFF000);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

/* 006	Light: Unknown
 *
 *	-------- -------- -------o oooooooo
 */

I (0x006)
{
}

D (0x006)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFF000);

	DISASM ("lit: unknown");
}

/* 046	Light: Unknown
 *
 *	-------- -------n ----oooo oooooooo
 *
 * n = Unknown
 */

I (0x046)
{
}

D (0x046)
{
	UNHANDLED |= !!(inst[0] & 0xFFFEF000);

	DISASM ("lit: unknown [%u]", (inst[0] >> 16) & 1);
}

/* 104	Commit Light
 *
 *	------nn nnnnnnnn ----oooo oooooooo
 *
 * n = Index
 */

I (0x104)
{
	uint32_t index = get_light_index (inst);

	gpu->lights.table[index] = gpu->lights.scratch;
	gpu->lights.table[index].set = true;
}

D (0x104)
{
	UNHANDLED |= !!(inst[0] & 0xFC00F000);

	DISASM ("lit: commit @%u", get_light_index (inst));
}

/* 064  Commit Lightset
 *
 *      -------- nnnnnnnn ---M---o oooooooo
 *      ------bb bbbbbbbb ------aa aaaaaaaa
 *      ------dd dddddddd ------cc cccccccc
 *      ???????? ???????? ???????? ????????
 *
 * M = Unknown (0 in the BOOTROM, 1 elsewhere)
 * n = Lightset index
 * a,b,c,d = Indices of four lights
 * ? = Used in BRAVEFF title?
 *
 * See PH:@0C017DF0.
 */

I (0x064)
{
	hikaru_gpu_lightset_t *ls;

	uint32_t index = gpu->lights.base + get_lightset_index (inst);
	uint32_t light0 = inst[1] & (NUM_LIGHTS - 1);
	uint32_t light1 = (inst[1] >> 16) & (NUM_LIGHTS - 1);
	uint32_t light2 = inst[2] & (NUM_LIGHTS - 1);
	uint32_t light3 = (inst[2] >> 16) & (NUM_LIGHTS - 1);

	if (index >= NUM_LIGHTSETS) {
		VK_ERROR ("CP: lightset commit index exceeds MAX (%u >= %u), skipping",
		          index, NUM_LIGHTSETS);
		return;
	}

	ls = &gpu->lights.sets[index];

	ls->lights[0] = &gpu->lights.table[light0];
	ls->lights[1] = &gpu->lights.table[light1];
	ls->lights[2] = &gpu->lights.table[light2];
	ls->lights[3] = &gpu->lights.table[light3];
	ls->set = true;
}

D (0x064)
{
	UNHANDLED |= !!(inst[0] & 0xFF00E000);
	UNHANDLED |= ((inst[0] & 0x1000) != 0x1000);
	UNHANDLED |= !!(inst[1] & 0xFC00FC00);
	UNHANDLED |= !!(inst[2] & 0xFC00FC00);

	DISASM ("lit: commit set @base+%u [%u %u %u %u]",
	        get_lightset_index (inst),
	        inst[1] & (NUM_LIGHTS - 1), (inst[1] >> 16) & (NUM_LIGHTS - 1),
	        inst[2] & (NUM_LIGHTS - 1), (inst[2] >> 16) & (NUM_LIGHTS - 1));
}

/* 043	Recall Lightset
 *
 *	----DDDD nnnnnnnn ---A---o oooooooo
 *
 * A = Active
 * D = Disabled lights mask
 * n = Index
 */

I (0x043)
{
	uint32_t index = get_lightset_index (inst);
	uint32_t mask  = (inst[0] >> 24) & 0xF;

	if (!(inst[0] & 0x1000))
		gpu->lights.base = index;
	else {
		index += gpu->lights.base;
		if (index >= NUM_LIGHTSETS) {
			VK_ERROR ("CP: lightset recall index exceeds MAX (%u >= %u), skipping",
			          index, NUM_LIGHTSETS);
			return;
		}

		gpu->lights.scratchset = gpu->lights.sets[index];
		gpu->lights.scratchset.mask = mask;
	}
}

D (0x043)
{
	UNHANDLED |= !!(inst[0] & 0xF000E000);

	if (!(inst[0] & 0x1000))
		DISASM ("lit: set set base %u", get_lightset_index (inst));
	else
		DISASM ("lit: recall set @base+%u [mask=%X]",
		        get_lightset_index (inst), (inst[0] >> 24) & 0xFF);
}

/*
 * Meshes
 * ======
 *
 * This class of instructions pushes (or otherwise deals with) vertex
 * data to the transformation/rasterization pipeline.
 */

/* 101	Mesh: Set Unknown (Set Light Unknown?)
 *
 *	----nnuu uuuuuuuu ----000o oooooooo
 *
 * n, u = Unknown
 *
 * 3FF is exactly the number of lights... Perhaps 'set sunlight'?
 *
 * See @0C008040, PH:@0C016418, PH:@0C016446.
 *
 *
 * 301	Mesh: Set Unknown
 *
 *	-------- unnnnnnn ----oooo oooooooo
 *
 * u, n = Unknown, n != 1
 *
 *
 * 501	Mesh: Set Unknown                                                             
 *                                                                             
 *	-------- ---ppppp -----oo oooooooo
 *                                                                             
 * p = Param, unknown.                                                         
 *                                                                             
 * Used by the BOOTROM.                                                        
 *                                                                             
 * See AT:@0C0C841C, the table that holds the constant parameters to the 501   
 * command, set by the routine AT:@0C049BA6 in variable AT:@0C61404C, which    
 * is wrapped into the 501 command in AT:@0C049D08.                            
 *
 *
 * 901	Mesh: Set Precision (Static)
 *
 *	-------- pppppppp ----100o oooooooo
 *
 * Information kindly provided by CaH4e3.
 */

I (0x101)
{
	uint32_t log;

	switch ((inst[0] >> 8) & 0xF) {
	case 1:
		break;
	case 3:
		break;
	case 5:
		break;
	case 9:
		log = (inst[0] >> 16) & 0xFF;
		gpu->static_mesh_precision = 1.0f / (1 << (0x8F - log - 2));
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

D (0x101)
{
	uint32_t log;
	float precision;

	switch ((inst[0] >> 8) & 0xF) {
	case 1:
		UNHANDLED |= !!(inst[0] & 0xF000F000);

		DISASM ("mesh: set unknown [%u]", (inst[0] >> 16) & 0x3FF);
		break;
	case 3:
		UNHANDLED |= !!(inst[0] & 0xFF00F000);

		DISASM ("mesh: set unknown [%u]", (inst[0] >> 16) & 0xFF);
		break;
	case 5:	
		UNHANDLED |= !!(inst[0] & 0xFFE0F000);

		DISASM ("mesh: set unknown [%u]", (inst[0] >> 16) & 0x1F);
		break;
	case 9:
		log = (inst[0] >> 16) & 0xFF;
		precision = 1.0f / (1 << (0x8F - log - 2));

		UNHANDLED |= !!(inst[0] & 0xFF00F000);

		DISASM ("mesh: set precision s [%u %f]", log, precision);	
		break;
	default:
		VK_ASSERT (0);
		break;
	}

}

/* 12x	Mesh: Push Position (Static)
 * 1Ax	Mesh: Push Position (Dynamic)
 * 1Bx	Mesh: Push All (Position, Normal, Texcoords) (Dynamic)
 *
 *
 * They appear to have a common 32-bit header:
 *
 *	AAAAAAAA U------- uuuSTTTo oooootpW
 *
 * A = Vertex alpha
 *
 * U = Unknown
 *
 *     Normal smoothing?
 *
 * u = Unknown
 *
 *     No idea.
 *
 * S = Unknown
 *
 *     Seemingly used for shadows in AIRTRIX (attract mode).
 *
 * T = Triangle
 *
 *     The only observed values so far are 0 and 7.
 *
 *     If 0, the vertex is pushed to the GPU vertex buffer. If 7, the vertex is
 *     pushed and defines a triangle along with the three previously pushed
 *     vertices.
 *
 * t = Texcoord pivot?
 *
 *     Apparently only used by 1Bx, which includes texcoord info.
 *
 * p = Position pivot
 *
 *     When 0, the vertex is linked to the previous two according to the
 *     winding bit. When 1, the vertex at offset -2 is kept unchanged in the
 *     vertex buffer, and acts as a pivot for building a triangle fan.
 *
 * W = Winding
 *
 *     Triangle winding specifies whether the vertices composing the
 *     to-be-drawn triangle are to be in the order (0, -1, -2), and W is 0
 *     in this case, or (0, -2, -1), and W is 1 in this case.
 *
 *     If vertex in position -2 is a pivot, it is treated as if it wasn't. [?]
 *
 * For 12x, the rest of the instruction looks like:
 *
 *	-------- -------- -------- --------
 *	xxxxxxxx xxxxxxxx ???????? ????????
 *	yyyyyyyy yyyyyyyy ???????? ????????
 *	zzzzzzzz zzzzzzzz ???????? ????????
 *
 * x,y,z = Vertex position.
 * ? = Normal in fixed-point? The sum of the three fields over distinct
 *     vectors in a mesh isn't constant tho.
 *
 * For 1BX, the rest of the instruction looks like:
 *
 *	-------- -------- -------- --------
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *
 * x,y,z = Vertex position
 *
 * For 1BX, the rest of the instruction looks like:
 *
 *	-------- -------- -------- --------
 *	xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
 *	yyyyyyyy yyyyyyyy yyyyyyyy yyyyyyyy
 *	zzzzzzzz zzzzzzzz zzzzzzzz zzzzzzzz
 *	ssssssss ssssssss tttttttt tttttttt
 *	uuuuuuuu uuuuuuuu uuuuuuuu uuuuuuuu
 *	vvvvvvvv vvvvvvvv vvvvvvvv vvvvvvvv
 *	wwwwwwww wwwwwwww wwwwwwww wwwwwwww
 *
 * x,y,z = Position
 * u,v,w = Normal
 * s,t = Texcoords
 *
 * Meshes come in two flavors: dynamic and static. Dynamic meshes are specified
 * with normal IEEE457 floating-point values. Static meshes are specified with
 * variable fixed-point values; the fixed-point precision is defined by command
 * 901.
 *
 * Information on the existence of static/dynamic mesh varieties, and info on
 * the fixed-point decoding kindly provided by CaH4e3.
 */

I (0x12C)
{
	hikaru_gpu_vertex_t v;

	v.info.full = inst[0];

	VK_ASSERT (gpu->static_mesh_precision > 0.0f);

	v.pos[0] = (int16_t)(inst[1] >> 16) * gpu->static_mesh_precision;
	v.pos[1] = (int16_t)(inst[2] >> 16) * gpu->static_mesh_precision;
	v.pos[2] = (int16_t)(inst[3] >> 16) * gpu->static_mesh_precision;

	v.nrm[0] = (int16_t)(inst[1] & 0xFFFF) / 16.0f;
	v.nrm[1] = (int16_t)(inst[2] & 0xFFFF) / 16.0f;
	v.nrm[2] = (int16_t)(inst[3] & 0xFFFF) / 16.0f;

	hikaru_renderer_push_vertices ((hikaru_renderer_t *) HR,
	                               &v, HR_PUSH_POS | HR_PUSH_POS, 1);
}

D (0x12C)
{
	UNHANDLED |= !!(inst[0] & 0x007F0000);

	DISASM ("mesh: push pos s");
}

I (0x1AC)
{
	hikaru_gpu_vertex_t v;

	v.info.full = inst[0];

	v.pos[0] = *(float *) &inst[1];
	v.pos[1] = *(float *) &inst[2];
	v.pos[2] = *(float *) &inst[3];

	hikaru_renderer_push_vertices ((hikaru_renderer_t *) HR,
	                               &v, HR_PUSH_POS, 1);
}

D (0x1AC)
{
	UNHANDLED |= !!(inst[0] & 0x007F0000);

	DISASM ("mesh: push pos d");
}

I (0x1B8)
{
	hikaru_gpu_vertex_t v;

	v.info.full = inst[0];

	v.pos[0] = *(float *) &inst[1];
	v.pos[1] = *(float *) &inst[2];
	v.pos[2] = *(float *) &inst[3];

	v.nrm[0] = *(float *) &inst[5];
	v.nrm[1] = *(float *) &inst[6];
	v.nrm[2] = *(float *) &inst[7];

	v.txc[0] = ((int16_t) inst[4]) / 16.0f;
	v.txc[1] = ((int16_t) (inst[4] >> 16)) / 16.0f;

	hikaru_renderer_push_vertices ((hikaru_renderer_t *) HR, &v,
	                               HR_PUSH_POS | HR_PUSH_NRM | HR_PUSH_TXC, 1);
}

D (0x1B8)
{
	UNHANDLED |= !!(inst[0] & 0x007F0000);

	DISASM ("mesh: push all d");
}

/* 0E8	Mesh: Push Texcoords 3
 *
 *	-------- -------x ----WWWo oooooooC
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *
 * The interaction of U, u, P, W, with the ones specified by the push position/
 * push all instructions is still unknown.
 *
 * u, v = Texcoords for three points.
 */

I (0x0E8)
{
	hikaru_gpu_vertex_t vs[3];
	unsigned i;

	for (i = 0; i < 3; i++) {
		vs[i].info.full = inst[0];

		vs[i].txc[0] = ((int16_t) inst[i+1]) / 16.0f;
		vs[i].txc[1] = ((int16_t) (inst[i+1] >> 16)) / 16.0f;
	}

	hikaru_renderer_push_vertices (HR, &vs[0], HR_PUSH_TXC, 3);
}

D (0x0E8)
{

	UNHANDLED |= !!(inst[0] & 0xFFFEF000);

	DISASM ("mesh: push txc 3");
}

/* 158	Mesh: Push Texcoords 1
 *
 *	-------- ?------- ----???o ooooo??C
 *	vvvvvvvv vvvvvvvv uuuuuuuu uuuuuuuu
 *
 * u, v = Texcoords for one point.
 */

I (0x158)
{
	hikaru_gpu_vertex_t v;

	v.info.full = inst[0];

	v.txc[0] = ((int16_t) inst[1]) / 16.0f;
	v.txc[1] = ((int16_t) (inst[1] >> 16)) / 16.0f;

	hikaru_renderer_push_vertices (HR, &v, HR_PUSH_TXC, 1);
}

D (0x158)
{
	UNHANDLED |= !!(inst[0] & 0xFF7FF000);

	DISASM ("mesh: push txc 1");
}

/****************************************************************************
 Unknown
****************************************************************************/

/* 181	Viewport: Set FB Property 1
 * 781	Viewport: Set FB Property 2
 *
 *	-------E nnnnnnnn -------o oooooooo
 *
 * E = Enable
 *
 *	E is set only if n is non-zero.
 *
 * n = Unknown
 *
 * See PH:@0C015B50. Probably related to 781, see PH:@0C038952.
 *
 *
 *	-----AAA -----BBB -------o oooooooo
 *
 * A, B = Unknown
 *
 *	Blending modes between 3D and framebuffer?
 *
 * The values of A and B are determined by the values of ports 1A00001C and
 * 1A000020 prior to the command upload. Its parameter is stored in (56, GBR).
 * It *may* act like a fence, toggling bits in the GPU MMIOs when processed.
 * See @0C0065D6, PH:@0C016336, PH:@0C038952, PH:@0C015B50.
 *
 * CaH4e3 suggests the two are related to screen transitions. He's probably
 * right. :-) (Do they also clear the framebuffer?)
 */

I (0x181)
{
	uint32_t e, n, a, b;

	switch ((inst[0] >> 8) & 7) {
	case 1:
		e = (inst[0] >> 24) & 1;
		n = (inst[0] >> 16) & 0xFF;

		DISASM ("vp: set fb flag 1 (%u %X)", e, n);

		UNHANDLED |= !!(inst[0] & 0xFE00F800);
		break;
	case 7:
		a = (inst[0] >> 24) & 7;
		b = (inst[0] >> 16) & 7;

		DISASM ("vp: set fb flag 7 (%u %u)", a, b);

		UNHANDLED |= !!(inst[0] & 0xF8F8F800);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

D (0x181)
{
	switch ((inst[0] >> 8) & 7) {
	case 1:
		UNHANDLED |= !!(inst[0] & 0xFE00F800);

		DISASM ("vp: set fb flag 1 (%u %X)",
		        (inst[0] >> 24) & 1, (inst[0] >> 16) & 0xFF);
		break;
	case 7:
		UNHANDLED |= !!(inst[0] & 0xF8F8F800);

		DISASM ("vp: set fb flag 7 (%u %u)",
		        (inst[0] >> 24) & 7, (inst[0] >> 16) & 7);
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

/* 088	Flush
 *
 *	-------- U------- ----xxxo oooooooo
 *
 * U = Unknown
 * x = Unknown
 *
 * Always comes as the last instruction. Perhaps some kind of 'flush all'
 * or 'raise IRQ' command or something like that. If it is a 'flush all'
 * command, it may set some GPU ports not set by 1C2 (1A000024 perhaps.)
 */

I (0x088)
{
}

D (0x088)
{
	UNHANDLED |= !!(inst[0] & 0xFF7FF000);

	DISASM ("unk: unknown");
}

/* 154	Commit Alpha Threshold
 *
 *	-------- --nnnnnn -------o oooooooo
 *	hhhhhhhh hhhhhhhh hhhhhhhh llllllll
 *
 * n = Unknown
 * l = Alpha low threshold
 * h = Alpha high threshold
 *
 * See PH:@0C017798, PH:@0C0CF868. It may be related to
 * command C81, see PH:@0C0CF872 and PH:@0C0CF872.
 */

I (0x154)
{
}

D (0x154)
{
	UNHANDLED |= !!(inst[0] & 0xFFC0F000);

	DISASM ("unk: set alpha thresh [%u (%X %X)]",
	        (inst[0] >> 16) & 0x3F, inst[1] & 0xFF, inst[1] >> 8);
}

/* 194	Commit Ramp Data
 *
 *	nnnnnnnn mmmmmmmm -------o oooooooo
 *	aaaaaaaa aaaaaaaa bbbbbbbb bbbbbbbb
 *
 * NOTE: these come in groups of 8. The data for each group
 * comes from a different ptr.
 *
 * NOTE: seems to be light related.
 *
 * See PH:@0C017A3E.
 */

I (0x194)
{
}

D (0x194)
{
	UNHANDLED |= !!(inst[0] & 0x0000F000);

	DISASM ("unk: set ramp [%u %u (%X %X)]",
	        (inst[0] >> 16) & 0xFF, inst[0] >> 24,
	         inst[1] & 0xFFFF, inst[1] >> 16);
}

/* 3A1	Set Lo Addresses
 *
 *	-------- -------- -----01o oooooooo
 *	llllllll llllllll llllllll llllllll
 *	LLLLLLLL LLLLLLLL LLLLLLLL LLLLLLLL
 *      -------- -------- -------- --------
 *
 *
 * 5A1	Set Hi Addresses
 *
 *	-------- -------- -----10o oooooooo
 *	uuuuuuuu uuuuuuuu uuuuuuuu uuuuuuuu
 *	UUUUUUUU UUUUUUUU UUUUUUUU UUUUUUUU
 *      -------- -------- -------- --------
 *
 * l,L,h,H = Addresses? Possibly watermarks?
 *
 * See PH:@0C016308 for both.
 */

I (0x1A1)
{
}

D (0x1A1)
{
	UNHANDLED |= !!(inst[0] & 0xFFFFF000);
	UNHANDLED |= (inst[3] != 0);

	DISASM ("unk: set address [%08X %08X]", inst[1], inst[2]);
}

/* 0D1	Set Unknown
 *
 *	???????? ??????aa -----11o oooooooo
 *	bbbbbbbb bbbbbbbb cccccccc cccccccc
 *
 * These come in quartets. May be related to matrices. See PH:@0C015C3E. Note
 * that the values b and c here come from FPU computations, see PH:@0C0FF970.
 */

I (0x0D1)
{
}

D (0x0D1)
{
	UNHANDLED |= !!(inst[0] & 0xFFFCF000);

	DISASM ("unk: unknown [%X %X %X]",
	        inst[0] >> 16, inst[1] & 0xFFFF, inst[1] >> 16);
}

/* 103	Recall Fog
 * 113	Recall Fog
 *
 *	FFFFFFFF -------- ----ssso oooooooo
 *
 * s = Sub-opcode
 * F = Fog-related value? See PH:@0C0DA8BC. (-1)
 *
 * Notes: The sub-opcode
 *
 *  3: disabled (it always comes with F=0 or F=0xFF).
 *  9: enabled, F is positive.
 *  D: enabled, F is negative (actual value is ~F).
 *
 * See AT:@0C049CDA for N=8 and N=C, see PH:@0C0173CA for N=2, N=8. The
 * commands are emitted at e.g. AT:@0C69A220 (all three of them).
 */

static void
get_fog_params (uint32_t *inst, bool *enabled, float *kappa)
{
	*enabled = false;
	*kappa = 0.0f;

	switch ((inst[0] >> 8) & 15) {
	case 3:
		break;
	case 9:
		*kappa = ((int32_t)(uint8_t)(inst[0] >> 24)) / 255.0f;
		break;
	case 0xD:
		*kappa = ((int32_t)(int8_t)(~(inst[0] >> 24))) / 255.0f;
		break;
	default:
		VK_ASSERT (0);
		break;
	}
}

I (0x103)
{
}

D (0x103)
{
	bool enabled;
	float kappa;

	get_fog_params (inst, &enabled, &kappa);

	UNHANDLED |= !!(inst[0] & 0x00FFF000);

	DISASM ("vp: recall fog [%u %f]", enabled, kappa);
}

/****************************************************************************
 Opcodes
****************************************************************************/

#define K(op_, base_op_, flags_) \
	{ op_, flags_, hikaru_gpu_inst_##base_op_, hikaru_gpu_disasm_##base_op_ }

static const struct {
	uint32_t op;
	uint16_t flags;
	void (* handler)(hikaru_gpu_t *gpu, uint32_t *inst);
	void (* disasm)(hikaru_gpu_t *gpu, uint32_t *inst);
} insns_desc[] = {
	/* 0x00 */
	K(0x000, 0x000, FLAG_CONTINUE),
	K(0x003, 0x003, 0),
	K(0x004, 0x004, 0),
	K(0x005, 0x005, 0),
	K(0x006, 0x006, 0),
	K(0x011, 0x011, 0),
	K(0x012, 0x012, FLAG_JUMP),
	K(0x021, 0x021, 0),
	/* 0x40 */
	K(0x043, 0x043, 0),
	K(0x046, 0x046, 0),
	K(0x051, 0x051, 0),
	K(0x052, 0x052, FLAG_JUMP),
	K(0x055, 0x055, 0),
	K(0x061, 0x061, 0),
	K(0x064, 0x064, 0),
	/* 0x80 */
	K(0x081, 0x081, FLAG_CONTINUE),
	K(0x082, 0x082, FLAG_JUMP),
	K(0x083, 0x083, FLAG_CONTINUE),
	K(0x084, 0x084, 0),
	K(0x088, 0x088, 0),
	K(0x091, 0x091, FLAG_CONTINUE),
	K(0x095, 0x095, 0),
	/* 0xC0 */
	K(0x0C1, 0x0C1, 0),
	K(0x0C3, 0x0C3, 0),
	K(0x0C4, 0x0C4, 0),
	K(0x0D1, 0x0D1, 0),
	K(0x0E8, 0x0E8, FLAG_PUSH),
	K(0x0E9, 0x0E8, FLAG_PUSH),
	/* 0x100 */
	K(0x101, 0x101, 0),
	K(0x103, 0x103, 0),
	K(0x104, 0x104, 0),
	K(0x113, 0x103, 0),
	K(0x12C, 0x12C, FLAG_PUSH | FLAG_STATIC),
	K(0x12D, 0x12C, FLAG_PUSH | FLAG_STATIC),
	K(0x12E, 0x12C, FLAG_PUSH | FLAG_STATIC),
	K(0x12F, 0x12C, FLAG_PUSH | FLAG_STATIC),
	/* 0x140 */
	K(0x154, 0x154, 0),
	K(0x158, 0x158, FLAG_PUSH),
	K(0x159, 0x158, FLAG_PUSH),
	K(0x15A, 0x158, FLAG_PUSH),
	K(0x15B, 0x158, FLAG_PUSH),
	K(0x161, 0x161, 0),
	/* 0x180 */
	K(0x181, 0x181, 0),
	K(0x191, 0x191, 0),
	K(0x194, 0x194, 0),
	K(0x1A1, 0x1A1, 0),
	K(0x1AC, 0x1AC, FLAG_PUSH),
	K(0x1AD, 0x1AC, FLAG_PUSH),
	K(0x1AE, 0x1AC, FLAG_PUSH),
	K(0x1AF, 0x1AC, FLAG_PUSH),
	K(0x1B8, 0x1B8, FLAG_PUSH),
	K(0x1B9, 0x1B8, FLAG_PUSH),
	K(0x1BA, 0x1B8, FLAG_PUSH),
	K(0x1BB, 0x1B8, FLAG_PUSH),
	K(0x1BC, 0x1B8, FLAG_PUSH),
	K(0x1BD, 0x1B8, FLAG_PUSH),
	K(0x1BE, 0x1B8, FLAG_PUSH),
	K(0x1BF, 0x1B8, FLAG_PUSH),
	/* 0x1C0 */
	K(0x1C2, 0x1C2, FLAG_JUMP)
};

#undef K

void
hikaru_gpu_cp_init (hikaru_gpu_t *gpu)
{
	unsigned i;
	for (i = 0; i < 0x200; i++) {
		insns[i].handler = NULL;
		insns[i].flags = FLAG_INVALID;
		disasm[i] = NULL;
	}
	for (i = 0; i < NUMELEM (insns_desc); i++) {
		uint32_t op = insns_desc[i].op;
		insns[op].handler = insns_desc[i].handler;
		insns[op].flags   = insns_desc[i].flags;
		disasm[op]        = insns_desc[i].disasm;
	}
}

